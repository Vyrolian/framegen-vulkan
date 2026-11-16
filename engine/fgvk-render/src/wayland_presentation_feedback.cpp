/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "wayland_presentation_feedback.hpp"
#include "wayland_client_abi.hpp"
#include "present_diagnostics.hpp"
#include "swapchain/uncapped_auto_cap.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <time.h>
#include <unistd.h>

using namespace fgvk::layer;
namespace {
using Clock = present_diagnostics::Clock;
uint64_t hostNow() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count());
}
template<class T> bool symbol(void* library, T& target, const char* name) {
    target = reinterpret_cast<T>(dlsym(library, name));
    return target != nullptr;
}
}

struct WaylandPresentationFeedback::Impl {
    std::mutex mutex;
    void* library{};
    wl_display* display{}; // Borrowed until vkDestroySurfaceKHR, then stop().
    wl_surface* surface{};
    wl_event_queue* queue{};
    wl_proxy* registry{};
    wl_proxy* presentation{};
    wl_proxy* discovery{};
    bool discovered{}, stopped{}, diagnostics{};
    uint32_t global{}, clockId{UINT32_MAX};
    uint64_t nextId{};
    uint64_t token{present_diagnostics::allocateContextId()};
    // Fed only from presented feedback on this surface's private queue, which
    // is dispatched exclusively under `mutex`.

    wl_event_queue* (*createQueue)(wl_display*){};
    void (*destroyQueue)(wl_event_queue*){};
    int (*dispatchPending)(wl_display*, wl_event_queue*){};
    void* (*createWrapper)(void*){};
    void (*destroyWrapper)(void*){};
    wl_proxy* (*construct)(wl_proxy*, uint32_t, wl_argument*, const wl_interface*, uint32_t){};
    void (*marshal)(wl_proxy*, uint32_t, wl_argument*){};
    int (*listen)(wl_proxy*, void (**)(void), void*){};
    void (*setQueue)(wl_proxy*, wl_event_queue*){};
    void (*destroy)(wl_proxy*){};
    uint32_t (*proxyId)(wl_proxy*){};
    const wl_interface* registryInterface{};
    const wl_interface* callbackInterface{};
    const wl_interface* surfaceInterface{};
    const wl_interface* outputInterface{};

    // Hand-declared version-1 wire subset of stable presentation-time.xml.
    // Never request a target time, frame callback or wl_surface.commit.
    std::array<const wl_interface*, 2> feedbackTypes{};
    std::array<const wl_interface*, 1> outputTypes{};
    std::array<wl_message, 2> requests{{{"destroy", "", nullptr}, {"feedback", "on", feedbackTypes.data()}}};
    std::array<wl_message, 1> events{{{"clock_id", "u", nullptr}}};
    std::array<wl_message, 3> feedbackEvents{{
        {"sync_output", "o", outputTypes.data()}, {"presented", "uuuuuuu", nullptr}, {"discarded", "", nullptr}}};
    wl_interface feedbackInterface{"wp_presentation_feedback", 1, 0, nullptr, 3, feedbackEvents.data()};
    wl_interface presentationInterface{"wp_presentation", 1, 2, requests.data(), 1, events.data()};

    using Event = present_diagnostics::WaylandEvent;
    using Sample = present_diagnostics::WaylandSample;
    struct Pending {
        Impl* owner{};
        wl_proxy* proxy{};
        Sample sample;
        uint32_t syncedMilliHz{}; // X7: the mode rate of the output sync_output named, if it is ours.
    };
    std::array<Pending, 256> pending{};
    // Samples are queued as they happen. A sample the queue refuses is counted here and reported
    // as a gap before the next sample that is queued.
    uint64_t dropped{};
    uint64_t requestedId{};
    // X6: the output refresh from the last presented event (0 unknown or variable). Written under
    // mutex by presented(), read lock-free by the present path's refresh query.
    std::atomic<uint32_t> refreshNs{0};
    // X7: the wl_output globals bound at version 1 on the private queue, up to eight. Each element is its
    // listener's data, so it stays at one address while its proxy lives.
    struct Output {
        uint32_t global{};
        wl_proxy* proxy{};
        uint32_t milliHz{}; // The current mode's refresh in mHz; 0 until a current mode arrives.
    };
    std::array<Output, 8> outputs{};
    // X7: the rate of the output the surface was last presented on while the refresh is 0. Written under
    // mutex by presented(), read lock-free by the present path's refresh query.
    std::atomic<uint32_t> outputMilliHz{0};
    uint64_t presents{};
    // OS1: the screen sink (see setScreenSink) and whether its unsupported-clock line was printed.
    std::weak_ptr<ScreenSampleRing> screenSink;
    bool clockLogged{};

    // OS1: whether the screen sink is open. Every present asks for feedback while it is.
    bool screenSinkOpen() const {
        const auto ring = screenSink.lock();
        return ring && ring->accepting();
    }
    // OS1: a presented event with a valid timestamp feeds the sink, on CLOCK_MONOTONIC only. Caller holds mutex.
    void pushScreenSample(const Sample& sample) noexcept {
        const auto ring = screenSink.lock();
        if (!ring) return;
        if (!screenSampleClockSupported(clockId)) {
            if (!clockLogged) {
                clockLogged = true;
                std::clog << "framegen-vulkan: on-screen correction: wayland clock=" << clockId << " unsupported\n";
            }
            return;
        }
        ring->push(ScreenSample{sample.context, sample.source, sample.generated,
            static_cast<int64_t>(sample.presented)});
    }

    // The mode rate of the only bound output; 0 when no output or several outputs are bound.
    uint32_t onlyOutputMilliHz() const noexcept {
        const Output* only{};
        for (const auto& slot : outputs) {
            if (!slot.proxy) continue;
            if (only) return 0;
            only = &slot;
        }
        return only ? only->milliHz : 0;
    }

    void record(const Sample& sample) noexcept {
        if (!diagnostics) return;
        if (dropped && present_diagnostics::recordWaylandGap(token, dropped)) dropped = 0;
        if (!present_diagnostics::recordWaylandSample(token, sample)) ++dropped;
    }
    static void globalAdded(void* data, wl_proxy*, uint32_t name, const char* interface, uint32_t version) noexcept {
        auto& self = *static_cast<Impl*>(data);
        if (!version) return;
        if (std::strcmp(interface, "wl_output") == 0) {
            self.bindOutput(name);
            return;
        }
        if (self.presentation || std::strcmp(interface, "wp_presentation") != 0) return;
        wl_argument args[4]{};
        args[0].u = name;
        args[1].s = self.presentationInterface.name;
        args[2].u = 1;
        self.presentation = self.construct(self.registry, 0, args, &self.presentationInterface, 1);
        if (!self.presentation) return;
        static void (*listener[])(void){reinterpret_cast<void (*)(void)>(&clock)};
        if (self.listen(self.presentation, listener, &self) != 0) {
            self.destroy(self.presentation);
            self.presentation = nullptr;
            return;
        }
        self.global = name;
    }
    static void globalRemoved(void* data, wl_proxy*, uint32_t name) noexcept {
        auto& self = *static_cast<Impl*>(data);
        for (auto& slot : self.outputs) {
            if (!slot.proxy || slot.global != name) continue;
            self.destroy(slot.proxy);
            slot.proxy = nullptr;
            slot.global = 0;
            slot.milliHz = 0;
        }
        if (name == self.global && self.presentation) {
            self.marshal(self.presentation, 0, nullptr);
            self.destroy(self.presentation);
            self.presentation = nullptr;
            self.clockId = UINT32_MAX;
        }
    }
    static void clock(void* data, wl_proxy*, uint32_t id) noexcept {
        static_cast<Impl*>(data)->clockId = id;
    }
    static void discoveryDone(void* data, wl_proxy* proxy, uint32_t) noexcept {
        auto& self = *static_cast<Impl*>(data);
        self.discovered = true;
        self.destroy(proxy);
        self.discovery = nullptr;
    }
    static void syncOutput(void* data, wl_proxy*, wl_proxy* output) noexcept {
        auto& item = *static_cast<Pending*>(data);
        item.sample.output = item.owner->proxyId(output);
        for (const auto& slot : item.owner->outputs)
            if (slot.proxy && slot.proxy == output) item.syncedMilliHz = slot.milliHz;
    }
    // X7: wl_output events. Only the current mode's refresh is kept; geometry, done, scale, name and
    // description are never used, but each has an entry because the listener array covers every event.
    static void outputGeometry(void*, wl_proxy*, int32_t, int32_t, int32_t, int32_t, int32_t,
            const char*, const char*, int32_t) noexcept {}
    static void outputMode(void* data, wl_proxy*, uint32_t flags, int32_t, int32_t, int32_t refresh) noexcept {
        auto& output = *static_cast<Output*>(data);
        if ((flags & 0x1) && refresh > 0) output.milliHz = static_cast<uint32_t>(refresh); // 0x1: current mode
    }
    static void outputDone(void*, wl_proxy*) noexcept {}
    static void outputScale(void*, wl_proxy*, int32_t) noexcept {}
    static void outputName(void*, wl_proxy*, const char*) noexcept {}
    static void outputDescription(void*, wl_proxy*, const char*) noexcept {}
    static void presented(void* data, wl_proxy*, uint32_t hi, uint32_t lo, uint32_t nsec,
            uint32_t refresh, uint32_t seqHi, uint32_t seqLo, uint32_t flags) noexcept {
        auto& item = *static_cast<Pending*>(data);
        // Every presented event, diagnostics or not; 0 (unknown or variable refresh) clears the rate.
        item.owner->refreshNs.store(refresh, std::memory_order_release);
        item.owner->outputMilliHz.store(UncappedAutoCap::waylandOutputMilliHz(refresh, item.syncedMilliHz,
            item.owner->onlyOutputMilliHz()), std::memory_order_release);
        auto sample = item.sample;
        sample.event = Event::Presented;
        sample.received = hostNow();
        const uint64_t seconds = (uint64_t{hi} << 32) | lo;
        const bool validTimestamp = nsec < 1'000'000'000 && seconds <= (UINT64_MAX - nsec) / 1'000'000'000;
        if (validTimestamp)
            sample.presented = seconds * 1'000'000'000 + nsec;
        sample.sequence = (uint64_t{seqHi} << 32) | seqLo;
        sample.refresh = refresh;
        sample.flags = flags;
        item.owner->record(sample);
        if (validTimestamp) item.owner->pushScreenSample(sample);
        item.owner->destroy(item.proxy);
        item.proxy = nullptr;
    }
    static void discarded(void* data, wl_proxy*) noexcept {
        auto& item = *static_cast<Pending*>(data);
        auto sample = item.sample;
        sample.event = Event::Discarded;
        sample.received = hostNow();
        item.owner->record(sample);
        item.owner->destroy(item.proxy);
        item.proxy = nullptr;
    }

    bool initialize(wl_display* borrowedDisplay, wl_surface* borrowedSurface) {
        display = borrowedDisplay;
        surface = borrowedSurface;
        library = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!library) return false;
        if (!symbol(library, createQueue, "wl_display_create_queue") ||
            !symbol(library, destroyQueue, "wl_event_queue_destroy") ||
            !symbol(library, dispatchPending, "wl_display_dispatch_queue_pending") ||
            !symbol(library, createWrapper, "wl_proxy_create_wrapper") ||
            !symbol(library, destroyWrapper, "wl_proxy_wrapper_destroy") ||
            !symbol(library, construct, "wl_proxy_marshal_array_constructor_versioned") ||
            !symbol(library, marshal, "wl_proxy_marshal_array") ||
            !symbol(library, listen, "wl_proxy_add_listener") ||
            !symbol(library, setQueue, "wl_proxy_set_queue") ||
            !symbol(library, destroy, "wl_proxy_destroy") ||
            !symbol(library, proxyId, "wl_proxy_get_id")) return false;
        registryInterface = static_cast<const wl_interface*>(dlsym(library, "wl_registry_interface"));
        callbackInterface = static_cast<const wl_interface*>(dlsym(library, "wl_callback_interface"));
        surfaceInterface = static_cast<const wl_interface*>(dlsym(library, "wl_surface_interface"));
        outputInterface = static_cast<const wl_interface*>(dlsym(library, "wl_output_interface"));
        if (!registryInterface || !callbackInterface || !surfaceInterface || !outputInterface) return false;
        feedbackTypes = {surfaceInterface, &feedbackInterface};
        outputTypes = {outputInterface};
        queue = createQueue(display);
        if (!queue) return false;
        auto* wrapper = static_cast<wl_proxy*>(createWrapper(display));
        if (!wrapper) return false;
        setQueue(wrapper, queue);
        wl_argument arg{};
        registry = construct(wrapper, 1, &arg, registryInterface, 1);
        discovery = construct(wrapper, 0, &arg, callbackInterface, 1);
        destroyWrapper(wrapper);
        if (!registry || !discovery) return false;
        static void (*registryListener[])(void){reinterpret_cast<void (*)(void)>(&globalAdded), reinterpret_cast<void (*)(void)>(&globalRemoved)};
        static void (*doneListener[])(void){reinterpret_cast<void (*)(void)>(&discoveryDone)};
        return listen(registry, registryListener, this) == 0 && listen(discovery, doneListener, this) == 0;
        // No roundtrip, socket read, flush, sleep or second event-reader thread.
        // The application's existing display reader queues our events.
    }

    // X7: binds one wl_output global at version 1 into a free slot on the private queue, where the
    // registry lives. Globals beyond the eight slots are ignored.
    void bindOutput(uint32_t name) {
        for (auto& slot : outputs) {
            if (slot.proxy) continue;
            wl_argument args[4]{};
            args[0].u = name;
            args[1].s = outputInterface->name;
            args[2].u = 1;
            wl_proxy* proxy = construct(registry, 0, args, outputInterface, 1);
            if (!proxy) return;
            static void (*listener[])(void){reinterpret_cast<void (*)(void)>(&outputGeometry),
                reinterpret_cast<void (*)(void)>(&outputMode), reinterpret_cast<void (*)(void)>(&outputDone),
                reinterpret_cast<void (*)(void)>(&outputScale), reinterpret_cast<void (*)(void)>(&outputName),
                reinterpret_cast<void (*)(void)>(&outputDescription)};
            if (listen(proxy, listener, &slot) != 0) { destroy(proxy); return; }
            slot = Output{name, proxy, 0};
            return;
        }
    }

    uint64_t before(uint64_t context, uint64_t source, uint32_t image, bool generated) {
        if (stopped) return 0;
        if (dispatchPending(display, queue) < 0) { cleanup(); return 0; }
        requestedId = 0;
        // Without diagnostics only the refresh source asks, on the UncappedAutoCap cadence. A rate is known
        // from the refresh (X6) or from the output's mode while the refresh is 0 (X7). OS1: an open screen
        // sink asks for every present.
        const uint64_t presentIndex = presents++;
        const bool known = refreshNs.load(std::memory_order_relaxed) != 0 ||
            outputMilliHz.load(std::memory_order_relaxed) != 0;
        if (!diagnostics && !screenSinkOpen() && !UncappedAutoCap::waylandFeedbackDue(presentIndex, known))
            return 0;
        Sample sample{.event = Event::Request, .id = ++nextId, .context = context,
            .source = source, .image = image, .generated = generated, .requested = hostNow(), .clock = clockId};
        uint32_t reason = !presentation ? (discovered ? 2u : 1u) : clockId == UINT32_MAX ? 3u : 0u;
        if (!reason) {
            for (auto& item : pending) {
                if (item.proxy) continue;
                timespec value{};
                if (clock_gettime(static_cast<clockid_t>(clockId), &value) == 0 && value.tv_sec >= 0 &&
                        static_cast<uint64_t>(value.tv_sec) <= (UINT64_MAX - static_cast<uint64_t>(value.tv_nsec)) / 1'000'000'000) {
                    sample.presentationNow = static_cast<uint64_t>(value.tv_sec) * 1'000'000'000 + static_cast<uint64_t>(value.tv_nsec);
                    sample.calibrationSpan = hostNow() - sample.requested;
                }
                item.owner = this;
                item.sample = sample;
                item.syncedMilliHz = 0;
                wl_argument args[2]{};
                args[0].o = surface;
                item.proxy = construct(presentation, 1, args, &feedbackInterface, 1);
                static void (*listener[])(void){reinterpret_cast<void (*)(void)>(&syncOutput),
                    reinterpret_cast<void (*)(void)>(&presented), reinterpret_cast<void (*)(void)>(&discarded)};
                if (!item.proxy) { reason = 5; break; }
                if (listen(item.proxy, listener, &item) != 0) {
                    destroy(item.proxy); item.proxy = nullptr; reason = 5; break;
                }
                record(sample);
                requestedId = sample.id;
                return sample.id;
            }
            if (!reason) reason = 4; // Bounded outstanding-feedback capacity.
        }
        sample.event = Event::Unavailable;
        sample.reason = reason;
        record(sample);
        return sample.id;
    }
    void after(uint64_t id, uint64_t started, uint64_t finished, VkResult result) {
        if (!id) return;
        record(Sample{.event = Event::Call, .id = id, .requested = started,
            .finished = finished, .result = result});
    }
    void cleanup() noexcept {
        if (stopped) return;
        stopped = true;
        for (auto& item : pending) {
            if (!item.proxy) continue;
            auto sample = item.sample;
            sample.event = Event::Abandoned;
            sample.received = hostNow();
            record(sample);
            destroy(item.proxy);
            item.proxy = nullptr;
        }
        if (presentation) { marshal(presentation, 0, nullptr); destroy(presentation); presentation = nullptr; }
        if (discovery) { destroy(discovery); discovery = nullptr; }
        for (auto& slot : outputs) {
            if (!slot.proxy) continue;
            destroy(slot.proxy);
            slot.proxy = nullptr;
        }
        if (registry) { destroy(registry); registry = nullptr; }
        if (queue) { destroyQueue(queue); queue = nullptr; }
    }
    ~Impl() {
        cleanup();
        if (library) dlclose(library);
    }
};

WaylandPresentationFeedback::WaylandPresentationFeedback() : impl(std::make_unique<Impl>()) {}
WaylandPresentationFeedback::~WaylandPresentationFeedback() = default;

uint32_t WaylandPresentationFeedback::refreshNs() const noexcept {
    return impl->refreshNs.load(std::memory_order_acquire);
}

uint32_t WaylandPresentationFeedback::outputMilliHz() const noexcept {
    return impl->outputMilliHz.load(std::memory_order_acquire);
}

void WaylandPresentationFeedback::setScreenSink(std::weak_ptr<ScreenSampleRing> sink) noexcept {
    try {
        const std::lock_guard lock(impl->mutex);
        impl->screenSink = std::move(sink);
    } catch (...) {}
}

std::shared_ptr<WaylandPresentationFeedback> WaylandPresentationFeedback::create(
        wl_display* display, wl_surface* surface, bool diagnostics) noexcept {
    if (!display || !surface) return {};
    try {
        auto result = std::shared_ptr<WaylandPresentationFeedback>(new WaylandPresentationFeedback);
        result->impl->diagnostics = diagnostics;
        if (result->impl->initialize(display, surface)) return result;
    } catch (...) {}
    return {};
}
void WaylandPresentationFeedback::stop() noexcept {
    try { const std::lock_guard lock(impl->mutex); impl->cleanup(); } catch (...) {}
}
VkResult WaylandPresentationFeedback::observe(void* owner, PFN_vkQueuePresentKHR lower, VkQueue queue,
        const VkPresentInfoKHR& info, uint64_t context, uint64_t source, bool generated,
        uint64_t* feedbackId) noexcept {
    auto& self = *static_cast<WaylandPresentationFeedback*>(owner)->impl;
    std::unique_lock lock(self.mutex, std::defer_lock);
    uint64_t id{};
    // The display probe's own cost (before, dispatch, after) is measured only when it is reported.
    const bool measured = self.diagnostics;
    const auto costStarted = measured ? Clock::now() : Clock::time_point{};
    Clock::time_point costBefore{}, costAfterStarted{};
    try {
        lock.lock();
        if (info.swapchainCount == 1 && info.pImageIndices)
            id = self.before(context, source, *info.pImageIndices, generated);
    } catch (...) {}
    if (measured) costBefore = Clock::now();
    const auto started = hostNow();
    const auto result = lower(queue, &info); // Exactly once, unchanged arguments/result.
    const auto finished = hostNow();
    try {
        if (lock.owns_lock()) {
            if (measured) costAfterStarted = Clock::now();
            self.after(id, started, finished, result);
        }
    } catch (...) {}
    if (measured) {
        const auto costEnded = Clock::now();
        const auto spent = (costBefore - costStarted) + (costEnded - costAfterStarted);
        present_diagnostics::recordDisplayProbeCost(static_cast<uint64_t>(std::max<int64_t>(0,
            std::chrono::duration_cast<std::chrono::nanoseconds>(spent).count())));
    }
    if (feedbackId) *feedbackId = self.requestedId;
    return result;
}

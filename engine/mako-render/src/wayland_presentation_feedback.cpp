/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "wayland_presentation_feedback.hpp"
#include "wayland_client_abi.hpp"
#include "present_diagnostics.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <time.h>
#include <unistd.h>

using namespace mako::layer;
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
    uint64_t nextId{}, token{present_diagnostics::allocateContextId()};
    Clock::time_point published{Clock::now()};
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

    enum class Event { Request, Call, Presented, Discarded, Unavailable, Abandoned };
    struct Sample {
        Event event{};
        uint64_t id{}, context{}, source{};
        uint32_t image{};
        bool generated{};
        uint64_t requested{}, received{}, finished{}, presentationNow{}, calibrationSpan{};
        uint64_t presented{}, sequence{};
        uint32_t refresh{}, flags{}, output{}, clock{UINT32_MAX}, reason{};
        VkResult result{};
    };
    struct Pending {
        Impl* owner{};
        wl_proxy* proxy{};
        Sample sample;
    };
    std::array<Pending, 256> pending{};
    std::array<Sample, 512> samples{};
    size_t count{}, dropped{};

    void record(const Sample& sample) noexcept {
        if (!diagnostics) return;
        if (count < samples.size()) samples[count++] = sample;
        else ++dropped;
    }
    void publish(bool force = false) {
        if (!diagnostics || (!force && Clock::now() - published < std::chrono::seconds(1))) return;
        std::ostringstream out;
        out.imbue(std::locale::classic());
        for (size_t i = 0; i < count; ++i) {
            const auto& s = samples[i];
            constexpr std::array<const char*, 6> names{"request", "call", "presented", "discarded", "unavailable", "abandoned"};
            out << "MAKO Renderer: present diagnostics: operation=pacing-profile-wayland version=2 pid=" << getpid()
                << " surface_token=" << token << " event=" << names[static_cast<size_t>(s.event)]
                << " feedback=" << s.id;
            if (s.event == Event::Request || s.event == Event::Unavailable) {
                out << " context=" << s.context << " source=" << s.source
                    << " image_index=" << s.image << " generated=" << s.generated
                    << " requested_ns=" << s.requested << " clock_id=" << s.clock;
                if (s.event == Event::Request)
                    out << " presentation_now_ns=" << s.presentationNow << " calibration_span_ns=" << s.calibrationSpan;
                else out << " reason=" << s.reason;
            } else if (s.event == Event::Call) {
                out << " started_ns=" << s.requested << " finished_ns=" << s.finished << " result=" << s.result;
            } else {
                out << " received_ns=" << s.received;
                if (s.event == Event::Presented)
                    out << " clock_id=" << s.clock << " presented_ns=" << s.presented << " sequence=" << s.sequence
                        << " refresh_ns=" << s.refresh << " flags=" << s.flags << " output_id=" << s.output;
            }
            out << '\n';
        }
        if (dropped)
            out << "MAKO Renderer: present diagnostics: operation=pacing-profile-wayland-gap version=1 pid="
                << getpid() << " surface_token=" << token << " lost_records=" << dropped << '\n';
        present_diagnostics::writePacingProfileRecords(out.str());
        count = dropped = 0;
        published = Clock::now();
    }
    static void globalAdded(void* data, wl_proxy*, uint32_t name, const char* interface, uint32_t version) noexcept {
        auto& self = *static_cast<Impl*>(data);
        if (self.presentation || !version || std::strcmp(interface, "wp_presentation") != 0) return;
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
    }
    static void presented(void* data, wl_proxy*, uint32_t hi, uint32_t lo, uint32_t nsec,
            uint32_t refresh, uint32_t seqHi, uint32_t seqLo, uint32_t flags) noexcept {
        auto& item = *static_cast<Pending*>(data);
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

    uint64_t before(uint64_t context, uint64_t source, uint32_t image, bool generated) {
        if (stopped) return 0;
        if (dispatchPending(display, queue) < 0) { cleanup(); return 0; }
        publish();
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
        if (registry) { destroy(registry); registry = nullptr; }
        if (queue) { destroyQueue(queue); queue = nullptr; }
        try { publish(true); } catch (...) {}
    }
    ~Impl() {
        cleanup();
        if (library) dlclose(library);
    }
};

WaylandPresentationFeedback::WaylandPresentationFeedback() : impl(std::make_unique<Impl>()) {}
WaylandPresentationFeedback::~WaylandPresentationFeedback() = default;

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
        const VkPresentInfoKHR& info, uint64_t context, uint64_t source, bool generated) noexcept {
    auto& self = *static_cast<WaylandPresentationFeedback*>(owner)->impl;
    std::unique_lock lock(self.mutex, std::defer_lock);
    uint64_t id{};
    try {
        lock.lock();
        if (info.swapchainCount == 1 && info.pImageIndices)
            id = self.before(context, source, *info.pImageIndices, generated);
    } catch (...) {}
    const auto started = hostNow();
    const auto result = lower(queue, &info); // Exactly once, unchanged arguments/result.
    const auto finished = hostNow();
    try { if (lock.owns_lock()) self.after(id, started, finished, result); } catch (...) {}
    return result;
}

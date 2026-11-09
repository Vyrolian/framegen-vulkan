/* SPDX-License-Identifier: GPL-3.0-or-later */

// The approach mirrors engine/fgvk-render/src/wayland_presentation_feedback.cpp: a private event queue,
// a proxy wrapper that binds the registry on that queue, and a hand-declared version-1 wire subset.
// No code is shared with fgvk.

#include "wayland_feedback.hpp"

#include "events.hpp"
#include "measure_log.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <mutex>

namespace fgvk::measure {
namespace {

// Opaque libwayland-client types and the wire description used for the subset.
struct wl_proxy;
struct wl_event_queue;
struct wl_interface;

struct wl_message {
    const char* name;
    const char* signature;
    const wl_interface** types;
};

struct wl_interface {
    const char* name;
    int version;
    int methodCount;
    const wl_message* methods;
    int eventCount;
    const wl_message* events;
};

union wl_argument {
    int32_t i;
    uint32_t u;
    const char* s;
    void* o;
    uint32_t n;
};

using CreateQueueFn = wl_event_queue* (*)(wl_display*);
using DestroyQueueFn = void (*)(wl_event_queue*);
using DispatchQueuePendingFn = int (*)(wl_display*, wl_event_queue*);
using CreateWrapperFn = void* (*)(void*);
using DestroyWrapperFn = void (*)(void*);
using ConstructFn = wl_proxy* (*)(wl_proxy*, uint32_t, wl_argument*, const wl_interface*, uint32_t);
using MarshalFn = void (*)(wl_proxy*, uint32_t, wl_argument*);
using ListenFn = int (*)(wl_proxy*, void (**)(void), void*);
using SetQueueFn = void (*)(wl_proxy*, wl_event_queue*);
using DestroyProxyFn = void (*)(wl_proxy*);

template <class T>
bool loadSymbol(void* library, T& target, const char* name) {
    target = reinterpret_cast<T>(dlsym(library, name));
    return target != nullptr;
}

}  // namespace

struct WaylandFeedback::Impl {
    struct Pending {
        Impl* owner{};
        wl_proxy* proxy{};
        uint64_t seq{};
    };

    std::mutex mutex;
    MeasureLog* log{};
    void* library{};
    wl_display* display{};
    wl_event_queue* queue{};
    wl_proxy* registry{};
    wl_proxy* presentation{};
    uint32_t global{};
    bool stopped{};
    bool clockReported{};

    CreateQueueFn createQueue{};
    DestroyQueueFn destroyQueue{};
    DispatchQueuePendingFn dispatchPending{};
    CreateWrapperFn createWrapper{};
    DestroyWrapperFn destroyWrapper{};
    ConstructFn construct{};
    MarshalFn marshal{};
    ListenFn listen{};
    SetQueueFn setQueue{};
    DestroyProxyFn destroy{};
    const wl_interface* registryInterface{};
    const wl_interface* surfaceInterface{};
    const wl_interface* outputInterface{};

    // Never request a target time, a frame callback or a commit with these.
    std::array<const wl_interface*, 2> feedbackTypes{};
    std::array<const wl_interface*, 1> outputTypes{};
    std::array<wl_message, 2> requests{{{"destroy", "", nullptr}, {"feedback", "on", feedbackTypes.data()}}};
    std::array<wl_message, 1> events{{{"clock_id", "u", nullptr}}};
    std::array<wl_message, 3> feedbackEvents{{
        {"sync_output", "o", outputTypes.data()}, {"presented", "uuuuuuu", nullptr}, {"discarded", "", nullptr}}};
    wl_interface feedbackInterface{"wp_presentation_feedback", 1, 0, nullptr, 3, feedbackEvents.data()};
    wl_interface presentationInterface{"wp_presentation", 1, 2, requests.data(), 1, events.data()};

    // Feedback objects are bounded. A request that finds no free entry is simply not made.
    std::array<Pending, 256> pending{};

    static void registryGlobal(void* data, wl_proxy*, uint32_t name, const char* interface, uint32_t version) noexcept {
        auto& self = *static_cast<Impl*>(data);
        if (self.presentation || !version || std::strcmp(interface, "wp_presentation") != 0) return;
        wl_argument args[4]{};
        args[0].u = name;
        args[1].s = self.presentationInterface.name;
        args[2].u = 1;
        self.presentation = self.construct(self.registry, 0, args, &self.presentationInterface, 1);
        if (!self.presentation) return;
        static void (*listener[])(void){reinterpret_cast<void (*)(void)>(&clockIdEvent)};
        if (self.listen(self.presentation, listener, &self) != 0) {
            self.destroy(self.presentation);
            self.presentation = nullptr;
            return;
        }
        self.global = name;
    }

    static void registryGlobalRemove(void* data, wl_proxy*, uint32_t name) noexcept {
        auto& self = *static_cast<Impl*>(data);
        if (name == self.global && self.presentation) {
            self.marshal(self.presentation, 0, nullptr);
            self.destroy(self.presentation);
            self.presentation = nullptr;
        }
    }

    static void clockIdEvent(void* data, wl_proxy*, uint32_t id) noexcept {
        auto& self = *static_cast<Impl*>(data);
        if (self.clockReported) return;
        self.clockReported = true;
        Event event{};
        event.kind = EventKind::Clock;
        event.clockId = id;
        self.log->push(event);
    }

    static void syncOutput(void*, wl_proxy*, wl_proxy*) noexcept {}

    static void presented(void* data, wl_proxy*, uint32_t secondsHi, uint32_t secondsLo, uint32_t nsec,
            uint32_t refresh, uint32_t seqHi, uint32_t seqLo, uint32_t flags) noexcept {
        auto& item = *static_cast<Pending*>(data);
        Event event{};
        event.kind = EventKind::Presented;
        event.seq = item.seq;
        const uint64_t seconds = (uint64_t{secondsHi} << 32) | secondsLo;
        if (nsec < 1'000'000'000 && seconds <= (UINT64_MAX - nsec) / 1'000'000'000) {
            event.presentedNs = seconds * 1'000'000'000 + nsec;
        }
        event.refreshNs = refresh;
        event.flags = flags;
        event.msc = (uint64_t{seqHi} << 32) | seqLo;
        item.owner->log->push(event);
        item.owner->destroy(item.proxy);
        item.proxy = nullptr;
    }

    static void discarded(void* data, wl_proxy*) noexcept {
        auto& item = *static_cast<Pending*>(data);
        Event event{};
        event.kind = EventKind::Discarded;
        event.seq = item.seq;
        item.owner->log->push(event);
        item.owner->destroy(item.proxy);
        item.proxy = nullptr;
    }

    bool initialize(wl_display* borrowedDisplay) {
        display = borrowedDisplay;
        library = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!library) return false;
        if (!loadSymbol(library, createQueue, "wl_display_create_queue") ||
            !loadSymbol(library, destroyQueue, "wl_event_queue_destroy") ||
            !loadSymbol(library, dispatchPending, "wl_display_dispatch_queue_pending") ||
            !loadSymbol(library, createWrapper, "wl_proxy_create_wrapper") ||
            !loadSymbol(library, destroyWrapper, "wl_proxy_wrapper_destroy") ||
            !loadSymbol(library, construct, "wl_proxy_marshal_array_constructor_versioned") ||
            !loadSymbol(library, marshal, "wl_proxy_marshal_array") ||
            !loadSymbol(library, listen, "wl_proxy_add_listener") ||
            !loadSymbol(library, setQueue, "wl_proxy_set_queue") ||
            !loadSymbol(library, destroy, "wl_proxy_destroy")) {
            return false;
        }
        registryInterface = static_cast<const wl_interface*>(dlsym(library, "wl_registry_interface"));
        surfaceInterface = static_cast<const wl_interface*>(dlsym(library, "wl_surface_interface"));
        outputInterface = static_cast<const wl_interface*>(dlsym(library, "wl_output_interface"));
        if (!registryInterface || !surfaceInterface || !outputInterface) return false;
        feedbackTypes = {surfaceInterface, &feedbackInterface};
        outputTypes = {outputInterface};
        queue = createQueue(display);
        if (!queue) return false;
        auto* wrapper = static_cast<wl_proxy*>(createWrapper(display));
        if (!wrapper) return false;
        setQueue(wrapper, queue);
        wl_argument arg{};
        registry = construct(wrapper, 1, &arg, registryInterface, 1);
        destroyWrapper(wrapper);
        if (!registry) return false;
        static void (*registryListener[])(void){reinterpret_cast<void (*)(void)>(&registryGlobal),
                reinterpret_cast<void (*)(void)>(&registryGlobalRemove)};
        // No roundtrip and no socket read: the application's display reader queues our events.
        return listen(registry, registryListener, this) == 0;
    }

    bool request(wl_surface* surface, uint64_t seq) {
        if (stopped || !presentation || !surface) return false;
        Pending* slot = nullptr;
        for (auto& item : pending) {
            if (!item.proxy) {
                slot = &item;
                break;
            }
        }
        if (!slot) return false;
        wl_argument args[2]{};
        args[0].o = surface;
        wl_proxy* proxy = construct(presentation, 1, args, &feedbackInterface, 1);
        if (!proxy) return false;
        static void (*listener[])(void){reinterpret_cast<void (*)(void)>(&syncOutput),
                reinterpret_cast<void (*)(void)>(&presented), reinterpret_cast<void (*)(void)>(&discarded)};
        slot->owner = this;
        slot->seq = seq;
        slot->proxy = proxy;
        if (listen(proxy, listener, slot) != 0) {
            destroy(proxy);
            slot->proxy = nullptr;
            return false;
        }
        return true;
    }

    void dispatch() {
        if (stopped) return;
        if (dispatchPending(display, queue) < 0) cleanup();
    }

    void cleanup() noexcept {
        if (stopped) return;
        stopped = true;
        for (auto& item : pending) {
            if (!item.proxy) continue;
            destroy(item.proxy);
            item.proxy = nullptr;
        }
        if (presentation) {
            marshal(presentation, 0, nullptr);
            destroy(presentation);
            presentation = nullptr;
        }
        if (registry) {
            destroy(registry);
            registry = nullptr;
        }
        if (queue) {
            destroyQueue(queue);
            queue = nullptr;
        }
    }

    ~Impl() {
        cleanup();
    }
};

WaylandFeedback::WaylandFeedback() : impl_(std::make_unique<Impl>()) {}

WaylandFeedback::~WaylandFeedback() = default;

std::shared_ptr<WaylandFeedback> WaylandFeedback::create(wl_display* display, MeasureLog& log) noexcept {
    if (!display) return {};
    try {
        std::shared_ptr<WaylandFeedback> result(new WaylandFeedback());
        result->impl_->log = &log;
        if (result->impl_->initialize(display)) return result;
    } catch (...) {
    }
    return {};
}

bool WaylandFeedback::request(wl_surface* surface, uint64_t seq) noexcept {
    try {
        const std::lock_guard lock(impl_->mutex);
        return impl_->request(surface, seq);
    } catch (...) {
        return false;
    }
}

void WaylandFeedback::dispatch() noexcept {
    try {
        const std::lock_guard lock(impl_->mutex);
        impl_->dispatch();
    } catch (...) {
    }
}

void WaylandFeedback::stop() noexcept {
    try {
        const std::lock_guard lock(impl_->mutex);
        impl_->cleanup();
    } catch (...) {
    }
}

}  // namespace fgvk::measure

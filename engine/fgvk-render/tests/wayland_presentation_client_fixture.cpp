/* SPDX-License-Identifier: GPL-3.0-or-later */
// Runtime ABI peer for the production observer. Deliberately exports no socket
// reader, dispatch/roundtrip, flush, surface-commit or display-disconnect APIs.
#include "wayland_client_abi.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string_view>
#include <vector>

struct wl_event_queue {};
struct wl_proxy {
    const wl_interface* interface{};
    wl_event_queue* queue{};
    void (**listener)(void){};
    void* data{};
    bool submitted{};
    uint32_t id{};
};
struct wl_display {};
struct wl_surface {};
namespace {
wl_display display;
wl_surface surface;
std::vector<wl_proxy*> objects;
struct Event { wl_proxy* proxy; int kind; uint64_t sequence; };
std::deque<Event> events;
int mode{}, queues{}, feedbacks{}, commits{};
uint32_t nextId{10};
wl_proxy* make(const wl_interface* interface, wl_event_queue* queue) {
    auto* proxy = new wl_proxy{.interface=interface, .queue=queue, .id=++nextId};
    objects.push_back(proxy);
    return proxy;
}
}
extern "C" {
extern const wl_interface wl_registry_interface{"wl_registry", 1, 0, nullptr, 0, nullptr};
extern const wl_interface wl_callback_interface{"wl_callback", 1, 0, nullptr, 0, nullptr};
extern const wl_interface wl_surface_interface{"wl_surface", 1, 0, nullptr, 0, nullptr};
extern const wl_interface wl_output_interface{"wl_output", 1, 0, nullptr, 0, nullptr};
wl_display* fgvk_test_wayland_display() { return &display; }
wl_surface* fgvk_test_wayland_surface() { return &surface; }
int fgvk_test_wayland_objects() { return static_cast<int>(objects.size()) + queues; }
int fgvk_test_wayland_feedbacks() { return feedbacks; }
void fgvk_test_wayland_mode(int value) { mode=value; feedbacks=commits=0; }
void fgvk_test_wayland_commit() {
    ++commits;
    for (auto* proxy : objects) {
        if (std::string_view(proxy->interface->name) != "wp_presentation_feedback" || proxy->submitted) continue;
        proxy->submitted=true;
        if (mode != 4) events.push_back({proxy, mode == 3 ? 4 : 3, static_cast<uint64_t>(mode == 5 ? 1 : commits)});
    }
}
wl_event_queue* wl_display_create_queue(wl_display* value) {
    if (value != &display) std::abort();
    ++queues; return new wl_event_queue;
}
void wl_event_queue_destroy(wl_event_queue* queue) { --queues; delete queue; }
void* wl_proxy_create_wrapper(void* value) {
    if (value != &display) std::abort();
    return make(&wl_surface_interface, nullptr);
}
void wl_proxy_destroy(wl_proxy* proxy) {
    std::erase_if(events, [proxy](const auto& event) { return event.proxy == proxy; });
    std::erase(objects, proxy); delete proxy;
}
void wl_proxy_wrapper_destroy(void* proxy) { wl_proxy_destroy(static_cast<wl_proxy*>(proxy)); }
void wl_proxy_set_queue(wl_proxy* proxy, wl_event_queue* queue) { proxy->queue=queue; }
uint32_t wl_proxy_get_id(wl_proxy* proxy) { return proxy->id; }
int wl_proxy_add_listener(wl_proxy* proxy, void (**listener)(void), void* data) {
    if (!proxy->queue || proxy->listener) std::abort();
    proxy->listener=listener; proxy->data=data; return 0;
}
void wl_proxy_marshal_array(wl_proxy* proxy, uint32_t opcode, wl_argument*) {
    if (std::string_view(proxy->interface->name) != "wp_presentation" || opcode != 0) std::abort();
}
wl_proxy* wl_proxy_marshal_array_constructor_versioned(wl_proxy* parent, uint32_t opcode,
        wl_argument* args, const wl_interface* interface, uint32_t version) {
    if (version != 1 || !parent->queue) std::abort();
    const std::string_view kind=interface->name;
    if ((kind == "wl_registry" && opcode != 1) || (kind == "wl_callback" && opcode != 0)) std::abort();
    if (kind == "wp_presentation" && (opcode != 0 || args[0].u != 42 ||
            std::strcmp(args[1].s, "wp_presentation") || args[2].u != 1 || args[3].o)) std::abort();
    if (kind == "wp_presentation_feedback") {
        if (opcode != 1 || args[0].o != &surface || args[1].o || interface->event_count != 3 ||
                std::strcmp(interface->events[1].signature, "uuuuuuu")) std::abort();
        ++feedbacks;
    }
    auto* proxy=make(interface, parent->queue);
    if (kind == "wl_registry" && mode != 1) events.push_back({proxy, 0, 0});
    if (kind == "wl_callback") events.push_back({proxy, 1, 0});
    if (kind == "wp_presentation" && mode != 2) events.push_back({proxy, 2, 0});
    return proxy;
}
int wl_display_dispatch_queue_pending(wl_display* value, wl_event_queue* queue) {
    if (value != &display || !queue) std::abort();
    while (!events.empty()) {
        const auto event=events.front(); events.pop_front();
        auto* p=event.proxy;
        if (p->queue != queue || !p->listener) std::abort();
        if (event.kind == 0) {
            using Fn=void (*)(void*, wl_proxy*, uint32_t, const char*, uint32_t);
            reinterpret_cast<Fn>(p->listener[0])(p->data,p,42,"wp_presentation",3);
        } else if (event.kind == 1 || event.kind == 2) {
            using Fn=void (*)(void*, wl_proxy*, uint32_t);
            reinterpret_cast<Fn>(p->listener[0])(p->data,p,1); // CLOCK_MONOTONIC
        } else if (event.kind == 3) {
            using Fn=void (*)(void*, wl_proxy*, uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
            reinterpret_cast<Fn>(p->listener[1])(p->data,p,0,10,
                static_cast<uint32_t>(event.sequence*8'000'000),8'000'000,0,static_cast<uint32_t>(event.sequence),7);
        } else {
            using Fn=void (*)(void*, wl_proxy*);
            reinterpret_cast<Fn>(p->listener[2])(p->data,p);
        }
    }
    return 0;
}
}

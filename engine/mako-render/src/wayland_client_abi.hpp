/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstdint>

// Public libwayland-client ABI, resolved at runtime by the optional adapters.
// Keep the layer independent of Wayland development headers and link libraries.
struct wl_proxy;
struct wl_display;
struct wl_surface;
struct wl_event_queue;
struct wl_message;
struct wl_interface {
    const char* name;
    int version;
    int method_count;
    const wl_message* methods;
    int event_count;
    const wl_message* events;
};
struct wl_message {
    const char* name;
    const char* signature;
    const wl_interface** types;
};
union wl_argument {
    int32_t i;
    uint32_t u;
    const char* s;
    void* o;
};

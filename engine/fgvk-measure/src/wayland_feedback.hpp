/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <cstdint>
#include <memory>

// libwayland-client types, declared globally so they match the Vulkan WSI structures.
struct wl_display;
struct wl_surface;

namespace fgvk::measure {

class MeasureLog;

// wp_presentation feedback for the game's presents, on a private event queue of this display.
// Requests only the feedback object: never a commit, a frame callback or a target time.
class WaylandFeedback {
public:
    // Binds wp_presentation through a private registry on a private queue. Returns nothing when
    // libwayland or the protocol global is unavailable. Events are queued into `log`.
    static std::shared_ptr<WaylandFeedback> create(wl_display* display, MeasureLog& log) noexcept;

    ~WaylandFeedback();
    WaylandFeedback(const WaylandFeedback&) = delete;
    WaylandFeedback& operator=(const WaylandFeedback&) = delete;

    // Requests feedback for `surface`, tagged with `seq`. Call before the lower present.
    bool request(wl_surface* surface, uint64_t seq) noexcept;

    // Dispatches events already queued on the private queue. It never reads the socket, so the
    // application's own display reader delivers the events.
    void dispatch() noexcept;

    // Destroys the protocol objects. Later requests and dispatches do nothing.
    void stop() noexcept;

private:
    WaylandFeedback();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fgvk::measure

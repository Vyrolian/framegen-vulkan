/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <vulkan/vulkan_core.h>

struct wl_display;
struct wl_surface;

namespace fgvk::layer {

// Opt-in observation of the application's native Wayland surface. Owns its
// proxies/queue and the surface's refresh-phase state, never the display,
// surface, event reader or frame pacing policy.
class WaylandPresentationFeedback {
public:
    static std::shared_ptr<WaylandPresentationFeedback> create(wl_display*, wl_surface*, bool diagnostics) noexcept;
    ~WaylandPresentationFeedback();
    void stop() noexcept;
    static VkResult observe(void* owner, PFN_vkQueuePresentKHR lower, VkQueue queue,
        const VkPresentInfoKHR& info, uint64_t context, uint64_t source, bool generated) noexcept;
    // Returns the refresh-aligned send time for a FIFO output; `desired` on any failure.
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    WaylandPresentationFeedback();
};

}

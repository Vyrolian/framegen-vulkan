/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <vulkan/vulkan_core.h>

#include "swapchain/screen_samples.hpp"

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
    // OS1: the ring that presented outputs of this surface are pushed to, held weakly. With an open sink and
    // correction enabled every present asks for feedback. A presented event with a valid
    // timestamp on CLOCK_MONOTONIC is pushed; other clocks give no sample.
    void setScreenSink(std::weak_ptr<ScreenSampleRing> sink) noexcept;
    // The output refresh in nanoseconds from the last presented feedback; 0 when unknown or variable. Lock-free.
    [[nodiscard]] uint32_t refreshNs() const noexcept;
    // The current-mode rate of the output the surface was last presented on, in mHz; set only while the
    // presented refresh is 0 (VRR); 0 otherwise. Lock-free.
    [[nodiscard]] uint32_t outputMilliHz() const noexcept;
    // feedbackId, when given, receives the feedback this present requested (0 if none).
    static VkResult observe(void* owner, PFN_vkQueuePresentKHR lower, VkQueue queue,
        const VkPresentInfoKHR& info, uint64_t context, uint64_t source, bool generated,
        uint64_t* feedbackId = nullptr) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    WaylandPresentationFeedback();
};

}

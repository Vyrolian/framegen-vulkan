/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "swapchain/reflex_frame_limit.hpp"

#include <cstdint>
#include <mutex>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

// Reflex frame attribution of a managed swapchain's lower presents (VK_NV_low_latency2, wsi.adoc "Frame
// Attribution"). The driver attributes each vkQueuePresentKHR to an open out-of-band present pair, else to the
// most recent PRESENT_START not yet mapped to a present, else to no frame. fgvk owns the out-of-band pair of
// every lower present it makes on this swapchain, so the application's out-of-band markers are not forwarded.
class ReflexPresentAttribution {
public:
    // Whether the application's marker is forwarded to the driver. The out-of-band present pair is fgvk's own
    // on a managed swapchain, so its markers are kept here instead.
    [[nodiscard]] bool applicationMarker(const VkLatencyMarkerNV marker, const uint64_t presentId) {
        const std::lock_guard lock(this->mutex);
        if (marker == VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV) {
            this->outOfBandId = presentId;
            return false;
        }
        if (marker == VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV) {
            this->outOfBandId = 0;
            return false;
        }
        if (marker == VK_LATENCY_MARKER_PRESENT_START_NV) {
            this->presentStartId = presentId;
            this->presentStartMapped = false;
        }
        return true;
    }

    // The Reflex ID of the application's next vkQueuePresentKHR, or 0 for a frame without one. Call once per present.
    [[nodiscard]] uint64_t takeApplicationPresent() {
        const std::lock_guard lock(this->mutex);
        if (this->outOfBandId) return this->outOfBandId;
        if (!this->presentStartMapped) {
            this->presentStartMapped = true;
            return this->presentStartId;
        }
        return 0;
    }

private:
    std::mutex mutex;
    // The open out-of-band present ID, or 0 when none is open.
    uint64_t outOfBandId{};
    // The most recent PRESENT_START ID, and whether a present has taken it.
    uint64_t presentStartId{};
    bool presentStartMapped{true};
};

using ReflexPresentAttributionRegistry = ReflexSwapchainRegistry<ReflexPresentAttribution>;

// Where a managed swapchain's own lower markers go: the driver's vkSetLatencyMarkerNV (never the layer's
// wrapper), and `record` when set, after each marker sent.
struct ReflexMarkerSink {
    VkDevice device{};
    PFN_vkSetLatencyMarkerNV mark{};
    void (*record)(uint64_t presentId, VkLatencyMarkerNV marker) noexcept {};
};

// Runs one lower present made for the application present `presentId` inside fgvk's own out-of-band pair. Nothing
// is sent for ID 0 or without a driver function. The end marker is sent even when `present` throws.
template<class Present>
VkResult attributedPresent(const ReflexMarkerSink& sink, const VkSwapchainKHR swapchain,
        const uint64_t presentId, Present&& present) {
    if (presentId == 0 || !sink.mark) return present();
    const auto mark = [&sink, swapchain, presentId](const VkLatencyMarkerNV marker) {
        const VkSetLatencyMarkerInfoNV info{
            .sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV, .pNext = nullptr,
            .presentID = presentId, .marker = marker,
        };
        sink.mark(sink.device, swapchain, &info);
        if (sink.record) sink.record(presentId, marker);
    };
    mark(VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV);
    struct End {
        decltype(mark)& send;
        ~End() { send(VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV); }
    } end{mark};
    return present();
}

}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vulkan/vulkan_core.h>

namespace vk {
    class Vulkan;
}

namespace fgvk::layer {

    // T4, diagnostics only (FGVK_PACING_PROFILE=1 and FGVK_PACING_PROFILE_GPU_SPLIT=1): a GPU
    // timestamp pair submitted on the private present queue right after each observed lower
    // present. Its start is when that queue got past the present's queue-side work. Values are
    // read only after the marker's own fence signalled, so a read never returns a previous use.
    // Returns null when the gate is off or anything fails (nothing is marked then).
    // S8b: FGVK_REAL_OFFSET_US=auto also creates it; it then keeps finished markers for
    // readPresentMarkerSamples and logs records only when the profile gate is on.
    [[nodiscard]] std::shared_ptr<void> createPresentMarker(const vk::Vulkan& vk, bool calibration) noexcept;

    // S8b: one finished marker. gpuNs is the marker start on the device timestamp clock
    // (ticks times the timestamp period, not calibrated), comparable only with other samples.
    struct PresentMarkerSample {
        uint64_t context{}, source{};
        int64_t gpuNs{};
        bool generated{};
    };
    // Caller holds DeferredPresentQueue::mutex. Copies up to max finished markers newer than
    // *cursor, oldest first, and advances *cursor. Entries the ring has overwritten are skipped.
    size_t readPresentMarkerSamples(void* marker, uint64_t* cursor, PresentMarkerSample* out,
        size_t max) noexcept;

    // Caller holds the queue's external synchronisation (DeferredPresentQueue::mutex). Collects
    // finished markers without waiting, then submits one marker for this present.
    void presentMarkerAfterPresent(void* marker, VkQueue queue, uint64_t context, uint64_t source,
        uint64_t feedback, bool generated) noexcept;

}

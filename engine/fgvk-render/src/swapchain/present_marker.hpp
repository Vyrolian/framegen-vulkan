/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

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
    [[nodiscard]] std::shared_ptr<void> createPresentMarker(const vk::Vulkan& vk, bool calibration) noexcept;

    // Caller holds the queue's external synchronisation (DeferredPresentQueue::mutex). Collects
    // finished markers without waiting, then submits one marker for this present.
    void presentMarkerAfterPresent(void* marker, VkQueue queue, uint64_t context, uint64_t source,
        uint64_t feedback, bool generated) noexcept;

}

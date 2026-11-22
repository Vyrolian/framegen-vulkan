/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

// PT3: the past presentation timing of a gated swapchain, in the application's terms. The on-screen gate gives every lower
// present a lower ID (see onscreen_ids.hpp), so the driver reports lower IDs. This rewrite maps each entry back to the
// application ID it was recorded with and removes the entries that have none. Nothing here calls the driver, so it is
// unit-tested directly.

#include "swapchain/onscreen_ids.hpp"

#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <cstdint>
#include <span>

namespace fgvk::layer {

#if defined(VK_EXT_present_timing)
// Rewrites a past presentation timing result in place and compacts it, keeping the entries in order. An entry whose presentId
// has no application ID is removed. capacities holds, for each slot, the presentStageCount the application passed in, read
// before the driver overwrote it (the driver sets it to the number of stages it wrote). A kept entry that moves into an earlier
// slot copies its scalar fields and its stages into that slot's own pPresentStages array, up to the slot's capacity, and sets
// that slot's presentStageCount to the number copied. Pointers are never moved. A query without entries
// (pPresentationTimings == NULL) is left as it is. Returns the number of entries removed.
[[nodiscard]] inline uint32_t rewritePastPresentationIds(VkPastPresentationTimingPropertiesEXT& result,
        const std::span<const uint32_t> capacities, const OnScreenIds& ids) {
    if (!result.pPresentationTimings) return 0;
    VkPastPresentationTimingEXT* const entries = result.pPresentationTimings;
    const uint32_t count = result.presentationTimingCount;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const auto applicationId = ids.applicationIdOf(entries[i].presentId);
        if (!applicationId) continue;
        if (kept != i) {
            // The source is read by value first, so the scalars are copied whole. Stages are copied only between distinct arrays.
            const VkPastPresentationTimingEXT source = entries[i];
            VkPastPresentationTimingEXT& slot = entries[kept];
            const uint32_t capacity = kept < capacities.size() ? capacities[kept] : 0;
            const uint32_t stages = source.pPresentStages && slot.pPresentStages
                ? std::min(source.presentStageCount, capacity) : 0;
            if (stages) std::copy_n(source.pPresentStages, stages, slot.pPresentStages);
            slot.presentStageCount = stages;
            slot.targetTime = source.targetTime;
            slot.timeDomain = source.timeDomain;
            slot.timeDomainId = source.timeDomainId;
            slot.reportComplete = source.reportComplete;
        }
        entries[kept].presentId = *applicationId;
        ++kept;
    }
    result.presentationTimingCount = kept;
    return count - kept;
}
#endif

} // namespace fgvk::layer

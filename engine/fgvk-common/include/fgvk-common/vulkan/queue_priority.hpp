/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <vulkan/vulkan_core.h>

namespace vk {

    /// The exact experiment names accepted by FGVK_BACKEND_QUEUE_PRIORITY.
    /// Any other value is ignored and produces no change.
    [[nodiscard]] constexpr std::optional<VkQueueGlobalPriorityKHR> requestedQueueGlobalPriority(
            const std::string_view environment) {
        if (environment == "high")
            return VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR;
        if (environment == "realtime")
            return VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR;
        return std::nullopt;
    }

    enum class GlobalPrioritySelectionOutcome : uint8_t {
        NotRequested,
        Unsupported,
        NotListed,
        Selected,
    };

    struct GlobalPrioritySelection {
        GlobalPrioritySelectionOutcome outcome{GlobalPrioritySelectionOutcome::NotRequested};
        std::optional<VkQueueGlobalPriorityKHR> priority;
    };

    /// Choose the requested global priority from the device's capabilities.
    /// extensionKhr means VK_KHR_global_priority is present and `supported`
    /// holds the compute family's list, which is then authoritative. The
    /// extensionExt flag means an attempt-only global priority extension is
    /// present: VK_EXT_global_priority, or VK_KHR_global_priority without a
    /// queryable list. Device creation then decides whether the priority is granted.
    [[nodiscard]] inline GlobalPrioritySelection selectGlobalPriority(
            const std::string_view environment,
            const bool extensionKhr, const bool extensionExt,
            const std::span<const VkQueueGlobalPriorityKHR> supported) {
        const auto requested = requestedQueueGlobalPriority(environment);
        if (!requested)
            return {.outcome = GlobalPrioritySelectionOutcome::NotRequested};
        if (!extensionKhr && !extensionExt)
            return {.outcome = GlobalPrioritySelectionOutcome::Unsupported};
        if (extensionKhr && std::find(supported.begin(), supported.end(), *requested) == supported.end())
            return {.outcome = GlobalPrioritySelectionOutcome::NotListed};
        return {.outcome = GlobalPrioritySelectionOutcome::Selected, .priority = requested};
    }
}

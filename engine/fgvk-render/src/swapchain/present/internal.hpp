/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "fgvk-common/vulkan/external_image.hpp"

#include "present_diagnostics.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string_view>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer::present_detail {
    using DiagnosticsClock = present_diagnostics::Clock;
    using DiagnosticsContextScope = present_diagnostics::ContextScope;

    // Phase timers read the clock only with the pacing profile. Without it they return zero
    // durations, so the presentation path reads no extra clock.
    inline DiagnosticsClock::time_point startPhaseTimer() {
        return present_diagnostics::pacingProfileEnabled()
            ? DiagnosticsClock::now() : DiagnosticsClock::time_point{};
    }

    inline DiagnosticsClock::duration finishPhaseTimer(
            const DiagnosticsClock::time_point started) {
        if (!present_diagnostics::pacingProfileEnabled())
            return {};
        return DiagnosticsClock::now() - started;
    }

    struct PresentPhaseDurations {
        DiagnosticsClock::duration renderFence{};
        DiagnosticsClock::duration schedule{};
        DiagnosticsClock::duration sourceCopy{};
        DiagnosticsClock::duration acquire{};
        DiagnosticsClock::duration generatedSubmit{};
        DiagnosticsClock::duration generatedPresent{};
        DiagnosticsClock::duration originalPresent{};
    };

    // Hands one present's phase durations to the pacing profile. Nothing is recorded without it.
    inline void recordPresentPhases(const uint64_t contextId,
            const DiagnosticsClock::duration totalDuration,
            const PresentPhaseDurations& phases) {
        if (!present_diagnostics::pacingProfileEnabled())
            return;
        const auto attributed = phases.renderFence + phases.schedule + phases.sourceCopy +
            phases.acquire + phases.generatedSubmit + phases.generatedPresent + phases.originalPresent;
        present_diagnostics::recordPacingProfilePhases(contextId, {
            totalDuration, phases.renderFence, phases.schedule, phases.sourceCopy,
            phases.acquire, phases.generatedSubmit, phases.generatedPresent,
            phases.originalPresent, std::max(DiagnosticsClock::duration{}, totalDuration - attributed)});
    }

    inline VkImageMemoryBarrier barrierHelper(const VkImage handle,
            const VkAccessFlags srcAccessMask,
            const VkAccessFlags dstAccessMask,
            const VkImageLayout oldLayout,
            const VkImageLayout newLayout) {
        return VkImageMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = srcAccessMask,
            .dstAccessMask = dstAccessMask,
            .oldLayout = oldLayout,
            .newLayout = newLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = handle,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };
    }
}

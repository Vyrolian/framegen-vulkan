/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "fgvk-common/vulkan/gpu_clock.hpp"
#include "fgvk-common/vulkan/queue_priority.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }

    void testTickMapping() {
        constexpr int64_t base = 5'000'000'000;
        constexpr uint64_t wrap36 = (uint64_t{1} << 36) - 10;
        constexpr uint64_t wrap64 = UINT64_MAX - 5;

        // Ticks after and before the calibration point, period 1.0 and 0.5.
        expect(vk::gpuTicksToMonotonicNs(1500, 1000, base, 64, 1.0F) == base + 500,
            "ticks after the calibration point must advance by period 1.0");
        expect(vk::gpuTicksToMonotonicNs(900, 1000, base, 64, 1.0F) == base - 100,
            "ticks before the calibration point must move back by period 1.0");
        expect(vk::gpuTicksToMonotonicNs(1500, 1000, base, 64, 0.5F) == base + 250,
            "ticks after the calibration point must advance by period 0.5");
        expect(vk::gpuTicksToMonotonicNs(900, 1000, base, 64, 0.5F) == base - 50,
            "ticks before the calibration point must move back by period 0.5");

        // A value exactly at the calibration point is the calibration time.
        expect(vk::gpuTicksToMonotonicNs(1000, 1000, base, 64, 1.0F) == base,
            "the calibration tick must map to the calibration time");
        expect(vk::gpuTicksToMonotonicNs(1000, 1000, base, 36, 0.5F) == base,
            "the calibration tick must map to the calibration time for a masked width");

        // A 36-bit counter that wraps between the calibration point and the sample.
        expect(vk::gpuTicksToMonotonicNs(5, wrap36, base, 36, 1.0F) == base + 15,
            "a 36-bit wrap after the calibration point must map forward");
        expect(vk::gpuTicksToMonotonicNs(6, wrap36, base, 36, 0.5F) == base + 8,
            "a 36-bit wrap must scale by the timestamp period");
        expect(vk::gpuTicksToMonotonicNs((uint64_t{1} << 36) - 3, 5, base, 36, 1.0F) == base - 8,
            "a 36-bit sample before the wrap must map backward");

        // A 64-bit counter wraps through the full range.
        expect(vk::gpuTicksToMonotonicNs(10, wrap64, base, 64, 1.0F) == base + 16,
            "a 64-bit wrap after the calibration point must map forward");
        expect(vk::gpuTicksToMonotonicNs(UINT64_MAX - 3, 10, base, 64, 1.0F) == base - 14,
            "a 64-bit sample before the wrap must map backward");

        // Invalid properties are reported as unknown rather than guessed.
        expect(vk::gpuTicksToMonotonicNs(1, 0, base, 0, 1.0F) == 0,
            "a zero valid-bit count must be unknown");
        expect(vk::gpuTicksToMonotonicNs(1, 0, base, 65, 1.0F) == 0,
            "an oversized valid-bit count must be unknown");
        expect(vk::gpuTicksToMonotonicNs(1, 0, base, 64, 0.0F) == 0,
            "a zero timestamp period must be unknown");
    }

    void testGlobalPrioritySelection() {
        using vk::GlobalPrioritySelectionOutcome;
        const std::array<VkQueueGlobalPriorityKHR, 0> none{};
        const std::array lowAndMedium{
            VK_QUEUE_GLOBAL_PRIORITY_LOW, VK_QUEUE_GLOBAL_PRIORITY_MEDIUM,
        };
        const std::array highAndRealtime{
            VK_QUEUE_GLOBAL_PRIORITY_HIGH, VK_QUEUE_GLOBAL_PRIORITY_REALTIME,
        };

        // Unset or other values request nothing and select nothing.
        expect(vk::selectGlobalPriority("", true, true, highAndRealtime).outcome ==
                GlobalPrioritySelectionOutcome::NotRequested,
            "an unset request must not select a priority");
        expect(vk::selectGlobalPriority("medium", true, false, highAndRealtime).outcome ==
                GlobalPrioritySelectionOutcome::NotRequested,
            "an unknown request must not select a priority");
        expect(vk::selectGlobalPriority("HIGH", true, false, highAndRealtime).outcome ==
                GlobalPrioritySelectionOutcome::NotRequested,
            "requests are exact, case-sensitive values");

        // Without either extension the requested priority is unsupported.
        expect(vk::selectGlobalPriority("high", false, false, highAndRealtime).outcome ==
                GlobalPrioritySelectionOutcome::Unsupported,
            "a request without the extension must be unsupported");

        // With the KHR list, a priority the family does not list is refused.
        expect(vk::selectGlobalPriority("high", true, false, lowAndMedium).outcome ==
                GlobalPrioritySelectionOutcome::NotListed,
            "a priority absent from the family list must not be selected");

        // A listed priority is selected, and the EXT attempt needs no list.
        const auto realtime = vk::selectGlobalPriority("realtime", true, false, highAndRealtime);
        expect(realtime.outcome == GlobalPrioritySelectionOutcome::Selected &&
                realtime.priority == VK_QUEUE_GLOBAL_PRIORITY_REALTIME,
            "a listed realtime request must be selected");
        const auto attempt = vk::selectGlobalPriority("high", false, true, none);
        expect(attempt.outcome == GlobalPrioritySelectionOutcome::Selected &&
                attempt.priority == VK_QUEUE_GLOBAL_PRIORITY_HIGH,
            "an EXT-only request must be selected for a creation attempt");
    }
}

int main() {
    testTickMapping();
    testGlobalPrioritySelection();
    return 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cmath>
#include <cstdint>

#include <vulkan/vulkan_core.h>

namespace vk {

    /// Convert device timestamp ticks to CLOCK_MONOTONIC nanoseconds with one
    /// calibration pair. Ticks may lie before or after the calibration point:
    /// the masked difference is read as signed within half the valid range.
    /// Returns zero for invalid inputs; callers report that as unknown.
    [[nodiscard]] inline int64_t gpuTicksToMonotonicNs(const uint64_t ticks,
            const uint64_t calibrationTicks, const int64_t calibrationMonotonicNs,
            const uint32_t validBits, const float periodNs) {
        if (validBits == 0 || validBits > 64 || !std::isfinite(periodNs) || periodNs <= 0.0F)
            return 0;
        const uint64_t mask = validBits == 64 ? UINT64_MAX : (uint64_t{1} << validBits) - 1;
        const uint64_t difference = (ticks - calibrationTicks) & mask;
        // Modular subtraction maps a masked difference in the upper half to a negative offset.
        const uint64_t half = uint64_t{1} << (validBits - 1);
        const uint64_t range = validBits == 64 ? uint64_t{0} : uint64_t{1} << validBits;
        const auto signedTicks = static_cast<int64_t>(difference >= half ? difference - range : difference);
        const double nanoseconds = static_cast<double>(signedTicks) * static_cast<double>(periodNs);
        if (!(std::fabs(nanoseconds) < 9.0e18))
            return 0;
        return calibrationMonotonicNs + static_cast<int64_t>(std::llround(nanoseconds));
    }

    /// Latest device and CLOCK_MONOTONIC pair from vkGetCalibratedTimestamps.
    /// The owner calls recalibrate() at most once per second. A failed call
    /// keeps the previous pair, and an uncalibrated clock reports zero.
    class CalibratedClock {
    public:
        CalibratedClock() = default;
        CalibratedClock(VkDevice device, PFN_vkGetCalibratedTimestampsKHR calibrate,
                uint32_t validBits, float periodNs) :
            device(device), calibrate(calibrate), validBits(validBits), periodNs(periodNs) {}

        bool recalibrate() noexcept {
            if (!this->calibrate || this->device == VK_NULL_HANDLE)
                return false;
            VkCalibratedTimestampInfoKHR deviceInfo{};
            deviceInfo.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
            deviceInfo.timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
            VkCalibratedTimestampInfoKHR monotonicInfo{};
            monotonicInfo.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
            monotonicInfo.timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
            const VkCalibratedTimestampInfoKHR infos[2]{deviceInfo, monotonicInfo};
            uint64_t values[2]{};
            uint64_t deviation{};
            if (this->calibrate(this->device, 2, infos, values, &deviation) != VK_SUCCESS)
                return false;
            this->deviceTicks = values[0];
            this->monotonicNs = static_cast<int64_t>(values[1]);
            this->valid = true;
            return true;
        }

        [[nodiscard]] bool calibrated() const noexcept { return this->valid; }

        [[nodiscard]] int64_t monotonicNanoseconds(const uint64_t ticks) const noexcept {
            if (!this->valid)
                return 0;
            return gpuTicksToMonotonicNs(ticks, this->deviceTicks, this->monotonicNs,
                this->validBits, this->periodNs);
        }

    private:
        VkDevice device{VK_NULL_HANDLE};
        PFN_vkGetCalibratedTimestampsKHR calibrate{};
        uint32_t validBits{};
        float periodNs{};
        uint64_t deviceTicks{};
        int64_t monotonicNs{};
        bool valid{};
    };
}

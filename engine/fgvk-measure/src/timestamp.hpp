/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

namespace fgvk::measure {

// Timestamp bits and tick length of one queue family: VkQueueFamilyProperties::timestampValidBits
// and VkPhysicalDeviceLimits::timestampPeriod (nanoseconds per tick).
struct TickDomain {
    uint32_t validBits{};
    double periodNs{};
};

// A device timestamp and the CLOCK_MONOTONIC reading that vkGetCalibratedTimestampsKHR returned
// with it.
struct Calibration {
    uint64_t deviceTicks{};
    uint64_t hostNs{};
};

inline uint64_t timestampMask(uint32_t validBits) noexcept {
    return validBits >= 64 ? ~uint64_t{0} : (uint64_t{1} << validBits) - 1;
}

// Converts a query timestamp to CLOCK_MONOTONIC nanoseconds. Only the valid bits count. The tick
// difference from the calibration is taken modulo 2^validBits and read as signed, so a counter that
// wrapped between calibration and query still lands on the correct side of the calibration. No value
// comes back when the family has no timestamps, the period is unusable, or the result would fall
// before the clock's zero.
inline std::optional<uint64_t> ticksToHostNs(uint64_t ticks, const Calibration& calibration,
        const TickDomain& domain) noexcept {
    if (domain.validBits == 0 || !(domain.periodNs > 0.0)) return std::nullopt;
    const uint64_t mask = timestampMask(domain.validBits);
    const uint64_t half = mask >> 1;
    const uint64_t reference = calibration.deviceTicks & mask;
    const uint64_t value = ticks & mask;
    const uint64_t forward = (value - reference) & mask;
    int64_t tickDelta = 0;
    if (forward <= half) {
        tickDelta = static_cast<int64_t>(forward);
    } else {
        const uint64_t back = (reference - value) & mask;
        if (back > half) return std::nullopt;
        tickDelta = -static_cast<int64_t>(back);
    }
    const double scaled = static_cast<double>(tickDelta) * domain.periodNs;
    if (!(std::fabs(scaled) < 9.0e18)) return std::nullopt;
    const auto delta = static_cast<int64_t>(std::llround(scaled));
    if (delta >= 0) return calibration.hostNs + static_cast<uint64_t>(delta);
    const auto back = static_cast<uint64_t>(-delta);
    if (back > calibration.hostNs) return std::nullopt;
    return calibration.hostNs - back;
}

}  // namespace fgvk::measure

/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstdint>
#include <limits>

namespace mako::layer {

// It derives the automatic cap ceiling for uncapped Fixed frame generation
// from Vulkan present timing; the cap is refresh divided by the multiplier
// and never moves with GPU load.
class UncappedAutoCap {
public:
    // VK_EXT_present_timing reports nanoseconds; unknown values are all ones.
    [[nodiscard]] static double refreshHzFromTiming(uint64_t refreshDuration, uint64_t refreshInterval) {
        constexpr uint64_t unknown = std::numeric_limits<uint64_t>::max();
        double hz = 0.0;
        if (refreshDuration > 0 && refreshDuration < unknown)
            hz = 1e9 / static_cast<double>(refreshDuration);
        else if (refreshInterval > 0 && refreshInterval < unknown)
            hz = 1e9 / static_cast<double>(refreshInterval);
        return hz >= 20.0 && hz <= 1000.0 ? hz : 0.0;
    }
};

} // namespace mako::layer

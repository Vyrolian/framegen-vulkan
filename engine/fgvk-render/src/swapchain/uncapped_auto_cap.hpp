/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstdint>
#include <limits>

namespace fgvk::layer {

// It derives the automatic cap ceiling for uncapped Fixed frame generation
// from Vulkan present timing; the cap is refresh divided by the multiplier
// and never moves with GPU load.
class UncappedAutoCap {
public:
    // Every refresh source (timing, X11 RandR, Wayland output, wp_presentation) passes this range; outside it is unknown.
    [[nodiscard]] static double refreshHzInRange(double hz) {
        return hz >= 20.0 && hz <= 1000.0 ? hz : 0.0;
    }

    // VK_EXT_present_timing reports nanoseconds; unknown values are all ones.
    [[nodiscard]] static double refreshHzFromTiming(uint64_t refreshDuration, uint64_t refreshInterval) {
        constexpr uint64_t unknown = std::numeric_limits<uint64_t>::max();
        double hz = 0.0;
        if (refreshDuration > 0 && refreshDuration < unknown)
            hz = 1e9 / static_cast<double>(refreshDuration);
        else if (refreshInterval > 0 && refreshInterval < unknown)
            hz = 1e9 / static_cast<double>(refreshInterval);
        return refreshHzInRange(hz);
    }

    // wp_presentation's presented event reports the output's refresh in nanoseconds; 0 is unknown or variable.
    [[nodiscard]] static double refreshHzFromWaylandNs(uint32_t refreshNs) {
        return refreshNs ? refreshHzInRange(1e9 / static_cast<double>(refreshNs)) : 0.0;
    }

    // Whether a native Wayland present asks for presentation feedback. Until a refresh is known every
    // 8th present does; once known, every 240th does.
    [[nodiscard]] static bool waylandFeedbackDue(uint64_t presents, bool known) {
        return known ? presents % 240 == 0 : presents % 8 == 0;
    }

    // Whether this present queries the driver's refresh. Until a rate is known
    // (known is false), the first 64 presents query and then every 8th present does.
    // Once known, a query runs only on multiples of 240 from present 240 on.
    [[nodiscard]] static bool refreshQueryDue(uint64_t presents, bool known) {
        if (!known)
            return presents < 64 || presents % 8 == 0;
        return presents >= 64 && presents % 240 == 0;
    }
};

} // namespace fgvk::layer

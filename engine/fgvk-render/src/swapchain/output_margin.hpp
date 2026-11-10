/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace fgvk::layer {
/// Optional reserve after readiness. The integral bias compensates persistent
/// source-clock drift; bounded positive steps never move an output earlier.
struct OutputMarginController {
    using Duration = std::chrono::steady_clock::duration;
    Duration bias{};

    [[nodiscard]] constexpr Duration step(Duration slack, Duration margin) noexcept {
        if (margin <= Duration{}) {
            reset();
            return {};
        }
        const auto error = std::clamp(margin - slack, -margin, margin);
        bias = std::clamp(bias + error / 32, Duration{}, margin / 4);
        // Anti-windup at the reserve ceiling. The specified positive-only PI
        // rule otherwise strands zero-drift slack above the reserve forever.
        // Retain the allowed margin/8 tolerance so 0.4 ms/batch drift can
        // still be compensated by the bounded 0.5 ms step at a 2 ms margin.
        const auto ceilingStep = std::max(Duration{}, margin + margin / 8 - slack);
        return std::min(std::clamp(error / 8 + bias, Duration{}, margin / 4), ceilingStep);
    }
    constexpr void reset() noexcept { bias = {}; }
};

[[nodiscard]] inline std::chrono::steady_clock::duration outputMargin() noexcept {
    static const auto margin = [] {
        const char* text = std::getenv("FGVK_OUTPUT_MARGIN_MS");
        char* end = nullptr;
        const double value = text ? std::strtod(text, &end) : 0.0;
        if (!text || end == text || !(value > 0.0))
            return std::chrono::steady_clock::duration{};
        const double clamped = value < 2.0 ? value : 2.0;
        std::fprintf(stderr, "framegen-vulkan: output margin: margin_ms=%g\n", clamped);
        return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::milli>(clamped));
    }();
    return margin;
}

}

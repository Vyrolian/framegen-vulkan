/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace fgvk::layer {
/// Optional output margin (B1, FGVK_OUTPUT_MARGIN_MS, 0 to 2 ms, default 0 = off).
/// A released batch normally presents the moment its generated frame is ready,
/// so any later readiness moves the frames on screen. With a margin the output
/// clock is steered to sit up to that long after readiness: each batch whose
/// slack (slot minus readiness, negative when late) is below the margin moves
/// the clock an eighth of the missing slack later. Small steps keep the spacing
/// even; the added latency never exceeds the margin.
[[nodiscard]] constexpr std::chrono::steady_clock::duration outputMarginStep(
        const std::chrono::steady_clock::duration slack,
        const std::chrono::steady_clock::duration margin) noexcept {
    using Duration = std::chrono::steady_clock::duration;
    const auto held = slack > Duration{} ? slack : Duration{};
    return held < margin ? (margin - held) / 8 : Duration{};
}
static_assert(outputMarginStep(std::chrono::milliseconds{0}, std::chrono::milliseconds{0}).count() == 0);
static_assert(outputMarginStep(std::chrono::milliseconds{-5}, std::chrono::milliseconds{2}) ==
    std::chrono::microseconds{250});
static_assert(outputMarginStep(std::chrono::milliseconds{1}, std::chrono::milliseconds{2}) ==
    std::chrono::microseconds{125});
static_assert(outputMarginStep(std::chrono::milliseconds{3}, std::chrono::milliseconds{2}).count() == 0);

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

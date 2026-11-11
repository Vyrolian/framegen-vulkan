/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fgvk::layer {
/// Optional reserve after readiness. The integral bias compensates persistent
/// source-clock drift; bounded positive steps never move an output earlier.
struct OutputMarginController {
    using Duration = std::chrono::steady_clock::duration;
    Duration bias{};
    /// The last stepped batch ended with less reserve than the margin. The
    /// worker then keeps full spacing from the previous output instead of
    /// its sixteenth-interval catch-up, which otherwise drains the reserve.
    bool below{};

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
        const auto applied = std::min(std::clamp(error / 8 + bias, Duration{}, margin / 4), ceilingStep);
        below = std::max(slack, Duration{}) + applied < margin;
        return applied;
    }
    constexpr void reset() noexcept { bias = {}; below = false; }
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

// S6: FGVK_GENERATED_YIELD=1 makes the game queue wait for the worker's early generated
// copy, so the copy does not wait behind the game's GPU timeslice. Off by default; needs
// the early preparation (FGVK_INTERNAL_PREPARE not 0). Read once per process.
[[nodiscard]] inline bool generatedYieldEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("FGVK_GENERATED_YIELD");
        const char* prepare = std::getenv("FGVK_INTERNAL_PREPARE");
        const bool on = value && std::strcmp(value, "1") == 0 &&
            !(prepare && std::strcmp(prepare, "0") == 0);
        if (on) std::fprintf(stderr, "framegen-vulkan: generated yield: enabled\n");
        return on;
    }();
    return enabled;
}

// S7: FGVK_REAL_OFFSET_US=<-1000..1000> moves the real output that follows a generated
// output on released internal-image batches by that many microseconds (negative =
// earlier). The next batch's first-output floor ignores the move, so only reals shift.
// It centres the generated output between its reals when the two kinds wait unequally
// for the private queue's GPU turn. 0 or unset = unchanged timing. Read once per process.
template <typename TimePoint, typename Duration>
[[nodiscard]] constexpr TimePoint realDeadlineWithOffset(const TimePoint deadline,
        const TimePoint generatedStarted, const Duration interval, const Duration offset) noexcept {
    // Never closer to the generated present than a quarter output interval, and
    // never later because of that bound than the unmoved deadline.
    return offset == Duration{} ? deadline
        : std::max(deadline + offset, std::min(deadline, generatedStarted + interval / 4));
}
[[nodiscard]] inline std::chrono::steady_clock::duration realOffset() noexcept {
    static const auto offset = [] {
        const char* text = std::getenv("FGVK_REAL_OFFSET_US");
        char* end = nullptr;
        const long value = text ? std::strtol(text, &end, 10) : 0;
        if (!text || end == text || value == 0)
            return std::chrono::steady_clock::duration{};
        const long clamped = std::clamp(value, -1000L, 1000L);
        std::fprintf(stderr, "framegen-vulkan: real offset: offset_us=%ld\n", clamped);
        return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::microseconds{clamped});
    }();
    return offset;
}

}

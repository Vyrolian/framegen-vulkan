/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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
// S8d: the part of a real's move that took effect. A join expedite or a late wake can
// start the real before its moved deadline; a later move counts only up to the actual
// start, an earlier one (negative) cannot be undone by starting even earlier.
template <typename TimePoint, typename Duration>
[[nodiscard]] constexpr Duration realOffsetEffective(const Duration applied, const TimePoint unmoved,
        const TimePoint scheduled, const TimePoint started) noexcept {
    return applied > Duration{}
        ? std::clamp(Duration{std::min(started, scheduled) - unmoved}, Duration{}, applied) : applied;
}
// S8d: the sign of the commanded offset when the real did not get it (by more than 50 us),
// else 0. The automatic controller does not integrate further in that direction.
template <typename Duration>
[[nodiscard]] constexpr int realOffsetLimited(const Duration commanded, const Duration effective) noexcept {
    constexpr Duration tolerance = std::chrono::microseconds{50};
    if (commanded > Duration{}) return effective < commanded - tolerance ? 1 : 0;
    if (commanded < Duration{}) return effective > commanded + tolerance ? -1 : 0;
    return 0;
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

// S8b: the automatic real offset derives the S7 offset at run time from the GPU present
// marker: the start of the private queue's next GPU work after each lower present, which is
// when that present reaches the screen up to a constant. On by the `[global]
// pacing_correction` key (default true), which the layer stores here at startup, before any
// device exists; without that (tests, tools) it is off. FGVK_REAL_OFFSET_US overrides:
// `auto` turns it on, any other value (a number, 0) turns it off and keeps S7's fixed value.
inline std::atomic<bool> realOffsetAutoConfigured{false};
[[nodiscard]] inline bool realOffsetAuto() noexcept {
    static const int fromEnvironment = [] {
        const char* text = std::getenv("FGVK_REAL_OFFSET_US");
        if (!text) return -1;
        return std::strcmp(text, "auto") == 0 ? 1 : 0;
    }();
    const bool on = fromEnvironment >= 0 ? fromEnvironment == 1
        : realOffsetAutoConfigured.load(std::memory_order_relaxed);
    static std::atomic<bool> reported{false};
    if (on && !reported.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "framegen-vulkan: real offset: auto\n");
    return on;
}
// S8b diagnostics only: the last worker's current auto offset and accepted samples.
inline std::atomic<int64_t> realOffsetAutoNs{0};
inline std::atomic<uint64_t> realOffsetAutoSamples{0};
inline std::atomic<uint64_t> realOffsetAutoHeld{0};

/// S8b: integrates the generated output's distance from the midpoint of its two reals,
/// measured on the private queue's GPU clock, into the real offset. A positive error
/// (generated late) moves the reals later. Only the mean is corrected: the per-frame
/// GPU-turn wait is decided after the present call. Worker thread only. S8d: while the
/// last real could not take the offset (`limited`, its sign), samples pushing further that
/// way are held: counted in `held`, not integrated, not accepted.
struct RealOffsetController {
    using Duration = std::chrono::nanoseconds;
    static constexpr Duration limit = std::chrono::microseconds{1000};
    static constexpr Duration errorClamp = std::chrono::milliseconds{1};
    static constexpr Duration errorReject = std::chrono::milliseconds{3};
    static constexpr Duration minStep = std::chrono::milliseconds{8};
    static constexpr Duration maxStep = std::chrono::milliseconds{25};
    static constexpr int64_t gain = 64;
    Duration offset{};
    Duration lastError{};
    uint64_t accepted{};
    uint64_t held{};
    int limited{};
    bool haveReal{}, haveGenerated{};
    uint64_t realSource{}, generatedSource{};
    Duration realAt{}, generatedAt{};

    /// One finished marker, in present order. Returns true when it completed an accepted sample.
    constexpr bool observe(const bool generated, const uint64_t source, const Duration at) noexcept {
        if (generated) {
            generatedSource = source;
            generatedAt = at;
            haveGenerated = true;
            return false;
        }
        bool used = false;
        if (haveReal && haveGenerated && generatedSource == source && realSource + 1 == source &&
                realAt < generatedAt && generatedAt < at) {
            const auto step = at - realAt;
            if (step >= minStep && step <= maxStep) {
                const auto error = generatedAt - (realAt + step / 2);
                if (error <= errorReject && error >= -errorReject) {
                    lastError = error;
                    if ((limited > 0 && error > Duration{}) || (limited < 0 && error < Duration{})) {
                        ++held;
                    } else {
                        offset = std::clamp(offset + std::clamp(error, -errorClamp, errorClamp) / gain,
                            -limit, limit);
                        ++accepted;
                        used = true;
                    }
                }
            }
        }
        haveReal = true;
        realSource = source;
        realAt = at;
        haveGenerated = false;
        return used;
    }
    /// Drops the pending markers; the offset and `limited` are kept.
    constexpr void forget() noexcept { haveReal = haveGenerated = false; }
};

}

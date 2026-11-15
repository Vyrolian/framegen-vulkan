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

/// S8b: integrates the mean distance of a source's generated outputs from the midpoint of
/// its two reals, measured on the private queue's GPU clock, into the real offset. A positive
/// error (generated late) moves the reals later. For N - 1 evenly placed generated outputs the
/// mean expected position is the midpoint for every N, so one offset serves every multiplier.
/// Only the mean is corrected: the per-frame GPU-turn wait is decided after the present call.
/// Worker thread only. S8d: while the last real could not take the offset (`limited`, its
/// sign), samples pushing further that way are held: counted in `held`, not integrated, not accepted.
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
    // Generated outputs per source of the current batch (multiplier - 1), set by the worker.
    // A source with a different count gives no sample.
    uint32_t expectedGenerated{1};
    bool haveReal{};
    uint64_t realSource{}, generatedSource{};
    uint32_t generatedCount{};
    Duration realAt{}, generatedSum{}, generatedFirst{}, generatedLast{};

    /// One finished marker, in present order. Returns true when it completed an accepted sample.
    constexpr bool observe(const bool generated, const uint64_t source, const Duration at) noexcept {
        if (generated) {
            if (generatedCount == 0 || generatedSource != source) {
                generatedSource = source;
                generatedCount = 1;
                generatedSum = at;
                generatedFirst = generatedLast = at;
            } else {
                ++generatedCount;
                generatedSum += at;
                generatedLast = at;
            }
            return false;
        }
        bool used = false;
        if (haveReal && generatedCount != 0 && generatedCount == expectedGenerated &&
                generatedSource == source && realSource + 1 == source &&
                realAt < generatedFirst && generatedLast < at) {
            const auto step = at - realAt;
            if (step >= minStep && step <= maxStep) {
                const auto error = generatedSum / generatedCount - (realAt + step / 2);
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
        generatedCount = 0;
        return used;
    }
    /// Drops the pending markers; the offset and `limited` are kept.
    constexpr void forget() noexcept { haveReal = false; generatedCount = 0; }
};

}

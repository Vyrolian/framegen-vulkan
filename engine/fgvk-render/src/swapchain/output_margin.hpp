/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fgvk::layer {

// W2: a generated output on the worker copy path moves its own deadline by its commanded
// offset (negative = earlier, from the automatic controller). Never closer to the previous
// output's start than a quarter output interval, and never later because of that bound than
// the unmoved deadline.
template <typename TimePoint, typename Duration>
[[nodiscard]] constexpr TimePoint deadlineWithOffset(const TimePoint deadline,
        const TimePoint previousStarted, const Duration interval, const Duration offset) noexcept {
    return offset == Duration{} ? deadline
        : std::max(deadline + offset, std::min(deadline, previousStarted + interval / 4));
}
// W2: the part of a generated output's move that took effect. A later move counts only up to
// the final scheduled deadline: a join expedite or a late wake can start the output before its
// moved slot, and the wake itself is ignored. An earlier move (negative) counts only as far as
// the final deadline went, so a raise that undoes it counts as the part that remains.
template <typename TimePoint, typename Duration>
[[nodiscard]] constexpr Duration outputOffsetEffective(const Duration applied, const TimePoint unmoved,
        const TimePoint scheduled, const TimePoint started) noexcept {
    if (applied > Duration{})
        return std::clamp(Duration{std::min(started, scheduled) - unmoved}, Duration{}, applied);
    if (applied < Duration{})
        return std::clamp(Duration{scheduled - unmoved}, applied, Duration{});
    return Duration{};
}
// W2: the sign of the commanded offset when the output did not get it (by more than 50 us),
// else 0. The automatic controller does not integrate further in that direction.
template <typename Duration>
[[nodiscard]] constexpr int outputOffsetLimited(const Duration commanded, const Duration effective) noexcept {
    constexpr Duration tolerance = std::chrono::microseconds{50};
    if (commanded > Duration{}) return effective < commanded - tolerance ? 1 : 0;
    if (commanded < Duration{}) return effective > commanded + tolerance ? -1 : 0;
    return 0;
}

// FGVK_REAL_OFFSET_US=auto turns the automatic output correction on, and any other value (a
// number, 0) turns it off; there is no fixed offset. Unset, it follows the `[global]
// pacing_correction` key (default true), which the layer stores here at startup, before any
// device exists; without that (tests, tools) it is off.
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
// Diagnostics only: the last worker's equivalent real offset, accepted samples and held positions.
inline std::atomic<int64_t> realOffsetAutoNs{0};
inline std::atomic<uint64_t> realOffsetAutoSamples{0};
inline std::atomic<uint64_t> realOffsetAutoHeld{0};
// OS1, diagnostics only: the last worker's correction source (true: screen samples), the accepted screen samples
// since the last summary, and the screen ring's overflow total.
inline std::atomic<bool> realOffsetScreen{false};
inline std::atomic<uint64_t> realOffsetScreenSamples{0};
inline std::atomic<uint64_t> realOffsetScreenDropped{0};

/// W2: one correction per generated output. Position k is the k-th generated output of a batch
/// (k < maxPositions). Its ideal point is the (k + 1)-th of expectedGenerated + 1 even points
/// between the two reals of its source, measured on the private queue's GPU clock: a late output
/// (positive error) gets a negative offset and moves earlier. Each position is an independent
/// integrator with the same gain, clamp and reject. The reals are never moved. While a position
/// could not take its offset (`limited`, its sign), samples pushing further that way are held:
/// counted in `held`, not integrated. Worker thread only.
struct OutputOffsetController {
    using Duration = std::chrono::nanoseconds;
    static constexpr size_t maxPositions = 4;
    static constexpr Duration limit = std::chrono::microseconds{2500};
    static constexpr Duration errorClamp = std::chrono::milliseconds{1};
    static constexpr Duration errorReject = std::chrono::milliseconds{3};
    static constexpr Duration minStep = std::chrono::milliseconds{8};
    static constexpr Duration maxStep = std::chrono::milliseconds{25};
    static constexpr int64_t gain = 64;
    std::array<Duration, maxPositions> offset{};
    std::array<int, maxPositions> limited{};
    Duration lastError{};
    uint64_t accepted{};
    uint64_t held{};
    // Generated outputs per source of the current batch (multiplier - 1), set by the worker.
    // A source with a different count gives no sample.
    uint32_t expectedGenerated{1};
    bool haveReal{};
    uint64_t realSource{}, generatedSource{};
    uint32_t generatedCount{};
    Duration realAt{};
    std::array<Duration, maxPositions> generatedAt{};

    /// One finished marker, in present order. Returns true when it completed an accepted sample.
    constexpr bool observe(const bool generated, const uint64_t source, const Duration at) noexcept {
        if (generated) {
            if (generatedCount == 0 || generatedSource != source) {
                generatedSource = source;
                generatedCount = 0;
            }
            // A marker past maxPositions is counted but not stored, and such a source gives no sample.
            if (generatedCount < maxPositions) generatedAt[generatedCount] = at;
            ++generatedCount;
            return false;
        }
        bool used = false;
        if (haveReal && generatedCount != 0 && generatedCount == expectedGenerated &&
                generatedCount <= maxPositions && generatedSource == source &&
                realSource + 1 == source && realAt < generatedAt[0] &&
                generatedAt[generatedCount - 1] < at) {
            const auto step = at - realAt;
            if (step >= minStep && step <= maxStep) {
                bool integrated{};
                for (uint32_t k = 0; k < generatedCount; ++k) {
                    const auto ideal = realAt + step * static_cast<int64_t>(k + 1) /
                        static_cast<int64_t>(generatedCount + 1);
                    const auto error = generatedAt[k] - ideal;
                    if (error > errorReject || error < -errorReject) continue;
                    if (k == 0) lastError = error;
                    if ((limited[k] < 0 && error > Duration{}) || (limited[k] > 0 && error < Duration{})) {
                        ++held;
                    } else {
                        offset[k] = std::clamp(offset[k] - std::clamp(error, -errorClamp, errorClamp) / gain,
                            -limit, limit);
                        integrated = true;
                    }
                }
                if (integrated) {
                    ++accepted;
                    used = true;
                }
            }
        }
        haveReal = true;
        realSource = source;
        realAt = at;
        generatedCount = 0;
        return used;
    }
    /// The worker's generated count for the batch. A different count changes what each position
    /// measures, so the offsets and holds start again; the same count keeps them.
    constexpr void setExpected(const uint32_t count) noexcept {
        if (count == expectedGenerated) return;
        expectedGenerated = count;
        offset = {};
        limited = {};
    }
    /// The real offset that moves the mean of the generated outputs by the same amount: the
    /// summary's `real_offset_us`. Exact at 2x.
    [[nodiscard]] constexpr Duration equivalentRealOffset() const noexcept {
        if (expectedGenerated == 0) return Duration{};
        Duration sum{};
        for (uint32_t k = 0; k < expectedGenerated && k < maxPositions; ++k) sum += offset[k];
        return -sum / static_cast<int64_t>(expectedGenerated);
    }
    /// Drops the pending markers; the offsets and `limited` are kept.
    constexpr void forget() noexcept { haveReal = false; generatedCount = 0; }
};

}

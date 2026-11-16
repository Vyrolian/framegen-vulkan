/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace fgvk::layer {

// Phase-locked start of a held batch (batch_hold; released and detached ready batches, low_latency on or off).
//
// A held batch starts at its own readiness plus a margin, but its start is not taken
// from one readiness alone. The lock follows the batch cadence: the expected start is the
// previous start plus the measured batch period, and the start moves a quarter of the way from
// that expectation to the readiness target. The period follows the readiness spacing by a
// sixteenth of each difference, so one jittery readiness moves it little. A target more than one
// period from the expectation (a rate change or a whole-period hitch) relocks the phase at once.
//
// The start never comes before the batch's readiness. The caller resets the lock when its
// inputs stop being consecutive: an idle gap, a readiness miss, a batch that takes no hold, or a
// change between just-in-time and scheduled batches (BH2). A detached ready batch is just in time when
// low_latency or batch_hold asks (BH3); a scheduled batch is one on the cap grid, the cap-scheduled path.
// forceStart() records a start the caller moved later, such as the quarter-interval guard or a later
// schedule.
class BatchPhaseLock {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;
    using TimePoint = Clock::time_point;

    static constexpr Duration margin = std::chrono::milliseconds{2};
    static constexpr int64_t phaseGain = 4;
    static constexpr int64_t periodGain = 16;

    // The start of a batch that became ready at readyAt. nominalPeriod is the batch period (the
    // output interval times the batch's output count). Zero or less means no period to lock to:
    // the lock resets and the batch starts at readyAt.
    [[nodiscard]] constexpr TimePoint start(const TimePoint readyAt, const Duration nominalPeriod) noexcept {
        if (nominalPeriod <= Duration::zero()) {
            reset();
            return readyAt;
        }
        if (previousReady) {
            const Duration gap = readyAt - *previousReady;
            if (gap > Duration::zero() && gap < 2 * period)
                period += (gap - period) / periodGain;
        }
        if (period <= Duration::zero() || period < nominalPeriod / 2 || period > 2 * nominalPeriod)
            period = nominalPeriod;
        const TimePoint target = readyAt + margin;
        TimePoint at = target;
        if (previousStart) {
            const TimePoint expected = *previousStart + period;
            const Duration error = target > expected ? target - expected : expected - target;
            if (error <= period) at = expected + (target - expected) / phaseGain;
        }
        at = std::max(at, readyAt);
        previousStart = at;
        previousReady = readyAt;
        return at;
    }

    // Records a start that the caller moved later, so the next expectation follows it.
    constexpr void forceStart(const TimePoint at) noexcept { previousStart = at; }
    constexpr void reset() noexcept { *this = BatchPhaseLock{}; }

private:
    std::optional<TimePoint> previousStart;
    std::optional<TimePoint> previousReady;
    Duration period{};
};

}

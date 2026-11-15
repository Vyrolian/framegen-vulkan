/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace fgvk::layer {

// Just-in-time release (the low_latency profile setting). A game without a frame
// limit may start its next frame as soon as its previous release point arrives,
// so a three-frame queue starts frames about two cap intervals before they are
// needed. Every release point of a capped swapchain is moved later by delay().
//
// lead = limiterDeadline - limiterArrival of a source present at the cap. The
// delay moves by an eighth of (lead - margin) in both directions, so it settles
// where presents arrive one margin early; a larger gain oscillates on a
// three-frame queue, whose answer to a change comes three frames later. The
// margin is 1 ms plus three times the mean frame-to-frame change of the lead, so
// a jittery game keeps more slack and rarely arrives late. The delay stays within
// [0, 3 cap intervals]. Without a cap, with frame generation off, or while the
// application's Reflex limiter holds the game (lowLatencyPlan), the step resets it to zero.
class LowLatencyRelease {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;

    static constexpr Duration baseMargin = std::chrono::milliseconds{1};

    // One present. While `active`, observes the lead and returns the delay of this present's
    // release point. Otherwise resets and returns zero.
    constexpr Duration update(bool active, Duration lead, Duration capInterval) noexcept {
        if (!active) {
            reset();
            return Duration::zero();
        }
        observe(lead, capInterval);
        return extraDelay;
    }
    constexpr void observe(Duration lead, Duration capInterval) noexcept {
        if (capInterval <= Duration::zero()) {
            reset();
            return;
        }
        if (hasPrevious) {
            const Duration change = lead > previousLead ? lead - previousLead : previousLead - lead;
            leadChange += (std::min(change, capInterval) - leadChange) / 16;
        }
        previousLead = lead;
        hasPrevious = true;
        extraDelay += (lead - margin()) / 8;
        extraDelay = std::clamp(extraDelay, Duration::zero(), 3 * capInterval);
    }
    [[nodiscard]] constexpr Duration margin() const noexcept { return baseMargin + 3 * leadChange; }
    constexpr void reset() noexcept { *this = LowLatencyRelease{}; }
    [[nodiscard]] constexpr Duration delay() const noexcept { return extraDelay; }

private:
    Duration extraDelay{};
    Duration previousLead{};
    Duration leadChange{};
    bool hasPrevious{false};
};

// GPU release (L5): where the game's next frame is released when the GPU is the limit, Reflex-like.
// Off, a token's release point is only its deadline (plus the L1 delay). Predicted: the backend's
// batch ends time the release (GpuReleasePredictor). Fence: the backend has no split timestamps, so
// a stepping hold runs on a non-blocking fence status query (GpuSteppingHold).
enum class GpuReleaseMode : uint8_t { Off, Predicted, Fence };

// Predicted release of the game's next frame (L5). The backend reports a batch's GPU end once that
// batch's fence has signalled, so the newest known end is a few batches old. Model: the batch of a
// source starts when the GPU frees the previous batch, so the source should arrive one GPU
// deviation before that end: the queue then stays one deviation deep, instead of the game running
// ahead of the GPU by a full three-frame queue. Only saturated frames (the previous batch ended after
// the frame arrived) measure the batch period, so the predictor never measures its own hold.
//
// Estimates, all with steady_clock durations:
//   period      EMA 1/8 of consecutive batch-end differences of saturated frames, within [1 ms, 3 cap intervals].
//   gpuDev      EMA 1/16 of |interval - period| on the same frames: the queue target.
//   cpu         EMA 1/8 of release -> arrival; cpuDev its EMA 1/16 deviation.
//   bias        EMA 1/8 of (previous batch end - arrival) toward gpuDev, within +-3 cap intervals.
// release(source) = previousFree - cpu - 2 cpuDev + bias, where previousFree is the predicted end of
// the batch before `source`. Nothing is predicted before the first batch end or when the newest end
// is more than maxLag sources old.
class GpuReleasePredictor {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;
    using TimePoint = Clock::time_point;

    static constexpr uint64_t maxLag = 4;
    static constexpr size_t arrivalCapacity = 8;
    static constexpr Duration minimumPeriod = std::chrono::milliseconds{1};

    constexpr GpuReleasePredictor() noexcept = default;
    explicit constexpr GpuReleasePredictor(Duration initialPeriod) noexcept : period(initialPeriod) {}

    constexpr void reset(Duration initialPeriod) noexcept { *this = GpuReleasePredictor{initialPeriod}; }

    // Batch `source` finished on the GPU at `end`. Its frame updates the period when that frame
    // arrived before the previous batch ended (saturated). The arrival of the next source, if
    // already observed, resolves its bias sample here. Returns that sample (the previous batch end
    // minus that arrival), or nothing.
    constexpr std::optional<Duration> observeBatchEnd(const uint64_t source, const TimePoint end,
            const Duration frame) noexcept {
        if (frame <= Duration::zero() || (hasEnd && source <= lastSource)) return std::nullopt;
        if (hasEnd) {
            // A gap of skipped batches averages the interval; saturation of the newest frame then
            // implies saturation of the gap, because arrivals are monotonic.
            const auto gap = static_cast<int64_t>(source - lastSource);
            const Duration interval = clampTo((end - lastEnd) / gap, minimumPeriod, 3 * frame);
            const Arrival* arrival = find(source);
            if (arrival && arrival->arrived <= lastEnd) {
                const Duration deviation = interval > period ? interval - period : period - interval;
                gpuDeviation += (deviation - gpuDeviation) / 16;
                period = clampTo(period + (interval - period) / 8, minimumPeriod, 3 * frame);
            }
        }
        lastSource = source;
        lastEnd = end;
        hasEnd = true;
        if (const Arrival* next = find(source + 1)) return resolve(next->arrived, frame);
        return std::nullopt;
    }

    // Source `source` was released at `released` (its wait returned) and arrived at `arrived`.
    // Updates the CPU estimate. Its bias sample resolves here when the previous batch end is
    // already known, and otherwise when that end is observed. Returns the sample, or nothing.
    constexpr std::optional<Duration> observeArrival(const uint64_t source, const TimePoint released,
            const TimePoint arrived, const Duration frame) noexcept {
        if (frame <= Duration::zero()) return std::nullopt;
        const Duration sample = arrived - released;
        const Duration deviation = sample > cpu ? sample - cpu : cpu - sample;
        cpuDeviation += (deviation - cpuDeviation) / 16;
        cpu += (sample - cpu) / 8;
        record(source, arrived);
        if (hasEnd && source == lastSource + 1) return resolve(arrived, frame);
        return std::nullopt;
    }

    // The predicted release of source `source`, or nothing (see the class comment).
    [[nodiscard]] constexpr std::optional<TimePoint> release(const uint64_t source) const noexcept {
        if (!hasEnd || lastSource + maxLag < source) return std::nullopt;
        const TimePoint previousFree = lastSource + 1 < source
            ? lastEnd + static_cast<int64_t>(source - 1 - lastSource) * period
            : lastEnd;
        return previousFree - cpu - 2 * cpuDeviation + bias;
    }

    [[nodiscard]] constexpr Duration periodEstimate() const noexcept { return period; }
    [[nodiscard]] constexpr Duration gpuDeviationEstimate() const noexcept { return gpuDeviation; }
    [[nodiscard]] constexpr Duration cpuEstimate() const noexcept { return cpu; }
    [[nodiscard]] constexpr Duration biasEstimate() const noexcept { return bias; }

private:
    struct Arrival {
        uint64_t source{};
        TimePoint arrived{};
        bool valid{};
    };

    static constexpr Duration clampTo(const Duration value, const Duration low, const Duration high) noexcept {
        return std::min(std::max(value, low), high);
    }
    // The error of the source whose previous batch end is `lastEnd`, which the bias moves toward gpuDev.
    constexpr Duration resolve(const TimePoint arrived, const Duration frame) noexcept {
        const Duration error = lastEnd - arrived;
        bias += (error - gpuDeviation) / 8;
        bias = clampTo(bias, -3 * frame, 3 * frame);
        return error;
    }
    constexpr void record(const uint64_t source, const TimePoint arrived) noexcept {
        for (auto& entry : arrivals) {
            if (entry.valid && entry.source == source) entry.valid = false;
        }
        arrivals[nextArrival] = Arrival{source, arrived, true};
        nextArrival = (nextArrival + 1) % arrivalCapacity;
    }
    [[nodiscard]] constexpr const Arrival* find(const uint64_t source) const noexcept {
        for (const auto& entry : arrivals) {
            if (entry.valid && entry.source == source) return &entry;
        }
        return nullptr;
    }

    Duration period{};
    Duration gpuDeviation{};
    Duration cpu{};
    Duration cpuDeviation{};
    Duration bias{};
    TimePoint lastEnd{};
    uint64_t lastSource{};
    bool hasEnd{false};
    std::array<Arrival, arrivalCapacity> arrivals{};
    size_t nextArrival{};
};

// Fallback GPU release without split timestamps (L5). Each present observes whether the previous
// batch still runs on the GPU (one non-blocking fence status query, no wait) and moves a hold on
// the release of every token. A busy observation adds a step that doubles while the busy run
// lasts, at most an eighth of a cap interval; an idle one takes a fixed amount off and restarts
// the step. The hold stays within [0, 3 cap intervals]. The hold is measured from the satisfying
// wait's return, so the release is the return plus hold. Without a cap, the hold resets to zero.
class GpuSteppingHold {
public:
    using Duration = std::chrono::steady_clock::duration;

    static constexpr Duration firstStep = std::chrono::microseconds{100};
    static constexpr Duration idleRelease = std::chrono::microseconds{500};

    // One present. busy: the previous batch has not finished on the GPU.
    constexpr void observe(bool busy, Duration capInterval) noexcept {
        if (capInterval <= Duration::zero()) {
            reset();
            return;
        }
        if (busy) {
            step = previousBusy ? std::min(2 * step, capInterval / 8) : firstStep;
            held += step;
        } else {
            held -= idleRelease;
            step = Duration::zero();
        }
        held = std::clamp(held, Duration::zero(), 3 * capInterval);
        previousBusy = busy;
    }
    [[nodiscard]] constexpr Duration hold() const noexcept { return held; }
    constexpr void reset() noexcept { *this = GpuSteppingHold{}; }

private:
    Duration held{};
    Duration step{};
    bool previousBusy{false};
};

// The parts of low_latency that one present uses. The setting is on for a capped
// swapchain with frame generation. `justInTime` starts each released output batch at
// its own readiness (L2, L3: batch_phase.hpp and the worker). `releaseDelay` runs the
// controller above with its holds: the release point moves by the delay, the DXVK
// token is recorded and the native acquire fallback holds. The GPU release (L5,
// GpuReleasePredictor or GpuSteppingHold) runs with `releaseDelay` too. While the
// application's own Reflex limiter holds the game (its last sleep mode has lowLatencyMode on,
// reflex_frame_limit.hpp), `releaseDelay` is off and the controller and the GPU release reset,
// and `justInTime` stays on. `reflexHolds` marks a present where Reflex is the reason.
struct LowLatencyPlan {
    bool justInTime{};
    bool releaseDelay{};
    bool reflexHolds{};
};

[[nodiscard]] constexpr LowLatencyPlan lowLatencyPlan(const bool setting, const bool capped,
        const bool frameGeneration, const bool reflexLowLatency) noexcept {
    const bool on = setting && capped && frameGeneration;
    return {
        .justInTime = on,
        .releaseDelay = on && !reflexLowLatency,
        .reflexHolds = on && reflexLowLatency,
    };
}

// Release-token kind bit of the pacing log (low_latency_hold=reflex): a present whose release
// delay Reflex stepped aside. Bits 0 to 3 are ReleaseGate::kindBit and the acquire bit.
inline constexpr uint32_t reflexKindBit = uint32_t{1} << 4;

// Native acquire fallback: how long an acquire waits for the release point of the
// swapchain's last capped present. `wait` is the time to sleep. `expired` means the
// point lies past the caller's timeout: the acquire then reports VK_TIMEOUT after
// sleeping `wait` (the timeout). A zero timeout is a poll and is never held.
struct AcquireHold {
    LowLatencyRelease::Duration wait{};
    bool expired{};
};

[[nodiscard]] constexpr AcquireHold acquireHold(LowLatencyRelease::Clock::time_point now,
        LowLatencyRelease::Clock::time_point release, uint64_t timeout) noexcept {
    using Duration = LowLatencyRelease::Duration;
    if (timeout == 0 || release <= now) return {};
    const Duration remaining = release - now;
    if (timeout == UINT64_MAX) return {remaining, false};
    // Timeouts of an hour or more are not bounded here, as in the release waits.
    const Duration limit = timeout >= uint64_t{3'600'000'000'000}
        ? Duration::max() : std::chrono::nanoseconds{static_cast<int64_t>(timeout)};
    return {std::min(remaining, limit), remaining > limit};
}

}

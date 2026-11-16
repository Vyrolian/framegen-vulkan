/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

// GT1: gamescope tick lock. Under gamescope an output reaches the screen at one of gamescope's ticks, a grid of one
// refresh period P. The worker starts each output a lead D before the tick it targets, so that every output starts a
// safe distance before its latch and consecutive outputs never share a tick. This header holds the pure rules: the
// grid's phase and period from the screen samples, the lead, and the tick choice. Times are steady-clock nanoseconds
// (int64_t). Nothing here touches Vulkan, a clock or a thread, so the rules are checked by static_asserts and tests.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace fgvk::layer {

// Integer division by a positive divisor b, rounding down, up, and to the nearest value (ties up).
constexpr int64_t floorDivide(const int64_t a, const int64_t b) noexcept {
    const int64_t quotient = a / b;
    return a % b < 0 ? quotient - 1 : quotient;
}
constexpr int64_t ceilDivide(const int64_t a, const int64_t b) noexcept {
    return -floorDivide(-a, b);
}
constexpr int64_t roundDivide(const int64_t a, const int64_t b) noexcept {
    return floorDivide(2 * a + b, 2 * b);
}
// x taken to (-period/2, period/2], for a positive period: the distance of a time from the nearest tick.
constexpr int64_t wrapResidual(const int64_t x, const int64_t period) noexcept {
    int64_t residual = x - period * floorDivide(x, period);
    if (2 * residual > period) residual -= period;
    return residual;
}
constexpr int64_t magnitude(const int64_t value) noexcept {
    return value < 0 ? -value : value;
}

// The refresh period in nanoseconds for a rate in hertz; zero when the rate is unknown.
[[nodiscard]] constexpr int64_t refreshPeriodNs(const double hz) noexcept {
    return hz > 0.0 ? static_cast<int64_t>(1e9 / hz + 0.5) : 0;
}

// What a snapped output's screen sample says about its lead (rule 3). Unlocked is any sample of an output that was
// not snapped, or no snapped output at all.
enum class TickVerdict : uint8_t { unlocked, hit, miss, early };

// One screen sample: when the output reached the screen, when its lower present started, and its target tick
// (0 when it was not snapped).
struct TickSample {
    int64_t screenNs{};
    int64_t startedNs{};
    int64_t targetNs{};
};

// One snapped output: its target tick and its deadline (target minus the lead).
struct TickSlot {
    int64_t target{};
    int64_t deadline{};
};

// GT1 rules 2 to 4 and 6, one state per lower swapchain. Worker thread only.
class TickLock {
public:
    // Rule 2: the period stays within 1/50 (2 %) of the refresh period. The phase and period gains are rule 2's.
    static constexpr int64_t periodTolerance = 50;
    static constexpr int64_t phaseGain = 16;
    static constexpr int64_t periodGain = 256;
    // Rule 2: validity needs this many good samples in one window, and a window is one second of screen time.
    static constexpr size_t validSamples = 64;
    static constexpr int64_t windowNs = 1'000'000'000;
    // Rule 3: the first lead comes from this many unlocked samples; the lead step and the late bound are rule 3's.
    static constexpr size_t leadSamples = 128;
    static constexpr int64_t leadStepNs = 250'000;
    static constexpr int64_t lateNs = 250'000;
    // Snapped outputs whose screen sample may still arrive. The watcher reports them in order, so a few are enough.
    static constexpr size_t slotCount = 16;

    // A refresh period that differs from the grid's starts everything again (rule 6). The same period changes nothing.
    constexpr void setRefresh(const int64_t refreshNs) noexcept {
        if (refreshNs == refresh) return;
        *this = TickLock{};
        refresh = refreshNs;
        period = refreshNs;
    }

    // Rule 6: the previous output's tick is forgotten. The grid, its validity and the lead are kept.
    constexpr void resetOutputs() noexcept {
        previousTarget = 0;
    }

    // Rules 2 and 3: one screen sample of this context, in present order. Returns what a snapped output's sample
    // says about its lead, and unlocked for every other sample.
    constexpr TickVerdict observe(const TickSample& sample) noexcept {
        if (period <= 0) return TickVerdict::unlocked;
        const int64_t residual = followGrid(sample.screenNs);
        judgeWindow(sample.screenNs, residual);
        if (sample.targetNs == 0) {
            if (valid && !leadKnown) collectLead(sample);
            return TickVerdict::unlocked;
        }
        return judgeLead(sample);
    }

    // Rule 4: the slot of an output whose unsnapped deadline is `deadline`. `floorNs` is its earliest allowed deadline,
    // or 0 for none. Only while snapping(). The slot is kept for the output's screen sample (see judgeLead).
    constexpr TickSlot snap(const int64_t deadline, const int64_t floorNs) noexcept {
        int64_t target = phase + period * roundDivide(deadline + lead - phase, period);
        // No shared tick: a tick later than the previous output's.
        if (previousTarget != 0 && target <= previousTarget) target = previousTarget + period;
        int64_t slotDeadline = target - lead;
        // Not before the floor: the next tick that is late enough.
        if (floorNs != 0 && slotDeadline < floorNs) {
            target += period * ceilDivide(floorNs - slotDeadline, period);
            slotDeadline = target - lead;
        }
        previousTarget = target;
        inFlight[inFlightNext] = TickSlot{target, slotDeadline};
        inFlightNext = (inFlightNext + 1) % slotCount;
        return TickSlot{target, slotDeadline};
    }

    // GT1 is on: the grid is valid and the lead is known. Only then does the worker snap outputs.
    [[nodiscard]] constexpr bool snapping() const noexcept {
        return period > 0 && valid && leadKnown;
    }
    [[nodiscard]] constexpr bool gridValid() const noexcept { return valid; }
    [[nodiscard]] constexpr int64_t currentPeriod() const noexcept { return period; }
    [[nodiscard]] constexpr int64_t currentPhase() const noexcept { return phase; }
    [[nodiscard]] constexpr int64_t currentLead() const noexcept { return lead; }

private:
    // Rule 2: the reference tick g takes each sample's own tick (the sample less its residual r, a tick of the same
    // grid), then moves 1/16 of r toward the sample. The period moves by r/256, clamped to the band. Returns r (zero
    // for the first sample). The reference follows the samples in absolute time: a reference left behind by whole
    // periods would make the period term's sensitivity grow with the number of periods, which is unstable.
    constexpr int64_t followGrid(const int64_t screenNs) noexcept {
        if (!gridStarted) {
            gridStarted = true;
            phase = screenNs;
            return 0;
        }
        const int64_t residual = wrapResidual(screenNs - phase, period);
        phase = screenNs - residual + residual / phaseGain;
        const int64_t band = refresh / periodTolerance;
        period = std::clamp(period + residual / periodGain, refresh - band, refresh + band);
        return residual;
    }

    // Rule 2: the grid is valid after validSamples samples within |r| < P/4 in one window. A closed window with more
    // than a quarter of its samples at |r| >= P/4 (irregular ticks, VRR) makes it invalid. Validity is granted only
    // while the window so far has no more than a quarter bad: a stream half off-grid would otherwise re-validate
    // within the same window, since 64 good samples come quickly at a 4 ms cadence.
    constexpr void judgeWindow(const int64_t screenNs, const int64_t residual) noexcept {
        if (!windowOpen || screenNs - windowStart >= windowNs) {
            if (windowOpen && valid && 4 * bad > total) valid = false;
            windowOpen = true;
            windowStart = screenNs;
            total = 0;
            good = 0;
            bad = 0;
        }
        ++total;
        if (4 * magnitude(residual) < period) ++good;
        else ++bad;
        if (good >= validSamples && 4 * bad <= total) valid = true;
    }

    // Rule 3, first estimate: the second percentile of start to screen over leadSamples unlocked samples, plus P/2.
    constexpr void collectLead(const TickSample& sample) noexcept {
        estimate[estimateCount++] = sample.screenNs - sample.startedNs;
        if (estimateCount < leadSamples) return;
        // Insertion sort: constexpr, and it runs once per refresh period.
        for (size_t i = 1; i < leadSamples; ++i)
            for (size_t j = i; j > 0 && estimate[j - 1] > estimate[j]; --j) {
                const int64_t swapped = estimate[j - 1];
                estimate[j - 1] = estimate[j];
                estimate[j] = swapped;
            }
        const int64_t p2 = estimate[leadSamples * 2 / 100];
        lead = std::clamp(p2 + period / 2, leadStepNs, 2 * period);
        leadKnown = true;
    }

    // Rule 3, afterwards: k = round((screen - target) / P). A miss (k >= 1) moves the lead up, an early catch (k <= -1)
    // moves it down, by 250 us, unless the output started more than 250 us after its own slot's deadline.
    constexpr TickVerdict judgeLead(const TickSample& sample) noexcept {
        const int64_t k = roundDivide(sample.screenNs - sample.targetNs, period);
        const TickVerdict verdict = k >= 1 ? TickVerdict::miss : k <= -1 ? TickVerdict::early : TickVerdict::hit;
        for (size_t back = 0; back < slotCount; ++back) {
            TickSlot& slot = inFlight[(inFlightNext + slotCount - 1 - back) % slotCount];
            if (slot.target != sample.targetNs) continue;
            const bool late = sample.startedNs - slot.deadline > lateNs;
            slot.target = 0;
            if (leadKnown && verdict != TickVerdict::hit && !late) {
                const int64_t step = verdict == TickVerdict::miss ? leadStepNs : -leadStepNs;
                lead = std::clamp(lead + step, leadStepNs, 2 * period);
            }
            break;
        }
        return verdict;
    }

    int64_t refresh{};
    int64_t period{};
    int64_t phase{};
    bool gridStarted{};
    bool valid{};
    bool windowOpen{};
    int64_t windowStart{};
    size_t total{};
    size_t good{};
    size_t bad{};
    std::array<int64_t, leadSamples> estimate{};
    size_t estimateCount{};
    bool leadKnown{};
    int64_t lead{};
    int64_t previousTarget{};
    std::array<TickSlot, slotCount> inFlight{};
    size_t inFlightNext{};
};

// GT1 gate (rule 1): the environment, a gamescope session, the watcher as the screen source and the automatic real offset.
struct TickLockPlan {
    bool enabled{};
    const char* reason{"none"};
};
[[nodiscard]] constexpr TickLockPlan tickLockPlan(const bool environmentOn, const bool gamescopeSession,
        const bool watcherSource, const bool realOffset) noexcept {
    if (!environmentOn) return {false, "env-off"};
    if (!gamescopeSession) return {false, "not-gamescope"};
    if (!watcherSource) return {false, "not-watcher"};
    if (!realOffset) return {false, "real-offset-off"};
    return {true, "active"};
}

// FGVK_GAMESCOPE_TICK_LOCK=0 turns the tick lock off; any other value leaves it on. Read once.
[[nodiscard]] inline bool gamescopeTickLockEnvironment() noexcept {
    static const bool enabled = [] {
        const char* text = std::getenv("FGVK_GAMESCOPE_TICK_LOCK");
        return !(text && std::strcmp(text, "0") == 0);
    }();
    return enabled;
}

// GT1, diagnostics only: the last worker's tick state, and the misses, early catches and snapped outputs since the
// previous summary.
inline std::atomic<bool> tickLockOn{false};
inline std::atomic<int64_t> tickLockPeriodNs{0};
inline std::atomic<int64_t> tickLockLeadNs{0};
inline std::atomic<uint64_t> tickLockMisses{0};
inline std::atomic<uint64_t> tickLockEarly{0};
inline std::atomic<uint64_t> tickLockSnapped{0};

} // namespace fgvk::layer

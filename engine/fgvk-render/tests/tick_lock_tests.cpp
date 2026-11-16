/* SPDX-License-Identifier: GPL-3.0-or-later */
// GT1: the gamescope tick grid, the lead and the tick choice, checked with static_asserts. The simulations are
// constexpr: a synthetic 240 Hz grid, a simulated latch at tick - 0.6 ms with +-0.3 ms jitter, and VRR-like streams.
#include "swapchain/tick_lock.hpp"

#include <cstdio>
#include <string_view>

using namespace fgvk::layer;

namespace {
constexpr int64_t us(const int64_t value) { return value * 1000; }
constexpr int64_t ms(const int64_t value) { return value * 1'000'000; }

constexpr int64_t period240 = 4'166'667;       // 240 Hz, the refresh period the cap uses
constexpr int64_t gridBase = 1'000'000'000'000; // the true ticks are gridBase + n * period240

// A deterministic pseudo-random value in [0, span) for index k.
constexpr int64_t spread(const int64_t k, const int64_t span) {
    return static_cast<int64_t>((static_cast<uint64_t>(k) * 2654435761ull + 12345ull) %
        static_cast<uint64_t>(span));
}

// The simulated latch: tick n latches 0.6 ms before the tick, moved by +-0.3 ms per tick. An output whose frame is
// ready at readyNs reaches the screen on the first tick whose latch is not before readyNs.
constexpr int64_t latchJitter(const int64_t n) {
    return (spread(n, 601) - 300) * 1000;
}
constexpr int64_t screenTick(const int64_t readyNs) {
    int64_t n = ceilDivide(readyNs + us(600) - gridBase, period240) - 1;
    while (gridBase + n * period240 - us(600) + latchJitter(n) < readyNs) ++n;
    return gridBase + n * period240;
}

// Rules 2 and 3, first estimate: unlocked outputs at irregular starts, fed until the lead is known.
constexpr TickLock learnedLock() {
    TickLock lock;
    lock.setRefresh(period240);
    for (int64_t k = 0; k < 2000 && !lock.snapping(); ++k) {
        const int64_t started = gridBase + ms(10) + k * 4'123'457 + spread(k, us(1000));
        lock.observe(TickSample{screenTick(started), started, 0});
    }
    return lock;
}

// The true tick of a deadline's nearest slot, for a grid whose ticks are gridBase + n * period240.
constexpr int64_t nearestTick(const int64_t time, const int64_t lead) {
    return gridBase + roundDivide(time + lead - gridBase, period240) * period240;
}
}

// ---- The arithmetic and the gate. ----
static_assert(floorDivide(7, 2) == 3 && floorDivide(-7, 2) == -4 && floorDivide(-8, 2) == -4);
static_assert(ceilDivide(7, 2) == 4 && ceilDivide(-7, 2) == -3 && ceilDivide(8, 2) == 4);
static_assert(roundDivide(5, 4) == 1 && roundDivide(6, 4) == 2 && roundDivide(-7, 2) == -3);
static_assert(wrapResidual(2, 4) == 2 && wrapResidual(3, 4) == -1 && wrapResidual(-1, 4) == -1);
static_assert(wrapResidual(5, 4) == 1 && wrapResidual(-3, 4) == 1 && wrapResidual(0, 4) == 0);
static_assert(refreshPeriodNs(240.0) == 4'166'667 && refreshPeriodNs(0.0) == 0);
static_assert(refreshPeriodNs(-5.0) == 0);

static_assert(tickLockPlan(true, true, true, true).enabled);
static_assert(std::string_view(tickLockPlan(true, true, true, true).reason) == "active");
static_assert(!tickLockPlan(true, false, true, true).enabled &&
    std::string_view(tickLockPlan(true, false, true, true).reason) == "not-gamescope");
static_assert(!tickLockPlan(true, true, false, true).enabled &&
    std::string_view(tickLockPlan(true, true, false, true).reason) == "not-watcher");
static_assert(!tickLockPlan(true, true, true, false).enabled &&
    std::string_view(tickLockPlan(true, true, true, false).reason) == "real-offset-off");
static_assert(!tickLockPlan(false, true, true, true).enabled &&
    std::string_view(tickLockPlan(false, true, true, true).reason) == "env-off");

// ---- Rule 2: the grid locks, and rejects irregular ticks. ----

// A 240 Hz sample stream with 200 us jitter: from the 100th sample on, the reference tick stays within 50 us of each
// sample's own true tick, and the grid is valid with its period within 5 us of the true one.
constexpr bool gridLocksToJitteredStream() {
    TickLock lock;
    lock.setRefresh(period240);
    int64_t worst = 0;
    for (int64_t k = 0; k < 400; ++k) {
        const int64_t truth = gridBase + k * period240;
        lock.observe(TickSample{truth + us(spread(k, 401) - 200), 0, 0});
        if (k >= 100)
            worst = std::max(worst, magnitude(wrapResidual(lock.currentPhase() - truth, period240)));
    }
    return worst < us(50) && lock.gridValid() && magnitude(lock.currentPeriod() - period240) < us(5);
}
static_assert(gridLocksToJitteredStream());

// Samples 8 to 16 ms apart at random: about half land a quarter period or more from the grid. The grid is never valid.
constexpr bool irregularStreamIsRejected() {
    TickLock lock;
    lock.setRefresh(period240);
    int64_t at = gridBase;
    bool everValid = false;
    for (int64_t k = 0; k < 300; ++k) {
        at += ms(8) + ms(spread(k + 1000, 9));
        lock.observe(TickSample{at, 0, 0});
        everValid = everValid || lock.gridValid();
    }
    return !everValid && !lock.snapping();
}
static_assert(irregularStreamIsRejected());

// Dense irregular ticks: 4.35 ms apart on a cadence, each at a random offset in the period. About half are off-grid
// in every window, so the grid never becomes valid, however many good samples come by.
constexpr bool denseIrregularStreamIsRejected() {
    TickLock lock;
    lock.setRefresh(period240);
    bool everValid = false;
    for (int64_t k = 0; k < 1000; ++k) {
        const int64_t at = gridBase + k * 4'347'826 + spread(k + 3000, period240);
        lock.observe(TickSample{at, 0, 0});
        everValid = everValid || lock.gridValid();
    }
    return !everValid && !lock.snapping();
}
static_assert(denseIrregularStreamIsRejected());

// A valid grid turns invalid once a full second of irregular ticks has more than a quarter of its samples off-grid.
constexpr bool irregularTicksInvalidateAValidGrid() {
    TickLock lock;
    lock.setRefresh(period240);
    int64_t at = gridBase;
    for (int64_t k = 0; k < 300; ++k) {
        at += period240;
        lock.observe(TickSample{at, 0, 0});
    }
    const bool validBefore = lock.gridValid();
    for (int64_t k = 0; k < 400; ++k) {
        at += ms(8) + ms(spread(k + 2000, 9));
        lock.observe(TickSample{at, 0, 0});
    }
    return validBefore && !lock.gridValid();
}
static_assert(irregularTicksInvalidateAValidGrid());

// ---- Rule 3: the lead converges to the latch midpoint, and a miss or an early catch moves it the right way. ----

// The latch midpoint of the feasible lead window: the window [0.6 ms, 0.6 ms + P) for an output ready at its start.
constexpr int64_t latchMidpoint = us(600) + period240 / 2;

// The first estimate, then 300 snapped outputs at irregular requests, each started at its snapped deadline.
constexpr int64_t snappedLead() {
    TickLock lock = learnedLock();
    for (int64_t k = 0; k < 300; ++k) {
        const int64_t requested = gridBase + ms(2000) + k * 4'350'000 + spread(k, us(2000)) - us(1000);
        const TickSlot slot = lock.snap(requested, 0);
        lock.observe(TickSample{screenTick(slot.deadline), slot.deadline, slot.target});
    }
    return lock.currentLead();
}
// The first estimate is the second percentile of start to screen plus P/2; the jitter moves it at most 0.3 ms.
static_assert(snappedLead() > latchMidpoint - us(300) && snappedLead() < latchMidpoint + us(300));

// A miss (screen one tick late) raises the lead by 250 us; an early catch (one tick early) lowers it by 250 us.
constexpr bool missAndEarlyMoveTheLead() {
    TickLock lock = learnedLock();
    const int64_t before = lock.currentLead();
    const TickSlot late = lock.snap(gridBase + ms(3000), 0);
    const TickVerdict missed = lock.observe(TickSample{late.target + period240, late.deadline, late.target});
    const bool raised = missed == TickVerdict::miss && lock.currentLead() == before + us(250);
    const TickSlot early = lock.snap(gridBase + ms(3100), 0);
    const TickVerdict caught = lock.observe(TickSample{early.target - period240, early.deadline, early.target});
    const bool lowered = caught == TickVerdict::early && lock.currentLead() == before;
    return raised && lowered;
}
static_assert(missAndEarlyMoveTheLead());

// A hit moves nothing, and a sample whose output started more than 250 us after its deadline does not move the lead.
constexpr bool hitsAndLateStartsKeepTheLead() {
    TickLock lock = learnedLock();
    const int64_t before = lock.currentLead();
    const TickSlot hit = lock.snap(gridBase + ms(3000), 0);
    const bool isHit = lock.observe(TickSample{hit.target, hit.deadline, hit.target}) == TickVerdict::hit;
    const TickSlot late = lock.snap(gridBase + ms(3100), 0);
    const TickVerdict lateMiss = lock.observe(TickSample{late.target + period240,
        late.deadline + us(300), late.target});
    return isHit && lateMiss == TickVerdict::miss && lock.currentLead() == before;
}
static_assert(hitsAndLateStartsKeepTheLead());

// The lead's clamp: at least 250 us, at most 2 P.
static_assert(snappedLead() >= us(250) && snappedLead() <= 2 * period240);

// ---- Rule 4: the snap. ----

// The nearest tick to the request plus the lead, and a deadline no more than P/2 from the request.
constexpr bool snapsToTheNearestTick() {
    const TickLock base = learnedLock();
    const int64_t lead = base.currentLead();
    for (int64_t k = 0; k < 64; ++k) {
        TickLock lock = base;
        const int64_t requested = gridBase + ms(3000) + k * us(137);
        const TickSlot slot = lock.snap(requested, 0);
        if (slot.target != nearestTick(requested, lead)) return false;
        if (slot.deadline != slot.target - lead) return false;
        if (magnitude(slot.deadline - requested) > period240 / 2) return false;
    }
    return true;
}
static_assert(snapsToTheNearestTick());

// A deadline already on a slot does not move: the zero snap.
constexpr bool zeroSnapOnASlot() {
    TickLock lock = learnedLock();
    const int64_t lead = lock.currentLead();
    const int64_t onSlot = gridBase + 100 * period240 - lead;
    const TickSlot slot = lock.snap(onSlot, 0);
    return slot.deadline == onSlot && slot.target == gridBase + 100 * period240;
}
static_assert(zeroSnapOnASlot());

// No shared tick: a second output that would take the same tick gets the next one.
constexpr bool neverTheSameTickTwice() {
    TickLock lock = learnedLock();
    lock.resetOutputs();
    const int64_t lead = lock.currentLead();
    const int64_t requested = gridBase + 100 * period240 - lead;
    const TickSlot first = lock.snap(requested, 0);
    const TickSlot second = lock.snap(requested + us(50), 0);
    return second.target == first.target + period240 && second.deadline == first.deadline + period240;
}
static_assert(neverTheSameTickTwice());

// Rule 6: resetOutputs forgets the previous tick, so the same tick is allowed again.
constexpr bool resetAllowsTheSameTick() {
    TickLock lock = learnedLock();
    lock.resetOutputs();
    const int64_t lead = lock.currentLead();
    const int64_t requested = gridBase + 100 * period240 - lead;
    const TickSlot first = lock.snap(requested, 0);
    lock.resetOutputs();
    const TickSlot again = lock.snap(requested, 0);
    return again.target == first.target;
}
static_assert(resetAllowsTheSameTick());

// The readiness floor (the first output's readyAt) and the P/2 floor (a later output's previous start plus P/2): a
// floor the slot meets keeps it, and a later floor takes the next tick.
constexpr bool floorsKeepOrMoveTheTick() {
    TickLock lock = learnedLock();
    lock.resetOutputs();
    const int64_t lead = lock.currentLead();
    const int64_t onSlot = gridBase + 100 * period240 - lead;

    const TickSlot kept = lock.snap(onSlot, onSlot - us(10));
    if (kept.target != gridBase + 100 * period240 || kept.deadline != onSlot) return false;

    lock.resetOutputs();
    const TickSlot next = lock.snap(onSlot, onSlot + us(10));
    if (next.target != gridBase + 101 * period240 || next.deadline != onSlot + period240) return false;

    lock.resetOutputs();
    const TickSlot far = lock.snap(onSlot, onSlot + 2 * period240 + us(10));
    return far.deadline >= onSlot + 2 * period240 + us(10) &&
        far.deadline - period240 < onSlot + 2 * period240 + us(10) && far.target == far.deadline + lead;
}
static_assert(floorsKeepOrMoveTheTick());

// A refresh period that changes starts everything again; the same period changes nothing.
constexpr bool refreshChangeStartsAgain() {
    TickLock lock = learnedLock();
    lock.setRefresh(period240);
    if (!lock.snapping()) return false;
    lock.setRefresh(period240 + us(1000));
    return !lock.snapping() && lock.currentPeriod() == period240 + us(1000) && lock.currentLead() == 0;
}
static_assert(refreshChangeStartsAgain());

// With no refresh the grid never starts: no sample moves it and nothing snaps.
constexpr bool noRefreshNeverSnaps() {
    TickLock lock;
    for (int64_t k = 0; k < 300; ++k) lock.observe(TickSample{gridBase + k * period240, 0, 0});
    return !lock.snapping() && lock.currentPeriod() == 0;
}
static_assert(noRefreshNeverSnaps());

int main() {
    std::puts("framegen-vulkan: tick lock tests passed (static assertions)");
    return 0;
}

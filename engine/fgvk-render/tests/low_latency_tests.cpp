/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/low_latency.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace fgvk::layer;
using namespace std::chrono_literals;

namespace {
void expect(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

using Clock = LowLatencyRelease::Clock;
using Duration = LowLatencyRelease::Duration;

void steadyLeadMovesDelayByAnEighthOfTheExcess() {
    LowLatencyRelease release;
    release.observe(9ms, 20ms);
    expect(release.margin() == 1ms, "the first observation changed the margin");
    expect(release.delay() == 1ms, "a 9 ms lead did not raise the delay by an eighth of 8 ms");
}

void earlyThenLateMovesDelayBothWays() {
    LowLatencyRelease release;
    for (int i = 0; i < 20; ++i) release.observe(17ms, 100ms);
    const auto raised = release.delay();
    expect(raised > Duration::zero(), "an early present did not raise the delay");
    release.observe(-5ms, 100ms);
    expect(release.delay() < raised, "a late present did not lower the delay");
}

void jitterWidensMargin() {
    LowLatencyRelease release;
    for (int i = 0; i < 200; ++i) release.observe(i % 2 ? 4ms : 8ms, 20ms);
    expect(release.margin() > 9ms && release.margin() <= 13ms, "a lead alternating by 4 ms did not widen the margin");
}

void delayStaysWithinThreeCapIntervals() {
    LowLatencyRelease release;
    for (int i = 0; i < 1000; ++i) release.observe(1s, 10ms);
    expect(release.delay() == 30ms, "the delay exceeded three cap intervals");
    for (int i = 0; i < 1000; ++i) release.observe(-1s, 10ms);
    expect(release.delay() == Duration::zero(), "repeated late presents took the delay below zero");
}

void noCapResetsDelay() {
    LowLatencyRelease release;
    release.observe(20ms, 10ms);
    expect(release.delay() > Duration::zero(), "setup did not raise the delay");
    release.observe(20ms, Duration::zero());
    expect(release.delay() == Duration::zero(), "a zero cap interval did not reset the delay");
    release.observe(20ms, 10ms);
    release.reset();
    expect(release.delay() == Duration::zero(), "reset did not clear the delay");
}

// The setting's parts for one present. The release delay steps aside while Reflex holds the game, and
// the just-in-time batch start stays on.
void lowLatencyPlanFollowsSettingCapFrameGenerationAndReflex() {
    const auto on = lowLatencyPlan(true, true, true, false);
    expect(on.justInTime && on.releaseDelay && !on.reflexHolds,
        "the setting on, Reflex off, did not run every part");
    expect(!lowLatencyPlan(false, true, true, false).justInTime, "the setting off left low latency on");
    expect(!lowLatencyPlan(false, true, true, true).reflexHolds, "the setting off reported Reflex holding the game");
    expect(!lowLatencyPlan(true, false, true, false).justInTime, "no cap left low latency on");
    expect(!lowLatencyPlan(true, true, false, false).justInTime, "frame generation off left low latency on");
    expect(!lowLatencyPlan(true, false, false, true).reflexHolds, "no cap or frame generation reported Reflex holding");
}

void reflexKeepsJustInTimeBatchesAndStepsReleaseDelayAside() {
    const auto plan = lowLatencyPlan(true, true, true, true);
    expect(plan.justInTime, "Reflex on turned off the just-in-time batch start");
    expect(!plan.releaseDelay, "Reflex on left the release delay running");
    expect(plan.reflexHolds, "Reflex on was not reported as holding the game");
}

// Reflex on forces the delay to zero on every present and resets the controller. Reflex off resumes
// from zero, as a fresh controller would.
void reflexOnForcesZeroAndReflexOffResumesFromZero() {
    const auto off = lowLatencyPlan(true, true, true, false);
    const auto on = lowLatencyPlan(true, true, true, true);
    LowLatencyRelease release;
    Duration delay{};
    for (int i = 0; i < 20; ++i) delay = release.update(off.releaseDelay, 17ms, 100ms);
    expect(delay > Duration::zero(), "setup: early presents with Reflex off did not raise the delay");
    for (int i = 0; i < 20; ++i)
        expect(release.update(on.releaseDelay, 17ms, 100ms) == Duration::zero(),
            "Reflex on did not force the delay to zero");
    expect(release.margin() == 1ms && release.delay() == Duration::zero(),
        "Reflex on did not reset the controller");
    expect(release.update(off.releaseDelay, 9ms, 20ms) == 1ms,
        "Reflex off did not resume the delay from zero");
}

// Native acquire model: the next frame starts at the previous release point less one interval
// (not before the previous present returned at its deadline), renders for `render`, and
// presents at its own deadline. A steady game settles at the base margin.
void nativeAcquireModelSettlesAtMargin() {
    const Duration interval = std::chrono::microseconds{16667};
    const Duration render = 4ms;
    LowLatencyRelease release;
    Duration lead{};
    for (int64_t frame = 1; frame <= 600; ++frame) {
        const Duration previous = (frame - 1) * interval;
        const Duration start = std::max(previous + release.delay() - interval, previous);
        const Duration deadline = frame * interval;
        lead = deadline - (start + render);
        release.observe(lead, interval);
    }
    expect(std::abs((lead - release.margin()).count()) < 100'000,
        "the native acquire model did not settle at the margin");
}

// Three-frame queue model (VKD3D-Proton's default latency): a frame starts when the release
// point of the frame three before it has passed. A steady game settles near the margin
// instead of arriving two intervals early, and never oscillates into late arrivals.
void threeFrameQueueSettlesAtMargin() {
    const Duration interval = std::chrono::microseconds{16667};
    const Duration render = 8ms;
    LowLatencyRelease release;
    const Clock::time_point epoch{};
    std::vector<Clock::time_point> released(1200 + 3, epoch);
    Duration lead{};
    for (size_t frame = 0; frame < 1200; ++frame) {
        const auto deadline = epoch + static_cast<int64_t>(frame + 1) * interval;
        lead = deadline - (released[frame] + render);
        release.observe(lead, interval);
        if (frame > 600) expect(lead >= Duration::zero(), "the three-frame queue model arrived late");
        released[frame + 3] = deadline + release.delay();
    }
    expect(lead < release.margin() + 500us, "the three-frame queue model did not settle near the margin");
}

void acquireHoldIsBoundedByTheTimeout() {
    const auto now = Clock::now();
    auto hold = acquireHold(now, now + 10ms, 0);
    expect(hold.wait == Duration::zero() && !hold.expired, "a poll acquire was held");
    hold = acquireHold(now, now - 1ms, 5'000'000);
    expect(hold.wait == Duration::zero() && !hold.expired, "a passed release point was held");
    hold = acquireHold(now, now + 10ms, 20'000'000);
    expect(hold.wait == 10ms && !hold.expired, "a release point within the timeout was not held to it");
    hold = acquireHold(now, now + 10ms, 4'000'000);
    expect(hold.wait == 4ms && hold.expired, "a release point past the timeout was not cut to the timeout");
    hold = acquireHold(now, now + 10ms, UINT64_MAX);
    expect(hold.wait == 10ms && !hold.expired, "an infinite timeout did not hold to the release point");
}
}

int main() {
    steadyLeadMovesDelayByAnEighthOfTheExcess();
    earlyThenLateMovesDelayBothWays();
    jitterWidensMargin();
    delayStaysWithinThreeCapIntervals();
    noCapResetsDelay();
    lowLatencyPlanFollowsSettingCapFrameGenerationAndReflex();
    reflexKeepsJustInTimeBatchesAndStepsReleaseDelayAside();
    reflexOnForcesZeroAndReflexOffResumesFromZero();
    nativeAcquireModelSettlesAtMargin();
    threeFrameQueueSettlesAtMargin();
    acquireHoldIsBoundedByTheTimeout();
    std::cout << "low latency tests passed\n";
}

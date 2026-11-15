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
    nativeAcquireModelSettlesAtMargin();
    threeFrameQueueSettlesAtMargin();
    acquireHoldIsBoundedByTheTimeout();
    std::cout << "low latency tests passed\n";
}

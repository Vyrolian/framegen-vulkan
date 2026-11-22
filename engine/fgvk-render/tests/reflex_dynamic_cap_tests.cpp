/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/reflex_dynamic_cap.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

using namespace fgvk::layer;

namespace {
void expect(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

constexpr int64_t ms = 1'000'000;
constexpr int64_t second = 1'000'000'000;
// 120 FPS at 240 Hz with 2x frame generation: the static interval the controller floors at.
constexpr uint32_t floor120 = 8335;
constexpr uintptr_t mainQueue = 0x100;
constexpr uintptr_t otherQueue = 0x200;
const int64_t start = 10 * second;

// One full window of the main queue: sixteen frames with the same wait, all at `now`.
void feedWindow(ReflexDynamicCap& cap, const uintptr_t queue, const uint64_t submits, const int64_t waitNs,
        const int64_t now) {
    for (size_t frame = 0; frame < reflexDynamicWindowFrames; ++frame)
        cap.feed(queue, submits, waitNs, now);
}

// A steady low wait at the floor is the attainable ceiling: the interval stays exactly at the static one.
void steadyLowWaitStaysAtFloor() {
    ReflexDynamicCap cap;
    int64_t now = start;
    for (int window = 0; window < 50; ++window) {
        feedWindow(cap, mainQueue, 18, 90'000, now);
        expect(cap.interval(now, floor120, true) == floor120, "a low wait at the floor moved the interval");
        now += 133 * ms;
    }
    expect(reflexDynamicRunning.load() && reflexDynamicIntervalUs.load() == floor120,
        "the running controller did not report its exact floor");
}

// A window above 1 ms raises the interval by a quarter of the excess over 0.5 ms, at most 5 % per window.
void highWaitRaisesWithinFivePercent() {
    ReflexDynamicCap cap;
    const int64_t now = start;
    expect(cap.interval(now, floor120, true) == floor120, "the first present did not start at the floor");
    feedWindow(cap, mainQueue, 18, 5 * ms, now);
    // 8335 + min(0.25 * (5000 - 500), 0.05 * 8335) = 8751.75, rounded up.
    const auto first = cap.interval(now, floor120, true);
    expect(first == 8752, "a 5 ms median did not raise the interval by the 5 % limit");
    expect(reflexDynamicWaitUs.load() == 5000, "the window median was not published");
    feedWindow(cap, mainQueue, 18, 5 * ms, now);
    // 8751.75 + 0.05 * 8751.75 = 9189.34, rounded up.
    const auto nextInterval = cap.interval(now, floor120, true);
    expect(nextInterval == 9190, "the second window did not raise the interval by 5 % of its own value");
}

// Below 0.3 ms the interval falls by 0.5 % per window and ends at the static interval, exactly.
void lowWaitDecaysToExactFloor() {
    ReflexDynamicCap cap;
    const int64_t now = start;
    expect(cap.interval(now, floor120, true) == floor120, "the first present did not start at the floor");
    for (int window = 0; window < 4; ++window) feedWindow(cap, mainQueue, 18, 5 * ms, now);
    uint32_t previous = cap.interval(now, floor120, true);
    expect(previous > floor120, "the controller did not raise before the decay");
    for (int window = 0; window < 400; ++window) {
        feedWindow(cap, mainQueue, 18, 100'000, now);
        const auto next = cap.interval(now, floor120, true);
        expect(next <= previous && next >= floor120, "the decay rose or passed the static interval");
        previous = next;
    }
    expect(previous == floor120, "the decay did not return exactly to the static interval");
}

// A static interval that rises past the controller's snaps the controller up to it.
void floorChangeSnaps() {
    ReflexDynamicCap cap;
    const int64_t now = start;
    expect(cap.interval(now, floor120, true) == floor120, "the first present did not start at the floor");
    feedWindow(cap, mainQueue, 18, 5 * ms, now);
    expect(cap.interval(now, floor120, true) == 8752, "setup did not raise the interval");
    expect(cap.interval(now, 9000, true) == 9000, "a higher static interval did not snap the controller up");
    expect(cap.interval(now, 9000, true) == 9000, "the snapped interval moved without a wait");
    expect(cap.interval(now, floor120, false) == floor120,
        "turning the controller off did not return the static interval");
    expect(!reflexDynamicRunning.load(), "an inactive controller reported itself running");
}

// Frames of queues other than the main queue are ignored. The main queue is the one with the highest average
// of submits per frame, so a queue that submits more per frame takes over with its own waits.
void mainQueueIsTheBusiestByPerFrameSubmits() {
    ReflexDynamicCap cap;
    const int64_t now = start;
    for (int frame = 0; frame < 40; ++frame) {
        cap.feed(mainQueue, 18, 90'000, now);
        cap.feed(otherQueue, 3, 5 * ms, now);
    }
    expect(cap.interval(now, floor120, true) == floor120,
        "a queue with few submits per frame drove the interval");

    // The other queue now submits more per frame than the main queue, and its waits raise the interval.
    for (int frame = 0; frame < 40; ++frame) {
        cap.feed(mainQueue, 18, 90'000, now);
        cap.feed(otherQueue, 40, 5 * ms, now);
    }
    expect(cap.interval(now, floor120, true) > floor120,
        "the queue with the most submits per frame did not become the main queue");
}

// No main-queue frame for a second resets the controller to the static interval (pause, loading).
void idleResetsToFloor() {
    ReflexDynamicCap cap;
    const int64_t now = start;
    expect(cap.interval(now, floor120, true) == floor120, "the first present did not start at the floor");
    feedWindow(cap, mainQueue, 18, 5 * ms, now);
    expect(cap.interval(now, floor120, true) > floor120, "setup did not raise the interval");
    expect(cap.interval(now + 2 * second, floor120, true) == floor120,
        "a second without main-queue frames did not reset the interval");
    expect(!reflexDynamicRunning.load(), "an idle controller reported itself running");
}

// The registry hands out one controller per device and forgets it when the device goes.
void registryIsPerDevice() {
    const auto deviceA = reinterpret_cast<VkDevice>(uintptr_t{0x5000});
    const auto deviceB = reinterpret_cast<VkDevice>(uintptr_t{0x6000});
    const auto first = ReflexDynamicCapRegistry::get(deviceA);
    expect(first == ReflexDynamicCapRegistry::get(deviceA), "one device got two controllers");
    expect(first != ReflexDynamicCapRegistry::get(deviceB), "two devices shared a controller");
    ReflexDynamicCapRegistry::erase(deviceA);
    expect(first != ReflexDynamicCapRegistry::get(deviceA), "an erased device kept its controller");
    ReflexDynamicCapRegistry::erase(deviceA);
    ReflexDynamicCapRegistry::erase(deviceB);
}
}

int main() {
    steadyLowWaitStaysAtFloor();
    highWaitRaisesWithinFivePercent();
    lowWaitDecaysToExactFloor();
    floorChangeSnaps();
    mainQueueIsTheBusiestByPerFrameSubmits();
    idleResetsToFloor();
    registryIsPerDevice();
    std::cout << "reflex dynamic cap tests passed\n";
}

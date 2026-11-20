/* SPDX-License-Identifier: GPL-3.0-or-later */
// OS1 tests: the screen sample ring, the producer mode of one context and the source rule. Pure: no
// Vulkan, no Wayland, no threads except the ring's push-after-close check.
#include "swapchain/screen_samples.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

using namespace fgvk::layer;

namespace {
void expect(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

ScreenSample sampleOf(const uint64_t context) {
    return ScreenSample{context, context * 10, context % 2 == 1, static_cast<int64_t>(context) * 1000};
}

// ---- Ring -----------------------------------------------------------------------------------------

void ringReadsInPushOrderAndKeepsTheCursor() {
    ScreenSampleRing ring;
    for (uint64_t context = 1; context <= 5; ++context) ring.push(sampleOf(context));
    uint64_t cursor = 0;
    std::array<ScreenSample, 3> out{};
    expect(ring.read(&cursor, out.data(), out.size()) == 3, "a read takes at most the requested count");
    expect(out[0].context == 1 && out[2].context == 3, "samples are read in push order");
    expect(ring.read(&cursor, out.data(), out.size()) == 2, "the next read continues from the cursor");
    expect(out[0].context == 4 && out[1].context == 5, "the second read gives the rest, in order");
    expect(ring.read(&cursor, out.data(), out.size()) == 0, "an empty read returns nothing");
    expect(ring.dropped() == 0, "a ring that was read is not counted as dropped");
    expect(out[1].source == 50 && out[1].generated && out[1].screenNs == 5000,
        "every field of the sample is kept");
}

void ringDropsTheOldestUnreadAndCountsIt() {
    ScreenSampleRing ring;
    for (uint64_t context = 0; context < 300; ++context) ring.push(sampleOf(context));
    // 300 unread samples into 256 slots: 0..43 are dropped and counted at push.
    expect(ring.dropped() == 44, "pushes past the capacity drop the oldest unread samples and count them");
    uint64_t cursor = 0;
    std::vector<ScreenSample> out(ScreenSampleRing::capacity);
    expect(ring.read(&cursor, out.data(), out.size()) == ScreenSampleRing::capacity,
        "the ring keeps the newest capacity samples");
    expect(out.front().context == 44 && out.back().context == 299, "the kept samples are the newest, in order");
    expect(ring.read(&cursor, out.data(), out.size()) == 0, "nothing is left after the read");
}

void readerThatKeptUpIsNeverDropped() {
    ScreenSampleRing ring;
    uint64_t cursor = 0;
    ScreenSample out{};
    for (uint64_t context = 0; context < 1000; ++context) {
        ring.push(sampleOf(context));
        expect(ring.read(&cursor, &out, 1) == 1 && out.context == context,
            "a reader that reads after each push sees every sample");
    }
    expect(ring.dropped() == 0, "a reader that keeps up never drops");
}

void cursorBehindTheDropsResumesAtTheOldestKept() {
    ScreenSampleRing ring;
    for (uint64_t context = 0; context < 260; ++context) ring.push(sampleOf(context));
    uint64_t stale = 0;
    ScreenSample out{};
    expect(ring.read(&stale, &out, 1) == 1 && out.context == 4,
        "a cursor behind the dropped samples resumes at the oldest kept one");
}

void pushAfterCloseIsANoOp() {
    ScreenSampleRing ring;
    ring.push(sampleOf(1));
    expect(ring.accepting(), "a new ring accepts samples");
    ring.close();
    expect(!ring.accepting(), "a closed ring refuses samples");
    ring.push(sampleOf(2));
    ring.push(sampleOf(3));
    expect(ring.dropped() == 0, "a push after close is not counted as a drop");
    uint64_t cursor = 0;
    std::array<ScreenSample, 4> out{};
    expect(ring.read(&cursor, out.data(), out.size()) == 1 && out[0].context == 1,
        "samples pushed before the close stay readable, and nothing is added after it");
}

void pushAfterCloseFromAnotherThreadIsSafe() {
    auto ring = std::make_shared<ScreenSampleRing>();
    std::thread producer([ring] {
        for (uint64_t context = 0; context < 10000; ++context) ring->push(sampleOf(context));
    });
    ring->close();
    producer.join();
    expect(!ring->accepting(), "the close is seen by the ring after the producer joins");
    const uint64_t dropped = ring->dropped();
    ring->push(sampleOf(1));
    expect(ring->dropped() == dropped, "no push after the close changes the drop count");
}

// ---- Mode ----------------------------------------------------------------------------------------

// marker -> screen -> timeout -> marker -> screen: each switch is reported once.
void modeSwitchesOnScreenSamplesAndTimesOut() {
    constexpr int64_t second = 1'000'000'000;
    ScreenOffsetMode mode;
    expect(!mode.screen(), "the mode starts with the markers");
    expect(!mode.expire(5 * second), "expiry without screen mode is nothing");
    expect(mode.sample(second), "the first screen sample starts screen mode");
    expect(mode.screen(), "screen mode is on after the sample");
    expect(!mode.sample(second + 1), "a further sample does not switch again");
    expect(!mode.expire(second + 1 + ScreenOffsetMode::timeoutNs - 1),
        "screen mode holds until a full second passes without a sample");
    expect(mode.expire(second + 1 + ScreenOffsetMode::timeoutNs), "a second without a sample ends screen mode");
    expect(!mode.screen(), "the mode is back on the markers");
    expect(!mode.expire(10 * second), "expiry is reported once");
    expect(mode.sample(10 * second), "a new screen sample starts screen mode again");
    expect(mode.screen(), "screen mode is on again");
}

void aSampleKeepsScreenModeAlive() {
    constexpr int64_t second = 1'000'000'000;
    ScreenOffsetMode mode;
    static_cast<void>(mode.sample(0));
    for (int64_t at = 500'000'000; at < 10 * second; at += 500'000'000) {
        expect(!mode.expire(at), "screen mode is not ended while samples keep arriving");
        static_cast<void>(mode.sample(at));
    }
    expect(mode.screen(), "screen mode stays on for a steady stream of samples");
}

// ---- Clock and source rule ----------------------------------------------------------------------

void onlyTheMonotonicClockGivesScreenSamples() {
    expect(screenSampleClockSupported(static_cast<uint32_t>(CLOCK_MONOTONIC)),
        "CLOCK_MONOTONIC shares the steady clock's domain");
    expect(!screenSampleClockSupported(0), "CLOCK_REALTIME gives no screen sample");
    expect(!screenSampleClockSupported(2), "CLOCK_MONOTONIC_RAW gives no screen sample");
    expect(!screenSampleClockSupported(UINT32_MAX), "an unknown clock gives no screen sample");
}

void sourceRuleNamesTheFirstMissingInput() {
    const auto none = ScreenSource::None;
    // The correction needs the real offset and the environment switch, in that order. No pacing profile is an input.
    const auto noOffset = screenCorrectionSource(false, true, true, nullptr, true);
    expect(noOffset.source == none && std::string_view(noOffset.reason) == "real-offset-off",
        "the real offset switch is the first input");
    const auto noEnvironment = screenCorrectionSource(true, false, true, nullptr, true);
    expect(noEnvironment.source == none && std::string_view(noEnvironment.reason) == "env-off",
        "FGVK_ONSCREEN_CORRECTION=0 is the second input");
    // The watcher drives the correction with the profile off: the counter, not the profile, names the batches.
    const auto unprofiledWatcher = screenCorrectionSource(true, true, true, "active", false);
    expect(unprofiledWatcher.source == ScreenSource::Watcher && std::string_view(unprofiledWatcher.reason) == "active",
        "the correction runs without the pacing profile");
    // Then the watcher first, then native Wayland feedback, then none with the gate's reason.
    const auto watcher = screenCorrectionSource(true, true, true, nullptr, true);
    expect(watcher.source == ScreenSource::Watcher && std::string_view(watcher.reason) == "active",
        "an active watcher is the source, even when the surface also has feedback");
    const auto wayland = screenCorrectionSource(true, true, false, "surface-kind", true);
    expect(wayland.source == ScreenSource::Wayland && std::string_view(wayland.reason) == "active",
        "native Wayland feedback is the source when the watcher is not active");
    const auto gateRefused = screenCorrectionSource(true, true, false, "present-id2-flag", false);
    expect(gateRefused.source == none && std::string_view(gateRefused.reason) == "present-id2-flag",
        "without a watcher or feedback the gate's reason is reported");
    const auto noReason = screenCorrectionSource(true, true, false, nullptr, false);
    expect(noReason.source == none && std::string_view(noReason.reason) == "gate",
        "a missing reason is reported as the gate");
    expect(std::string_view(screenSourceName(ScreenSource::Watcher)) == "watcher" &&
        std::string_view(screenSourceName(ScreenSource::Wayland)) == "wayland" &&
        std::string_view(screenSourceName(none)) == "none", "source names");
}
} // namespace

int main() {
    ringReadsInPushOrderAndKeepsTheCursor();
    ringDropsTheOldestUnreadAndCountsIt();
    readerThatKeptUpIsNeverDropped();
    cursorBehindTheDropsResumesAtTheOldestKept();
    pushAfterCloseIsANoOp();
    pushAfterCloseFromAnotherThreadIsSafe();
    modeSwitchesOnScreenSamplesAndTimesOut();
    aSampleKeepsScreenModeAlive();
    onlyTheMonotonicClockGivesScreenSamples();
    sourceRuleNamesTheFirstMissingInput();
    std::cout << "framegen-vulkan: screen sample tests passed\n";
    return 0;
}

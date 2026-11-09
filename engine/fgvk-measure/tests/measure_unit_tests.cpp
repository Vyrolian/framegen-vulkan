/* SPDX-License-Identifier: GPL-3.0-or-later */

// Device-free unit tests for the measurer's pure logic: timestamp conversion, the slot ring state
// machine and the exact record lines. Run with one argument: timestamp, slot-ring or record-format.

#include "events.hpp"
#include "slot_ring.hpp"
#include "timestamp.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace {

using fgvk::measure::Calibration;
using fgvk::measure::Event;
using fgvk::measure::EventKind;
using fgvk::measure::SlotRing;
using fgvk::measure::Summary;
using fgvk::measure::TickDomain;
using fgvk::measure::ticksToHostNs;

int failures = 0;

void expectTrue(bool value, const char* expression, int line) {
    if (value) return;
    std::fprintf(stderr, "line %d: expected true: %s\n", line, expression);
    ++failures;
}

void expectEqual(unsigned long long actual, unsigned long long expected, const char* expression, int line) {
    if (actual == expected) return;
    std::fprintf(stderr, "line %d: %s = %llu, expected %llu\n", line, expression, actual, expected);
    ++failures;
}

void expectEqualText(const std::string& actual, const std::string& expected, int line) {
    if (actual == expected) return;
    std::fprintf(stderr, "line %d: text mismatch\n  actual:   %s  expected: %s", line, actual.c_str(), expected.c_str());
    ++failures;
}

#define EXPECT_TRUE(expr) expectTrue((expr), #expr, __LINE__)
#define EXPECT_EQ(actual, expected) expectEqual((actual), (expected), #actual, __LINE__)
#define EXPECT_TEXT(actual, expected) expectEqualText((actual), (expected), __LINE__)

void timestampConversion() {
    // Unit tick period: ticks map one-to-one onto the host offset from the calibration.
    const TickDomain nanoseconds{64, 1.0};
    const Calibration base{1000, 5'000'000'000};
    EXPECT_EQ(*ticksToHostNs(1500, base, nanoseconds), 5'000'000'500ull);
    EXPECT_EQ(*ticksToHostNs(1000, base, nanoseconds), 5'000'000'000ull);

    // 19.2 MHz timestamp clock: 19,200 ticks are 1 ms.
    const TickDomain nineteenPointTwo{64, 1.0e9 / 19.2e6};
    EXPECT_EQ(*ticksToHostNs(19'200, Calibration{0, 1000}, nineteenPointTwo), 1'001'000ull);

    // Fractional period: 4 ticks of 0.5 ns is exactly 2 ns.
    const TickDomain halfNanosecond{64, 0.5};
    EXPECT_EQ(*ticksToHostNs(1004, Calibration{1000, 2000}, halfNanosecond), 2002ull);

    // A 36-bit counter that wrapped after the calibration: the calibration sits 10 ticks before the
    // wrap and the query 10 ticks after it, so the query is 20 ticks later.
    const uint64_t mask36 = (uint64_t{1} << 36) - 1;
    const TickDomain bits36{36, 1.0};
    EXPECT_EQ(*ticksToHostNs(10, Calibration{mask36 - 9, 1'000'000'000}, bits36), 1'000'000'020ull);

    // The other side of the wrap: the query is 15 ticks before the calibration (10 back to zero,
    // then 5 more back across the wrap).
    EXPECT_EQ(*ticksToHostNs(mask36 - 4, Calibration{10, 1'000'000'000}, bits36), 999'999'985ull);

    // Bits above the valid range are ignored, so a dirty upper half converts the same way.
    EXPECT_EQ(*ticksToHostNs((uint64_t{1} << 40) | 1000, Calibration{1000, 2'000'000'000}, bits36), 2'000'000'000ull);

    // A 32-bit counter wrapping inside 32 bits: 0x10 is 32 ticks after 0xFFFFFFF0.
    EXPECT_EQ(*ticksToHostNs(0x10, Calibration{0xFFFFFFF0u, 1000}, TickDomain{32, 1.0}), 1032ull);

    // A full 64-bit counter: the calibration is two ticks below the wrap and the query two above.
    EXPECT_EQ(*ticksToHostNs(2, Calibration{UINT64_MAX - 1, 1000}, TickDomain{64, 1.0}), 1004ull);

    // No timestamps in the family, no period, or a result before the clock's zero: no value.
    EXPECT_TRUE(!ticksToHostNs(1, base, TickDomain{0, 1.0}).has_value());
    EXPECT_TRUE(!ticksToHostNs(1, base, TickDomain{64, 0.0}).has_value());
    EXPECT_TRUE(!ticksToHostNs(0, Calibration{100, 5}, nanoseconds).has_value());
}

// Claims every free slot, in index order, and returns them.
std::vector<std::size_t> claimAll(SlotRing<32>& ring) {
    std::vector<std::size_t> claimed;
    while (const auto slot = ring.claim()) claimed.push_back(*slot);
    return claimed;
}

void slotRingStateMachine() {
    using State = SlotRing<32>::State;
    SlotRing<32> ring;

    // A fresh ring hands out all 32 slots, each once, and then reports full (unmeasured).
    const std::vector<std::size_t> all = claimAll(ring);
    EXPECT_EQ(all.size(), 32ull);
    std::array<bool, 32> seen{};
    for (const std::size_t slot : all) {
        EXPECT_TRUE(slot < 32);
        if (slot < 32) seen[slot] = true;
    }
    for (const bool value : seen) EXPECT_TRUE(value);
    EXPECT_TRUE(!ring.claim().has_value());

    // Slot 3 is submitted with sequence 42. Its sequence is kept, and it is submitted, not free.
    EXPECT_TRUE(ring.publish(3, 42));
    EXPECT_TRUE(ring.submitted(3));
    EXPECT_TRUE(ring.state(3) == State::Submitted);
    EXPECT_EQ(ring.seq(3), 42ull);

    // Publishing a slot that is not recording is refused, and so is retiring one that is not submitted.
    EXPECT_TRUE(!ring.publish(3, 43));
    EXPECT_TRUE(!ring.retire(4));
    EXPECT_EQ(ring.seq(3), 42ull);

    // The other 31 slots are cancelled (their submits failed). Only they can be claimed again, so
    // slot 3 is never handed out while its fence and queries are pending.
    for (std::size_t i = 0; i < 32; ++i) {
        if (i != 3) EXPECT_TRUE(ring.cancel(i));
    }
    const std::vector<std::size_t> others = claimAll(ring);
    EXPECT_EQ(others.size(), 31ull);
    for (const std::size_t slot : others) EXPECT_TRUE(slot != 3);
    EXPECT_TRUE(ring.state(3) == State::Submitted);
    for (const std::size_t slot : others) EXPECT_TRUE(ring.cancel(slot));

    // The reader retires slot 3 after its fence-ready read. Afterwards the ring hands it out again.
    EXPECT_TRUE(ring.retire(3));
    EXPECT_TRUE(ring.state(3) == State::Free);
    const std::vector<std::size_t> again = claimAll(ring);
    EXPECT_EQ(again.size(), 32ull);
    bool sawThree = false;
    for (const std::size_t slot : again) sawThree = sawThree || slot == 3;
    EXPECT_TRUE(sawThree);

    // A recording slot that fails to submit is cancelled back to free and is not submitted.
    SlotRing<32> failing;
    const auto slot = failing.claim();
    EXPECT_TRUE(slot.has_value());
    if (slot) {
        EXPECT_TRUE(!failing.retire(*slot));  // Recording is not Submitted.
        EXPECT_TRUE(failing.cancel(*slot));
        EXPECT_TRUE(failing.state(*slot) == State::Free);
        EXPECT_TRUE(!failing.submitted(*slot));
        EXPECT_TRUE(!failing.anySubmitted());
    }

    // A small ring rejects out-of-range indices.
    SlotRing<4> small;
    EXPECT_TRUE(!small.retire(0));
    EXPECT_TRUE(!small.publish(0, 7));
    EXPECT_TRUE(!small.retire(9));
    EXPECT_TRUE(!small.cancel(9));
    EXPECT_TRUE(!small.submitted(0));
    EXPECT_TRUE(!small.anySubmitted());
}

void recordFormatting() {
    std::string out;
    Event present{};
    present.kind = EventKind::Present;
    present.seq = 7;
    present.swapchain = 2;
    present.image = 1;
    present.waits = 1;
    present.callStartNs = 1000;
    present.callEndNs = 1500;
    present.result = 0;
    present.measured = 1;
    fgvk::measure::appendEvent(out, present);
    EXPECT_TEXT(out, "framegen-vulkan: measure: present seq=7 swapchain=2 image=1 waits=1 call_start_ns=1000 "
            "call_end_ns=1500 result=0 measured=1\n");

    // Without GPU timing the present is unmeasured and its gpu line reports available=0 with zeros.
    out.clear();
    present.measured = 0;
    present.gpuUnavailable = 1;
    fgvk::measure::appendEvent(out, present);
    EXPECT_TEXT(out, "framegen-vulkan: measure: present seq=7 swapchain=2 image=1 waits=1 call_start_ns=1000 "
            "call_end_ns=1500 result=0 measured=0\n"
            "framegen-vulkan: measure: gpu seq=7 available=0 gpu_start_ns=0 gpu_end_ns=0\n");

    // A negative VkResult stays negative.
    out.clear();
    present.gpuUnavailable = 0;
    present.result = -1000001004;
    fgvk::measure::appendEvent(out, present);
    EXPECT_TEXT(out, "framegen-vulkan: measure: present seq=7 swapchain=2 image=1 waits=1 call_start_ns=1000 "
            "call_end_ns=1500 result=-1000001004 measured=0\n");

    out.clear();
    Event gpu{};
    gpu.kind = EventKind::Gpu;
    gpu.seq = 7;
    gpu.available = 1;
    gpu.gpuStartNs = 1200;
    gpu.gpuEndNs = 1300;
    fgvk::measure::appendEvent(out, gpu);
    EXPECT_TEXT(out, "framegen-vulkan: measure: gpu seq=7 available=1 gpu_start_ns=1200 gpu_end_ns=1300\n");

    out.clear();
    Event presented{};
    presented.kind = EventKind::Presented;
    presented.seq = 7;
    presented.presentedNs = 5'000'000'000;
    presented.refreshNs = 16'666'667;
    presented.flags = 1;
    presented.msc = 12345;
    fgvk::measure::appendEvent(out, presented);
    EXPECT_TEXT(out, "framegen-vulkan: measure: presented seq=7 presented_ns=5000000000 refresh_ns=16666667 "
            "flags=1 msc=12345\n");

    out.clear();
    Event discarded{};
    discarded.kind = EventKind::Discarded;
    discarded.seq = 8;
    fgvk::measure::appendEvent(out, discarded);
    EXPECT_TEXT(out, "framegen-vulkan: measure: discarded seq=8\n");

    out.clear();
    Event clock{};
    clock.kind = EventKind::Clock;
    clock.clockId = 1;
    fgvk::measure::appendEvent(out, clock);
    EXPECT_TEXT(out, "framegen-vulkan: measure: clock clock_id=1\n");

    out.clear();
    fgvk::measure::appendSession(out, 4242, "AMD Radeon Graphics (RADV GFX1100)", true);
    EXPECT_TEXT(out, "framegen-vulkan: measure: session pid=4242 device=AMD Radeon Graphics (RADV GFX1100) "
            "calibrated=1\n");

    out.clear();
    Summary summary{};
    summary.presents = 10;
    summary.measured = 9;
    summary.unmeasured = 1;
    summary.feedbacks = 8;
    summary.presented = 7;
    summary.discarded = 1;
    fgvk::measure::appendSummary(out, summary);
    EXPECT_TEXT(out, "framegen-vulkan: measure: summary presents=10 measured=9 unmeasured=1 feedbacks=8 "
            "presented=7 discarded=1\n");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "";
    if (which == "timestamp") {
        timestampConversion();
    } else if (which == "slot-ring") {
        slotRingStateMachine();
    } else if (which == "record-format") {
        recordFormatting();
    } else {
        std::fprintf(stderr, "usage: %s timestamp|slot-ring|record-format\n", argc ? argv[0] : "measure_unit_tests");
        return 2;
    }
    if (failures) {
        std::fprintf(stderr, "%s: %d failure(s)\n", which.c_str(), failures);
        return 1;
    }
    std::printf("%s: ok\n", which.c_str());
    return 0;
}

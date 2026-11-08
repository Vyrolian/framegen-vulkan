/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/render_slot_ring.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

using namespace fgvk::layer;
using namespace std::chrono_literals;

namespace {
    using Clock = std::chrono::steady_clock;

    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }

    // Matches Swapchain::renderSlotDepth.
    constexpr std::size_t depth = 3;
    constexpr uint64_t budgetNs = 150'000'000;

    void testRingWrapsAtEveryDepth() {
        for (const std::size_t ringDepth : {std::size_t{1}, std::size_t{2}, depth}) {
            std::size_t index = 0;
            for (std::size_t step = 1; step <= 2 * ringDepth; ++step) {
                index = nextRenderSlot(index, ringDepth);
                expect(index == step % ringDepth,
                    "render slot ring did not wrap to the expected slot");
                expect(index < ringDepth,
                    "render slot ring left its depth");
            }
            expect(index == 0, "two full rotations did not return to slot zero");
        }
        expect(nextRenderSlot(0, 1) == 0,
            "depth one did not keep every batch in slot zero");
        expect(nextRenderSlot(1, 2) == 0 && nextRenderSlot(2, 3) == 0,
            "last slot did not wrap to slot zero");
    }

    void testIdleAdmissionNeverWaits() {
        bool inFlight = false;
        bool waited = false;
        expect(admitRenderSlot(inFlight, budgetNs, [&waited](const uint64_t) {
                waited = true;
                return false;
            }) && !waited && !inFlight,
            "an idle render slot waited before reuse");
    }

    void testAdmitTimeoutKeepsSlotInFlight() {
        bool inFlight = true;
        uint64_t requested = 0;
        expect(!admitRenderSlot(inFlight, budgetNs, [&requested](const uint64_t budget) {
                requested = budget;
                return false;
            }) && inFlight && requested == budgetNs,
            "admission timeout cleared a slot that is still in flight");

        expect(admitRenderSlot(inFlight, budgetNs, [](const uint64_t) { return true; }) &&
            !inFlight,
            "signalled admission left its slot in flight");
    }

    void testDrainSharesOneBudget() {
        // The fake clock advances only inside the waits, so each wait spends
        // time that the next wait cannot recover.
        Clock::time_point clock{};
        std::array<bool, depth> inFlight{true, true, true};
        std::vector<uint64_t> budgets;
        bool waited = false;
        const bool drained = drainRenderSlots(inFlight, 150ms,
            [&](const std::size_t, const uint64_t remainingNs) {
                budgets.push_back(remainingNs);
                clock += 100ms;
                return true;
            },
            [&clock] { return clock; }, waited);
        expect(drained && waited, "signalled drain did not report its waits");
        expect(budgets == std::vector<uint64_t>({150'000'000, 50'000'000, 0}),
            "in-flight slots did not share one drain budget");
        expect(inFlight == (std::array<bool, depth>{false, false, false}),
            "signalled drain left a slot in flight");
    }

    void testDrainStopsAtFirstFailure() {
        Clock::time_point clock{};
        std::array<bool, depth> inFlight{true, true, true};
        std::vector<std::size_t> visited;
        bool waited = false;
        const bool drained = drainRenderSlots(inFlight, 150ms,
            [&visited](const std::size_t index, const uint64_t) {
                visited.push_back(index);
                return index != 1;
            },
            [&clock] { return clock; }, waited);
        expect(!drained && visited == std::vector<std::size_t>({0, 1}),
            "drain waited for a slot after the first failure");
        expect(inFlight == (std::array<bool, depth>{false, true, true}),
            "failed drain changed a slot it did not signal or left a later slot clear");
        expect(waited, "failed drain did not report the slot it waited for");
    }

    void testDrainWithNothingInFlight() {
        Clock::time_point clock{};
        std::array<bool, depth> inFlight{};
        bool waited = true;
        bool called = false;
        const bool drained = drainRenderSlots(inFlight, 150ms,
            [&called](const std::size_t, const uint64_t) {
                called = true;
                return true;
            },
            [&clock] { return clock; }, waited);
        expect(drained && !called && !waited,
            "an empty drain reported an in-flight wait");
    }

    void testRealOnlyBatchLeavesSlotIdle() {
        // Models the timeline path of Swapchain::prepareRenderFence with the
        // seams. Frame two carries a real-only deferred batch. It does not
        // submit the slot's fence, so the slot is not in flight when the ring
        // returns to it.
        std::array<bool, depth> inFlight{};
        std::size_t index = 0;
        std::size_t waits = 0;
        const std::array<bool, 6> waitedAtFrame{false, false, false, true, true, false};
        for (std::size_t frame = 0; frame < waitedAtFrame.size(); ++frame) {
            index = nextRenderSlot(index, depth);
            const std::size_t before = waits;
            expect(admitRenderSlot(inFlight.at(index), budgetNs, [&waits](const uint64_t) {
                    ++waits;
                    return true;
                }),
                "modelled admission failed on a signalled slot");
            // current behaviour, see handoff: no slot fence while sourceCopy is
            // re-recorded, so the real-only batch's slot is admitted without a wait.
            expect((waits != before) == waitedAtFrame.at(frame),
                "a real-only batch changed when its slot was admitted");
            inFlight.at(index) = frame != 2;
        }
    }
}

int main() {
    testRingWrapsAtEveryDepth();
    testIdleAdmissionNeverWaits();
    testAdmitTimeoutKeepsSlotInFlight();
    testDrainSharesOneBudget();
    testDrainStopsAtFirstFailure();
    testDrainWithNothingInFlight();
    testRealOnlyBatchLeavesSlotIdle();
    std::cout << "render slot ring tests passed\n";
    return 0;
}

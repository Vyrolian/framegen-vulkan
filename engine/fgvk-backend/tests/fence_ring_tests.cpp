/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fence_ring.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

using namespace fgvk::backend;

namespace {
    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }

    // Same shape as the backend's previous-work fence budget.
    constexpr uint64_t sharedBudgetNs = 250'000'000;
    constexpr uint64_t waitCostNs = 100'000'000;

    void testOnlyCurrentSlotWaitedWithoutWaitAll() {
        const std::array<bool, 3> scheduled{true, true, true};
        std::array<bool, 3> signalled{};
        std::vector<std::size_t> visited;
        const bool ok = waitScheduledFences(scheduled, 3, 1, false, sharedBudgetNs,
            [&visited](const std::size_t slot, uint64_t&) {
                visited.push_back(slot);
                return true;
            }, signalled);
        expect(ok && visited == std::vector<std::size_t>{1},
            "non-waitAll scan waited a slot other than the current one");
        expect(signalled == (std::array<bool, 3>{false, true, false}),
            "non-waitAll scan reported the wrong signalled slot");

        // An unscheduled current slot has nothing to wait for.
        visited.clear();
        const std::array<bool, 3> idleCurrent{true, false, true};
        expect(waitScheduledFences(idleCurrent, 3, 1, false, sharedBudgetNs,
                [&visited](const std::size_t slot, uint64_t&) {
                    visited.push_back(slot);
                    return true;
                }, signalled) && visited.empty(),
            "unscheduled current slot was waited");
    }

    void testWaitAllWaitsEveryScheduledSlot() {
        const std::array<bool, 3> scheduled{true, false, true};
        std::array<bool, 3> signalled{};
        std::vector<std::size_t> visited;
        const bool ok = waitScheduledFences(scheduled, 3, 0, true, sharedBudgetNs,
            [&visited](const std::size_t slot, uint64_t&) {
                visited.push_back(slot);
                return true;
            }, signalled);
        expect(ok && visited == std::vector<std::size_t>({0, 2}),
            "waitAll did not wait every scheduled slot exactly once");
        expect(signalled == (std::array<bool, 3>{true, false, true}),
            "waitAll reported the wrong signalled slots");
    }

    void testSharedBudgetDecrements() {
        const std::array<bool, 3> scheduled{true, true, true};
        std::array<bool, 3> signalled{};
        std::vector<uint64_t> budgets;
        const bool ok = waitScheduledFences(scheduled, 3, 0, true, sharedBudgetNs,
            [&budgets](const std::size_t, uint64_t& remainingNs) {
                budgets.push_back(remainingNs);
                remainingNs -= std::min(remainingNs, waitCostNs);
                return true;
            }, signalled);
        expect(ok && budgets == std::vector<uint64_t>({250'000'000, 150'000'000, 50'000'000}),
            "scheduled waits did not share one decrementing budget");

        // A wait that overruns the shared budget leaves zero for the next one.
        budgets.clear();
        expect(waitScheduledFences(scheduled, 3, 0, true, sharedBudgetNs,
                [&budgets](const std::size_t, uint64_t& remainingNs) {
                    budgets.push_back(remainingNs);
                    remainingNs = 0;
                    return true;
                }, signalled) &&
            budgets == std::vector<uint64_t>({250'000'000, 0, 0}),
            "an overrunning wait did not exhaust the shared budget");
    }

    void testTimeoutLeavesLaterSlotsUnwaited() {
        const std::array<bool, 3> scheduled{true, true, true};
        std::array<bool, 3> signalled{};
        std::vector<std::size_t> visited;
        const bool ok = waitScheduledFences(scheduled, 3, 0, true, sharedBudgetNs,
            [&visited](const std::size_t slot, uint64_t&) {
                visited.push_back(slot);
                return slot != 1;
            }, signalled);
        expect(!ok && visited == std::vector<std::size_t>({0, 1}),
            "timeout waited for slots after the failed fence");
        expect(signalled == (std::array<bool, 3>{true, false, false}),
            "timeout marked the failed or later fence as signalled");
    }

    void testPollVisitsEveryScheduledSlot() {
        const std::array<bool, 3> scheduled{true, false, true};
        std::vector<std::size_t> visited;
        std::vector<uint64_t> budgets;
        // Slot zero is unsignalled and slot two is signalled. Both are visited.
        const bool idle = pollScheduledFences(scheduled, 3, 0,
            [&](const std::size_t slot, uint64_t& remainingNs) {
                visited.push_back(slot);
                budgets.push_back(remainingNs);
                return slot == 2;
            });
        expect(!idle && visited == std::vector<std::size_t>({0, 2}),
            "poll stopped after an unsignalled fence");
        expect(budgets == std::vector<uint64_t>({0, 0}),
            "zero-budget poll did not pass zero to every fence");

        expect(pollScheduledFences(scheduled, 3, 0,
                [](const std::size_t, uint64_t&) { return true; }),
            "poll reported busy when every scheduled fence signalled");
        const std::array<bool, 3> none{};
        expect(pollScheduledFences(none, 3, 0,
                [](const std::size_t, uint64_t&) { return false; }),
            "poll with nothing scheduled reported busy");
    }

    void testPollSharesOneBudget() {
        const std::array<bool, 3> scheduled{true, true, true};
        std::vector<uint64_t> budgets;
        const bool idle = pollScheduledFences(scheduled, 3, sharedBudgetNs,
            [&budgets](const std::size_t, uint64_t& remainingNs) {
                budgets.push_back(remainingNs);
                remainingNs -= std::min(remainingNs, waitCostNs);
                return true;
            });
        expect(idle && budgets == std::vector<uint64_t>({250'000'000, 150'000'000, 50'000'000}),
            "poll did not share one decrementing budget");
    }

    // Records each visited slot and reports every fence as signalled.
    struct SignalledVisits {
        std::vector<std::size_t> slots;
        bool operator()(const std::size_t slot, uint64_t&) {
            slots.push_back(slot);
            return true;
        }
    };

    void testDepthLimitsTheScan() {
        const std::array<bool, 3> scheduled{true, true, true};
        std::array<bool, 3> signalled{};

        // Depth one has only slot zero, even when every slot is scheduled.
        SignalledVisits depthOne;
        expect(waitScheduledFences(scheduled, 1, 0, true, sharedBudgetNs,
                depthOne, signalled) &&
            depthOne.slots == std::vector<std::size_t>{0},
            "depth one waited beyond its single slot");
        expect(signalled == (std::array<bool, 3>{true, false, false}),
            "depth one reported slots outside the ring");

        SignalledVisits depthTwoCurrent;
        expect(waitScheduledFences(scheduled, 2, 1, false, sharedBudgetNs,
                depthTwoCurrent, signalled) &&
            depthTwoCurrent.slots == std::vector<std::size_t>{1},
            "depth two non-waitAll scan waited a slot other than current");

        SignalledVisits depthTwoAll;
        expect(waitScheduledFences(scheduled, 2, 1, true, sharedBudgetNs,
                depthTwoAll, signalled) &&
            depthTwoAll.slots == std::vector<std::size_t>({0, 1}),
            "depth two waitAll scan visited slots beyond the ring");

        SignalledVisits depthTwoPoll;
        expect(pollScheduledFences(scheduled, 2, sharedBudgetNs, depthTwoPoll) &&
            depthTwoPoll.slots == std::vector<std::size_t>({0, 1}),
            "depth two poll visited slots beyond the ring");
    }
}

int main() {
    testOnlyCurrentSlotWaitedWithoutWaitAll();
    testWaitAllWaitsEveryScheduledSlot();
    testSharedBudgetDecrements();
    testTimeoutLeavesLaterSlotsUnwaited();
    testPollVisitsEveryScheduledSlot();
    testPollSharesOneBudget();
    testDepthLimitsTheScan();
    std::cout << "backend fence ring tests passed\n";
    return 0;
}

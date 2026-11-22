/* SPDX-License-Identifier: GPL-3.0-or-later */

// FI-default: the FGVK_FG_INLINE parse, where only the exact value "0" turns the attached path off.
// FI1: pure helpers of the attached (same-device) generation path: the submit order of the source copy, the release
// value of a batch scheduled after its copy, and where a failed schedule presents from.
// FI2: the call-held lower budget (rule 2) and the hold check.

#include "swapchain/inline_generation.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
    using fgvk::layer::ScheduleFailureRoute;

    void require(const bool condition, const std::string& message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    // FI-default: unset (null), empty and every value other than "0" attach. Only "0" exactly turns the switch off.
    void testSwitchParse() {
        using fgvk::layer::parseInlineGenerationSwitch;
        static_assert(parseInlineGenerationSwitch(nullptr), "the switch parse must be usable in constant expressions");
        static_assert(!parseInlineGenerationSwitch("0"), "the switch parse must be usable in constant expressions");
        require(parseInlineGenerationSwitch(nullptr), "an unset FGVK_FG_INLINE must attach by default");
        require(parseInlineGenerationSwitch(""), "an empty FGVK_FG_INLINE must attach");
        require(parseInlineGenerationSwitch("1"), "FGVK_FG_INLINE=1 must attach");
        require(!parseInlineGenerationSwitch("0"), "FGVK_FG_INLINE=0 must turn the attached path off");
        require(parseInlineGenerationSwitch("00"), "only the exact value 0 turns the attached path off, not 00");
        require(parseInlineGenerationSwitch("yes"), "an unrecognised FGVK_FG_INLINE value must attach");
    }

    void testSubmitOrder() {
        require(fgvk::layer::sourceCopyPrecedesSchedule(true),
            "an attached source copy must be submitted before its batch");
        require(!fgvk::layer::sourceCopyPrecedesSchedule(false),
            "a detached source copy must keep its existing order after the batch");
    }

    void testReleaseValue() {
        // A copy of value S advances the index to S + 1. A batch with G outputs then signals S + 1 .. S + G, and its
        // source is released at S + G.
        for (uint64_t copy = 1; copy <= 9; ++copy) {
            for (size_t outputs = 1; outputs <= 4; ++outputs) {
                const uint64_t release = fgvk::layer::attachedSourceReleaseValue(copy + 1, outputs);
                require(release == copy + outputs,
                    "the attached release value must be the copy's value plus the output count");
            }
        }
        require(fgvk::layer::attachedSourceReleaseValue(2, 1) == 2,
            "the first single-output attached batch must release at value 2");
    }

    void testFailureRoutes() {
        require(fgvk::layer::scheduleFailureRoute(true, false) == ScheduleFailureRoute::PostCopyOriginal,
            "an attached failure without preacquired images must present through the post-copy semaphore");
        require(fgvk::layer::scheduleFailureRoute(true, true) == ScheduleFailureRoute::PostCopyRetirement,
            "an attached failure with preacquired images must retire them through the post-copy semaphore");
        require(fgvk::layer::scheduleFailureRoute(false, false) == ScheduleFailureRoute::NativeOnly,
            "a detached failure without preacquired images must stay native");
        require(fgvk::layer::scheduleFailureRoute(false, true) == ScheduleFailureRoute::NativeWithRetirement,
            "a detached failure with preacquired images must keep its retirement route");
        for (const bool preacquired : {false, true}) {
            const auto route = fgvk::layer::scheduleFailureRoute(true, preacquired);
            require(route == ScheduleFailureRoute::PostCopyOriginal || route == ScheduleFailureRoute::PostCopyRetirement,
                "an attached failure must never re-wait the application's wait semaphores");
        }
    }

    // FI2 rule 2: the budget is the lower images above the surface minimum and the worker's own holds, capped at four.
    void testCallBudget() {
        using fgvk::layer::inlineCallLowerBudget;
        require(fgvk::layer::inlineCallLowerImageCap == 4, "the call-held budget cap must be four");
        require(inlineCallLowerBudget(8, 4, 2) == 2, "two spare images must give a budget of two");
        require(inlineCallLowerBudget(10, 4, 2) == 4, "four spare images must give the full budget of four");
        require(inlineCallLowerBudget(12, 4, 2) == 4, "more spare images than the cap must still give four");
        require(inlineCallLowerBudget(9, 4, 0) == 4, "a spare count above the cap must be capped with no worker holds");
        require(inlineCallLowerBudget(7, 4, 0) == 3, "the budget must count the images above the surface minimum");
        require(inlineCallLowerBudget(6, 4, 2) == 0, "no spare image must give a budget of zero");
        require(inlineCallLowerBudget(5, 4, 2) == 0, "a negative spare count must give a budget of zero");
        require(inlineCallLowerBudget(0, 0, 0) == 0, "an empty swapchain must give a budget of zero");
        require(inlineCallLowerBudget(10, UINT32_MAX, 0) == 0,
            "a surface minimum above the image count must give a budget of zero");
    }

    // FI2 rule 2: one more output may be held when the outputs already held, plus this one, stay within the budget.
    void testCallHoldFits() {
        require(fgvk::layer::inlineCallHoldFits(0, 1), "one output must fit a budget of one");
        require(!fgvk::layer::inlineCallHoldFits(1, 1), "a second output must not fit a budget of one");
        require(fgvk::layer::inlineCallHoldFits(1, 2), "a second output must fit a budget of two");
        require(!fgvk::layer::inlineCallHoldFits(2, 2), "a third output must not fit a budget of two");
        require(fgvk::layer::inlineCallHoldFits(3, 4), "a fourth output must fit a budget of four");
        require(!fgvk::layer::inlineCallHoldFits(4, 4), "a fifth output must not fit a budget of four");
        require(!fgvk::layer::inlineCallHoldFits(0, 0), "no output may be held with a zero budget");
    }
}

// FP1: each generated attempt has one reason, in the order budget, lock, acquire.
void testGeneratedClassification() {
    using namespace fgvk::layer;
    require(classifyGeneratedAttempt(true, false, true, false) == InlineCallOutcome::Call,
        "an acquired generated output was not served at the call");
    require(classifyGeneratedAttempt(false, false, false, true) == InlineCallOutcome::Budget,
        "a budget refusal was not classified as budget");
    require(classifyGeneratedAttempt(false, true, false, false) == InlineCallOutcome::Budget,
        "a budget refusal must win over a busy queue");
    require(classifyGeneratedAttempt(true, true, false, false) == InlineCallOutcome::Lock,
        "a busy queue was not classified as lock");
    require(classifyGeneratedAttempt(true, false, false, true) == InlineCallOutcome::Acquire,
        "a not-ready lower acquire was not classified as acquire");
    require(classifyGeneratedAttempt(true, false, false, false) == InlineCallOutcome::Other,
        "a driver error was not classified as other");
}

// FP1: each real attempt has one reason. A cascade is an output that was never attempted, and an error (a refusal, a
// consumed copy that failed) is other.
void testRealClassification() {
    using namespace fgvk::layer;
    require(classifyRealAttempt(false, true, false, CallCopyStatus::Done) == InlineCallOutcome::Cascade,
        "a real whose generated output fell back was not a cascade");
    require(classifyRealAttempt(true, false, false, CallCopyStatus::NotReady) == InlineCallOutcome::Budget,
        "an over-budget real was not classified as budget");
    require(classifyRealAttempt(true, true, true, CallCopyStatus::NotReady) == InlineCallOutcome::Lock,
        "a busy queue for a real was not classified as lock");
    require(classifyRealAttempt(true, true, false, CallCopyStatus::NotReady) == InlineCallOutcome::Acquire,
        "a not-ready lower image for a real was not classified as acquire");
    require(classifyRealAttempt(true, true, false, CallCopyStatus::Done) == InlineCallOutcome::Call,
        "a copied real was not served at the call");
    require(classifyRealAttempt(true, true, false, CallCopyStatus::Error) == InlineCallOutcome::Other,
        "a call copy error was not classified as other");
}

// FP1: a fallback counts under its reason, and an output that was never attempted counts as other.
void testFallbackReason() {
    using namespace fgvk::layer;
    require(inlineCallFallbackReason(InlineCallOutcome::None) == InlineCallOutcome::Other,
        "an output never attempted must count as other");
    require(inlineCallFallbackReason(InlineCallOutcome::Lock) == InlineCallOutcome::Lock,
        "a lock fallback changed its reason");
    require(inlineCallServed(InlineCallOutcome::Call) && inlineCallServed(InlineCallOutcome::Parked) &&
            !inlineCallServed(InlineCallOutcome::Budget) && !inlineCallServed(InlineCallOutcome::None),
        "only call and parked outputs are served");
}

// FP2 rule 2: parked images count with the call-held outputs against the budget, so the sum never exceeds it.
void testParkBudget() {
    using namespace fgvk::layer;
    require(inlineCallOutputsHeld(1, 2) == 3, "parked images must count with the call-held outputs");
    require(inlineParkFits(0, 0, 2, 2), "an empty pool must park one image within a budget of two");
    require(inlineParkFits(1, 0, 2, 2), "a second image must park within a budget of two");
    require(!inlineParkFits(2, 0, 2, 2), "a third image must not park with a target of two");
    require(!inlineParkFits(1, 1, 2, 2), "a parked image and a call-held output fill a budget of two");
    require(inlineParkFits(0, 1, 2, 2), "one call-held output and one parked image fill a budget of two");
    require(!inlineParkFits(0, 2, 2, 2), "parking must not exceed the budget");
    require(!inlineParkFits(0, 0, 0, 4), "a zero target parks nothing");
    require(!inlineParkFits(0, 0, 2, 0), "a zero budget parks nothing");
}

// FP2 rule 6 (fixed): a sticky owner drains. A live owner parks only when the target, budget and slot allow it. A park
// target of zero neither parks nor drains, so the parked images stay in the pool for the next acquire.
void testParkAfterPresentAction() {
    using namespace fgvk::layer;
    // FP3: each refusal reports its own reason. Full comes first (a target of zero is always full), then Budget, then SlotBusy.
    require(parkAfterPresentAction(false, 2, 0, 0, 2, false) == ParkOutcome::Full,
        "a park target of zero was not full with images still parked");
    require(parkAfterPresentAction(false, 0, 0, 0, 2, false) == ParkOutcome::Full,
        "a park target of zero parked an image");
    require(parkAfterPresentAction(false, 0, 0, 2, 2, false) == ParkOutcome::Parked,
        "an empty pool did not park within a target of two and a budget of two");
    require(parkAfterPresentAction(false, 1, 0, 2, 2, false) == ParkOutcome::Parked,
        "a second image did not park within a target of two and a budget of two");
    require(parkAfterPresentAction(false, 2, 0, 2, 2, false) == ParkOutcome::Full,
        "a full pool parked a third image");
    require(parkAfterPresentAction(false, 1, 1, 2, 2, false) == ParkOutcome::Budget,
        "a parked image and a call-held output fill a budget of two, but the image was not refused as budget");
    require(parkAfterPresentAction(false, 0, 0, 2, 0, false) == ParkOutcome::Budget,
        "a zero budget parked an image");
    require(parkAfterPresentAction(false, 0, 0, 2, 2, true) == ParkOutcome::SlotBusy,
        "a busy lower slot was not refused as slot busy");
    require(parkAfterPresentAction(false, 0, 1, 2, 1, true) == ParkOutcome::Budget,
        "a busy slot with no budget left was not refused as budget first");
    require(parkAfterPresentAction(true, 2, 0, 2, 2, false) == ParkOutcome::Drained,
        "a sticky owner did not drain its parked images");
    require(parkAfterPresentAction(true, 2, 0, 0, 2, false) == ParkOutcome::Drained,
        "a sticky owner with a target of zero did not drain its parked images");
}

// FP3: each lower acquire poll maps to its park outcome, and only the outcomes an idle wait can clear are retried. Budget,
// Full, Error, Drained and Parked are final for their present. The retry allowance and wait are the ones the worker uses.
void testParkOutcomes() {
    using namespace fgvk::layer;
    require(parkOutcomeOfPoll(LowerPoll::Acquired) == ParkOutcome::Parked, "a successful poll is not a parked image");
    require(parkOutcomeOfPoll(LowerPoll::SlotBusy) == ParkOutcome::SlotBusy, "a busy slot is not slot busy");
    require(parkOutcomeOfPoll(LowerPoll::Fence) == ParkOutcome::Fence, "an unsignalled fence is not fence");
    require(parkOutcomeOfPoll(LowerPoll::Acquire) == ParkOutcome::Acquire, "a not-ready acquire is not acquire");
    require(parkOutcomeOfPoll(LowerPoll::Error) == ParkOutcome::Error, "a poll error is not error");
    require(parkRetryAfter(ParkOutcome::SlotBusy) && parkRetryAfter(ParkOutcome::Fence) &&
        parkRetryAfter(ParkOutcome::Acquire), "a busy slot, an unsignalled fence or a not-ready acquire is not retried");
    require(!parkRetryAfter(ParkOutcome::Parked) && !parkRetryAfter(ParkOutcome::Full) &&
        !parkRetryAfter(ParkOutcome::Budget) && !parkRetryAfter(ParkOutcome::Error) &&
        !parkRetryAfter(ParkOutcome::Drained), "a final park outcome was retried");
    require(inlineParkRetryLimit == 8, "the idle retry allowance is not eight per worker present");
    require(inlineParkRetryWait == std::chrono::microseconds{500}, "the idle retry wait is not 0.5 ms");
    static_assert(parkCountedOutcomeCount == 7 && static_cast<size_t>(ParkOutcome::Drained) == parkCountedOutcomeCount,
        "Drained must be the only outcome left out of the counters");
    const ParkCounts empty{};
    require(empty.retries == 0 && empty.retriesParked == 0 && empty.outcomes[0] == 0,
        "fresh park counts are not zero");
}

// FP2 rules 4 and 5 (fixed): an acquire takes the oldest parked image before a fresh acquire, whatever the park target.
// A sticky owner takes none, because its parked images are drained on the error path.
void testAcquireTakesParkedFirst() {
    using namespace fgvk::layer;
    require(acquireTakesParkedFirst(false, 1) && acquireTakesParkedFirst(false, 2),
        "a parked image was not taken before a fresh acquire");
    require(!acquireTakesParkedFirst(false, 0), "an acquire took a parked image from an empty pool");
    require(!acquireTakesParkedFirst(true, 2), "a sticky owner took a parked image");
}

int main() {
    try {
        testSwitchParse();
        testSubmitOrder();
        testReleaseValue();
        testFailureRoutes();
        testCallBudget();
        testCallHoldFits();
        testGeneratedClassification();
        testRealClassification();
        testFallbackReason();
        testParkBudget();
        testParkAfterPresentAction();
        testParkOutcomes();
        testAcquireTakesParkedFirst();
    } catch (const std::exception& error) {
        std::cerr << "Inline generation test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "inline generation tests passed\n";
    return 0;
}

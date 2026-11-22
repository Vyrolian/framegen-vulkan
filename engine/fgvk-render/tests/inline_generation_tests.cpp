/* SPDX-License-Identifier: GPL-3.0-or-later */

// FI-default: the FGVK_FG_INLINE parse, where only the exact value "0" turns the attached path off.
// FI1: pure helpers of the attached (same-device) generation path: the submit order of the source copy, the release
// value of a batch scheduled after its copy, and where a failed schedule presents from.
// FI2: the call-held lower budget (rule 2) and the hold check.
// FI4: the call path on every worker batch, its spare images, the budget cap, the experiment gate, the park target, the
// one-timeout-per-batch wait rule and the signal each output waits.

#include "swapchain/create_policy.hpp"
#include "swapchain/inline_generation.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

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

    // FI2 rule 2, FI4: the budget is the lower images above the surface minimum and the worker's own holds, capped at the
    // swapchain's call images (outputs per batch).
    void testCallBudget() {
        using fgvk::layer::inlineCallLowerBudget;
        require(inlineCallLowerBudget(8, 4, 2, 4) == 2, "two spare images must give a budget of two");
        require(inlineCallLowerBudget(10, 4, 2, 4) == 4, "four spare images must give the full budget of four");
        require(inlineCallLowerBudget(12, 4, 2, 4) == 4, "more spare images than the cap must still give four");
        require(inlineCallLowerBudget(9, 4, 0, 4) == 4, "a spare count above the cap must be capped with no worker holds");
        require(inlineCallLowerBudget(7, 4, 0, 4) == 3, "the budget must count the images above the surface minimum");
        require(inlineCallLowerBudget(6, 4, 2, 4) == 0, "no spare image must give a budget of zero");
        require(inlineCallLowerBudget(5, 4, 2, 4) == 0, "a negative spare count must give a budget of zero");
        require(inlineCallLowerBudget(0, 0, 0, 4) == 0, "an empty swapchain must give a budget of zero");
        require(inlineCallLowerBudget(10, UINT32_MAX, 0, 4) == 0,
            "a surface minimum above the image count must give a budget of zero");
        // FI5: the call budget reserves the worker's one held lower image, not the create floor's two.
        using fgvk::layer::workerHeldLowerImages;
        require(inlineCallLowerBudget(8, 4, workerHeldLowerImages, 4) == 3,
            "X11/gamescope: surface minimum 4 of 8 leaves three call images");
        require(inlineCallLowerBudget(8, 2, workerHeldLowerImages, 4) == 4,
            "the cap of four is unchanged on Wayland");
        for (size_t lowerImages = 0; lowerImages <= 12; ++lowerImages) {
            for (uint32_t surfaceMin = 0; surfaceMin <= lowerImages; ++surfaceMin) {
                for (uint32_t callImages = 1; callImages <= 8; ++callImages) {
                    const uint32_t budget =
                        inlineCallLowerBudget(lowerImages, surfaceMin, workerHeldLowerImages, callImages);
                    require(budget <= callImages, "the call budget must not exceed the call images");
                    require(budget == 0 || budget + workerHeldLowerImages <= lowerImages - surfaceMin,
                        "call budget plus the worker's hold must stay within S - M");
                }
            }
        }
    }

    // FI4: a 2x batch's cap is two, a 3x batch's three and a 4x batch's four, so spare images above the cap give the cap.
    void testCallBudgetFollowsOutputsPerBatch() {
        using fgvk::layer::inlineCallLowerBudget;
        for (const uint32_t outputs : {2u, 3u, 4u, 5u}) {
            require(inlineCallLowerBudget(4 + 2 + outputs, 4, 2, outputs) == outputs,
                "the budget must equal the outputs per batch when the images allow it");
            require(inlineCallLowerBudget(4 + 2 + outputs + 3, 4, 2, outputs) == outputs,
                "spare images above the outputs per batch must not raise the budget");
            require(inlineCallLowerBudget(4 + 2 + outputs - 1, 4, 2, outputs) == outputs - 1,
                "one image short of the outputs per batch must give one less");
        }
        require(inlineCallLowerBudget(20, 4, 2, 0) == 0, "a swapchain without call images must give a budget of zero");
    }

    // FI4/FI4b: spare images are the outputs per batch at the profile's capacity plus two, and only on a swapchain that runs the worker
    // path. Each zero has one reason, checked in the order off, compat, ordered.
    void testSpareImages() {
        using fgvk::layer::InlineCallImagesReason;
        using fgvk::layer::inlineCallReasonName;
        using fgvk::layer::inlineCallSpareImages;
        for (const size_t multiplier : {2u, 3u, 4u, 5u}) {
            const auto eligible = inlineCallSpareImages(true, true, false, false, multiplier - 1);
            require(eligible.count == multiplier + fgvk::layer::inlineCallHeadroomImages &&
                    eligible.reason == InlineCallImagesReason::Eligible,
                "spare images must be the outputs per batch at the profile's capacity plus the headroom");
        }
        const auto off = inlineCallSpareImages(false, true, false, false, 1);
        require(off.count == 0 && off.reason == InlineCallImagesReason::Off, "the inline switch off must give no spare images");
        const auto unprovisioned = inlineCallSpareImages(true, false, false, false, 1);
        require(unprovisioned.count == 0 && unprovisioned.reason == InlineCallImagesReason::Off,
            "an unprovisioned swapchain must give no spare images");
        const auto noCapacity = inlineCallSpareImages(true, true, false, false, 0);
        require(noCapacity.count == 0 && noCapacity.reason == InlineCallImagesReason::Off,
            "a profile with no generated capacity must give no spare images, whatever the provisioning flag says");
        const auto compat = inlineCallSpareImages(true, true, false, true, 1);
        require(compat.count == 0 && compat.reason == InlineCallImagesReason::Compat,
            "a swapchain with the application's image count kept must give no spare images");
        const auto ordered = inlineCallSpareImages(true, true, true, false, 1);
        require(ordered.count == 0 && ordered.reason == InlineCallImagesReason::Ordered,
            "an ordered FIFO transport never runs the call path and must give no spare images");
        const auto both = inlineCallSpareImages(true, true, true, true, 1);
        require(both.reason == InlineCallImagesReason::Compat, "compat must be named before ordered");
        require(!inlineCallSpareImages(false, false, true, true, 4).count, "no path may give spare images without a switch");
        require(std::string_view(inlineCallReasonName(InlineCallImagesReason::Eligible)) == "eligible" &&
                std::string_view(inlineCallReasonName(InlineCallImagesReason::Ordered)) == "ordered" &&
                std::string_view(inlineCallReasonName(InlineCallImagesReason::Compat)) == "compat" &&
                std::string_view(inlineCallReasonName(InlineCallImagesReason::Off)) == "off" &&
                std::string_view(inlineCallReasonName(InlineCallImagesReason::Fallback)) == "fallback",
            "the reason names must match the create log line");
        // FI4c: a fallback swapchain has no call images, so its budget is zero at every batch size and surface minimum.
        const fgvk::layer::InlineCallImages fallback{0, InlineCallImagesReason::Fallback};
        require(fallback.count == 0 && fgvk::layer::inlineCallLowerBudget(8, 4, 2, fallback.count) == 0,
            "a fallback swapchain must keep the worker copy only");
    }

    // FI4: the call path is taken by every worker batch with a budget, on every surface, mode and multiplier. The helper
    // takes no surface, mode or multiplier, so those cannot refuse it. The budget and the attachment do.
    void testCallBatchEligibility() {
        using fgvk::layer::inlineCallBatchEligible;
        for (const uint32_t outputs : {2u, 3u, 4u, 5u}) {
            require(inlineCallBatchEligible(true, true, true, false, 4 + 2 + outputs, 4, 2, outputs),
                "a worker batch with its call images must take the call path at every multiplier");
        }
        require(inlineCallBatchEligible(true, true, true, false, 8, 4, 2, 2),
            "a worker batch on a swapchain with two spare images must be eligible");
        require(!inlineCallBatchEligible(false, true, true, false, 8, 4, 2, 2),
            "a batch of a detached backend must not take the call path");
        require(!inlineCallBatchEligible(true, false, true, false, 8, 4, 2, 2),
            "a batch the worker does not copy must not take the call path");
        require(!inlineCallBatchEligible(true, true, false, false, 8, 4, 2, 2),
            "a batch without internal images must not take the call path");
        require(!inlineCallBatchEligible(true, true, true, true, 8, 4, 2, 2),
            "a batch whose copies are on the transfer family must not take the call path");
        require(!inlineCallBatchEligible(true, true, true, false, 8, 0, 2, 2),
            "an unknown surface minimum must not take the call path");
        require(!inlineCallBatchEligible(true, true, true, false, 6, 4, 2, 2),
            "a swapchain without spare images must not take the call path");
        require(!inlineCallBatchEligible(true, true, true, false, 8, 4, 2, 0),
            "a swapchain with no call images (ordered, compat or off) must not take the call path");
    }

    // FI4: the experiments keep their gate. Native Wayland, IMMEDIATE and one output are the only batches that pass it.
    void testExperimentGate() {
        using fgvk::layer::nativeImmediateTwoXBatch;
        using fgvk::layer::SurfaceOrigin;
        require(nativeImmediateTwoXBatch(true, true, 1, SurfaceOrigin::Wayland, VK_PRESENT_MODE_IMMEDIATE_KHR),
            "the experiment gate must admit a native Wayland IMMEDIATE one-output batch");
        require(!nativeImmediateTwoXBatch(true, true, 1, SurfaceOrigin::Wayland, VK_PRESENT_MODE_FIFO_KHR),
            "the experiment gate must refuse FIFO (the mode term)");
        require(!nativeImmediateTwoXBatch(true, true, 1, SurfaceOrigin::Wayland, VK_PRESENT_MODE_MAILBOX_KHR),
            "the experiment gate must refuse MAILBOX");
        require(!nativeImmediateTwoXBatch(true, true, 1, SurfaceOrigin::Wayland, VK_PRESENT_MODE_FIFO_LATEST_READY_KHR),
            "the experiment gate must refuse FIFO_LATEST_READY");
        require(!nativeImmediateTwoXBatch(true, true, 1, SurfaceOrigin::Xcb, VK_PRESENT_MODE_IMMEDIATE_KHR),
            "the experiment gate must refuse X11 (the surface term)");
        require(!nativeImmediateTwoXBatch(true, true, 2, SurfaceOrigin::Wayland, VK_PRESENT_MODE_IMMEDIATE_KHR),
            "the experiment gate must refuse a batch of two outputs (the 2x term)");
        require(!nativeImmediateTwoXBatch(false, true, 1, SurfaceOrigin::Wayland, VK_PRESENT_MODE_IMMEDIATE_KHR),
            "the experiment gate must refuse a batch the worker does not copy");
        require(!nativeImmediateTwoXBatch(true, false, 1, SurfaceOrigin::Wayland, VK_PRESENT_MODE_IMMEDIATE_KHR),
            "the experiment gate must refuse a batch without internal images");
    }

    // FI4: a batch parks one lower image per output, the generated outputs and the real.
    void testBatchOutputs() {
        using fgvk::layer::inlineBatchOutputs;
        require(inlineBatchOutputs(1) == 2, "a 2x batch has two outputs: one generated and the real");
        require(inlineBatchOutputs(2) == 3, "a 3x batch has three outputs");
        require(inlineBatchOutputs(3) == 4, "a 4x batch has four outputs");
        require(inlineBatchOutputs(4) == 5, "a 5x batch has five outputs");
    }

    // FI4: the park target of an eligible batch is its outputs, so a park fits within the budget at every multiplier.
    void testParkTargetFollowsOutputs() {
        using namespace fgvk::layer;
        for (const uint32_t outputs : {2u, 3u, 4u, 5u}) {
            require(parkAfterPresentAction(false, 0, 0, outputs, outputs, false) == ParkOutcome::Parked,
                "an empty pool did not park within a target and budget of the outputs per batch");
            require(parkAfterPresentAction(false, outputs - 1, 0, outputs, outputs, false) == ParkOutcome::Parked,
                "the pool did not fill to the outputs per batch");
            require(parkAfterPresentAction(false, outputs, 0, outputs, outputs, false) == ParkOutcome::Full,
                "a pool of the outputs per batch parked one more");
        }
    }

    // FI4 with FP4: at most one call wait per batch times out. Only a timeout ends the batch's waits, and a wait that
    // returns None or a parked or unlocked result never does.
    void testCallWaitOncePerBatch() {
        using namespace fgvk::layer;
        require(inlineCallWaitAllowed(false), "the first call wait of a batch must be allowed");
        require(!inlineCallWaitAllowed(true), "a call wait after a timed-out wait of the batch must be skipped");
        require(inlineCallWaitBatchTimedOut(false, CallLowerWait::Timeout),
            "a timed-out call wait must end the batch's waits");
        require(!inlineCallWaitBatchTimedOut(false, CallLowerWait::Parked) &&
                !inlineCallWaitBatchTimedOut(false, CallLowerWait::Unlocked) &&
                !inlineCallWaitBatchTimedOut(false, CallLowerWait::None),
            "a parked, unlocked or unwaited call must keep the batch's waits allowed");
        require(inlineCallWaitBatchTimedOut(true, CallLowerWait::Parked),
            "a timeout must stay recorded for the rest of the batch");
    }

    // FI4: each output waits what its predecessor signalled. A worker copy is followed by a copied signal, a direct or
    // present-thread copy by the post-copy chain, and the first output waits nothing.
    void testChainSignalBefore() {
        using namespace fgvk::layer;
        require(chainSignalBefore(true, false) == ChainSignal::None && chainSignalBefore(true, true) == ChainSignal::None,
            "the first output must wait no predecessor");
        require(chainSignalBefore(false, true) == ChainSignal::Copied,
            "an output after a worker copy must wait that copy's copied signal");
        require(chainSignalBefore(false, false) == ChainSignal::PostCopy,
            "an output after a direct or present-thread copy must wait the post-copy chain");
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

// FP3: each lower acquire poll maps to its park outcome. FP4 removed the idle retry, so no outcome is retried any more.
void testParkOutcomes() {
    using namespace fgvk::layer;
    require(parkOutcomeOfPoll(LowerPoll::Acquired) == ParkOutcome::Parked, "a successful poll is not a parked image");
    require(parkOutcomeOfPoll(LowerPoll::SlotBusy) == ParkOutcome::SlotBusy, "a busy slot is not slot busy");
    require(parkOutcomeOfPoll(LowerPoll::Fence) == ParkOutcome::Fence, "an unsignalled fence is not fence");
    require(parkOutcomeOfPoll(LowerPoll::Acquire) == ParkOutcome::Acquire, "a not-ready acquire is not acquire");
    require(parkOutcomeOfPoll(LowerPoll::Error) == ParkOutcome::Error, "a poll error is not error");
    static_assert(parkCountedOutcomeCount == 7 && static_cast<size_t>(ParkOutcome::Drained) == parkCountedOutcomeCount,
        "Drained must be the only outcome left out of the counters");
    const ParkCounts empty{};
    require(empty.outcomes[0] == 0 && empty.outcomes.back() == 0, "fresh park counts are not zero");
}

// FP4: the call wait is bounded at 0.5 ms, and its counts start at zero. The wait itself is tested in deferred_present_tests.
void testCallLowerWaitBounds() {
    using namespace fgvk::layer;
    require(inlineCallLowerWait == std::chrono::microseconds{500}, "the call wait for the worker's present is not 0.5 ms");
    const CallWaitCounts empty{};
    require(empty.waits == 0 && empty.parked == 0 && empty.unlocked == 0 && empty.timeout == 0 && empty.waitUs == 0,
        "fresh call wait counts are not zero");
    require(CallLowerWait::None != CallLowerWait::Parked && CallLowerWait::Unlocked != CallLowerWait::Timeout,
        "the call wait outcomes are not distinct");
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
        testCallBudgetFollowsOutputsPerBatch();
        testSpareImages();
        testCallBatchEligibility();
        testExperimentGate();
        testBatchOutputs();
        testParkTargetFollowsOutputs();
        testCallWaitOncePerBatch();
        testChainSignalBefore();
        testCallHoldFits();
        testGeneratedClassification();
        testRealClassification();
        testFallbackReason();
        testParkBudget();
        testParkAfterPresentAction();
        testParkOutcomes();
        testCallLowerWaitBounds();
        testAcquireTakesParkedFirst();
    } catch (const std::exception& error) {
        std::cerr << "Inline generation test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "inline generation tests passed\n";
    return 0;
}

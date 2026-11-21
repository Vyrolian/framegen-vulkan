/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace fgvk::layer {

// FI1: frame generation attaches to the application's device and queue, on by default. FGVK_FG_INLINE=0 turns it off
// and keeps the separate private device, with the descriptor backend and its queues as they were. Only the exact value
// "0" is off: unset (null), empty and every other value attach.
[[nodiscard]] constexpr bool parseInlineGenerationSwitch(const char* value) noexcept {
    return !(value && std::string_view(value) == "0");
}

// Read once per process.
[[nodiscard]] inline bool inlineGenerationRequested() noexcept {
    static const bool requested = parseInlineGenerationSwitch(std::getenv("FGVK_FG_INLINE"));
    return requested;
}

// An attached backend records on the application's queue, so the source copy is submitted before its batch. A batch
// that waits on a timeline value the copy has not signalled yet would block that queue. A detached copy follows its batch.
[[nodiscard]] constexpr bool sourceCopyPrecedesSchedule(const bool attached) noexcept {
    return attached;
}

// Release value of a batch scheduled after its source copy. The copy advanced the timeline index past its own value S,
// so the index is S + 1 here, and the batch signals S + 1 .. S + G for G outputs. The release value is S + G.
[[nodiscard]] constexpr uint64_t attachedSourceReleaseValue(const uint64_t timelineIndexAfterCopy,
        const size_t scheduledCount) noexcept {
    return timelineIndexAfterCopy - 1 + scheduledCount;
}

// Where a failed schedule presents from. An attached copy has already consumed the application's wait semaphores, so
// it presents through the copy's post-copy semaphore only. A detached failure happens before any copy, so today's
// native routes apply unchanged.
enum class ScheduleFailureRoute : uint8_t {
    NativeOnly,            // detached: present the application image natively
    NativeWithRetirement,  // detached: retire preacquired images, waiting on the application's semaphores
    PostCopyOriginal,      // attached: present the original image through the copy's post-copy semaphore
    PostCopyRetirement,    // attached: retire preacquired images, waiting on the copy's post-copy semaphore
};

[[nodiscard]] constexpr ScheduleFailureRoute scheduleFailureRoute(const bool attached,
        const bool preacquiredRetirement) noexcept {
    if (attached)
        return preacquiredRetirement ? ScheduleFailureRoute::PostCopyRetirement
                                     : ScheduleFailureRoute::PostCopyOriginal;
    return preacquiredRetirement ? ScheduleFailureRoute::NativeWithRetirement
                                 : ScheduleFailureRoute::NativeOnly;
}

// FI2: most lower images that copies made at the present call may hold, on top of the worker's own holds.
inline constexpr uint32_t inlineCallLowerImageCap = 4;

// FI2: lower images an attached batch may hold at its present call. These are the images above the surface minimum
// that the worker's own holds leave, capped at inlineCallLowerImageCap. Zero when there are none.
[[nodiscard]] constexpr uint32_t inlineCallLowerBudget(const size_t lowerImages, const uint32_t surfaceMinImages,
        const uint32_t workerHeldImages) noexcept {
    const int64_t spare = static_cast<int64_t>(lowerImages) - static_cast<int64_t>(surfaceMinImages) -
        static_cast<int64_t>(workerHeldImages);
    if (spare <= 0)
        return 0;
    return static_cast<uint32_t>(std::min<int64_t>(spare, inlineCallLowerImageCap));
}

// FI2: one more output may be held at the call when the outputs already held, plus this one, stay within the budget.
// A batch's generated output is checked against the held count before it. Its real output is checked against the
// count after the generated output, so the real needs held + 2 <= budget with the held count from before the batch.
[[nodiscard]] constexpr bool inlineCallHoldFits(const size_t heldOutputs, const uint32_t budget) noexcept {
    return heldOutputs + 1 <= budget;
}

// FP1: where one output of an attached, eligible batch ended at its present call. Call and Parked are served by the
// game queue (Parked from an image the worker parked). The other values are fallbacks to the worker copy, each with one
// reason, so the five reasons sum to the fallback count. None is no attempt: the output was not routed to the call.
enum class InlineCallOutcome : uint8_t { None, Call, Parked, Budget, Lock, Acquire, Cascade, Other };

[[nodiscard]] constexpr bool inlineCallServed(const InlineCallOutcome outcome) noexcept {
    return outcome == InlineCallOutcome::Call || outcome == InlineCallOutcome::Parked;
}

// FP1: the reason an unserved output counts under. An output that was never attempted counts as other.
[[nodiscard]] constexpr InlineCallOutcome inlineCallFallbackReason(const InlineCallOutcome outcome) noexcept {
    return outcome == InlineCallOutcome::None || inlineCallServed(outcome) ? InlineCallOutcome::Other : outcome;
}

// FP1 rule 2, a generated output that was not parked. budgetFits is the budget check, lockMissed a failed queue try-lock,
// acquired a successful lower acquire, and notReady its NOT_READY or TIMEOUT. A budget refusal wins over the lock, and an
// error that is not a not-ready result is other.
[[nodiscard]] constexpr InlineCallOutcome classifyGeneratedAttempt(const bool budgetFits, const bool lockMissed,
        const bool acquired, const bool notReady) noexcept {
    if (!budgetFits) return InlineCallOutcome::Budget;
    if (lockMissed) return InlineCallOutcome::Lock;
    if (acquired) return InlineCallOutcome::Call;
    if (notReady) return InlineCallOutcome::Acquire;
    return InlineCallOutcome::Other;
}

// FP1: the result of one call-side copy attempt. NotReady leaves the image for the worker and is an acquire fallback;
// Error (a refusal, a missing owner, an acquire or submit error) is other.
enum class CallCopyStatus : uint8_t { Done, NotReady, Error };

// FP1 rule 2, a real output. A real whose generated output fell back is a cascade and is never attempted.
[[nodiscard]] constexpr InlineCallOutcome classifyRealAttempt(const bool eligible, const bool budgetFits,
        const bool lockMissed, const CallCopyStatus status) noexcept {
    if (!eligible) return InlineCallOutcome::Cascade;
    if (!budgetFits) return InlineCallOutcome::Budget;
    if (lockMissed) return InlineCallOutcome::Lock;
    switch (status) {
    case CallCopyStatus::Done: return InlineCallOutcome::Call;
    case CallCopyStatus::NotReady: return InlineCallOutcome::Acquire;
    case CallCopyStatus::Error: break;
    }
    return InlineCallOutcome::Other;
}

// FP2: the lower images an attached batch's outputs hold at the call: the call-held outputs (direct generated outputs and
// call-prepared reals of the worker's batches) plus the parked images. The budget bounds this sum.
[[nodiscard]] constexpr size_t inlineCallOutputsHeld(const size_t callHeld, const size_t parked) noexcept {
    return callHeld + parked;
}

// FP2 rule 2: the worker may park one more lower image after a lower present when fewer than target images are parked
// and the call-held outputs plus the parked images, with this one, stay within the budget.
[[nodiscard]] constexpr bool inlineParkFits(const size_t parked, const size_t callHeld, const uint32_t target,
        const uint32_t budget) noexcept {
    return parked < target && inlineCallHoldFits(inlineCallOutputsHeld(callHeld, parked), budget);
}

// FP2 rule 6: what parkAfterPresent does after a lower present of the worker. A sticky owner drains its parked images, on
// the error path only. Otherwise one image is parked when the target and budget allow it and its slot is free. A target of
// zero parks nothing and drains nothing, so the parked images stay in the pool for the next acquire to take.
enum class ParkAction : uint8_t { Nothing, Park, Drain };

[[nodiscard]] constexpr ParkAction parkAfterPresentAction(const bool sticky, const size_t parked, const size_t callHeld,
        const uint32_t target, const uint32_t budget, const bool slotBusy) noexcept {
    if (sticky) return ParkAction::Drain;
    if (slotBusy || !inlineParkFits(parked, callHeld, target, budget)) return ParkAction::Nothing;
    return ParkAction::Park;
}

// FP2 rules 4 and 5: an acquire (the worker's, or a synchronous materialize) takes the oldest parked image before it
// acquires a fresh one, whatever the park target is. A sticky owner takes none: its parked images are drained instead.
[[nodiscard]] constexpr bool acquireTakesParkedFirst(const bool sticky, const size_t parked) noexcept {
    return !sticky && parked > 0;
}

// FP2: parked images per swapchain while the attached batch is eligible (one generated output and one real).
inline constexpr uint32_t inlineParkTarget = 2;

}  // namespace fgvk::layer

/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "swapchain/surface_origin.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include <vulkan/vulkan_core.h>

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

// FI4: the lower images the swapchain is created with for the call path. These are the outputs of one batch at the
// profile's capacity (generated outputs plus the real). Off when the inline switch, frame-generation provisioning or a
// profile capacity is missing; compat when the application's image count is kept as asked; ordered for FIFO-only ordered
// transport, whose batches may still reach the worker but get no call images, so they keep the worker copy. FI4c:
// fallback when the lower create succeeded only on a retry (the application's count, and possibly without the owned
// timing bits), which has no spare images, so every batch keeps the worker copy.
enum class InlineCallImagesReason : uint8_t { Off, Compat, Ordered, Eligible, Fallback };

struct InlineCallImages {
    uint32_t count{};
    InlineCallImagesReason reason{InlineCallImagesReason::Off};
};

// FI4b: images on top of the outputs per batch. A call-copied image stays acquired until the worker presents it at its
// paced time, so the call copies the next batch while the last one still waits: two batches in flight at 2×. FI4 run
// 2026-11-22 12:41 (2× IMMEDIATE, outputs only): 3408 budget fallbacks of 5460 and 84.85 FPS, against 40 and 86.13 FPS
// with 4 images (FI2). Two more restore FI2 at 2×.
inline constexpr uint32_t inlineCallHeadroomImages = 2;

[[nodiscard]] constexpr InlineCallImages inlineCallSpareImages(const bool switchOn, const bool provisioned,
        const bool orderedTransport, const bool countCompatibility, const size_t generatedCapacity) noexcept {
    // A zero capacity is an unprovisioned profile (generatedFrameCapacityForProfile): it has no generated outputs to hold.
    if (!switchOn || !provisioned || generatedCapacity == 0)
        return {0, InlineCallImagesReason::Off};
    if (countCompatibility)
        return {0, InlineCallImagesReason::Compat};
    if (orderedTransport)
        return {0, InlineCallImagesReason::Ordered};
    return {static_cast<uint32_t>(generatedCapacity + 1 + inlineCallHeadroomImages), InlineCallImagesReason::Eligible};
}

// FI4: the reason name of the swapchain create log line.
[[nodiscard]] constexpr const char* inlineCallReasonName(const InlineCallImagesReason reason) noexcept {
    switch (reason) {
    case InlineCallImagesReason::Off: return "off";
    case InlineCallImagesReason::Compat: return "compat";
    case InlineCallImagesReason::Ordered: return "ordered";
    case InlineCallImagesReason::Eligible: return "eligible";
    case InlineCallImagesReason::Fallback: return "fallback";
    }
    return "off";
}

// FI2: lower images an attached batch may hold at its present call. These are the images above the surface minimum
// that the worker's own holds leave, capped at callImages (FI4: the swapchain's outputs per batch, see InlineCallImages).
// Zero when there are none.
[[nodiscard]] constexpr uint32_t inlineCallLowerBudget(const size_t lowerImages, const uint32_t surfaceMinImages,
        const uint32_t workerHeldImages, const uint32_t callImages) noexcept {
    const int64_t spare = static_cast<int64_t>(lowerImages) - static_cast<int64_t>(surfaceMinImages) -
        static_cast<int64_t>(workerHeldImages);
    if (spare <= 0)
        return 0;
    return static_cast<uint32_t>(std::min<int64_t>(spare, callImages));
}

// FI4: a batch takes the call path when the worker copies it (every surface, present mode and multiplier), it is
// attached to the application's device with internal images, its copies are not on the transfer family, and the
// swapchain has a call budget. The budget is zero without spare images, so no output waits for a park that cannot come.
[[nodiscard]] constexpr bool inlineCallBatchEligible(const bool attached, const bool workerCopies,
        const bool internalPresent, const bool transferCopies, const size_t lowerImages,
        const uint32_t surfaceMinImages, const uint32_t workerHeldImages, const uint32_t callImages) noexcept {
    return attached && workerCopies && internalPresent && !transferCopies && surfaceMinImages != 0 &&
        inlineCallLowerBudget(lowerImages, surfaceMinImages, workerHeldImages, callImages) > 0;
}

// FI4: the gate of the opt-in non-inline experiments (FGVK_DIRECT_GENERATED_COPY and FGVK_SAME_QUEUE_GENERATED_COPY). It is
// unchanged: a worker batch of one generated output on a native Wayland surface in IMMEDIATE mode.
[[nodiscard]] constexpr bool nativeImmediateTwoXBatch(const bool workerCopies, const bool internalPresent,
        const size_t generatedOutputs, const SurfaceOrigin origin, const VkPresentModeKHR mode) noexcept {
    return workerCopies && internalPresent && generatedOutputs == 1 && origin == SurfaceOrigin::Wayland &&
        mode == VK_PRESENT_MODE_IMMEDIATE_KHR;
}

// FI4: the outputs of a batch, its generated outputs and its real. An eligible batch parks this many lower images.
[[nodiscard]] constexpr uint32_t inlineBatchOutputs(const size_t generatedOutputs) noexcept {
    return static_cast<uint32_t>(generatedOutputs + 1);
}

// FI6: the call image cap of one batch. A live multiplier switch does not recreate the swapchain, so the create-time count
// keeps the outputs of the multiplier it was created for. The cap follows the current batch's outputs, and the create-time
// count stays the floor. A swapchain without a call path (createdCount 0) has no cap. inlineCallLowerBudget still bounds the
// cap by the images above the surface minimum and the worker's hold, so it never exceeds the Vulkan forward-progress bound.
[[nodiscard]] constexpr uint32_t inlineCallImagesForBatch(const uint32_t createdCount, const uint32_t batchOutputs) noexcept {
    if (createdCount == 0)
        return 0;
    return std::max(createdCount, batchOutputs + inlineCallHeadroomImages);
}

// FI4: the signal that output i waits for before its copy. A worker copy or skip signals its copied semaphore, every other
// output (a direct call copy or a present-thread copy) signals the post-copy chain. Each output waits what its predecessor
// signalled, so the two chains join at any boundary between kinds.
enum class ChainSignal : uint8_t { None, Copied, PostCopy };

[[nodiscard]] constexpr ChainSignal chainSignalBefore(const bool first, const bool previousWorker) noexcept {
    if (first)
        return ChainSignal::None;
    return previousWorker ? ChainSignal::Copied : ChainSignal::PostCopy;
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

// FP3: why one lower park did or did not park. Parked is the image parked. Full: the parked images reached the target (a
// target of zero is always full). Budget: the call-held outputs and the parked images, with one more, exceed the budget.
// SlotBusy: the next lower-acquire slot is busy. Fence: its fence is not signalled. Acquire: the lower acquire was NOT_READY
// or TIMEOUT. Error: any other acquire error. Drained: the owner drained its parked images on the sticky error path, or no
// park was attempted (the worker is stopping or has no owner). Drained is never counted.
enum class ParkOutcome : uint8_t { Parked, Full, Budget, SlotBusy, Fence, Acquire, Error, Drained };

// FP2 rule 6, with the FP3 reason: what parkAfterPresent does after a lower present of the worker. A sticky owner drains its
// parked images, on the error path only. Otherwise one image is parked when the target and budget allow it and its slot is
// free. A target of zero parks nothing and drains nothing, so the parked images stay in the pool for the next acquire to
// take. Parked is the decision to park: the lower acquire that follows may still end as Fence, Acquire or Error.
[[nodiscard]] constexpr ParkOutcome parkAfterPresentAction(const bool sticky, const size_t parked, const size_t callHeld,
        const uint32_t target, const uint32_t budget, const bool slotBusy) noexcept {
    if (sticky) return ParkOutcome::Drained;
    if (parked >= target) return ParkOutcome::Full;
    if (!inlineCallHoldFits(inlineCallOutputsHeld(callHeld, parked), budget)) return ParkOutcome::Budget;
    if (slotBusy) return ParkOutcome::SlotBusy;
    return ParkOutcome::Parked;
}

// FP2 rules 4 and 5: an acquire (the worker's, or a synchronous materialize) takes the oldest parked image before it
// acquires a fresh one, whatever the park target is. A sticky owner takes none: its parked images are drained instead.
[[nodiscard]] constexpr bool acquireTakesParkedFirst(const bool sticky, const size_t parked) noexcept {
    return !sticky && parked > 0;
}

// FP3: why a zero-timeout lower acquire under queue->mutex returned no image (InternalSwapchainImages::tryAcquireLowerLocked).
// Acquired is success. SlotBusy: the next lower-acquire slot is held by a parked image or a call. Fence: its fence is not
// signalled yet. Acquire: the driver returned NOT_READY or TIMEOUT. Error: any other result.
enum class LowerPoll : uint8_t { Acquired, SlotBusy, Fence, Acquire, Error };

// FP3: the park outcome of a lower acquire that returned no image. Acquired is the successful park.
[[nodiscard]] constexpr ParkOutcome parkOutcomeOfPoll(const LowerPoll poll) noexcept {
    switch (poll) {
    case LowerPoll::Acquired: return ParkOutcome::Parked;
    case LowerPoll::SlotBusy: return ParkOutcome::SlotBusy;
    case LowerPoll::Fence: return ParkOutcome::Fence;
    case LowerPoll::Acquire: return ParkOutcome::Acquire;
    case LowerPoll::Error: break;
    }
    return ParkOutcome::Error;
}

// FP3: the outcomes the park counters keep, indexed by ParkOutcome. Drained is last and is not kept.
inline constexpr size_t parkCountedOutcomeCount = static_cast<size_t>(ParkOutcome::Drained);

// FP3: the park counters taken from the worker for one window. outcomes counts every park attempt by outcome.
struct ParkCounts {
    std::array<size_t, parkCountedOutcomeCount> outcomes{};
};

// FP4: what DeferredPresent::awaitCallLower found. None: no wait, because an image was already parked or the queue lock was
// free at once. Parked: an image was parked during the wait. Unlocked: the queue lock was free during the wait, so the
// worker's present and its park were over. Timeout: neither happened within inlineCallLowerWait.
enum class CallLowerWait : uint8_t { None, Parked, Unlocked, Timeout };

// FP4: the longest wait at a present call for the worker's present of a lower image. The FP measurements put that present at
// p50 0.105 ms and p99 0.52 ms, so this wait covers almost every present that the call collides with.
inline constexpr std::chrono::microseconds inlineCallLowerWait{500};

// FP4: the call waits taken from the worker for one window, generated and real together. waits counts the waits that did not
// return None, each outcome is counted in its own field, and waitUs is the sum of their durations.
struct CallWaitCounts {
    size_t waits{};
    size_t parked{};
    size_t unlocked{};
    size_t timeout{};
    size_t waitUs{};
};

// XH2a: the worker's polls of a real for a lower image that found none, taken for one window, by LowerPoll reason (Acquired is
// the success and is not counted). waitMaxUs is the longest wait of a worker real, from its first poll that found none to the
// poll that ended it, in us. The window keeps the maximum of the takes it is given.
struct WorkerRealPollCounts {
    size_t slotBusy{};
    size_t fence{};
    size_t acquire{};
    size_t error{};
    size_t waitMaxUs{};
};

// PT1: the relative application targets the worker forwarded and presented, taken for one window. real counts the reals whose
// target went through the batch split (zero generated outputs keep the whole target), generated counts the generated outputs
// that carried their share. The last targets are in ns, from the last take that forwarded one of that kind.
struct TimingRelativeCounts {
    size_t real{};
    size_t generated{};
    uint64_t realLastNs{};
    uint64_t generatedLastNs{};
};

// FI4, with FP4: at most one call wait times out per batch. A call output may wait only while no earlier wait of its batch
// timed out, so later outputs of that batch take the poll and the worker fallback as they do without a wait.
[[nodiscard]] constexpr bool inlineCallWaitAllowed(const bool batchTimedOut) noexcept {
    return !batchTimedOut;
}

// FI4: the batch's timeout state after one wait of that batch. A Timeout ends the batch's waits, and so does a timeout before.
[[nodiscard]] constexpr bool inlineCallWaitBatchTimedOut(const bool batchTimedOut, const CallLowerWait wait) noexcept {
    return batchTimedOut || wait == CallLowerWait::Timeout;
}

}  // namespace fgvk::layer

/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace fgvk::layer {

// FI1: FGVK_FG_INLINE=1 attaches frame generation to the application's device and queue. Read once per process.
// Unset or any other value changes nothing: the descriptor backend and its queues stay as they are.
[[nodiscard]] inline bool inlineGenerationRequested() noexcept {
    static const bool requested = [] {
        const auto* value = std::getenv("FGVK_FG_INLINE");
        return value && std::string_view(value) == "1";
    }();
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

}  // namespace fgvk::layer

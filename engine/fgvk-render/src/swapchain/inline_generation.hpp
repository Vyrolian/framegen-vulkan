/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

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

}  // namespace fgvk::layer

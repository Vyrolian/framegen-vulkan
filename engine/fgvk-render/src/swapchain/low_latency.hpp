/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace fgvk::layer {

// Just-in-time release (the low_latency profile setting). A game without a frame
// limit may start its next frame as soon as its previous release point arrives,
// so a three-frame queue starts frames about two cap intervals before they are
// needed. Every release point of a capped swapchain is moved later by delay().
//
// lead = limiterDeadline - limiterArrival of a source present at the cap. The
// delay moves by an eighth of (lead - margin) in both directions, so it settles
// where presents arrive one margin early; a larger gain oscillates on a
// three-frame queue, whose answer to a change comes three frames later. The
// margin is 1 ms plus three times the mean frame-to-frame change of the lead, so
// a jittery game keeps more slack and rarely arrives late. The delay stays within
// [0, 3 cap intervals]. Without a cap, or with frame generation off, the caller
// resets it to zero.
class LowLatencyRelease {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;

    static constexpr Duration baseMargin = std::chrono::milliseconds{1};

    constexpr void observe(Duration lead, Duration capInterval) noexcept {
        if (capInterval <= Duration::zero()) {
            reset();
            return;
        }
        if (hasPrevious) {
            const Duration change = lead > previousLead ? lead - previousLead : previousLead - lead;
            leadChange += (std::min(change, capInterval) - leadChange) / 16;
        }
        previousLead = lead;
        hasPrevious = true;
        extraDelay += (lead - margin()) / 8;
        extraDelay = std::clamp(extraDelay, Duration::zero(), 3 * capInterval);
    }
    [[nodiscard]] constexpr Duration margin() const noexcept { return baseMargin + 3 * leadChange; }
    constexpr void reset() noexcept { *this = LowLatencyRelease{}; }
    [[nodiscard]] constexpr Duration delay() const noexcept { return extraDelay; }

private:
    Duration extraDelay{};
    Duration previousLead{};
    Duration leadChange{};
    bool hasPrevious{false};
};

// Native acquire fallback: how long an acquire waits for the release point of the
// swapchain's last capped present. `wait` is the time to sleep. `expired` means the
// point lies past the caller's timeout: the acquire then reports VK_TIMEOUT after
// sleeping `wait` (the timeout). A zero timeout is a poll and is never held.
struct AcquireHold {
    LowLatencyRelease::Duration wait{};
    bool expired{};
};

[[nodiscard]] constexpr AcquireHold acquireHold(LowLatencyRelease::Clock::time_point now,
        LowLatencyRelease::Clock::time_point release, uint64_t timeout) noexcept {
    using Duration = LowLatencyRelease::Duration;
    if (timeout == 0 || release <= now) return {};
    const Duration remaining = release - now;
    if (timeout == UINT64_MAX) return {remaining, false};
    // Timeouts of an hour or more are not bounded here, as in the release waits.
    const Duration limit = timeout >= uint64_t{3'600'000'000'000}
        ? Duration::max() : std::chrono::nanoseconds{static_cast<int64_t>(timeout)};
    return {std::min(remaining, limit), remaining > limit};
}

}

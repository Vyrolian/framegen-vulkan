/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace fgvk::layer {

    /// Advances the render-slot ring by one slot. Depth one keeps every batch
    /// in slot zero, so the same slot is reused every time.
    [[nodiscard]] constexpr std::size_t nextRenderSlot(
            const std::size_t index, const std::size_t depth) noexcept {
        return (index + 1) % depth;
    }

    /// Timeline path: a slot that is still in flight must be waited for before
    /// reuse. A timeout leaves the slot in flight, so the caller keeps its ring
    /// index and fence unchanged. An idle slot never calls wait.
    template <class Wait>
    [[nodiscard]] bool admitRenderSlot(bool& inFlight,
            const uint64_t budgetNs, Wait&& wait) {
        if (!inFlight)
            return true;
        if (!wait(budgetNs))
            return false;
        inFlight = false;
        return true;
    }

    /// CPU drain path: every in-flight slot shares one deadline, so the first
    /// wait spends part of the budget that later waits see. Each signalled slot
    /// is cleared as it is observed. The first timeout stops the drain and
    /// leaves later flags set. waitedForInFlight reports whether any slot was
    /// in flight, whether or not its wait succeeded.
    template <class Flags, class Wait, class Now>
    [[nodiscard]] bool drainRenderSlots(Flags& inFlight,
            const std::chrono::nanoseconds budget,
            Wait&& wait, Now&& now, bool& waitedForInFlight) {
        waitedForInFlight = false;
        const auto deadline = now() + budget;
        for (std::size_t index = 0; index < inFlight.size(); ++index) {
            if (!inFlight[index])
                continue;
            waitedForInFlight = true;
            const auto current = now();
            const uint64_t remainingNs = deadline > current
                ? static_cast<uint64_t>(std::chrono::duration_cast<
                    std::chrono::nanoseconds>(deadline - current).count())
                : 0;
            if (!wait(index, remainingNs))
                return false;
            inFlight[index] = false;
        }
        return true;
    }
}

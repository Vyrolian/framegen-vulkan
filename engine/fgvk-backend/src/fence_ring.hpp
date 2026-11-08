/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fgvk::backend {

    /// prepareWork's fence scan. Unscheduled slots are skipped. Only the
    /// current ring slot is required unless waitAll is set. One budget is
    /// shared by every wait through the remainingNs reference. The first
    /// timeout ends the scan without touching later slots. signalled reports
    /// the slots observed complete by this call.
    template <std::size_t N, class Wait>
    [[nodiscard]] bool waitScheduledFences(const std::array<bool, N>& scheduled,
            const std::size_t depth, const std::size_t current, const bool waitAll,
            const uint64_t budgetNs, Wait&& wait, std::array<bool, N>& signalled) {
        signalled = {};
        uint64_t remainingNs = budgetNs;
        for (std::size_t slot = 0; slot < depth; ++slot) {
            if (!scheduled.at(slot) || (!waitAll && slot != current))
                continue;
            if (!wait(slot, remainingNs))
                return false;
            signalled.at(slot) = true;
        }
        return true;
    }

    /// waitForIdle's scan. Every scheduled slot is observed, even after one
    /// has not signalled, so the result is the AND of all observations. A
    /// zero budget only polls.
    template <std::size_t N, class Wait>
    [[nodiscard]] bool pollScheduledFences(const std::array<bool, N>& scheduled,
            const std::size_t depth, const uint64_t budgetNs, Wait&& wait) {
        uint64_t remainingNs = budgetNs;
        bool idle = true;
        for (std::size_t slot = 0; slot < depth; ++slot) {
            if (!scheduled.at(slot))
                continue;
            const bool signalled = wait(slot, remainingNs);
            idle = signalled && idle;
        }
        return idle;
    }
}

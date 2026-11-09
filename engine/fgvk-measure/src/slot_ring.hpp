/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace fgvk::measure {

// Life cycle of one GPU end-point slot. The presenting thread claims a Free slot (Free -> Recording),
// records and submits it, then publishes it (Recording -> Submitted) only if the submit succeeded.
// Only the reader retires a Submitted slot (Submitted -> Free), after the slot's fence signalled and
// its queries were read. Nothing else moves a Submitted slot, so queries are never read before their
// own submit completed and never return a previous use's values.
template <std::size_t Count>
class SlotRing {
    static_assert(Count > 0, "a ring needs at least one slot");

public:
    enum class State : uint8_t { Free, Recording, Submitted };

    // Presenting thread: the first free slot, or nothing when every slot is in use (unmeasured).
    std::optional<std::size_t> claim() noexcept {
        for (std::size_t i = 0; i < Count; ++i) {
            State expected = State::Free;
            if (states_[i].compare_exchange_strong(expected, State::Recording, std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                return i;
            }
        }
        return std::nullopt;
    }

    // Presenting thread: the submit failed, so nothing will execute on the slot and it is free again.
    bool cancel(std::size_t i) noexcept {
        if (i >= Count) return false;
        State expected = State::Recording;
        return states_[i].compare_exchange_strong(expected, State::Free, std::memory_order_release,
                std::memory_order_relaxed);
    }

    // Presenting thread: the submit succeeded. The sequence is written before the release store, so
    // the reader that observes Submitted also observes the sequence.
    bool publish(std::size_t i, uint64_t seq) noexcept {
        if (i >= Count || states_[i].load(std::memory_order_relaxed) != State::Recording) return false;
        sequences_[i] = seq;
        states_[i].store(State::Submitted, std::memory_order_release);
        return true;
    }

    // Reader only: the fence signalled, the queries were read and the fence was reset.
    bool retire(std::size_t i) noexcept {
        if (i >= Count) return false;
        State expected = State::Submitted;
        return states_[i].compare_exchange_strong(expected, State::Free, std::memory_order_release,
                std::memory_order_relaxed);
    }

    bool submitted(std::size_t i) const noexcept {
        return i < Count && states_[i].load(std::memory_order_acquire) == State::Submitted;
    }

    // Valid only while the slot is Submitted, which the reader checked with an acquire load.
    uint64_t seq(std::size_t i) const noexcept { return sequences_[i]; }

    State state(std::size_t i) const noexcept { return states_[i].load(std::memory_order_acquire); }

    bool anySubmitted() const noexcept {
        for (std::size_t i = 0; i < Count; ++i) {
            if (submitted(i)) return true;
        }
        return false;
    }

private:
    std::array<std::atomic<State>, Count> states_{};
    std::array<uint64_t, Count> sequences_{};
};

}  // namespace fgvk::measure

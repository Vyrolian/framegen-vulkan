/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>

namespace fgvk::layer::trace_recorder {

using Clock = std::chrono::steady_clock;
// Records carry steady-clock nanoseconds; the writer converts them exactly as the text always did.
static_assert(std::is_same_v<Clock::duration, std::chrono::nanoseconds>);

/// The fixed payload bound of one record. Larger observations go through a window slot. GT1 grew the pacing output
/// sample by two 8-byte fields (tick_snap_ns, target_tick_ns) past 256; the queue exists only in a profiled process.
inline constexpr size_t payloadCapacity = 288;

/// Every record kind the writer formats. Kinds are stable names inside the process only.
enum class RecordKind : uint16_t {
    None,
    /// Rare one-off text. The payload holds a heap string the writer frees after writing.
    Text,
    PacingWindow,
    PacingOutput,
    PacingSkip,
    PacingSource,
    PacingGpu,
    PacingGpuSplit,
    PacingOutputGpu,
    PacingRealCopyGpu,
    PacingPresentMarkerGpu,
    PacingAcquireDriver,
    PacingApplication,
    PacingWait,
    PacingCopy,
    ApplicationPresentWindow,
    ApplicationPresentWaitWindow,
    PacingCall,
    PacingCallWindow,
    PacingAppFrame,
    PacingAcquire,
    ReflexMarker,
    ReflexSleep,
    ReflexMode,
    ReflexTiming,
    WaylandSample,
    WaylandGap,
    SubmitTag,
    PacingGpuPass,
    QueueFrame,
    Count
};

/// One queued record. The payload is a trivially copyable struct of at most payloadCapacity bytes.
struct TraceRecord {
    RecordKind kind{RecordKind::None};
    uint16_t bytes{};
    uint32_t thread{};
    // Steady-clock nanoseconds taken at enqueue. The writer's lag is measured from it.
    int64_t enqueuedNs{};
    alignas(8) std::array<std::byte, payloadCapacity> payload{};
};
static_assert(std::is_trivially_copyable_v<TraceRecord>);

/// Bounded multi-producer, single-consumer queue (Vyukov-style per-slot sequence numbers).
/// Storage is allocated and initialised once by the constructor. push never blocks, never
/// allocates and makes no system call; a full queue reports failure to the caller.
template<size_t Capacity>
class BasicRecordQueue {
    static_assert(std::has_single_bit(Capacity) && Capacity >= 2);
public:
    BasicRecordQueue() : cells(std::make_unique<Cell[]>(Capacity)) {
        for (size_t index = 0; index < Capacity; ++index)
            cells[index].sequence.store(index, std::memory_order_relaxed);
    }
    BasicRecordQueue(const BasicRecordQueue&) = delete;
    BasicRecordQueue& operator=(const BasicRecordQueue&) = delete;

    /// Returns false when the queue is full. Safe from any number of threads.
    [[nodiscard]] bool tryPush(const TraceRecord& record) noexcept {
        size_t position = enqueuePosition.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells[position & (Capacity - 1)];
            const size_t sequence = cell.sequence.load(std::memory_order_acquire);
            const auto difference = static_cast<std::intptr_t>(sequence) - static_cast<std::intptr_t>(position);
            if (difference == 0) {
                if (enqueuePosition.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
                    cell.record = record;
                    cell.sequence.store(position + 1, std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = enqueuePosition.load(std::memory_order_relaxed);
            }
        }
    }

    /// Hands every ready record to `consume` in queue order, at most one full lap per call.
    /// Only one thread may drain at a time.
    template<class Consume>
    size_t drain(Consume&& consume) {
        size_t drained = 0;
        for (; drained < Capacity; ++drained) {
            Cell& cell = cells[dequeuePosition & (Capacity - 1)];
            if (cell.sequence.load(std::memory_order_acquire) != dequeuePosition + 1)
                break;
            consume(std::as_const(cell.record));
            cell.sequence.store(dequeuePosition + Capacity, std::memory_order_release);
            ++dequeuePosition;
        }
        return drained;
    }

    [[nodiscard]] static constexpr size_t capacity() { return Capacity; }

private:
    struct Cell {
        std::atomic<size_t> sequence{};
        TraceRecord record{};
    };
    std::unique_ptr<Cell[]> cells;
    alignas(64) std::atomic<size_t> enqueuePosition{0};
    alignas(64) size_t dequeuePosition{0};
};

/// Fixed slots for observations too large for one record. A producer claims a free slot by
/// copy; the writer reads it and releases it. A full pool returns `none` and the caller counts a drop.
template<class T, size_t N>
class SlotPool {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(N > 0 && N < 32);
public:
    static constexpr size_t none = N;

    [[nodiscard]] size_t claim(const T& value) noexcept {
        uint32_t mask = freeMask.load(std::memory_order_acquire);
        for (;;) {
            if (mask == 0) return none;
            const auto slot = static_cast<uint32_t>(std::countr_zero(mask));
            if (freeMask.compare_exchange_weak(mask, mask & ~(uint32_t{1} << slot),
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                slots[slot] = value;
                return slot;
            }
        }
    }
    [[nodiscard]] const T& at(const size_t slot) const noexcept { return slots[slot]; }
    void release(const size_t slot) noexcept {
        freeMask.fetch_or(uint32_t{1} << slot, std::memory_order_release);
    }

private:
    std::array<T, N> slots{};
    std::atomic<uint32_t> freeMask{(uint32_t{1} << N) - 1};
};

/// The process recorder: one queue plus the counters that describe its own cost.
class TraceRecorder {
public:
    static constexpr size_t capacity = 65536;
    using Queue = BasicRecordQueue<capacity>;

    struct Counters {
        uint64_t enqueued{};
        uint64_t dropped{};
        uint64_t slotDrops{};
        int64_t enqueueSumNs{};
        int64_t enqueueMaxNs{};
    };

    TraceRecorder();

    /// Enqueues one trivially copyable payload. Never blocks, allocates or makes a system call.
    template<class Payload>
    bool record(const RecordKind kind, const Payload& payload) noexcept {
        static_assert(std::is_trivially_copyable_v<Payload>);
        static_assert(sizeof(Payload) <= payloadCapacity);
        return recordBytes(kind, &payload, sizeof(Payload));
    }

    /// Enqueues raw payload bytes, at most payloadCapacity of them.
    bool recordBytes(RecordKind kind, const void* data, size_t bytes) noexcept;

    /// Counts a window slot that could not be claimed. The record itself is never enqueued.
    void noteSlotDrop() noexcept { slotDrops.fetch_add(1, std::memory_order_relaxed); }

    /// Returns and clears the counters accumulated since the previous call.
    [[nodiscard]] Counters takeCounters() noexcept;

    template<class Consume>
    size_t drain(Consume&& consume) { return queue.drain(std::forward<Consume>(consume)); }

private:
    Queue queue;
    std::atomic<uint64_t> enqueued{};
    std::atomic<uint64_t> dropped{};
    std::atomic<uint64_t> slotDrops{};
    std::atomic<int64_t> enqueueSumNs{};
    std::atomic<int64_t> enqueueMaxNs{};
};

/// The steady-clock time of `value` in nanoseconds, the unit every record uses.
[[nodiscard]] inline int64_t nanoseconds(const Clock::time_point value) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
}

/// The calling thread's kernel id, read once per thread.
[[nodiscard]] uint32_t currentThreadId() noexcept;

}

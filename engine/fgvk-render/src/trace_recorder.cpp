/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "trace_recorder.hpp"

#include <unistd.h>

namespace fgvk::layer::trace_recorder {

TraceRecorder::TraceRecorder() = default;

bool TraceRecorder::recordBytes(const RecordKind kind, const void* const data, const size_t bytes) noexcept {
    if (bytes > payloadCapacity) return false;
    TraceRecord record{};
    record.kind = kind;
    record.bytes = static_cast<uint16_t>(bytes);
    record.thread = currentThreadId();
    const auto started = Clock::now();
    record.enqueuedNs = nanoseconds(started);
    if (bytes) std::memcpy(record.payload.data(), data, bytes);
    const bool pushed = queue.tryPush(record);
    const auto elapsed = nanoseconds(Clock::now()) - record.enqueuedNs;
    enqueueSumNs.fetch_add(elapsed, std::memory_order_relaxed);
    auto maximum = enqueueMaxNs.load(std::memory_order_relaxed);
    while (elapsed > maximum &&
            !enqueueMaxNs.compare_exchange_weak(maximum, elapsed, std::memory_order_relaxed)) {
    }
    if (pushed) enqueued.fetch_add(1, std::memory_order_relaxed);
    else dropped.fetch_add(1, std::memory_order_relaxed);
    return pushed;
}

TraceRecorder::Counters TraceRecorder::takeCounters() noexcept {
    Counters counters;
    counters.enqueued = enqueued.exchange(0, std::memory_order_relaxed);
    counters.dropped = dropped.exchange(0, std::memory_order_relaxed);
    counters.slotDrops = slotDrops.exchange(0, std::memory_order_relaxed);
    counters.enqueueSumNs = enqueueSumNs.exchange(0, std::memory_order_relaxed);
    counters.enqueueMaxNs = enqueueMaxNs.exchange(0, std::memory_order_relaxed);
    return counters;
}

uint32_t currentThreadId() noexcept {
    thread_local const uint32_t identifier = static_cast<uint32_t>(::gettid());
    return identifier;
}

}

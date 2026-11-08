/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "trace_recorder.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

using namespace fgvk::layer::trace_recorder;

namespace {
    void expect(const bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            std::exit(1);
        }
    }

    struct Probe {
        uint32_t producer{};
        uint32_t sequence{};
    };

    TraceRecord recordOf(const Probe& probe) {
        TraceRecord record{};
        record.kind = RecordKind::PacingWindow;
        record.bytes = sizeof(Probe);
        std::memcpy(record.payload.data(), &probe, sizeof(Probe));
        return record;
    }

    Probe probeOf(const TraceRecord& record) {
        Probe probe{};
        std::memcpy(&probe, record.payload.data(), sizeof(Probe));
        return probe;
    }

    void fullQueueRefusesAndDrainsInOrder() {
        BasicRecordQueue<8> queue;
        for (uint32_t index = 0; index < 8; ++index)
            expect(queue.tryPush(recordOf({0, index})), "push into a queue with room failed");
        expect(!queue.tryPush(recordOf({0, 99})), "push into a full queue succeeded");
        std::vector<uint32_t> seen;
        const size_t drained = queue.drain([&](const TraceRecord& record) { seen.push_back(probeOf(record).sequence); });
        expect(drained == 8, "drain did not return every queued record");
        for (uint32_t index = 0; index < 8; ++index)
            expect(seen[index] == index, "drain did not keep queue order");
        expect(queue.drain([](const TraceRecord&) {}) == 0, "an empty queue drained a record");
        expect(queue.tryPush(recordOf({0, 8})), "drained cells were not reused");
    }

    // Four producers race on a small queue while the test thread drains. Each producer pushes once
    // per sequence number and counts a refusal as a drop. Every push is either drained or dropped,
    // and the records of one producer come out in the order they were pushed.
    void multiProducerStressKeepsOrderAndCounts() {
        constexpr uint32_t producers = 4;
        constexpr uint32_t perProducer = 20000;
        BasicRecordQueue<256> queue;
        std::array<std::atomic<uint64_t>, producers> accepted{};
        std::array<std::atomic<uint64_t>, producers> dropped{};
        std::atomic<uint32_t> running{producers};
        std::vector<std::thread> threads;
        for (uint32_t producer = 0; producer < producers; ++producer) {
            threads.emplace_back([&, producer] {
                for (uint32_t sequence = 0; sequence < perProducer; ++sequence) {
                    if (queue.tryPush(recordOf({producer, sequence}))) accepted[producer].fetch_add(1);
                    else dropped[producer].fetch_add(1);
                }
                running.fetch_sub(1);
            });
        }
        std::array<uint64_t, producers> drained{};
        std::array<int64_t, producers> lastSequence{};
        lastSequence.fill(-1);
        const auto consume = [&](const TraceRecord& record) {
            const Probe probe = probeOf(record);
            expect(probe.producer < producers, "a record carried an unknown producer");
            expect(static_cast<int64_t>(probe.sequence) > lastSequence[probe.producer],
                "records of one producer were reordered");
            lastSequence[probe.producer] = probe.sequence;
            drained[probe.producer]++;
        };
        while (running.load() != 0)
            queue.drain(consume);
        for (auto& thread : threads) thread.join();
        while (queue.drain(consume) != 0) {
        }
        for (uint32_t producer = 0; producer < producers; ++producer) {
            expect(drained[producer] == accepted[producer].load(),
                "a record accepted by the queue was lost before the drain");
            expect(accepted[producer].load() + dropped[producer].load() == perProducer,
                "a push was neither accepted nor counted as a drop");
        }
    }

    void slotPoolClaimsAndReleases() {
        SlotPool<Probe, 4> pool;
        std::array<size_t, 4> slots{};
        for (uint32_t index = 0; index < 4; ++index) {
            slots[index] = pool.claim(Probe{7, index});
            expect(slots[index] != SlotPool<Probe, 4>::none, "a pool with free slots refused a claim");
        }
        expect(pool.claim(Probe{}) == SlotPool<Probe, 4>::none, "a full pool handed out a slot");
        expect(pool.at(slots[2]).sequence == 2, "a claimed slot lost its value");
        pool.release(slots[2]);
        const size_t again = pool.claim(Probe{8, 9});
        expect(again == slots[2], "a released slot was not reused");
        expect(pool.at(again).producer == 8 && pool.at(again).sequence == 9,
            "a reused slot kept its previous value");
    }

    void recorderRefusesOversizedAndCountsTakes() {
        TraceRecorder recorder;
        std::array<std::byte, payloadCapacity + 1> oversized{};
        expect(!recorder.recordBytes(RecordKind::Text, oversized.data(), oversized.size()),
            "a payload above the record bound was accepted");
        expect(recorder.record(RecordKind::PacingWindow, Probe{1, 2}), "a small record was refused");
        recorder.noteSlotDrop();
        const auto first = recorder.takeCounters();
        expect(first.enqueued == 1 && first.dropped == 0 && first.slotDrops == 1,
            "counters did not describe the records since the previous take");
        const auto second = recorder.takeCounters();
        expect(second.enqueued == 0 && second.slotDrops == 0, "counters were not cleared by a take");
        size_t drained = recorder.drain([](const TraceRecord& record) {
            expect(record.kind == RecordKind::PacingWindow, "the drained record changed kind");
        });
        expect(drained == 1, "the recorder did not hold the enqueued record");
    }
}

int main() {
    fullQueueRefusesAndDrainsInOrder();
    multiProducerStressKeepsOrderAndCounts();
    slotPoolClaimsAndReleases();
    recorderRefusesOversizedAndCountsTakes();
    std::cout << "trace recorder tests passed\n";
    return 0;
}

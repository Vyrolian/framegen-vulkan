/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "presentation_policy.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

using namespace fgvk::layer;
using namespace std::chrono_literals;

namespace {

    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }

    void testReleasedCadenceUsesBurstThroughput() {
        using Clock = RealFramePacer::Clock;
        RealFramePacer pacer;
        auto arrival = Clock::time_point{100s};
        constexpr auto capPeriod = 8333333ns;
        pacer.observeSource(arrival, true, 2, true);
        Clock::duration previousPeriod{};
        for (const auto period : {12500us, 25000us, 16666us, 8333us}) {
            // A slower block mean is adopted only after a second slower block,
            // so the first window of such a step still uses the previous period.
            const bool slowerStep = previousPeriod > Clock::duration::zero() &&
                period > previousPeriod + previousPeriod / 16;
            // Two GPU-queued groups plus the next queue refill. Most calls
            // arrive early, but all eight together represent eight sources.
            const std::array burst{period / 50, period / 50, 3 * period - period / 25,
                period / 50, period / 50, 3 * period - period / 25,
                period / 50, 2 * period - period / 50};
            for (size_t window = 0; window < 8; ++window) {
                for (const auto elapsed : burst) {
                    arrival += elapsed;
                    static_cast<void>(pacer.schedule(arrival, 120.0));
                    pacer.observeSource(arrival, true, 2, true);
                    pacer.recordOutputWait(0ns, true);
                }
                const auto batch = pacer.scheduleReleasedOutputs(arrival, capPeriod, 2);
                const auto measured = window == 0 && slowerStep ? previousPeriod : Clock::duration{period};
                const auto expected = std::max(measured, Clock::duration{capPeriod}) / 2;
                expect(batch && batch->interval == expected,
                    "released source bursts changed spacing away from measured throughput");
            }
            previousPeriod = period;
        }
        arrival += 2s;
        pacer.observeSource(arrival, true, 2, true);
        auto batch = pacer.scheduleReleasedOutputs(arrival, capPeriod, 2);
        expect(batch && batch->interval == capPeriod / 2,
            "a long pause contaminated the released throughput estimate");
        for (size_t i = 0; i < 8; ++i) {
            arrival += 12500us;
            pacer.observeSource(arrival, true, 2, true);
        }
        batch = pacer.scheduleReleasedOutputs(arrival, capPeriod, 2);
        expect(batch && batch->interval == 6250us,
            "released cadence did not recover after a long pause");
        // Returning to ordinary delivery must discard the released window.
        for (size_t i = 0; i < 12; ++i) {
            arrival += 16666us;
            pacer.observeSource(arrival, true);
        }
        batch = pacer.scheduleReleasedOutputs(arrival, capPeriod, 2);
        expect(batch && batch->interval == 8333us,
            "released qualification leaked into ordinary source observation");
    }

    // Release-gated caps return the application present immediately, so the
    // worker spaces each batch from readiness on the cap's output interval.
    void testReleasedOutputsStayInsideTheSourceInterval() {
        using Clock = RealFramePacer::Clock;
        const Clock::time_point start{};
        const auto source = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / 60.0));
        RealFramePacer pacer;
        expect(!pacer.scheduleReleasedOutputs(start, source, 1) &&
                !pacer.scheduleReleasedOutputs(start, Clock::duration{}, 2),
            "released output pacing accepted an empty batch or interval");
        std::optional<Clock::time_point> previousLast;
        auto arrival = start;
        for (size_t frame = 0; frame < 120; ++frame) {
            // Producer jitter around the cap, as the release wait allows.
            arrival += source + (frame % 3 == 0 ? 400us : frame % 3 == 1 ? -400us : 0us);
            const auto ready = arrival + 1ms;
            const auto batch = pacer.scheduleReleasedOutputs(ready, source, 2);
            expect(batch && batch->interval == source / 2,
                "released output pacing did not use the cap output interval");
            expect(batch->at(0) >= ready && batch->at(0) - ready <= RealFramePacer::maximumDetachedHold(batch->interval),
                "released output pacing held a batch beyond its bounded phase");
            expect(batch->at(1) < arrival + source,
                "a released batch outlived its source interval and would join the producer");
            if (previousLast && frame > 2)
                expect(batch->at(0) - *previousLast >= batch->interval - 400us,
                    "released output pacing compressed a generated slot beyond producer jitter");
            pacer.completeOutputBatch(batch->at(1));
            previousLast = batch->at(1);
        }

        // An arrival 0.9 output slot early shortens one gap instead of
        // retaining a phase that would end after the next arrival.
        arrival += source - source * 9 / 20;
        const auto early = pacer.scheduleReleasedOutputs(arrival + 1ms, source, 2);
        expect(early && early->at(0) - (arrival + 1ms) <= RealFramePacer::maximumDetachedHold(early->interval) &&
                early->at(1) < arrival + source,
            "an early source kept a retained phase past its own interval");
    }

    // A GPU-bound source slower than a non-binding release cap is spaced on
    // its measured period. A source faster than the cap never shortens it.
    void testReleasedOutputsUseMeasuredSourcePeriod() {
        using Clock = RealFramePacer::Clock;
        const Clock::time_point start{};
        const auto releasedIntervalMs = [&](const Clock::duration source, const size_t arrivals) {
            RealFramePacer pacer;
            auto arrival = start;
            for (size_t frame = 0; frame < arrivals; ++frame) {
                arrival += source;
                pacer.observeSource(arrival, true, 2);
            }
            const auto batch = pacer.scheduleReleasedOutputs(arrival + 1ms, 8333us, 2);
            expect(batch.has_value(), "released output pacing rejected a valid cap batch");
            return std::chrono::duration<double, std::milli>(batch->interval).count();
        };
        expect(std::abs(releasedIntervalMs(12500us, 12) - 6.25) < 0.05,
            "released batches ignored a slower measured GPU-bound source period");
        expect(std::abs(releasedIntervalMs(8333us, 12) - 4.1667) < 0.05,
            "a measured source period shortened the released cap interval");
        expect(std::abs(releasedIntervalMs(12500us, 0) - 4.1667) < 0.05,
            "released batches without an observed source ignored the cap interval");
    }

    // Without a cap there is no interval to bound a released batch. Its
    // spacing comes only from a qualified, uninterrupted measured source
    // period; until that qualifies, the caller keeps its ready-output path.
    void testReleasedUncappedOutputsUseQualifiedPeriod() {
        using Clock = RealFramePacer::Clock;
        const auto start = Clock::time_point{100s};
        const auto batchMs = [](const std::optional<RealFramePacer::OutputBatch>& batch) {
            return batch ? std::chrono::duration<double, std::milli>(batch->interval).count() : -1.0;
        };
        RealFramePacer pacer;
        expect(!pacer.scheduleReleasedOutputs(start, Clock::duration{}, 2),
            "uncapped released pacing returned a batch before any source was observed");
        auto arrival = start;
        // The first arrival only starts the clock. Seven measured intervals
        // leave the eight-sample released window incomplete.
        pacer.observeSource(arrival, true, 2, true);
        for (size_t i = 0; i < 7; ++i) {
            arrival += 13ms;
            pacer.observeSource(arrival, true, 2, true);
        }
        expect(!pacer.scheduleReleasedOutputs(arrival, Clock::duration{}, 2),
            "uncapped released pacing qualified before eight released samples");
        arrival += 13ms;
        pacer.observeSource(arrival, true, 2, true);
        expect(std::abs(batchMs(pacer.scheduleReleasedOutputs(arrival, Clock::duration{}, 2)) - 6.5) < 0.05,
            "uncapped released pacing ignored the qualified 13 ms source period");

        // An arrival gap longer than 250 ms discards the proven cadence until
        // a complete released window has been measured again.
        arrival += 251ms;
        pacer.observeSource(arrival, true, 2, true);
        expect(!pacer.scheduleReleasedOutputs(arrival, Clock::duration{}, 2),
            "uncapped released pacing kept a stale cadence across an arrival gap");
        for (size_t i = 0; i < 7; ++i) {
            arrival += 13ms;
            pacer.observeSource(arrival, true, 2, true);
        }
        expect(!pacer.scheduleReleasedOutputs(arrival, Clock::duration{}, 2),
            "uncapped released pacing requalified before a complete window after a gap");
        arrival += 13ms;
        pacer.observeSource(arrival, true, 2, true);
        expect(std::abs(batchMs(pacer.scheduleReleasedOutputs(arrival, Clock::duration{}, 2)) - 6.5) < 0.05,
            "uncapped released pacing did not requalify after an arrival gap");
    }

    // low_latency (just-in-time): a released batch whose planned phase is still
    // pending starts at its nominal time, where the chained batch is held to
    // that phase. Both paths keep the same bookkeeping for a later chained batch.
    void testJustInTimeReleasedBatchIgnoresPendingPhase() {
        using Clock = RealFramePacer::Clock;
        const Clock::time_point start{};
        constexpr auto source = 16ms;
        const auto arrivals = start + 12ms;

        RealFramePacer chained;
        const auto firstChained = chained.scheduleReleasedOutputs(start, source, 2);
        expect(firstChained && firstChained->at(0) == start && firstChained->interval == 8ms,
            "chained released batch did not start at its nominal time");
        const auto held = chained.scheduleReleasedOutputs(arrivals, source, 2);
        expect(held && held->at(0) == start + 16ms,
            "chained released batch lost its planned-phase hold");

        RealFramePacer justInTime;
        const auto firstJust = justInTime.scheduleReleasedOutputs(start, source, 2, true);
        expect(firstJust && firstJust->at(0) == start && firstJust->interval == 8ms,
            "just-in-time released batch did not start at its nominal time");
        const auto ready = justInTime.scheduleReleasedOutputs(arrivals, source, 2, true);
        expect(ready && ready->at(0) == arrivals && ready->interval == 8ms,
            "just-in-time released batch was held to the planned phase");
        // Its planned end is start + 20 ms, so a chained batch at start + 22 ms holds to start + 28 ms.
        const auto after = justInTime.scheduleReleasedOutputs(start + 22ms, source, 2);
        expect(after && after->at(0) == start + 28ms,
            "a chained batch after a just-in-time batch lost the planned phase");
    }

    // One late released block is a transient, not a new cadence. A slower
    // period needs a second consecutive slower block and then adopts the
    // smaller of the two means. A block that is not slower clears that pending
    // evidence, so a later isolated late block starts over.
    void testSlowerReleasedBlocksNeedConsecutiveConfirmation() {
        using Clock = RealFramePacer::Clock;
        using Block = std::array<std::chrono::microseconds, 8>;
        const auto intervalMs = [](const std::optional<RealFramePacer::OutputBatch>& batch) {
            return batch ? std::chrono::duration<double, std::milli>(batch->interval).count() : -1.0;
        };
        const Block qualifying{16667us, 16667us, 16667us, 16667us, 16667us, 16667us, 16667us, 16667us};
        const Block singleLate{16667us, 16667us, 16667us, 16667us, 16667us, 16667us, 16667us, 40000us};
        const Block sustained{25000us, 25000us, 25000us, 25000us, 25000us, 25000us, 25000us, 25000us};
        for (const Clock::duration sourceInterval : {Clock::duration{16667us}, Clock::duration{}}) {
            RealFramePacer pacer;
            auto arrival = Clock::time_point{100s};
            const auto observe = [&](const Block& block) {
                for (const auto elapsed : block) {
                    arrival += elapsed;
                    pacer.observeSource(arrival, true, 2, true);
                }
            };
            pacer.observeSource(arrival, true, 2, true);
            observe(qualifying);
            expect(std::abs(intervalMs(pacer.scheduleReleasedOutputs(arrival, sourceInterval, 2)) - 8.33) < 0.05,
                "released pacing did not use the qualified 16.67 ms cadence");
            observe(singleLate);
            expect(std::abs(intervalMs(pacer.scheduleReleasedOutputs(arrival, sourceInterval, 2)) - 8.33) < 0.05,
                "a single late released block changed the qualified cadence");
            // The normal block clears the pending late block, so the next late
            // block is again only the first slower block.
            observe(qualifying);
            observe(singleLate);
            expect(std::abs(intervalMs(pacer.scheduleReleasedOutputs(arrival, sourceInterval, 2)) - 8.33) < 0.05,
                "a late block after a normal block counted as a confirmed slower cadence");
            observe(sustained);
            observe(sustained);
            expect(std::abs(intervalMs(pacer.scheduleReleasedOutputs(arrival, sourceInterval, 2)) - 12.5) < 0.05,
                "two consecutive slower released blocks did not move the cadence");
        }
    }

    void testOrderedAcquireUsesExplicitFailureOnly() {
        OrderedAcquireRecovery recovery;
        const auto now = OrderedAcquireRecovery::TimePoint{};

        recovery.armTransitionRecreation();
        expect(recovery.transitionRecoveryActive(),
            "explicit transition did not arm one recreation permission");
        const auto healthy = recovery.observe(now, false, false, false);
        expect(!healthy.quarantined && !recovery.active() &&
                !recovery.transitionRecoveryActive() &&
                !recovery.signalRecreation(),
            "successful generated-image transport was treated as a failure");

        recovery.armTransitionRecreation();
        const auto failed = recovery.observe(now, true, false, false);
        expect(failed.quarantined && failed.timedOut &&
                failed.retryDelay == 250ms && recovery.active(),
            "explicit Vulkan timeout did not enter bounded native backoff");
        expect(recovery.signalRecreation() && !recovery.signalRecreation(),
            "transition-scoped recreation was not one-shot");
    }

    void testOrderedAcquireBackoffAndProbeAreFinite() {
        OrderedAcquireRecovery recovery;
        const auto start = OrderedAcquireRecovery::TimePoint{};
        const auto failed = recovery.observe(start, true);
        expect(recovery.beforePresent(start + 249ms).bypassGeneration,
            "direct acquire failure retried before its backoff");
        auto decision = recovery.beforePresent(start + failed.retryDelay);
        expect(decision.beginHistoryWarmup && decision.limitGeneratedFrames &&
                decision.preacquireGeneratedFrame &&
                decision.boundedAcquireProbe,
            "direct acquire failure did not reach one bounded retry");
        const auto missed = recovery.reportNonblockingProbeUnavailable(
            start + failed.retryDelay
        );
        expect(missed.quarantined && missed.boundedProbeFailed &&
                missed.retryDelay == 500ms,
            "failed bounded probe did not return to finite backoff");
        decision = recovery.beforePresent(
            start + failed.retryDelay + missed.retryDelay
        );
        expect(decision.boundedAcquireProbe,
            "second direct retry did not become eligible");
        const auto recovered = recovery.observe(
            start + failed.retryDelay + missed.retryDelay,
            false, false, true
        );
        expect(recovered.recovered && !recovery.active(),
            "successful bounded probe did not end direct recovery");
    }

    void testExternalInterruptionDiscardsOldFailure() {
        OrderedAcquireRecovery recovery;
        const auto now = OrderedAcquireRecovery::TimePoint{};
        recovery.armTransitionRecreation();
        static_cast<void>(recovery.observe(now, true));
        recovery.pauseForExternalInterruption();
        const auto decision = recovery.beforePresent(now + 1s);
        expect(!recovery.active() && !recovery.transitionRecoveryActive() &&
                !recovery.signalRecreation() &&
                !decision.bypassGeneration &&
                !decision.preacquireGeneratedFrame,
            "menu interruption resumed stale acquire recovery evidence");
    }
}

void testOrderedPresentationTimeline() {
    using Clock = OrderedPresentTimeline::Clock;
    const auto start = Clock::time_point{10s};
    for (const double refresh : {30., 40., 60., 90., 120., 144., 240.}) {
        const auto period = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / refresh));
        // A completion-limited application must not inherit a generated
        // batch's two-refresh lead while live Frame Generation is disabled.
        OrderedPresentTimeline native;
        auto completed = start;
        for (size_t frame = 0; frame < 120; ++frame) {
            const auto slot = native.schedule(completed, refresh, refresh, 1, false);
            expect(slot && slot->submitAt == completed && slot->presentAt == completed,
                "disabled generation delayed a completion-limited application");
            completed += period;
        }
        // Explicit policy changes must retain queued deadlines. An active
        // Fractional one-output turn still needs its original readiness lead.
        OrderedPresentTimeline transition;
        const auto generated = transition.schedule(start, refresh, refresh, 3);
        const auto disabled = transition.schedule(start, refresh, refresh, 1, false);
        expect(disabled->presentAt == generated->presentAt + period &&
                disabled->presentAt - disabled->submitAt == period,
            "live generation-off overtook queued output or retained batch lead");
        const auto resumed = transition.schedule(disabled->presentAt, refresh, refresh, 1, true);
        expect(resumed->presentAt == disabled->presentAt + 2 * period,
            "active one-output turn lost its generated readiness lead");
        const auto stalled = transition.schedule(start + 2s, refresh, refresh, 1, false);
        expect(stalled->presentAt == start + 2s && stalled->submitAt == start + 2s,
            "disabled generation retained queue debt after a stall");
        for (size_t multiplier = 2; multiplier <= 5; ++multiplier) {
            // Returning from an application presentation wait does not prove
            // the next frame's asynchronous GPU work is ready. Preserve the
            // readiness lead even when an earlier deadline is still future.
            OrderedPresentTimeline completionLimited;
            auto ready = start;
            auto previousOutput = start;
            for (size_t batch = 0; batch < 60; ++batch) {
                for (size_t output = 0; output < multiplier; ++output) {
                    const auto slot = completionLimited.schedule(
                        ready, refresh, refresh, multiplier);
                    const auto lead = period * static_cast<int64_t>(multiplier);
                    expect(slot && slot->presentAt - ready >= lead &&
                            slot->presentAt > previousOutput,
                        "completion wait consumed asynchronous GPU readiness lead");
                    previousOutput = slot->presentAt;
                    ready = slot->submitAt;
                }
                ready = previousOutput + period / 4;
            }
            OrderedPresentTimeline timeline;
            auto now = start;
            auto last = start;
            for (size_t batch = 0; batch < 60; ++batch) {
                // Outputs arrive as a burst from each generated batch.
                for (size_t output = 0; output < multiplier; ++output) {
                    const auto slot = timeline.schedule(now, refresh, refresh, multiplier);
                    expect(slot && slot->presentAt > last,
                        "bridge burst overtook an earlier output");
                    now = slot->submitAt;
                    const auto lead = period * static_cast<int64_t>(multiplier);
                    expect(slot->presentAt - now >= lead &&
                            slot->presentAt - now <= lead + period,
                        "bridge queue escaped its refresh-relative bounds");
                    if (batch > 0)
                        expect(slot->presentAt - last == period,
                            "bridge burst lost uniform output spacing");
                    last = slot->presentAt;
                }
            }
            const auto late = timeline.schedule(now + 2s, refresh, refresh);
            expect(late->submitAt == now + 2s &&
                    late->presentAt == now + 2s + 2 * period,
                "loading stall left catch-up debt in the bridge");
        }
    }
    OrderedPresentTimeline timeline;
    const auto slow = timeline.schedule(start, 30, 120);
    const auto slowNext = timeline.schedule(start, 30, 120);
    expect(slowNext->presentAt - slow->presentAt >= 33ms &&
            slowNext->presentAt - slowNext->submitAt <= 42ms,
        "below-refresh target escaped its batch-relative queue bound");
    const auto changed = timeline.schedule(slowNext->submitAt, 240, 240);
    expect(changed->presentAt > slowNext->presentAt &&
            changed->presentAt - changed->submitAt <= 12500us,
        "live refresh/target change overtook queued output or lost its bound");
    expect(!timeline.schedule(start, 120, 120, 0) &&
            !timeline.schedule(start, 120, 120, GeneratedFramePlan::capacity + 2),
        "bridge accepted an unbounded generated batch");
    OrderedPresentTimeline fractional;
    auto last = start;
    for (size_t frame = 0; frame < 40; ++frame) {
        auto arrival = start + 50ms * static_cast<int64_t>(frame);
        const size_t count = frame % 2 == 0 ? 5 : 4;
        for (size_t output = 0; output < count; ++output) {
            const auto slot = fractional.schedule(arrival, 90, 120, count);
            expect(slot->presentAt > last && slot->presentAt - slot->submitAt < 64ms,
                "alternating fractional batches lost ordering or grew the queue");
            arrival = slot->submitAt;
            last = slot->presentAt;
        }
    }
    OrderedPresentTimeline growing;
    const auto small = growing.schedule(start, 120, 120, 2);
    const auto larger = growing.schedule(start + 10ms, 120, 120, 5);
    expect(larger->presentAt > small->presentAt &&
            larger->presentAt >= start + 10ms + 41666us,
        "increased generated workload reused a shorter readiness lead");
    for (const double invalid : {0., -1., 1001.,
            std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN()}) {
        expect(!timeline.schedule(start, invalid, 120) &&
                !timeline.schedule(start, 120, invalid),
            "bridge accepted an invalid timing clock");
    }
}

void testBridgeCompositorReadiness() {
    using Timeline = OrderedPresentTimeline;
    using Clock = Timeline::Clock;
    const auto start = Clock::time_point{10s};
    const auto tick = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / 120.0));
    const auto prime = [&](Timeline& timeline, uint32_t id, auto arrival, auto cost) {
        timeline.observeSourceBatch(arrival, id);
        const auto slot = timeline.schedule(arrival, 120, 120, 2);
        timeline.submitted(id, slot->presentAt, slot->submitAt);
        timeline.feedback(id, slot->presentAt, slot->submitAt + cost);
        return *slot;
    };

    // Different CPU arrival gaps with the same readiness evidence must not
    // change GPU allowance. Includes presentation waits, CPU hitches and load.
    for (const auto gap : {16ms, 26ms, 50ms, 80ms}) {
        Timeline timeline;
        prime(timeline, 1, start, tick);
        const auto arrival = start + 200ms;
        timeline.observeSourceBatch(arrival, 2);
        auto slot = timeline.schedule(arrival, 120, 120, 2);
        expect(slot->presentAt == arrival + tick,
            "CPU source interval changed compositor readiness");
        timeline.submitted(2, slot->presentAt, slot->submitAt);
        timeline.feedback(2, slot->presentAt, slot->submitAt + tick);
        timeline.observeSourceBatch(arrival + gap, 3);
        slot = timeline.schedule(arrival + gap, 120, 120, 2);
        expect(slot->presentAt == arrival + gap + tick,
            "presentation waiting fed back into source readiness");
    }

    // Closed loop: each application waits for its real image before supplying
    // another batch. Source timing includes our lead. It must converge instead
    // of multiplying that delay, with every output retaining a distinct slot.
    for (const double refresh : {30., 60., 90., 120., 240.}) {
        const auto period = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / refresh));
        for (size_t count = 2; count <= 5; ++count) {
            Timeline timeline;
            auto arrival = start;
            auto completedReal = start;
            uint32_t id = 0;
            for (uint64_t batch = 0; batch < 80; ++batch) {
                timeline.observeSourceBatch(arrival, batch);
                const auto ready = arrival + period / 2;
                auto last = arrival;
                std::optional<Timeline::Slot> firstOutput;
                uint32_t firstId{};
                std::optional<Timeline::Slot> lastOutput;
                for (size_t output = 0; output < count; ++output) {
                    const auto slot = timeline.schedule(arrival, refresh, refresh, count);
                    timeline.submitted(++id, slot->presentAt, slot->submitAt);
                    if (output == 0) {
                        firstOutput = slot;
                        firstId = id;
                    }
                    expect(slot->presentAt > last,
                        "completion-paced outputs lost ordering");
                    // The real wait releases at compositor refresh, not at
                    // an arbitrary requested timestamp between refreshes.
                    const auto ticks = (slot->presentAt - start + period - 1ns) / period;
                    const auto completed = start + period * ticks;
                    if (batch > 0)
                        expect(completed == completedReal + period *
                                static_cast<int64_t>(output + 1),
                            "serialized source missed a refresh through its own lead");
                    last = completed;
                    lastOutput = slot;
                }
                // Deliver readiness after the real output completes and the
                // application's wait releases, before it begins the next batch.
                timeline.feedback(firstId, firstOutput->presentAt,
                    arrival + period, period - (ready - arrival));
                timeline.feedback(id, lastOutput->presentAt,
                    last, period / 2);
                // Real-image waits can return at the latch before display.
                completedReal = last;
                arrival = last - period / 4;
            }
        }
    }

    // Warm-up native output does not measure generated-batch readiness. Prime
    // when generation first starts, but never re-prime every Fractional turn.
    Timeline warmup;
    warmup.observeSourceBatch(start, 1);
    const auto native = warmup.schedule(start, 120, 120, 1);
    warmup.submitted(1, native->presentAt, start);
    warmup.feedback(1, native->presentAt, start + 1ms);
    warmup.observeSourceBatch(start + 100ms, 2);
    const auto generated = warmup.schedule(start + 100ms, 120, 120, 2);
    expect(generated->presentAt == start + 100ms + 4 * tick,
        "generation startup reused native-only readiness");
    warmup.submitted(2, generated->presentAt, start + 100ms);
    warmup.feedback(2, generated->presentAt, start + 105ms);
    for (uint64_t batch = 3; batch < 12; ++batch) {
        const auto now = start + 100ms * batch;
        warmup.observeSourceBatch(now, batch);
        const auto slot = warmup.schedule(now, 120, 120, batch % 2 ? 1 : 2);
        expect(slot->presentAt == now + tick,
            "Fractional batch alternation repeatedly primed readiness");
        warmup.submitted(static_cast<uint32_t>(batch), slot->presentAt, now);
        warmup.feedback(static_cast<uint32_t>(batch), slot->presentAt, now + 5ms);
    }

    // Asynchronous queued output has extra spacing ahead of its readiness
    // lead. That owned spacing must be subtracted from the readiness sample.
    Timeline queued;
    queued.observeSourceBatch(start, 1);
    const auto first = queued.schedule(start, 120, 120, 2);
    queued.submitted(1, first->presentAt, start);
    const auto real = queued.schedule(start, 120, 120, 2);
    queued.submitted(2, real->presentAt, start);
    queued.observeSourceBatch(start, 2);
    const auto behind = queued.schedule(start, 120, 120, 2);
    queued.submitted(3, behind->presentAt, behind->submitAt);
    queued.feedback(3, behind->presentAt, behind->presentAt - tick);
    queued.observeSourceBatch(start + 200ms, 3);
    const auto afterQueue = queued.schedule(start + 200ms, 120, 120, 2);
    expect(afterQueue->presentAt == start + 200ms + 3 * tick,
        "queued output spacing was counted as GPU work");

    // A separate presentation waiter releases a source-frame credit while
    // the real output of the next batch is still pending. That tail is not
    // a full batch of asynchronous work and must not impose the queue reserve.
    for (size_t count = 2; count <= 5; ++count) {
        Timeline occupancy;
        occupancy.observeSourceBatch(start, 1);
        auto previous = start;
        for (size_t output = 0; output < count; ++output) {
            const auto slot = occupancy.schedule(start, 120, 120, count);
            occupancy.submitted(static_cast<uint32_t>(output + 1), slot->presentAt, start);
            previous = slot->presentAt;
            if (output + 1 < count)
                occupancy.feedback(static_cast<uint32_t>(output + 1), previous, start + tick / 2);
        }
        const auto arrival = previous - 1ms;
        occupancy.observeSourceBatch(arrival, 2);
        const auto tail = occupancy.schedule(arrival, 120, 120, count);
        expect(tail->presentAt == previous + tick,
            "partial queued batch added a hold or lost its next ordered slot");
        occupancy.submitted(static_cast<uint32_t>(count + 1), tail->presentAt, arrival);
        // Keep the next batch pending without acknowledging the old real image.
        for (size_t output = 1; output < count; ++output) {
            const auto slot = occupancy.schedule(arrival, 120, 120, count);
            occupancy.submitted(static_cast<uint32_t>(count + output + 1), slot->presentAt, arrival);
        }
        const auto nextArrival = previous + tick;
        occupancy.observeSourceBatch(nextArrival, 3);
        const auto queuedSlot = occupancy.schedule(nextArrival, 120, 120, count);
        expect(queuedSlot->presentAt >= nextArrival + 3 * tick,
            "fully queued asynchronous work lost its bounded reserve");
        occupancy.submitted(static_cast<uint32_t>(2 * count + 1), queuedSlot->presentAt, nextArrival);
        occupancy.feedback(static_cast<uint32_t>(2 * count + 1), queuedSlot->presentAt, nextArrival + tick / 2);
        occupancy.observeSourceBatch(start + 200ms, 4);
        expect(occupancy.schedule(start + 200ms, 120, 120, count)->presentAt == start + 200ms + tick,
            "completion-paced source retained the asynchronous queue reserve");
    }

    // The next batch can be entirely pending when a separate waiter releases
    // a credit. At depth two, adding a reserve here limits throughput even
    // though every frame's CPU/GPU work fits inside the source budget.
    for (size_t count = 2; count <= 5; ++count) {
        Timeline pipeline;
        pipeline.observeSourceBatch(start, 0);
        const auto learned = pipeline.schedule(start, 120, 120, count);
        pipeline.submitted(UINT32_MAX, learned->presentAt, start);
        pipeline.feedback(UINT32_MAX, learned->presentAt, start + tick / 2);
        const auto arrival = start + 200ms;
        pipeline.observeSourceBatch(arrival, 1);
        auto last = arrival;
        for (size_t output = 0; output < count; ++output) {
            const auto slot = pipeline.schedule(arrival, 120, 120, count);
            pipeline.submitted(static_cast<uint32_t>(output), slot->presentAt, arrival);
            last = slot->presentAt;
        }
        pipeline.observeSourceBatch(last - 1ms, 2);
        expect(pipeline.schedule(last - 1ms, 120, 120, count)->presentAt == last + tick,
            "one pending batch added a hold or lost its next ordered slot");
    }

    // FIFO eligibility includes older outputs. Remove their owned slots from
    // the learned work allowance, but do not subtract spacing already removed
    // by the desired-deadline correction a second time.
    for (const auto arrivalGap : {1ms, 9ms}) {
        Timeline fifo;
        prime(fifo, 1, start, tick / 2);
        const auto producer = start + 200ms;
        fifo.observeSourceBatch(producer, 2);
        const auto priorFirst = fifo.schedule(producer, 120, 120, 2);
        fifo.submitted(2, priorFirst->presentAt, producer);
        const auto priorReal = fifo.schedule(producer, 120, 120, 2);
        fifo.submitted(3, priorReal->presentAt, producer);
        const auto arrival = producer + arrivalGap;
        fifo.observeSourceBatch(arrival, 3);
        const auto first = fifo.schedule(arrival, 120, 120, 2);
        fifo.submitted(4, first->presentAt, arrival);
        const auto reserved = std::max(Clock::duration::zero(),
            first->presentAt - arrival - tick);
        const auto ownedHold = std::max(reserved, 2 * tick);
        fifo.feedback(4, first->presentAt, arrival + ownedHold + tick / 2);
        fifo.observeSourceBatch(start + 400ms, 4);
        expect(fifo.schedule(start + 400ms, 120, 120, 2)->presentAt == start + 400ms + tick,
            "FIFO occupancy was learned as GPU work or deducted twice");
    }

    // Rising GPU cost extends readiness immediately. Four recent batches
    // retain that protection; one late output cannot lower the first deadline.
    Timeline rising;
    prime(rising, 1, start, tick);
    prime(rising, 2, start + 200ms, 26ms);
    rising.observeSourceBatch(start + 300ms, 3);
    const auto expensive = rising.schedule(start + 300ms, 120, 120, 2);
    expect(expensive->presentAt == start + 326ms,
        "GPU readiness feedback did not extend the deadline");
    rising.submitted(3, expensive->presentAt, expensive->submitAt);
    const auto expensiveReal = rising.schedule(start + 300ms, 120, 120, 2);
    rising.submitted(4, expensiveReal->presentAt, expensiveReal->submitAt);
    rising.feedback(3, expensive->presentAt, start + 326ms);
    rising.feedback(4, expensiveReal->presentAt, start + 327ms);
    for (uint32_t id = 5; id < 9; ++id)
        prime(rising, id, start + 200ms * id, tick);
    rising.observeSourceBatch(start + 1900ms, 10);
    expect(rising.schedule(start + 1900ms, 120, 120, 2)->presentAt ==
            start + 1900ms + tick,
        "recovered GPU work retained stale readiness");

    // Compositors need not report a dropped first output. Later feedback must
    // still correct that batch's first deadline rather than hide the miss.
    Timeline dropped;
    dropped.observeSourceBatch(start, 1);
    const auto droppedFirst = dropped.schedule(start, 120, 120, 2);
    dropped.submitted(UINT32_MAX, droppedFirst->presentAt, start);
    const auto surviving = dropped.schedule(start, 120, 120, 2);
    dropped.submitted(0, surviving->presentAt, start);
    dropped.feedback(0, surviving->presentAt, start + 50ms);
    dropped.observeSourceBatch(start + 200ms, 2);
    expect(dropped.schedule(start + 200ms, 120, 120, 2)->presentAt == start + 250ms,
        "dropped first output or protocol-ID wrap lost readiness feedback");

    Timeline invalid;
    invalid.observeSourceBatch(start, 1);
    const auto initial = invalid.schedule(start, 120, 120, 2);
    invalid.submitted(1, initial->presentAt, start);
    invalid.feedback(99, initial->presentAt, start + tick);
    invalid.feedback(1, initial->presentAt + 1ns, start + tick);
    invalid.feedback(1, initial->presentAt, {});
    invalid.feedback(1, initial->presentAt, start - 1ns);
    invalid.feedback(1, initial->presentAt, start + 1s);
    invalid.feedback(1, initial->presentAt, start + tick, -1ns);
    invalid.feedback(1, initial->presentAt, start + tick, (start + tick).time_since_epoch());
    invalid.observeSourceBatch(start + 200ms, 2);
    expect(invalid.schedule(start + 200ms, 120, 120, 2)->presentAt ==
            start + 200ms + 4 * tick,
        "unmatched or invalid compositor feedback affected readiness");
    // No feedback is bounded even after the fixed pending ring wraps.
    for (uint64_t batch = 3; batch < 1000; ++batch) {
        const auto now = start + 200ms * batch;
        invalid.observeSourceBatch(now, batch);
        const auto slot = invalid.schedule(now, 120, 120, 2);
        invalid.submitted(static_cast<uint32_t>(batch), slot->presentAt, now);
        expect(slot->presentAt == now + 4 * tick,
            "missing feedback inflated bridge lead");
    }
    invalid.feedback(1, initial->presentAt, start + tick);

    Timeline transitions;
    prime(transitions, 1, start, 50ms);
    transitions.observeSourceBatch(start + 100ms, 2);
    const auto queuedBeforeOff = transitions.schedule(start + 100ms, 120, 120, 2);
    const auto off = transitions.schedule(start + 101ms, 120, 120, 1, false);
    expect(off->presentAt > queuedBeforeOff->presentAt &&
            off->presentAt - off->submitAt == tick,
        "FG off discarded queued order or retained generated readiness");
    transitions.observeSourceBatch(start + 300ms, 3);
    expect(transitions.schedule(start + 300ms, 120, 120, 2)->presentAt ==
            start + 300ms + 4 * tick,
        "FG re-enable reused stale feedback");
    prime(transitions, 4, start + 500ms, tick);
    transitions.observeSourceBatch(start + 3s, 5);
    expect(transitions.schedule(start + 3s, 120, 120, 2)->presentAt ==
            start + 3s + 2 * tick,
        "long pause lost its bounded recovery reserve");

    // A pause can drain the old queue without its feedback being consumed.
    // Retire those IDs and prime recovery once. Native history warm-up must
    // not release the allowance before generation resumes, nor add a second
    // startup hold. Fresh samples age the recovery allowance out naturally.
    for (const auto cost : {tick, 3 * tick}) {
        Timeline paused;
        prime(paused, 1, start, cost);
        const auto beforePause = start + 100ms;
        paused.observeSourceBatch(beforePause, 2);
        auto old = paused.schedule(beforePause, 120, 120, 2);
        paused.submitted(2, old->presentAt, beforePause);
        for (uint32_t id = 3; id <= 6; ++id) {
            const auto slot = paused.schedule(beforePause, 120, 120, 2);
            paused.submitted(id, slot->presentAt, slot->submitAt);
        }
        // More native-only frames than the readiness window, including a
        // second pause, must not consume the generated recovery allowance.
        for (uint32_t batch = 3; batch <= 16; ++batch) {
            const auto now = start + 2s + 100ms * (batch - 3) +
                (batch >= 9 ? 2s : 0s);
            paused.observeSourceBatch(now, batch);
            // Late feedback from before the pause must not contaminate the
            // retained estimate or the new queue's completion accounting.
            paused.feedback(2, old->presentAt, beforePause + 90ms);
            const auto slot = paused.schedule(now, 120, 120, batch >= 11 ? 2 : 1);
            const auto expected = batch < 7 ? cost : batch < 11 ? tick :
                batch < 15 ? std::max(cost, 2 * tick) : tick;
            expect(slot->presentAt == now + expected,
                "pause warm-up released recovery lead early or re-primed generation");
            paused.submitted(batch + 4, slot->presentAt, now);
            paused.feedback(batch + 4, slot->presentAt, now + tick / 2);
        }
        const auto backwards = start + 1s;
        paused.observeSourceBatch(backwards, 17);
        // Deadlines remain ordered, but the old cost must be invalidated.
        const auto fresh = start + 8s;
        paused.observeSourceBatch(fresh, 18);
        expect(paused.schedule(fresh, 120, 120, 2)->presentAt == fresh + 4 * tick,
            "clock reset retained an invalid readiness estimate");
    }
}

void testBridgeCompletionReturnDoesNotDrift() {
    using Timeline = OrderedPresentTimeline;
    const auto start = Timeline::TimePoint{10s};
    for (const double refresh : {60., 90., 120., 144., 240.}) {
        const auto tick = std::chrono::duration_cast<Timeline::Clock::duration>(
            std::chrono::duration<double>(1.0 / refresh));
        for (size_t count = 1; count <= 5; ++count) {
            for (const auto returnDelay : {100us, 300us, 1000us}) {
                Timeline timeline;
                auto arrival = start;
                auto previous = start;
                uint32_t id = 0;
                for (uint64_t batch = 1; batch <= 120; ++batch) {
                    timeline.observeSourceBatch(arrival, batch);
                    auto submit = arrival;
                    auto first = Timeline::Slot{};
                    uint32_t firstId = 0;
                    for (size_t output = 0; output < count; ++output) {
                        const auto slot = timeline.schedule(submit, refresh, refresh, count);
                        expect(slot.has_value(), "completion-paced bridge rejected a valid batch");
                        submit = slot->submitAt;
                        timeline.submitted(++id, slot->presentAt, submit);
                        if (output == 0) {
                            first = *slot;
                            firstId = id;
                        }
                        if (batch > 1)
                            expect(slot->presentAt == previous + tick,
                                "completion-return overhead drifted a ready ordered deadline");
                        expect(slot->presentAt >= submit + tick / 2,
                            "retained deadline consumed measured GPU work allowance");
                        previous = slot->presentAt;
                    }
                    // The next CPU turn follows completion slightly late, but
                    // the observed work still fits before the next output.
                    timeline.feedback(firstId, first.presentAt, first.submitAt + tick / 2);
                    timeline.feedback(id, previous, submit + tick / 2);
                    arrival = previous + returnDelay;
                }
                // A real source hitch must rebase, never replay old slots.
                const auto late = previous + 100ms;
                timeline.observeSourceBatch(late, 121);
                const auto resumed = timeline.schedule(late, refresh, refresh, count);
                expect(resumed->presentAt == late + tick,
                    "completion-return tolerance retained debt across a source hitch");
            }
        }
    }
}

void testLongSessionDeadlineBounds() {
    const auto start = RealFramePacer::TimePoint{std::chrono::hours(2400)};
    for (const double fps : {45., 60., 90., 120.}) {
        RealFramePacer pacer;
        OrderedPresentTimeline timeline;
        const auto period = std::chrono::duration_cast<RealFramePacer::Clock::duration>(
            std::chrono::duration<double>(1.0 / fps));
        static_cast<void>(pacer.schedule(start, fps));
        auto bridgeArrival = start;
        const size_t frames = static_cast<size_t>(fps * 7200);
        for (size_t frame = 1; frame <= frames; ++frame) {
            const auto expected = start + period * static_cast<int64_t>(frame);
            const auto arrival = expected - 1ms;
            expect(pacer.schedule(arrival, fps) == expected,
                "two-hour source cap accumulated extra deadline debt");
            const auto slot = timeline.schedule(bridgeArrival, fps, fps, 2);
            expect(slot && slot->presentAt - slot->submitAt <= 3 * period,
                "two-hour ordered timeline accumulated a growing queue");
            bridgeArrival = slot->submitAt;
        }
        const auto elapsedNs = std::chrono::duration<long double, std::nano>(
            period * static_cast<int64_t>(frames)).count();
        const auto idealNs = static_cast<long double>(frames) * 1'000'000'000.L / fps;
        expect(std::abs(elapsedNs - idealNs) < 1'000'000.L,
            "two-hour nanosecond rounding became a millisecond-scale pacing error");
        const auto late = start + 2h + 100ms;
        expect(pacer.schedule(late, fps) == late,
            "late source frame after two hours retained old timing debt");
        const auto slot = timeline.schedule(bridgeArrival + 100ms, fps, fps, 2);
        expect(slot && slot->submitAt == bridgeArrival + 100ms,
            "late ordered frame after two hours retained old queue debt");
    }
}

void testFixedUsesExistingSourceDeadline() {
    using Clock = RealFramePacer::Clock;
    const auto start = Clock::time_point{10s};
    for (const double outputFps : {60., 90., 120.}) {
        for (const size_t count : {2U, 3U, 4U, 5U}) {
            const double sourceFps = outputFps / count;
            const auto sourceInterval = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(1. / sourceFps));
            const auto outputInterval = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(1. / outputFps));
            RealFramePacer pacer;
            static_cast<void>(pacer.schedule(start, sourceFps));
            auto previousReal = start;
            std::optional<Clock::time_point> previousOutput;
            for (size_t frame = 0; frame < 120; ++frame) {
                const auto real = pacer.schedule(previousReal + 1ms, sourceFps);
                expect(real == previousReal + sourceInterval,
                    "Fixed output pacing changed the existing source cap");
                const auto batch = pacer.scheduleOutputs(previousReal + 2ms,
                    real, outputFps, count, false);
                expect(batch && batch->count == count &&
                        batch->first > previousReal && batch->at(count - 1) <= real,
                    "Fixed output batch escaped its reserved source interval");
                for (size_t output = 0; output < count; ++output) {
                    const auto deadline = batch->at(output);
                    if (previousOutput) {
                        const auto error = deadline - *previousOutput - outputInterval;
                        expect(error >= -100ns && error <= 100ns,
                            "Fixed generated and real frames lost uniform output spacing");
                    }
                    previousOutput = deadline;
                }
                previousReal = real;
            }
            const auto late = previousReal + 2s;
            const auto real = pacer.schedule(late, sourceFps);
            expect(real == late && !pacer.scheduleOutputs(late, real, outputFps, count, false),
                "Fixed loading stall accumulated catch-up output debt");
            const auto next = pacer.schedule(late + 1ms, sourceFps);
            expect(!pacer.scheduleOutputs(next - 1ms, next, outputFps, count, false),
                "slow Fixed preparation compressed an expired output batch");
            expect(!pacer.scheduleOutputs(late + 1ms, next, outputFps, 1, false),
                "native Fixed recovery acquired a generated-output deadline");
        }
    }
}

void testFixedCapRecoversAfterLateArrival() {
    using Clock = RealFramePacer::Clock;
    for (const double cap : {30., 60., 90.}) {
        const auto period = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1. / cap));
        for (const size_t count : {2U, 3U, 4U, 5U}) {
            RealFramePacer pacer;
            const auto outputInterval = period / static_cast<int64_t>(count);
            const auto turnaround = std::min(Clock::duration{3ms}, outputInterval / 2);
            const auto start = Clock::time_point{10s};
            auto returned = start;
            size_t recoveredBatches = 0;
            for (size_t frame = 0; frame < 180; ++frame) {
                // Model Minecraft's short game turnaround, asynchronous GPU
                // preparation and a late arrival after the cap has settled.
                // A second readiness wait followed by a full output span
                // exceeds the next source period and perpetuates the miss.
                const auto arrival = returned + turnaround + (frame == 90 ? period : 0ns);
                const auto deadline = pacer.schedule(arrival, cap);
                pacer.observeSource(arrival, true);
                auto now = arrival + 100us;
                if (deadline == arrival)
                    pacer.resetOutputs();
                auto batch = deadline > arrival
                    ? pacer.scheduleOutputs(now, deadline, cap * count, count, false)
                    : std::nullopt;
                Clock::duration waited{};
                if (!batch) {
                    const auto budget = pacer.sourceReadinessBudget(now, count);
                    const auto ready = arrival + outputInterval - turnaround / 2;
                    const auto wait = std::min(budget, std::max(Clock::duration{}, ready - now));
                    waited += wait;
                    now += wait;
                    if (now >= ready)
                        batch = pacer.scheduleReadyOutputs(now, count);
                }
                if (batch) {
                    waited += std::max(Clock::duration{}, batch->at(count - 1) - now);
                    now = std::max(now, batch->at(count - 1));
                }
                returned = std::max(now, deadline);
                pacer.recordOutputWait(waited);
                if (frame > 92) {
                    expect(batch && batch->at(count - 1) <= deadline &&
                            deadline - batch->at(count - 1) < 1us &&
                            batch->interval == period / static_cast<int64_t>(count),
                        "a short source hitch trapped a proven cap in ready-output fallback");
                    ++recoveredBatches;
                }
            }
            expect(recoveredBatches == 87,
                "source-cap recovery did not retain the warmed output cadence");
        }
    }
}

void testLateNativeFixedCapKeepsSpacing() {
    using Clock = RealFramePacer::Clock;
    for (const double cap : {30., 60., 70., 90.}) {
        const auto period = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1. / cap));
        for (const size_t count : {2U, 3U, 4U, 5U}) {
            RealFramePacer pacer;
            const auto slot = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(1. / (cap * count)));
            auto returned = Clock::time_point{10s};
            auto previousReal = returned;
            (void)pacer.schedule(returned, cap);
            for (size_t frame = 0; frame < 180; ++frame) {
                const auto arrival = returned + slot / 8;
                const auto deadline = pacer.schedule(arrival, cap);
                pacer.observeSource(arrival, true, count);
                expect(deadline == returned + period,
                    "late detached output shifted the application's configured source cap");
                const auto prepared = frame % 17 == 8
                    ? deadline - slot / 2 : returned + slot / 4;
                const auto batch = pacer.scheduleOutputs(prepared, deadline,
                    cap * count, count, false, true);
                expect(batch && batch->first >= prepared && batch->interval == slot,
                    "late native Fixed preparation dropped or compressed output spacing");
                expect(batch->at(count - 1) < deadline + slot * static_cast<int64_t>(count - 1),
                    "late native Fixed pacing retained more than one batch span of debt");
                if (frame > 0)
                    expect(batch->at(count - 1) >= previousReal + slot * static_cast<int64_t>(count),
                        "a rephased Fixed cap compressed the following output batch");
                previousReal = batch->at(count - 1);
                pacer.recordOutputWait(slot / 8, true);
                // The final image is detached. The application returns at the
                // existing source-cap boundary, without serializing its next
                // frame's preparation behind the shifted real output.
                returned = deadline;
            }
            const auto lateArrival = returned + period + slot / 16;
            expect(pacer.schedule(lateArrival, cap) == lateArrival,
                "a missed source-cap deadline unexpectedly reserved output slack");
            pacer.observeSource(lateArrival, true, count);
            expect(!pacer.scheduleReadyOutputs(lateArrival + slot / 4, count),
                "a synchronous cap gained an independent readiness wait");
            expect(pacer.scheduleReadyOutputs(lateArrival + slot / 4, count, true).has_value(),
                "a short cap miss dropped pacing despite proven detached delivery");
            pacer.recordOutputWait(slot / 4, false);
            expect(!pacer.scheduleReadyOutputs(lateArrival + slot / 4, count, true),
                "private queue availability replaced actual detached-delivery evidence");
            const auto deadline = pacer.schedule(returned + slot / 8, cap);
            expect(!pacer.scheduleOutputs(deadline + period, deadline,
                    cap * count, count, false, true),
                "native Fixed phase correction accumulated a missed source interval");
            RealFramePacer target;
            (void)target.schedule(returned, cap);
            expect(!target.scheduleOutputs(returned + period - slot / 2,
                    returned + period, cap * count, count, true, true),
                "native Fixed phase correction moved a target output clock");
        }
    }
}

void testWorkerReadinessPreservesCapAndOutputPhase() {
    RealFramePacer pacer;
    const auto start = RealFramePacer::TimePoint{} + 1s;
    static_cast<void>(pacer.schedule(start, 60.));
    const auto cap = pacer.schedule(start + 1ms, 60.);
    const auto batch = pacer.scheduleOutputs(start + 2ms, cap, 120., 2, false, true);
    expect(batch.has_value(), "native cap could not prepare its first batch");
    const auto gpuShift = 9ms;
    pacer.completeOutputBatch(batch->at(1) + gpuShift, gpuShift);
    const auto nextCap = pacer.schedule(cap + 1ms, 60.);
    const auto next = pacer.scheduleOutputs(cap + 2ms, nextCap, 120., 2, false, true);
    expect(next && next->first == batch->at(1) + gpuShift + batch->interval,
        "worker GPU readiness shift was lost at the next batch boundary");
    expect(nextCap - cap == batch->interval * 2,
        "worker GPU readiness added another source-cap interval");
}

void testReadyFixedOutputs() {
    using Clock = RealFramePacer::Clock;
    const auto start = Clock::time_point{10s};
    const auto period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1. / 60));
    for (const size_t count : {2U, 3U, 4U, 5U}) {
        for (const double cap : {0., 60.}) {
            RealFramePacer pacer;
            auto arrival = start;
            std::optional<RealFramePacer::OutputBatch> batch;
            for (size_t frame = 0; frame < 200; ++frame) {
                (void)pacer.schedule(arrival, cap);
                pacer.observeSource(arrival, true);
                batch = pacer.scheduleReadyOutputs(arrival + 1ms, count);
                if (frame >= 8) {
                    const auto span = (period / static_cast<int64_t>(count)) *
                        static_cast<int64_t>(count - 1);
                    expect(pacer.sourceReadinessBudget(arrival + 1ms, count) == period - span - 1ms,
                        "GPU readiness did not reserve the complete output span");
                    expect(batch && batch->first == arrival + 1ms &&
                            batch->interval == period / static_cast<int64_t>(count) &&
                            batch->at(count - 1) < arrival + period + 1ms,
                        "game/combined caps lost ready-output spacing or accumulated latency");
                    pacer.recordOutputWait(batch->interval * static_cast<int64_t>(count - 1));
                }
                arrival += period;
            }
            arrival += 100ms;
            pacer.observeSource(arrival, true);
            expect(!pacer.scheduleReadyOutputs(arrival + 1ms, count),
                "a source hitch acquired another generated-output wait");
            arrival += period;
            pacer.observeSource(arrival, true);
            batch = pacer.scheduleReadyOutputs(arrival + 1ms, count);
            expect(batch && batch->interval == period / static_cast<int64_t>(count),
                "a source hitch stretched the retained output clock");
            expect(!pacer.scheduleReadyOutputs(arrival + period, count),
                "late GPU/CPU preparation acquired another full output interval");
            expect(!pacer.scheduleReadyOutputs(arrival + period -
                    batch->interval * static_cast<int64_t>(count - 1) + 1ns, count),
                "GPU preparation borrowed the output span from the next source frame");
            pacer.recordOutputWait(batch->interval * static_cast<int64_t>(count - 1));
            for (size_t frame = 0; frame < 200; ++frame) {
                const auto wait = batch->interval * static_cast<int64_t>(count - 1);
                arrival += period + wait;
                (void)pacer.schedule(arrival, cap);
                pacer.observeSource(arrival, true);
                batch = pacer.scheduleReadyOutputs(arrival + 1ms, count);
                expect(batch && batch->interval == period / static_cast<int64_t>(count),
                    "relative game limiter fed intentional waits into a growing cadence estimate");
                pacer.recordOutputWait(wait);
            }
            for (size_t frame = 0; frame < 32; ++frame) {
                arrival += 2 * period;
                pacer.observeSource(arrival, true);
                batch = pacer.scheduleReadyOutputs(arrival + 1ms, count);
                if (batch)
                    pacer.recordOutputWait(batch->interval * static_cast<int64_t>(count - 1));
            }
            expect(batch && batch->interval == 2 * period / static_cast<int64_t>(count),
                "sustained slower work failed to relearn from unpaced samples");
            for (size_t frame = 0; frame < 24; ++frame) {
                arrival += period;
                pacer.observeSource(arrival, true);
                batch = pacer.scheduleReadyOutputs(arrival + 1ms, count);
                if (batch)
                    pacer.recordOutputWait(batch->interval * static_cast<int64_t>(count - 1));
            }
            expect(batch && batch->interval == period / static_cast<int64_t>(count),
                "source acceleration retained a slower output interval");
            pacer.observeSource(arrival + period, false);
            expect(!pacer.scheduleReadyOutputs(arrival + period, count),
                "another timing owner retained ready-output pacing");
            pacer.reset();
            expect(pacer.sourceReadinessBudget(arrival) == Clock::duration::zero(),
                "live pacer reset retained source-readiness state");
        }
    }
}

void testReadyFixedCapOwnershipRequalification() {
    using Clock = RealFramePacer::Clock;
    constexpr auto period = 16666666ns;
    const auto start = Clock::time_point{10s};
    for (const size_t count : {2U, 3U, 4U, 5U}) {
        RealFramePacer pacer;
        auto arrival = start;
        for (size_t frame = 0; frame < 40; ++frame) {
            const auto deadline = pacer.schedule(arrival, 60.);
            pacer.observeSource(arrival, true);
            if (deadline > arrival) {
                const auto batch = pacer.scheduleOutputs(arrival + 100us,
                    deadline, 60. * count, count, false);
                expect(batch.has_value(), "healthy source cap did not establish output ownership");
                pacer.recordOutputWait(batch->at(count - 1) - arrival - 100us);
                arrival = deadline + 1ms;
            } else {
                arrival += 1ms;
            }
        }
        expect(pacer.sourceReadinessBudget(arrival, count) == Clock::duration{},
            "proven source cap allowed an independent readiness clock");

        // Real work that stays slower than the cap must still qualify the
        // game-owned fallback. Do not turn one successful cap into a latch
        // that survives a permanent scene or game-limiter change.
        std::optional<RealFramePacer::OutputBatch> batch;
        for (size_t frame = 0; frame < 40; ++frame) {
            arrival += 2 * period;
            (void)pacer.schedule(arrival, 60.);
            pacer.observeSource(arrival, true);
            batch = pacer.scheduleReadyOutputs(arrival + 100us, count);
            pacer.recordOutputWait(batch
                ? batch->interval * static_cast<int64_t>(count - 1) : Clock::duration{});
        }
        expect(batch && batch->interval == 2 * period / static_cast<int64_t>(count),
            "sustained slower source could not release configured-cap ownership");

        pacer.reset();
        (void)pacer.schedule(arrival, 60.);
        pacer.observeSource(arrival, true);
        const auto deadline = pacer.schedule(arrival + 1ms, 60.);
        pacer.observeSource(arrival + 1ms, true);
        expect(pacer.scheduleOutputs(arrival + 1100us, deadline,
                60. * count, count, false).has_value(),
            "reset source cap could not regain output ownership");
        (void)pacer.schedule(arrival + 2ms, 0.);
        for (size_t frame = 1; frame <= 10; ++frame) {
            pacer.observeSource(arrival + frame * period, true);
        }
        expect(pacer.scheduleReadyOutputs(arrival + 10 * period + 100us, count).has_value(),
            "disabling the configured cap retained its output ownership");
    }
}

void testGameCapRetainsFallbackAfterIsolatedCapBatch() {
    using Clock = RealFramePacer::Clock;
    constexpr auto period = 16666666ns;
    for (const size_t count : {2U, 3U, 4U, 5U}) {
        RealFramePacer pacer;
        auto arrival = Clock::time_point{10s};
        for (size_t frame = 0; frame < 200; ++frame) {
            // An absolute 60 FPS game limiter below framegen-vulkan's 70 FPS cap.
            // A partial batch returns promptly, allowing one fast source
            // arrival followed by a compensating long game-limiter interval.
            if (frame) arrival += frame == 80 ? 1ms : frame == 81 ? 2 * period - 1ms : period;
            const auto deadline = pacer.schedule(arrival, 70.);
            pacer.observeSource(arrival, true, count);
            auto batch = frame != 79 && deadline > arrival
                ? pacer.scheduleOutputs(arrival + 500us, deadline, 70. * count, count, false)
                : std::nullopt;
            if (!batch && frame != 79)
                batch = pacer.scheduleReadyOutputs(arrival + 500us, count);
            if (frame >= 83)
                expect(batch && batch->interval == period / static_cast<int64_t>(count),
                    "one nonbinding-cap batch permanently disabled game-owned output spacing");
            pacer.recordOutputWait(batch
                ? batch->at(count - 1) - arrival - 500us : Clock::duration{});
        }

        // Cold startup may initially give the cap ownership before a source
        // period exists. A subsequently learned slower game period must not
        // keep that ownership alive indefinitely either.
        RealFramePacer cold;
        arrival = Clock::time_point{20s};
        (void)cold.schedule(arrival, 70.);
        cold.observeSource(arrival, true, count);
        arrival += 1ms;
        const auto deadline = cold.schedule(arrival, 70.);
        cold.observeSource(arrival, true, count);
        const auto first = cold.scheduleOutputs(arrival + 500us, deadline, 70. * count, count, false);
        expect(first.has_value(), "cold cap did not establish its ordinary output owner");
        cold.recordOutputWait(first->at(count - 1) - arrival - 500us);
        for (size_t frame = 0; frame < 40; ++frame) {
            arrival += period;
            (void)cold.schedule(arrival, 70.);
            cold.observeSource(arrival, true, count);
            const auto batch = cold.scheduleReadyOutputs(arrival + 500us, count);
            if (frame >= 28)
                expect(batch && batch->interval == period / static_cast<int64_t>(count),
                    "cold cap ownership survived a genuinely slower game limiter");
            cold.recordOutputWait(batch
                ? batch->at(count - 1) - arrival - 500us : Clock::duration{});
        }
    }
}

void testStableGameCadenceWithChangingReadiness() {
    using Clock = RealFramePacer::Clock;
    constexpr auto period = 16666666ns;
    for (const size_t count : {2U, 3U, 4U, 5U}) {
        RealFramePacer pacer;
        auto arrival = Clock::time_point{30s};
        for (size_t frame = 0; frame < 600; ++frame) {
            pacer.observeSource(arrival, true, count);
            // The absolute game limiter absorbs readiness changes in its
            // outside wait. A shorter wait-excluded interval is not a game
            // speedup when complete source intervals remain unchanged.
            const auto readiness = period / static_cast<int64_t>(count * (frame / 40 % 2 ? 2 : 4));
            const auto batch = pacer.scheduleReadyOutputs(arrival + readiness, count);
            if (frame >= 8)
                expect(batch && batch->interval == period / static_cast<int64_t>(count),
                    "changing GPU readiness restarted unpaced qualification at a constant game rate");
            pacer.recordOutputWait(batch
                ? readiness + batch->interval * static_cast<int64_t>(count - 1) : Clock::duration{});
            arrival += period;
        }
    }
}

void testReadyFixedSourceJitterAndCapIncrease() {
    using Clock = RealFramePacer::Clock;
    constexpr auto period = 16666666ns;
    const auto start = Clock::time_point{10s};
    auto arrival = start;
    for (const auto recordedWait : {Clock::duration{}, Clock::duration{period / 2}}) {
        RealFramePacer jitter;
        arrival = start;
        for (size_t frame = 0; frame < 9; ++frame) {
            jitter.observeSource(arrival, true);
            arrival += period;
        }
        for (size_t frame = 0; frame < 600; ++frame) {
            jitter.observeSource(arrival, true);
            const auto batch = jitter.scheduleReadyOutputs(arrival + 1ms, 2);
            expect(batch && batch->interval == period / 2,
                "source jitter biased the learned output interval toward faster outliers");
            jitter.recordOutputWait(recordedWait);
            arrival += period + (frame % 2 == 0 ? 2ms : -2ms);
        }
    }

    // The game owns a 20 FPS cap while framegen-vulkan is uncapped or already permits
    // 40 FPS. Raising/removing the game cap must release the old 25 ms output
    // sleep, rather than learning each step of its own gradual acceleration.
    for (const double cap : {0., 40.}) {
        RealFramePacer pacer;
        arrival = start;
        for (size_t frame = 0; frame < 40; ++frame) {
            (void)pacer.schedule(arrival, cap);
            pacer.observeSource(arrival, true);
            const auto batch = pacer.scheduleReadyOutputs(arrival + 1ms, 2);
            if (batch) pacer.recordOutputWait(batch->interval);
            arrival += 50ms;
        }
        for (size_t frame = 0; frame < 12; ++frame) {
            // One last paced frame can hold the first faster arrival back.
            arrival += frame == 0 ? -23ms : 25ms;
            (void)pacer.schedule(arrival, cap);
            pacer.observeSource(arrival, true);
            const auto batch = pacer.scheduleReadyOutputs(arrival + 1ms, 2);
            if (frame == 1)
                expect(!batch, "faster source retained the previous limiter's output sleep");
            if (frame >= 9)
                expect(batch && batch->interval == 12500us,
                    "source-cap increase did not requalify directly at the new cadence");
            if (batch) pacer.recordOutputWait(batch->interval);
        }
    }
}

void testDetachedOutputSourceJitter() {
    using Clock = RealFramePacer::Clock;
    constexpr auto period = 16666666ns;
    RealFramePacer pacer;
    auto arrival = Clock::time_point{30s};
    for (size_t frame = 0; frame < 9; ++frame) {
        pacer.observeSource(arrival, true);
        arrival += period;
    }
    // Paired early arrivals followed by paired late arrivals preserve 60 FPS.
    // Real captures show these small changes without a change in game cap.
    // A detached real output contributes no synchronous output-slot sleep.
    for (size_t frame = 0; frame < 1200; ++frame) {
        pacer.observeSource(arrival, true);
        const auto batch = pacer.scheduleReadyOutputs(arrival + 4ms, 2);
        expect(batch.has_value(), "source jitter released eight independently paced batches");
        if (frame > 16)
            expect(batch->interval == period / 2,
                "independent source median ratcheted toward faster jitter");
        pacer.recordOutputWait(4ms, true);
        if (frame % 4 < 2) pacer.recordAcquireWait(1ms);
        arrival += period + (frame % 4 < 2 ? -1500us : 1500us);
    }
    // Four or more early samples also occur without a change in mean rate.
    // Recovery must keep a usable output interval even when that run passes
    // the acceleration threshold. Extending a consecutive-sample threshold
    // merely moves the same eight-frame gap to a different jitter pattern.
    for (const size_t run : {4U, 6U, 16U}) {
        RealFramePacer jitter;
        auto source = Clock::time_point{120s};
        for (size_t frame = 0; frame < 9; ++frame) {
            jitter.observeSource(source, true);
            source += period;
        }
        for (size_t frame = 0; frame < 1200; ++frame) {
            jitter.observeSource(source, true);
            const auto batch = jitter.scheduleReadyOutputs(source + 6ms, 2);
            expect(batch.has_value(), "a longer jitter run discarded qualified detached pacing");
            if (run == 4 && frame >= 16 && frame % 8 == 7)
                expect(batch->interval == period / 2,
                    "acceleration discarded slower samples from a balanced source window");
            jitter.recordOutputWait(6ms, true);
            if (frame % (2 * run) < run) jitter.recordAcquireWait(1ms);
            source += period + (frame % (2 * run) < run ? -1500us : 1500us);
        }
    }
    // A sustained faster source retunes without discarding output pacing.
    for (const auto changed : {14286us, 12500us, 14286us, 16667us}) {
        for (size_t frame = 0; frame < 40; ++frame) {
            arrival += changed;
            pacer.observeSource(arrival, true);
            const auto batch = pacer.scheduleReadyOutputs(arrival + 2ms, 2);
            expect(batch.has_value(), "a detached rate change discarded output spacing");
            if (frame >= 16)
                expect(batch && batch->interval == std::chrono::duration_cast<Clock::duration>(changed) / 2,
                    "independent output retained stale faster or slower spacing");
            pacer.recordOutputWait(2ms, batch.has_value());
        }
    }

    // A sustained slower source still updates its full-source median when
    // some of its time is spent joining a detached output.
    for (size_t frame = 0; frame < 40; ++frame) {
        arrival += 50ms;
        pacer.observeSource(arrival, true);
        const auto batch = pacer.scheduleReadyOutputs(arrival + 1ms, 2);
        if (frame >= 24)
            expect(batch && batch->interval == 25ms,
                "detached output retained stale spacing after sustained slower arrivals");
        pacer.recordOutputWait(1ms, batch.has_value());
        if (batch) pacer.recordAcquireWait(20ms);
    }
    RealFramePacer recovery;
    arrival = Clock::time_point{60s};
    for (const auto work : {50ms, 5ms, 50ms, 5ms}) {
        for (size_t frame = 0; frame < 120; ++frame) {
            recovery.observeSource(arrival, true);
            const auto ready = arrival + 100us;
            const auto batch = recovery.scheduleReadyOutputs(ready, 2);
            if (work == 5ms)
                expect(batch.has_value(), "detached recovery discarded output spacing");
            if (frame >= 24)
                expect(batch && batch->interval <= (work + 100us) / 2,
                    "detached acquisition feedback retained a slow-scene output slot");
            if (work == 5ms && frame == 3)
                expect(batch && batch->interval < 20ms,
                    "a large detached-source recovery retained old pacing debt");
            const auto nextWork = ready + work;
            const auto acquired = batch ? std::max(nextWork, batch->at(1)) : nextWork;
            recovery.recordOutputWait(100us, batch.has_value());
            if (acquired > nextWork) recovery.recordAcquireWait(acquired - nextWork);
            arrival = acquired;
        }
    }
    // Readiness and a late acquisition hide most of the work improvement.
    // With an old 28 ms source period, 8 ms readiness plus the retained
    // 14 ms slot makes the first recovered arrival 22 ms: less than a 25%
    // improvement. Feed that actual join back instead of prescribing a new
    // source rate. Sustained recovery must retune promptly and retain pacing.
    RealFramePacer loaded;
    arrival = Clock::time_point{90s};
    for (const auto work : {20ms, 10ms, 20ms, 10ms}) {
        for (size_t frame = 0; frame < 120; ++frame) {
            loaded.observeSource(arrival, true);
            const auto ready = arrival + 8ms;
            const auto batch = loaded.scheduleReadyOutputs(ready, 2);
            if (work == 10ms && frame == 5)
                expect(batch && batch->interval <= 11ms,
                    "readiness and acquisition concealed a post-load recovery");
            if (work == 10ms)
                expect(batch.has_value(), "readiness-heavy recovery discarded output spacing");
            if (frame >= 32)
                expect(batch && batch->interval <= (work + 8ms) / 2,
                    "post-load pacing continued to learn its own acquisition delay");
            const auto nextWork = ready + work;
            const auto acquired = batch ? std::max(nextWork, batch->at(1)) : nextWork;
            loaded.recordOutputWait(8ms, batch.has_value());
            if (acquired > nextWork) loaded.recordAcquireWait(acquired - nextWork);
            arrival = acquired;
        }
    }
}

void testCompletionThreadDoesNotTeachItsOutputWait() {
    using Clock = RealFramePacer::Clock;
    RealFramePacer pacer;
    auto arrival = Clock::time_point{140s};
    for (size_t frame = 0; frame < 240; ++frame) {
        (void)pacer.schedule(arrival, 0);
        if (frame >= 16) pacer.recordCompletionPacingWait(8ms);
        pacer.observeSource(arrival, true, 2);
        const auto batch = pacer.scheduleReadyOutputs(arrival + 1ms, 2, true);
        if (frame >= 8)
            expect(batch && batch->interval == 8ms,
                "another thread's completion event taught the source estimator its own output delay");
        if (frame >= 18)
            expect(pacer.completionPacingBlocksSource(),
                "two slower completion-gated sources did not release the conflicting ready-output owner");
        pacer.recordOutputWait(0ns, batch.has_value(), batch.has_value());
        arrival += frame >= 15 ? 24ms : 16ms;
    }
    for (unsigned interruption = 0; interruption < 2; ++interruption) {
        arrival += 1s;
        pacer.recordCompletionPacingWait(8ms);
        pacer.observeSource(arrival, true, 2);
        expect(!pacer.completionPacingBlocksSource(),
            "an interrupted source was classified as serialized completion pacing");
    }
}

void testDetachedBatchPhaseContinuity() {
    using Clock = RealFramePacer::Clock;
    constexpr auto period = 22222200ns; // Integral slots for every 2x-5x rung.
    for (const size_t count : {2U, 3U, 4U, 5U}) {
        RealFramePacer pacer;
        const auto slot = period / static_cast<int64_t>(count);
        auto arrival = Clock::time_point{150s};
        std::optional<Clock::time_point> previous;
        for (size_t frame = 0; frame < 240; ++frame) {
            // Exercise the production uncapped entry as well as the ready
            // planner. An inactive cap must not erase its output phase.
            (void)pacer.schedule(arrival, 0);
            pacer.observeSource(arrival, true, count);
            const auto readiness = frame % 2 ? 7ms : 4ms;
            const auto ready = arrival + readiness;
            const auto batch = pacer.scheduleReadyOutputs(ready, count, true);
            if (batch) {
                expect(batch->first >= ready && batch->first - ready < slot,
                    "ready phase waited before GPU completion or banked a full output slot");
                if (frame >= 16) {
                    expect(previous && batch->first + 100us - *previous ==
                            slot - (frame == 121 ? 200us : 0us),
                        "changing GPU readiness left a short/long gap between detached batches: count=" +
                            std::to_string(count) + " frame=" + std::to_string(frame) +
                            " gap_ns=" + std::to_string(previous
                                ? (batch->first - *previous).count() : 0) +
                            " slot_ns=" + std::to_string(slot.count()));
                    expect(batch->interval == slot,
                        "phase preservation changed the learned source rate");
                }
                previous = batch->at(count - 1) + 100us;
                // Ordinary wake lateness must not accumulate into the source
                // estimate or the output phase, even after hundreds of slots.
                if (frame == 120) *previous += 200us;
                pacer.completeOutputBatch(*previous);
            }
            pacer.recordOutputWait(readiness, batch.has_value(), batch.has_value());
            arrival += period;
        }
    }
}

void testDetachedBatchPhaseRecovery() {
    using Clock = RealFramePacer::Clock;
    for (const size_t count : {2U, 3U, 4U, 5U}) {
        for (const bool absoluteLimiter : {false, true}) {
            RealFramePacer pacer;
            auto deadline = Clock::time_point{155s};
            auto returned = deadline - 50ms;
            auto retained = returned;
            auto previousReadiness = 4ms;
            std::optional<Clock::time_point> recoveredAt;
            for (size_t frame = 0; frame < 400; ++frame) {
                const auto period = frame < 80 || frame >= 240 ? 50ms : 25ms;
                const auto readiness = frame % 2 ? 7ms : 4ms;
                const auto work = period - previousReadiness;
                const auto entry = absoluteLimiter ? std::max(deadline, returned + work)
                    : returned + work;
                const auto joined = std::max(entry, retained);
                pacer.recordAcquireWait(joined - entry);
                (void)pacer.schedule(joined, 0);
                pacer.observeSource(joined, true, count);
                const auto ready = std::max(joined, entry + readiness);
                const auto batch = pacer.scheduleReadyOutputs(ready, count, true);
                // The complete ready batch is retained: its first deadline
                // must not delay the application's next-frame CPU/GPU work.
                returned = ready;
                retained = batch ? batch->at(count - 1) + 100us : returned;
                if (batch) pacer.completeOutputBatch(retained);
                pacer.recordOutputWait(returned - joined, batch.has_value(), batch.has_value());
                if (frame >= 112 && frame < 240) {
                    expect(batch && batch->interval <= Clock::duration{26ms} / static_cast<int64_t>(count),
                        "continuous output phase concealed a faster source behind its own holds: count=" +
                            std::to_string(count) + " frame=" + std::to_string(frame) +
                            " absolute=" + std::to_string(absoluteLimiter) + " interval_ns=" +
                            std::to_string(batch ? batch->interval.count() : 0));
                    if (frame == 112) recoveredAt = joined;
                    if (frame == 239)
                        expect(recoveredAt && joined - *recoveredAt <= 127 * 26ms,
                            "phase recovery retained the old slower producer rate");
                }
                if (frame >= 288)
                    expect(batch && batch->interval >= Clock::duration{49ms} / static_cast<int64_t>(count),
                        "slower source failed to replace the previous output phase: count=" +
                            std::to_string(count) + " frame=" + std::to_string(frame) +
                            " absolute=" + std::to_string(absoluteLimiter) + " interval_ns=" +
                            std::to_string(batch ? batch->interval.count() : 0));
                previousReadiness = readiness;
                deadline = std::max(deadline + period, entry);
            }
        }
    }
}

void testDetachedLoadedReadiness() {
    using Clock = RealFramePacer::Clock;
    constexpr auto period = 17543859ns; // Gameplay's observed ~57 FPS.
    for (const size_t count : {2U, 3U, 4U, 5U}) {
        RealFramePacer pacer;
        auto arrival = Clock::time_point{170s};
        const auto slot = period / static_cast<int64_t>(count);
        // GPU work misses the old one-slot readiness budget but still leaves
        // room for the generated outputs and the game's next-frame work.
        const auto readiness = slot + 2ms;
        const auto generatedSpan = Clock::duration{};
        const auto gameWork = period - readiness - generatedSpan;
        for (size_t frame = 0; frame < 160; ++frame) {
            pacer.observeSource(arrival, true, count);
            const auto budget = pacer.sourceReadinessBudget(arrival, count, true);
            const auto waited = std::min(Clock::duration{readiness}, budget);
            const auto batch = waited == readiness
                ? pacer.scheduleReadyOutputs(arrival + waited, count, true) : std::nullopt;
            if (frame >= 16) {
                expect(batch && batch->interval == slot,
                    "gameplay readiness exhausted the detached FIFO output budget");
                expect(std::max(batch->at(0), arrival + readiness) + gameWork == arrival + period,
                    "retained real output serialized next-frame game work");
                expect(batch->at(count - 1) < arrival + 2 * period,
                    "worker accumulated a second source interval of output debt");
            }
            const auto returned = arrival + waited;
            pacer.recordOutputWait(returned - arrival, batch.has_value(), batch.has_value());
            arrival = std::max(arrival + period, returned + gameWork);
        }
        expect(!pacer.scheduleReadyOutputs(arrival + period, count, true),
            "detached FIFO kept a whole expired source interval");
    }
}

void testDetachedBatchWakeFeedback() {
    using Clock = RealFramePacer::Clock;
    constexpr auto period = 23255820ns; // Integral 43 FPS slots at 2x-5x.
    for (const size_t count : {2U, 3U, 4U, 5U}) {
        RealFramePacer pacer;
        auto deadline = Clock::time_point{180s};
        auto returned = deadline - period;
        auto retained = returned;
        std::optional<Clock::time_point> began;
        for (size_t frame = 0; frame <= 2400; ++frame) {
            const auto entry = std::max(deadline, returned + 4ms);
            const auto joined = std::max(entry, retained);
            pacer.recordAcquireWait(joined - entry);
            (void)pacer.schedule(joined, 60);
            pacer.observeSource(joined, true, count);
            const auto ready = std::max(joined, entry + 9ms) + 1500us;
            const auto batch = pacer.scheduleReadyOutputs(ready, count, true);
            returned = ready;
            retained = batch ? batch->at(count - 1) + 100us : ready;
            if (batch) pacer.completeOutputBatch(retained);
            pacer.recordOutputWait(returned - joined, batch.has_value(), batch.has_value());
            if (frame >= 1200) {
                expect(batch && batch->interval <= period * 102 / static_cast<int64_t>(100 * count),
                    "ordinary worker wake lateness inflated the learned source period");
                if (frame == 1200) began = joined;
                if (frame == 2400)
                    expect(began && joined - *began <= 1200 * period * 102 / 100,
                        "retained output feedback progressively slowed the producer");
            }
            deadline = std::max(deadline + period, entry);
        }
    }
}

void testDetachedTailGpuFeedback() {
    using Clock = RealFramePacer::Clock;
    constexpr auto period = 17543859ns;
    for (const size_t count : {2U, 3U, 4U, 5U}) {
        RealFramePacer pacer;
        auto deadline = Clock::time_point{175s};
        auto returned = deadline - period;
        auto retained = returned;
        const auto began = deadline;
        size_t paced = 0;
        for (size_t frame = 0; frame < 320; ++frame) {
            // Submit real application GPU work at the game's absolute cap,
            // before framegen-vulkan joins the previous output tail. GPU work can run
            // during that join; no host event or prescribed ready arrival.
            const auto entry = std::max(deadline, returned + 4ms);
            const auto gpuDone = entry + 9ms;
            const auto joined = std::max(entry, retained);
            pacer.observeSource(joined, true, count);
            const auto ready = std::max(joined, gpuDone) + 1500us;
            const auto budget = pacer.sourceReadinessBudget(joined, count, true);
            const auto waited = std::min(budget, ready - joined);
            const auto batch = joined + waited == ready
                ? pacer.scheduleReadyOutputs(ready, count, true) : std::nullopt;
            returned = joined + waited;
            retained = batch ? batch->at(count - 1) : returned;
            pacer.recordOutputWait(returned - joined, batch.has_value(), batch.has_value());
            if (frame >= 32 && batch) ++paced;
            deadline = std::max(deadline + period, entry);
        }
        expect(paced == 288, "loaded GPU/retained-tail feedback dropped qualified output batches");
        expect(returned - began < 321 * period,
            "output-tail joins fed their own delay back into source throughput");
    }
}

void testDetachedCadenceAfterReadinessMiss() {
    using Clock = RealFramePacer::Clock;
    constexpr auto period = 16666666ns;
    RealFramePacer pacer;
    auto arrival = Clock::time_point{180s};
    size_t missed = 0;
    for (size_t frame = 0; frame < 160; ++frame) {
        // Four early arrivals can underestimate the output slot until the
        // compensating late arrivals enter the same window. Six milliseconds
        // of preparation then exceeds the shortened half-period budget.
        auto elapsed = period;
        if (frame >= 32 && frame < 36) elapsed -= 5ms;
        if (frame >= 36 && frame < 40) elapsed += 5ms;
        arrival += elapsed;
        pacer.observeSource(arrival, true);
        const auto budget = pacer.sourceReadinessBudget(arrival, 2);
        const auto wait = std::min(budget, Clock::duration{6ms});
        const auto batch = wait == 6ms
            ? pacer.scheduleReadyOutputs(arrival + wait, 2) : std::nullopt;
        if (frame >= 32 && !batch) ++missed;
        pacer.recordOutputWait(wait, batch.has_value(), batch.has_value());
        if (frame >= 48) {
            expect(batch && batch->interval == period / 2,
                "one readiness miss trapped detached cadence in an underestimated interval");
        }
    }
    expect(missed <= 8,
        "a readiness miss cascaded into repeated unpaced qualification windows");
}

void testDetachedCapYieldsToGameLimiter() {
    using Clock = RealFramePacer::Clock;
    constexpr auto gamePeriod = 16666666ns;
    RealFramePacer pacer;
    auto arrival = Clock::time_point{210s};
    for (size_t frame = 0; frame < 40; ++frame) {
        const auto deadline = pacer.schedule(arrival, 70.);
        pacer.observeSource(arrival, true);
        const auto batch = deadline > arrival
            ? pacer.scheduleOutputs(arrival + 1ms, deadline, 140., 2, false, true)
            : std::nullopt;
        pacer.recordOutputWait(1ms, batch.has_value(), batch.has_value());
        arrival = deadline + 1ms;
    }
    for (size_t frame = 0; frame < 40; ++frame) {
        arrival += gamePeriod;
        (void)pacer.schedule(arrival, 70.);
        pacer.observeSource(arrival, true);
        const auto budget = pacer.sourceReadinessBudget(arrival, 2);
        const auto waited = std::min(budget, Clock::duration{6ms});
        const auto batch = waited == 6ms
            ? pacer.scheduleReadyOutputs(arrival + waited, 2) : std::nullopt;
        if (frame >= 16)
            expect(batch && batch->interval == gamePeriod / 2,
                "detached cap ownership suppressed a slower game's ready-output pacing");
        pacer.recordOutputWait(waited, batch.has_value(), batch.has_value());
    }
}

void testReadyFixedChangingWorkload() {
    using Clock = RealFramePacer::Clock;
    for (const size_t count : {2U, 3U, 4U, 5U}) {
        for (const double cap : {0., 90., 180.}) {
            for (const bool relative : {false, true}) {
                RealFramePacer pacer;
                auto arrival = Clock::time_point{10s};
                // A minute of slow work, a large recovery, then repeated
                // camera-like changes. Feed actual output sleeps back into
                // arrivals: prescribing faster arrivals hides a pacing ramp.
                for (const double rate : {20., 180., 70., 80., 70., 80.}) {
                    const auto work = std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double>(1. / rate));
                    const auto cappedPeriod = cap > 0. ? std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double>(1. / cap)) : Clock::duration{};
                    const auto expectedInterval = std::max(work + 100us, cappedPeriod) /
                        static_cast<int64_t>(count);
                    const size_t frames = rate == 20. ? 1200 : 120;
                    bool released = rate == 20.;
                    for (size_t frame = 0; frame < frames; ++frame) {
                        const auto deadline = pacer.schedule(arrival, cap);
                        pacer.observeSource(arrival, true, count);
                        const auto ready = arrival + 100us;
                        auto batch = deadline > arrival
                            ? pacer.scheduleOutputs(ready, deadline, cap * count, count, false)
                            : std::nullopt;
                        if (!batch) batch = pacer.scheduleReadyOutputs(ready, count);
                        if (frame >= 40)
                            expect(batch.has_value(), "settled workload lost smooth output spacing");
                        if (!batch) released = true;
                        auto returned = std::max(ready, deadline);
                        Clock::duration wait{};
                        if (batch) {
                            if (rate == 70. && frame >= 40)
                                expect(batch->interval >= std::max(work, cappedPeriod) /
                                        static_cast<int64_t>(count),
                                    "slower camera work retained the faster scene's uneven output spacing");
                            returned = std::max(returned, batch->at(count - 1));
                            wait = std::max(Clock::duration{}, batch->at(count - 1) - ready);
                            if (rate == 180. && frame >= 12)
                                expect(batch->interval <= expectedInterval,
                                    "output sleeps retained a slow-scene period after fast work returned");
                            if (rate == 80. && frame >= 12)
                                expect(batch->interval <= expectedInterval,
                                    "a modest camera-rate increase retained stale output spacing");
                        }
                        pacer.recordOutputWait(wait);
                        arrival = relative ? returned + work : std::max(arrival + work, returned);
                        if (rate == 180. && frame == 3)
                            expect(released, "a minute-long dip retained pacing debt after recovery");
                    }
                }
            }
        }
    }
}

void testFractionalUsesExistingSourceDeadline() {
    using Clock = RealFramePacer::Clock;
    const auto start = Clock::time_point{10s};
    for (const auto [sourceFps, targetFps] : std::array{
            std::pair{60., 90.}, std::pair{40., 60.}, std::pair{24., 60.},
            std::pair{20., 70.}, std::pair{20., 90.}, std::pair{72., 120.}}) {
        const auto sourceInterval = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / sourceFps));
        const auto outputInterval = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / targetFps));
        RealFramePacer pacer;
        static_cast<void>(pacer.schedule(start, sourceFps));
        auto previousReal = start;
        std::optional<Clock::time_point> previousOutput;
        double credit = 0.5;
        for (size_t frame = 0; frame < 60; ++frame) {
            // The cap reserves the same return deadline regardless of whether
            // the Fractional ledger grants a shorter or longer output batch.
            const auto real = pacer.schedule(previousReal + sourceInterval / 8, sourceFps);
            expect(real == previousReal + sourceInterval,
                "Fractional output placement changed the source-cap cadence");
            credit += targetFps / sourceFps;
            const auto count = static_cast<size_t>(std::floor(credit + 1e-9));
            credit -= static_cast<double>(count);
            const auto batch = pacer.scheduleOutputs(previousReal + 1ms, real, targetFps, count, true);
            expect(batch && batch->count == count &&
                    batch->first > previousReal && batch->at(count - 1) <= real,
                "Fractional outputs escaped the reserved source interval");
            for (size_t output = 0; output < count; ++output) {
                const auto deadline = batch->at(output);
                if (previousOutput) {
                    expect(deadline > *previousOutput,
                        "Fractional output overtook an earlier image");
                    if (frame > 2) {
                        const auto error = deadline - *previousOutput - outputInterval;
                        expect(error >= -100ns && error <= 100ns,
                            "alternating Fractional batches lost the target-output clock");
                    }
                }
                previousOutput = deadline;
            }
            previousReal = real;
        }
        const auto lateArrival = previousReal + 2s;
        const auto real = pacer.schedule(lateArrival, sourceFps);
        const auto resumed = pacer.scheduleOutputs(lateArrival, real, targetFps, 2, true);
        expect(real == lateArrival && !resumed,
            "loading stall retained catch-up debt or extended the source cap");
        pacer.resetOutputs();
        const auto changed = pacer.scheduleOutputs(real, real + sourceInterval, targetFps, 1, true);
        expect(changed && changed->first == real + sourceInterval,
            "leaving Fractional retained an earlier output clock");
    }
    RealFramePacer pacer;
    expect(!pacer.scheduleOutputs(start, start, 90., 2, true),
        "output pacing invented an unconfigured source cap");
    static_cast<void>(pacer.schedule(start, 60.));
    expect(!pacer.scheduleOutputs(start, start + 17ms, 90., 5, true) &&
            !pacer.scheduleOutputs(start, start + 17ms, 90., 0, true),
        "output pacing borrowed extra source time for an unplaceable batch");
    for (size_t frame = 1; frame <= 6; ++frame) {
        const auto deadline = start + 17ms * frame;
        expect(!pacer.scheduleOutputs(deadline - 16ms, deadline, 90., 1, false),
            "native-only startup/probe frames must retain the source-cap deadline");
    }
    const auto resumed = pacer.scheduleOutputs(start + 134ms, start + 150ms, 90., 1, true);
    expect(resumed && resumed->first == start + 150ms,
        "native-only pacing retained stale Fractional output debt");
    expect(pacer.scheduleOutputs(start + 151ms, start + 167ms, 120., 2, false).has_value(),
        "an accepted constant generated cadence lost capped output pacing");
    // A cap never guarantees the game reaches it. Both late arrivals and
    // preparation that consumes the cap's slack must keep ordinary delivery.
    for (const auto [cap, actual] : std::array{
            std::pair{30., 27.}, std::pair{45., 38.}, std::pair{45., 37.},
            std::pair{60., 58.}, std::pair{60., 57.}}) {
        RealFramePacer late;
        static_cast<void>(late.schedule(start, cap));
        const auto arrival = start + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1. / actual));
        const auto deadline = late.schedule(arrival, cap);
        expect(deadline == arrival &&
                !late.scheduleOutputs(arrival, deadline, 2 * cap, 2, true),
            "below-cap source acquired an additional output wait");
        const auto next = late.schedule(arrival + 1ms, cap);
        expect(late.scheduleOutputs(arrival + 1ms, next, 2 * cap, 2, true).has_value(),
            "return to the cap retained stale output debt");
        expect(!late.scheduleOutputs(next + 1ms, next, 2 * cap, 2, true),
            "slow preparation compressed an expired output batch");
    }
    for (const double invalid : {0., -1., 1001.,
            std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN()}) {
        expect(!pacer.scheduleOutputs(start, start + 17ms, invalid, 2, true),
            "Fractional output pacing accepted an invalid target");
    }
}

void testBridgeSourceDeadlinePreservesDisplayClock() {
    using Clock = OrderedPresentTimeline::Clock;
    const auto start = Clock::time_point{10s};
    OrderedPresentTimeline ordinary;
    OrderedPresentTimeline capped;
    ordinary.observeSourceBatch(start, 1);
    capped.observeSourceBatch(start, 1);
    (void)ordinary.schedule(start, 60, 120, 2);
    (void)capped.schedule(start, 60, 120, 2);
    // A below-refresh batch can outlast the normal submission lookahead.
    // Its producer's existing deadline permits earlier staging, never an
    // earlier display target or a second output clock.
    auto expected = ordinary.schedule(start, 60, 120, 2);
    auto actual = capped.schedule(start, 60, 120, 2, true, start + 5ms);
    expect(expected->submitAt > start + 5ms && actual->submitAt == start + 5ms &&
            actual->presentAt == expected->presentAt,
        "bridge source budget changed display timing or consumed the next source interval");
    expected = ordinary.schedule(start, 60, 120, 2);
    actual = capped.schedule(start, 60, 120, 2, true, start - 1ms);
    expect(actual->submitAt == start && actual->presentAt == expected->presentAt,
        "expired source budget moved submission into the past or compressed display targets");
    expected = ordinary.schedule(start, 60, 120, 2);
    actual = capped.schedule(start, 60, 120, 2);
    expect(actual->submitAt == expected->submitAt && actual->presentAt == expected->presentAt,
        "bridge retained a source budget after its producer released it");
}

void testBridgeStagedReadiness() {
    using Clock = OrderedPresentTimeline::Clock;
    const auto start = Clock::time_point{10s};
    for (const double refresh : {30., 60., 90., 120., 144., 240.}) {
        const auto period = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / refresh));
        for (size_t batchSize = 2; batchSize <= 5; ++batchSize) {
            // Different compositor holds must learn identical queue readiness.
            for (const auto hold : {0ms, 20ms, 60ms}) {
                OrderedPresentTimeline timeline;
                timeline.observeSourceBatch(start, 1);
                const auto slot = timeline.schedule(start, refresh, refresh, batchSize);
                constexpr uint64_t serial = uint64_t{UINT32_MAX} + 5;
                timeline.submittedTiming(serial, slot->presentAt, start);
                timeline.feedbackTiming(serial, start + 2ms, start + 2ms + hold);
                timeline.feedbackTiming(serial, start + 90ms, start + 90ms); // Duplicate.
                const auto next = start + 500ms;
                timeline.observeSourceBatch(next, 2);
                const auto learned = timeline.schedule(next, refresh, refresh, batchSize);
                expect(learned->presentAt == next + period,
                    "V2 readiness included compositor waiting or duplicate evidence");
                // Idle recovery and native history retain the existing guard.
                timeline.observeSourceBatch(next + 2s, 3);
                const auto recovered = timeline.schedule(next + 2s, refresh, refresh, batchSize);
                expect(recovered->presentAt == next + 2s + 2 * period,
                    "V2 feedback lost bounded recovery readiness");
            }
        }
    }
    for (const int invalid : {0, 1, 2, 3, 4}) {
        OrderedPresentTimeline timeline;
        timeline.observeSourceBatch(start, 1);
        const auto slot = timeline.schedule(start, 120, 120, 2);
        constexpr uint64_t serial = uint64_t{UINT32_MAX} + 8;
        timeline.submittedTiming(serial, slot->presentAt, start);
        timeline.feedbackTiming(invalid == 4 ? static_cast<uint32_t>(serial) : serial,
            invalid == 0 ? Clock::time_point{} : invalid == 1 ? start - 1ns :
            invalid == 2 ? start + 101ms : start + 2ms,
            invalid == 3 ? start + 1ms : start + 200ms);
        timeline.observeSourceBatch(start + 500ms, 2);
        const auto next = timeline.schedule(start + 500ms, 120, 120, 2);
        expect(next->presentAt - (start + 500ms) > 30ms,
            "drop, malformed clock or truncated serial fabricated V2 readiness");
    }
    OrderedPresentTimeline outOfOrder;
    {
        OrderedPresentTimeline droppedFirst;
        droppedFirst.observeSourceBatch(start, 1);
        auto slot = droppedFirst.schedule(start, 120, 120, 2);
        droppedFirst.submittedTiming(1, slot->presentAt, start);
        slot = droppedFirst.schedule(start + 20ms, 120, 120, 2);
        droppedFirst.submittedTiming(2, slot->presentAt, start + 20ms);
        droppedFirst.feedbackTiming(1, {}, start + 40ms);
        droppedFirst.feedbackTiming(2, start + 22ms, start + 40ms);
        droppedFirst.observeSourceBatch(start + 500ms, 2);
        slot = droppedFirst.schedule(start + 500ms, 120, 120, 2);
        expect(slot->presentAt - (start + 500ms) < 9ms,
            "later V2 feedback learned CPU spacing after an earlier dropped output");
    }
    uint64_t serial = 0;
    for (uint64_t batch = 1; batch <= 3; ++batch) {
        const auto now = start + batch * 20ms;
        outOfOrder.observeSourceBatch(now, batch);
        for (size_t output = 0; output < 2; ++output) {
            const auto slot = outOfOrder.schedule(now, 120, 120, 2);
            outOfOrder.submittedTiming(++serial, slot->presentAt, now);
        }
    }
    outOfOrder.feedbackTiming(1, start + 22ms, start + 100ms);
    outOfOrder.feedbackTiming(6, {}, start + 100ms); // Drop newer output first.
    outOfOrder.feedbackTiming(6, {}, start + 100ms); // Duplicate must not retire twice.
    outOfOrder.observeSourceBatch(start + 500ms, 4);
    auto slot = outOfOrder.schedule(start + 500ms, 120, 120, 2);
    expect(slot->presentAt - (start + 500ms) > 24ms,
        "out-of-order V2 drop retired unacknowledged older outputs");
    for (uint64_t id = 2; id <= 5; ++id)
        outOfOrder.feedbackTiming(id, {}, start + 501ms);
    outOfOrder.observeSourceBatch(start + 700ms, 5);
    slot = outOfOrder.schedule(start + 700ms, 120, 120, 2);
    expect(slot->presentAt - (start + 700ms) < 9ms,
        "explicit V2 drops retained nonexistent queue occupancy");
    OrderedPresentTimeline missing;
    serial = 0;
    for (uint64_t batch = 1; batch <= 33; ++batch) {
        const auto now = start + batch * 20ms;
        missing.observeSourceBatch(now, batch);
        for (size_t output = 0; output < 2; ++output) {
            slot = missing.schedule(now, 120, 120, 2);
            missing.submittedTiming(++serial, slot->presentAt, now);
        }
    }
    missing.feedbackTiming(66, start + 662ms, start + 700ms);
    for (uint64_t id = 35; id < 66; ++id)
        missing.feedbackTiming(id, {}, start + 700ms);
    missing.feedbackTiming(1, start + 750ms, start + 750ms); // Expired ring entry.
    missing.observeSourceBatch(start + 900ms, 34);
    slot = missing.schedule(start + 900ms, 120, 120, 2);
    expect(slot->presentAt - (start + 900ms) < 9ms,
        "V2 ring expiry retained phantom occupancy or accepted stale feedback");
}

int main() {
    {
        fgvk::layer::RealFramePacer pacer;
        using Clock = fgvk::layer::RealFramePacer::Clock;
        const auto origin = Clock::time_point{} + std::chrono::hours(100000);
        expect(pacer.schedule(origin, 40) == origin, "initial cap does not wait");
        const auto arrival = origin + std::chrono::milliseconds(2);
        const auto changed = pacer.schedule(arrival, 38, true);
        const auto interval = std::chrono::duration<double>(changed - origin).count();
        expect(interval > .026 && interval < .027,
            "small governor rate change preserves the outstanding source boundary");
        const auto restored = pacer.schedule(changed + std::chrono::milliseconds(2), 40, true);
        const auto restoredInterval = std::chrono::duration<double>(restored - changed).count();
        expect(restoredInterval > .024 && restoredInterval < .026,
            "governor relaxation preserves source cadence without a free frame");
        expect(pacer.schedule(restored, 0, true) == restored,
            "disabling a governed cap releases the source immediately");
    }

    testLongSessionDeadlineBounds();
    testFractionalUsesExistingSourceDeadline();
    testFixedUsesExistingSourceDeadline();
    testFixedCapRecoversAfterLateArrival();
    testLateNativeFixedCapKeepsSpacing();
    testReadyFixedOutputs();
    testWorkerReadinessPreservesCapAndOutputPhase();
    testReadyFixedCapOwnershipRequalification();
    testCompletionThreadDoesNotTeachItsOutputWait();
    testGameCapRetainsFallbackAfterIsolatedCapBatch();
    testStableGameCadenceWithChangingReadiness();
    testReadyFixedSourceJitterAndCapIncrease();
    testDetachedOutputSourceJitter();
    testDetachedLoadedReadiness();
    testDetachedBatchPhaseContinuity();
    testDetachedBatchPhaseRecovery();
    testDetachedBatchWakeFeedback();
    testDetachedTailGpuFeedback();
    testDetachedCadenceAfterReadinessMiss();
    testDetachedCapYieldsToGameLimiter();
    testReadyFixedChangingWorkload();
    testOrderedPresentationTimeline();
    testBridgeCompositorReadiness();
    testBridgeCompletionReturnDoesNotDrift();
    testBridgeStagedReadiness();
    testBridgeSourceDeadlinePreservesDisplayClock();
    testOrderedAcquireUsesExplicitFailureOnly();
    testOrderedAcquireBackoffAndProbeAreFinite();
    testExternalInterruptionDiscardsOldFailure();
    expect(!shouldRejectManagedMultiSwapchainPresent(1, true) &&
            !shouldRejectManagedMultiSwapchainPresent(2, false) &&
            shouldRejectManagedMultiSwapchainPresent(2, true),
        "managed multi-swapchain batches must fail closed before shared waits are consumed");

    expect(!orderedGeneratedBatchNeedsNonblockingAdmission(3, 5, 0) &&
            !orderedGeneratedBatchNeedsNonblockingAdmission(3, 5, 1) &&
            !orderedGeneratedBatchNeedsNonblockingAdmission(3, 5, 2) &&
            !orderedGeneratedBatchNeedsNonblockingAdmission(4, 5, 1) &&
            orderedGeneratedBatchNeedsNonblockingAdmission(3, 4, 2) &&
            !orderedGeneratedBatchNeedsNonblockingAdmission(4, 6, 1) &&
            orderedGeneratedBatchNeedsNonblockingAdmission(4, 3, 1),
        "ordered admission did not distinguish a fitting batch from insufficient headroom");
    for (uint32_t applicationImages = 2; applicationImages <= 4; ++applicationImages) {
        for (size_t generated = 1; generated <= 4; ++generated) {
            const size_t images = applicationImages + generated;
            expect(orderedGeneratedBatchCanPrepareBeforeCap(
                    applicationImages, images, generated, false) &&
                    !orderedGeneratedBatchCanPrepareBeforeCap(
                        applicationImages, images - 1, generated, false) &&
                    !orderedGeneratedBatchCanPrepareBeforeCap(
                        applicationImages, images, generated, true),
                "source-cap preparation moved undersized or pressured admission before image release");
            expect(!orderedGeneratedBatchCanPrepareBeforeCap(
                    applicationImages, images, generated + 1, false),
                "source-cap preparation ignored a possible higher generated batch");
            expect(!orderedGeneratedBatchNeedsNonblockingAdmission(
                    applicationImages, images, generated),
                "a fitting generated batch was dropped solely for lacking a relief image");
            expect(orderedGeneratedBatchNeedsNonblockingAdmission(
                    applicationImages, images - 1, generated),
                "an undersized pool lost its nonblocking admission protection");
            expect(orderedGeneratedBatchAcquireBudget(
                    applicationImages, images, generated, std::nullopt) == 50'000'000 &&
                    orderedGeneratedBatchAcquireBudget(
                        applicationImages, images, generated, 100'000'000) == 50'000'000 &&
                    orderedGeneratedBatchAcquireBudget(
                        applicationImages, images, generated, 5'000'000) == 5'000'000,
                "tight-pool delivery lost its shared finite ceiling or shorter user limit");
            expect(!orderedGeneratedBatchAcquireBudget(
                    applicationImages, images + 1, generated, std::nullopt),
                "relief-bearing ordered delivery changed its existing acquire contract");
        }
    }
    expect(!orderedGeneratedBatchCanPrepareBeforeCap(3, 7, 0, false) &&
            !orderedGeneratedBatchCanPrepareBeforeCap(3, 2, 1, false),
        "source-cap preparation accepted inactive generation or an invalid pool");
    expect(!orderedGeneratedBatchAcquireBudget(3, 3, 0, std::nullopt) &&
            !orderedGeneratedBatchAcquireBudget(3, 2, 1, std::nullopt),
        "empty or malformed pools gained a synthetic acquire budget");
    constexpr uint64_t acquireBudget = 50'000'000;
    expect(remainingGeneratedImageAcquireBudget(
            std::nullopt, 32'805'100) == std::nullopt,
        "unconfigured ordered acquire unexpectedly gained a finite budget");
    expect(remainingGeneratedImageAcquireBudget(
            acquireBudget, 0) == acquireBudget,
        "fresh ordered acquire budget did not retain its full deadline");
    expect(remainingGeneratedImageAcquireBudget(
            acquireBudget, 32'805'100) == 17'194'900,
        "second generated image did not receive only the remaining present budget");
    expect(remainingGeneratedImageAcquireBudget(
            acquireBudget, 49'999'999) == 1,
        "ordered acquire budget lost its final nanosecond");
    expect(remainingGeneratedImageAcquireBudget(
            acquireBudget, acquireBudget) == 0 &&
            remainingGeneratedImageAcquireBudget(
                acquireBudget, acquireBudget + 1) == 0,
        "exhausted ordered acquire budget allowed another blocking wait");
    expect(orderedRecoveryAcquireTimeout(120, acquireBudget) == acquireBudget &&
            orderedRecoveryAcquireTimeout(60, acquireBudget) == acquireBudget,
        "recovery probe ignored the configured acquire contract");
    expect(orderedRecoveryAcquireTimeout(120, std::nullopt) == 8'333'334 &&
            orderedRecoveryAcquireTimeout(60, std::nullopt) == 16'666'667,
        "unconfigured recovery probe lost its one-period display contract");
    expect(orderedRecoveryAcquireTimeout(std::nullopt, std::nullopt) == 0,
        "unknown unconfigured recovery invented a timeout");
    expect(orderedGeneratedImageAcquireTimeout(120, acquireBudget) ==
            acquireBudget,
        "per-image acquire did not use the authoritative remaining budget");
    expect(orderedGeneratedImageAcquireTimeout(120, 17'194'900) ==
            17'194'900,
        "the per-image ceiling ignored the remaining cumulative budget");
    expect(orderedGeneratedImageAcquireTimeout(120, 7'500'000) ==
            7'500'000,
        "the per-image ceiling exceeded the remaining cumulative budget");
    for (const uint32_t refresh : {40U, 60U, 90U, 120U, 240U, 360U}) {
        expect(orderedGeneratedImageAcquireTimeout(refresh, acquireBudget) ==
                acquireBudget,
            "refresh-rate heuristic changed the configured acquire budget");
    }
    expect(orderedGeneratedImageAcquireTimeout(
            std::nullopt, acquireBudget) == acquireBudget,
        "unknown refresh changed the configured acquire budget");
    expect(orderedGeneratedImageAcquireTimeout(120, std::nullopt) ==
            generatedImageAcquireSafetyTimeoutNs,
        "an unconfigured ordered path lost its safety timeout");

    GeneratedImageAdmission admission;
    expect(!admission.underPressure(),
        "generated-image admission started under pressure");
    expect(admission.reportUnavailable(),
        "first nonblocking admission miss was not diagnostic");
    expect(admission.underPressure(),
        "admission pressure was not retained");
    admission.reportBypassedFrame();
    expect(admission.reportUnavailable(),
        "second admission miss should be a power-of-two diagnostic");
    expect(!admission.reportUnavailable(),
        "third admission miss should be aggregated");
    admission.reportBypassedFrame();
    const auto pendingRecovery = admission.reportAvailable(2);
    expect(!pendingRecovery.resumed && admission.underPressure(),
        "one available batch prematurely re-armed admission");
    expect(admission.reportUnavailable(),
        "a miss after partial recovery lost pressure diagnostics");
    admission.reportBypassedFrame();
    const auto pendingAgain = admission.reportAvailable(2);
    expect(!pendingAgain.resumed && admission.underPressure(),
        "interrupted admission stability was not reset");
    const auto recovery = admission.reportAvailable(2);
    expect(recovery.resumed && recovery.missedAttempts == 4 &&
            recovery.bypassedFrames == 3 && recovery.stableBatches == 2 &&
            recovery.requiredStableBatches == 2,
        "admission recovery lost its aggregated pressure counters");
    expect(!admission.underPressure(),
        "admission recovery did not reset pressure");
    expect(generatedImageAdmissionRecoveryBatches(7, 2) == 3 &&
            generatedImageAdmissionRecoveryBatches(7, 1) == 4 &&
            generatedImageAdmissionRecoveryBatches(4, 3) == 1 &&
            generatedImageAdmissionRecoveryBatches(0, 2) == 1 &&
            generatedImageAdmissionRecoveryBatches(7, 0) == 1,
        "admission recovery was not derived from lower WSI turnover");

    expect(OrderedAcquireRecovery::maximumRetryDelay() == 30s,
        "ordered acquire recovery must cap direct-failure retry delay");
    expect(!preacquiredImagesRequireRetirement(false, 1) &&
            !preacquiredImagesRequireRetirement(true, 0) &&
            preacquiredImagesRequireRetirement(true, 1),
        "pre-acquired image retirement lost its ownership contract");

    RealFramePacer framePacer;
    const auto pacingStart = RealFramePacer::TimePoint{};
    expect(framePacer.schedule(pacingStart, 60) == pacingStart,
        "the first capped frame must not be delayed");
    const auto secondDeadline = framePacer.schedule(
        pacingStart + 16ms, 60
    );
    const auto thirdDeadline = framePacer.schedule(
        pacingStart + 32ms, 60
    );
    expect(secondDeadline > pacingStart + 16ms &&
            secondDeadline < pacingStart + 17ms,
        "60 FPS pacing did not schedule a 16.67 ms second frame");
    expect(thirdDeadline > pacingStart + 33ms &&
            thirdDeadline < pacingStart + 34ms,
        "early application frames did not remain on the absolute 60 FPS cadence");

    const auto afterStall = framePacer.schedule(pacingStart + 200ms, 60);
    expect(afterStall == pacingStart + 200ms,
        "a late frame was delayed for stale pacing debt");
    const auto afterStallDeadline = framePacer.schedule(
        pacingStart + 205ms, 60
    );
    expect(afterStallDeadline > pacingStart + 216ms &&
            afterStallDeadline < pacingStart + 217ms,
        "pacing did not rebase after a loading stall");

    expect(framePacer.schedule(pacingStart + 210ms, 0) ==
            pacingStart + 210ms,
        "disabling the cap delayed an application frame");
    expect(framePacer.schedule(pacingStart + 211ms, 60) ==
            pacingStart + 211ms,
        "re-enabling the cap retained a stale deadline");
    expect(framePacer.schedule(pacingStart + 220ms, 30) ==
            pacingStart + 220ms,
        "changing the cap retained the previous cadence");
    const auto thirtyFpsDeadline = framePacer.schedule(
        pacingStart + 230ms, 30
    );
    expect(thirtyFpsDeadline > pacingStart + 253ms &&
            thirtyFpsDeadline < pacingStart + 254ms,
        "30 FPS pacing did not establish a new 33.33 ms cadence");

    framePacer.reset();
    expect(framePacer.schedule(pacingStart, 82.5) == pacingStart,
        "the first fractionally capped frame must not be delayed");
    const auto fractionalDeadline = framePacer.schedule(
        pacingStart + 12ms, 82.5
    );
    expect(fractionalDeadline > pacingStart + 12ms &&
            fractionalDeadline < pacingStart + 13ms,
        "82.5 FPS pacing did not retain its fractional interval");

    for (size_t remaining = 0; remaining <= 3; ++remaining) {
        expect(historyWarmupFramesAfterRequest(remaining, 3, true) == 3,
            "a new Fixed menu return retained incomplete pre-menu history");
        expect(historyWarmupFramesAfterRequest(remaining, 3, false) ==
                (remaining == 0 ? 3 : remaining),
            "ordinary Fixed readiness checks restarted an active warm-up");
    }
    auto menuWarmupRemaining = historyWarmupFramesAfterRequest(0, 3, true);
    --menuWarmupRemaining;
    menuWarmupRemaining = historyWarmupFramesAfterRequest(menuWarmupRemaining, 3, true);
    for (size_t frame = 0; frame < 3; ++frame) {
        expect(menuWarmupRemaining == 3 - frame,
            "second Fixed interruption did not require three new real frames");
        --menuWarmupRemaining;
    }
    expect(menuWarmupRemaining == 0,
        "Fixed history warm-up did not complete after three fresh frames");

    FixedRefreshBudget budget;
    const auto start = FixedRefreshBudget::TimePoint{};
    size_t generated = 0;
    for (size_t frame = 0; frame <= 630; ++frame) {
        generated += budget.plan(
            start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(static_cast<double>(frame) / 63.0)
            ),
            120,
            1
        );
    }
    // The first real frame is deliberately ungenerated while timing warms up.
    expect(generated >= 565 && generated <= 575,
        "63 FPS Fixed 2x should budget approximately 120 displayed FPS");

    budget.reset();
    generated = 0;
    for (size_t frame = 0; frame <= 600; ++frame) {
        generated += budget.plan(start + frame * 10ms, 120, 1);
    }
    expect(generated >= 115 && generated <= 125,
        "100 FPS Fixed 2x should synthesize only the displayable remainder");

    // With Smooth Cadence and ordered FIFO, Fixed requests its full multiplier
    // and lets the present queue provide back-pressure instead of sleeping in
    // the game present call. First-frame and long-stall guards still apply.
    for (const uint32_t refreshHz : {60U, 90U, 120U, 144U}) {
        for (size_t multiplier = 2; multiplier <= 5; ++multiplier) {
            budget.reset();
            const size_t maximumGenerated = multiplier - 1;
            expect(budget.plan(start, refreshHz, maximumGenerated, true) == 0,
                "FIFO-paced Fixed generated on its first timing sample");
            for (size_t frame = 1; frame <= 30; ++frame) {
                expect(budget.plan(
                    start + frame * 10ms, refreshHz,
                    maximumGenerated, true
                ) == maximumGenerated,
                    "FIFO-paced Fixed suppressed part of its multiplier");
            }
            expect(budget.plan(
                start + 1s, refreshHz, maximumGenerated, true
            ) == 0,
                "FIFO-paced Fixed ignored a long timing discontinuity");

            // FIFO backpressure can return slightly early. Keep the selected
            // multiplier stable instead of dropping one output because the
            // fractional refresh budget saw a short interval.
            budget.reset();
            expect(budget.plan(start, refreshHz, maximumGenerated, true) == 0,
                "FIFO-paced Fixed generated on its first timing sample");
            for (size_t frame = 1; frame <= 16; ++frame) {
                const double seconds =
                    static_cast<double>(frame * multiplier) /
                        static_cast<double>(refreshHz) +
                    (frame % 2 == 0 ? 0.001 : -0.001);
                const auto when = start +
                    std::chrono::duration_cast<
                        std::chrono::steady_clock::duration
                    >(std::chrono::duration<double>(seconds));
                expect(budget.plan(
                    when, refreshHz, maximumGenerated, true
                ) == maximumGenerated,
                    "FIFO timing jitter changed the Fixed multiplier");
            }
        }
    }

    budget.reset();
    generated = 0;
    for (size_t frame = 0; frame < 600; ++frame) {
        generated += budget.plan(
            start + std::chrono::milliseconds(frame * 5 / 3), 90, 1
        );
    }
    expect(generated == 0,
        "600 FPS toward a 90 Hz display must remain real-only");

    for (const uint32_t refreshHz : {40U, 60U, 90U, 120U}) {
        for (size_t generatedCapacity = 1; generatedCapacity <= 3;
                ++generatedCapacity) {
            budget.reset();
            generated = 0;
            constexpr size_t sampleRealFrames = 600;
            const double realFps = static_cast<double>(refreshHz) /
                static_cast<double>(generatedCapacity + 1);
            for (size_t frame = 0; frame <= sampleRealFrames; ++frame) {
                const auto when = start +
                    std::chrono::duration_cast<
                        std::chrono::steady_clock::duration
                    >(std::chrono::duration<double>(
                        static_cast<double>(frame) / realFps
                    ));
                generated += budget.plan(
                    when, refreshHz, generatedCapacity
                );
            }
            const double seconds =
                static_cast<double>(sampleRealFrames) / realFps;
            const double outputFps = static_cast<double>(
                sampleRealFrames + generated
            ) / seconds;
            expect(std::abs(outputFps - static_cast<double>(refreshHz)) < 0.5,
                "fixed refresh matrix missed its display budget at " +
                    std::to_string(refreshHz) + " Hz / " +
                    std::to_string(generatedCapacity + 1) + "x");
        }
    }

    // Equal early/late timestamp noise must not slowly drain a full Fixed
    // multiplier. Check the settled tail so startup rounding is not mistaken
    // for a recurring skip, across refresh rates and every supported rung.
    for (const uint32_t refreshHz : {60U, 90U, 120U, 144U}) {
        for (size_t multiplier = 2; multiplier <= 5; ++multiplier) {
            for (const double jitterSeconds : {0.0001, 0.0005, 0.001}) {
                budget.reset();
                (void)budget.plan(start, refreshHz, multiplier - 1);
                size_t tailSkipped = 0;
                for (size_t frame = 1; frame <= 2400; ++frame) {
                    const double seconds =
                        static_cast<double>(frame * multiplier) / refreshHz +
                        (frame % 2 ? jitterSeconds : 0.0);
                    const auto when = start + std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(seconds));
                    const size_t count = budget.plan(when, refreshHz, multiplier - 1);
                    if (frame > 1200)
                        tailSkipped += multiplier - 1 - count;
                }
                expect(tailSkipped == 0,
                    "balanced timestamp jitter drained the Fixed refresh budget");
            }
        }
    }

    // A slow period may leave one output of credit, not a multi-frame bank
    // capable of defeating the display ceiling after a source-rate increase.
    budget.reset();
    for (size_t frame = 0; frame <= 100; ++frame)
        (void)budget.plan(start + frame * 100ms, 120, 1);
    generated = 0;
    for (size_t frame = 1; frame <= 1000; ++frame) {
        const size_t count = budget.plan(start + 10s + frame * 10ms, 120, 1);
        if (frame > 100)
            generated += count;
    }
    expect(generated >= 179 && generated <= 181,
        "slow-source timing credit defeated the later display budget");

    testReleasedCadenceUsesBurstThroughput();
    testReleasedOutputsStayInsideTheSourceInterval();
    testReleasedOutputsUseMeasuredSourcePeriod();
    testReleasedUncappedOutputsUseQualifiedPeriod();
    testJustInTimeReleasedBatchIgnoresPendingPhase();
    testSlowerReleasedBlocksNeedConsecutiveConfirmation();
    std::cout << "presentation policy tests passed\n";
    return 0;
}

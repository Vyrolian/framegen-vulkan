/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/low_latency.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>

using namespace fgvk::layer;
using namespace std::chrono_literals;

namespace {
void expect(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

using Clock = LowLatencyRelease::Clock;
using Duration = LowLatencyRelease::Duration;

void steadyLeadMovesDelayByAnEighthOfTheExcess() {
    LowLatencyRelease release;
    release.observe(9ms, 20ms);
    expect(release.margin() == 1ms, "the first observation changed the margin");
    expect(release.delay() == 1ms, "a 9 ms lead did not raise the delay by an eighth of 8 ms");
}

void earlyThenLateMovesDelayBothWays() {
    LowLatencyRelease release;
    for (int i = 0; i < 20; ++i) release.observe(17ms, 100ms);
    const auto raised = release.delay();
    expect(raised > Duration::zero(), "an early present did not raise the delay");
    release.observe(-5ms, 100ms);
    expect(release.delay() < raised, "a late present did not lower the delay");
}

void jitterWidensMargin() {
    LowLatencyRelease release;
    for (int i = 0; i < 200; ++i) release.observe(i % 2 ? 4ms : 8ms, 20ms);
    expect(release.margin() > 9ms && release.margin() <= 13ms, "a lead alternating by 4 ms did not widen the margin");
}

void delayStaysWithinThreeCapIntervals() {
    LowLatencyRelease release;
    for (int i = 0; i < 1000; ++i) release.observe(1s, 10ms);
    expect(release.delay() == 30ms, "the delay exceeded three cap intervals");
    for (int i = 0; i < 1000; ++i) release.observe(-1s, 10ms);
    expect(release.delay() == Duration::zero(), "repeated late presents took the delay below zero");
}

void noCapResetsDelay() {
    LowLatencyRelease release;
    release.observe(20ms, 10ms);
    expect(release.delay() > Duration::zero(), "setup did not raise the delay");
    release.observe(20ms, Duration::zero());
    expect(release.delay() == Duration::zero(), "a zero cap interval did not reset the delay");
    release.observe(20ms, 10ms);
    release.reset();
    expect(release.delay() == Duration::zero(), "reset did not clear the delay");
}

// The setting's parts for one present. The release delay steps aside while Reflex holds the game, and
// the just-in-time batch start stays on.
void lowLatencyPlanFollowsSettingCapFrameGenerationAndReflex() {
    const auto on = lowLatencyPlan(true, true, true, false);
    expect(on.justInTime && on.releaseDelay && !on.reflexHolds,
        "the setting on, Reflex off, did not run every part");
    expect(!lowLatencyPlan(false, true, true, false).justInTime, "the setting off left low latency on");
    expect(!lowLatencyPlan(false, true, true, true).reflexHolds, "the setting off reported Reflex holding the game");
    expect(!lowLatencyPlan(true, false, true, false).justInTime, "no cap left low latency on");
    expect(!lowLatencyPlan(true, true, false, false).justInTime, "frame generation off left low latency on");
    expect(!lowLatencyPlan(true, false, false, true).reflexHolds, "no cap or frame generation reported Reflex holding");
}

void reflexKeepsJustInTimeBatchesAndStepsReleaseDelayAside() {
    const auto plan = lowLatencyPlan(true, true, true, true);
    expect(plan.justInTime, "Reflex on turned off the just-in-time batch start");
    expect(!plan.releaseDelay, "Reflex on left the release delay running");
    expect(plan.reflexHolds, "Reflex on was not reported as holding the game");
}

// Reflex on forces the delay to zero on every present and resets the controller. Reflex off resumes
// from zero, as a fresh controller would.
void reflexOnForcesZeroAndReflexOffResumesFromZero() {
    const auto off = lowLatencyPlan(true, true, true, false);
    const auto on = lowLatencyPlan(true, true, true, true);
    LowLatencyRelease release;
    Duration delay{};
    for (int i = 0; i < 20; ++i) delay = release.update(off.releaseDelay, 17ms, 100ms);
    expect(delay > Duration::zero(), "setup: early presents with Reflex off did not raise the delay");
    for (int i = 0; i < 20; ++i)
        expect(release.update(on.releaseDelay, 17ms, 100ms) == Duration::zero(),
            "Reflex on did not force the delay to zero");
    expect(release.margin() == 1ms && release.delay() == Duration::zero(),
        "Reflex on did not reset the controller");
    expect(release.update(off.releaseDelay, 9ms, 20ms) == 1ms,
        "Reflex off did not resume the delay from zero");
}

// Native acquire model: the next frame starts at the previous release point less one interval
// (not before the previous present returned at its deadline), renders for `render`, and
// presents at its own deadline. A steady game settles at the base margin.
void nativeAcquireModelSettlesAtMargin() {
    const Duration interval = std::chrono::microseconds{16667};
    const Duration render = 4ms;
    LowLatencyRelease release;
    Duration lead{};
    for (int64_t frame = 1; frame <= 600; ++frame) {
        const Duration previous = (frame - 1) * interval;
        const Duration start = std::max(previous + release.delay() - interval, previous);
        const Duration deadline = frame * interval;
        lead = deadline - (start + render);
        release.observe(lead, interval);
    }
    expect(std::abs((lead - release.margin()).count()) < 100'000,
        "the native acquire model did not settle at the margin");
}

// Three-frame queue model (VKD3D-Proton's default latency): a frame starts when the release
// point of the frame three before it has passed. A steady game settles near the margin
// instead of arriving two intervals early, and never oscillates into late arrivals.
void threeFrameQueueSettlesAtMargin() {
    const Duration interval = std::chrono::microseconds{16667};
    const Duration render = 8ms;
    LowLatencyRelease release;
    const Clock::time_point epoch{};
    std::vector<Clock::time_point> released(1200 + 3, epoch);
    Duration lead{};
    for (size_t frame = 0; frame < 1200; ++frame) {
        const auto deadline = epoch + static_cast<int64_t>(frame + 1) * interval;
        lead = deadline - (released[frame] + render);
        release.observe(lead, interval);
        if (frame > 600) expect(lead >= Duration::zero(), "the three-frame queue model arrived late");
        released[frame + 3] = deadline + release.delay();
    }
    expect(lead < release.margin() + 500us, "the three-frame queue model did not settle near the margin");
}

void acquireHoldIsBoundedByTheTimeout() {
    const auto now = Clock::now();
    auto hold = acquireHold(now, now + 10ms, 0);
    expect(hold.wait == Duration::zero() && !hold.expired, "a poll acquire was held");
    hold = acquireHold(now, now - 1ms, 5'000'000);
    expect(hold.wait == Duration::zero() && !hold.expired, "a passed release point was held");
    hold = acquireHold(now, now + 10ms, 20'000'000);
    expect(hold.wait == 10ms && !hold.expired, "a release point within the timeout was not held to it");
    hold = acquireHold(now, now + 10ms, 4'000'000);
    expect(hold.wait == 4ms && hold.expired, "a release point past the timeout was not cut to the timeout");
    hold = acquireHold(now, now + 10ms, UINT64_MAX);
    expect(hold.wait == 10ms && !hold.expired, "an infinite timeout did not hold to the release point");
}

// ---- L5: GPU release ------------------------------------------------------------------------------

Duration msd(double value) {
    return std::chrono::duration_cast<Duration>(std::chrono::duration<double, std::milli>(value));
}
double milliseconds(Duration value) {
    return std::chrono::duration<double, std::milli>(value).count();
}
double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values.empty() ? 0.0 : values[values.size() / 2];
}

// Synthetic GPU-bound model (no jitter): each batch runs for `batch(source)` and starts when its
// source has arrived and the previous batch has ended. A source's release is its token's depth
// (three batches back) or the cap, or the predicted release when the predictor has one. The
// predictor observes each arrival at once and each batch end two sources later.
struct Synthetic {
    GpuReleasePredictor predictor;
    std::vector<Clock::time_point> arrivals;
    std::vector<Clock::time_point> ends;
    std::vector<Duration> periods; // the period estimate after each source
};

Synthetic runSynthetic(const std::function<Duration(uint64_t)>& batch, uint64_t sources,
        Duration initialPeriod, Duration frame) {
    Synthetic run{GpuReleasePredictor(initialPeriod), {}, {}, {}};
    const Duration cpu = 6ms;
    const Clock::time_point epoch{};
    Clock::time_point deadline = epoch;
    for (uint64_t source = 0; source < sources; ++source) {
        Clock::time_point released = deadline;
        if (source >= 3) released = std::max(released, run.ends[source - 3]);
        if (const auto predicted = run.predictor.release(source)) released = std::max(released, *predicted);
        const auto arrived = released + cpu;
        const auto previousEnd = source ? run.ends[source - 1] : epoch;
        run.ends.push_back(std::max(arrived, previousEnd) + batch(source));
        run.arrivals.push_back(arrived);
        run.predictor.observeArrival(source, released, arrived, frame);
        if (source >= 2) run.predictor.observeBatchEnd(source - 2, run.ends[source - 2], frame);
        run.periods.push_back(run.predictor.periodEstimate());
        deadline = std::max(deadline + frame, arrived);
    }
    return run;
}

// The predictor's period estimate follows batch ends that come one batch period apart, on saturated frames.
void predictorPeriodConvergesOnSaturatedFrames() {
    const Duration frame = msd(16.67);
    GpuReleasePredictor predictor(20ms);
    const Clock::time_point epoch{};
    Clock::time_point previousEnd = epoch;
    for (uint64_t source = 0; source < 200; ++source) {
        const auto end = epoch + static_cast<int64_t>(source + 1) * msd(13);
        // Saturated: the source arrives 1 ms before the batch before it ends.
        const auto arrived = source == 0 ? epoch : previousEnd - 1ms;
        predictor.observeArrival(source, arrived - 6ms, arrived, frame);
        predictor.observeBatchEnd(source, end, frame);
        previousEnd = end;
    }
    expect(std::abs(milliseconds(predictor.periodEstimate()) - 13.0) < 0.5,
        "the period did not converge to the batch period");
    expect(milliseconds(predictor.gpuDeviationEstimate()) < 0.5, "exact batch ends left a GPU deviation");
    expect(std::abs(milliseconds(predictor.cpuEstimate()) - 6.0) < 0.5, "the CPU estimate did not learn the release to arrival time");
}

// Unsaturated frames (the batch ended before the frame arrived) do not measure the period.
void unsaturatedFramesDoNotMoveThePeriod() {
    const Duration frame = msd(16.67);
    GpuReleasePredictor predictor(20ms);
    const Clock::time_point epoch{};
    for (uint64_t source = 0; source < 50; ++source) {
        const auto end = epoch + static_cast<int64_t>(source + 1) * msd(13);
        // The source arrives 3 ms after the previous batch ended: the GPU was not the limit.
        const auto arrived = epoch + static_cast<int64_t>(source) * msd(13) + 3ms;
        predictor.observeArrival(source, arrived - 6ms, arrived, frame);
        predictor.observeBatchEnd(source, end, frame);
    }
    expect(predictor.periodEstimate() == 20ms, "unsaturated frames moved the period");
}

// The release lands each arrival on the predicted free time of the GPU: with no jitter the arrival
// is within a millisecond of the previous batch's end, which the predictor predicted.
void releaseLandsArrivalOnThePredictedFreeTime() {
    const auto run = runSynthetic([](uint64_t) { return msd(18); }, 300, msd(16.67), msd(16.67));
    double worst = 0;
    for (size_t source = 150; source < run.arrivals.size(); ++source) {
        const auto previousEnd = run.ends[source - 1];
        worst = std::max(worst, std::abs(milliseconds(run.arrivals[source] - previousEnd)));
    }
    expect(worst <= 1.0, "a predicted release did not land the arrival on the GPU's free time");
}

// A load step (18 ms to 22 ms batches) re-converges: the period estimate is within 1 ms of 22 ms
// within 40 sources of the step.
void loadStepResettlesWithin40Sources() {
    const auto run = runSynthetic([](uint64_t source) { return source < 300 ? msd(18) : msd(22); },
        400, msd(16.67), msd(16.67));
    size_t settled = run.periods.size();
    for (size_t source = 300; source < run.periods.size(); ++source) {
        if (std::abs(milliseconds(run.periods[source]) - 22.0) <= 1.0) {
            settled = source - 300;
            break;
        }
    }
    expect(settled <= 40, "the period did not resettle within 40 sources after the load step");
}

// A predictor with no batch end predicts nothing, one whose newest end is more than four sources old
// predicts nothing, and reset returns it to that state.
void predictionStopsWhenTheNewestEndIsStale() {
    GpuReleasePredictor predictor(16ms);
    const Clock::time_point epoch{};
    expect(!predictor.release(0), "a release was predicted before any batch end");
    predictor.observeBatchEnd(0, epoch + 10ms, 16ms);
    expect(predictor.release(1).has_value(), "no release after the first batch end");
    expect(predictor.release(4).has_value(), "a release within the lag bound was dropped");
    expect(!predictor.release(5).has_value(), "a stale batch end still predicted");
    predictor.reset(16ms);
    expect(!predictor.release(1).has_value(), "reset kept the batch end");
}

// Batch ends that are not newer than the last one are ignored.
void olderBatchEndsAreIgnored() {
    GpuReleasePredictor predictor(16ms);
    const Clock::time_point epoch{};
    predictor.observeBatchEnd(5, epoch + 50ms, 16ms);
    const auto before = predictor.release(6);
    predictor.observeBatchEnd(3, epoch + 90ms, 16ms);
    predictor.observeBatchEnd(5, epoch + 95ms, 16ms);
    expect(predictor.release(6) == before, "an older or repeated batch end moved the prediction");
}

// The fallback without split timestamps (L5, rule 5): a busy run doubles its step, an idle present
// takes a fixed amount off, and the hold stays within [0, 3 cap intervals].
void steppingHoldGrowsWhileBusyAndShrinksWhenIdle() {
    GpuSteppingHold hold;
    hold.observe(true, 16ms);
    expect(hold.hold() == 100us, "the first busy observation did not step 100 us");
    hold.observe(true, 16ms);
    expect(hold.hold() == 300us, "a second busy observation did not double the step");
    hold.observe(true, 16ms);
    expect(hold.hold() == 700us, "a third busy observation did not double the step again");
    hold.observe(false, 16ms);
    expect(hold.hold() == 200us, "an idle observation did not take 500 us off");
    hold.observe(false, 16ms);
    expect(hold.hold() == Duration::zero(), "an idle observation took the hold below zero");
    hold.observe(true, 16ms);
    expect(hold.hold() == 100us, "a busy run after idle did not restart at the first step");
}

void steppingHoldStepIsCappedAndHoldIsBounded() {
    GpuSteppingHold busy;
    for (int i = 0; i < 5; ++i) busy.observe(true, 16ms);
    // 100, 200, 400, 800, 1600 us: the sixth step is capped at an eighth of 16 ms.
    expect(busy.hold() == 3100us, "the busy run did not double its step");
    busy.observe(true, 16ms);
    expect(busy.hold() == 5100us, "the step was not capped at an eighth of a cap interval");
    for (int i = 0; i < 40; ++i) busy.observe(true, 16ms);
    expect(busy.hold() == 3 * 16ms, "the hold exceeded three cap intervals");
    GpuSteppingHold uncapped;
    uncapped.observe(true, 16ms);
    uncapped.observe(true, Duration::zero());
    expect(uncapped.hold() == Duration::zero(), "a missing cap interval kept the hold");
}

// Closed-loop model of the game, its GPU and the release (the port of the reference sim l5tgt.py):
// GPU work of the game per frame, CPU time from release to present, a batch of the game's frame plus
// FG work, a three-frame queue (VKD3D's default), and batch ends observed two sources late.
struct LoopModel {
    std::function<double(size_t)> gpuMs;
    double gpuJitterMs{1.0};
    double cpuJitterMs{0.5};
    bool predicted{false};
    size_t frames{4000};
    uint64_t seed{1};
    double initialPeriodMs{16.67};
};

struct LoopRun {
    std::vector<double> arrivalToDone; // game GPU done minus present arrival, ms
    std::vector<double> lead;          // previous batch end minus arrival, ms
    std::vector<double> arrival;       // arrival times, ms
    double period{};                   // predictor period at the end, ms
};

// xorshift64* with Box-Muller: deterministic on every standard library.
class Gauss {
public:
    explicit Gauss(uint64_t seed) : state(seed ? seed : 1) {}
    double uniform() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        const uint64_t bits = (state * 0x2545F4914F6CDD1DULL) >> 11;
        return (static_cast<double>(bits) + 0.5) / 9007199254740992.0;
    }
    double normal(double sigma) {
        const double radius = std::sqrt(-2.0 * std::log(uniform()));
        return sigma * radius * std::cos(6.283185307179586 * uniform());
    }
private:
    uint64_t state;
};

LoopRun runLoop(const LoopModel& model) {
    const Duration cap = msd(16.67);
    const Duration pre = 3ms;
    const Duration fg = msd(4.7);
    const Clock::time_point epoch{};
    Gauss random(model.seed);
    GpuReleasePredictor predictor(msd(model.initialPeriodMs));
    std::vector<Clock::time_point> ends;
    ends.reserve(model.frames);
    LoopRun run;
    Clock::time_point gpuFree = epoch;
    Clock::time_point deadline = epoch;
    for (size_t k = 0; k < model.frames; ++k) {
        const Duration game = msd(std::max(1.0, model.gpuMs(k) + random.normal(model.gpuJitterMs)));
        const Duration cpuTime = msd(6.0 + std::fabs(random.normal(model.cpuJitterMs)));
        const auto token = k >= 3 ? ends[k - 3] : epoch;
        Clock::time_point nextDeadline = deadline + cap;
        Clock::time_point released = std::max(token, deadline);
        if (model.predicted) {
            if (const auto predicted = predictor.release(k)) released = std::max(released, *predicted);
        }
        const auto arrival = released + cpuTime;
        if (arrival > nextDeadline) nextDeadline = arrival;
        const auto done = std::max(arrival - pre, gpuFree) + game;
        const auto end = done + fg;
        gpuFree = end;
        ends.push_back(end);
        if (model.predicted) {
            predictor.observeArrival(k, released, arrival, cap);
            if (k >= 2) predictor.observeBatchEnd(k - 2, ends[k - 2], cap);
        }
        run.arrivalToDone.push_back(milliseconds(done - arrival));
        run.lead.push_back(k ? milliseconds(ends[k - 1] - arrival) : 0.0);
        run.arrival.push_back(milliseconds(arrival - epoch));
        deadline = nextDeadline;
    }
    run.period = milliseconds(predictor.periodEstimate());
    return run;
}

struct LoopSummary {
    double medianArrivalToDone{};
    double medianLead{};
    double fps{};
};

// The last three quarters of the run, as the reference sim measures it.
LoopSummary summarize(const LoopRun& run) {
    const size_t first = run.arrival.size() / 4;
    std::vector<double> done(run.arrivalToDone.begin() + static_cast<std::ptrdiff_t>(first), run.arrivalToDone.end());
    std::vector<double> lead(run.lead.begin() + static_cast<std::ptrdiff_t>(first), run.lead.end());
    const double span = (run.arrival.back() - run.arrival[first]) / 1000.0;
    return {median(done), median(lead), static_cast<double>(run.arrival.size() - first - 1) / span};
}

// Frames after `start` until the median of twenty consecutive frames is within 2 ms of `steady` (the
// reference sim's settle measure). The twenty-frame median averages over the ripple of the transient.
std::optional<size_t> settleFrames(const std::vector<double>& series, size_t start, double steady) {
    for (size_t n = 0; start + n + 20 <= series.size(); ++n) {
        const std::vector<double> window(series.begin() + static_cast<std::ptrdiff_t>(start + n),
            series.begin() + static_cast<std::ptrdiff_t>(start + n + 20));
        if (std::abs(median(window) - steady) <= 2.0) return n;
    }
    return std::nullopt;
}

LoopRun runLoopWith(const std::function<double(size_t)>& gpu, double gpuJitter, double cpuJitter,
        bool predicted, double initialPeriodMs = 16.67) {
    LoopModel model;
    model.gpuMs = gpu;
    model.gpuJitterMs = gpuJitter;
    model.cpuJitterMs = cpuJitter;
    model.predicted = predicted;
    model.initialPeriodMs = initialPeriodMs;
    return runLoop(model);
}

// The GPU is the limit (13 ms game work plus 4.7 ms FG, above the 16.67 ms cap): the release lands
// the game just in time, its frames arrive about one GPU deviation before the batch before them
// ends, and throughput does not change.
void closedLoopHeavyGpuStaysJustInTime() {
    const auto heavy = [](size_t) { return 13.0; };
    const auto none = summarize(runLoopWith(heavy, 1.0, 0.5, false));
    const auto pred = runLoopWith(heavy, 1.0, 0.5, true);
    const auto predicted = summarize(pred);
    std::cout << "framegen-vulkan: gpu release sim (13 +- 1 ms, cpu 6 + |N(0,0.5)|): arrival to done median "
              << predicted.medianArrivalToDone << " ms (no hold " << none.medianArrivalToDone
              << " ms), prev end minus arrival " << predicted.medianLead << " ms, fps " << predicted.fps
              << " vs " << none.fps << " (" << predicted.fps / none.fps << "), period " << pred.period << " ms\n";
    expect(predicted.medianArrivalToDone <= 16.0, "the heavy GPU sim arrived too early for the queue");
    expect(predicted.fps >= 0.98 * none.fps, "the heavy GPU sim lost more than 2% of its frame rate");
}

void closedLoopNoisyGpuStaysJustInTime() {
    const auto heavy = [](size_t) { return 13.0; };
    const auto none = summarize(runLoopWith(heavy, 3.0, 2.0, false));
    const auto predicted = summarize(runLoopWith(heavy, 3.0, 2.0, true));
    std::cout << "framegen-vulkan: gpu release sim (13 +- 3 ms, cpu 6 + |N(0,2)|): arrival to done median "
              << predicted.medianArrivalToDone << " ms, prev end minus arrival " << predicted.medianLead
              << " ms, fps " << predicted.fps << " vs " << none.fps << " (" << predicted.fps / none.fps << ")\n";
    expect(predicted.medianArrivalToDone <= 18.0, "the noisy GPU sim arrived too early for the queue");
    expect(predicted.fps >= 0.975 * none.fps, "the noisy GPU sim lost more than 2.5% of its frame rate");
}

// A period initialised at 40 ms (outside the cap's frame) still converges on saturated frames.
void closedLoopPeriodInitialisedAt40Converges() {
    const auto heavy = [](size_t) { return 13.0; };
    const auto normal = runLoopWith(heavy, 1.0, 0.5, true, 16.67);
    const auto late = runLoopWith(heavy, 1.0, 0.5, true, 40.0);
    std::cout << "framegen-vulkan: gpu release sim (period initialised at 40 ms): final period "
              << late.period << " ms (16.67 ms init " << normal.period << " ms), arrival to done median "
              << summarize(late).medianArrivalToDone << " ms\n";
    expect(std::abs(late.period - normal.period) < 1.0, "a 40 ms initial period did not converge");
    expect(summarize(late).medianArrivalToDone <= 16.0, "a 40 ms initial period left the queue deep");
}

// A load step from 8 to 14 ms GPU work resettles within 40 frames. The settle frame count is one
// draw of the jitter, so the test takes the median over nine seeds; every seed's count is printed.
// The transient overshoots (the queue builds for about 15 frames before it drains).
void closedLoopLoadStepSettlesWithin40Frames() {
    const auto step = [](size_t k) { return k < 2000 ? 8.0 : 14.0; };
    for (const auto jitter : {std::pair{1.0, 0.5}, std::pair{3.0, 2.0}}) {
        std::vector<double> settleCounts;
        double peakMax = 0.0;
        for (uint64_t seed = 1; seed <= 9; ++seed) {
            LoopModel model;
            model.gpuMs = step;
            model.gpuJitterMs = jitter.first;
            model.cpuJitterMs = jitter.second;
            model.predicted = true;
            model.seed = seed;
            const auto run = runLoop(model);
            const std::vector<double> tail(run.arrivalToDone.begin() + 3000, run.arrivalToDone.begin() + 4000);
            const auto settled = settleFrames(run.arrivalToDone, 2000, median(tail));
            peakMax = std::max(peakMax, *std::max_element(run.arrivalToDone.begin() + 2000,
                run.arrivalToDone.begin() + 2100));
            settleCounts.push_back(settled ? static_cast<double>(*settled) : 1e9);
        }
        const double settleMedian = median(settleCounts);
        std::cout << "framegen-vulkan: gpu release sim (8 to 14 ms step, jitter " << jitter.first
                  << " ms, seeds 1-9): settle frames median " << settleMedian << ", range "
                  << *std::min_element(settleCounts.begin(), settleCounts.end()) << " to "
                  << *std::max_element(settleCounts.begin(), settleCounts.end())
                  << ", peak arrival to done " << peakMax << " ms\n";
        expect(settleMedian <= 40.0, "a load step did not resettle within 40 frames");
    }
}

// Light scenes are unchanged: the predicted release does not move a game that the cap already paces.
void closedLoopLightSceneIsUnchanged() {
    const auto light = [](size_t) { return 8.0; };
    const auto none = summarize(runLoopWith(light, 1.0, 0.5, false));
    const auto predicted = summarize(runLoopWith(light, 1.0, 0.5, true));
    std::cout << "framegen-vulkan: gpu release sim (8 ms light scene): arrival to done median "
              << predicted.medianArrivalToDone << " ms vs " << none.medianArrivalToDone << " ms, fps "
              << predicted.fps << " vs " << none.fps << '\n';
    expect(std::abs(predicted.medianArrivalToDone - none.medianArrivalToDone) <= 0.5,
        "a light scene's arrival changed under the predicted release");
    expect(predicted.fps >= 0.995 * none.fps, "a light scene lost frame rate under the predicted release");
}
}

int main() {
    steadyLeadMovesDelayByAnEighthOfTheExcess();
    earlyThenLateMovesDelayBothWays();
    jitterWidensMargin();
    delayStaysWithinThreeCapIntervals();
    noCapResetsDelay();
    lowLatencyPlanFollowsSettingCapFrameGenerationAndReflex();
    reflexKeepsJustInTimeBatchesAndStepsReleaseDelayAside();
    reflexOnForcesZeroAndReflexOffResumesFromZero();
    nativeAcquireModelSettlesAtMargin();
    threeFrameQueueSettlesAtMargin();
    acquireHoldIsBoundedByTheTimeout();
    predictorPeriodConvergesOnSaturatedFrames();
    unsaturatedFramesDoNotMoveThePeriod();
    releaseLandsArrivalOnThePredictedFreeTime();
    loadStepResettlesWithin40Sources();
    predictionStopsWhenTheNewestEndIsStale();
    olderBatchEndsAreIgnored();
    steppingHoldGrowsWhileBusyAndShrinksWhenIdle();
    steppingHoldStepIsCappedAndHoldIsBounded();
    closedLoopHeavyGpuStaysJustInTime();
    closedLoopNoisyGpuStaysJustInTime();
    closedLoopPeriodInitialisedAt40Converges();
    closedLoopLoadStepSettlesWithin40Frames();
    closedLoopLightSceneIsUnchanged();
    std::cout << "low latency tests passed\n";
}

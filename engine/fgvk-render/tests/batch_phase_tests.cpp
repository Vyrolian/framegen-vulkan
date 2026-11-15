/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/batch_phase.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

using namespace fgvk::layer;
using namespace std::chrono_literals;

namespace {
void expect(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

using Lock = BatchPhaseLock;
using Duration = Lock::Duration;
using TimePoint = Lock::TimePoint;

constexpr TimePoint epoch{};
// 60 fps batches of one output each, as the period of a steady stream.
constexpr Duration cadence = std::chrono::microseconds{16667};

Duration absolute(const Duration value) { return value < Duration::zero() ? -value : value; }

// Readiness with ±1 ms of alternating jitter at a steady period: after 20 batches the starts stay one
// period apart within 0.6 ms, and each start comes 0 to 4 ms after its own readiness.
void alternatingJitterKeepsSpacingAndHold() {
    Lock lock;
    TimePoint previous{};
    for (int k = 0; k < 60; ++k) {
        const TimePoint ready = epoch + 100ms + k * cadence + (k % 2 ? 1ms : -1ms);
        const TimePoint start = lock.start(ready, cadence);
        if (k >= 20) {
            expect(absolute((start - previous) - cadence) <= 600us,
                "the phase lock kept no spacing under alternating readiness jitter");
            expect(start - ready >= Duration::zero() && start - ready <= 4ms,
                "the phase lock start was not 0 to 4 ms after readiness under jitter");
        }
        previous = start;
    }
}

// A constant cadence converges to each readiness plus the margin, exactly.
void steadyCadenceStartsMarginAfterReadiness() {
    Lock lock;
    for (int k = 0; k < 60; ++k) {
        const TimePoint ready = epoch + 100ms + k * cadence;
        const TimePoint start = lock.start(ready, cadence);
        if (k >= 20) expect(start == ready + 2ms, "a steady cadence did not converge to readiness plus 2 ms");
    }
}

// The cadence steps from 33.3 ms (30 fps) to 9.1 ms (110 fps). The lock relocks within three batches
// and its spacing settles within twenty.
void cadenceStepRelocksAndSettles() {
    const Duration slow = std::chrono::nanoseconds{33'333'333};
    const Duration fast = std::chrono::nanoseconds{9'100'000};
    Lock lock;
    TimePoint ready = epoch + 100ms;
    std::vector<TimePoint> starts;
    for (int k = 0; k < 60; ++k) {
        if (k > 0) ready += k < 20 ? slow : fast;
        const TimePoint start = lock.start(ready, k < 20 ? slow : fast);
        if (k >= 20 && k < 23)
            expect(absolute(start - (ready + 2ms)) <= 500us, "the phase lock did not relock within three batches");
        starts.push_back(start);
    }
    for (size_t k = 40; k < starts.size(); ++k)
        expect(absolute((starts[k] - starts[k - 1]) - fast) <= 600us,
            "the phase lock spacing did not settle after the cadence step");
}

// A readiness later than the lock expects, but within a period of it, starts at its readiness.
void lateReadyStartsAtReadiness() {
    Lock lock;
    expect(lock.start(epoch, 16ms) == epoch + 2ms, "the first batch did not start 2 ms after readiness");
    expect(lock.start(epoch + 20ms, 16ms) == epoch + 20ms, "a late batch did not start at its readiness");
}

// A zero or negative period has nothing to lock to: the batch starts at readiness, and the lock resets.
void zeroNominalStartsAtReadinessAndResets() {
    Lock lock;
    expect(lock.start(epoch + 5ms, Duration::zero()) == epoch + 5ms, "a zero period did not start at readiness");
    expect(lock.start(epoch + 6ms, -1ms) == epoch + 6ms, "a negative period did not start at readiness");
    // After the reset the next batch has no earlier phase: 20 + 2 ms. Without it the lock would start at 20 ms.
    expect(lock.start(epoch + 20ms, 16ms) == epoch + 22ms, "a zero period did not reset the lock");
}

void resetForgetsThePhase() {
    Lock lock;
    static_cast<void>(lock.start(epoch, 16ms));
    static_cast<void>(lock.start(epoch + 16ms, 16ms));
    lock.reset();
    expect(lock.start(epoch + 100ms, 16ms) == epoch + 102ms, "a reset lock kept the previous phase");
}

// A start the caller moved later becomes the expectation of the next start.
void forcedStartBecomesTheExpectation() {
    Lock lock;
    expect(lock.start(epoch, 16ms) == epoch + 2ms, "the first batch did not start 2 ms after readiness");
    lock.forceStart(epoch + 10ms);
    // Expected 10 + 16 = 26 ms; the target 18 ms is 8 ms earlier, so the start is a quarter of the way: 24 ms.
    expect(lock.start(epoch + 16ms, 16ms) == epoch + 24ms, "a forced start did not become the expectation");
}
}

int main() {
    alternatingJitterKeepsSpacingAndHold();
    steadyCadenceStartsMarginAfterReadiness();
    cadenceStepRelocksAndSettles();
    lateReadyStartsAtReadiness();
    zeroNominalStartsAtReadinessAndResets();
    resetForgetsThePhase();
    forcedStartBecomesTheExpectation();
    std::cout << "batch phase tests passed\n";
}

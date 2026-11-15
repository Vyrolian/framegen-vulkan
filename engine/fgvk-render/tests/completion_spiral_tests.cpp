/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "completion_spiral.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

using namespace fgvk::layer;

namespace {
using Detector = CompletionSpiralDetector;
using Event = Detector::Event;
using Clock = Detector::Clock;

void expect(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

bool near(double value, double expected) {
    return std::fabs(value - expected) < 1e-6;
}

Clock::duration toDuration(const double milliseconds) {
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(milliseconds));
}

// The rule's windows carry 100 sources, so a window's summed wait is 100 times its per-source wait.
constexpr size_t sourcesPerWindow = 100;

Detector::Report window(Detector& detector, const double periodMs, const double waitPerSourceMs) {
    return detector.observeWindow(periodMs, waitPerSourceMs * sourcesPerWindow, sourcesPerWindow);
}

// Three growing windows after a reference trip on the third one, and only once.
void growingPeriodWithWaitTripsOnThirdWindow() {
    Detector detector;
    expect(window(detector, 9.0, 0.5).event == Event::none, "the reference window reported an episode");
    // Each period grows by about 13% and the mean wait per source covers half of that growth.
    expect(window(detector, 10.2, 1.2).event == Event::none, "one growing window reported an episode");
    expect(window(detector, 11.5, 2.5).event == Event::none, "two growing windows reported before three");
    const auto suspected = window(detector, 12.9, 3.8);
    expect(suspected.event == Event::suspected, "three growing windows with matching waits did not trip");
    expect(suspected.windows == 3 && near(suspected.periodMs, 12.9) && near(suspected.meanCompletionWaitMs, 3.8),
        "the suspicion did not report its window's period, mean wait and streak");
    expect(detector.active(), "an episode was not active after suspicion");
    expect(window(detector, 14.5, 5.0).event == Event::none, "a continuing episode reported again");
    const auto cleared = window(detector, 14.5, 0.5);
    expect(cleared.event == Event::cleared && !detector.active(), "a flat window did not end the episode");
    expect(window(detector, 14.5, 0.5).event == Event::none, "a cleared episode reported again");
}

// The frame rate falls as the period grows, so the summed wait of a window can stay below the
// reference's while the mean wait per source keeps growing. The mean rule must still trip.
void growthWithFallingFrameRateTrips() {
    Detector detector;
    expect(detector.observeWindow(9.0, 0.5 * 100, 100).event == Event::none,
        "the reference window reported an episode");
    // Summed waits 45.5, 54 and 66 ms: the first is below the reference's 50 ms, yet its mean covers half the growth.
    expect(detector.observeWindow(10.2, 0.65 * 70, 70).event == Event::none, "a falling frame rate tripped at once");
    expect(detector.observeWindow(11.5, 0.9 * 60, 60).event == Event::none, "two windows reported before three");
    const auto suspected = detector.observeWindow(12.9, 1.2 * 55, 55);
    expect(suspected.event == Event::suspected && suspected.windows == 3,
        "a falling frame rate with a growing mean wait did not trip on the third window");
    expect(near(suspected.meanCompletionWaitMs, 1.2) && near(suspected.periodMs, 12.9),
        "the suspicion did not report the mean wait of its window");
}

// Two frames that wait 12 ms each, three times, each hitch separated by a normal window.
// The hitch raises the learned period in its own window only, so the streak never reaches three.
void twoFrameHitchesDoNotTrip() {
    Detector detector;
    expect(window(detector, 9.0, 0.5).event == Event::none, "the reference window reported an episode");
    for (unsigned hitch = 0; hitch < 3; ++hitch) {
        // Summed wait 50 ms + 2 x 12 ms, period up 13%.
        expect(window(detector, 10.2, 0.74).event == Event::none, "a two-frame hitch tripped the detector");
        expect(window(detector, 9.0, 0.5).event == Event::none, "the window after a hitch reported an episode");
    }
    expect(!detector.active(), "hitches left an episode active");
}

// Growth needs a wait that grows and covers half of the period growth. Neither alone trips.
void growthWithoutMatchingWaitDoesNotTrip() {
    // The mean wait stays flat, although it covers half of each growth step.
    Detector flat;
    expect(window(flat, 9.0, 0.7).event == Event::none, "the reference window reported an episode");
    for (const double period : {10.0, 11.1, 12.3, 13.6})
        expect(window(flat, period, 0.7).event == Event::none, "a growing period with a flat wait tripped");
    // The mean wait grows, but covers less than half of each growth step.
    Detector shortWait;
    expect(window(shortWait, 9.0, 0.4).event == Event::none, "the reference window reported an episode");
    for (const auto [period, wait] : {std::pair{10.2, 0.5}, {11.5, 0.6}, {12.9, 0.65}, {14.5, 0.75}})
        expect(window(shortWait, period, wait).event == Event::none,
            "a wait covering less than half of the growth tripped");
    expect(!flat.active() && !shortWait.active(), "growth without a covering wait left an episode");
}

// A window without sources is no evidence and does not reset a growing streak.
void emptyWindowKeepsStreak() {
    Detector detector;
    expect(window(detector, 9.0, 0.5).event == Event::none, "the reference window reported an episode");
    expect(window(detector, 10.2, 1.2).event == Event::none, "one growing window reported an episode");
    expect(window(detector, 11.5, 2.5).event == Event::none, "two growing windows reported before three");
    expect(detector.observeWindow(0.0, 0.0, 0).event == Event::none, "an empty window reported an episode");
    expect(window(detector, 12.9, 3.8).event == Event::suspected, "an empty window reset the growing streak");
}

// One second of sources at 10 ms spacing, each carrying the same learned period and wait.
void feedSecond(Detector& detector, const double startSeconds, const double periodMs, const double waitMs,
        std::vector<std::pair<double, Event>>& events) {
    const auto start = Clock::time_point{std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(startSeconds))};
    for (size_t index = 0; index < sourcesPerWindow; ++index) {
        const auto now = start + std::chrono::milliseconds{10 * static_cast<int64_t>(index)};
        const auto report = detector.observeSource(now, toDuration(waitMs), toDuration(periodMs));
        if (report.event != Event::none)
            events.emplace_back(startSeconds + 0.01 * static_cast<double>(index), report.event);
    }
}

// Sources are grouped into one-second windows. The growing windows of seconds 1-3 trip on
// the first source of second 4, and the flat second 5 clears the episode on the first source of second 6.
void sourceWindowsTripAndClear() {
    Detector detector;
    std::vector<std::pair<double, Event>> events;
    feedSecond(detector, 0, 9.0, 0.5, events);
    feedSecond(detector, 1, 10.2, 1.2, events);
    feedSecond(detector, 2, 11.5, 2.5, events);
    feedSecond(detector, 3, 12.9, 3.8, events);
    feedSecond(detector, 4, 14.5, 5.0, events);
    feedSecond(detector, 5, 14.5, 0.5, events);
    feedSecond(detector, 6, 14.5, 0.5, events);
    expect(events.size() == 2, "source windows did not report exactly one episode");
    expect(events[0].second == Event::suspected && near(events[0].first, 4.0),
        "the suspicion did not land on the fourth window boundary");
    expect(events[1].second == Event::cleared && near(events[1].first, 6.0),
        "the flat window did not clear the episode at its boundary");
}

// A pause longer than the source gap clears an active episode and restarts the windows: three
// growing windows after the pause are needed again, and the window that closes at the pause is dropped.
void pauseEndsEpisodeAndRestartsWindows() {
    Detector detector;
    std::vector<std::pair<double, Event>> events;
    feedSecond(detector, 0, 9.0, 0.5, events);
    feedSecond(detector, 1, 10.2, 1.2, events);
    feedSecond(detector, 2, 11.5, 2.5, events);
    feedSecond(detector, 3, 12.9, 3.8, events);
    feedSecond(detector, 4, 14.5, 5.0, events);
    // The first source after the pause, at second 10, clears the episode without closing a window.
    feedSecond(detector, 10, 14.5, 5.0, events);
    feedSecond(detector, 11, 16.0, 7.0, events);
    feedSecond(detector, 12, 17.8, 9.0, events);
    feedSecond(detector, 13, 19.6, 11.0, events);
    feedSecond(detector, 14, 19.6, 11.0, events);
    expect(events.size() == 3, "a pause did not produce exactly suspicion, clear and a new suspicion");
    expect(events[0].second == Event::suspected && near(events[0].first, 4.0),
        "the first suspicion did not land on the fourth window boundary");
    expect(events[1].second == Event::cleared && near(events[1].first, 10.0),
        "the pause did not clear the episode at its first source");
    expect(events[2].second == Event::suspected && near(events[2].first, 14.0),
        "the windows after the pause did not need three fresh growing windows");
}
} // namespace

int main() {
    growingPeriodWithWaitTripsOnThirdWindow();
    growthWithFallingFrameRateTrips();
    twoFrameHitchesDoNotTrip();
    growthWithoutMatchingWaitDoesNotTrip();
    emptyWindowKeepsStreak();
    sourceWindowsTripAndClear();
    pauseEndsEpisodeAndRestartsWindows();
    std::cout << "completion spiral tests passed\n";
    return 0;
}

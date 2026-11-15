/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/uncapped_auto_cap.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace fgvk::layer;

namespace {
void expect(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

bool near(double value, double expected, double tolerance) {
    return std::fabs(value - expected) <= tolerance;
}

void refreshFromTiming() {
    expect(near(UncappedAutoCap::refreshHzFromTiming(4167404, 0), 240.0, 0.1),
        "a 4 167 404 ns refresh duration is not about 240 Hz");
    expect(near(UncappedAutoCap::refreshHzFromTiming(std::numeric_limits<uint64_t>::max(), 4167404), 240.0, 0.1),
        "an unknown refresh duration did not fall back to the interval");
    expect(UncappedAutoCap::refreshHzFromTiming(0, 0) == 0.0,
        "zero refresh timing was not reported as unknown");
    expect(UncappedAutoCap::refreshHzFromTiming(2'000'000'000, 0) == 0.0,
        "a 0.5 Hz refresh was accepted");
}

void refreshRangeCheck() {
    // The range check every refresh source passes (timing, X11 RandR, Wayland output).
    expect(UncappedAutoCap::refreshHzInRange(59.94) == 59.94, "an in-range 59.94 Hz rate was changed");
    expect(UncappedAutoCap::refreshHzInRange(20.0) == 20.0, "20 Hz was rejected");
    expect(UncappedAutoCap::refreshHzInRange(1000.0) == 1000.0, "1000 Hz was rejected");
    expect(UncappedAutoCap::refreshHzInRange(19.99) == 0.0, "19.99 Hz was accepted");
    expect(UncappedAutoCap::refreshHzInRange(1000.01) == 0.0, "1000.01 Hz was accepted");
    expect(UncappedAutoCap::refreshHzInRange(std::numeric_limits<double>::quiet_NaN()) == 0.0,
        "a NaN refresh rate was accepted");
}

void refreshQueryCadence() {
    // Rate unknown: the first 64 presents query, then every 8th present does.
    expect(UncappedAutoCap::refreshQueryDue(0, false), "an unknown rate was not queried at present 0");
    expect(UncappedAutoCap::refreshQueryDue(63, false), "an unknown rate was not queried at present 63");
    expect(UncappedAutoCap::refreshQueryDue(64, false), "an unknown rate was not queried at present 64");
    expect(UncappedAutoCap::refreshQueryDue(72, false), "an unknown rate was not queried at present 72");
    expect(!UncappedAutoCap::refreshQueryDue(239, false), "an unknown rate was queried at present 239");
    expect(UncappedAutoCap::refreshQueryDue(240, false), "an unknown rate was not queried at present 240");
    expect(UncappedAutoCap::refreshQueryDue(480, false), "an unknown rate was not queried at present 480");
    // Rate known: today's cadence, once per 240 presents, none before present 64.
    expect(!UncappedAutoCap::refreshQueryDue(0, true), "a known rate was queried at present 0");
    expect(!UncappedAutoCap::refreshQueryDue(63, true), "a known rate was queried at present 63");
    expect(!UncappedAutoCap::refreshQueryDue(64, true), "a known rate was queried at present 64");
    expect(!UncappedAutoCap::refreshQueryDue(72, true), "a known rate was queried at present 72");
    expect(!UncappedAutoCap::refreshQueryDue(239, true), "a known rate was queried at present 239");
    expect(UncappedAutoCap::refreshQueryDue(240, true), "a known rate was not queried at present 240");
    expect(UncappedAutoCap::refreshQueryDue(480, true), "a known rate was not queried at present 480");
}
}

int main() {
    refreshFromTiming();
    refreshRangeCheck();
    refreshQueryCadence();
    std::cout << "uncapped auto cap tests passed\n";
    return 0;
}

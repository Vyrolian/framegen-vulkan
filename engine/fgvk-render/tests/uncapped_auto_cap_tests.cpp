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

void refreshFromWaylandNs() {
    expect(UncappedAutoCap::refreshHzFromWaylandNs(0) == 0.0,
        "an unknown wp_presentation refresh was not reported as unknown");
    expect(UncappedAutoCap::refreshHzFromWaylandNs(8'000'000) == 125.0,
        "an 8 000 000 ns wp_presentation refresh is not 125 Hz");
    expect(near(UncappedAutoCap::refreshHzFromWaylandNs(4'167'500), 239.95, 0.01),
        "a 4 167 500 ns wp_presentation refresh is not about 239.95 Hz");
    expect(UncappedAutoCap::refreshHzFromWaylandNs(1'000'000) == 1000.0,
        "a 1000 Hz wp_presentation refresh was rejected");
    expect(UncappedAutoCap::refreshHzFromWaylandNs(60'000'000) == 0.0,
        "a 16.7 Hz wp_presentation refresh was accepted");
}

void waylandOutputRate() {
    // A non-zero presented refresh is the rate, so the output's mode is not used.
    expect(UncappedAutoCap::waylandOutputMilliHz(8'000'000, 165'000, 165'000) == 0,
        "a fixed presented refresh still used the output mode rate");
    // Under VRR the output the feedback named wins over the only bound output.
    expect(UncappedAutoCap::waylandOutputMilliHz(0, 60'000, 165'000) == 60'000,
        "the output named by sync_output did not win over the only output");
    // Without a named output, the only bound output's mode rate is used.
    expect(UncappedAutoCap::waylandOutputMilliHz(0, 0, 165'000) == 165'000,
        "the only bound output's rate was not used without sync_output");
    expect(UncappedAutoCap::waylandOutputMilliHz(0, 0, 0) == 0,
        "no output rate was not reported as unknown");
    expect(near(UncappedAutoCap::refreshHzFromWaylandMilliHz(239'958), 239.958, 1e-9),
        "a 239 958 mHz output mode is not 239.958 Hz");
    expect(UncappedAutoCap::refreshHzFromWaylandMilliHz(165'000) == 165.0,
        "a 165 000 mHz output mode is not 165 Hz");
    expect(UncappedAutoCap::refreshHzFromWaylandMilliHz(0) == 0.0,
        "an unknown output mode was not reported as unknown");
    expect(UncappedAutoCap::refreshHzFromWaylandMilliHz(16'000) == 0.0,
        "a 16 Hz output mode was accepted");
    expect(UncappedAutoCap::refreshHzFromWaylandMilliHz(1'000'001) == 0.0,
        "a 1000.001 Hz output mode was accepted");
}

void waylandFeedbackCadence() {
    // Rate unknown: every 8th present asks for feedback.
    expect(UncappedAutoCap::waylandFeedbackDue(0, false), "an unknown rate did not ask at present 0");
    expect(!UncappedAutoCap::waylandFeedbackDue(7, false), "an unknown rate asked at present 7");
    expect(UncappedAutoCap::waylandFeedbackDue(8, false), "an unknown rate did not ask at present 8");
    expect(UncappedAutoCap::waylandFeedbackDue(16, false), "an unknown rate did not ask at present 16");
    // Rate known: every 240th present asks.
    expect(UncappedAutoCap::waylandFeedbackDue(0, true), "a known rate did not ask at present 0");
    expect(!UncappedAutoCap::waylandFeedbackDue(239, true), "a known rate asked at present 239");
    expect(UncappedAutoCap::waylandFeedbackDue(240, true), "a known rate did not ask at present 240");
    expect(UncappedAutoCap::waylandFeedbackDue(480, true), "a known rate did not ask at present 480");
}
}

int main() {
    refreshFromTiming();
    refreshRangeCheck();
    refreshQueryCadence();
    refreshFromWaylandNs();
    waylandOutputRate();
    waylandFeedbackCadence();
    std::cout << "uncapped auto cap tests passed\n";
    return 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "x11_refresh_policy.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>

using namespace fgvk::layer;

namespace {
void expect(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

bool near(double value, double expected, double tolerance) {
    return std::fabs(value - expected) <= tolerance;
}

bool chose(std::optional<size_t> index, size_t expected) {
    return index.has_value() && *index == expected;
}

void modeRefresh() {
    // 1920x1080 at 60 Hz: 148.5 MHz over 2200 x 1125 total pixels per frame.
    expect(near(randrModeRefreshHz(148500000, 2200, 1125, 0), 60.0, 1e-9),
        "a 1080p60 mode did not read 60 Hz");
    // Unrelated sync-polarity flags leave the rate alone.
    expect(near(randrModeRefreshHz(148500000, 2200, 1125, 0x1 | 0x4), 60.0, 1e-9),
        "sync polarity flags changed the mode rate");
    // An interlaced mode scans two fields per frame: 30 frames of 2200 x 1125 at 74.25 MHz is 60 fields.
    expect(near(randrModeRefreshHz(74250000, 2200, 1125, randrModeInterlace), 60.0, 1e-9),
        "an interlaced 30-frame mode did not read 60 Hz");
    // A double-scanned mode repeats each line, so its frame rate is half the dot-clock rate.
    expect(near(randrModeRefreshHz(25175000, 800, 525, randrModeDoubleScan), 25175000.0 / 420000.0 / 2.0, 1e-9),
        "a double-scanned mode was not halved");
    expect(randrModeRefreshHz(0, 2200, 1125, 0) == 0.0, "a zero dot clock was not unknown");
    expect(randrModeRefreshHz(148500000, 0, 1125, 0) == 0.0, "a zero htotal was not unknown");
    expect(randrModeRefreshHz(148500000, 2200, 0, 0) == 0.0, "a zero vtotal was not unknown");
}

void overlapRule() {
    const X11Rect leftOutput{0, 0, 1920, 1080};
    expect(overlapArea(X11Rect{100, 100, 200, 200}, leftOutput) == 200u * 200u,
        "a window inside an output did not overlap its full area");
    expect(overlapArea(X11Rect{-100, -100, 200, 200}, leftOutput) == 100u * 100u,
        "a window partly above and left of the screen did not overlap its visible corner");
    expect(overlapArea(X11Rect{1920, 0, 100, 100}, leftOutput) == 0,
        "a window touching the output edge overlapped it");
    expect(overlapArea(X11Rect{0, 0, 0, 100}, leftOutput) == 0,
        "a zero-width window overlapped an output");
}

void crtcChoice() {
    const X11Rect left{0, 0, 1920, 1080};
    const X11Rect right{1920, 0, 2560, 1440};
    const std::array<RandrCrtc, 2> crtcs{RandrCrtc{10, left, true}, RandrCrtc{11, right, true}};
    const auto noPrimary = []() -> uint32_t { return 0; };
    const auto primaryLeft = []() -> uint32_t { return 10; };
    const auto primaryRight = []() -> uint32_t { return 11; };

    expect(chose(chooseCrtc(crtcs, X11Rect{2100, 100, 800, 600}, noPrimary), 1),
        "a window on the right output did not choose the right CRTC");
    expect(chose(chooseCrtc(crtcs, X11Rect{1500, 100, 1000, 600}, noPrimary), 1),
        "a window with most of its area on the right did not choose the right CRTC");
    // Equal overlap on both outputs: the first CRTC in RandR order wins the tie.
    expect(chose(chooseCrtc(crtcs, X11Rect{920, 100, 2000, 600}, noPrimary), 0),
        "an equal overlap did not keep the first CRTC");

    // An inactive CRTC never wins, even with the largest overlap.
    const std::array<RandrCrtc, 2> inactiveLeft{RandrCrtc{10, left, false}, RandrCrtc{11, right, true}};
    expect(!largestOverlapCrtc(inactiveLeft, X11Rect{100, 100, 400, 400}).has_value(),
        "an inactive CRTC was chosen by overlap");

    // No overlapping CRTC: the primary output's CRTC decides, if it is active.
    const X11Rect offScreen{5000, 5000, 400, 400};
    expect(!largestOverlapCrtc(crtcs, offScreen).has_value(),
        "an off-screen window overlapped an output");
    expect(chose(chooseCrtc(crtcs, offScreen, primaryRight), 1),
        "an off-screen window did not fall back to the primary CRTC");
    expect(chose(chooseCrtc(crtcs, offScreen, primaryLeft), 0),
        "an off-screen window did not fall back to the primary CRTC on the left");
    expect(!chooseCrtc(crtcs, offScreen, noPrimary).has_value(),
        "a window without a primary CRTC was given one");
    const std::array<RandrCrtc, 2> inactivePrimary{RandrCrtc{10, left, true}, RandrCrtc{11, right, false}};
    expect(!chooseCrtc(inactivePrimary, offScreen, primaryRight).has_value(),
        "an inactive primary CRTC was chosen");

    // The primary lookup is paid for only when no active CRTC overlaps the window.
    int primaryLookups = 0;
    const auto countingPrimary = [&primaryLookups]() -> uint32_t {
        ++primaryLookups;
        return 11;
    };
    static_cast<void>(chooseCrtc(crtcs, X11Rect{2100, 100, 800, 600}, countingPrimary));
    expect(primaryLookups == 0, "the primary CRTC was looked up while a CRTC overlapped the window");
    static_cast<void>(chooseCrtc(crtcs, offScreen, countingPrimary));
    expect(primaryLookups == 1, "the primary CRTC was not looked up without an overlapping CRTC");

    // A zero-size window overlaps nothing.
    expect(!largestOverlapCrtc(crtcs, X11Rect{100, 100, 0, 0}).has_value(),
        "a zero-size window overlapped a CRTC");
}

void activeCrtcLookup() {
    const X11Rect rect{0, 0, 1920, 1080};
    const std::array<RandrCrtc, 2> crtcs{RandrCrtc{10, rect, true}, RandrCrtc{11, rect, false}};
    expect(chose(activeCrtcById(crtcs, 10), 0), "an active CRTC was not found by id");
    expect(!activeCrtcById(crtcs, 11).has_value(), "an inactive CRTC was found by id");
    expect(!activeCrtcById(crtcs, 12).has_value(), "an unknown CRTC id was found");
    expect(!activeCrtcById(crtcs, 0).has_value(), "CRTC id 0 was treated as a CRTC");
}

void outputNameMatch() {
    const std::array<NamedRefresh, 3> outputs{
        NamedRefresh{"HDMI-A-1", 60000},
        NamedRefresh{"DP-1", 59940},
        NamedRefresh{"DP-2", 0},
    };
    expect(outputRefreshMilliHz("DP-1", outputs) == 59940u,
        "DP-1 did not match its exact 59.94 Hz rate");
    expect(outputRefreshMilliHz("HDMI-A-1", outputs) == 60000u,
        "HDMI-A-1 did not match its rate");
    expect(!outputRefreshMilliHz("dp-1", outputs).has_value(),
        "output names matched case-insensitively");
    expect(!outputRefreshMilliHz("DP-2", outputs).has_value(),
        "an output without a current-mode rate matched");
    expect(!outputRefreshMilliHz("DP-3", outputs).has_value(),
        "an unknown output name matched");
    expect(!outputRefreshMilliHz("", outputs).has_value(),
        "an empty output name matched");
    expect(!outputRefreshMilliHz("DP-1", std::span<const NamedRefresh>{}).has_value(),
        "an empty output list matched");
    expect(near(refreshHzFromMilliHz(59940), 59.94, 1e-9),
        "59940 mHz did not convert to 59.94 Hz");
}
} // namespace

int main() {
    modeRefresh();
    overlapRule();
    crtcChoice();
    activeCrtcLookup();
    outputNameMatch();
    std::cout << "x11 refresh policy tests passed\n";
    return 0;
}

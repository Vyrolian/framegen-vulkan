/* SPDX-License-Identifier: GPL-3.0-or-later */
// RP1: the real present lead's marker pairing, error rule and limits. Pure; no Vulkan device.
#include "swapchain/real_present_lead.hpp"

#include <cstdlib>
#include <iostream>

using namespace fgvk::layer;
using namespace std::chrono_literals;

namespace {
using Duration = RealPresentLead::Duration;

void expect(const bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

constexpr uint64_t context = 5;
// The GPU clock's offset from the steady clock. Only differences of GPU markers are used, so it cancels.
constexpr Duration gpuOffset = 1000us;

// One period of the fixture. The generated output starts at `start` and its real is planned `spacing` later. The real is
// presented `lead.lead` early, and its GPU marker sits `extraWait` beyond its call. Both markers are observed in order.
// Returns whether the pair gave a sample.
bool period(RealPresentLead& lead, const uint64_t source, const Duration start, const Duration spacing,
        const Duration extraWait) {
    const Duration planned = start + spacing;
    lead.call(context, source, true, start, start);
    lead.call(context, source, false, planned - lead.lead, planned);
    const PresentMarkerSample generated{.context = context, .source = source,
        .gpuNs = (start + gpuOffset).count(), .generated = true};
    const PresentMarkerSample real{.context = context, .source = source,
        .gpuNs = (planned - lead.lead + extraWait + gpuOffset).count(), .generated = false};
    static_cast<void>(lead.observe(generated));
    return lead.observe(real);
}

// A steady extra GPU wait of `extraWait` converges to that wait, within the limit, over the fixture loop.
void convergesToTheExtraWait() {
    RealPresentLead lead;
    const Duration extraWait = 1700us;
    for (uint64_t source = 1; source <= 200; ++source) {
        const auto start = std::chrono::milliseconds{static_cast<int64_t>(source) * 20};
        expect(period(lead, source, start, 4ms, extraWait), "a matched period gave no sample");
        expect(lead.lead >= Duration{} && lead.lead <= RealPresentLead::limit,
            "the lead left its range while converging");
    }
    const Duration difference = lead.lead > extraWait ? lead.lead - extraWait : extraWait - lead.lead;
    expect(difference <= 10us, "the lead did not converge to the steady extra GPU wait");
}

// A wait beyond the limit is held at the limit, and never above it.
void holdsAtTheLimit() {
    RealPresentLead lead;
    for (uint64_t source = 1; source <= 200; ++source) {
        const auto start = std::chrono::milliseconds{static_cast<int64_t>(source) * 20};
        expect(period(lead, source, start, 4ms, 2900us), "a limited period gave no sample");
        expect(lead.lead <= RealPresentLead::limit, "the lead rose above its limit");
    }
    expect(lead.lead == RealPresentLead::limit, "a wait beyond the limit did not reach the limit");
}

// A real that is earlier on the GPU than planned pulls the lead down, and it is never negative.
void neverNegative() {
    RealPresentLead lead;
    for (uint64_t source = 1; source <= 50; ++source) {
        const auto start = std::chrono::milliseconds{static_cast<int64_t>(source) * 20};
        static_cast<void>(period(lead, source, start, 4ms, -1ms));
        expect(lead.lead >= Duration{}, "the lead went negative");
    }
    expect(lead.lead == Duration{}, "a negative extra wait did not hold the lead at zero");
}

// An error beyond errorReject (3 ms) in either direction gives no sample and does not move the lead.
void rejectsLargeErrors() {
    RealPresentLead lead;
    lead.lead = 1ms;
    expect(!period(lead, 1, 0ms, 4ms, 4500us), "a +3.5 ms error was not rejected");
    expect(lead.lead == 1ms, "a rejected error moved the lead");
    expect(!period(lead, 2, 40ms, 4ms, -2500us), "a -3.5 ms error was not rejected");
    expect(lead.lead == 1ms, "a rejected negative error moved the lead");
}

// Markers pair only with the call records of their own context and source.
void mismatchedSourceOrContextGivesNoSample() {
    RealPresentLead lead;
    lead.call(context, 9, true, 0ms, 0ms);
    lead.call(context, 9, false, 4ms, 4ms);
    lead.call(context + 1, 9, true, 0ms, 0ms);
    expect(!lead.observe({.context = context, .source = 9, .gpuNs = 0, .generated = true}) &&
        !lead.observe({.context = context + 1, .source = 9, .gpuNs = 4'000'000, .generated = false}),
        "a real of another context paired with a generated marker");
    expect(!lead.observe({.context = context, .source = 9, .gpuNs = 0, .generated = true}) &&
        !lead.observe({.context = context, .source = 10, .gpuNs = 4'000'000, .generated = false}),
        "a real of another source paired with a generated marker");
    expect(!lead.observe({.context = context, .source = 9, .gpuNs = 4'000'000, .generated = false}),
        "a real marker with no generated marker gave a sample");
    expect(lead.lead == Duration{}, "a mismatched pair moved the lead");
}

// A pair needs both call records. Markers without them give no sample.
void missingCallRecordsGiveNoSample() {
    RealPresentLead lead;
    expect(!lead.observe({.context = context, .source = 3, .gpuNs = 0, .generated = true}) &&
        !lead.observe({.context = context, .source = 3, .gpuNs = 4'000'000, .generated = false}),
        "markers without call records gave a sample");
    lead.call(context, 3, false, 4ms, 4ms);
    expect(!lead.observe({.context = context, .source = 3, .gpuNs = 0, .generated = true}) &&
        !lead.observe({.context = context, .source = 3, .gpuNs = 4'000'000, .generated = false}),
        "a pair without its generated call record gave a sample");
}

// forget drops pending markers and records but keeps the lead. reset also zeroes the lead.
void forgetKeepsLeadAndResetZeroes() {
    RealPresentLead lead;
    for (uint64_t source = 1; source <= 100; ++source) {
        const auto start = std::chrono::milliseconds{static_cast<int64_t>(source) * 20};
        static_cast<void>(period(lead, source, start, 4ms, 1700us));
    }
    const Duration kept = lead.lead;
    expect(kept > Duration{}, "the lead did not rise before the forget check");
    lead.call(context, 200, true, 0ms, 0ms);
    lead.call(context, 200, false, 4ms, 4ms);
    static_cast<void>(lead.observe({.context = context, .source = 200, .gpuNs = 0, .generated = true}));
    lead.forget();
    expect(lead.lead == kept, "forget changed the lead");
    expect(!lead.observe({.context = context, .source = 200, .gpuNs = 4'000'000, .generated = false}),
        "forget kept a pending generated marker or call record");
    lead.reset();
    expect(lead.lead == Duration{}, "reset did not zero the lead");
    // After a reset the first period moves the lead by one clamped step: an error of 1.7 ms clamps to 1 ms, and gain 8.
    expect(period(lead, 300, 0ms, 4ms, 1700us) && lead.lead == 125us,
        "a period after reset did not move the lead by one clamped step");
}
}

int main() {
    convergesToTheExtraWait();
    holdsAtTheLimit();
    neverNegative();
    rejectsLargeErrors();
    mismatchedSourceOrContextGivesNoSample();
    missingCallRecordsGiveNoSample();
    forgetKeepsLeadAndResetZeroes();
    std::cout << "real present lead tests passed\n";
    return 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/uncapped_auto_cap.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace mako::layer;

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
}

int main() {
    refreshFromTiming();
    std::cout << "uncapped auto cap tests passed\n";
    return 0;
}

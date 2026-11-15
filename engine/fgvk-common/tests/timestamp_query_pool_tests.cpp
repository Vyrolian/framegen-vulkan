/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "fgvk-common/vulkan/timestamp_query_pool.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }

    bool rejects(const uint32_t validBits, const float periodNs) {
        try {
            static_cast<void>(vk::timestampElapsedMicroseconds(0, 1, validBits, periodNs));
        } catch (const ls::error&) {
            return true;
        }
        return false;
    }

    void testElapsedMicroseconds() {
        // Counter rollover must use the queue's valid width, including 64 bits.
        expect(std::abs(vk::timestampElapsedMicroseconds(250, 4, 8, 100.0F) - 1.0) < 1e-10,
            "an 8-bit timestamp rollover must wrap at the queue's valid width");
        expect(std::abs(vk::timestampElapsedMicroseconds(UINT64_MAX - 4, 5, 64, 100.0F) - 1.0) < 1e-10,
            "a 64-bit timestamp rollover must wrap at the full counter width");

        // Queues without timestamps, counters wider than 64 bits and invalid periods are rejected.
        expect(rejects(0, 1.0F), "a zero valid-bit count must be rejected");
        expect(rejects(65, 1.0F), "a valid-bit count above 64 must be rejected");
        expect(rejects(64, 0.0F), "a zero timestamp period must be rejected");
        expect(rejects(64, std::numeric_limits<float>::infinity()),
            "an infinite timestamp period must be rejected");
    }
}

int main() {
    testElapsedMicroseconds();
    return 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "fgvk-common/vulkan/queue_priority.hpp"

#include <cstdlib>
#include <iostream>

namespace {
    void expect(const bool condition, const char* message) {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }

    void expectAccepted(const char* text, const float priority) {
        const auto parsed = vk::parseBackendQueuePriority(text);
        expect(parsed.set && parsed.accepted && parsed.priority == priority,
            "an accepted FGVK_BACKEND_QUEUE_PRIORITY value was not used");
    }

    void expectRejected(const char* text) {
        const auto parsed = vk::parseBackendQueuePriority(text);
        expect(parsed.set && !parsed.accepted && parsed.priority == 1.0F,
            "a rejected FGVK_BACKEND_QUEUE_PRIORITY value did not fall back to 1");
    }
}

int main() {
    const auto unset = vk::parseBackendQueuePriority(nullptr);
    expect(!unset.set && !unset.accepted && unset.priority == 1.0F,
        "an unset FGVK_BACKEND_QUEUE_PRIORITY did not give priority 1");

    expectAccepted("0", 0.0F);
    expectAccepted("0.5", 0.5F);
    expectAccepted("1", 1.0F);

    expectRejected("1.5");
    expectRejected("-1");
    expectRejected("abc");
    expectRejected("0.5x");
    expectRejected("nan");
    expectRejected("inf");
    expectRejected("");

    std::cout << "backend queue priority tests passed\n";
    return 0;
}

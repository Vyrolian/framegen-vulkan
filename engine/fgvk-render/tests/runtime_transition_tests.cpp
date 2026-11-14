/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "runtime_transition.hpp"
#include "presentation_policy.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>

using namespace fgvk::layer;
using namespace std::chrono_literals;

namespace {
    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

int main() {
    const auto start = std::chrono::steady_clock::time_point{};

    PrivateResourceTransition<int> resources;
    resources.request(2, 10, 500ms, start);
    expect(resources.pendingRequest() &&
            resources.phase() ==
                PrivateResourceTransitionPhase::Debouncing,
        "a private resource request must enter the debounce phase");
    expect(!resources.beginPreparation(start + 499ms) &&
            resources.beginPreparation(start + 500ms),
        "private resource preparation must respect the quiet period");
    resources.prepared();
    expect(resources.draining(),
        "prepared private resources must wait for old work to drain");
    resources.request(4, 11, 500ms, start + 600ms);
    expect(resources.phase() ==
                PrivateResourceTransitionPhase::Debouncing &&
            resources.value() == 4,
        "a newer request must replace an uncommitted prepared request");
    expect(resources.beginPreparation(start + 1100ms),
        "the replacement request did not become ready");
    resources.failed(1s, start + 1100ms);
    expect(resources.phase() == PrivateResourceTransitionPhase::Failed &&
            !resources.beginPreparation(start + 2099ms) &&
            resources.beginPreparation(start + 2100ms),
        "failed private resource preparation must use bounded retry");
    resources.prepared();
    expect(resources.committed() == 11 && !resources.pendingRequest() &&
            resources.phase() == PrivateResourceTransitionPhase::Idle,
        "a committed private resource request must publish its revision and return idle");
    resources.request(3, 12, 0ms, start + 3s);
    resources.cancel();
    expect(!resources.pendingRequest() &&
            resources.phase() == PrivateResourceTransitionPhase::Idle,
        "cancelling a private resource request must discard all transition state");

    resources.request(7, 23, 0ms, start + 4s);
    expect(resources.beginPreparation(start + 4s),
        "the request used to test restart must prepare immediately");
    resources.prepared();
    resources.restartPreparation(500ms, start + 4001ms);
    expect(resources.phase() ==
                PrivateResourceTransitionPhase::Debouncing &&
            resources.value() == 7 && resources.stateRevision() == 23,
        "restart must preserve the last request and its revision");
    expect(!resources.beginPreparation(start + 4499ms) &&
            resources.beginPreparation(start + 4501ms),
        "restart must establish a fresh quiet period");

    std::cout << "runtime transition tests passed\n";
    return 0;
}

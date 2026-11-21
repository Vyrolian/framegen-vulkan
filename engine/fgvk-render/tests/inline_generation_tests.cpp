/* SPDX-License-Identifier: GPL-3.0-or-later */

// FI1: pure helpers of the attached (same-device) generation path: the submit order of the source copy, the release
// value of a batch scheduled after its copy, and where a failed schedule presents from.

#include "swapchain/inline_generation.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
    using fgvk::layer::ScheduleFailureRoute;

    void require(const bool condition, const std::string& message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    void testSubmitOrder() {
        require(fgvk::layer::sourceCopyPrecedesSchedule(true),
            "an attached source copy must be submitted before its batch");
        require(!fgvk::layer::sourceCopyPrecedesSchedule(false),
            "a detached source copy must keep its existing order after the batch");
    }

    void testReleaseValue() {
        // A copy of value S advances the index to S + 1. A batch with G outputs then signals S + 1 .. S + G, and its
        // source is released at S + G.
        for (uint64_t copy = 1; copy <= 9; ++copy) {
            for (size_t outputs = 1; outputs <= 4; ++outputs) {
                const uint64_t release = fgvk::layer::attachedSourceReleaseValue(copy + 1, outputs);
                require(release == copy + outputs,
                    "the attached release value must be the copy's value plus the output count");
            }
        }
        require(fgvk::layer::attachedSourceReleaseValue(2, 1) == 2,
            "the first single-output attached batch must release at value 2");
    }

    void testFailureRoutes() {
        require(fgvk::layer::scheduleFailureRoute(true, false) == ScheduleFailureRoute::PostCopyOriginal,
            "an attached failure without preacquired images must present through the post-copy semaphore");
        require(fgvk::layer::scheduleFailureRoute(true, true) == ScheduleFailureRoute::PostCopyRetirement,
            "an attached failure with preacquired images must retire them through the post-copy semaphore");
        require(fgvk::layer::scheduleFailureRoute(false, false) == ScheduleFailureRoute::NativeOnly,
            "a detached failure without preacquired images must stay native");
        require(fgvk::layer::scheduleFailureRoute(false, true) == ScheduleFailureRoute::NativeWithRetirement,
            "a detached failure with preacquired images must keep its retirement route");
        for (const bool preacquired : {false, true}) {
            const auto route = fgvk::layer::scheduleFailureRoute(true, preacquired);
            require(route == ScheduleFailureRoute::PostCopyOriginal || route == ScheduleFailureRoute::PostCopyRetirement,
                "an attached failure must never re-wait the application's wait semaphores");
        }
    }
}

int main() {
    try {
        testSubmitOrder();
        testReleaseValue();
        testFailureRoutes();
    } catch (const std::exception& error) {
        std::cerr << "Inline generation test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "inline generation tests passed\n";
    return 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */

// FI-default: the FGVK_FG_INLINE parse, where only the exact value "0" turns the attached path off.
// FI1: pure helpers of the attached (same-device) generation path: the submit order of the source copy, the release
// value of a batch scheduled after its copy, and where a failed schedule presents from.
// FI2: the call-held lower budget (rule 2) and the hold check.

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

    // FI-default: unset (null), empty and every value other than "0" attach. Only "0" exactly turns the switch off.
    void testSwitchParse() {
        using fgvk::layer::parseInlineGenerationSwitch;
        static_assert(parseInlineGenerationSwitch(nullptr), "the switch parse must be usable in constant expressions");
        static_assert(!parseInlineGenerationSwitch("0"), "the switch parse must be usable in constant expressions");
        require(parseInlineGenerationSwitch(nullptr), "an unset FGVK_FG_INLINE must attach by default");
        require(parseInlineGenerationSwitch(""), "an empty FGVK_FG_INLINE must attach");
        require(parseInlineGenerationSwitch("1"), "FGVK_FG_INLINE=1 must attach");
        require(!parseInlineGenerationSwitch("0"), "FGVK_FG_INLINE=0 must turn the attached path off");
        require(parseInlineGenerationSwitch("00"), "only the exact value 0 turns the attached path off, not 00");
        require(parseInlineGenerationSwitch("yes"), "an unrecognised FGVK_FG_INLINE value must attach");
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

    // FI2 rule 2: the budget is the lower images above the surface minimum and the worker's own holds, capped at four.
    void testCallBudget() {
        using fgvk::layer::inlineCallLowerBudget;
        require(fgvk::layer::inlineCallLowerImageCap == 4, "the call-held budget cap must be four");
        require(inlineCallLowerBudget(8, 4, 2) == 2, "two spare images must give a budget of two");
        require(inlineCallLowerBudget(10, 4, 2) == 4, "four spare images must give the full budget of four");
        require(inlineCallLowerBudget(12, 4, 2) == 4, "more spare images than the cap must still give four");
        require(inlineCallLowerBudget(9, 4, 0) == 4, "a spare count above the cap must be capped with no worker holds");
        require(inlineCallLowerBudget(7, 4, 0) == 3, "the budget must count the images above the surface minimum");
        require(inlineCallLowerBudget(6, 4, 2) == 0, "no spare image must give a budget of zero");
        require(inlineCallLowerBudget(5, 4, 2) == 0, "a negative spare count must give a budget of zero");
        require(inlineCallLowerBudget(0, 0, 0) == 0, "an empty swapchain must give a budget of zero");
        require(inlineCallLowerBudget(10, UINT32_MAX, 0) == 0,
            "a surface minimum above the image count must give a budget of zero");
    }

    // FI2 rule 2: one more output may be held when the outputs already held, plus this one, stay within the budget.
    void testCallHoldFits() {
        require(fgvk::layer::inlineCallHoldFits(0, 1), "one output must fit a budget of one");
        require(!fgvk::layer::inlineCallHoldFits(1, 1), "a second output must not fit a budget of one");
        require(fgvk::layer::inlineCallHoldFits(1, 2), "a second output must fit a budget of two");
        require(!fgvk::layer::inlineCallHoldFits(2, 2), "a third output must not fit a budget of two");
        require(fgvk::layer::inlineCallHoldFits(3, 4), "a fourth output must fit a budget of four");
        require(!fgvk::layer::inlineCallHoldFits(4, 4), "a fifth output must not fit a budget of four");
        require(!fgvk::layer::inlineCallHoldFits(0, 0), "no output may be held with a zero budget");
    }
}

int main() {
    try {
        testSwitchParse();
        testSubmitOrder();
        testReleaseValue();
        testFailureRoutes();
        testCallBudget();
        testCallHoldFits();
    } catch (const std::exception& error) {
        std::cerr << "Inline generation test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "inline generation tests passed\n";
    return 0;
}

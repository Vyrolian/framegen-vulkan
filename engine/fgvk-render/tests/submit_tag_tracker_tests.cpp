/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/submit_tag_tracker.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>

using namespace fgvk::layer;

namespace {
void expect(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

template<class T> T handle(uintptr_t value) { return reinterpret_cast<T>(value); }

const auto queueA = handle<VkQueue>(0x6100);
const auto queueB = handle<VkQueue>(0x6200);

// A submit whose transition is not checked here (the counts are checked by the calls that do).
void submit(SubmitTagTracker& tracker, const VkQueue queue, const uint64_t tag) {
    static_cast<void>(tracker.observe(queue, tag));
}

// A transition with exactly these values.
bool isTransition(const std::optional<SubmitTagTransition>& value, uint64_t previousTag, uint64_t submits,
        uint64_t untagged) {
    return value && value->previousTag == previousTag && value->submits == submits && value->untagged == untagged;
}

// The first non-zero tag on a queue returns previous 0.
void firstTagReturnsZeroPrevious() {
    SubmitTagTracker tracker;
    expect(isTransition(tracker.observe(queueA, 5), 0, 0, 0), "the first tag 5 did not return previous 0");
}

// A repeated tag counts its submits and returns nothing.
void repeatedTagReturnsNothing() {
    SubmitTagTracker tracker;
    submit(tracker, queueA, 5);
    for (int call = 0; call < 4; ++call)
        expect(!tracker.observe(queueA, 5).has_value(), "a repeated tag 5 returned a transition");
}

// Untagged calls count between transitions. Before the first tag they are reported with it.
void untaggedCallsCount() {
    SubmitTagTracker tracker;
    expect(!tracker.observe(queueA, 0).has_value(), "an untagged call returned a transition");
    expect(!tracker.observe(queueA, 0).has_value(), "an untagged call returned a transition");
    expect(isTransition(tracker.observe(queueA, 5), 0, 0, 2),
        "the first tag 5 did not report the two untagged calls before it");
    expect(!tracker.observe(queueA, 0).has_value(), "an untagged call after tag 5 returned a transition");
    expect(!tracker.observe(queueA, 5).has_value(), "a repeated tag 5 returned a transition");
    expect(isTransition(tracker.observe(queueA, 6), 5, 2, 1),
        "the transition to 6 did not report 5 with two submits and one untagged call");
}

// Exact counts for a longer run: tag 5 for three submits, one untagged, then two more submits.
void transitionReportsExactCounts() {
    SubmitTagTracker tracker;
    submit(tracker, queueA, 5);
    submit(tracker, queueA, 5);
    submit(tracker, queueA, 5);
    submit(tracker, queueA, 0);
    submit(tracker, queueA, 5);
    submit(tracker, queueA, 5);
    expect(isTransition(tracker.observe(queueA, 6), 5, 5, 1),
        "the transition to 6 did not report 5 with five submits and one untagged call");
    // The counts restart at the transition: 6 is counted from its own call, and the next untagged call is one.
    expect(!tracker.observe(queueA, 6).has_value(), "a repeated tag 6 returned a transition");
    expect(!tracker.observe(queueA, 0).has_value(), "an untagged call returned a transition");
    expect(isTransition(tracker.observe(queueA, 7), 6, 2, 1),
        "the counts after the transition to 6 did not restart");
}

// Two queues keep separate tags and counts; a call on one never changes the other.
void queuesAreIndependent() {
    SubmitTagTracker tracker;
    submit(tracker, queueA, 5);
    submit(tracker, queueB, 9);
    submit(tracker, queueA, 5);
    submit(tracker, queueB, 0);
    submit(tracker, queueB, 0);
    submit(tracker, queueB, 9);
    submit(tracker, queueA, 0);
    expect(isTransition(tracker.observe(queueA, 6), 5, 2, 1),
        "queue A's counts took calls made on queue B");
    expect(isTransition(tracker.observe(queueB, 10), 9, 2, 2),
        "queue B's counts took calls made on queue A");
}

// An untagged call never returns a transition, and the transition on the first tag reports those calls.
void zeroNeverReturns() {
    SubmitTagTracker tracker;
    for (int call = 0; call < 3; ++call)
        expect(!tracker.observe(queueA, 0).has_value(), "a zero tag returned a transition");
    expect(isTransition(tracker.observe(queueA, 5), 0, 0, 3),
        "the first tag after three zero tags did not report them");
}

// The presentID of a VkLatencySubmissionPresentIdNV anywhere in the chain; 0 when the chain has none. The chains
// are built from real structs, linked through pNext as an application links them.
void submissionPresentIdWalksTheChain() {
    expect(submissionPresentId(nullptr) == 0, "an empty chain had a tag");
    VkTimelineSemaphoreSubmitInfo alone{};
    alone.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    expect(submissionPresentId(&alone) == 0, "a chain without the tag had a tag");
    VkLatencySubmissionPresentIdNV tag{
        .sType = VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV, .pNext = nullptr, .presentID = 7};
    expect(submissionPresentId(&tag) == 7, "the tag at the head of the chain was not read");
    VkTimelineSemaphoreSubmitInfo before{};
    before.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    before.pNext = &tag;
    expect(submissionPresentId(&before) == 7, "the tag behind another node was not read");
}
}

int main() {
    firstTagReturnsZeroPrevious();
    repeatedTagReturnsNothing();
    untaggedCallsCount();
    transitionReportsExactCounts();
    queuesAreIndependent();
    zeroNeverReturns();
    submissionPresentIdWalksTheChain();
    std::cout << "submit tag tracker tests passed\n";
}

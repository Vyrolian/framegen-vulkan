/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/queue_start_probe.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <vector>

using namespace fgvk::layer;

namespace {
void expect(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

template<class T> T handle(uintptr_t value) { return reinterpret_cast<T>(value); }

// The first call opens a frame with a start and an end query.
void firstCallOpensFrame() {
    QueueFrameRing ring(8);
    const auto plan = ring.begin(5, 100);
    expect(plan && plan->start && plan->startQuery == 0 && plan->endQuery == 1, "first call did not open a frame");
    expect(ring.oldest() == nullptr, "an open frame is pending");
}

// Calls with the same tag or untagged extend the frame: end only, the previous end released.
void sameTagAndUntaggedExtend() {
    QueueFrameRing ring(8);
    static_cast<void>(ring.begin(5, 100));
    const auto tagged = ring.begin(5, 200);
    expect(tagged && !tagged->start && tagged->startQuery == 0 && tagged->endQuery == 2, "same tag did not extend");
    const auto untagged = ring.begin(0, 300);
    expect(untagged && !untagged->start && untagged->endQuery == 3, "untagged did not extend");
    const auto next = ring.begin(6, 400);
    expect(next && next->start, "a new tag did not open a frame");
    const auto* frame = ring.oldest();
    expect(frame && frame->presentId == 5 && frame->submits == 3 && frame->submitNs == 100 &&
        frame->lastSubmitNs == 300 && frame->startQuery == 0 && frame->endQuery == 3, "closed frame is wrong");
    // Released end queries 1 and 2 are free again after the ring wraps.
    expect(next->startQuery == 4 && next->endQuery == 5, "next frame queries are wrong");
}

// Untagged submits before any tag form a frame with ID 0; a later tag opens a new frame.
void untaggedFirstFrame() {
    QueueFrameRing ring(8);
    const auto plan = ring.begin(0, 10);
    expect(plan && plan->start, "untagged first call did not open a frame");
    static_cast<void>(ring.begin(7, 20));
    expect(ring.oldest() && ring.oldest()->presentId == 0, "untagged frame did not close with ID 0");
}

// A held query blocks reuse: the call carries nothing and its frame counts as skipped.
void heldQuerySkipsFrame() {
    QueueFrameRing ring(4);
    static_cast<void>(ring.begin(1, 1));                  // queries 0, 1
    static_cast<void>(ring.begin(2, 2));                  // closes 1; queries 2, 3
    const auto blocked = ring.begin(3, 3);                // closes 2; next index 0 is held by frame 1
    expect(!blocked, "a held query was reused");
    expect(ring.takeSkipped() == 1, "skipped frame not counted");
    expect(!ring.begin(3, 4), "a skipped frame's later call carried timestamps");
    ring.retireOldest();                                  // frame 1 read: 0, 1 free
    const auto plan = ring.begin(4, 5);
    expect(plan && plan->start && plan->startQuery == 0 && plan->endQuery == 1, "freed queries not reused");
    expect(ring.pendingFrames() == 1 && ring.oldest()->presentId == 2, "skipped frame became pending");
}

// An extending call without a free query drops the frame: its end would be an earlier call's.
void extendWithoutQueryDropsFrame() {
    QueueFrameRing ring(2);
    static_cast<void>(ring.begin(1, 1));  // queries 0, 1
    expect(!ring.begin(1, 2), "extended without a free query");
    expect(ring.takeSkipped() == 1, "dropped frame not counted as skipped");
    const auto plan = ring.begin(2, 3);
    expect(plan && plan->start, "queries of the dropped frame were not released");
    expect(ring.oldest() == nullptr, "dropped frame became pending");
}

// More closed frames than the pending limit drop the oldest.
void pendingOverflowDropsOldest() {
    QueueFrameRing ring(1024);
    for (uint64_t tag = 1; tag <= QueueFrameRing::maximumPending + 2; ++tag)
        static_cast<void>(ring.begin(tag, static_cast<int64_t>(tag)));
    expect(ring.pendingFrames() == QueueFrameRing::maximumPending, "pending exceeded its limit");
    expect(ring.takeDropped() == 1, "overflow not counted");
    expect(ring.oldest()->presentId == 2, "the oldest frame was not the one dropped");
}

// The call tag is the first nonzero tag of its submit infos.
void callTagIsFirstNonzero() {
    VkLatencySubmissionPresentIdNV tagged{.sType = VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV,
        .presentID = 9};
    std::vector<VkSubmitInfo2> submits(3, VkSubmitInfo2{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2});
    submits[1].pNext = &tagged;
    const auto tagOf = [](const void* chain) {
        return chain ? static_cast<const VkLatencySubmissionPresentIdNV*>(chain)->presentID : uint64_t{0};
    };
    expect(submitCallTag(std::span<const VkSubmitInfo2>{submits}, tagOf) == 9, "call tag is wrong");
    submits[1].pNext = nullptr;
    expect(submitCallTag(std::span<const VkSubmitInfo2>{submits}, tagOf) == 0, "untagged call has a tag");
}

VkCommandBufferSubmitInfo bufferInfo(uintptr_t value, uint32_t mask) {
    return {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = handle<VkCommandBuffer>(value), .deviceMask = mask};
}

// One submit info gets the start in front and the end behind; everything else is kept.
void rewriteSingleInfo() {
    const auto start = handle<VkCommandBuffer>(0x51);
    const auto end = handle<VkCommandBuffer>(0x52);
    int chain = 0;
    const VkCommandBufferSubmitInfo original[2]{bufferInfo(0x10, 3), bufferInfo(0x11, 3)};
    VkSemaphoreSubmitInfo wait{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    const VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .pNext = &chain, .flags = 0,
        .waitSemaphoreInfoCount = 1, .pWaitSemaphoreInfos = &wait, .commandBufferInfoCount = 2,
        .pCommandBufferInfos = original};
    TimestampedSubmits out;
    expect(rewriteSubmitsWithTimestamps(std::span{&submit, 1}, start, end, out), "single rewrite failed");
    const auto& info = out.infos.at(0);
    expect(info.pNext == &chain && info.waitSemaphoreInfoCount == 1 && info.pWaitSemaphoreInfos == &wait,
        "single rewrite changed the submit");
    expect(info.commandBufferInfoCount == 4, "single rewrite count is wrong");
    expect(info.pCommandBufferInfos[0].commandBuffer == start && info.pCommandBufferInfos[0].deviceMask == 3 &&
        info.pCommandBufferInfos[1].commandBuffer == original[0].commandBuffer &&
        info.pCommandBufferInfos[2].commandBuffer == original[1].commandBuffer &&
        info.pCommandBufferInfos[3].commandBuffer == end, "single rewrite order is wrong");
    expect(original[0].commandBuffer == handle<VkCommandBuffer>(0x10), "the original array changed");
}

// Several infos: start on the first, end on the last, middle untouched; zero-buffer infos still get them.
void rewriteSeveralInfos() {
    const auto end = handle<VkCommandBuffer>(0x52);
    const VkCommandBufferSubmitInfo middle[1]{bufferInfo(0x20, 0)};
    std::vector<VkSubmitInfo2> submits(3, VkSubmitInfo2{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2});
    submits[1].commandBufferInfoCount = 1;
    submits[1].pCommandBufferInfos = middle;
    TimestampedSubmits out;
    expect(rewriteSubmitsWithTimestamps(std::span<const VkSubmitInfo2>{submits}, handle<VkCommandBuffer>(0x51), end,
        out), "several rewrite failed");
    expect(out.infos.size() == 3, "several rewrite info count is wrong");
    expect(out.infos[0].commandBufferInfoCount == 1 &&
        out.infos[0].pCommandBufferInfos[0].commandBuffer == handle<VkCommandBuffer>(0x51),
        "first info did not get the start");
    expect(out.infos[1].commandBufferInfoCount == 1 && out.infos[1].pCommandBufferInfos == middle,
        "middle info changed");
    expect(out.infos[2].commandBufferInfoCount == 1 && out.infos[2].pCommandBufferInfos[0].commandBuffer == end,
        "last info did not get the end");
    // An extending call has no start.
    expect(rewriteSubmitsWithTimestamps(std::span<const VkSubmitInfo2>{submits}, VK_NULL_HANDLE, end, out) &&
        out.infos[0].commandBufferInfoCount == 0 && out.infos[2].commandBufferInfoCount == 1,
        "extending rewrite added a start");
    expect(!rewriteSubmitsWithTimestamps(std::span<const VkSubmitInfo2>{}, VK_NULL_HANDLE, end, out),
        "empty call was rewritten");
}
}

int main() {
    firstCallOpensFrame();
    sameTagAndUntaggedExtend();
    untaggedFirstFrame();
    heldQuerySkipsFrame();
    extendWithoutQueryDropsFrame();
    pendingOverflowDropsOldest();
    callTagIsFirstNonzero();
    rewriteSingleInfo();
    rewriteSeveralInfos();
    std::cout << "queue start probe tests passed\n";
    return 0;
}

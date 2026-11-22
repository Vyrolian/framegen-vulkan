/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/internal_images.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

using namespace fgvk::layer;

namespace {
    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }

    void testRingFifoOrder() {
        InternalImageRing ring(3);
        expect(ring.acquire() == 0u && ring.acquire() == 1u && ring.acquire() == 2u,
            "a fresh ring did not hand out indices in order 0, 1, 2");
        expect(ring.acquiredCount() == 3, "three acquisitions did not count as acquired");
        expect(!ring.acquire().has_value(), "acquire succeeded with every image held");

        // A presented image stays unavailable until its copy completes.
        expect(ring.present(1) && ring.present(0),
            "presenting acquired images failed");
        expect(!ring.acquire().has_value(), "a presented image was reused before its copy completed");
        expect(ring.pendingCount() == 2 && ring.acquiredCount() == 1,
            "presented images were not counted as pending");
        // Completion joins the tail in call order. Acquires return the oldest first.
        expect(ring.complete(1, 0, ReleaseTimeline::Private) && ring.complete(0, 0, ReleaseTimeline::Private),
            "completing presented images failed");
        expect(ring.acquire() == 1u && ring.acquire() == 0u,
            "the FIFO did not return completed images in completion order");
        expect(!ring.acquire().has_value(), "a completed image was acquired twice");

        expect(ring.present(2) && ring.complete(2, 0, ReleaseTimeline::Private),
            "presenting and completing the last held image failed");
        expect(ring.acquire() == 2u, "the last completed image was not reused");
        expect(ring.acquiredCount() == 3, "steady-state acquired count drifted");
    }

    void testReleaseJoinsTailWithComplete() {
        InternalImageRing ring(3);
        expect(ring.acquire() == 0u && ring.acquire() == 1u && ring.acquire() == 2u,
            "setup acquisitions failed");
        expect(ring.present(2) && ring.complete(2, 0, ReleaseTimeline::Private) &&
                ring.present(1) && ring.complete(1, 0, ReleaseTimeline::Private) &&
                ring.present(0) && ring.complete(0, 0, ReleaseTimeline::Private),
            "setup presents failed");
        // Release shares the tail with completion; it is never an unordered list.
        expect(ring.acquire() == 2u, "the first released index was not first");
        expect(ring.release(2), "releasing an acquired image without a copy failed");
        expect(ring.acquire() == 1u && ring.acquire() == 0u && ring.acquire() == 2u,
            "release did not join the FIFO tail behind the earlier releases");
    }

    void testInvalidOperationsChangeNothing() {
        InternalImageRing ring(3);
        expect(!ring.present(0), "presenting a free image succeeded");
        expect(!ring.release(0), "releasing a free image succeeded");
        expect(!ring.complete(0, 1, ReleaseTimeline::Private), "completing a free image succeeded");
        expect(!ring.present(3) && !ring.release(3) && !ring.present(UINT32_MAX) &&
                !ring.complete(3, 1, ReleaseTimeline::Private),
            "an out-of-range index was accepted");
        expect(ring.acquiredCount() == 0 && ring.pendingCount() == 0,
            "invalid operations changed the counts");
        expect(ring.acquire() == 0u, "invalid operations changed the free order");

        expect(ring.present(0), "presenting the acquired image failed");
        expect(!ring.present(0) && !ring.release(0),
            "a second present or release of the same image succeeded");
        expect(ring.acquiredCount() == 0 && ring.pendingCount() == 1,
            "a rejected second present changed the count");
        // A pending image is neither free nor acquired: the others are handed out first.
        expect(ring.acquire() == 1u && ring.acquire() == 2u,
            "a pending image was handed out before its copy completed");
        expect(!ring.acquire().has_value(), "a pending image was acquired");

        // releaseAll is atomic: one bad index means no image is released.
        const std::vector<uint32_t> withPending{1, 0};
        expect(!ring.releaseAll(withPending), "releaseAll accepted a pending image");
        expect(ring.acquiredCount() == 2 && ring.isAcquired(1) && ring.isAcquired(2),
            "a rejected releaseAll changed an image");
        const std::vector<uint32_t> duplicate{1, 1};
        expect(!ring.releaseAll(duplicate), "releaseAll accepted a duplicate index");
        expect(ring.acquiredCount() == 2, "a duplicate releaseAll changed the count");
        const std::vector<uint32_t> valid{2, 1};
        expect(ring.releaseAll(valid) && ring.acquiredCount() == 0,
            "a valid releaseAll did not release every listed image");
        expect(ring.complete(0, 0, ReleaseTimeline::Private), "completing the pending image failed");
        expect(ring.acquire() == 2u && ring.acquire() == 1u && ring.acquire() == 0u,
            "releaseAll and complete did not append in order");
    }

    void testAcquiredCountAndAcquiredState() {
        InternalImageRing ring(2);
        expect(ring.acquiredCount() == 0 && !ring.isAcquired(0), "a fresh ring reported images held");
        expect(ring.acquire().has_value() && ring.acquiredCount() == 1 && ring.isAcquired(0),
            "one acquire did not count as one held image");
        expect(ring.acquire().has_value() && ring.acquiredCount() == 2,
            "two acquires did not count as two held images");
        expect(ring.present(0) && ring.acquiredCount() == 1 && !ring.isAcquired(0) &&
                ring.pendingCount() == 1,
            "presenting did not move the image from held to pending");
    }

    void testReleaseValuesPerIndex() {
        InternalImageRing ring(4);
        for (uint32_t index = 0; index < 4; ++index)
            expect(ring.releaseValue(index).value == 0, "a fresh index was already used");
        ring.releaseValue(2).value = 7;
        expect(ring.releaseValue(2).value == 7, "a stored release value was not kept");
        expect(ring.releaseValue(1).value == 0 && ring.releaseValue(3).value == 0,
            "a release value leaked into another index");
        bool outOfRange = false;
        try {
            static_cast<void>(ring.releaseValue(4).value);
        } catch (const std::out_of_range&) {
            outOfRange = true;
        }
        expect(outOfRange, "an out-of-range release value was not rejected");
    }

    void testCompleteStoresReleaseValue() {
        InternalImageRing ring(2);
        expect(ring.acquire() == 0u && ring.present(0), "setup failed");
        expect(ring.releaseValue(0).value == 0, "a pending image already had a release value");
        expect(ring.complete(0, 42, ReleaseTimeline::Private) && ring.releaseValue(0).value == 42,
            "complete did not record the release value");
    }

    void testReleaseValueOfReadsCompletedValue() {
        InternalImageRing ring(2);
        expect(ring.acquire() == 0u && ring.present(0), "setup failed");
        expect(ring.releaseValueOf(0).value == 0, "a pending image already had a release value");
        expect(ring.complete(0, 42, ReleaseTimeline::Private) && ring.releaseValueOf(0).value == 42,
            "releaseValueOf did not return the value stored by complete");
        bool outOfRange = false;
        try {
            static_cast<void>(ring.releaseValueOf(2).value);
        } catch (const std::out_of_range&) {
            outOfRange = true;
        }
        expect(outOfRange, "releaseValueOf accepted an index outside the ring");
    }

    // FI2b: complete stores the timeline with the value, and the stamp is what the next reuse waits on.
    void testCompleteStoresTimeline() {
        InternalImageRing ring(3);
        expect(ring.acquire() == 0u && ring.acquire() == 1u && ring.acquire() == 2u, "setup acquisitions failed");
        expect(ring.present(0) && ring.complete(0, 42, ReleaseTimeline::Call),
            "completing on the call timeline failed");
        const ReleaseStamp onCall = ring.releaseValueOf(0);
        expect(onCall.value == 42 && onCall.timeline == ReleaseTimeline::Call,
            "a call-timeline completion did not store its value and timeline");
        expect(ring.present(1) && ring.complete(1, 7, ReleaseTimeline::Private),
            "completing on the private timeline failed");
        const ReleaseStamp onPrivate = ring.releaseValueOf(1);
        expect(onPrivate.value == 7 && onPrivate.timeline == ReleaseTimeline::Private,
            "a private-timeline completion did not store its value and timeline");
        expect(ring.releaseValue(0).timeline == ReleaseTimeline::Call && ring.releaseValue(1).value == 7,
            "the releaseValue accessor did not return the stamp");
        // A later completion on the other timeline replaces the stamp, and a never-used index stays Private with zero.
        expect(ring.acquire() == 0u && ring.present(0) && ring.complete(0, 3, ReleaseTimeline::Private) &&
                ring.releaseValueOf(0).timeline == ReleaseTimeline::Private,
            "a reused index kept its old timeline");
        expect(ring.releaseValueOf(2) == ReleaseStamp{}, "a never-used index did not read as the zero stamp");
    }

    void testRingDescribeReportsStatesAndValues() {
        InternalImageRing ring(3);
        expect(ring.acquire() == 0u && ring.present(0) && ring.complete(0, 7, ReleaseTimeline::Private) &&
                ring.acquire() == 1u,
            "setup acquisitions and completion failed");
        const auto described = ring.tryDescribe();
        expect(described && described->size() == 3 &&
                (*described)[0] == std::pair{'F', uint64_t{7}} &&
                (*described)[1] == std::pair{'A', uint64_t{0}} &&
                (*described)[2] == std::pair{'F', uint64_t{0}},
            "the ring description did not report each index's state and release value");
        expect(ring.present(1), "presenting the acquired image failed");
        const auto pending = ring.tryDescribe();
        expect(pending && (*pending)[1] == std::pair{'P', uint64_t{0}},
            "a presented index was not described as pending");
    }

    void testRingSizeBounds() {
        for (const uint32_t count : {2u, 8u}) {
            InternalImageRing ring(count);
            expect(ring.acquiredCount() == 0, "a valid ring started with held images");
        }
        for (const uint32_t count : {0u, 1u, 9u}) {
            bool rejected = false;
            try {
                InternalImageRing ring(count);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            expect(rejected, "a ring size outside [2, 8] was accepted");
        }
    }

    void testAcquireWaitsForComplete() {
        // The acquire blocks until a presented image completes on another thread.
        InternalImageRing ring(2);
        expect(ring.acquire() == 0u && ring.acquire() == 1u, "setup acquisitions failed");
        expect(ring.present(0), "presenting the held image failed");
        std::atomic<bool> returned{false};
        std::optional<uint32_t> got;
        std::thread waiter([&] {
            got = ring.acquireWait(UINT64_MAX);
            returned = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        expect(!returned.load(), "an unlimited acquire returned before any image was completed");
        expect(ring.complete(0, 5, ReleaseTimeline::Private), "completing the pending image failed");
        waiter.join();
        expect(got && *got == 0u && ring.releaseValue(0).value == 5,
            "the blocked acquire did not receive the completed image");
    }

    void testAbortWakesBlockedAcquire() {
        // Every index is Pending, so no image can be freed: only abort() can end the wait.
        InternalImageRing ring(2);
        expect(ring.acquire() == 0u && ring.acquire() == 1u && ring.present(0) && ring.present(1),
            "setup of an all-pending ring failed");
        std::atomic<bool> returned{false};
        std::optional<uint32_t> got{99};
        std::thread waiter([&] {
            got = ring.acquireWait(UINT64_MAX);
            returned = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        expect(!returned.load(), "an unlimited acquire returned before abort on a ring with no free image");
        const auto abortedAt = std::chrono::steady_clock::now();
        ring.abort();
        while (!returned.load() && std::chrono::steady_clock::now() - abortedAt < std::chrono::seconds(1))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        expect(returned.load(), "abort did not wake a blocked unlimited acquire within 1 s");
        waiter.join();
        expect(!got.has_value(), "a blocked acquire woken by abort returned an index with none free");

        // A ring that was never aborted keeps the finite wait: it times out.
        InternalImageRing steady(2);
        expect(steady.acquire().has_value() && steady.acquire().has_value() && steady.present(0),
            "setup of the never-aborted ring failed");
        expect(!steady.acquireWait(20'000'000).has_value(),
            "a never-aborted ring returned an image while none was free");
    }

    void testAbortKeepsFreeImages() {
        InternalImageRing ring(2);
        expect(ring.acquire() == 0u && ring.acquire() == 1u && ring.present(0),
            "setup of the aborted ring failed");
        ring.abort();
        // Nothing is free after abort, so a finite wait returns at once with no index.
        const auto started = std::chrono::steady_clock::now();
        expect(!ring.acquireWait(1'000'000'000).has_value(),
            "an aborted ring with no free image returned an index");
        expect(std::chrono::steady_clock::now() - started < std::chrono::seconds(1),
            "an aborted ring with no free image waited");
        // A completed image is still handed out after abort.
        expect(ring.complete(0, 9, ReleaseTimeline::Private), "completing the pending image failed");
        const auto got = ring.acquireWait(1'000'000'000);
        expect(got && *got == 0u && ring.releaseValue(0).value == 9,
            "a free image was not handed out after abort");
    }

    void testAcquireWaitZeroNeverWaits() {
        InternalImageRing ring(2);
        expect(ring.acquire().has_value() && ring.acquire().has_value(), "setup acquisitions failed");
        const auto started = std::chrono::steady_clock::now();
        expect(!ring.acquireWait(0).has_value(), "a zero-timeout acquire returned an image");
        expect(std::chrono::steady_clock::now() - started < std::chrono::seconds(1),
            "a zero-timeout acquire waited");
        expect(ring.acquiredCount() == 2, "a rejected zero-timeout acquire changed the ring");
    }

    void testAcquireWaitTimesOut() {
        InternalImageRing ring(2);
        expect(ring.acquire().has_value() && ring.acquire().has_value(), "setup acquisitions failed");
        const auto started = std::chrono::steady_clock::now();
        expect(!ring.acquireWait(20'000'000).has_value(),
            "a finite acquire returned an image while none was free");
        const auto waited = std::chrono::steady_clock::now() - started;
        expect(waited >= std::chrono::milliseconds(10) && waited < std::chrono::seconds(5),
            "a finite acquire did not wait for its timeout");
        expect(ring.acquiredCount() == 2, "a timed-out acquire changed the ring");
    }

    void testCrossThreadExchange() {
        // The application can acquire on one thread and present on another.
        // Only index hand-off is shared here; the ring itself owns the counts.
        constexpr int rounds = 5000;
        InternalImageRing ring(3);
        std::mutex handoffMutex;
        std::deque<uint32_t> handoff;
        int presented = 0;
        std::thread presenter([&] {
            while (presented < rounds) {
                std::optional<uint32_t> index;
                {
                    const std::lock_guard lock(handoffMutex);
                    if (!handoff.empty()) {
                        index = handoff.front();
                        handoff.pop_front();
                    }
                }
                if (!index) {
                    std::this_thread::yield();
                    continue;
                }
                expect(ring.present(*index) && ring.complete(*index, 0, ReleaseTimeline::Private),
                    "a handed-off image was not held");
                ++presented;
            }
        });
        int acquired = 0;
        while (acquired < rounds) {
            const auto index = ring.acquire();
            if (!index) {
                std::this_thread::yield();
                continue;
            }
            ++acquired;
            const std::lock_guard lock(handoffMutex);
            handoff.push_back(*index);
        }
        presenter.join();
        expect(ring.acquiredCount() == 0 && ring.pendingCount() == 0,
            "the cross-thread exchange left images held");
    }

    InternalImageGateInput eligibleInput() {
        return InternalImageGateInput{
            .frameGenerationEnabled = true,
            .deferredPresentQueue = true,
            .applicationExtent = {.width = 1920, .height = 1080},
            .lowerExtent = {.width = 1920, .height = 1080},
            .applicationFormat = VK_FORMAT_B8G8R8A8_UNORM,
            .lowerFormat = VK_FORMAT_B8G8R8A8_UNORM,
            .imageArrayLayers = 1,
            .applicationFlags = 0,
            .lowerImageCount = 3,
            .formatBlitCapable = true,
            .imageFormatSupported = true,
        };
    }

    void testEligibleBaseline() {
        const auto input = eligibleInput();
        expect(internalImagesIneligibleReason(input) == nullptr,
            "a fully eligible swapchain reported a reason");
        expect(internalImagesEligible(input), "a fully eligible swapchain was refused");
    }

    void testEachFailingConditionAlone() {
        struct Case {
            const char* reason;
            std::function<void(InternalImageGateInput&)> mutate;
        };
        const std::vector<Case> cases{
            {"frame-generation-disabled", [](auto& in) { in.frameGenerationEnabled = false; }},
            {"no-deferred-present-queue", [](auto& in) { in.deferredPresentQueue = false; }},
            {"extent-mismatch", [](auto& in) { in.applicationExtent.width = 1280; }},
            {"extent-mismatch", [](auto& in) { in.applicationExtent.height = 720; }},
            {"format-mismatch", [](auto& in) { in.applicationFormat = VK_FORMAT_R8G8B8A8_UNORM; }},
            {"array-layers", [](auto& in) { in.imageArrayLayers = 2; }},
            {"application-min-image-count", [](auto& in) { in.applicationMinImageCount = 9; }},
            {"lower-image-count", [](auto& in) { in.lowerImageCount = 1; }},
            {"create-flags", [](auto& in) { in.applicationFlags = VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR; }},
            {"create-flags", [](auto& in) { in.applicationFlags = VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR; }},
            {"format-blit", [](auto& in) { in.formatBlitCapable = false; }},
            {"image-format", [](auto& in) { in.imageFormatSupported = false; }},
        };
        for (const auto& testCase : cases) {
            auto input = eligibleInput();
            testCase.mutate(input);
            const char* reason = internalImagesIneligibleReason(input);
            expect(reason != nullptr && std::string_view(reason) == testCase.reason,
                "a single failing condition reported the wrong reason");
            expect(!internalImagesEligible(input),
                "a single failing condition still reported eligible");
        }
    }

    void testFirstFailingReasonWins() {
        auto input = eligibleInput();
        input.frameGenerationEnabled = false;
        input.deferredPresentQueue = false;
        input.imageFormatSupported = false;
        expect(std::string_view(internalImagesIneligibleReason(input)) == "frame-generation-disabled",
            "the gate did not report the first failing condition");
    }

    void testAllowedCreateFlags() {
        auto input = eligibleInput();
#if defined(VK_KHR_present_id2)
        input.applicationFlags |= VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
#endif
#if defined(VK_KHR_present_wait2)
        input.applicationFlags |= VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
#endif
#if defined(VK_EXT_present_timing)
        input.applicationFlags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
#endif
        expect(internalImagesEligible(input),
            "the supported present-timing and present-ID create flags refused eligibility");
    }

    // Zink's mutable swapchain: allowed only with the application's view format list, and an
    // unknown flag next to it is still refused. Private images take both WSI image flags.
    void testMutableFormatNeedsFormatList() {
        auto withList = eligibleInput();
        withList.applicationFlags = VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
        withList.applicationViewFormatCount = 2;
        expect(internalImagesEligible(withList),
            "a mutable swapchain with its view format list was refused");

        auto withoutList = withList;
        withoutList.applicationViewFormatCount = 0;
        expect(std::string_view(internalImagesIneligibleReason(withoutList)) == "create-flags",
            "a mutable swapchain without a view format list was not refused as create-flags");

        auto withUnknown = withList;
        withUnknown.applicationFlags |= VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR;
        expect(std::string_view(internalImagesIneligibleReason(withUnknown)) == "create-flags",
            "an unknown flag next to the mutable bit was not refused as create-flags");

        expect(internalImageCreateFlags(VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) ==
                (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT),
            "a mutable swapchain requires mutable-format and extended-usage image flags");
        expect(internalImageCreateFlags(VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR) == 0 &&
                internalImageCreateFlags(0) == 0,
            "a swapchain flag without an image counterpart leaked into the image flags");
    }

    template<class T> T handleOf(uintptr_t value) {
        if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(value);
        else return static_cast<T>(value);
    }

    // F10: the host acquire decision. Every condition is checked alone, and counters only refuse
    // when a nonzero value has not been reached.
    void testHostAcquireEligibility() {
        const auto semaphore = handleOf<VkSemaphore>(0x10);
        const auto fence = handleOf<VkFence>(0x20);
        const VkFence none = VK_NULL_HANDLE;
        const VkSemaphore noSemaphore = VK_NULL_HANDLE;

        // Baseline: the release value is reached and the tail counter has passed its value.
        expect(hostAcquireEligible(none, semaphore, false, true, true, 5, 5, 7, 9),
            "a host acquire with both values reached was refused");
        // A counter equal to its value is reached.
        expect(hostAcquireEligible(none, semaphore, false, true, true, 5, 5, 7, 7),
            "a counter equal to its waited value was refused");

        // Each condition alone refuses the host path.
        expect(!hostAcquireEligible(fence, semaphore, false, true, true, 5, 5, 7, 9),
            "a fence did not refuse the host path");
        expect(!hostAcquireEligible(none, noSemaphore, false, true, true, 5, 5, 7, 9),
            "a missing semaphore did not refuse the host path");
        expect(!hostAcquireEligible(none, semaphore, true, true, true, 5, 5, 7, 9),
            "a disabled host path was still used");
        expect(!hostAcquireEligible(none, semaphore, false, false, true, 5, 5, 7, 9),
            "a missing tail timeline did not refuse the host path");
        expect(!hostAcquireEligible(none, semaphore, false, true, false, 5, 5, 7, 9),
            "missing counter functions did not refuse the host path");

        // A zero value needs no counter, so a zero counter is accepted.
        expect(hostAcquireEligible(none, semaphore, false, true, true, 0, 0, 7, 9),
            "a zero release value was refused for lack of a release counter");
        expect(hostAcquireEligible(none, semaphore, false, true, true, 5, 5, 0, 0),
            "a zero tail value was refused for lack of a tail counter");
        expect(hostAcquireEligible(none, semaphore, false, true, true, 0, 0, 0, 0),
            "an acquire with no outstanding work was refused");
        expect(hostAcquireEligible(none, semaphore, false, true, true, 0, 3, 0, 4),
            "nonzero counters with zero values refused the host path");

        // A counter below its nonzero value refuses, whichever wait it belongs to.
        expect(!hostAcquireEligible(none, semaphore, false, true, true, 5, 4, 7, 9),
            "a release counter below its value did not refuse the host path");
        expect(!hostAcquireEligible(none, semaphore, false, true, true, 5, 5, 7, 6),
            "a tail counter below its value did not refuse the host path");
        // A failed counter read is passed as zero, which refuses a nonzero value.
        expect(!hostAcquireEligible(none, semaphore, false, true, true, 5, 0, 0, 0),
            "a failed release read was treated as reached");
        expect(!hostAcquireEligible(none, semaphore, false, true, true, 0, 0, 7, 0),
            "a failed tail read was treated as reached");
    }

    // FI2b: each queue signals its own timeline and counts only that timeline's values. A pending
    // value on the other timeline neither moves the value nor adds a wait (the plan has no wait field).
    void testReleaseSignalPlan() {
        const ReleaseSignal firstPrivate = planReleaseSignal(true, 0, 0);
        expect(firstPrivate.timeline == ReleaseTimeline::Private && firstPrivate.value == 1,
            "the first private release did not plan value 1 on the private timeline");
        // A private release is pending at 5 and the game queue has signalled nothing yet.
        const ReleaseSignal call = planReleaseSignal(false, 5, 0);
        expect(call.timeline == ReleaseTimeline::Call && call.value == 1,
            "a call release after a pending private release was not planned on its own timeline at 1");
        const ReleaseSignal secondCall = planReleaseSignal(false, 5, 3);
        expect(secondCall.timeline == ReleaseTimeline::Call && secondCall.value == 4,
            "the call value did not count only the call timeline");
        const ReleaseSignal secondPrivate = planReleaseSignal(true, 5, 3);
        expect(secondPrivate.timeline == ReleaseTimeline::Private && secondPrivate.value == 6,
            "the private value did not count only the private timeline");
    }

    // FI2b: the acquire waits and the release signals use the semaphore of the timeline a stamp names.
    // Mutation check: returning the private semaphore for every stamp must fail this test.
    void testTimelineHandleSelection() {
        const auto privateHandle = handleOf<VkSemaphore>(0x30);
        const auto callHandle = handleOf<VkSemaphore>(0x40);
        const std::array timelines{privateHandle, callHandle};
        expect(timelineHandleOf(timelines, ReleaseTimeline::Private) == privateHandle,
            "a private-timeline value was not waited or signalled on the private semaphore");
        expect(timelineHandleOf(timelines, ReleaseTimeline::Call) == callHandle,
            "a call-timeline value was waited on the private semaphore");
    }

    void testImageCountRule() {
        struct Case {
            uint32_t minImages;
            uint32_t lowerImages;
            uint32_t expected;
        };
        const std::vector<Case> cases{
            {0, 3, 5},   // max(min, 3) + 2 gives five
            {2, 4, 5},   // three is the floor before the two extra images
            {5, 8, 7},   // the application minimum is honoured, plus two
            {8, 8, 8},   // the ring can still meet an eight-image minimum
            {3, 2, 5},   // two lower images do not limit the private count
            {3, 1, 0},   // one lower image cannot host the ring
            {0, 12, 5},  // the ceiling does not raise a small minimum
            {9, 12, 0},  // the ring cannot promise more than eight images
            {UINT32_MAX, 12, 0},
        };
        for (const auto& testCase : cases) {
            expect(internalImageCount(testCase.minImages, testCase.lowerImages) == testCase.expected,
                "the internal image count rule diverged from the specification");
        }
    }
}

// FP2: the parked pool keeps the oldest image first, marks a slot busy from its park until released, and never holds more
// than the target. parked and call-held outputs together stay within the budget.
void testLowerParkingPoolBudgetAndOrder() {
    LowerParkingPool pool;
    pool.reserveSlots(4);
    pool.setPolicy(2, 2, true);
    expect(pool.target() == 2, "a same-family policy did not keep its target of two");
    expect(pool.canPark(0), "an empty pool did not park within a budget of two");
    expect(pool.push(ParkedLower{.lowerIndex = 3, .slot = 1}), "parking the first image failed");
    expect(pool.isBusy(1) && !pool.isBusy(0), "a parked slot was not busy, or a free slot was");
    expect(pool.canPark(0), "one parked image and no call-held output did not fit a budget of two");
    expect(!pool.canPark(1), "a parked image and a call-held output fit a budget of two");
    expect(pool.push(ParkedLower{.lowerIndex = 5, .slot = 2}), "parking the second image failed");
    expect(!pool.canPark(0), "a full pool parked a third image");
    expect(pool.push(ParkedLower{.lowerIndex = 6, .slot = 0}), "the pool did not hold a third image up to its capacity");
    expect(!pool.push(ParkedLower{.lowerIndex = 7, .slot = 9}), "an out-of-range slot was pushed");
    const auto first = pool.take();
    expect(first && first->lowerIndex == 3 && first->slot == 1, "the oldest parked image was not taken first");
    expect(pool.isBusy(1), "a taken slot was free before its copy returned");
    pool.release(1);
    expect(!pool.isBusy(1), "a released slot stayed busy");
    expect(pool.size() == 2 && !pool.canPark(0), "the take did not leave the parked images at the target");
    expect(pool.isBusy(9), "an out-of-range slot did not read as busy");
}

// FP2 rule 6: a drain takes every parked image once and leaves none. Each returned slot is released after its submit.
void testLowerParkingDrainLeavesNone() {
    LowerParkingPool pool;
    pool.reserveSlots(4);
    pool.setPolicy(2, 2, true);
    expect(pool.push(ParkedLower{.lowerIndex = 0, .slot = 0}) && pool.push(ParkedLower{.lowerIndex = 2, .slot = 3}),
        "setting up two parked images failed");
    size_t drains = 0;
    while (const auto entry = pool.take()) {
        ++drains;
        pool.release(entry->slot);
    }
    expect(drains == 2 && pool.size() == 0, "a drain did not submit once per parked image, or left one");
    expect(!pool.isBusy(0) && !pool.isBusy(3), "a drained slot stayed busy after its submit");
    expect(!pool.take().has_value(), "a drained pool still held an image");
}

// FP2 rule 3 and FP2.4: a target applies only to copies of the call's family, and never exceeds the parked capacity.
// FI4: the capacity holds one parked image per output of the largest batch, so a 5x target is not clamped below five.
void testLowerParkingPolicy() {
    LowerParkingPool pool;
    pool.reserveSlots(8);
    static_assert(LowerParkingPool::capacity == 5, "the parked capacity must hold a 5x batch's outputs");
    pool.setPolicy(2, 4, false);
    expect(pool.target() == 0 && !pool.canPark(0), "a policy for another family still parked");
    for (const uint32_t outputs : {2u, 3u, 4u, 5u}) {
        pool.setPolicy(outputs, 5, true);
        expect(pool.target() == outputs, "a target of the outputs per batch was clamped below the batch");
    }
    pool.setPolicy(9, 9, true);
    expect(pool.target() == LowerParkingPool::capacity, "a target above the capacity was not capped at the capacity");
    pool.setPolicy(0, 4, true);
    expect(pool.target() == 0 && !pool.canPark(0), "a zero target still parked");
}

// FP2 rule 6 (fixed): with the park target at zero, nothing is drained and nothing is parked, so the parked images wait for
// the next acquire. The model drives the owner's decisions (parkAfterPresentAction, acquireTakesParkedFirst) and the pool
// through repeated park, target-zero and acquire cycles. The lower images the application has not got back (acquired and
// not presented) always equal the parked images, and stay within the capacity, so no acquire is starved.
void testParkedImagesSurviveTargetZero() {
    LowerParkingPool pool;
    pool.reserveSlots(4);
    const auto firstFreeSlot = [&pool]() -> uint32_t {
        for (uint32_t slot = 0; slot < 4; ++slot)
            if (!pool.isBusy(slot))
                return slot;
        return 4;
    };
    size_t acquired = 0;
    size_t presented = 0;
    for (int cycle = 0; cycle < 60; ++cycle) {
        // Two lower presents of the worker with a live target. Every third cycle has none, so the pool can run empty.
        pool.setPolicy(2, 2, true);
        const int parkingPresents = cycle % 3 == 2 ? 0 : 2;
        for (int present = 0; present < parkingPresents; ++present) {
            const uint32_t slot = firstFreeSlot();
            if (parkAfterPresentAction(false, pool.size(), 0, pool.target(), pool.budgetOutputs(),
                    pool.isBusy(slot)) == ParkOutcome::Parked) {
                ++acquired;
                expect(pool.push(ParkedLower{.lowerIndex = slot, .slot = slot}), "a parked image was not pushed");
            }
        }
        // Live FG off: the target drops to zero. A lower present now neither drains nor parks.
        pool.setPolicy(0, 2, true);
        const size_t parkedBefore = pool.size();
        expect(parkAfterPresentAction(false, pool.size(), 0, pool.target(), pool.budgetOutputs(), false) ==
            ParkOutcome::Full, "a park target of zero drained or parked an image");
        expect(pool.size() == parkedBefore, "a park target of zero changed the parked images");
        // A synchronous or worker acquire takes the oldest parked image before any fresh acquire, and presents it.
        if (acquireTakesParkedFirst(false, pool.size())) {
            const auto entry = pool.take();
            expect(entry.has_value(), "an acquire found no parked image to take");
            pool.release(entry->slot);
        } else {
            expect(pool.size() == 0 && firstFreeSlot() < 4, "a fresh acquire ran while an image was parked");
            ++acquired;
        }
        ++presented;
        expect(acquired - presented == pool.size(), "an acquired lower image was neither parked nor presented");
        expect(pool.size() <= LowerParkingPool::capacity, "the parked images exceeded the capacity");
    }
}

int main() {
    testParkedImagesSurviveTargetZero();
    testLowerParkingPoolBudgetAndOrder();
    testLowerParkingDrainLeavesNone();
    testLowerParkingPolicy();
    testRingFifoOrder();
    testReleaseJoinsTailWithComplete();
    testInvalidOperationsChangeNothing();
    testAcquiredCountAndAcquiredState();
    testReleaseValuesPerIndex();
    testCompleteStoresReleaseValue();
    testReleaseValueOfReadsCompletedValue();
    testCompleteStoresTimeline();
    testRingDescribeReportsStatesAndValues();
    testRingSizeBounds();
    testAcquireWaitsForComplete();
    testAbortWakesBlockedAcquire();
    testAbortKeepsFreeImages();
    testAcquireWaitZeroNeverWaits();
    testAcquireWaitTimesOut();
    testCrossThreadExchange();
    testEligibleBaseline();
    testEachFailingConditionAlone();
    testFirstFailingReasonWins();
    testAllowedCreateFlags();
    testMutableFormatNeedsFormatList();
    testImageCountRule();
    testHostAcquireEligibility();
    testReleaseSignalPlan();
    testTimelineHandleSelection();
    std::cout << "internal images tests passed\n";
    return 0;
}

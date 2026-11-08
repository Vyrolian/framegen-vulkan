/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/internal_images.hpp"

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

        // Presents join the tail in call order. Acquires return the oldest first.
        expect(ring.present(1) && ring.present(0),
            "presenting acquired images failed");
        expect(ring.acquire() == 1u && ring.acquire() == 0u,
            "the FIFO did not return released images in release order");
        expect(!ring.acquire().has_value(), "a presented image was acquired twice");

        expect(ring.present(2), "presenting the last held image failed");
        expect(ring.acquire() == 2u, "the last presented image was not reused");
        expect(ring.acquiredCount() == 3, "steady-state acquired count drifted");
    }

    void testReleaseJoinsTailWithPresent() {
        InternalImageRing ring(3);
        expect(ring.acquire() == 0u && ring.acquire() == 1u && ring.acquire() == 2u,
            "setup acquisitions failed");
        expect(ring.present(2) && ring.present(1) && ring.present(0),
            "setup presents failed");
        // Release shares the tail with present; it is never an unordered list.
        expect(ring.acquire() == 2u, "the first released index was not first");
        expect(ring.release(2), "releasing an acquired image without a copy failed");
        expect(ring.acquire() == 1u && ring.acquire() == 0u && ring.acquire() == 2u,
            "release did not join the FIFO tail behind the earlier releases");
    }

    void testInvalidOperationsChangeNothing() {
        InternalImageRing ring(3);
        expect(!ring.present(0), "presenting a free image succeeded");
        expect(!ring.release(0), "releasing a free image succeeded");
        expect(!ring.present(3) && !ring.release(3) && !ring.present(UINT32_MAX),
            "an out-of-range index was accepted");
        expect(ring.acquiredCount() == 0, "invalid operations changed the acquired count");
        expect(ring.acquire() == 0u, "invalid operations changed the free order");

        expect(ring.present(0), "presenting the acquired image failed");
        expect(!ring.present(0) && !ring.release(0),
            "a second present or release of the same image succeeded");
        expect(ring.acquiredCount() == 0, "a rejected second present changed the count");

        // releaseAll is atomic: one bad index means no image is released.
        expect(ring.acquire() == 1u && ring.acquire() == 2u,
            "setup acquisitions failed");
        const std::vector<uint32_t> withFree{1, 0};
        expect(!ring.releaseAll(withFree), "releaseAll accepted a free image");
        expect(ring.acquiredCount() == 2 && ring.isAcquired(1) && ring.isAcquired(2),
            "a rejected releaseAll changed an image");
        const std::vector<uint32_t> duplicate{1, 1};
        expect(!ring.releaseAll(duplicate), "releaseAll accepted a duplicate index");
        expect(ring.acquiredCount() == 2, "a duplicate releaseAll changed the count");
        const std::vector<uint32_t> valid{2, 1};
        expect(ring.releaseAll(valid) && ring.acquiredCount() == 0,
            "a valid releaseAll did not release every listed image");
        expect(ring.acquire() == 0u && ring.acquire() == 2u && ring.acquire() == 1u,
            "releaseAll did not append in the listed order");
    }

    void testAcquiredCountAndAcquiredState() {
        InternalImageRing ring(2);
        expect(ring.acquiredCount() == 0 && !ring.isAcquired(0), "a fresh ring reported images held");
        expect(ring.acquire().has_value() && ring.acquiredCount() == 1 && ring.isAcquired(0),
            "one acquire did not count as one held image");
        expect(ring.acquire().has_value() && ring.acquiredCount() == 2,
            "two acquires did not count as two held images");
        expect(ring.present(0) && ring.acquiredCount() == 1 && !ring.isAcquired(0),
            "presenting did not reduce the held count");
    }

    void testReleaseValuesPerIndex() {
        InternalImageRing ring(4);
        for (uint32_t index = 0; index < 4; ++index)
            expect(ring.releaseValue(index) == 0, "a fresh index was already used");
        ring.releaseValue(2) = 7;
        expect(ring.releaseValue(2) == 7, "a stored release value was not kept");
        expect(ring.releaseValue(1) == 0 && ring.releaseValue(3) == 0,
            "a release value leaked into another index");
        bool outOfRange = false;
        try {
            static_cast<void>(ring.releaseValue(4));
        } catch (const std::out_of_range&) {
            outOfRange = true;
        }
        expect(outOfRange, "an out-of-range release value was not rejected");
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
                expect(ring.present(*index), "a handed-off image was not held");
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
        expect(ring.acquiredCount() == 0, "the cross-thread exchange left images held");
    }

    InternalImageGateInput eligibleInput() {
        return InternalImageGateInput{
            .environmentEnabled = true,
            .nativeSwapchain = false,
            .contextCreated = true,
            .frameGenerationEnabled = true,
            .adaptive = false,
            .deferredPresentQueue = true,
            .gamescopeScalingSurface = false,
            .gamescopeDetected = false,
            .spatialScalingActive = false,
            .hdrDisplayResize = false,
            .variableSurface = false,
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
            {"environment-disabled", [](auto& in) { in.environmentEnabled = false; }},
            {"native-swapchain", [](auto& in) { in.nativeSwapchain = true; }},
            {"context-not-created", [](auto& in) { in.contextCreated = false; }},
            {"frame-generation-disabled", [](auto& in) { in.frameGenerationEnabled = false; }},
            {"adaptive", [](auto& in) { in.adaptive = true; }},
            {"no-deferred-present-queue", [](auto& in) { in.deferredPresentQueue = false; }},
            {"gamescope", [](auto& in) { in.gamescopeScalingSurface = true; }},
            {"gamescope", [](auto& in) { in.gamescopeDetected = true; }},
            {"spatial-scaling", [](auto& in) { in.spatialScalingActive = true; }},
            {"hdr-display-resize", [](auto& in) { in.hdrDisplayResize = true; }},
            {"variable-surface", [](auto& in) { in.variableSurface = true; }},
            {"extent-mismatch", [](auto& in) { in.applicationExtent.width = 1280; }},
            {"extent-mismatch", [](auto& in) { in.applicationExtent.height = 720; }},
            {"format-mismatch", [](auto& in) { in.applicationFormat = VK_FORMAT_R8G8B8A8_UNORM; }},
            {"array-layers", [](auto& in) { in.imageArrayLayers = 2; }},
            {"lower-image-count", [](auto& in) { in.lowerImageCount = 1; }},
            {"create-flags", [](auto& in) { in.applicationFlags = VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR; }},
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
        input.environmentEnabled = false;
        input.adaptive = true;
        input.imageFormatSupported = false;
        expect(std::string_view(internalImagesIneligibleReason(input)) == "environment-disabled",
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

    void testEnvironmentSwitch() {
        expect(internalSwapchainEnvironmentAllows(nullptr), "an unset switch disabled the feature");
        expect(internalSwapchainEnvironmentAllows("1"), "FGVK_INTERNAL_SWAPCHAIN=1 disabled the feature");
        expect(internalSwapchainEnvironmentAllows(""), "an empty switch disabled the feature");
        expect(internalSwapchainEnvironmentAllows("00"), "a non-exact \"0\" disabled the feature");
        expect(!internalSwapchainEnvironmentAllows("0"), "FGVK_INTERNAL_SWAPCHAIN=0 did not disable the feature");
    }

    void testImageCountRule() {
        struct Case {
            uint32_t minImages;
            uint32_t lowerImages;
            uint32_t expected;
        };
        const std::vector<Case> cases{
            {0, 3, 3},   // max(min, 3) gives three
            {2, 4, 3},   // three is the floor when the lower count allows it
            {5, 8, 5},   // the application minimum is honoured
            {9, 8, 8},   // the lower count caps the private count
            {3, 2, 2},   // two lower images still yield two private images
            {3, 1, 0},   // one lower image cannot host the ring
            {0, 12, 3},  // the eight-image ceiling does not raise a small minimum
            {12, 12, 8}, // the eight-image ceiling caps a large minimum
        };
        for (const auto& testCase : cases) {
            expect(internalImageCount(testCase.minImages, testCase.lowerImages) == testCase.expected,
                "the internal image count rule diverged from the specification");
        }
    }
}

int main() {
    testRingFifoOrder();
    testReleaseJoinsTailWithPresent();
    testInvalidOperationsChangeNothing();
    testAcquiredCountAndAcquiredState();
    testReleaseValuesPerIndex();
    testRingSizeBounds();
    testCrossThreadExchange();
    testEligibleBaseline();
    testEachFailingConditionAlone();
    testFirstFailingReasonWins();
    testAllowedCreateFlags();
    testEnvironmentSwitch();
    testImageCountRule();
    std::cout << "internal images tests passed\n";
    return 0;
}

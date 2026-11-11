/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "fgvk-common/configuration/config.hpp"
#include "profile_update.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

    /// Modes whose present never blocks on the display: the application's mode is kept
    /// and outputs are spaced on the CPU. FIFO_LATEST_READY latches the newest ready
    /// image at each refresh and drops older ones, like MAILBOX with ready-time selection.
    [[nodiscard]] constexpr bool directUnthrottledPresentation(
            const bool gamescopeDetected, const VkPresentModeKHR mode) noexcept {
        return !gamescopeDetected &&
            (mode == VK_PRESENT_MODE_MAILBOX_KHR || mode == VK_PRESENT_MODE_IMMEDIATE_KHR ||
             mode == VK_PRESENT_MODE_FIFO_LATEST_READY_KHR);
    }

    /// Native FIFO supports CPU output spacing without taking a Vulkan timing
    /// namespace. Source-cap and ready-output policies decide whether existing
    /// caller timing permits that spacing.
    [[nodiscard]] constexpr bool nativeFifoOutputPacingEligible(
            const bool gamescopeDetected, const VkPresentModeKHR mode) noexcept {
        return !gamescopeDetected && mode == VK_PRESENT_MODE_FIFO_KHR;
    }

    /// One native presentation queue for the complete context lifetime.
    /// Clock changes, incomplete batches and live FG/scaling changes must not
    /// split successive present requests between application/private queues.
    [[nodiscard]] constexpr bool nativePrivatePresentQueueEligible(
            const bool gamescopeDetected, const bool gamescopeBridge,
            const bool privateQueueAvailable) noexcept {
        return !gamescopeDetected && !gamescopeBridge && privateQueueAvailable;
    }

    struct GamescopeScalingPresentContract {
        VkPresentModeKHR lowerPresentMode{VK_PRESENT_MODE_FIFO_KHR};
        std::optional<VkPresentModeKHR> compositorPresentMode;
    };

    /// Every private bridge output needs FIFO plus a distinct bounded future
    /// deadline: FIFO alone can still lose outputs that become ready together.
    /// MAILBOX avoids the second Wayland FIFO callback on the game's thread;
    /// retain lower FIFO unless both that mode and a timing clock are proven.
    [[nodiscard]] constexpr GamescopeScalingPresentContract
    gamescopeScalingPresentContract(const VkPresentModeKHR intendedPresentMode,
            const bool gamescopeScalingSurface,
            const bool privateOrderedTransport,
            const bool lowerMailboxSupported,
            const bool refreshKnown) noexcept {
        if (gamescopeScalingSurface && privateOrderedTransport &&
                lowerMailboxSupported && refreshKnown) {
            return {
                .lowerPresentMode = VK_PRESENT_MODE_MAILBOX_KHR,
                .compositorPresentMode = VK_PRESENT_MODE_FIFO_KHR,
            };
        }
        return {.lowerPresentMode = intendedPresentMode};
    }

    [[nodiscard]] inline bool shouldRetrySwapchainWithApplicationMinimum(
            const VkResult result, const uint32_t requestedMinImages,
            const uint32_t provisionedMinImages) {
        if (requestedMinImages >= provisionedMinImages)
            return false;
        return result == VK_ERROR_INITIALIZATION_FAILED ||
            result == VK_ERROR_OUT_OF_DEVICE_MEMORY ||
            result == VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    /// Return the largest generated batch that fits beyond the application's
    /// requested WSI ownership. The intercepted real frame already occupies
    /// one of those application images, so it must not be reserved a second
    /// time. A driver returning fewer images than requested is malformed and
    /// must fail closed.
    [[nodiscard]] inline size_t
    generatedFrameCapacitySupportedByExistingSwapchain(
            const uint32_t requestedMinImages,
            const size_t returnedImages) noexcept {
        if (returnedImages < requestedMinImages)
            return 0;
        return returnedImages - requestedMinImages;
    }

    /// Lower images the layer itself holds acquired and not yet presented at
    /// one time: an internal real prepared early (acquired and copied before
    /// its deadline) plus the generated output the worker is acquiring and
    /// copying. The worker presents generated outputs one at a time, so this
    /// does not grow with the multiplier.
    inline constexpr uint32_t layerHeldLowerImages = 2;

    /// Vulkan sizing rule: the surface minimum plus the images held at once
    /// (the same rule as an application's minImageCount + 1 for one held
    /// image). Used as a floor, so the application-derived count, which the
    /// direct path and the generated capacity check rely on, never shrinks.
    /// surfaceMinImages == 0 leaves the count unchanged.
    [[nodiscard]] constexpr uint32_t lowerImageCountWithLayerFloor(
            const uint32_t current, const uint32_t surfaceMinImages,
            const uint32_t maxImages) noexcept {
        if (surfaceMinImages == 0)
            return current;
        uint32_t count = std::max(current, surfaceMinImages + layerHeldLowerImages);
        if (maxImages && count > maxImages)
            count = maxImages;
        return count;
    }

    /// Apply only the WSI mutations justified by resources provisioned on the
    /// application device. Standalone scaling needs transfer usage, but must
    /// not reserve FG images or change the application's present mode.
    /// surfaceMinImages (0 = unknown) adds the layer-needs floor for Fixed FG
    /// without combined scaling.
    [[nodiscard]] inline bool applySwapchainCreateProvisioning(
            const ls::GameConf& profile, const uint32_t maxImages,
            VkSwapchainCreateInfoKHR& createInfo,
            const bool frameGenerationProvisioned,
            const bool spatialScalingActive,
            const bool orderedFrameGenerationTransport,
            const bool swapchainImageCountCompatibility = false,
            const uint32_t surfaceMinImages = 0) {
        if (!frameGenerationProvisioned && !spatialScalingActive)
            return false;

        createInfo.imageUsage |=
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!frameGenerationProvisioned)
            return false;

        switch (profile.pacing) {
            case ls::Pacing::None:
                // Preserve the application's requested WSI ownership and add
                // one image per generated output. Ordered FG-only restores
                // the release-3.0 relief image which keeps multi-output FIFO
                // batches off the headroom-tight admission path. Potentially
                // active combined scaling deliberately omits that sixth image:
                // some engines reject or churn the enlarged lower replacement
                // pool, while the replacement-prime and native-first paths
                // protect its real frame without double-counting it. An enabled
                // 1.0 scaler remains inactive and must preserve FG-only relief.
                if (!swapchainImageCountCompatibility) {
                    const bool combinedScalingTopology =
                        spatialScalingActive ||
                        ls::spatialScalingMayActivate(profile);
                    const uint64_t orderedFrameGenerationReliefImages =
                        orderedFrameGenerationTransport &&
                            !combinedScalingTopology
                        ? 1 : 0;
                    createInfo.minImageCount = static_cast<uint32_t>(std::min(
                        static_cast<uint64_t>(
                            std::numeric_limits<uint32_t>::max()
                        ),
                        static_cast<uint64_t>(createInfo.minImageCount) +
                            generatedFrameCapacityForProfile(profile) +
                            orderedFrameGenerationReliefImages
                    ));
                    if (maxImages && createInfo.minImageCount > maxImages)
                        createInfo.minImageCount = maxImages;
                    if (!combinedScalingTopology && !profile.adaptive)
                        createInfo.minImageCount = lowerImageCountWithLayerFloor(
                            createInfo.minImageCount, surfaceMinImages, maxImages);
                }
                if (orderedFrameGenerationTransport) {
                    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
                    return true;
                }
                return false;
        }
        return false;
    }

}

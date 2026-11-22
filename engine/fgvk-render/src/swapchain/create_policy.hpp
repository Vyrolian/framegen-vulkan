/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "fgvk-common/configuration/config.hpp"
#include "profile_update.hpp"
#include "swapchain/inline_generation.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

    /// Modes whose present never blocks on the display: the application's mode is kept
    /// and outputs are spaced on the CPU. FIFO_LATEST_READY latches the newest ready
    /// image at each refresh and drops older ones, like MAILBOX with ready-time selection.
    [[nodiscard]] constexpr bool directUnthrottledPresentation(
            const VkPresentModeKHR mode) noexcept {
        return mode == VK_PRESENT_MODE_MAILBOX_KHR || mode == VK_PRESENT_MODE_IMMEDIATE_KHR ||
            mode == VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
    }

    /// The application's VK_EXT_swapchain_maintenance1 present-mode group (the modes
    /// each present may switch between), read from a swapchain create chain.
    struct PresentModeGroup {
        bool unthrottled{false};
        bool fifo{false};
    };

    [[nodiscard]] inline PresentModeGroup presentModeGroup(const void* createChain) noexcept {
        PresentModeGroup group{};
        for (const auto* node = static_cast<const VkBaseInStructure*>(createChain); node; node = node->pNext) {
            if (node->sType != VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT)
                continue;
            const auto* modes = reinterpret_cast<const VkSwapchainPresentModesCreateInfoEXT*>(node);
            for (uint32_t i = 0; modes->pPresentModes && i < modes->presentModeCount; ++i) {
                const auto mode = modes->pPresentModes[i];
                group.unthrottled |= directUnthrottledPresentation(mode);
                group.fifo |= mode == VK_PRESENT_MODE_FIFO_KHR || mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
            }
        }
        return group;
    }

    /// Worker admission by the mode actually sent on this present: FIFO and the
    /// unthrottled modes admit any Fixed multiplier (P1; the old IMMEDIATE/MAILBOX
    /// 2x limit had no recorded reason and the worker is mode-agnostic).
    [[nodiscard]] constexpr bool detachedOutputModeAdmits(
            const bool directUnthrottled, const VkPresentModeKHR mode) noexcept {
        return directUnthrottled || mode == VK_PRESENT_MODE_FIFO_KHR;
    }

    /// Native FIFO supports CPU output spacing without taking a Vulkan timing
    /// namespace. Source-cap and ready-output policies decide whether existing
    /// caller timing permits that spacing.
    [[nodiscard]] constexpr bool nativeFifoOutputPacingEligible(
            const VkPresentModeKHR mode) noexcept {
        return mode == VK_PRESENT_MODE_FIFO_KHR;
    }

    /// One native presentation queue for the complete context lifetime.
    /// Clock changes, incomplete batches and live FG changes must not
    /// split successive present requests between application/private queues.
    [[nodiscard]] constexpr bool nativePrivatePresentQueueEligible(
            const bool privateQueueAvailable) noexcept {
        return privateQueueAvailable;
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

    /// FI4b: the surface minimum M of a lower swapchain whose present-mode group is queried mode by mode
    /// (swapchain-acquire-forward-progress): the maximum over the plain minimum and each mode's minimum. A mode whose
    /// query failed (nullopt) contributes the plain minimum, so it never lowers the result.
    [[nodiscard]] inline uint32_t presentModeGroupSurfaceMinImages(
            const uint32_t plainMinImages,
            std::span<const std::optional<uint32_t>> perModeMinImages) noexcept {
        uint32_t minImages = plainMinImages;
        for (const auto& perMode : perModeMinImages)
            minImages = std::max(minImages, perMode.value_or(plainMinImages));
        return minImages;
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
    /// FI4: callImages adds the lower images of the call path to the floor (see
    /// inlineCallSpareImages). Zero for a swapchain without the call path.
    [[nodiscard]] constexpr uint32_t lowerImageCountWithLayerFloor(
            const uint32_t current, const uint32_t surfaceMinImages,
            const uint32_t maxImages, const uint32_t callImages = 0) noexcept {
        if (surfaceMinImages == 0)
            return current;
        uint32_t count = std::max(current, surfaceMinImages + layerHeldLowerImages + callImages);
        if (maxImages && count > maxImages)
            count = maxImages;
        return count;
    }

    /// Apply only the WSI mutations justified by resources provisioned on the
    /// application device. Frame generation needs transfer usage.
    /// surfaceMinImages (0 = unknown) adds the layer-needs floor for Fixed FG.
    [[nodiscard]] inline bool applySwapchainCreateProvisioning(
            const ls::GameConf& profile, const uint32_t maxImages,
            VkSwapchainCreateInfoKHR& createInfo,
            const bool frameGenerationProvisioned,
            const bool orderedFrameGenerationTransport,
            const bool swapchainImageCountCompatibility = false,
            const uint32_t surfaceMinImages = 0,
            const bool fifoGroupRelief = false,
            const uint32_t inlineCallImages = 0) {
        if (!frameGenerationProvisioned)
            return false;

        createInfo.imageUsage |=
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        switch (profile.pacing) {
            case ls::Pacing::None:
                // Preserve the application's requested WSI ownership and add
                // one image per generated output. Ordered FG-only restores
                // the release-3.0 relief image which keeps multi-output FIFO
                // batches off the headroom-tight admission path.
                // fifoGroupRelief: a direct-transport swapchain whose present-mode
                // group includes FIFO (in-game VSync can switch to it per present)
                // keeps the same relief image.
                if (!swapchainImageCountCompatibility) {
                    const uint64_t orderedFrameGenerationReliefImages =
                        orderedFrameGenerationTransport || fifoGroupRelief
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
                    createInfo.minImageCount = lowerImageCountWithLayerFloor(
                        createInfo.minImageCount, surfaceMinImages, maxImages, inlineCallImages);
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

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

    /// FI4c: retry the lower create once with the application's own count when the layer raised the count above it and
    /// the create failed, whatever the error. requestedMinImages is the application's count, provisionedMinImages the
    /// layer's. An application count at or above the layer's is never retried: there is no layer floor to drop.
    [[nodiscard]] inline bool shouldRetrySwapchainWithApplicationMinimum(
            const VkResult result, const uint32_t requestedMinImages,
            const uint32_t provisionedMinImages) noexcept {
        return result != VK_SUCCESS && requestedMinImages < provisionedMinImages;
    }

    /// FI4c: the lower create attempts of one swapchain create, in ladder order. AsBuilt is the request with the layer's
    /// count and flags. ApplicationCount drops the layer's count. ApplicationCountWithoutOwnedTiming also drops the
    /// owned present-timing bits that framegen-vulkan added to the request (never the application's own bits).
    enum class LowerCreateAttempt : uint8_t { AsBuilt, ApplicationCount, ApplicationCountWithoutOwnedTiming };

    /// FI4c: the step name of the create log lines.
    [[nodiscard]] constexpr const char* lowerCreateAttemptName(const LowerCreateAttempt attempt) noexcept {
        switch (attempt) {
        case LowerCreateAttempt::AsBuilt: return "as-built";
        case LowerCreateAttempt::ApplicationCount: return "app-count";
        case LowerCreateAttempt::ApplicationCountWithoutOwnedTiming: return "no-owned-timing";
        }
        return "as-built";
    }

    /// FI4c: the attempt that follows `previous` after it failed with `result`, or nullopt when the ladder stops (the
    /// create succeeded, or no step is left and the last error is returned).
    /// - The application-count step runs only when the layer raised the count above the application's
    ///   (shouldRetrySwapchainWithApplicationMinimum). With an equal count it would repeat the request as built, so it is skipped.
    /// - The no-owned-timing step runs after any failed step when framegen-vulkan added the owned timing bit (the
    ///   application did not ask for it itself). It is the only attempt that differs from the request in that case.
    [[nodiscard]] inline std::optional<LowerCreateAttempt> nextLowerCreateAttempt(
            const LowerCreateAttempt previous, const VkResult result,
            const uint32_t requestedMinImages, const uint32_t provisionedMinImages,
            const bool ownedTimingAddedByFgvk) noexcept {
        if (result == VK_SUCCESS)
            return std::nullopt;
        if (previous == LowerCreateAttempt::AsBuilt &&
                shouldRetrySwapchainWithApplicationMinimum(result, requestedMinImages, provisionedMinImages))
            return LowerCreateAttempt::ApplicationCount;
        if (previous != LowerCreateAttempt::ApplicationCountWithoutOwnedTiming && ownedTimingAddedByFgvk)
            return LowerCreateAttempt::ApplicationCountWithoutOwnedTiming;
        return std::nullopt;
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

    /// FI4c: the largest lower image count the layer may ask for on a swapchain whose present-mode group is queried mode
    /// by mode: the smallest non-zero of the plain maximum and each mode's maximum (0 = no limit; a failed query is
    /// ignored). Returns 0 for no limit. When a per-mode maximum narrows the limit, the result is never below the
    /// application's own count, so the layer never lowers the count the application asked for.
    [[nodiscard]] inline uint32_t presentModeGroupMaxImages(
            const uint32_t plainMaxImages,
            std::span<const std::optional<uint32_t>> perModeMaxImages,
            const uint32_t applicationMinImages) noexcept {
        uint32_t limit = plainMaxImages;
        bool narrowed = false;
        for (const auto& perMode : perModeMaxImages) {
            if (!perMode || *perMode == 0)
                continue;
            if (limit == 0 || *perMode < limit) {
                limit = *perMode;
                narrowed = true;
            }
        }
        return narrowed ? std::max(limit, applicationMinImages) : limit;
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

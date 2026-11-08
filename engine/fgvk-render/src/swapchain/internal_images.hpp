/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace vk {
    class Vulkan;
}

namespace fgvk::layer {

    struct DeferredPresentQueue;

    /// Facts the F5 internal-image gate reads from create-time state. Every
    /// field is plain data so the gate can be unit-tested without Vulkan.
    struct InternalImageGateInput {
        // FGVK_INTERNAL_SWAPCHAIN is not exactly "0"; read once per process.
        bool environmentEnabled{true};
        bool nativeSwapchain{false};
        bool contextCreated{false};
        bool frameGenerationEnabled{false};
        bool adaptive{false};
        bool deferredPresentQueue{false};
        // gamescopeScalingSurface != nullptr; the detection flag is separate.
        bool gamescopeScalingSurface{false};
        bool gamescopeDetected{false};
        bool spatialScalingActive{false};
        bool hdrDisplayResize{false};
        bool variableSurface{false};
        VkExtent2D applicationExtent{};
        VkExtent2D lowerExtent{};
        VkFormat applicationFormat{VK_FORMAT_UNDEFINED};
        VkFormat lowerFormat{VK_FORMAT_UNDEFINED};
        uint32_t imageArrayLayers{1};
        VkSwapchainCreateFlagsKHR applicationFlags{0};
        uint32_t lowerImageCount{0};
        // Optimal-tiling BLIT_SRC and BLIT_DST for the application format.
        bool formatBlitCapable{false};
        // Two-dimensional optimal image with the application usage plus TRANSFER_SRC.
        bool imageFormatSupported{false};
    };

    /// Parses FGVK_INTERNAL_SWAPCHAIN. Only the exact value "0" disables the
    /// feature; unset and every other value leave it on.
    [[nodiscard]] inline bool internalSwapchainEnvironmentAllows(const char* value) noexcept {
        return !(value != nullptr && std::string_view(value) == "0");
    }

    /// Create-time flags the application may set while internal images are used.
    [[nodiscard]] constexpr VkSwapchainCreateFlagsKHR internalImageAllowedCreateFlags() noexcept {
        VkSwapchainCreateFlagsKHR allowed = 0;
#if defined(VK_KHR_present_id2)
        allowed |= VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
#endif
#if defined(VK_KHR_present_wait2)
        allowed |= VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
#endif
#if defined(VK_EXT_present_timing)
        allowed |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
#endif
        return allowed;
    }

    /// Returns the first failing gate reason as a stable kebab-case string, or
    /// nullptr when internal application images are eligible.
    [[nodiscard]] inline const char* internalImagesIneligibleReason(
            const InternalImageGateInput& input) noexcept {
        if (!input.environmentEnabled) return "environment-disabled";
        if (input.nativeSwapchain) return "native-swapchain";
        if (!input.contextCreated) return "context-not-created";
        if (!input.frameGenerationEnabled) return "frame-generation-disabled";
        if (input.adaptive) return "adaptive";
        if (!input.deferredPresentQueue) return "no-deferred-present-queue";
        if (input.gamescopeScalingSurface || input.gamescopeDetected) return "gamescope";
        if (input.spatialScalingActive) return "spatial-scaling";
        if (input.hdrDisplayResize) return "hdr-display-resize";
        if (input.variableSurface) return "variable-surface";
        if (input.applicationExtent.width != input.lowerExtent.width ||
                input.applicationExtent.height != input.lowerExtent.height) {
            return "extent-mismatch";
        }
        if (input.applicationFormat != input.lowerFormat) return "format-mismatch";
        if (input.imageArrayLayers != 1) return "array-layers";
        if (input.lowerImageCount < 2) return "lower-image-count";
        if ((input.applicationFlags & ~internalImageAllowedCreateFlags()) != 0) return "create-flags";
        if (!input.formatBlitCapable) return "format-blit";
        if (!input.imageFormatSupported) return "image-format";
        return nullptr;
    }

    [[nodiscard]] inline bool internalImagesEligible(const InternalImageGateInput& input) noexcept {
        return internalImagesIneligibleReason(input) == nullptr;
    }

    /// Private image count: clamp(max(minImageCount, 3), 2, min(8, lower)).
    /// Returns zero when the lower swapchain cannot host two private images.
    [[nodiscard]] constexpr uint32_t internalImageCount(
            const uint32_t minImageCount, const uint32_t lowerImageCount) noexcept {
        constexpr uint32_t minCount = 2;
        constexpr uint32_t maxCount = 8;
        const uint32_t upper = std::min(maxCount, lowerImageCount);
        if (upper < minCount) return 0;
        return std::clamp(std::max(minImageCount, 3u), minCount, upper);
    }

    /// Free-image ring for the application-visible images. Acquire returns the
    /// least recently released index. Present and release return an index to
    /// the tail; GPU reuse safety is ordered by the release timeline, not here.
    /// The CPU state tracks only Free and Acquired. A presented index is Free
    /// here while its GPU copy may still be pending.
    class InternalImageRing {
    public:
        static constexpr uint32_t minCount = 2;
        static constexpr uint32_t maxCount = 8;

        /// @throws std::invalid_argument when count is outside [2, 8].
        explicit InternalImageRing(const uint32_t count)
            : states(checkedCount(count), State::Free), values(count, 0) {
            for (uint32_t index = 0; index < count; ++index)
                freeOrder.push_back(index);
        }

        /// Takes the least recently released free index.
        [[nodiscard]] std::optional<uint32_t> acquire() {
            const std::lock_guard lock(mutex);
            if (freeOrder.empty())
                return std::nullopt;
            const uint32_t index = freeOrder.front();
            freeOrder.pop_front();
            states[index] = State::Acquired;
            return index;
        }

        /// Returns an acquired index after its copy has been submitted.
        [[nodiscard]] bool present(const uint32_t index) {
            return returnToTail(index);
        }

        /// Returns an acquired index that was never presented.
        [[nodiscard]] bool release(const uint32_t index) {
            return returnToTail(index);
        }

        /// Returns every listed index only if all of them are distinct and acquired.
        [[nodiscard]] bool releaseAll(std::span<const uint32_t> indices) {
            const std::lock_guard lock(mutex);
            for (size_t i = 0; i < indices.size(); ++i) {
                const uint32_t index = indices[i];
                if (index >= states.size() || states[index] != State::Acquired)
                    return false;
                for (size_t j = 0; j < i; ++j) {
                    if (indices[j] == index)
                        return false;
                }
            }
            for (const uint32_t index : indices) {
                states[index] = State::Free;
                freeOrder.push_back(index);
            }
            return true;
        }

        /// Timeline value of the last copy that read this index. Zero means
        /// never used. The owner serialises access with the queue that signals it.
        /// @throws std::out_of_range for an index outside the ring.
        [[nodiscard]] uint64_t& releaseValue(const uint32_t index) {
            return values.at(index);
        }

        [[nodiscard]] size_t acquiredCount() const {
            const std::lock_guard lock(mutex);
            return states.size() - freeOrder.size();
        }

        [[nodiscard]] bool isAcquired(const uint32_t index) const {
            const std::lock_guard lock(mutex);
            return index < states.size() && states[index] == State::Acquired;
        }

    private:
        enum class State : uint8_t { Free, Acquired };

        static size_t checkedCount(const uint32_t count) {
            if (count < minCount || count > maxCount)
                throw std::invalid_argument("internal image ring size must be between 2 and 8");
            return count;
        }

        bool returnToTail(const uint32_t index) {
            const std::lock_guard lock(mutex);
            if (index >= states.size() || states[index] != State::Acquired)
                return false;
            states[index] = State::Free;
            freeOrder.push_back(index);
            return true;
        }

        mutable std::mutex mutex;
        std::vector<State> states;
        std::vector<uint64_t> values;
        std::deque<uint32_t> freeOrder;
    };

    /// Device support for the private image format. Filled by the Vulkan
    /// side and copied into InternalImageGateInput.
    struct InternalImageDeviceSupport {
        bool formatBlitCapable{false};
        bool imageFormatSupported{false};
    };

    /// Layer-owned application images for one eligible swapchain. The
    /// application renders into these images. Present copies the selected
    /// image into a lower swapchain image and then uses the existing
    /// Swapchain::present path with that lower index.
    class InternalSwapchainImages {
    public:
        /// Creates the private images and their synchronization objects.
        /// Returns nullptr on any failure. Partial resources are destroyed.
        [[nodiscard]] static std::shared_ptr<InternalSwapchainImages> create(
            const vk::Vulkan& vk, const VkSwapchainCreateInfoKHR& application,
            VkExtent2D extent, std::span<const VkImage> lowerImages,
            uint32_t imageCount, std::shared_ptr<DeferredPresentQueue> queue);

        /// Waits the release timeline (2 s, then vkDeviceWaitIdle) and destroys
        /// command buffers, semaphores, fences, images and memory.
        ~InternalSwapchainImages();
        InternalSwapchainImages(const InternalSwapchainImages&) = delete;
        InternalSwapchainImages& operator=(const InternalSwapchainImages&) = delete;

        [[nodiscard]] std::span<const VkImage> images() const noexcept;
        [[nodiscard]] uint32_t lowerImageCount() const noexcept;
        [[nodiscard]] bool isAcquired(uint32_t index) const;

        /// Emulated vkAcquireNextImage*. The caller has already joined the
        /// deferred present. deferredSuboptimal is the caller's suboptimal state.
        [[nodiscard]] VkResult acquireApplicationImage(uint64_t timeout,
            VkSemaphore semaphore, VkFence fence, bool deferredSuboptimal,
            uint32_t* imageIndex);

        /// vkReleaseSwapchainImages: no lower call and no GPU work.
        [[nodiscard]] VkResult releaseApplicationImages(std::span<const uint32_t> indices);

        /// Waits until the next lower-acquire slot can be reused. A zero
        /// timeout polls without sleeping, for use under the queue lock.
        [[nodiscard]] VkResult waitLowerAcquireSlot(uint64_t timeout);
        [[nodiscard]] VkSemaphore lowerAcquireSemaphore() const;
        /// Call after a successful lower acquire. Resets the slot fence and
        /// returns the slot that the copy will wait on.
        [[nodiscard]] uint32_t commitLowerAcquire();

        /// Submits the pre-recorded copy from application image p to lower
        /// image L. Waits the application waits and lowerAcquire[slot]. On
        /// success it signals copied[L] and the release timeline.
        [[nodiscard]] VkResult presentCopy(const vk::Vulkan& vk, uint32_t applicationIndex,
            uint32_t lowerIndex, uint32_t slot, std::span<const VkSemaphore> applicationWaits);
        /// Consumes the application waits without copying, so the image returns to the ring.
        [[nodiscard]] VkResult presentWithoutCopy(uint32_t applicationIndex,
            std::span<const VkSemaphore> applicationWaits);
        [[nodiscard]] VkSemaphore copiedSemaphore(uint32_t lowerIndex) const;

        /// Sticky failure. OUT_OF_DATE, SURFACE_LOST and full-screen-exclusive
        /// loss from the lower path are kept. Submit failures are also kept.
        [[nodiscard]] VkResult stickyError() const noexcept;
        void noteLowerError(VkResult error) noexcept;
        void markLowerSuboptimal() noexcept;

    private:
        struct Impl;
        explicit InternalSwapchainImages(std::unique_ptr<Impl> state);
        std::unique_ptr<Impl> impl;
    };

    /// Device support queries for the private image format and usage.
    [[nodiscard]] InternalImageDeviceSupport queryInternalImageSupport(
        const vk::Vulkan& vk, VkFormat format, VkImageUsageFlags usage);

    /// FGVK_INTERNAL_SWAPCHAIN, read once for the process.
    [[nodiscard]] bool internalSwapchainEnvironmentEnabled() noexcept;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain/internal_images.hpp"

#include "swapchain/deferred_present.hpp"
#include "fgvk-common/helpers/errors.hpp"
#include "fgvk-common/vulkan/command_buffer.hpp"
#include "fgvk-common/vulkan/device_memory_accounting.hpp"
#include "fgvk-common/vulkan/semaphore.hpp"
#include "fgvk-common/vulkan/timeline_semaphore.hpp"
#include "fgvk-common/vulkan/vulkan.hpp"

#include <array>
#include <bitset>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

    namespace {
        constexpr uint64_t destroyWaitTimeoutNs = 2'000'000'000ULL;

        VkImageMemoryBarrier barrier(const VkImage image,
                const VkAccessFlags srcAccess, const VkAccessFlags dstAccess,
                const VkImageLayout oldLayout, const VkImageLayout newLayout) {
            return VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = srcAccess,
                .dstAccessMask = dstAccess,
                .oldLayout = oldLayout,
                .newLayout = newLayout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            };
        }
    }

    struct InternalSwapchainImages::Impl {
        VkDevice device{VK_NULL_HANDLE};
        vk::VulkanDeviceFuncs funcs{};
        std::shared_ptr<vk::DeviceMemoryAccounting> accounting;
        std::shared_ptr<DeferredPresentQueue> queue;
        VkExtent2D extent{};
        InternalImageRing ring;

        std::vector<VkImage> images;
        std::vector<VkDeviceMemory> memories;
        std::vector<VkDeviceSize> memorySizes;
        std::vector<VkImage> lowerImages;

        std::optional<vk::TimelineSemaphore> release;
        uint64_t lastReleaseValue{0};
        std::vector<vk::Semaphore> copied;
        std::vector<vk::Semaphore> lowerAcquire;
        std::vector<VkFence> lowerFences;
        uint32_t lowerHead{0};
        // Index applicationIndex * lowerImages.size() + lowerIndex.
        std::vector<std::optional<vk::CommandBuffer>> copies;

        std::atomic<VkResult> sticky{VK_SUCCESS};
        std::atomic<bool> lowerSuboptimal{false};

        Impl(const vk::Vulkan& vk, const uint32_t imageCount,
                std::shared_ptr<DeferredPresentQueue> presentQueue)
            : device(vk.dev()), funcs(vk.df()),
              accounting(vk.deviceMemoryAccounting()),
              queue(std::move(presentQueue)), ring(imageCount) {}

        ~Impl();

        bool initialize(const vk::Vulkan& vk, const VkSwapchainCreateInfoKHR& application,
                VkExtent2D applicationExtent, std::span<const VkImage> lowerHandles,
                uint32_t imageCount);
        bool createImages(const vk::Vulkan& vk, const VkSwapchainCreateInfoKHR& application,
                uint32_t imageCount);
        VkCommandBuffer copyCommandFor(const vk::Vulkan& vk, uint32_t applicationIndex,
                uint32_t lowerIndex);
    };

    bool InternalSwapchainImages::Impl::initialize(const vk::Vulkan& vk,
            const VkSwapchainCreateInfoKHR& application,
            const VkExtent2D applicationExtent,
            const std::span<const VkImage> lowerHandles,
            const uint32_t imageCount) {
        extent = applicationExtent;
        lowerImages.assign(lowerHandles.begin(), lowerHandles.end());
        const size_t lowerCount = lowerImages.size();

        if (!createImages(vk, application, imageCount))
            return false;

        release.emplace(vk, 0);
        copied.reserve(lowerCount);
        for (size_t i = 0; i < lowerCount; ++i)
            copied.emplace_back(vk);

        // Slots beyond the lower count let one lower image be acquired while
        // earlier copies still hold their own slots.
        const size_t slotCount = lowerCount + 2;
        lowerAcquire.reserve(slotCount);
        lowerFences.reserve(slotCount);
        for (size_t i = 0; i < slotCount; ++i) {
            lowerAcquire.emplace_back(vk);
            // Created signalled: the first wait on each slot must return at once.
            const VkFenceCreateInfo fenceInfo{
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .flags = VK_FENCE_CREATE_SIGNALED_BIT,
            };
            VkFence fence{VK_NULL_HANDLE};
            if (funcs.CreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
                return false;
            lowerFences.push_back(fence);
        }
        copies.resize(static_cast<size_t>(imageCount) * lowerCount);
        return true;
    }

    bool InternalSwapchainImages::Impl::createImages(const vk::Vulkan& vk,
            const VkSwapchainCreateInfoKHR& application, const uint32_t imageCount) {
        images.reserve(imageCount);
        memories.reserve(imageCount);
        memorySizes.reserve(imageCount);
        for (uint32_t i = 0; i < imageCount; ++i) {
            const VkImageCreateInfo imageInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = application.imageFormat,
                .extent = {.width = extent.width, .height = extent.height, .depth = 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = application.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                .sharingMode = application.imageSharingMode,
                .queueFamilyIndexCount = application.queueFamilyIndexCount,
                .pQueueFamilyIndices = application.pQueueFamilyIndices,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };
            VkImage image{VK_NULL_HANDLE};
            if (funcs.CreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS)
                return false;
            images.push_back(image);

            VkMemoryRequirements requirements{};
            funcs.GetImageMemoryRequirements(device, image, &requirements);
            const auto memoryType = vk.findMemoryTypeIndex(requirements.memoryTypeBits, false);
            if (!memoryType)
                return false;
            const VkMemoryDedicatedAllocateInfo dedicated{
                .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                .image = image,
            };
            const VkMemoryAllocateInfo allocation{
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .pNext = &dedicated,
                .allocationSize = requirements.size,
                .memoryTypeIndex = *memoryType,
            };
            VkDeviceMemory memory{VK_NULL_HANDLE};
            if (funcs.AllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS ||
                    memory == VK_NULL_HANDLE)
                return false;
            memories.push_back(memory);
            memorySizes.push_back(requirements.size);
            accounting->recordAllocation(vk::DeviceMemoryKind::Internal, requirements.size);
            if (funcs.BindImageMemory(device, image, memory, 0) != VK_SUCCESS)
                return false;
        }
        return true;
    }

    // Records the copy for one application/lower pair on first use. The
    // SIMULTANEOUS_USE flag allows a later submission of the same pair while an
    // earlier execution is still pending, because the copy does not depend on
    // the state of any earlier execution.
    VkCommandBuffer InternalSwapchainImages::Impl::copyCommandFor(const vk::Vulkan& vk,
            const uint32_t applicationIndex, const uint32_t lowerIndex) {
        auto& slot = copies.at(static_cast<size_t>(applicationIndex) * lowerImages.size() + lowerIndex);
        if (!slot) {
            const VkImage source = images.at(applicationIndex);
            const VkImage target = lowerImages.at(lowerIndex);
            try {
                slot.emplace(vk);
                slot->begin(vk, VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);
                // The application leaves its image in PRESENT_SRC_KHR. The
                // lower image is discarded because the whole extent is written.
                const std::array preBarriers{
                    barrier(source, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                    barrier(target, VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
                };
                const std::array postBarriers{
                    barrier(source, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
                    barrier(target, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
                };
                slot->blitImage(vk, preBarriers, {source, target}, extent, postBarriers);
                slot->end(vk);
            } catch (...) {
                slot.reset();
                throw;
            }
        }
        return slot->handle();
    }

    InternalSwapchainImages::Impl::~Impl() {
        // Wait for the last copy so no pending GPU work reads the images or
        // signals the semaphores that are destroyed below.
        if (release && lastReleaseValue != 0) {
            const VkSemaphore handle = release->handle();
            const VkSemaphoreWaitInfo waitInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                .semaphoreCount = 1,
                .pSemaphores = &handle,
                .pValues = &lastReleaseValue,
            };
            if (funcs.WaitSemaphoresKHR(device, &waitInfo, destroyWaitTimeoutNs) == VK_TIMEOUT) {
                std::cerr << "framegen-vulkan: internal swapchain images: destroy wait timed out\n";
                static_cast<void>(funcs.DeviceWaitIdle(device));
            }
        }
        copies.clear();
        lowerAcquire.clear();
        copied.clear();
        release.reset();
        for (const VkFence fence : lowerFences) {
            if (fence != VK_NULL_HANDLE)
                funcs.DestroyFence(device, fence, nullptr);
        }
        lowerFences.clear();
        for (const VkImage image : images) {
            if (image != VK_NULL_HANDLE)
                funcs.DestroyImage(device, image, nullptr);
        }
        images.clear();
        for (size_t i = 0; i < memories.size(); ++i) {
            if (memories[i] == VK_NULL_HANDLE)
                continue;
            funcs.FreeMemory(device, memories[i], nullptr);
            accounting->recordFree(vk::DeviceMemoryKind::Internal, memorySizes[i]);
        }
        memories.clear();
        memorySizes.clear();
    }

    std::shared_ptr<InternalSwapchainImages> InternalSwapchainImages::create(
            const vk::Vulkan& vk, const VkSwapchainCreateInfoKHR& application,
            const VkExtent2D extent, const std::span<const VkImage> lowerImages,
            const uint32_t imageCount, std::shared_ptr<DeferredPresentQueue> queue) {
        if (imageCount < InternalImageRing::minCount || imageCount > InternalImageRing::maxCount ||
                lowerImages.size() < 2 || !queue || queue->queue == VK_NULL_HANDLE) {
            return nullptr;
        }
        auto state = std::make_unique<Impl>(vk, imageCount, std::move(queue));
        try {
            if (!state->initialize(vk, application, extent, lowerImages, imageCount))
                return nullptr;
        } catch (const std::exception& error) {
            std::cerr << "framegen-vulkan: internal swapchain images: setup failed: "
                      << error.what() << '\n';
            return nullptr;
        }
        return std::shared_ptr<InternalSwapchainImages>(
            new InternalSwapchainImages(std::move(state)));
    }

    InternalSwapchainImages::InternalSwapchainImages(std::unique_ptr<Impl> state)
        : impl(std::move(state)) {}

    InternalSwapchainImages::~InternalSwapchainImages() = default;

    std::span<const VkImage> InternalSwapchainImages::images() const noexcept {
        return impl->images;
    }

    uint32_t InternalSwapchainImages::lowerImageCount() const noexcept {
        return static_cast<uint32_t>(impl->lowerImages.size());
    }

    bool InternalSwapchainImages::isAcquired(const uint32_t index) const {
        return impl->ring.isAcquired(index);
    }

    VkResult InternalSwapchainImages::acquireApplicationImage(const uint64_t timeout,
            const VkSemaphore semaphore, const VkFence fence,
            const bool deferredSuboptimal, uint32_t* const imageIndex) {
        if (const auto sticky = impl->sticky.load(); sticky != VK_SUCCESS)
            return sticky;
        const auto index = impl->ring.acquire();
        if (!index)
            return timeout == 0 ? VK_NOT_READY : VK_TIMEOUT;

        // The release timeline orders the acquired image after its last copy.
        // The value is read under the queue lock, where every writer updates it.
        {
            const std::lock_guard lock(impl->queue->mutex);
            const uint64_t waitValue = impl->ring.releaseValue(*index);
            const bool waitRelease = waitValue != 0;
            const bool signalSemaphore = semaphore != VK_NULL_HANDLE;
            if (waitRelease || signalSemaphore || fence != VK_NULL_HANDLE) {
                const VkSemaphore releaseHandle = impl->release->handle();
                const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                const uint64_t zero = 0;
                const VkTimelineSemaphoreSubmitInfo timeline{
                    .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                    .waitSemaphoreValueCount = waitRelease ? 1u : 0u,
                    .pWaitSemaphoreValues = &waitValue,
                    .signalSemaphoreValueCount = signalSemaphore ? 1u : 0u,
                    .pSignalSemaphoreValues = &zero,
                };
                const VkSubmitInfo submit{
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .pNext = &timeline,
                    .waitSemaphoreCount = waitRelease ? 1u : 0u,
                    .pWaitSemaphores = &releaseHandle,
                    .pWaitDstStageMask = &stage,
                    .commandBufferCount = 0,
                    .pCommandBuffers = nullptr,
                    .signalSemaphoreCount = signalSemaphore ? 1u : 0u,
                    .pSignalSemaphores = &semaphore,
                };
                const auto submitted = impl->funcs.QueueSubmit(impl->queue->queue, 1, &submit, fence);
                if (submitted != VK_SUCCESS) {
                    impl->sticky.store(submitted);
                    static_cast<void>(impl->ring.release(*index));
                    return submitted;
                }
            }
        }

        *imageIndex = *index;
        const bool flagged = impl->lowerSuboptimal.exchange(false);
        return deferredSuboptimal || flagged ? VK_SUBOPTIMAL_KHR : VK_SUCCESS;
    }

    VkResult InternalSwapchainImages::releaseApplicationImages(
            const std::span<const uint32_t> indices) {
        return impl->ring.releaseAll(indices) ? VK_SUCCESS : VK_ERROR_VALIDATION_FAILED_EXT;
    }

    VkResult InternalSwapchainImages::waitLowerAcquireSlot(const uint64_t timeout) {
        // A zero timeout is a poll. The deferred-present owner calls it with
        // the DeferredPresentQueue mutex held, so it must never sleep there.
        const VkFence fence = impl->lowerFences.at(impl->lowerHead);
        if (timeout == 0)
            return impl->funcs.GetFenceStatus(impl->device, fence);
        return impl->funcs.WaitForFences(impl->device, 1, &fence, VK_TRUE, timeout);
    }

    VkSemaphore InternalSwapchainImages::lowerAcquireSemaphore() const {
        return impl->lowerAcquire.at(impl->lowerHead).handle();
    }

    uint32_t InternalSwapchainImages::commitLowerAcquire() {
        const uint32_t slot = impl->lowerHead;
        const VkFence fence = impl->lowerFences.at(slot);
        static_cast<void>(impl->funcs.ResetFences(impl->device, 1, &fence));
        impl->lowerHead = static_cast<uint32_t>((slot + 1) % impl->lowerFences.size());
        return slot;
    }

    VkResult InternalSwapchainImages::presentCopy(const vk::Vulkan& vk,
            const uint32_t applicationIndex, const uint32_t lowerIndex, const uint32_t slot,
            const std::span<const VkSemaphore> applicationWaits) {
        VkCommandBuffer command{VK_NULL_HANDLE};
        try {
            command = impl->copyCommandFor(vk, applicationIndex, lowerIndex);
        } catch (const ls::vulkan_error& error) {
            // The lower image is already acquired, so the failure is sticky.
            impl->sticky.store(error.error());
            static_cast<void>(presentWithoutCopy(applicationIndex, applicationWaits));
            return error.error();
        } catch (const std::exception&) {
            impl->sticky.store(VK_ERROR_UNKNOWN);
            static_cast<void>(presentWithoutCopy(applicationIndex, applicationWaits));
            return VK_ERROR_UNKNOWN;
        }

        VkResult result = VK_SUCCESS;
        {
            const std::lock_guard lock(impl->queue->mutex);
            const uint64_t value = impl->lastReleaseValue + 1;

            std::vector<VkSemaphore> waits(applicationWaits.begin(), applicationWaits.end());
            waits.push_back(impl->lowerAcquire.at(slot).handle());
            const std::vector<VkPipelineStageFlags> stages(waits.size(), VK_PIPELINE_STAGE_TRANSFER_BIT);
            const std::vector<uint64_t> waitValues(waits.size(), 0);
            const std::array<VkSemaphore, 2> signals{
                impl->copied.at(lowerIndex).handle(), impl->release->handle(),
            };
            const std::array<uint64_t, 2> signalValues{0, value};
            const VkTimelineSemaphoreSubmitInfo timeline{
                .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                .waitSemaphoreValueCount = static_cast<uint32_t>(waitValues.size()),
                .pWaitSemaphoreValues = waitValues.data(),
                .signalSemaphoreValueCount = static_cast<uint32_t>(signalValues.size()),
                .pSignalSemaphoreValues = signalValues.data(),
            };
            const VkSubmitInfo submit{
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext = &timeline,
                .waitSemaphoreCount = static_cast<uint32_t>(waits.size()),
                .pWaitSemaphores = waits.data(),
                .pWaitDstStageMask = stages.data(),
                .commandBufferCount = 1,
                .pCommandBuffers = &command,
                .signalSemaphoreCount = static_cast<uint32_t>(signals.size()),
                .pSignalSemaphores = signals.data(),
            };
            result = impl->funcs.QueueSubmit(impl->queue->queue, 1, &submit,
                impl->lowerFences.at(slot));
            if (result == VK_SUCCESS) {
                impl->lastReleaseValue = value;
                impl->ring.releaseValue(applicationIndex) = value;
            } else {
                impl->sticky.store(result);
            }
        }
        static_cast<void>(impl->ring.present(applicationIndex));
        return result;
    }

    VkResult InternalSwapchainImages::presentWithoutCopy(const uint32_t applicationIndex,
            const std::span<const VkSemaphore> applicationWaits) {
        VkResult result = VK_SUCCESS;
        {
            const std::lock_guard lock(impl->queue->mutex);
            const uint64_t value = impl->lastReleaseValue + 1;
            const std::vector<VkPipelineStageFlags> stages(applicationWaits.size(),
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            const std::vector<uint64_t> waitValues(applicationWaits.size(), 0);
            const VkSemaphore releaseHandle = impl->release->handle();
            const uint64_t signalValue = value;
            const VkTimelineSemaphoreSubmitInfo timeline{
                .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                .waitSemaphoreValueCount = static_cast<uint32_t>(waitValues.size()),
                .pWaitSemaphoreValues = waitValues.data(),
                .signalSemaphoreValueCount = 1,
                .pSignalSemaphoreValues = &signalValue,
            };
            const VkSubmitInfo submit{
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext = &timeline,
                .waitSemaphoreCount = static_cast<uint32_t>(applicationWaits.size()),
                .pWaitSemaphores = applicationWaits.data(),
                .pWaitDstStageMask = stages.data(),
                .commandBufferCount = 0,
                .pCommandBuffers = nullptr,
                .signalSemaphoreCount = 1,
                .pSignalSemaphores = &releaseHandle,
            };
            result = impl->funcs.QueueSubmit(impl->queue->queue, 1, &submit, VK_NULL_HANDLE);
            if (result == VK_SUCCESS) {
                impl->lastReleaseValue = value;
                impl->ring.releaseValue(applicationIndex) = value;
            } else {
                impl->sticky.store(result);
            }
        }
        static_cast<void>(impl->ring.present(applicationIndex));
        return result;
    }

    VkSemaphore InternalSwapchainImages::copiedSemaphore(const uint32_t lowerIndex) const {
        return impl->copied.at(lowerIndex).handle();
    }

    VkResult InternalSwapchainImages::stickyError() const noexcept {
        return impl->sticky.load();
    }

    void InternalSwapchainImages::noteLowerError(const VkResult error) noexcept {
        if (error == VK_ERROR_OUT_OF_DATE_KHR || error == VK_ERROR_SURFACE_LOST_KHR ||
                error == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
            impl->sticky.store(error);
        }
    }

    void InternalSwapchainImages::markLowerSuboptimal() noexcept {
        impl->lowerSuboptimal.store(true);
    }

    InternalImageDeviceSupport queryInternalImageSupport(const vk::Vulkan& vk,
            const VkFormat format, const VkImageUsageFlags usage) {
        InternalImageDeviceSupport support{};
        support.formatBlitCapable = vk.supportsOptimalTilingFormatFeatures(
            format, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT);
        const auto query = vk.fi().GetPhysicalDeviceImageFormatProperties2;
        if (!query)
            return support;
        const VkPhysicalDeviceImageFormatInfo2 formatInfo{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .format = format,
            .type = VK_IMAGE_TYPE_2D,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .flags = 0,
        };
        VkImageFormatProperties2 properties{
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        };
        support.imageFormatSupported =
            query(vk.physdev(), &formatInfo, &properties) == VK_SUCCESS;
        return support;
    }

    bool internalSwapchainEnvironmentEnabled() noexcept {
        static const bool enabled = internalSwapchainEnvironmentAllows(
            std::getenv("FGVK_INTERNAL_SWAPCHAIN"));
        return enabled;
    }
}

/* SPDX-License-Identifier: GPL-3.0-or-later */

// FI1: borrowed images and timelines on the application device, and the queue-family
// name of their ownership barriers. Mock device functions only; no Vulkan device is needed.

#include "fgvk-common/vulkan/external_image.hpp"
#include "fgvk-common/vulkan/image.hpp"
#include "fgvk-common/vulkan/timeline_semaphore.hpp"
#include "fgvk-common/vulkan/vulkan.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
    void require(const bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    template<typename T> T handle(const uintptr_t value) {
        if constexpr (std::is_pointer_v<T>)
            return reinterpret_cast<T>(value);
        else
            return static_cast<T>(value);
    }

    // Counts every object the borrowed wrappers must not create or destroy.
    struct Counts {
        int imagesCreated{}, imagesDestroyed{}, memoryAllocated{}, semaphoresCreated{},
            semaphoresDestroyed{}, viewsCreated{}, viewsDestroyed{}, barriers{};
        std::vector<VkImageMemoryBarrier> recordedBarriers;
        uint64_t lastSignal{};
        uint64_t lastWait{};
    } counts;

    vk::Vulkan makeVulkan(const char* calibrated = nullptr) {
        vk::VulkanInstanceFuncs fi{};
        fi.GetPhysicalDeviceQueueFamilyProperties = [](VkPhysicalDevice, uint32_t* count, VkQueueFamilyProperties* props) {
            *count = 1;
            if (props) props[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        };
        vk::VulkanDeviceFuncs df{};
        df.GetDeviceQueue = [](VkDevice, uint32_t, uint32_t, VkQueue* queue) { *queue = handle<VkQueue>(1); };
        df.CreateCommandPool = [](VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool* pool) {
            *pool = handle<VkCommandPool>(2); return VK_SUCCESS;
        };
        df.DestroyCommandPool = [](VkDevice, VkCommandPool, const VkAllocationCallbacks*) {};
        df.CreatePipelineCache = [](VkDevice, const VkPipelineCacheCreateInfo*, const VkAllocationCallbacks*, VkPipelineCache* cache) {
            *cache = handle<VkPipelineCache>(3); return VK_SUCCESS;
        };
        df.DestroyPipelineCache = [](VkDevice, VkPipelineCache, const VkAllocationCallbacks*) {};
        df.CreateImage = [](VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage* image) {
            ++counts.imagesCreated; *image = handle<VkImage>(4); return VK_SUCCESS;
        };
        df.DestroyImage = [](VkDevice, VkImage, const VkAllocationCallbacks*) { ++counts.imagesDestroyed; };
        df.AllocateMemory = [](VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory* memory) {
            ++counts.memoryAllocated; *memory = handle<VkDeviceMemory>(5); return VK_SUCCESS;
        };
        df.FreeMemory = [](VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) {};
        df.GetImageMemoryRequirements = [](VkDevice, VkImage, VkMemoryRequirements* reqs) { *reqs = {4096, 256, 1}; };
        df.BindImageMemory = [](VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; };
        df.CreateImageView = [](VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView* view) {
            ++counts.viewsCreated; *view = handle<VkImageView>(6); return VK_SUCCESS;
        };
        df.DestroyImageView = [](VkDevice, VkImageView, const VkAllocationCallbacks*) { ++counts.viewsDestroyed; };
        df.CreateSemaphore = [](VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore* semaphore) {
            ++counts.semaphoresCreated; *semaphore = handle<VkSemaphore>(7); return VK_SUCCESS;
        };
        df.DestroySemaphore = [](VkDevice, VkSemaphore, const VkAllocationCallbacks*) { ++counts.semaphoresDestroyed; };
        df.SignalSemaphoreKHR = [](VkDevice, const VkSemaphoreSignalInfo* info) {
            counts.lastSignal = info->value; return VK_SUCCESS;
        };
        df.WaitSemaphoresKHR = [](VkDevice, const VkSemaphoreWaitInfo* info, uint64_t) {
            counts.lastWait = info->pValues[0]; return VK_SUCCESS;
        };
        df.CmdPipelineBarrier = [](VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
                VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*,
                uint32_t count, const VkImageMemoryBarrier* images) {
            ++counts.barriers;
            for (uint32_t i = 0; i < count; ++i) counts.recordedBarriers.push_back(images[i]);
        };
        return {handle<VkInstance>(8), handle<VkDevice>(9), handle<VkPhysicalDevice>(10), fi, df, 0, true,
            true, std::nullopt, std::nullopt, false, calibrated};
    }

    void testOwnershipSelection() {
        const auto attached = vk::sharedImageOwnership(true, 0);
        require(attached.family == VK_QUEUE_FAMILY_IGNORED && attached.foreign == VK_QUEUE_FAMILY_IGNORED,
            "an attached image must use IGNORED for both queue-family fields");
        const auto fd = vk::sharedImageOwnership(false, 3);
        require(fd.family == 3 && fd.foreign == VK_QUEUE_FAMILY_EXTERNAL,
            "an fd-shared image must keep its family and name EXTERNAL as the other side");
    }

    void testBarrierFamilies() {
        const VkImage image = handle<VkImage>(11);
        const auto acquireFd = vk::externalImageAcquire(image, 0, VK_ACCESS_SHADER_READ_BIT);
        require(acquireFd.srcQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL &&
            acquireFd.dstQueueFamilyIndex == 0, "fd acquire must default to EXTERNAL");
        const auto attached = vk::sharedImageOwnership(true, 0);
        const auto acquireAttached = vk::externalImageAcquire(image, attached.family,
            VK_ACCESS_SHADER_READ_BIT, attached.foreign);
        require(acquireAttached.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED &&
            acquireAttached.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED,
            "attached acquire must use IGNORED for both fields");
        require(acquireAttached.oldLayout == VK_IMAGE_LAYOUT_GENERAL &&
            acquireAttached.newLayout == VK_IMAGE_LAYOUT_GENERAL &&
            acquireAttached.dstAccessMask == VK_ACCESS_SHADER_READ_BIT,
            "attached acquire must keep the GENERAL layout and access mask");

        const auto releaseAttached = vk::externalImageRelease(image, attached.family,
            VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, attached.foreign);
        require(releaseAttached.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED &&
            releaseAttached.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED,
            "attached release must use IGNORED for both fields");
        require(releaseAttached.srcAccessMask == VK_ACCESS_SHADER_WRITE_BIT &&
            releaseAttached.oldLayout == VK_IMAGE_LAYOUT_GENERAL &&
            releaseAttached.newLayout == VK_IMAGE_LAYOUT_GENERAL,
            "attached release must keep the access mask and layouts");
        const auto releaseFd = vk::externalImageRelease(image, 0, VK_ACCESS_SHADER_WRITE_BIT);
        require(releaseFd.dstQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL,
            "fd release must default to EXTERNAL");
    }

    void testBorrowedImage(const vk::Vulkan& vk) {
        counts = {};
        const VkImage existing = handle<VkImage>(12);
        {
            const vk::Image borrowed(vk, existing, VkExtent2D{64, 32}, VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            require(borrowed.handle() == existing, "borrowed image lost its handle");
            require(borrowed.imageview() != VK_NULL_HANDLE, "borrowed sampled image needs a view");
            require(borrowed.getExtent().width == 64 && borrowed.getExtent().height == 32,
                "borrowed image lost its extent");
            require(!borrowed.external(), "borrowed image must not be external");
            require(counts.viewsCreated == 1, "borrowed image must create exactly its view");
        }
        require(counts.imagesCreated == 0 && counts.imagesDestroyed == 0,
            "borrowed image must neither create nor destroy the VkImage");
        require(counts.memoryAllocated == 0, "borrowed image must not allocate memory");
        require(counts.viewsDestroyed == 1, "borrowed image must destroy its own view");

        counts = {};
        {
            const vk::Image transferOnly(vk, existing, VkExtent2D{4, 4}, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            require(transferOnly.imageview() == VK_NULL_HANDLE,
                "transfer-only borrowed image must have no view");
        }
        require(counts.viewsCreated == 0 && counts.imagesDestroyed == 0,
            "transfer-only borrowed image must neither create views nor destroy the image");

        bool rejected{false};
        try {
            const vk::Image none(vk, VK_NULL_HANDLE, VkExtent2D{4, 4}, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_SAMPLED_BIT);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "a null borrowed image handle must be rejected");
    }

    void testBorrowedTimeline(const vk::Vulkan& vk) {
        counts = {};
        const VkSemaphore existing = handle<VkSemaphore>(13);
        {
            const vk::TimelineSemaphore borrowed(existing);
            require(borrowed.handle() == existing, "borrowed timeline lost its handle");
            borrowed.signal(vk, 5);
            require(counts.lastSignal == 5, "borrowed timeline must signal through the device");
            require(borrowed.wait(vk, 3), "borrowed timeline must wait through the device");
            require(counts.lastWait == 3, "borrowed timeline wait used the wrong value");
        }
        require(counts.semaphoresCreated == 0 && counts.semaphoresDestroyed == 0,
            "borrowed timeline must neither create nor destroy the VkSemaphore");

        bool rejected{false};
        try {
            const vk::TimelineSemaphore none(VK_NULL_HANDLE);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "a null borrowed timeline handle must be rejected");
    }

    void testCalibratedTimestampsPassThrough() {
        const auto without = makeVulkan();
        require(without.calibratedTimestampsExtension() == nullptr && !without.supportsCalibratedTimestamps(),
            "an external device without a calibration extension must report none");
        const auto with = makeVulkan(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        require(with.supportsCalibratedTimestamps() &&
            std::string(with.calibratedTimestampsExtension()) == VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
            "an external device must report the calibration extension it was given");
    }
}

int main() {
    try {
        testOwnershipSelection();
        testBarrierFamilies();
        const auto vk = makeVulkan();
        testBorrowedImage(vk);
        testBorrowedTimeline(vk);
        testCalibratedTimestampsPassThrough();
    } catch (const std::exception& error) {
        std::cerr << "Attached image test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "attached image tests passed\n";
    return 0;
}

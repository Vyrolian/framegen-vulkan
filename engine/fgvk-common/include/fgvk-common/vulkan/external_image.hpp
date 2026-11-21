/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "vulkan.hpp"

#include <vulkan/vulkan_core.h>

namespace vk {
    // The two queue-family fields of an ownership barrier on a shared image. An fd-shared image crosses Vulkan
    // instance ownership: the local family is this device's queue family and the other side is EXTERNAL. An
    // attached image lives on one device and needs no ownership transfer: both fields are IGNORED.
    struct SharedImageOwnership {
        uint32_t family{VK_QUEUE_FAMILY_EXTERNAL};
        uint32_t foreign{VK_QUEUE_FAMILY_EXTERNAL};
    };

    // Pure: the ownership fields for an image, so the rule can be tested without a device.
    [[nodiscard]] constexpr SharedImageOwnership sharedImageOwnership(const bool attached,
            const uint32_t queueFamily) noexcept {
        if (attached)
            return {VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED};
        return {queueFamily, VK_QUEUE_FAMILY_EXTERNAL};
    }

    // Images shared with the private backend cross Vulkan instance ownership.
    // GENERAL is the layout at that boundary; a semaphore orders the release
    // and acquire, but does not replace either ownership operation.
    [[nodiscard]] inline VkImageMemoryBarrier externalImageAcquire(
            const VkImage image, const uint32_t family,
            const VkAccessFlags access,
            const uint32_t foreignFamily = VK_QUEUE_FAMILY_EXTERNAL) {
        return {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = access,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = foreignFamily,
            .dstQueueFamilyIndex = family,
            .image = image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
    }

    [[nodiscard]] inline VkImageMemoryBarrier externalImageRelease(
            const VkImage image, const uint32_t family,
            const VkAccessFlags access,
            const VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL,
            const uint32_t foreignFamily = VK_QUEUE_FAMILY_EXTERNAL) {
        return {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = access,
            .oldLayout = layout,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = family,
            .dstQueueFamilyIndex = foreignFamily,
            .image = image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
    }

    // Queue-family names for an image's ownership barrier: the device's own family with the fd default
    // (EXTERNAL on the other side), or the attached form, which needs no transfer.
    inline void acquireExternalImage(const Vulkan& vk, const VkCommandBuffer command,
            const VkImage image, const VkAccessFlags access,
            const VkPipelineStageFlags stage, const SharedImageOwnership ownership) {
        const auto barrier = externalImageAcquire(image, ownership.family, access, ownership.foreign);
        vk.df().CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    inline void acquireExternalImage(const Vulkan& vk, const VkCommandBuffer command,
            const VkImage image, const VkAccessFlags access,
            const VkPipelineStageFlags stage) {
        acquireExternalImage(vk, command, image, access, stage,
            sharedImageOwnership(false, vk.queueFamilyIndex()));
    }

    inline void releaseExternalImage(const Vulkan& vk, const VkCommandBuffer command,
            const VkImage image, const VkAccessFlags access,
            const VkPipelineStageFlags stage,
            const VkImageLayout layout, const SharedImageOwnership ownership) {
        const auto barrier = externalImageRelease(image, ownership.family, access, layout, ownership.foreign);
        vk.df().CmdPipelineBarrier(command, stage, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    inline void releaseExternalImage(const Vulkan& vk, const VkCommandBuffer command,
            const VkImage image, const VkAccessFlags access,
            const VkPipelineStageFlags stage,
            const VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL) {
        releaseExternalImage(vk, command, image, access, stage, layout,
            sharedImageOwnership(false, vk.queueFamilyIndex()));
    }
}

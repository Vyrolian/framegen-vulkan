/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "../helpers/pointers.hpp"
#include "vulkan.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include <vulkan/vulkan_core.h>

namespace vk {
    class ImageMemoryPool;

    /// vulkan image
    class Image {
    public:
        /// create an image
        /// @param vk the vulkan instance
        /// @param extent extent of the image in pixels
        /// @param format vulkan format of the image
        /// @param usage usage flags
        /// Import consumes the descriptor on success and failure. Export gives
        /// the caller an owned descriptor only after construction succeeds.
        /// @param importFd optional file descriptor for shared memory
        /// @param exportFd optional pointer to an integer where the file descriptor will be stored
        /// @param internalPool optional context-local memory pool
        /// @param concurrentFamilies queue families that share the image. Empty (the default) makes
        ///     the image exclusive; otherwise at least two distinct families make it concurrent.
        /// @throws ls::vulkan_error on failure
        Image(const vk::Vulkan& vk,
            VkExtent2D extent,
            VkFormat format = VK_FORMAT_R8G8B8A8_UNORM,
            VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            std::optional<int> importFd = std::nullopt,
            std::optional<int*> exportFd = std::nullopt,
            std::shared_ptr<ImageMemoryPool> internalPool = nullptr,
            std::span<const uint32_t> concurrentFamilies = {});

        /// create an internal image backed by a context-local non-aliasing pool
        Image(const vk::Vulkan& vk,
            VkExtent2D extent,
            std::shared_ptr<ImageMemoryPool> internalPool,
            VkFormat format = VK_FORMAT_R8G8B8A8_UNORM,
            VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

        /// borrow an existing image on the same device: only the view is created and owned.
        /// The image and its memory belong to the caller and must outlive this object.
        /// @param image the existing image, created with `usage` and not exported or imported
        /// @throws ls::vulkan_error on failure
        Image(const vk::Vulkan& vk, VkImage image, VkExtent2D extent,
            VkFormat format, VkImageUsageFlags usage);

        /// get the image handle
        /// @return the image handle
        [[nodiscard]] const auto& handle() const { return this->image.get(); }
        /// get the image view handle
        /// @return the image view handle, or null for transfer-only images
        [[nodiscard]] const auto& imageview() const { return this->view.get(); }

        /// get the extent of the image
        /// @return the extent of the image
        [[nodiscard]] VkExtent2D getExtent() const { return this->extent; }
        /// Whether this image imports or exports memory across instances.
        [[nodiscard]] bool external() const { return this->externalMemory; }
    private:
        std::shared_ptr<void> pooledMemory;
        ls::owned_ptr<VkDeviceMemory> memory;
        ls::owned_ptr<VkImage> image;
        ls::owned_ptr<VkImageView> view;

        VkExtent2D extent{};
        bool externalMemory{};
    };
}

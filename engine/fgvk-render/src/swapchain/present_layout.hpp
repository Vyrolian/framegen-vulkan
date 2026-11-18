/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <span>
#include <string_view>
#include <vulkan/vulkan_core.h>

namespace fgvk::layer::present_layout {
    constexpr VkImageLayout backingLayout = VK_IMAGE_LAYOUT_GENERAL;

    constexpr VkImageLayout substitute(VkImageLayout layout) noexcept {
        return layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ? backingLayout : layout;
    }

    // The lower command must be available before exposing an override.
    PFN_vkVoidFunction command(std::string_view name);
    bool registerDevice(VkDevice device, PFN_vkGetDeviceProcAddr lower) noexcept;
    void unregisterDevice(VkDevice device) noexcept;
    bool registerImages(VkDevice device, std::span<const VkImage> images) noexcept;
    void unregisterImages(VkDevice device, std::span<const VkImage> images) noexcept;
}

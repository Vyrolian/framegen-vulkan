/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "color_pipeline.hpp"

using namespace fgvk;

namespace {
    bool isEightBitSdrFormat(const VkFormat format) {
        switch (format) {
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
                return true;
            default:
                return false;
        }
    }

    bool isHighPrecisionSdrFormat(const VkFormat format) {
        switch (format) {
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
            case VK_FORMAT_R16G16B16A16_SFLOAT:
                return true;
            default:
                return false;
        }
    }

}

layer::SwapchainColorPipeline layer::classifySwapchainColor(
        const VkFormat format, const VkColorSpaceKHR colorSpace) {
    if (colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        const bool hdr = colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT ||
            colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT ||
            colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT ||
            colorSpace == VK_COLOR_SPACE_HDR10_HLG_EXT ||
            colorSpace == VK_COLOR_SPACE_DOLBYVISION_EXT;
        return {
            .generationSupported = false,
            .hdr = hdr,
            .name = "hdr-passthrough",
            .reason = "HDR is not supported; frame generation passes through",
        };
    }

    if (isEightBitSdrFormat(format)) {
        return {
            .encoding = backend::FrameEncoding::Sdr8,
            .exchangeFormat = VK_FORMAT_R8G8B8A8_UNORM,
            .generationSupported = true,
            .name = "sdr-8-bit",
        };
    }

    if (isHighPrecisionSdrFormat(format)) {
        return {
            .encoding = backend::FrameEncoding::SdrHighPrecision,
            .exchangeFormat = VK_FORMAT_R16G16B16A16_SFLOAT,
            .generationSupported = true,
            .name = "sdr-high-precision",
        };
    }

    return {
        .generationSupported = false,
        .name = "unsupported-sdr-format",
        .reason = "the swapchain format has no validated framegen-vulkan transport",
    };
}

size_t layer::transportBytesPerPixel(
        const backend::FrameEncoding encoding) {
    switch (encoding) {
        case backend::FrameEncoding::Sdr8:
            return 4;
        case backend::FrameEncoding::SdrHighPrecision:
            return 8;
    }
    return 8;
}

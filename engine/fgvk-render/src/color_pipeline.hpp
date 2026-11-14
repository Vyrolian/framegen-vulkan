/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "fgvk-backend/fgvk.hpp"

#include <cstddef>
#include <string_view>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

    /// Colour handling selected from the complete Vulkan surface-format pair.
    /// `hdr` describes transfer-function semantics, not component bit depth:
    /// a 10-bit UNORM/sRGB pair is still SDR, while the same packed format with
    /// confirmed PQ semantics is HDR10. Keeping those concepts separate avoids
    /// washed-out output from interpreting SDR code values as PQ (or reverse).
    struct SwapchainColorPipeline {
        backend::FrameEncoding encoding{backend::FrameEncoding::Sdr8};
        VkFormat exchangeFormat{VK_FORMAT_R8G8B8A8_UNORM};
        bool generationSupported{true};
        bool hdr{false};
        std::string_view name{"sdr-8-bit"};
        std::string_view reason{};
    };

    /// Only nonlinear sRGB swapchains support generation.
    [[nodiscard]] SwapchainColorPipeline classifySwapchainColor(
        VkFormat format, VkColorSpaceKHR colorSpace);

    [[nodiscard]] size_t transportBytesPerPixel(
        backend::FrameEncoding encoding
    );

}

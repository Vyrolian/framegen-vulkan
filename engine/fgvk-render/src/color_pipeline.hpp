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

    /// Nonlinear sRGB swapchains and the validated HDR pairs (HDR10/PQ on a
    /// 10-bit packed format, linear scRGB on R16G16B16A16_SFLOAT) support
    /// generation. Every other colour space and format passes through, except
    /// PASS_THROUGH with an 8-bit SDR format (Mesa Zink), which is sRGB-equivalent.
    /// `winePassThroughHdr` is set for a native Wayland surface in a Wine process:
    /// winewayland maps those same HDR pairs to PASS_THROUGH and sets the HDR
    /// description on the surface itself, so PASS_THROUGH is then the HDR pair.
    [[nodiscard]] SwapchainColorPipeline classifySwapchainColor(
        VkFormat format, VkColorSpaceKHR colorSpace, bool winePassThroughHdr = false);

    [[nodiscard]] size_t transportBytesPerPixel(
        backend::FrameEncoding encoding
    );

}

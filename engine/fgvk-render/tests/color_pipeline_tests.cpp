/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "color_pipeline.hpp"

#include <cstdlib>
#include <iostream>

using namespace fgvk;
using namespace fgvk::layer;

namespace {
    void expect(bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            std::exit(1);
        }
    }
}

int main() {
    for (const auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
            VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB}) {
        const auto pipeline = classifySwapchainColor(format, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
        expect(pipeline.generationSupported && !pipeline.hdr &&
            pipeline.encoding == backend::FrameEncoding::Sdr8 &&
            pipeline.exchangeFormat == VK_FORMAT_R8G8B8A8_UNORM, "8-bit SDR must support generation");
    }
    for (const auto format : {VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_FORMAT_R16G16B16A16_SFLOAT}) {
        const auto pipeline = classifySwapchainColor(format, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
        expect(pipeline.generationSupported && !pipeline.hdr &&
            pipeline.encoding == backend::FrameEncoding::SdrHighPrecision &&
            pipeline.exchangeFormat == VK_FORMAT_R16G16B16A16_SFLOAT, "10/16-bit SDR must keep its precision");
    }
    const struct ColorCase { VkColorSpaceKHR space; bool hdr; } colors[]{
        {VK_COLOR_SPACE_HDR10_ST2084_EXT, true},
        {VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, true},
        {VK_COLOR_SPACE_HDR10_HLG_EXT, true},
        {VK_COLOR_SPACE_DOLBYVISION_EXT, true},
        {VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT, true},
        {VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT, false},
        {VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT, false},
        {VK_COLOR_SPACE_DCI_P3_NONLINEAR_EXT, false},
        {VK_COLOR_SPACE_BT709_LINEAR_EXT, false},
        {VK_COLOR_SPACE_BT709_NONLINEAR_EXT, false},
        {VK_COLOR_SPACE_BT2020_LINEAR_EXT, false},
        {VK_COLOR_SPACE_ADOBERGB_LINEAR_EXT, false},
        {VK_COLOR_SPACE_ADOBERGB_NONLINEAR_EXT, false},
        {VK_COLOR_SPACE_PASS_THROUGH_EXT, false},
        {VK_COLOR_SPACE_DISPLAY_NATIVE_AMD, false},
        {VK_COLOR_SPACE_MAX_ENUM_KHR, false},
    };
    for (const auto& row : colors) {
        for (const auto format : {VK_FORMAT_R8G8B8A8_UNORM,
                VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_FORMAT_UNDEFINED}) {
            const auto pipeline = classifySwapchainColor(format, row.space);
            expect(!pipeline.generationSupported && pipeline.hdr == row.hdr &&
                pipeline.name == "hdr-passthrough" &&
                pipeline.reason == "HDR is not supported; frame generation passes through",
                "Every non-sRGB colour space must retain real-frame passthrough");
        }
    }
    for (const auto format : {VK_FORMAT_UNDEFINED, VK_FORMAT_R5G6B5_UNORM_PACK16,
            VK_FORMAT_R32G32B32A32_SFLOAT}) {
        expect(!classifySwapchainColor(format, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR).generationSupported,
            "Unsupported SDR formats must pass through");
    }
    expect(transportBytesPerPixel(backend::FrameEncoding::Sdr8) == 4 &&
        transportBytesPerPixel(backend::FrameEncoding::SdrHighPrecision) == 8,
        "SDR transport sizes must preserve their formats");
    std::cout << "color pipeline tests passed\n";
}

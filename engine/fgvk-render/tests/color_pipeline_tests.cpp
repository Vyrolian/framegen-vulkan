/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "color_pipeline.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

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

    // HDR10/PQ: packed 10-bit formats only. The transport is float RGBA16F and
    // the model runs in linear light; the same packed pair with sRGB stays SDR.
    for (const auto format : {VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_FORMAT_A2R10G10B10_UNORM_PACK32}) {
        const auto hdr10 = classifySwapchainColor(format, VK_COLOR_SPACE_HDR10_ST2084_EXT);
        expect(hdr10.generationSupported, "HDR10/PQ on a 10-bit format must support generation");
        expect(hdr10.encoding == backend::FrameEncoding::Hdr10Pq,
            "HDR10/PQ must select the PQ conversion encoding");
        expect(hdr10.exchangeFormat == VK_FORMAT_R16G16B16A16_SFLOAT,
            "HDR10/PQ must use the float RGBA16F exchange format");
        expect(hdr10.hdr && hdr10.name == "hdr10-pq",
            "HDR10/PQ must be classified as HDR with its diagnostic name");
        expect(transportBytesPerPixel(hdr10.encoding) == 8,
            "HDR10/PQ transport must be 8 bytes per pixel");
    }
    for (const auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
            VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED}) {
        const auto pipeline = classifySwapchainColor(format, VK_COLOR_SPACE_HDR10_ST2084_EXT);
        expect(!pipeline.generationSupported && pipeline.hdr &&
            pipeline.name == "unsupported-hdr10-format" &&
            pipeline.reason == "HDR10/PQ requires an A2R10G10B10 or A2B10G10R10 swapchain",
            "HDR10/PQ on another format must pass through");
    }

    // Linear scRGB: float swapchains are already linear and need no PQ stages.
    const auto scrgb = classifySwapchainColor(VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT);
    expect(scrgb.generationSupported && scrgb.hdr &&
        scrgb.encoding == backend::FrameEncoding::ScRgbLinear &&
        scrgb.exchangeFormat == VK_FORMAT_R16G16B16A16_SFLOAT &&
        scrgb.name == "scrgb-linear",
        "linear scRGB on R16G16B16A16_SFLOAT must use the scRGB model path");
    expect(transportBytesPerPixel(scrgb.encoding) == 8,
        "linear scRGB transport must be 8 bytes per pixel");
    for (const auto format : {VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED}) {
        const auto pipeline = classifySwapchainColor(format, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT);
        expect(!pipeline.generationSupported && pipeline.hdr &&
            pipeline.name == "unsupported-scrgb-format" &&
            pipeline.reason == "linear scRGB requires an R16G16B16A16_SFLOAT swapchain",
            "linear scRGB on another format must pass through");
    }

    // Wine's winewayland maps HDR10/PQ and linear scRGB to PASS_THROUGH on a
    // native Wayland surface and sets the HDR description itself. With the flag
    // the same validated pairs run frame generation; without it PASS_THROUGH
    // stays real-frame passthrough.
    const auto winePassThroughCases = {
        std::pair{VK_FORMAT_A2B10G10R10_UNORM_PACK32, std::string_view{"wine-hdr10-pq"}},
        std::pair{VK_FORMAT_A2R10G10B10_UNORM_PACK32, std::string_view{"wine-hdr10-pq"}},
        std::pair{VK_FORMAT_R16G16B16A16_SFLOAT, std::string_view{"wine-scrgb-linear"}},
    };
    for (const auto& [format, name] : winePassThroughCases) {
        const auto pipeline = classifySwapchainColor(format, VK_COLOR_SPACE_PASS_THROUGH_EXT, true);
        expect(pipeline.generationSupported && pipeline.hdr && pipeline.name == name,
            "Wine PASS_THROUGH HDR pairs must support generation with their diagnostic name");
        expect(pipeline.exchangeFormat == VK_FORMAT_R16G16B16A16_SFLOAT &&
            transportBytesPerPixel(pipeline.encoding) == 8,
            "Wine PASS_THROUGH HDR must use the float RGBA16F exchange path");
    }
    expect(classifySwapchainColor(VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_COLOR_SPACE_PASS_THROUGH_EXT, true).encoding == backend::FrameEncoding::Hdr10Pq &&
        classifySwapchainColor(VK_FORMAT_A2R10G10B10_UNORM_PACK32,
            VK_COLOR_SPACE_PASS_THROUGH_EXT, true).encoding == backend::FrameEncoding::Hdr10Pq &&
        classifySwapchainColor(VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_COLOR_SPACE_PASS_THROUGH_EXT, true).encoding == backend::FrameEncoding::ScRgbLinear,
        "Wine PASS_THROUGH must select the HDR10/PQ and scRGB encodings");
    {
        const auto unsupported = classifySwapchainColor(VK_FORMAT_B8G8R8A8_UNORM,
            VK_COLOR_SPACE_PASS_THROUGH_EXT, true);
        expect(!unsupported.generationSupported && unsupported.hdr &&
            unsupported.name == "unsupported-wine-hdr-format" && !unsupported.reason.empty(),
            "Wine PASS_THROUGH on another format must pass through as unsupported HDR");
    }
    // Mesa Zink asks for PASS_THROUGH with an 8-bit SDR format (OpenGL has no colour
    // space). Without the Wine flag that swapchain is sRGB 8-bit, named passthrough-sdr8.
    for (const auto format : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB,
            VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB}) {
        const auto pass = classifySwapchainColor(format, VK_COLOR_SPACE_PASS_THROUGH_EXT, false);
        const auto srgb = classifySwapchainColor(format, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
        expect(pass.generationSupported && !pass.hdr &&
            pass.encoding == backend::FrameEncoding::Sdr8 &&
            pass.exchangeFormat == VK_FORMAT_R8G8B8A8_UNORM &&
            pass.name == "passthrough-sdr8",
            "PASS_THROUGH with an 8-bit SDR format must support generation as passthrough-sdr8");
        expect(pass.encoding == srgb.encoding && pass.exchangeFormat == srgb.exchangeFormat &&
            pass.generationSupported == srgb.generationSupported && pass.hdr == srgb.hdr,
            "PASS_THROUGH with an 8-bit SDR format must classify exactly like SRGB_NONLINEAR");
    }
    for (const auto format : {VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_FORMAT_UNDEFINED}) {
        const auto pipeline = classifySwapchainColor(format, VK_COLOR_SPACE_PASS_THROUGH_EXT, false);
        expect(!pipeline.generationSupported && !pipeline.hdr &&
            pipeline.name == "hdr-passthrough" &&
            pipeline.reason == "HDR is not supported; frame generation passes through",
            "PASS_THROUGH without the Wine flag and not an 8-bit SDR format must stay real-frame passthrough");
    }
    // The flag only ever selects PASS_THROUGH; other colour spaces are unchanged.
    expect(classifySwapchainColor(VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, true).name == "sdr-high-precision" &&
        classifySwapchainColor(VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, true).name == "scrgb-linear" &&
        classifySwapchainColor(VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_COLOR_SPACE_HDR10_ST2084_EXT, true).name == "hdr10-pq",
        "the Wine flag must not change non-PASS_THROUGH colour spaces");

    // Colour spaces with no validated model stay real-frame passthrough.
    const struct ColorCase { VkColorSpaceKHR space; bool hdr; } colors[]{
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
        {VK_COLOR_SPACE_DISPLAY_NATIVE_AMD, false},
        {VK_COLOR_SPACE_MAX_ENUM_KHR, false},
    };
    // PASS_THROUGH has its own cases above, because its 8-bit SDR formats are not passthrough.
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

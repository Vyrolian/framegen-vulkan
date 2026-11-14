/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "fgvk-common/configuration/config.hpp"

#include <cstdlib>
#include <string_view>

namespace fgvk::layer {

    inline constexpr std::string_view layerRoleName = "frame-generation";
    inline constexpr std::string_view layerIdentity = "VK_LAYER_FGVK_render";

    /// The single framegen-vulkan layer owns frame generation and spatial
    /// scaling resources in one library. The process profile passes through
    /// unchanged for capability queries and process-static policy.
    [[nodiscard]] inline ls::GameConf profileForLayer(
            ls::GameConf profile) {
        return profile;
    }

    /// The single layer owns every resource class, so the context profile
    /// passes through unchanged.
    [[nodiscard]] inline ls::GameConf profileForLayerContext(
            ls::GameConf profile) {
        return profile;
    }

    [[nodiscard]] inline bool shouldRejectUnmatchedFixedSpatialCreate(
            const bool fixedVirtualSourceRequest,
            const bool scalingExtentsSelected) {
        return fixedVirtualSourceRequest && !scalingExtentsSelected;
    }

}

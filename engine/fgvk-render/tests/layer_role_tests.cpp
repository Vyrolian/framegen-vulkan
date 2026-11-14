/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "layer_role.hpp"
#include "profile_update.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

using namespace fgvk::layer;

namespace {
    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }

    ls::GameConf exercisedProfile() {
        ls::GameConf profile{};
        profile.frame_generation_enabled = true;
        profile.adaptive = true;
        profile.multiplier = 4;
        profile.target_fps = 120;
        profile.adaptive_target_refresh_rate = true;
        profile.scaling_enabled = true;
        profile.scaling_factor = 2.0F;
        profile.ultra_performance = true;
        return profile;
    }
}

int main() {
    expect(layerRoleName == "frame-generation",
        "the single layer must identify the frame-generation role");
    expect(layerIdentity == "VK_LAYER_FGVK_render",
        "the single layer must keep its Vulkan layer identity");

    const auto exercised = exercisedProfile();
    const auto direct = profileForLayer(exercised);
    expect(direct.frame_generation_enabled && direct.adaptive &&
            direct.multiplier == 4 && direct.target_fps == 120 &&
            direct.adaptive_target_refresh_rate && direct.scaling_enabled &&
            direct.ultra_performance,
        "the process profile must pass through the single layer unchanged");
    const auto context = profileForLayerContext(exercised);
    expect(context.frame_generation_enabled && context.scaling_enabled &&
            context.scaling_factor == 2.0F && context.ultra_performance,
        "the context profile must pass through the single layer unchanged");

    expect(shouldRejectUnmatchedFixedSpatialCreate(true, false),
        "the single layer must reject an unmatched fixed spatial request");
    expect(!shouldRejectUnmatchedFixedSpatialCreate(true, true),
        "a selected scaling extent must satisfy the fixed contract");
    expect(!shouldRejectUnmatchedFixedSpatialCreate(false, false),
        "a non-fixed request must not be rejected as unmatched");
}

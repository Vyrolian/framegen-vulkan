/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "layer_role.hpp"

#include <cstdlib>
#include <initializer_list>
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
}

int main() {
    expect(layerRoleName == "frame-generation",
        "the single layer must identify the frame-generation role");
    expect(layerIdentity == "VK_LAYER_FGVK_render",
        "the single layer must keep its Vulkan layer identity");

    expect(!gamescopeSessionBlocked(false, nullptr) &&
            !gamescopeSessionBlocked(false, "1") &&
            !gamescopeSessionBlocked(false, "0"),
        "a desktop session must never be blocked by the Gamescope gate");
    expect(!gamescopeSessionBlocked(true, nullptr),
        "a Gamescope session without the opt-out must run frame generation");
    expect(!gamescopeSessionBlocked(true, "1"),
        "FGVK_GAMESCOPE=1 must let a Gamescope session through");
    expect(gamescopeSessionBlocked(true, "0"),
        "FGVK_GAMESCOPE=0 must turn the layer off in a Gamescope session");
    for (const char* value : {"", "false", "no", "00", "0 ", " 0"}) {
        expect(!gamescopeSessionBlocked(true, value),
            "only the exact value 0 may turn the layer off in a Gamescope session");
    }
}

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "layer_role.hpp"

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
}

int main() {
    expect(layerRoleName == "frame-generation",
        "the single layer must identify the frame-generation role");
    expect(layerIdentity == "VK_LAYER_FGVK_render",
        "the single layer must keep its Vulkan layer identity");
}

/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <string_view>

namespace fgvk::layer {

    inline constexpr std::string_view layerRoleName = "frame-generation";
    inline constexpr std::string_view layerIdentity = "VK_LAYER_FGVK_render";

    /// Gamescope sessions run frame generation by default. They are experimental,
    /// so the layer stays inactive in one only when FGVK_GAMESCOPE is exactly "0".
    [[nodiscard]] inline bool gamescopeSessionBlocked(const bool sessionDetected,
            const char* fgvkGamescope) noexcept {
        return sessionDetected && fgvkGamescope &&
            std::string_view(fgvkGamescope) == "0";
    }

}

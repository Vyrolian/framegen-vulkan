/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <string_view>

namespace vk {

    /// Whether FGVK_BACKEND_OVERLAP selects the overlapped backend dispatch grouping (GP2).
    /// Only the exact text "1" enables it; unset or any other value keeps the serial recording.
    [[nodiscard]] inline bool parseBackendOverlap(const char* text) {
        return text && std::string_view(text) == "1";
    }
}

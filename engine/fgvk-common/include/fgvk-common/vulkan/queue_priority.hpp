/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <cmath>
#include <cstdlib>

namespace vk {

    /// The backend device queue priority from FGVK_BACKEND_QUEUE_PRIORITY. `set` is false when the variable is
    /// unset. `accepted` is true when the text parses fully to a finite value in [0, 1]; otherwise the priority is 1.
    struct BackendQueuePriority {
        float priority{1.0F};
        bool set{};
        bool accepted{};
    };

    /// Parse the variable's text (nullptr when unset) with std::strtof, which must consume every character.
    [[nodiscard]] inline BackendQueuePriority parseBackendQueuePriority(const char* text) {
        if (!text)
            return {};
        char* end = nullptr;
        const float value = std::strtof(text, &end);
        const bool accepted = end != text && *end == '\0' && std::isfinite(value) &&
            value >= 0.0F && value <= 1.0F;
        return {.priority = accepted ? value : 1.0F, .set = true, .accepted = accepted};
    }
}

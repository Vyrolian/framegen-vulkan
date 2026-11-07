/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "mako-backend/mako.hpp"
#include <cstddef>
#include <optional>
#include <utility>

namespace mako::backend {
    /// One bounded, consume-once diagnostic slot. The caller must establish
    /// GPU completion using its normal fence before invoking collection.
    class CompletedFrameProfile {
    public:
        explicit CompletedFrameProfile(bool enabled = false) : enabled(enabled) {}

        template<typename Read>
        void collectAfterCompletion(size_t frame, bool generated, Read&& read) {
            if (!enabled || !generated || frame == 0 || frame == collectedFrame) return;
            collectedFrame = frame;
            try {
                completed = read();
            } catch (...) {
                enabled = false;
                throw;
            }
        }

        [[nodiscard]] std::optional<FrameProfile> take() {
            return std::exchange(completed, std::nullopt);
        }
    private:
        bool enabled{};
        size_t collectedFrame{};
        std::optional<FrameProfile> completed;
    };
}

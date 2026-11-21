/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace fgvk::layer {

// RL2: the out-of-band queue type fgvk marks on its private present queue. FGVK_REFLEX_OUT_OF_BAND=present or
// render, read once; any other value, or unset, marks nothing. entrypoint.cpp applies it only to a device whose
// application enabled VK_NV_low_latency2, and never to the application's own queues.
enum class ReflexOutOfBandType : uint32_t { None = 0, Present = 1, Render = 2 };

[[nodiscard]] inline ReflexOutOfBandType reflexOutOfBandRequest() noexcept {
    static const ReflexOutOfBandType requested = [] {
        const char* text = std::getenv("FGVK_REFLEX_OUT_OF_BAND");
        if (!text) return ReflexOutOfBandType::None;
        if (std::strcmp(text, "present") == 0) return ReflexOutOfBandType::Present;
        if (std::strcmp(text, "render") == 0) return ReflexOutOfBandType::Render;
        return ReflexOutOfBandType::None;
    }();
    return requested;
}

// RL2, diagnostics only: the type applied to the most recent private present queue (0 none, 1 present, 2 render).
// The per-second pacing summary prints it as reflex_oob=.
inline std::atomic<uint32_t> reflexOutOfBandApplied{0};

}

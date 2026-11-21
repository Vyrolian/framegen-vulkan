/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace fgvk::layer {

// RL2: the out-of-band queue type fgvk marks on its private present queue. RF1 made it the default: unset, or any
// value other than the three below, marks the queue as the Reflex out-of-band present queue. `off` marks nothing,
// `render` marks the render queue, and `present` is the same as unset. entrypoint.cpp applies it only to a device whose
// application enabled VK_NV_low_latency2, and never to the application's own queues.
enum class ReflexOutOfBandType : uint32_t { None = 0, Present = 1, Render = 2 };

// RF1: the type for an FGVK_REFLEX_OUT_OF_BAND value (nullptr when unset). Pure, so the rule can be tested.
[[nodiscard]] inline ReflexOutOfBandType parseReflexOutOfBandRequest(const char* text) noexcept {
    if (!text || std::strcmp(text, "present") == 0) return ReflexOutOfBandType::Present;
    if (std::strcmp(text, "render") == 0) return ReflexOutOfBandType::Render;
    if (std::strcmp(text, "off") == 0) return ReflexOutOfBandType::None;
    return ReflexOutOfBandType::Present;
}

[[nodiscard]] inline ReflexOutOfBandType reflexOutOfBandRequest() noexcept {
    static const ReflexOutOfBandType requested = parseReflexOutOfBandRequest(std::getenv("FGVK_REFLEX_OUT_OF_BAND"));
    return requested;
}

// RL2, diagnostics only: the type applied to the most recent private present queue (0 none, 1 present, 2 render).
// The per-second pacing summary prints it as reflex_oob=.
inline std::atomic<uint32_t> reflexOutOfBandApplied{0};

}

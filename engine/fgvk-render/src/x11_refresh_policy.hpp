/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace fgvk::layer {

// Pure rules behind the X11 refresh query (x11_refresh.cpp) and its Wayland output match
// (wayland_output_refresh.cpp). Nothing here calls X, RandR or Wayland.

// RandR mode flags (xcb/randr.h, xcb_randr_mode_flag_t).
inline constexpr uint32_t randrModeInterlace = 1u << 4;
inline constexpr uint32_t randrModeDoubleScan = 1u << 5;

// Refresh of a RandR mode: dot clock (Hz) over total pixels per frame. Interlaced modes scan
// two fields per frame and double-scanned modes repeat each line. Zero when unknown.
[[nodiscard]] inline double randrModeRefreshHz(const uint32_t dotClockHz, const uint32_t htotal,
        const uint32_t vtotal, const uint32_t modeFlags) noexcept {
    if (dotClockHz == 0 || htotal == 0 || vtotal == 0)
        return 0.0;
    double hz = static_cast<double>(dotClockHz) /
        (static_cast<double>(htotal) * static_cast<double>(vtotal));
    if (modeFlags & randrModeInterlace)
        hz *= 2.0;
    if (modeFlags & randrModeDoubleScan)
        hz /= 2.0;
    return hz;
}

// Root-relative rectangle, in the same coordinate space RandR uses for CRTC positions.
struct X11Rect {
    int32_t x{0};
    int32_t y{0};
    uint32_t width{0};
    uint32_t height{0};
};

[[nodiscard]] inline uint64_t overlapArea(const X11Rect a, const X11Rect b) noexcept {
    const int64_t left = std::max<int64_t>(a.x, b.x);
    const int64_t top = std::max<int64_t>(a.y, b.y);
    const int64_t right = std::min<int64_t>(int64_t{a.x} + a.width, int64_t{b.x} + b.width);
    const int64_t bottom = std::min<int64_t>(int64_t{a.y} + a.height, int64_t{b.y} + b.height);
    if (right <= left || bottom <= top)
        return 0;
    return static_cast<uint64_t>(right - left) * static_cast<uint64_t>(bottom - top);
}

// One CRTC as RandR reports it. `active` means the CRTC drives at least one output with a mode.
struct RandrCrtc {
    uint32_t id{0};
    X11Rect rect{};
    bool active{false};
};

// Index of the active CRTC with the largest positive overlap with the window; the first one wins a tie.
[[nodiscard]] inline std::optional<size_t> largestOverlapCrtc(
        std::span<const RandrCrtc> crtcs, const X11Rect window) noexcept {
    std::optional<size_t> best;
    uint64_t bestArea = 0;
    for (size_t index = 0; index < crtcs.size(); ++index) {
        const auto& crtc = crtcs[index];
        if (!crtc.active)
            continue;
        const uint64_t area = overlapArea(crtc.rect, window);
        if (area > bestArea) {
            best = index;
            bestArea = area;
        }
    }
    return best;
}

// Index of the active CRTC with this id; id 0 means no CRTC.
[[nodiscard]] inline std::optional<size_t> activeCrtcById(
        std::span<const RandrCrtc> crtcs, const uint32_t id) noexcept {
    if (id == 0)
        return std::nullopt;
    for (size_t index = 0; index < crtcs.size(); ++index) {
        if (crtcs[index].id == id && crtcs[index].active)
            return index;
    }
    return std::nullopt;
}

// The CRTC for a window: the largest active overlap. Only when no active CRTC overlaps does it
// ask for the primary output's CRTC (id 0 when none), so the caller pays for that lookup only then.
template <class PrimaryCrtc>
[[nodiscard]] std::optional<size_t> chooseCrtc(std::span<const RandrCrtc> crtcs,
        const X11Rect window, PrimaryCrtc&& primary) {
    if (const auto overlapping = largestOverlapCrtc(crtcs, window))
        return overlapping;
    return activeCrtcById(crtcs, primary());
}

// An output name with the current-mode rate of its wl_output, in mHz (0 when unknown).
struct NamedRefresh {
    std::string_view name;
    uint32_t milliHz{0};
};

// The mHz rate of the output whose name equals `name` exactly. Nullopt when no output of that name
// has a known current-mode rate.
[[nodiscard]] inline std::optional<uint32_t> outputRefreshMilliHz(
        const std::string_view name, std::span<const NamedRefresh> outputs) noexcept {
    if (name.empty())
        return std::nullopt;
    for (const auto& output : outputs) {
        if (output.name == name && output.milliHz > 0)
            return output.milliHz;
    }
    return std::nullopt;
}

[[nodiscard]] inline double refreshHzFromMilliHz(const uint32_t milliHz) noexcept {
    return static_cast<double>(milliHz) / 1000.0;
}

} // namespace fgvk::layer

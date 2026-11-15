/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

namespace fgvk::layer {

    /// WSI family that created the application's surface. Diagnostics report it
    /// in the pacing profile.
    enum class SurfaceOrigin {
        Unknown,
        Wayland,
        Xcb,
        Xlib,
    };

    [[nodiscard]] constexpr const char* surfaceOriginName(
            const SurfaceOrigin origin) noexcept {
        switch (origin) {
            case SurfaceOrigin::Unknown:
                return "unknown";
            case SurfaceOrigin::Wayland:
                return "wayland";
            case SurfaceOrigin::Xcb:
                return "xcb";
            case SurfaceOrigin::Xlib:
                return "xlib";
        }
        return "unknown";
    }

}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace fgvk::layer {

struct WaylandOutputRefresh {
    uint32_t milliHz{0}; // exact current-mode rate, 0 when unknown
    std::string reason;  // set when milliHz is 0
};

// Reads the current-mode rate of the wl_output called `outputName` on the compositor that
// WAYLAND_DISPLAY names. It opens and closes its own connection and never touches the
// application's display. The round trips block, so call it only from the X11 query thread
// (x11_refresh.cpp), never from swapchain creation or the present path.
[[nodiscard]] WaylandOutputRefresh queryWaylandOutputRefresh(std::string_view outputName);

} // namespace fgvk::layer

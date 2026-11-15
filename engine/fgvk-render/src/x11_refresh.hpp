/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace fgvk::layer {

// Refresh of the monitor under an X11 window (an Xlib or XCB surface), read once when its swapchain
// is created. RandR gives the active CRTC that overlaps the window most. On Xwayland the exact rate
// of the wl_output with the same name replaces the synthesized RandR rate.
struct X11WindowRefresh {
    double hz{0.0};             // 0 when unknown; reason says why
    uint32_t crtc{0};           // RandR CRTC chosen for the window
    std::string output;         // RandR output name of that CRTC; empty when unknown
    const char* source{"none"}; // "randr" or "wayland-output"
    std::string reason;         // set when hz is 0
    std::string waylandReason;  // set when the Wayland lookup gave no rate and RandR was kept
};

// The result cell shared by a swapchain and its detached query thread. The thread writes it once;
// the present path reads it. Neither side ever waits for the other.
struct X11RefreshCell {
    std::atomic<double> hz{0.0};
    // A string literal (`randr`, `wayland-output` or `none`), so the pointer stays valid.
    std::atomic<const char*> source{"none"};
};

// Starts the query on a detached thread that owns its own reference to the cell, and logs this
// swapchain's single `x11 refresh` record from that thread. The thread is pinned with the layer, as
// the other detached layer threads are. A thread that cannot start leaves the cell at zero.
[[nodiscard]] std::shared_ptr<X11RefreshCell> startX11WindowRefreshQuery(uint64_t window);

// Opens and closes its own X connection on DISPLAY (never the application's) and, when the output
// name is known and WAYLAND_DISPLAY is set, its own Wayland connection. Both round trips block, so
// call this only from the thread that startX11WindowRefreshQuery starts, never from creation or the
// present path.
[[nodiscard]] X11WindowRefresh queryX11WindowRefresh(uint64_t window);

} // namespace fgvk::layer

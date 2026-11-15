/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "x11_refresh.hpp"
#include "library_pin.hpp"
#include "wayland_output_refresh.hpp"
#include "x11_refresh_policy.hpp"
#include "swapchain/uncapped_auto_cap.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace fgvk::layer {
namespace {
    // Layout mirrors of the libxcb and RandR replies read below. The layer builds without X headers.
    // libxcb allocates every reply and the caller frees it with free(). Field order and sizes follow
    // xcb/xproto.h and xcb/randr.h, and a layout check against those headers is kept outside the build.
    struct XcbConnection;
    struct XcbExtension; // the key of one extension, xcb_randr_id

    struct XcbCookie {
        unsigned int sequence;
    };

    struct XcbGenericError {
        uint8_t response_type;
        uint8_t error_code;
        uint16_t sequence;
        uint32_t resource_id;
        uint16_t minor_code;
        uint8_t major_code;
        uint8_t pad0;
        uint32_t pad[5];
        uint32_t full_sequence;
    };

    struct XcbQueryExtensionReply {
        uint8_t response_type;
        uint8_t pad0;
        uint16_t sequence;
        uint32_t length;
        uint8_t present;
        uint8_t major_opcode;
        uint8_t first_event;
        uint8_t first_error;
    };

    struct XcbGeometryReply {
        uint8_t response_type;
        uint8_t depth;
        uint16_t sequence;
        uint32_t length;
        uint32_t root;
        int16_t x;
        int16_t y;
        uint16_t width;
        uint16_t height;
        uint16_t border_width;
        uint8_t pad0[2];
    };

    struct XcbTranslateReply {
        uint8_t response_type;
        uint8_t same_screen;
        uint16_t sequence;
        uint32_t length;
        uint32_t child;
        int16_t dst_x;
        int16_t dst_y;
    };

    struct RandrScreenResourcesReply {
        uint8_t response_type;
        uint8_t pad0;
        uint16_t sequence;
        uint32_t length;
        uint32_t timestamp;
        uint32_t config_timestamp;
        uint16_t num_crtcs;
        uint16_t num_outputs;
        uint16_t num_modes;
        uint16_t names_len;
        uint8_t pad1[8];
    };

    // The array elements after the screen resources reply header: xcb_randr_mode_info_t.
    struct RandrModeInfo {
        uint32_t id;
        uint16_t width;
        uint16_t height;
        uint32_t dot_clock;
        uint16_t hsync_start;
        uint16_t hsync_end;
        uint16_t htotal;
        uint16_t hskew;
        uint16_t vsync_start;
        uint16_t vsync_end;
        uint16_t vtotal;
        uint16_t name_len;
        uint32_t mode_flags;
    };

    struct RandrCrtcInfoReply {
        uint8_t response_type;
        uint8_t status;
        uint16_t sequence;
        uint32_t length;
        uint32_t timestamp;
        int16_t x;
        int16_t y;
        uint16_t width;
        uint16_t height;
        uint32_t mode;
        uint16_t rotation;
        uint16_t rotations;
        uint16_t num_outputs;
        uint16_t num_possible_outputs;
    };

    struct RandrOutputPrimaryReply {
        uint8_t response_type;
        uint8_t pad0;
        uint16_t sequence;
        uint32_t length;
        uint32_t output;
    };

    struct RandrOutputInfoReply {
        uint8_t response_type;
        uint8_t status;
        uint16_t sequence;
        uint32_t length;
        uint32_t timestamp;
        uint32_t crtc;
        uint32_t mm_width;
        uint32_t mm_height;
        uint8_t connection;
        uint8_t subpixel_order;
        uint16_t num_crtcs;
        uint16_t num_modes;
        uint16_t num_preferred;
        uint16_t num_clones;
        uint16_t name_len;
    };

    // libxcb and libxcb-randr entry points, resolved with dlsym. Replies come back typed as void*
    // and are cast by replyOrReason. The libraries stay mapped for the life of the process.
    struct XcbApi {
        XcbConnection* (*connect)(const char*, int*) {};
        void (*disconnect)(XcbConnection*) {};
        int (*hasError)(XcbConnection*) {};
        const XcbQueryExtensionReply* (*extensionData)(XcbConnection*, XcbExtension*) {};
        XcbExtension* randrId{};

        XcbCookie (*getGeometry)(XcbConnection*, uint32_t) {};
        void* (*getGeometryReply)(XcbConnection*, XcbCookie, XcbGenericError**) {};
        XcbCookie (*translateCoordinates)(XcbConnection*, uint32_t, uint32_t, int16_t, int16_t) {};
        void* (*translateCoordinatesReply)(XcbConnection*, XcbCookie, XcbGenericError**) {};

        XcbCookie (*randrQueryVersion)(XcbConnection*, uint32_t, uint32_t) {};
        void* (*randrQueryVersionReply)(XcbConnection*, XcbCookie, XcbGenericError**) {};
        XcbCookie (*randrGetScreenResourcesCurrent)(XcbConnection*, uint32_t) {};
        void* (*randrGetScreenResourcesCurrentReply)(XcbConnection*, XcbCookie, XcbGenericError**) {};
        const uint32_t* (*resourcesCrtcs)(const RandrScreenResourcesReply*) {};
        int (*resourcesCrtcsLength)(const RandrScreenResourcesReply*) {};
        const RandrModeInfo* (*resourcesModes)(const RandrScreenResourcesReply*) {};
        int (*resourcesModesLength)(const RandrScreenResourcesReply*) {};

        XcbCookie (*randrGetCrtcInfo)(XcbConnection*, uint32_t, uint32_t) {};
        void* (*randrGetCrtcInfoReply)(XcbConnection*, XcbCookie, XcbGenericError**) {};
        const uint32_t* (*crtcInfoOutputs)(const RandrCrtcInfoReply*) {};
        int (*crtcInfoOutputsLength)(const RandrCrtcInfoReply*) {};

        XcbCookie (*randrGetOutputPrimary)(XcbConnection*, uint32_t) {};
        void* (*randrGetOutputPrimaryReply)(XcbConnection*, XcbCookie, XcbGenericError**) {};
        XcbCookie (*randrGetOutputInfo)(XcbConnection*, uint32_t, uint32_t) {};
        void* (*randrGetOutputInfoReply)(XcbConnection*, XcbCookie, XcbGenericError**) {};
        const uint8_t* (*outputInfoName)(const RandrOutputInfoReply*) {};
        int (*outputInfoNameLength)(const RandrOutputInfoReply*) {};
    };

    struct XcbLoad {
        XcbApi api{};
        bool ok{false};
        std::string reason;
    };

    template <class T> bool symbol(void* library, T& target, const char* name) {
        target = reinterpret_cast<T>(dlsym(library, name));
        return target != nullptr;
    }

    const XcbLoad& loadXcbApi() {
        static const XcbLoad load = [] {
            XcbLoad result;
            void* xcb = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
            if (!xcb) {
                result.reason = "no-libxcb";
                return result;
            }
            void* randr = dlopen("libxcb-randr.so.0", RTLD_NOW | RTLD_LOCAL);
            if (!randr) {
                result.reason = "no-libxcb-randr";
                return result;
            }
            auto& api = result.api;
            const bool resolved =
                symbol(xcb, api.connect, "xcb_connect") &&
                symbol(xcb, api.disconnect, "xcb_disconnect") &&
                symbol(xcb, api.hasError, "xcb_connection_has_error") &&
                symbol(xcb, api.extensionData, "xcb_get_extension_data") &&
                symbol(randr, api.randrId, "xcb_randr_id") &&
                symbol(xcb, api.getGeometry, "xcb_get_geometry") &&
                symbol(xcb, api.getGeometryReply, "xcb_get_geometry_reply") &&
                symbol(xcb, api.translateCoordinates, "xcb_translate_coordinates") &&
                symbol(xcb, api.translateCoordinatesReply, "xcb_translate_coordinates_reply") &&
                symbol(randr, api.randrQueryVersion, "xcb_randr_query_version") &&
                symbol(randr, api.randrQueryVersionReply, "xcb_randr_query_version_reply") &&
                symbol(randr, api.randrGetScreenResourcesCurrent, "xcb_randr_get_screen_resources_current") &&
                symbol(randr, api.randrGetScreenResourcesCurrentReply, "xcb_randr_get_screen_resources_current_reply") &&
                symbol(randr, api.resourcesCrtcs, "xcb_randr_get_screen_resources_current_crtcs") &&
                symbol(randr, api.resourcesCrtcsLength, "xcb_randr_get_screen_resources_current_crtcs_length") &&
                symbol(randr, api.resourcesModes, "xcb_randr_get_screen_resources_current_modes") &&
                symbol(randr, api.resourcesModesLength, "xcb_randr_get_screen_resources_current_modes_length") &&
                symbol(randr, api.randrGetCrtcInfo, "xcb_randr_get_crtc_info") &&
                symbol(randr, api.randrGetCrtcInfoReply, "xcb_randr_get_crtc_info_reply") &&
                symbol(randr, api.crtcInfoOutputs, "xcb_randr_get_crtc_info_outputs") &&
                symbol(randr, api.crtcInfoOutputsLength, "xcb_randr_get_crtc_info_outputs_length") &&
                symbol(randr, api.randrGetOutputPrimary, "xcb_randr_get_output_primary") &&
                symbol(randr, api.randrGetOutputPrimaryReply, "xcb_randr_get_output_primary_reply") &&
                symbol(randr, api.randrGetOutputInfo, "xcb_randr_get_output_info") &&
                symbol(randr, api.randrGetOutputInfoReply, "xcb_randr_get_output_info_reply") &&
                symbol(randr, api.outputInfoName, "xcb_randr_get_output_info_name") &&
                symbol(randr, api.outputInfoNameLength, "xcb_randr_get_output_info_name_length");
            if (!resolved) {
                result.reason = "missing-symbol";
                return result;
            }
            result.ok = true;
            return result;
        }();
        return load;
    }

    struct Freeing {
        void operator()(void* reply) const noexcept {
            std::free(reply);
        }
    };
    template <class T> using Owned = std::unique_ptr<T, Freeing>;

    struct Disconnect {
        void (*disconnect)(XcbConnection*);
        void operator()(XcbConnection* connection) const noexcept {
            disconnect(connection);
        }
    };

    // Waits for one reply. A request that failed yields no reply, and the reason names the stage
    // with libxcb's error code when it sent one.
    template <class T, class Fn, class... Args>
    Owned<T> replyOrReason(const char* stage, std::string& reason, Fn fetch, Args... args) {
        XcbGenericError* error = nullptr;
        Owned<T> value(static_cast<T*>(fetch(args..., &error)));
        const Owned<XcbGenericError> failure(error);
        if (!value) {
            reason = std::string(stage) + (error
                ? std::string("-error-") + std::to_string(error->error_code)
                : std::string("-failed"));
        }
        return value;
    }

    template <class T> std::span<const T> spanOf(const T* data, const int count) {
        return {data, static_cast<size_t>(count > 0 ? count : 0)};
    }

    // What the query needs per CRTC beyond the geometry in RandrCrtc.
    struct CrtcUse {
        uint32_t mode{0};
        uint32_t output{0};
    };

    const RandrModeInfo* findMode(std::span<const RandrModeInfo> modes, const uint32_t id) {
        for (const auto& mode : modes) {
            if (mode.id == id)
                return &mode;
        }
        return nullptr;
    }

    // The RandR query: window rectangle, CRTC choice, mode rate and output name. Fills `out` on
    // success; every failure sets out.reason and leaves hz at zero.
    void queryRandr(const uint32_t window, X11WindowRefresh& out) {
        const auto& loaded = loadXcbApi();
        if (!loaded.ok) {
            out.reason = loaded.reason;
            return;
        }
        const XcbApi& api = loaded.api;

        XcbConnection* raw = api.connect(nullptr, nullptr);
        if (!raw) {
            out.reason = "connect-failed";
            return;
        }
        const std::unique_ptr<XcbConnection, Disconnect> connection(raw, Disconnect{api.disconnect});
        if (api.hasError(raw)) {
            out.reason = "connect-failed";
            return;
        }

        const auto* extension = api.extensionData(raw, api.randrId);
        if (!extension || !extension->present) {
            out.reason = "randr-absent";
            return;
        }
        if (!replyOrReason<void>("randr-version", out.reason, api.randrQueryVersionReply,
                raw, api.randrQueryVersion(raw, 1, 3)))
            return;

        // The window's rectangle in root coordinates.
        const auto geometry = replyOrReason<XcbGeometryReply>("geometry", out.reason,
            api.getGeometryReply, raw, api.getGeometry(raw, window));
        if (!geometry)
            return;
        const uint32_t root = geometry->root;
        const uint32_t width = geometry->width;
        const uint32_t height = geometry->height;
        const auto translated = replyOrReason<XcbTranslateReply>("translate", out.reason,
            api.translateCoordinatesReply, raw, api.translateCoordinates(raw, window, root, 0, 0));
        if (!translated)
            return;
        const X11Rect windowRect{translated->dst_x, translated->dst_y, width, height};

        const auto resources = replyOrReason<RandrScreenResourcesReply>("resources", out.reason,
            api.randrGetScreenResourcesCurrentReply, raw, api.randrGetScreenResourcesCurrent(raw, root));
        if (!resources)
            return;
        const uint32_t configTimestamp = resources->config_timestamp;
        const auto crtcIds = spanOf(api.resourcesCrtcs(resources.get()),
            api.resourcesCrtcsLength(resources.get()));
        const auto modes = spanOf(api.resourcesModes(resources.get()),
            api.resourcesModesLength(resources.get()));

        std::vector<RandrCrtc> crtcs;
        std::vector<CrtcUse> uses;
        crtcs.reserve(crtcIds.size());
        uses.reserve(crtcIds.size());
        for (const uint32_t crtc : crtcIds) {
            const auto info = replyOrReason<RandrCrtcInfoReply>("crtc-info", out.reason,
                api.randrGetCrtcInfoReply, raw, api.randrGetCrtcInfo(raw, crtc, configTimestamp));
            if (!info)
                return;
            if (info->status != 0) {
                out.reason = "crtc-status-" + std::to_string(info->status);
                return;
            }
            const auto outputs = spanOf(api.crtcInfoOutputs(info.get()), api.crtcInfoOutputsLength(info.get()));
            crtcs.push_back(RandrCrtc{crtc, X11Rect{info->x, info->y, info->width, info->height},
                info->mode != 0 && !outputs.empty()});
            uses.push_back(CrtcUse{info->mode, outputs.empty() ? 0u : outputs.front()});
        }

        // The primary output's CRTC is asked for only when no active CRTC overlaps the window.
        const auto primaryCrtc = [&]() -> uint32_t {
            std::string ignored;
            const auto primary = replyOrReason<RandrOutputPrimaryReply>("primary", ignored,
                api.randrGetOutputPrimaryReply, raw, api.randrGetOutputPrimary(raw, root));
            if (!primary || primary->output == 0)
                return 0;
            const auto info = replyOrReason<RandrOutputInfoReply>("output-info", ignored,
                api.randrGetOutputInfoReply, raw, api.randrGetOutputInfo(raw, primary->output, configTimestamp));
            return info ? info->crtc : 0;
        };
        const auto chosen = chooseCrtc(crtcs, windowRect, primaryCrtc);
        if (!chosen) {
            out.reason = "no-active-crtc";
            return;
        }

        const CrtcUse& use = uses[*chosen];
        const RandrModeInfo* mode = findMode(modes, use.mode);
        if (!mode) {
            out.reason = "mode-not-found";
            return;
        }
        const double randrHz = UncappedAutoCap::refreshHzInRange(
            randrModeRefreshHz(mode->dot_clock, mode->htotal, mode->vtotal, mode->mode_flags));
        if (randrHz <= 0) {
            out.reason = "mode-rate-out-of-range";
            return;
        }
        out.crtc = crtcs[*chosen].id;
        out.hz = randrHz;
        out.source = "randr";

        // The output name is informational for the log and the Wayland match. Its failure keeps RandR.
        if (use.output != 0) {
            std::string ignored;
            const auto info = replyOrReason<RandrOutputInfoReply>("output-info", ignored,
                api.randrGetOutputInfoReply, raw, api.randrGetOutputInfo(raw, use.output, configTimestamp));
            if (info) {
                const auto name = spanOf(api.outputInfoName(info.get()), api.outputInfoNameLength(info.get()));
                if (!name.empty())
                    out.output.assign(reinterpret_cast<const char*>(name.data()), name.size());
            }
        }

        // Xwayland's RandR modes are synthesized CVT timings, slightly off the compositor's rate
        // (for example 59.88 for 59.95). The wl_output of the same name reports the exact rate.
        const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
        if (!out.output.empty() && waylandDisplay && *waylandDisplay) {
            const auto wayland = queryWaylandOutputRefresh(out.output);
            const double waylandHz = UncappedAutoCap::refreshHzInRange(refreshHzFromMilliHz(wayland.milliHz));
            if (waylandHz > 0) {
                out.hz = waylandHz;
                out.source = "wayland-output";
            } else {
                out.waylandReason = wayland.milliHz > 0 ? std::string("rate-out-of-range") : wayland.reason;
            }
        }
    }
} // namespace

X11WindowRefresh queryX11WindowRefresh(const uint64_t window) {
    X11WindowRefresh result;
    if (window == 0 || window > UINT32_MAX) {
        result.reason = "invalid-window";
        return result;
    }
    try {
        queryRandr(static_cast<uint32_t>(window), result);
    } catch (...) {
        result = X11WindowRefresh{};
        result.reason = "internal-error";
    }
    if (result.hz <= 0 && result.reason.empty())
        result.reason = "unknown";
    return result;
}

namespace {
    // The one record of a swapchain's query, written from the query thread.
    void logX11Refresh(const X11WindowRefresh& x11) {
        if (x11.hz > 0) {
            std::clog << "framegen-vulkan: x11 refresh: hz=" << x11.hz
                      << " crtc=" << x11.crtc
                      << " output=" << (x11.output.empty() ? std::string("unknown") : x11.output)
                      << " source=" << x11.source;
            if (!x11.waylandReason.empty())
                std::clog << " wayland=" << x11.waylandReason;
            std::clog << '\n';
        } else {
            std::clog << "framegen-vulkan: x11 refresh: hz=0 reason=" << x11.reason << '\n';
        }
    }
} // namespace

std::shared_ptr<X11RefreshCell> startX11WindowRefreshQuery(const uint64_t window) {
    auto cell = std::make_shared<X11RefreshCell>();
    // The query thread runs layer code. Like the other detached layer threads, it runs only while the
    // layer stays mapped; when that pin fails, no query runs.
    if (!pinLayerLibrary()) {
        std::clog << "framegen-vulkan: x11 refresh: hz=0 reason=layer-pin-failed\n";
        return cell;
    }
    try {
        // The creating thread never waits here. A stalled X or Wayland server holds only this thread,
        // and the cell it writes stays alive through the reference it owns.
        std::thread([cell, window]() noexcept {
            try {
                const auto x11 = queryX11WindowRefresh(window);
                logX11Refresh(x11);
                cell->source.store(x11.source, std::memory_order_relaxed);
                // Release publishes the source written above to the acquire load on the present path.
                cell->hz.store(x11.hz, std::memory_order_release);
            } catch (...) {
                std::clog << "framegen-vulkan: x11 refresh: hz=0 reason=internal-error\n";
            }
        }).detach();
    } catch (...) {
        std::clog << "framegen-vulkan: x11 refresh: hz=0 reason=thread-failed\n";
    }
    return cell;
}

} // namespace fgvk::layer

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "wayland_output_refresh.hpp"
#include "wayland_client_abi.hpp"
#include "x11_refresh_policy.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fgvk::layer {
namespace {
    // wl_output.mode flag of the mode the output is using now (wayland-client-protocol.h).
    constexpr uint32_t modeCurrent = 0x1;
    // wl_output.name arrived in version 4. Older outputs cannot be matched by name.
    constexpr uint32_t nameSinceVersion = 4;

    // libwayland-client entry points this query needs, resolved at run time.
    struct WaylandApi {
        wl_display* (*connect)(const char*) {};
        void (*disconnect)(wl_display*) {};
        int (*roundtrip)(wl_display*) {};
        wl_proxy* (*construct)(wl_proxy*, uint32_t, wl_argument*, const wl_interface*, uint32_t) {};
        int (*listen)(wl_proxy*, void (**)(void), void*) {};
        void (*destroy)(wl_proxy*) {};
        const wl_interface* registry{};
        const wl_interface* output{};
    };

    struct WaylandLoad {
        WaylandApi api{};
        bool ok{false};
    };

    template <class T> bool symbol(void* library, T& target, const char* name) {
        target = reinterpret_cast<T>(dlsym(library, name));
        return target != nullptr;
    }

    // Resolved once. The library stays mapped for the life of the process.
    const WaylandLoad& loadWaylandApi() {
        static const WaylandLoad load = [] {
            WaylandLoad result;
            void* library = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
            if (!library)
                return result;
            auto& api = result.api;
            if (!symbol(library, api.connect, "wl_display_connect") ||
                    !symbol(library, api.disconnect, "wl_display_disconnect") ||
                    !symbol(library, api.roundtrip, "wl_display_roundtrip") ||
                    !symbol(library, api.construct, "wl_proxy_marshal_array_constructor_versioned") ||
                    !symbol(library, api.listen, "wl_proxy_add_listener") ||
                    !symbol(library, api.destroy, "wl_proxy_destroy"))
                return result;
            api.registry = static_cast<const wl_interface*>(dlsym(library, "wl_registry_interface"));
            api.output = static_cast<const wl_interface*>(dlsym(library, "wl_output_interface"));
            result.ok = api.registry != nullptr && api.output != nullptr;
            return result;
        }();
        return load;
    }

    // One bound wl_output. Heap-allocated so its address is the listener's data.
    struct Output {
        std::string name;
        uint32_t milliHz{0};
        wl_proxy* proxy{nullptr};
    };

    // wl_output listener, in protocol event order: geometry, mode, done, scale, name, description.
    void outputGeometry(void*, wl_proxy*, int32_t, int32_t, int32_t, int32_t, int32_t,
            const char*, const char*, int32_t) noexcept {}
    void outputMode(void* data, wl_proxy*, uint32_t flags, int32_t, int32_t, int32_t refresh) noexcept {
        auto& output = *static_cast<Output*>(data);
        if ((flags & modeCurrent) != 0 && refresh > 0)
            output.milliHz = static_cast<uint32_t>(refresh);
    }
    void outputDone(void*, wl_proxy*) noexcept {}
    void outputScale(void*, wl_proxy*, int32_t) noexcept {}
    void outputName(void* data, wl_proxy*, const char* name) noexcept {
        if (!name)
            return;
        try {
            static_cast<Output*>(data)->name = name;
        } catch (...) {} // A missing name only leaves this output unmatched.
    }
    void outputDescription(void*, wl_proxy*, const char*) noexcept {}

    void registryGlobal(void* data, wl_proxy*, uint32_t name, const char* interface, uint32_t version) noexcept;
    void registryGlobalRemove(void*, wl_proxy*, uint32_t) noexcept {}

    // Owns one connection to the compositor, its registry and its output proxies. Every proxy is
    // destroyed before the display is disconnected, on every path out of the query.
    class Connection {
    public:
        Connection(const WaylandApi& wayland, wl_display* connected) : api(wayland), display(connected) {}
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        ~Connection() {
            for (const auto& output : outputs) {
                if (output->proxy)
                    api.destroy(output->proxy);
            }
            if (registry)
                api.destroy(registry);
            api.disconnect(display);
        }

        // wl_display.get_registry is request opcode 1. The display proxy shares the wl_proxy
        // layout, so the request goes through the same marshalling as the generated inline.
        bool start() {
            wl_argument arg{};
            registry = api.construct(reinterpret_cast<wl_proxy*>(display), 1, &arg, api.registry, 1);
            static void (*listener[])(void){reinterpret_cast<void (*)(void)>(&registryGlobal),
                reinterpret_cast<void (*)(void)>(&registryGlobalRemove)};
            return registry != nullptr && api.listen(registry, listener, this) == 0;
        }

        // Called from the registry's global event during the first round trip. Binds version 4+
        // wl_output globals; the events for the bound outputs arrive during the second round trip.
        void bindOutput(const uint32_t name, const char* interface, const uint32_t version) {
            if (version < nameSinceVersion || !interface || std::strcmp(interface, api.output->name) != 0)
                return;
            const uint32_t bound = std::min(version, nameSinceVersion);
            wl_argument args[4]{};
            args[0].u = name;
            args[1].s = api.output->name;
            args[2].u = bound;
            outputs.push_back(std::make_unique<Output>());
            Output& output = *outputs.back();
            output.proxy = api.construct(registry, 0, args, api.output, bound);
            if (!output.proxy)
                return;
            static void (*listener[])(void){reinterpret_cast<void (*)(void)>(&outputGeometry),
                reinterpret_cast<void (*)(void)>(&outputMode),
                reinterpret_cast<void (*)(void)>(&outputDone),
                reinterpret_cast<void (*)(void)>(&outputScale),
                reinterpret_cast<void (*)(void)>(&outputName),
                reinterpret_cast<void (*)(void)>(&outputDescription)};
            if (api.listen(output.proxy, listener, &output) != 0) {
                api.destroy(output.proxy);
                output.proxy = nullptr;
            }
        }

        [[nodiscard]] bool hasOutputs() const {
            return !outputs.empty();
        }

        [[nodiscard]] std::optional<uint32_t> milliHzOf(const std::string_view name) const {
            std::vector<NamedRefresh> named;
            named.reserve(outputs.size());
            for (const auto& output : outputs)
                named.push_back(NamedRefresh{output->name, output->milliHz});
            return outputRefreshMilliHz(name, named);
        }

    private:
        const WaylandApi& api;
        wl_display* display;
        wl_proxy* registry{nullptr};
        std::vector<std::unique_ptr<Output>> outputs;
    };

    void registryGlobal(void* data, wl_proxy*, uint32_t name, const char* interface, uint32_t version) noexcept {
        try {
            static_cast<Connection*>(data)->bindOutput(name, interface, version);
        } catch (...) {} // Not binding an output leaves the query without a match.
    }
} // namespace

WaylandOutputRefresh queryWaylandOutputRefresh(const std::string_view outputName) {
    WaylandOutputRefresh result;
    try {
        const auto& loaded = loadWaylandApi();
        if (!loaded.ok) {
            result.reason = "no-libwayland";
            return result;
        }
        wl_display* display = loaded.api.connect(nullptr);
        if (!display) {
            result.reason = "connect-failed";
            return result;
        }
        Connection connection(loaded.api, display);
        if (!connection.start()) {
            result.reason = "registry-failed";
            return result;
        }
        // The first round trip delivers the globals and binds the outputs; the second delivers
        // their name and mode events.
        if (loaded.api.roundtrip(display) < 0 || loaded.api.roundtrip(display) < 0) {
            result.reason = "roundtrip-failed";
            return result;
        }
        if (!connection.hasOutputs()) {
            result.reason = "no-wl-output-v4";
            return result;
        }
        const auto milliHz = connection.milliHzOf(outputName);
        if (!milliHz) {
            result.reason = "no-output-match";
            return result;
        }
        result.milliHz = *milliHz;
    } catch (...) {
        result = WaylandOutputRefresh{};
        result.reason = "internal-error";
    }
    return result;
}

} // namespace fgvk::layer

/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

// GP1: the gamescope on-screen profile. While the gate is active the layer owns the lower present
// IDs of its swapchain: every lower present gets the next ID of OnScreenIds, and an application
// wait for its own ID is translated to the lower ID that was presented for it. Nothing here touches
// Vulkan, so the gate, the owner and the registry are unit-tested without a driver.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace fgvk::layer {

// The session kind the gate reads: a gamescope session, an X11 surface (Xwayland or plain X11), or
// neither (native Wayland surfaces outside gamescope).
enum class OnScreenKind : uint8_t { None, Gamescope, X11 };

[[nodiscard]] constexpr const char* onScreenKindName(const OnScreenKind kind) noexcept {
    switch (kind) {
        case OnScreenKind::Gamescope: return "gamescope";
        case OnScreenKind::X11: return "x11";
        case OnScreenKind::None: break;
    }
    return "none";
}

// The gate, decided once per lower swapchain at creation. `reason` names the first false input
// (a string literal); it is null when the gate is active. `kind` is the session kind it was given.
struct OnScreenGate {
    bool active{};
    const char* reason{};
    OnScreenKind kind{};
};

// The gate's inputs are checked in this order, so the reason names the first missing one.
// The owned-present-timing reason is not an input here: the caller adds it (see entrypoint.cpp).
// `enabled` is the watcher's enable: the pacing profile (its records) or the automatic output correction (OS1),
// which needs the watcher's screen samples and no profile. With neither, the reason is "disabled".
[[nodiscard]] constexpr OnScreenGate onScreenGate(const bool enabled, const OnScreenKind sessionKind,
        const bool presentId2Flag, const bool presentWait2Flag, const bool waitFunction) {
    if (!enabled) return {false, "disabled", sessionKind};
    if (sessionKind == OnScreenKind::None) return {false, "surface-kind", sessionKind};
    if (!presentId2Flag) return {false, "present-id2-flag", sessionKind};
    if (!presentWait2Flag) return {false, "present-wait2-flag", sessionKind};
    if (!waitFunction) return {false, "wait-function", sessionKind};
    return {true, nullptr, sessionKind};
}

// One lower swapchain's ID owner. Thread-safe: presents hand out IDs under queue->mutex, waits
// translate under this lock only. Lower IDs start at 1 (0 means no ID) and rise by one per lower
// present on the swapchain; the caller presents in the order the IDs were handed out.
class OnScreenIds {
public:
    static constexpr size_t ringCapacity = 256;

    // The next lower present ID.
    [[nodiscard]] uint64_t allocate() noexcept {
        const std::lock_guard lock(mutex);
        return ++last;
    }

    // A real output's application ID and the lower ID it was presented with. Zero is not recorded.
    void record(const uint64_t appId, const uint64_t lowerId) noexcept {
        if (!appId || !lowerId) return;
        const std::lock_guard lock(mutex);
        ring[next++ % ringCapacity] = Pair{appId, lowerId};
        if (count < ringCapacity) ++count;
    }

    // Present-wait semantics ("at least N"): the lower ID of the smallest recorded application ID
    // >= appId. An ID older than the ring gives the oldest entry, which is already on screen. Nothing
    // recorded at or above appId gives nullopt.
    [[nodiscard]] std::optional<uint64_t> translate(const uint64_t appId) const noexcept {
        const std::lock_guard lock(mutex);
        std::optional<Pair> best;
        for (size_t i = 0; i < count; ++i) {
            const Pair& pair = ring[i];
            if (pair.appId >= appId && (!best || pair.appId < best->appId)) best = pair;
        }
        if (!best) return std::nullopt;
        return best->lowerId;
    }

    // True for the first caller only: a translation miss is logged once per swapchain.
    [[nodiscard]] bool firstMiss() noexcept {
        return !missLogged.exchange(true, std::memory_order_relaxed);
    }

    // PT3: the reverse of translate. The application ID a lower ID was recorded with, exactly: lower IDs are unique on a
    // swapchain. A lower ID that was not recorded (a generated or internal present, an ID older than the ring, or zero)
    // gives nullopt.
    [[nodiscard]] std::optional<uint64_t> applicationIdOf(const uint64_t lowerId) const noexcept {
        if (!lowerId) return std::nullopt;
        const std::lock_guard lock(mutex);
        for (size_t i = 0; i < count; ++i) {
            if (ring[i].lowerId == lowerId) return ring[i].appId;
        }
        return std::nullopt;
    }

    // PT3: true for the first caller only: a past presentation entry removed for having no application ID is logged once per
    // swapchain.
    [[nodiscard]] bool firstDrop() noexcept {
        return !dropLogged.exchange(true, std::memory_order_relaxed);
    }

private:
    struct Pair {
        uint64_t appId{};
        uint64_t lowerId{};
    };
    mutable std::mutex mutex;
    uint64_t last{};
    std::array<Pair, ringCapacity> ring{};
    size_t next{};
    size_t count{};
    std::atomic_bool missLogged{false};
    std::atomic_bool dropLogged{false};
};

// The ID owners of the active lower swapchains, found by a wait's device and swapchain. Only an
// active gate registers, so a wait pays for the lookup only while some gate is active.
class OnScreenRegistry {
public:
    using Key = std::pair<uint64_t, uint64_t>; // device, swapchain

    static void add(const Key key, std::shared_ptr<OnScreenIds> ids) {
        const std::lock_guard lock(mutex());
        entries()[key] = std::move(ids);
        activeCount().store(entries().size(), std::memory_order_release);
    }
    static void remove(const Key key) noexcept {
        try {
            const std::lock_guard lock(mutex());
            entries().erase(key);
            activeCount().store(entries().size(), std::memory_order_release);
        } catch (...) {
            // Erasing an existing key cannot fail; a lock failure leaves the entry for a later stop.
        }
    }
    [[nodiscard]] static bool anyActive() noexcept {
        return activeCount().load(std::memory_order_acquire) != 0;
    }
    [[nodiscard]] static std::shared_ptr<OnScreenIds> find(const Key key) {
        const std::lock_guard lock(mutex());
        const auto found = entries().find(key);
        return found == entries().end() ? nullptr : found->second;
    }

private:
    static std::mutex& mutex() {
        static std::mutex value;
        return value;
    }
    static std::map<Key, std::shared_ptr<OnScreenIds>>& entries() {
        static std::map<Key, std::shared_ptr<OnScreenIds>> value;
        return value;
    }
    static std::atomic<size_t>& activeCount() {
        static std::atomic<size_t> value{0};
        return value;
    }
};

} // namespace fgvk::layer

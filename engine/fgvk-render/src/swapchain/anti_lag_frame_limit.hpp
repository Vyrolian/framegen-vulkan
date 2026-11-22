/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

// The maxFPS the driver gets for the application's VK_AMD_anti_lag request. With anti-lag on and frame generation
// at a cap, the limit is the cap rounded up, so the driver never holds below fgvk's exact cap, or the application's
// own limit when that is lower. OFF, frame generation off and no cap pass the application's limit through. The
// spec reads a maxFPS of 0 as no limit, so a positive cap rounds to at least 1.
[[nodiscard]] inline uint32_t antiLagMaxFps(const VkAntiLagModeAMD mode, const uint32_t appMaxFps,
        const bool frameGeneration, const double capFps) noexcept {
    if (mode == VK_ANTI_LAG_MODE_OFF_AMD || !frameGeneration || !(capFps > 0.0))
        return appMaxFps;
    const double capCeil = std::ceil(capFps - 1e-6);
    const uint32_t cap = capCeil >= static_cast<double>(std::numeric_limits<uint32_t>::max())
        ? std::numeric_limits<uint32_t>::max()
        : std::max(uint32_t{1}, static_cast<uint32_t>(std::max(capCeil, 0.0)));
    return appMaxFps == 0 ? cap : std::min(appMaxFps, cap);
}

[[nodiscard]] inline const char* antiLagModeName(const VkAntiLagModeAMD mode) noexcept {
    if (mode == VK_ANTI_LAG_MODE_OFF_AMD) return "off";
    if (mode == VK_ANTI_LAG_MODE_ON_AMD) return "on";
    if (mode == VK_ANTI_LAG_MODE_DRIVER_CONTROL_AMD) return "driver";
    return "unknown";
}

// AMD anti-lag state of one VkDevice. The present path stores the frame generation state and the effective cap
// (observe). The application's vkAntiLagUpdateAMD is copied, limited and forwarded (applicationUpdate). The
// application may call from another thread than the one presenting, so both are guarded. The driver call happens
// outside the lock (the INPUT stage may sleep there).
class AntiLagFrameLimit {
public:
    using Clock = std::chrono::steady_clock;

    // A game that stops calling anti-lag no longer holds the game: its last ON call counts for this long.
    static constexpr std::chrono::milliseconds holdWindow{500};

    explicit AntiLagFrameLimit(const VkDevice owner = nullptr) : device(owner) {}

    // The present path, once per present with that present's frame generation state and effective cap.
    // Stores only: no driver call and one lock.
    void observe(const bool frameGeneration, const double capFps) {
        const std::lock_guard lock(this->mutex);
        this->observedFrameGeneration = frameGeneration;
        this->observedCapFps = capFps;
    }

    // The application's vkAntiLagUpdateAMD. A copy of the request carries the limited maxFPS, and every other
    // field is forwarded as the application made it. `send` makes the driver call and returns its result. The driver
    // call runs without the lock: the INPUT stage is where the driver sleeps to pace the game, and the present path
    // (observe, holdsGame) must never wait behind that sleep.
    template<typename Send>
    VkResult applicationUpdate(const VkAntiLagDataAMD& application, Send&& send) {
        VkAntiLagDataAMD forwarded = application;
        {
            const std::lock_guard lock(this->mutex);
            forwarded.maxFPS = antiLagMaxFps(application.mode, application.maxFPS,
                this->observedFrameGeneration, this->observedCapFps);
        }
        const auto result = send(static_cast<const VkAntiLagDataAMD*>(&forwarded));
        const Call call{application.mode, forwarded.maxFPS, Clock::now()};
        const std::lock_guard lock(this->mutex);
        // One line per applied change of the mode or of the limit; a repeated call logs nothing.
        if (!this->last || this->last->mode != call.mode || this->last->maxFPS != call.maxFPS) {
            std::clog << "framegen-vulkan: anti-lag frame limit: device=" << static_cast<const void*>(this->device)
                      << " mode=" << antiLagModeName(call.mode)
                      << " app_max_fps=" << application.maxFPS
                      << " applied_max_fps=" << call.maxFPS
                      << " result=" << static_cast<int>(result) << '\n';
        }
        this->last = call;
        return result;
    }

    // A null pData is forwarded unchanged and touches no state.
    template<typename Send>
    VkResult applicationUpdate(const VkAntiLagDataAMD* application, Send&& send) {
        if (application) return applicationUpdate(*application, std::forward<Send>(send));
        return send(static_cast<const VkAntiLagDataAMD*>(nullptr));
    }

    // Whether the game's anti-lag holds it: its last accepted call asked for ON, and that call was within holdWindow
    // of `now`. Read per present under the same lock as the calls.
    [[nodiscard]] bool holdsGame(const Clock::time_point now) const {
        const std::lock_guard lock(this->mutex);
        return this->last && this->last->mode == VK_ANTI_LAG_MODE_ON_AMD && now - this->last->at < holdWindow;
    }

private:
    struct Call {
        VkAntiLagModeAMD mode{};
        uint32_t maxFPS{};
        Clock::time_point at{};
    };

    mutable std::mutex mutex;
    VkDevice device{};
    bool observedFrameGeneration{false};
    double observedCapFps{0.0};
    std::optional<Call> last;
};

// Anti-lag state per VkDevice, shared by the anti-lag wrapper (any application thread) and the device's present
// path. An entry is made by whichever side comes first, so an update made before the first present is kept, and is
// erased at vkDestroyDevice.
class AntiLagFrameLimitRegistry {
public:
    [[nodiscard]] static std::shared_ptr<AntiLagFrameLimit> get(const VkDevice device) {
        auto& self = instance();
        const std::lock_guard lock(self.mutex);
        auto& entry = self.entries[device];
        if (!entry) entry = std::make_shared<AntiLagFrameLimit>(device);
        return entry;
    }
    static void erase(const VkDevice device) {
        auto& self = instance();
        const std::lock_guard lock(self.mutex);
        self.entries.erase(device);
    }

private:
    static AntiLagFrameLimitRegistry& instance() {
        static AntiLagFrameLimitRegistry registry;
        return registry;
    }
    std::mutex mutex;
    std::unordered_map<VkDevice, std::shared_ptr<AntiLagFrameLimit>> entries;
};

}

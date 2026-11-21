/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

// The minimum interval the application's Reflex limiter (VK_NV_low_latency2)
// holds between frames. With frame generation on at an output cap, the cap's
// interval is the least the game may hold, so the game stops running ahead of
// the cap. Any other request reaches the driver with the application's values.
[[nodiscard]] inline uint32_t reflexMinimumIntervalUs(const VkLatencySleepModeInfoNV& request,
        const bool frameGeneration, const double capFps) noexcept {
    if (!request.lowLatencyMode || !frameGeneration || !(capFps > 0.0))
        return request.minimumIntervalUs;
    const double capIntervalUs = std::ceil(1e6 / capFps);
    const auto capInterval = capIntervalUs >= static_cast<double>(std::numeric_limits<uint32_t>::max())
        ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(capIntervalUs);
    return std::max(request.minimumIntervalUs, capInterval);
}

// One swapchain's Reflex request and the frame state it is forwarded with. The
// application may call Reflex from another thread than the one presenting, so
// both are guarded. The driver call happens under the lock, so the driver sees
// the decisions in the order they were made.
class ReflexFrameLimit {
public:
    // One driver call: what it carries, and why it happens.
    struct Forward {
        VkLatencySleepModeInfoNV info{};
        uint32_t applicationIntervalUs{};
        uint32_t previousIntervalUs{};
        const char* reason{};
    };

    // The application's vkSetLatencySleepModeNV. Its request is remembered, with
    // pNext cleared, and forwarded at once with the frame limit applied. `send`
    // makes the driver call and returns its result.
    template<typename Send>
    VkResult applicationRequest(const VkSwapchainKHR swapchain,
            const VkLatencySleepModeInfoNV& application, Send&& send) {
        const std::lock_guard lock(this->mutex);
        Forward forward{
            .info = application,
            .applicationIntervalUs = application.minimumIntervalUs,
            .reason = "application",
        };
        forward.info.minimumIntervalUs =
            reflexMinimumIntervalUs(application, this->frameGeneration, this->capFps);
        const auto result = this->forwardLocked(swapchain, std::move(forward), send);
        if (result == VK_SUCCESS) {
            this->request = application;
            this->request->pNext = nullptr;
        }
        return result;
    }

    // A null sleep-mode request disables the driver state and must retire the cached request.
    template<typename Send>
    VkResult clearRequest(Send&& send) {
        const std::lock_guard lock(this->mutex);
        const auto result = send();
        if (result == VK_SUCCESS) {
            this->request.reset();
            this->forwardedIntervalUs = 0;
        }
        return result;
    }

    // The present path, once per present with that present's frame generation
    // state and effective cap. It sends only when the interval changed since the
    // last call, so it costs one lock and a compare on every other present.
    template<typename Send>
    void observe(const VkSwapchainKHR swapchain, const bool frameGeneration,
            const double capFps, Send&& send) {
        const std::lock_guard lock(this->mutex);
        const bool frameGenerationChanged = frameGeneration != this->frameGeneration;
        this->frameGeneration = frameGeneration;
        this->capFps = capFps;
        if (!this->request) return;
        const auto interval = reflexMinimumIntervalUs(*this->request, frameGeneration, capFps);
        if (interval == this->forwardedIntervalUs) return;
        Forward forward{
            .info = *this->request,
            .applicationIntervalUs = this->request->minimumIntervalUs,
            .reason = frameGenerationChanged ? "frame-generation" : "cap",
        };
        forward.info.minimumIntervalUs = interval;
        this->forwardLocked(swapchain, std::move(forward), send);
    }

    // Whether the application's last accepted sleep mode asks for low latency mode, so the game's own
    // limiter holds its frames. Read per present under the same lock as the requests.
    [[nodiscard]] bool reflexLowLatency() const {
        const std::lock_guard lock(this->mutex);
        return this->request && this->request->lowLatencyMode;
    }

    // The G2 condition: low latency mode is requested and frame generation is active on this swapchain (RF1 uses it to
    // decide when the queue drain runs). Read per sleep under the same lock as the requests.
    [[nodiscard]] bool lowLatencyFrameGeneration() const {
        const std::lock_guard lock(this->mutex);
        return this->request && this->request->lowLatencyMode && this->frameGeneration;
    }

    // The minimum interval the driver last accepted for this swapchain, 0 when none.
    [[nodiscard]] uint32_t acceptedIntervalUs() const {
        const std::lock_guard lock(this->mutex);
        return this->forwardedIntervalUs;
    }

private:
    template<typename Send>
    VkResult forwardLocked(const VkSwapchainKHR swapchain, Forward forward, Send& send) {
        forward.previousIntervalUs = this->forwardedIntervalUs;
        const auto result = send(static_cast<const Forward&>(forward));
        if (result == VK_SUCCESS)
            this->forwardedIntervalUs = forward.info.minimumIntervalUs;
        // One line per applied change of the interval; a repeated request logs nothing.
        if (forward.info.minimumIntervalUs != forward.previousIntervalUs) {
            std::clog << "framegen-vulkan: reflex frame limit: swapchain=" << swapchain
                      << " app_interval_us=" << forward.applicationIntervalUs
                      << " applied_interval_us=" << forward.info.minimumIntervalUs
                      << " reason=" << forward.reason
                      << " result=" << static_cast<int>(result) << '\n';
        }
        return result;
    }

    mutable std::mutex mutex;
    std::optional<VkLatencySleepModeInfoNV> request;
    bool frameGeneration{false};
    double capFps{0.0};
    // Last interval accepted by the driver for this swapchain.
    uint32_t forwardedIntervalUs{0};
};

// Reflex state per application swapchain handle, shared by the Reflex wrapper
// (any application thread) and the swapchain's present path. Guarded by its own
// lock because the application may call Reflex while another thread creates or
// destroys swapchains. An entry is made by whichever side comes first, so a
// request made before the first present is kept, and erased at destroy. One
// registry per entry type T.
template<class T>
class ReflexSwapchainRegistry {
public:
    [[nodiscard]] static std::shared_ptr<T> get(const VkSwapchainKHR swapchain) {
        auto& self = instance();
        const std::lock_guard lock(self.mutex);
        auto& entry = self.entries[swapchain];
        if (!entry) entry = std::make_shared<T>();
        return entry;
    }
    // The swapchain's entry, or null. Never creates one.
    [[nodiscard]] static std::shared_ptr<T> find(const VkSwapchainKHR swapchain) {
        auto& self = instance();
        const std::lock_guard lock(self.mutex);
        const auto entry = self.entries.find(swapchain);
        return entry == self.entries.end() ? std::shared_ptr<T>{} : entry->second;
    }
    static void erase(const VkSwapchainKHR swapchain) {
        auto& self = instance();
        const std::lock_guard lock(self.mutex);
        self.entries.erase(swapchain);
    }
    // Every entry at this moment, for an action over the whole registry (RF1 stops the relays of a destroyed device).
    [[nodiscard]] static std::vector<std::pair<VkSwapchainKHR, std::shared_ptr<T>>> snapshot() {
        auto& self = instance();
        const std::lock_guard lock(self.mutex);
        std::vector<std::pair<VkSwapchainKHR, std::shared_ptr<T>>> all;
        all.reserve(self.entries.size());
        for (const auto& [swapchain, entry] : self.entries) all.emplace_back(swapchain, entry);
        return all;
    }

private:
    static ReflexSwapchainRegistry& instance() {
        static ReflexSwapchainRegistry registry;
        return registry;
    }
    std::mutex mutex;
    std::unordered_map<VkSwapchainKHR, std::shared_ptr<T>> entries;
};

using ReflexFrameLimitRegistry = ReflexSwapchainRegistry<ReflexFrameLimit>;

}

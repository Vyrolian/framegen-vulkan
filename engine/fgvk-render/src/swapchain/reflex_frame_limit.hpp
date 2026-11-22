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
#include <string_view>
#include <unordered_map>
#include <utility>

#include <vulkan/vulkan_core.h>

#include "swapchain/reflex_dynamic_cap.hpp"

namespace fgvk::layer {

// The minimum interval the application's Reflex limiter (VK_NV_low_latency2) holds between frames. With frame generation
// on at an output cap, the cap's interval is the least the game may hold, so the game stops running ahead of the cap.
// Any other request reaches the driver with the application's values.
[[nodiscard]] inline uint32_t reflexMinimumIntervalUs(const VkLatencySleepModeInfoNV& request,
        const bool frameGeneration, const double capFps) noexcept {
    if (!request.lowLatencyMode || !frameGeneration || !(capFps > 0.0))
        return request.minimumIntervalUs;
    const double capIntervalUs = std::ceil(1e6 / capFps);
    const auto capInterval = capIntervalUs >= static_cast<double>(std::numeric_limits<uint32_t>::max())
        ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(capIntervalUs);
    return std::max(request.minimumIntervalUs, capInterval);
}

// RDC1: what the dynamic cap contributes to one present: the device's controller (null when there is none), the
// profile setting, and the clock the controller reads (reflexClockNs()).
struct ReflexDynamicInput {
    std::shared_ptr<ReflexDynamicCap> controller{};
    bool setting{};
    int64_t nowNs{};
};

// One swapchain's Reflex request and the frame state it is forwarded with. The application may call Reflex from another
// thread than the one presenting, so both are guarded. The driver call happens under the lock, so the driver sees the
// decisions in the order they were made.
class ReflexFrameLimit {
public:
    // One driver call: what it carries, and why it happens.
    struct Forward {
        VkLatencySleepModeInfoNV info{};
        uint32_t applicationIntervalUs{};
        uint32_t previousIntervalUs{};
        const char* reason{};
        // The interval is the dynamic cap's, above the static interval.
        bool dynamic{};
    };

    // The application's vkSetLatencySleepModeNV. Its request is remembered, with
    // pNext cleared, and forwarded at once with the frame limit applied. `send`
    // makes the driver call and returns its result.
    template<typename Send>
    VkResult applicationRequest(const VkSwapchainKHR swapchain,
            const VkLatencySleepModeInfoNV& application, Send&& send) {
        const std::lock_guard lock(this->mutex);
        const auto staticInterval = reflexMinimumIntervalUs(application, this->frameGeneration, this->capFps);
        const auto applied = this->appliedIntervalLocked(application, reflexClockNs());
        Forward forward{
            .info = application,
            .applicationIntervalUs = application.minimumIntervalUs,
            .reason = "application",
            .dynamic = applied != staticInterval,
        };
        forward.info.minimumIntervalUs = applied;
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
            this->forwardedDynamic = false;
            this->pendingDynamicLog = false;
        }
        return result;
    }

    // The present path, once per present with that present's frame generation state, effective cap and dynamic
    // input. It sends only when the interval changed since the last call, so it costs one lock and a compare on
    // every other present.
    template<typename Send>
    void observe(const VkSwapchainKHR swapchain, const bool frameGeneration,
            const double capFps, Send&& send) {
        observe(swapchain, frameGeneration, capFps, ReflexDynamicInput{}, std::forward<Send>(send));
    }

    template<typename Send>
    void observe(const VkSwapchainKHR swapchain, const bool frameGeneration,
            const double capFps, const ReflexDynamicInput& dynamicInput, Send&& send) {
        const std::lock_guard lock(this->mutex);
        const bool frameGenerationChanged = frameGeneration != this->frameGeneration;
        this->frameGeneration = frameGeneration;
        this->capFps = capFps;
        this->dynamicCap = dynamicInput.controller;
        this->dynamicSetting = dynamicInput.setting;
        this->flushDynamicLogLocked(swapchain);
        if (!this->request) return;
        const auto staticInterval = reflexMinimumIntervalUs(*this->request, frameGeneration, capFps);
        const auto interval = this->appliedIntervalLocked(*this->request, dynamicInput.nowNs);
        if (interval == this->forwardedIntervalUs) return;
        // A dynamic change moves the driver only by 50 us or more. A return to the static interval always goes out, so
        // the static interval is exact again.
        const auto moved = interval > this->forwardedIntervalUs ? interval - this->forwardedIntervalUs
            : this->forwardedIntervalUs - interval;
        const bool raised = interval != staticInterval;
        if (raised && moved < reflexDynamicMinimumChangeUs) return;
        Forward forward{
            .info = *this->request,
            .applicationIntervalUs = this->request->minimumIntervalUs,
            .reason = frameGenerationChanged ? "frame-generation"
                : (raised || this->forwardedDynamic) ? "dynamic" : "cap",
            .dynamic = raised,
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

private:
    // RDC1: the interval the driver should hold for `requested`: the static interval, or the dynamic cap's when it is
    // higher. The dynamic cap runs only with the setting, Reflex low latency mode, frame generation and a cap.
    [[nodiscard]] uint32_t appliedIntervalLocked(const VkLatencySleepModeInfoNV& requested, const int64_t nowNs) {
        const auto staticInterval = reflexMinimumIntervalUs(requested, this->frameGeneration, this->capFps);
        if (!this->dynamicCap) return staticInterval;
        const bool active = this->dynamicSetting && requested.lowLatencyMode && this->frameGeneration &&
            this->capFps > 0.0;
        return std::max(staticInterval, this->dynamicCap->interval(nowNs, staticInterval, active));
    }

    template<typename Send>
    VkResult forwardLocked(const VkSwapchainKHR swapchain, Forward forward, Send& send) {
        forward.previousIntervalUs = this->forwardedIntervalUs;
        const auto result = send(static_cast<const Forward&>(forward));
        if (result == VK_SUCCESS) {
            this->forwardedIntervalUs = forward.info.minimumIntervalUs;
            this->forwardedDynamic = forward.dynamic;
        }
        // One line per applied change of the interval; a repeated request logs nothing. Dynamic changes log at most
        // once a second: a change inside that second waits, and a later present logs the latest value.
        if (forward.info.minimumIntervalUs == forward.previousIntervalUs) return result;
        if (std::string_view(forward.reason) == "dynamic") {
            const auto now = std::chrono::steady_clock::now();
            if (now - this->lastDynamicLog < std::chrono::seconds(1)) {
                this->pendingDynamicLog = true;
                return result;
            }
            this->lastDynamicLog = now;
            this->pendingDynamicLog = false;
        }
        logForward(swapchain, forward.applicationIntervalUs, forward.info.minimumIntervalUs, forward.reason, result);
        return result;
    }

    // Logs the latest dynamic value once the second since the last dynamic log has passed.
    void flushDynamicLogLocked(const VkSwapchainKHR swapchain) {
        if (!this->pendingDynamicLog || !this->request) return;
        const auto now = std::chrono::steady_clock::now();
        if (now - this->lastDynamicLog < std::chrono::seconds(1)) return;
        this->lastDynamicLog = now;
        this->pendingDynamicLog = false;
        logForward(swapchain, this->request->minimumIntervalUs, this->forwardedIntervalUs, "dynamic", VK_SUCCESS);
    }

    static void logForward(const VkSwapchainKHR swapchain, const uint32_t applicationIntervalUs,
            const uint32_t appliedIntervalUs, const char* reason, const VkResult result) {
        std::clog << "framegen-vulkan: reflex frame limit: swapchain=" << swapchain
                  << " app_interval_us=" << applicationIntervalUs
                  << " applied_interval_us=" << appliedIntervalUs
                  << " reason=" << reason << " result=" << static_cast<int>(result) << '\n';
    }

    // A dynamic change of the interval smaller than this is not sent (a return to the static interval is always sent).
    static constexpr uint32_t reflexDynamicMinimumChangeUs = 50;

    mutable std::mutex mutex;
    std::optional<VkLatencySleepModeInfoNV> request;
    bool frameGeneration{false};
    double capFps{0.0};
    // Last interval accepted by the driver for this swapchain, and whether the dynamic cap set it.
    uint32_t forwardedIntervalUs{0};
    bool forwardedDynamic{false};
    // The device's dynamic cap controller and the profile setting, from the last present.
    std::shared_ptr<ReflexDynamicCap> dynamicCap;
    bool dynamicSetting{false};
    std::chrono::steady_clock::time_point lastDynamicLog{};
    bool pendingDynamicLog{false};
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

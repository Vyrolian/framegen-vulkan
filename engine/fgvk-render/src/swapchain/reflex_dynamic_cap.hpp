/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

// RDC1, the dynamic Reflex cap. The game's Reflex (VK_NV_low_latency2) holds each frame at the interval that fgvk
// forwards (reflex_frame_limit.hpp). With that interval on this driver the GPU stays 100 % busy and the game's frames
// wait in the GPU queue: 4.9 ms p50 uncapped against 0.09 ms at the cap (GQ1). This controller raises the forwarded
// interval while the main queue's wait is high and lowers it again when the wait is low. It never goes below the static
// interval, so at an attainable ceiling the wait stays low and the interval stays exactly at the static one.
//
// Input: the queue wait of each resolved frame from the GQ1 probe (queue_start_probes.cpp), GPU start less the frame's
// first submit. Only the main queue feeds the controller: the queue with the highest moving average of submits per frame.

// FGVK_REFLEX_DYNAMIC_CAP=1 turns the dynamic cap on and =0 turns it off. Any other value, or unset, leaves the
// profile's reflex_dynamic_cap setting in charge. Read once.
[[nodiscard]] inline int reflexDynamicCapEnvironment() noexcept {
    static const int fromEnvironment = [] {
        const char* text = std::getenv("FGVK_REFLEX_DYNAMIC_CAP");
        if (!text) return -1;
        if (std::strcmp(text, "1") == 0) return 1;
        if (std::strcmp(text, "0") == 0) return 0;
        return -1;
    }();
    return fromEnvironment;
}

// Whether the dynamic cap is on for a profile whose setting is `setting`, with the environment override.
[[nodiscard]] inline bool reflexDynamicCapEnabled(const bool setting) noexcept {
    const int forced = reflexDynamicCapEnvironment();
    return forced < 0 ? setting : forced == 1;
}

// Controller constants, in microseconds where a time applies. A window is the median wait of this many main-queue frames.
inline constexpr size_t reflexDynamicWindowFrames = 16;
// The interval rises while the median exceeds this wait, by gain times the excess over the target, at most by the limit.
inline constexpr double reflexDynamicRaiseAboveUs = 1000.0;
inline constexpr double reflexDynamicRaiseTargetUs = 500.0;
inline constexpr double reflexDynamicRaiseGain = 0.25;
inline constexpr double reflexDynamicRaiseLimit = 0.05;
// The interval falls by the decay fraction while the median stays below this wait.
inline constexpr double reflexDynamicDecayBelowUs = 300.0;
inline constexpr double reflexDynamicDecay = 0.005;
// The interval never exceeds this, unless the static interval itself is longer.
inline constexpr double reflexDynamicMaximumUs = 50000.0;
// No main-queue frame for this long resets the controller to the static interval (pause, loading).
inline constexpr int64_t reflexDynamicIdleNs = 1'000'000'000;
// Application queues tracked per device; a new queue past this replaces the one with the lowest average.
inline constexpr size_t reflexDynamicMaximumQueues = 8;
// Smoothing of the submits-per-frame average that picks the main queue: 1/16 per frame.
inline constexpr double reflexDynamicSubmitsSmoothing = 1.0 / 16.0;

// Diagnostics only, for the per-second pacing summary. Running: the last interval() call was active (the setting, Reflex
// low latency mode and a cap) and a main-queue frame came within the last second. Interval: the last interval it
// returned. Wait: the last window's median queue wait.
inline std::atomic<bool> reflexDynamicRunning{false};
inline std::atomic<uint32_t> reflexDynamicIntervalUs{0};
inline std::atomic<uint32_t> reflexDynamicWaitUs{0};

// CLOCK_MONOTONIC-based steady clock in nanoseconds: the clock of the submit times the probe records and of every
// present or request that asks the controller.
[[nodiscard]] inline int64_t reflexClockNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The per-device controller. The probes of a device's queues feed it from submit threads, and the present path and
// the application's requests read it. One lock guards the state; no lock is held across a Vulkan call.
class ReflexDynamicCap {
public:
    // One resolved frame of an application queue with available timestamps. `waitNs` is its GPU start less its first
    // submit, clamped at zero (a calibration error can put the start a little before the submit). `lastSubmitNs` is
    // the CPU time of its last submit, on the steady clock of reflexClockNs().
    void feed(const uintptr_t queue, const uint64_t submits, const int64_t waitNs, const int64_t lastSubmitNs) {
        const std::lock_guard lock(this->mutex);
        this->observeQueue(queue, submits);
        if (queue != this->mainQueue) return;
        this->lastSampleNs = lastSubmitNs;
        this->window[this->windowCount++] = std::max<int64_t>(waitNs, 0);
        if (this->windowCount < reflexDynamicWindowFrames) return;
        this->windowCount = 0;
        std::array<int64_t, reflexDynamicWindowFrames> sorted = this->window;
        std::sort(sorted.begin(), sorted.end());
        const auto middle = reflexDynamicWindowFrames / 2;
        const double medianUs = static_cast<double>(sorted[middle - 1] + sorted[middle]) / 2000.0;
        reflexDynamicWaitUs.store(static_cast<uint32_t>(medianUs), std::memory_order_relaxed);
        this->step(medianUs);
    }

    // The interval the driver should hold at `nowNs` for the static interval `floorUs` (zero for no cap). `active` is
    // the setting with Reflex low latency mode and a cap. When it is false, or no main-queue frame came within the
    // idle limit, the controller resets to the static interval. The result is never below `floorUs`.
    [[nodiscard]] uint32_t interval(const int64_t nowNs, const uint32_t staticUs, const bool active) {
        const std::lock_guard lock(this->mutex);
        this->floorUs = static_cast<double>(staticUs);
        const bool fresh = this->lastSampleNs != 0 && nowNs - this->lastSampleNs <= reflexDynamicIdleNs;
        if (!active || !fresh) {
            this->intervalUs = this->floorUs;
            this->windowCount = 0;
            reflexDynamicRunning.store(false, std::memory_order_relaxed);
            reflexDynamicIntervalUs.store(staticUs, std::memory_order_relaxed);
            return staticUs;
        }
        // A static interval that rose past the controller's snaps the controller up to it.
        this->intervalUs = std::max(this->intervalUs, this->floorUs);
        const auto output = static_cast<uint32_t>(std::max(this->floorUs, std::ceil(this->intervalUs)));
        reflexDynamicRunning.store(true, std::memory_order_relaxed);
        reflexDynamicIntervalUs.store(output, std::memory_order_relaxed);
        return output;
    }

private:
    struct QueueAverage {
        uintptr_t queue{};
        double submits{};
    };

    // Keeps each queue's average of submits per frame, and the main queue: the highest average. The current main queue
    // keeps the role on a tie. A change of the main queue starts a new window.
    void observeQueue(const uintptr_t queue, const uint64_t submits) {
        size_t index = 0;
        while (index < this->queueCount && this->queues[index].queue != queue) ++index;
        if (index < this->queueCount) {
            this->queues[index].submits += (static_cast<double>(submits) - this->queues[index].submits) *
                reflexDynamicSubmitsSmoothing;
        } else {
            if (this->queueCount < reflexDynamicMaximumQueues) {
                index = this->queueCount++;
            } else {
                index = 0;
                for (size_t candidate = 1; candidate < this->queueCount; ++candidate)
                    if (this->queues[candidate].submits < this->queues[index].submits) index = candidate;
            }
            this->queues[index] = QueueAverage{.queue = queue, .submits = static_cast<double>(submits)};
        }
        size_t best = 0;
        for (size_t candidate = 1; candidate < this->queueCount; ++candidate)
            if (this->queues[candidate].submits > this->queues[best].submits) best = candidate;
        size_t current = 0;
        while (current < this->queueCount && this->queues[current].queue != this->mainQueue) ++current;
        if (current < this->queueCount && this->queues[current].submits >= this->queues[best].submits) return;
        if (this->queues[best].queue != this->mainQueue) {
            this->mainQueue = this->queues[best].queue;
            this->windowCount = 0;
        }
    }

    // One window's step on its median wait in microseconds. The interval stays within [floor, maximum], where a floor
    // above the maximum (a cap under 20 fps) is its own maximum.
    void step(const double waitUs) {
        // No static interval seen yet (no present has asked): there is nothing to move from.
        if (this->floorUs <= 0.0) return;
        double next = this->intervalUs;
        if (waitUs > reflexDynamicRaiseAboveUs) {
            next += std::min(reflexDynamicRaiseGain * (waitUs - reflexDynamicRaiseTargetUs),
                reflexDynamicRaiseLimit * next);
        } else if (waitUs < reflexDynamicDecayBelowUs) {
            next -= reflexDynamicDecay * next;
        }
        const double upper = std::max(this->floorUs, reflexDynamicMaximumUs);
        this->intervalUs = std::clamp(next, this->floorUs, upper);
    }

    std::mutex mutex;
    std::array<QueueAverage, reflexDynamicMaximumQueues> queues{};
    size_t queueCount{};
    uintptr_t mainQueue{};
    std::array<int64_t, reflexDynamicWindowFrames> window{};
    size_t windowCount{};
    int64_t lastSampleNs{};
    double floorUs{};
    double intervalUs{};
};

// The controller of each device: shared by the device's GQ1 probes and by its swapchains' present paths, created by
// whichever asks first and erased at vkDestroyDevice. It is keyed by device, not by swapchain, because the probe knows
// the device and its queues but not the swapchains. A device with several swapchains shares one controller; they all
// wait on the same queues, so the state is the game's queue wait either way. The registry is leaked on purpose, as the
// probe registry is: submits may race static destruction at exit.
class ReflexDynamicCapRegistry {
public:
    [[nodiscard]] static std::shared_ptr<ReflexDynamicCap> get(const VkDevice device) {
        auto& self = state();
        const std::lock_guard lock(self.mutex);
        auto& entry = self.entries[device];
        if (!entry) entry = std::make_shared<ReflexDynamicCap>();
        return entry;
    }
    static void erase(const VkDevice device) {
        auto& self = state();
        const std::lock_guard lock(self.mutex);
        self.entries.erase(device);
    }

private:
    struct State {
        std::mutex mutex;
        std::unordered_map<VkDevice, std::shared_ptr<ReflexDynamicCap>> entries;
    };
    static State& state() {
        static auto* value = new State();
        return *value;
    }
};

}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "swapchain/reflex_frame_limit.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <system_error>
#include <thread>
#include <utility>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

// RT1: the driver's render queue wait of a Reflex frame, measured from its reports, decides an extra wait after each
// Reflex sleep. The game's next frame then starts with the GPU queue nearly empty. The extra wait is a closed loop on the
// median queued time: it moves a quarter of the median's distance from 300 us per observation, stays within [0, 4 ms],
// and is at most a third of the output period. Pure; no Vulkan. Each member is guarded by its own mutex.
class ReflexQueueDrain {
public:
    using Duration = std::chrono::nanoseconds;
    static constexpr Duration target = std::chrono::microseconds{300};
    static constexpr Duration limit = std::chrono::microseconds{4000};
    static constexpr Duration stale = std::chrono::seconds{1};
    // gain 0.25: the extra moves by a quarter of the median's error.
    static constexpr int64_t gainDivisor = 4;

    // The output period in microseconds, from the Reflex minimum interval (0: no bound beyond limit).
    void setPeriodUs(const uint32_t periodUs) noexcept {
        const std::lock_guard lock(this->mutex);
        this->periodUs = periodUs;
    }

    // One group of queued times in microseconds, observed at `now` (nanoseconds on the steady clock). An empty group
    // changes nothing. Each sample is clamped at zero; at most 64 are used.
    void observe(std::span<const int64_t> queuedUs, const Duration now) noexcept {
        if (queuedUs.empty()) return;
        std::array<int64_t, maxSamples> sorted{};
        const size_t count = std::min(queuedUs.size(), maxSamples);
        for (size_t index = 0; index < count; ++index) sorted[index] = std::max<int64_t>(queuedUs[index], 0);
        std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count));
        const int64_t medianUs = count % 2 != 0 ? sorted[count / 2]
            : (sorted[count / 2 - 1] + sorted[count / 2]) / 2;
        const std::lock_guard lock(this->mutex);
        const Duration error = std::chrono::microseconds{medianUs} - target;
        this->wait = std::clamp(this->wait + error / gainDivisor, Duration{}, bound());
        this->last = now;
        this->observed = true;
    }

    // The extra wait after a Reflex sleep at `now`: zero when nothing was observed within `stale`.
    [[nodiscard]] Duration extra(const Duration now) const noexcept {
        const std::lock_guard lock(this->mutex);
        if (!this->observed || now - this->last > stale) return Duration{};
        return std::min(this->wait, bound());
    }

    // Zeroes everything, the period included.
    void reset() noexcept {
        const std::lock_guard lock(this->mutex);
        this->wait = {};
        this->last = {};
        this->observed = false;
        this->periodUs = 0;
    }

private:
    static constexpr size_t maxSamples = 64;
    // Caller holds the mutex.
    [[nodiscard]] Duration bound() const noexcept {
        if (this->periodUs == 0) return limit;
        return std::min(limit, Duration{std::chrono::microseconds{static_cast<int64_t>(this->periodUs / 3)}});
    }
    mutable std::mutex mutex;
    Duration wait{};
    Duration last{};
    bool observed{};
    uint32_t periodUs{};
};

// The lower device's semaphore functions a relay calls, looked up with the lower GetDeviceProcAddr (never a layer
// wrapper). Tests pass fakes.
struct ReflexSemaphoreFunctions {
    PFN_vkCreateSemaphore create{};
    PFN_vkDestroySemaphore destroy{};
    PFN_vkWaitSemaphores wait{};
    PFN_vkGetSemaphoreCounterValue counter{};
    PFN_vkSignalSemaphore signal{};

    [[nodiscard]] bool complete() const noexcept {
        return create && destroy && wait && counter && signal;
    }
};

// RT1, one per managed swapchain. A Reflex sleep that the drain extends is made with the relay's private timeline
// semaphore as its signal, so the driver's sleep does not release the application's value. A helper thread waits for
// that private value, sleeps the extra wait, then signals the application's value. The game thread never waits on a
// semaphore. Stopping flushes every queued job at once (no extra wait, one private wait each), so no application value
// is left unsignalled.
class ReflexSleepRelay {
public:
    using Duration = ReflexQueueDrain::Duration;
    using Clock = std::chrono::steady_clock;
    static constexpr size_t capacity = 64;

    ReflexQueueDrain drain;

    ReflexSleepRelay() = default;
    ReflexSleepRelay(const ReflexSleepRelay&) = delete;
    ReflexSleepRelay& operator=(const ReflexSleepRelay&) = delete;
    ~ReflexSleepRelay() { stop(); }

    // Records the lower device and its functions. Idempotent; a function set that is not complete attaches nothing.
    void attach(const VkDevice lowerDevice, const ReflexSemaphoreFunctions& lowerFunctions) {
        const std::lock_guard lock(this->mutex);
        if (this->attachedDevice || !lowerFunctions.complete()) return;
        this->device = lowerDevice;
        this->functions = lowerFunctions;
        this->attachedDevice = true;
    }

    [[nodiscard]] bool attached() const {
        const std::lock_guard lock(this->mutex);
        return this->attachedDevice;
    }

    [[nodiscard]] bool belongsTo(const VkDevice lowerDevice) const {
        const std::lock_guard lock(this->mutex);
        return this->attachedDevice && this->device == lowerDevice;
    }

    // Makes one application sleep as the substituted call, on the game thread. `lowerSleep` is called once with the
    // application's sleep, its signal replaced by the private semaphore and value, and returns the driver's result.
    // On VK_SUCCESS the application's value is queued to follow the private value plus `extra`. Returns nullopt, with no
    // driver call, when the relay cannot substitute. Otherwise it returns the driver's result: the caller forwards the
    // application's original sleep unless that result is VK_SUCCESS.
    template<class LowerSleep>
    [[nodiscard]] std::optional<VkResult> sleep(const VkLatencySleepInfoNV& application, const Duration extra,
            LowerSleep&& lowerSleep) {
        const std::lock_guard lock(this->mutex);
        if (!this->attachedDevice || this->stopping || this->count == capacity ||
                application.signalSemaphore == VK_NULL_HANDLE || !ensureLocked())
            return std::nullopt;
        VkLatencySleepInfoNV substituted = application;
        substituted.signalSemaphore = this->semaphore;
        substituted.value = this->counter + 1;
        const VkResult result = lowerSleep(static_cast<const VkLatencySleepInfoNV&>(substituted));
        if (result != VK_SUCCESS) return result;
        this->counter = substituted.value;
        this->jobs[(this->head + this->count) % capacity] = Job{
            .application = application.signalSemaphore,
            .applicationValue = application.value,
            .privateValue = substituted.value,
            .extra = extra,
        };
        ++this->count;
        this->changed.notify_all();
        return VK_SUCCESS;
    }

    // True while an application value queued by `sleep` is not yet signalled. The caller keeps routing sleeps through
    // the relay (with no extra wait) while busy: a direct sleep could let the driver signal a later application value
    // before the helper signals an earlier one, and a host signal must never go backwards on a timeline.
    [[nodiscard]] bool busy() const {
        const std::lock_guard lock(this->mutex);
        return this->count != 0 || this->active != 0;
    }

    // Stops the helper after it finishes its queue and joins it. The private semaphore is destroyed only once the
    // driver has signalled every private value it was given (waiting up to one second); otherwise it is left alive,
    // since destroying a semaphore with a pending driver signal is invalid. Idempotent. Called before the lower
    // swapchain or device is destroyed.
    void stop() {
        std::thread joining;
        {
            const std::lock_guard lock(this->mutex);
            this->stopping = true;
            joining = std::move(this->thread);
        }
        this->changed.notify_all();
        if (joining.joinable()) joining.join();
        {
            const std::lock_guard lock(this->mutex);
            if (this->semaphore != VK_NULL_HANDLE) {
                const VkSemaphoreWaitInfo info{
                    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .semaphoreCount = 1,
                    .pSemaphores = &this->semaphore,
                    .pValues = &this->counter,
                };
                const VkResult settled = this->counter == 0 ? VK_SUCCESS
                    : this->functions.wait(this->device, &info, settleNs);
                if (settled == VK_SUCCESS) this->functions.destroy(this->device, this->semaphore, nullptr);
                else logError(settled);
                this->semaphore = VK_NULL_HANDLE;
            }
        }
        this->drain.reset();
    }

private:
    struct Job {
        VkSemaphore application{};
        uint64_t applicationValue{};
        uint64_t privateValue{};
        Duration extra{};
    };
    static constexpr uint64_t waitSliceNs = 100'000'000;
    static constexpr uint64_t settleNs = 1'000'000'000;

    // Caller holds the mutex. Creates the private semaphore and starts the helper once; false when either fails.
    bool ensureLocked() {
        if (this->semaphore == VK_NULL_HANDLE) {
            const VkSemaphoreTypeCreateInfo type{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                .pNext = nullptr,
                .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                .initialValue = 0,
            };
            const VkSemaphoreCreateInfo create{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                .pNext = &type,
                .flags = 0,
            };
            VkSemaphore created = VK_NULL_HANDLE;
            const VkResult result = this->functions.create(this->device, &create, nullptr, &created);
            if (result != VK_SUCCESS || created == VK_NULL_HANDLE) {
                logError(result);
                return false;
            }
            this->semaphore = created;
        }
        if (!this->thread.joinable()) {
            try {
                this->thread = std::thread([this] { run(); });
            } catch (const std::system_error&) {
                return false;
            }
        }
        return true;
    }

    // The helper: jobs in FIFO order. It returns once stop was requested and the queue is empty.
    void run() {
        for (;;) {
            Job job{};
            {
                std::unique_lock lock(this->mutex);
                this->changed.wait(lock, [this] { return this->stopping || this->count != 0; });
                if (this->count == 0) return;
                job = this->jobs[this->head];
                this->head = (this->head + 1) % capacity;
                --this->count;
                ++this->active;
            }
            finish(job);
            const std::lock_guard lock(this->mutex);
            --this->active;
        }
    }

    // Waits for the private value in slices, checking the stop flag between them. After a stop request, one slice is
    // the most a job waits. A successful wait sleeps the extra (not after a stop). An error signals at once.
    void finish(const Job& job) {
        VkResult status = VK_SUCCESS;
        for (;;) {
            const VkSemaphoreWaitInfo info{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                .pNext = nullptr,
                .flags = 0,
                .semaphoreCount = 1,
                .pSemaphores = &this->semaphore,
                .pValues = &job.privateValue,
            };
            status = this->functions.wait(this->device, &info, waitSliceNs);
            if (status != VK_TIMEOUT || stopRequested()) break;
        }
        if (status == VK_SUCCESS && job.extra > Duration{} && !stopRequested()) {
            pauseUntil(Clock::now() + std::chrono::duration_cast<Clock::duration>(job.extra));
        } else if (status != VK_SUCCESS && status != VK_TIMEOUT) {
            logError(status);
        }
        signalApplication(job);
    }

    // Signals the application's value unless the semaphore already reached it. A counter error signals anyway.
    void signalApplication(const Job& job) {
        uint64_t current = 0;
        const VkResult counted = this->functions.counter(this->device, job.application, &current);
        if (counted != VK_SUCCESS) logError(counted);
        if (counted == VK_SUCCESS && current >= job.applicationValue) return;
        const VkSemaphoreSignalInfo info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
            .pNext = nullptr,
            .semaphore = job.application,
            .value = job.applicationValue,
        };
        const VkResult signalled = this->functions.signal(this->device, &info);
        if (signalled != VK_SUCCESS) logError(signalled);
    }

    // Sleeps until `until` unless a stop is requested first.
    void pauseUntil(const Clock::time_point until) {
        std::unique_lock lock(this->mutex);
        this->changed.wait_until(lock, until, [this] { return this->stopping; });
    }

    [[nodiscard]] bool stopRequested() {
        const std::lock_guard lock(this->mutex);
        return this->stopping;
    }

    // Logged once per process.
    static void logError(const VkResult result) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true, std::memory_order_relaxed))
            std::cerr << "framegen-vulkan: reflex queue drain error " << static_cast<int>(result) << '\n';
    }

    mutable std::mutex mutex;
    std::condition_variable changed;
    VkDevice device{};
    ReflexSemaphoreFunctions functions{};
    bool attachedDevice{};
    VkSemaphore semaphore{VK_NULL_HANDLE};
    uint64_t counter{};
    std::thread thread;
    bool stopping{};
    std::array<Job, capacity> jobs{};
    size_t head{};
    size_t count{};
    size_t active{};
};

using ReflexSleepRelayRegistry = ReflexSwapchainRegistry<ReflexSleepRelay>;

// Stops and removes a swapchain's relay. Called where the other Reflex registries are erased.
inline void retireReflexSleepRelay(const VkSwapchainKHR swapchain) {
    if (const auto relay = ReflexSleepRelayRegistry::find(swapchain)) relay->stop();
    ReflexSleepRelayRegistry::erase(swapchain);
}

// Stops and removes every relay of a device being destroyed. Applications destroy their swapchains first, so this only
// matters for a device destroyed with its swapchains alive.
inline void retireReflexSleepRelays(const VkDevice device) {
    for (const auto& [swapchain, relay] : ReflexSleepRelayRegistry::snapshot()) {
        if (!relay->belongsTo(device)) continue;
        relay->stop();
        ReflexSleepRelayRegistry::erase(swapchain);
    }
}

// FGVK_REFLEX_QUEUE_DRAIN=1 turns the queue drain on and =0 turns it off. Any other value, or unset, leaves the
// [global] reflex_queue_drain setting in charge. Read once.
[[nodiscard]] inline int reflexQueueDrainEnvironment() noexcept {
    static const int fromEnvironment = [] {
        const char* text = std::getenv("FGVK_REFLEX_QUEUE_DRAIN");
        if (!text) return -1;
        if (std::strcmp(text, "1") == 0) return 1;
        if (std::strcmp(text, "0") == 0) return 0;
        return -1;
    }();
    return fromEnvironment;
}

// The low bit is the setting. The whole value lets a reader detect even a pair of changes.
inline std::atomic<uint64_t> reflexQueueDrainConfiguration{0};
inline void setReflexQueueDrainConfigured(const bool enabled) noexcept {
    auto state = reflexQueueDrainConfiguration.load(std::memory_order_relaxed);
    while ((state & 1) != enabled && !reflexQueueDrainConfiguration.compare_exchange_weak(
            state, state + 1, std::memory_order_relaxed)) {}
}

[[nodiscard]] inline uint64_t reflexQueueDrainState() noexcept {
    const auto override = reflexQueueDrainEnvironment();
    return override < 0 ? reflexQueueDrainConfiguration.load(std::memory_order_relaxed)
                        : static_cast<uint64_t>(override);
}

[[nodiscard]] inline bool reflexQueueDrainOn() noexcept {
    return (reflexQueueDrainState() & 1) != 0;
}

// The steady clock as the drain's `now`.
[[nodiscard]] inline ReflexQueueDrain::Duration reflexQueueDrainNow() noexcept {
    return std::chrono::duration_cast<ReflexQueueDrain::Duration>(
        std::chrono::steady_clock::now().time_since_epoch());
}

// Diagnostics only: whether the drain was in effect at the most recent sleep, and that sleep's extra wait in
// nanoseconds. The per-second pacing summary prints them as reflex_drain= and reflex_drain_us=.
inline std::atomic<bool> reflexQueueDrainActive{false};
inline std::atomic<int64_t> reflexQueueDrainNs{0};

}

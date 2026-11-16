/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

// GP1: the watcher of one active lower swapchain. The worker and the synchronous present paths push
// each successful lower present ID onto a bounded ring; one thread (fgvk-onscreen) waits each ID in
// order through the next layer's vkWaitForPresent2KHR and emits when it completed. The push is O(1),
// never waits for the thread, and never allocates. The wait function and the emitter are injected,
// so the tests run without a driver.

#include "swapchain/onscreen_ids.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <unistd.h>
#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

// One finished on-screen wait. Times are steady-clock nanoseconds, the clock of the output records.
struct OnScreenRecord {
    uint64_t context{};
    uint64_t lowerId{};
    VkResult result{VK_SUCCESS};
    int64_t waitEnteredNs{};
    int64_t doneNs{};
    uint64_t dropped{};
};

// One pacing-profile line for a record (the `framegen-vulkan:` prefix is the record writer's).
[[nodiscard]] inline std::string formatOnScreenRecord(const OnScreenRecord& record, const long pid) {
    std::ostringstream line;
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-onscreen version=1"
         << " context=" << record.context << " pid=" << pid << " lower_present_id=" << record.lowerId
         << " result=" << static_cast<int32_t>(record.result) << " wait_entered_ns=" << record.waitEnteredNs
         << " done_ns=" << record.doneNs << " dropped=" << record.dropped << '\n';
    return line.str();
}

class OnScreenWatch {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr size_t capacity = 512;
    // One wait per slice; timeouts are retried until the budget (1 s in total) is spent.
    static constexpr uint64_t sliceNs = 100'000'000;
    static constexpr uint64_t budgetNs = 1'000'000'000;

    // wait(lowerId, timeoutNs) is the next layer's vkWaitForPresent2KHR for this swapchain.
    using Wait = std::function<VkResult(uint64_t lowerId, uint64_t timeoutNs)>;
    using Emit = std::function<void(const OnScreenRecord& record)>;

    // Starts the thread. It may throw std::system_error when the thread cannot start.
    OnScreenWatch(Wait wait, Emit emit);
    ~OnScreenWatch() { stop(); }
    OnScreenWatch(const OnScreenWatch&) = delete;
    OnScreenWatch& operator=(const OnScreenWatch&) = delete;

    // Worker push: O(1), never waits for the thread, never allocates. A full ring drops the entry
    // and counts it. Nothing is queued once stop() or an error has ended the waits.
    void push(const uint64_t lowerId, const uint64_t context) noexcept {
        {
            const std::lock_guard lock(mutex);
            if (stopping || dead) return;
            if (count == capacity) {
                ++dropped;
                return;
            }
            ring[(head + count) % capacity] = Entry{lowerId, context};
            ++count;
        }
        changed.notify_one();
    }

    // Sets the stop flag, lets the wait in progress end (at most one slice), and joins the thread.
    // Idempotent. After it returns no wait of this watcher is in flight.
    void stop() noexcept;

    // True once a wait returned VK_SUCCESS or VK_SUBOPTIMAL_KHR: present feedback works on this
    // swapchain. Stands in for the feedback probe, which does not tag while the gate is active.
    [[nodiscard]] bool confirmed() const noexcept { return sawSuccess.load(std::memory_order_acquire); }

    [[nodiscard]] uint64_t droppedCount() const noexcept {
        const std::lock_guard lock(mutex);
        return dropped;
    }

private:
    struct Entry {
        uint64_t lowerId{};
        uint64_t context{};
    };
    void run();
    std::atomic_bool sawSuccess{false};
    [[nodiscard]] bool stopRequested() const noexcept {
        const std::lock_guard lock(mutex);
        return stopping;
    }

    Wait wait;
    Emit emit;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::array<Entry, capacity> ring{};
    size_t head{};
    size_t count{};
    uint64_t dropped{};
    bool stopping{};
    // An error (out of date, surface lost or any other) ended the waits: entries are discarded.
    bool dead{};
    std::thread thread;
};

// GP1: one active lower swapchain's on-screen profile. The ID owner is registered for application
// waits until stop(). stop() unregisters, then joins the watcher; it is called before the lower
// swapchain is destroyed on every path, and the destructor calls it too.
class OnScreenProfile {
public:
    OnScreenProfile(const uint64_t device, const uint64_t swapchain, const uint64_t context,
            OnScreenWatch::Wait wait, OnScreenWatch::Emit emit) :
            key{device, swapchain}, ids(std::make_shared<OnScreenIds>()),
            watcher(std::make_unique<OnScreenWatch>(std::move(wait), std::move(emit))),
            contextId(context) {
        OnScreenRegistry::add(key, ids);
    }
    ~OnScreenProfile() { stop(); }
    OnScreenProfile(const OnScreenProfile&) = delete;
    OnScreenProfile& operator=(const OnScreenProfile&) = delete;

    void stop() noexcept {
        OnScreenRegistry::remove(key);
        watcher->stop();
    }
    [[nodiscard]] OnScreenIds& lowerIds() noexcept { return *ids; }
    [[nodiscard]] OnScreenWatch& watch() noexcept { return *watcher; }
    [[nodiscard]] uint64_t context() const noexcept { return contextId; }

private:
    OnScreenRegistry::Key key;
    std::shared_ptr<OnScreenIds> ids;
    std::unique_ptr<OnScreenWatch> watcher;
    uint64_t contextId{};
};

} // namespace fgvk::layer

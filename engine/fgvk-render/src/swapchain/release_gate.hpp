/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

// Moves a source-cap deadline from the application's vkQueuePresentKHR to the
// point where the application releases its producer.
//
// VKD3D-Proton runs Present on its D3D12 direct-queue submission thread. After
// an unlocked present (no present ID) it submits one signal-only timeline value
// on the same queue and thread, and a separate wait thread waits for that value
// before it releases the game's frame-latency event. Holding the present call
// for a cap therefore stalls every later ExecuteCommandLists, Signal and Wait
// on that D3D12 queue. VKD3D's own limiter sleeps on its wait thread instead.
//
// The gate records the cap deadline against the first signal-only submit on
// the presenting thread after a present, and delays the application's
// successful wait for that value until the deadline. A swapchain owner only
// stops holding its present after such waits have been observed; an
// application that never waits keeps the ordinary present hold.
//
// A present that carries an application present ID (VKD3D-Proton with VSync on)
// is released the same way through that ID: the token is the present itself, and
// the application's successful wait on the ID (its frame-latency thread) is held to
// the deadline. Tokens of the two kinds never match each other.
class ReleaseGate {
public:
    using Clock = std::chrono::steady_clock;

    struct Owner {
        std::atomic_bool alive{true};
        std::atomic<uint32_t> consumedRun{0};
        std::atomic<uint32_t> outstanding{0};

        // Two consecutive consumed tokens with a bounded backlog show that
        // the application releases its producer through this wait.
        [[nodiscard]] bool active() const noexcept {
            return alive.load(std::memory_order_relaxed) &&
                consumedRun.load(std::memory_order_relaxed) >= activationRun &&
                outstanding.load(std::memory_order_relaxed) <= maxOutstanding;
        }
    };

    static constexpr uint32_t activationRun = 2;
    // Default D3D12 latency queues three frames; allow scheduling jitter.
    static constexpr uint32_t maxOutstanding = 6;
    static constexpr size_t capacity = 32;
    // Bounds teardown/drain waits and stale handle reuse.
    static constexpr Clock::duration maxDelay = std::chrono::milliseconds{250};
    static constexpr Clock::duration maxAge = std::chrono::seconds{1};

    struct Release {
        Clock::time_point deadline{};
        // False when the wait's remaining timeout ends before the deadline:
        // the token stays registered and the caller reports VK_TIMEOUT.
        bool consumed{};
    };

    static ReleaseGate& global() {
        static ReleaseGate gate;
        return gate;
    }

    // Presenting thread only. The next submit on this thread consumes it.
    static void arm(std::shared_ptr<Owner> owner, VkQueue queue, Clock::time_point deadline) {
        auto& slot = pending();
        slot.owner = std::move(owner);
        slot.queue = queue;
        slot.deadline = deadline;
    }
    static void disarm() noexcept { pending().owner.reset(); }
    [[nodiscard]] static bool armed() noexcept { return static_cast<bool>(pending().owner); }

    // Called for every application vkQueueSubmit2. The unarmed path is one
    // thread-local load. Any submit consumes the arm: only the submit that
    // immediately follows the present can carry its release token.
    void observeSubmit2(VkQueue queue, uint32_t submitCount,
            const VkSubmitInfo2* submits, VkFence fence, Clock::time_point now = Clock::now()) {
        if (!armed()) return;
        auto slot = std::move(pending());
        pending() = {};
        if (queue != slot.queue || submitCount != 1 || !submits || fence != VK_NULL_HANDLE)
            return;
        const auto& submit = submits[0];
        if (submit.commandBufferInfoCount != 0 || submit.waitSemaphoreInfoCount != 0 ||
                submit.signalSemaphoreInfoCount != 1 || !submit.pSignalSemaphoreInfos)
            return;
        const auto& signal = submit.pSignalSemaphoreInfos[0];
        if (!signal.semaphore || signal.value == 0) return;
        record(Kind::Semaphore, reinterpret_cast<uint64_t>(signal.semaphore), signal.value,
            slot.deadline, std::move(slot.owner), now);
    }

    // A present with a non-zero application present ID: the token is that ID on
    // the swapchain. The caller records it when the present returns, so the
    // application's own wait for the ID is the release wait.
    void recordPresentId(std::shared_ptr<Owner> owner, VkSwapchainKHR swapchain, uint64_t presentId,
            Clock::time_point deadline, Clock::time_point now = Clock::now()) {
        record(Kind::PresentId, reinterpret_cast<uint64_t>(swapchain), presentId, deadline,
            std::move(owner), now);
    }

    [[nodiscard]] bool empty() const noexcept {
        return count.load(std::memory_order_acquire) == 0;
    }

    // After a successful wait-all on these semaphores. `limit` is the end of
    // the caller's timeout. Returns the latest pending deadline among tokens
    // the wait satisfied, or nothing when no live token is affected.
    [[nodiscard]] std::optional<Release> released(const VkSemaphoreWaitInfo& info,
            Clock::time_point now, Clock::time_point limit) {
        if (empty() || (info.flags & VK_SEMAPHORE_WAIT_ANY_BIT) ||
                !info.pSemaphores || !info.pValues)
            return std::nullopt;
        return releaseSatisfied(now, limit, [&](const Entry& entry) {
            if (entry.kind != Kind::Semaphore) return false;
            for (uint32_t i = 0; i < info.semaphoreCount; ++i)
                if (reinterpret_cast<uint64_t>(info.pSemaphores[i]) == entry.key &&
                        entry.value <= info.pValues[i])
                    return true;
            return false;
        });
    }

    // After a successful wait on a present ID of swapchain. The wait satisfies
    // every present ID token on that swapchain at or below presentId.
    [[nodiscard]] std::optional<Release> releasedPresent(VkSwapchainKHR swapchain, uint64_t presentId,
            Clock::time_point now, Clock::time_point limit) {
        if (empty()) return std::nullopt;
        const auto key = reinterpret_cast<uint64_t>(swapchain);
        return releaseSatisfied(now, limit, [&](const Entry& entry) {
            return entry.kind == Kind::PresentId && entry.key == key && entry.value <= presentId;
        });
    }

    // Present ID tokens consumed by the application's waits, process-wide.
    [[nodiscard]] uint64_t presentIdReleases() const noexcept {
        return presentIdConsumed.load(std::memory_order_relaxed);
    }

    // Swapchain teardown: forget its tokens so drains never sleep for it.
    void forget(const Owner& owner) {
        const std::lock_guard lock(mutex);
        std::erase_if(entries, [&](const Entry& entry) {
            const auto live = entry.owner.lock();
            return !live || live.get() == &owner;
        });
        count.store(entries.size(), std::memory_order_release);
    }

private:
    enum class Kind : uint8_t { Semaphore, PresentId };
    struct Entry {
        uint64_t key{};
        uint64_t value{};
        Clock::time_point deadline{}, registered{};
        std::weak_ptr<Owner> owner;
        Kind kind{Kind::Semaphore};
    };
    struct Pending {
        std::shared_ptr<Owner> owner;
        VkQueue queue{};
        Clock::time_point deadline{};
    };
    static Pending& pending() noexcept {
        static thread_local Pending slot;
        return slot;
    }

    // Owner counters change only under the registry mutex; atomics let the
    // presenting thread read active() without taking it.
    static void release(Owner& owner) noexcept {
        const auto value = owner.outstanding.load(std::memory_order_relaxed);
        owner.outstanding.store(value ? value - 1 : 0, std::memory_order_relaxed);
    }
    void eraseEntry(std::vector<Entry>::iterator entry) {
        if (const auto owner = entry->owner.lock()) release(*owner);
        entries.erase(entry);
    }

    // Consumes every live token that satisfied(entry) accepts. Returns the latest
    // deadline among them, or nothing when none is affected. A deadline after limit
    // keeps the tokens and reports it unconsumed.
    template<typename Satisfied>
    std::optional<Release> releaseSatisfied(Clock::time_point now, Clock::time_point limit,
            Satisfied&& satisfied) {
        const std::lock_guard lock(mutex);
        std::optional<Clock::time_point> latest;
        for (const auto& entry : entries) {
            if (!satisfied(entry) || now - entry.registered > maxAge) continue;
            const auto owner = entry.owner.lock();
            if (!owner || !owner->alive.load(std::memory_order_relaxed)) continue;
            const auto deadline = std::min(entry.deadline, now + maxDelay);
            latest = latest ? std::max(*latest, deadline) : deadline;
        }
        if (latest && *latest > limit)
            return Release{*latest, false};
        bool any = false;
        std::erase_if(entries, [&](const Entry& entry) {
            if (!satisfied(entry)) return false;
            if (const auto owner = entry.owner.lock()) {
                release(*owner);
                owner->consumedRun.fetch_add(1, std::memory_order_relaxed);
            }
            if (entry.kind == Kind::PresentId)
                presentIdConsumed.fetch_add(1, std::memory_order_relaxed);
            any = true;
            return true;
        });
        count.store(entries.size(), std::memory_order_release);
        if (!any) return std::nullopt;
        return latest ? std::optional{Release{*latest, true}} : std::nullopt;
    }

    void record(Kind kind, uint64_t key, uint64_t value, Clock::time_point deadline,
            std::shared_ptr<Owner> owner, Clock::time_point now) {
        if (!owner) return;
        const std::lock_guard lock(mutex);
        // A value at or below an existing token of the same kind and handle means the
        // handle was reused or the counter restarted; older tokens can no longer be matched.
        for (auto entry = entries.begin(); entry != entries.end();) {
            if ((entry->kind == kind && entry->key == key && entry->value >= value) ||
                    now - entry->registered > maxAge) {
                const auto offset = entry - entries.begin();
                eraseEntry(entry);
                entry = entries.begin() + offset;
            } else {
                ++entry;
            }
        }
        // The application stopped waiting: hold presents again and keep only
        // a bounded learning backlog.
        if (owner->outstanding.load(std::memory_order_relaxed) >= maxOutstanding) {
            owner->consumedRun.store(0, std::memory_order_relaxed);
            const auto oldest = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) {
                return entry.owner.lock() == owner;
            });
            if (oldest != entries.end()) eraseEntry(oldest);
            else owner->outstanding.store(0, std::memory_order_relaxed);
        }
        if (entries.size() >= capacity) eraseEntry(entries.begin());
        try {
            entries.push_back({key, value, deadline, now, owner, kind});
            owner->outstanding.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {}
        count.store(entries.size(), std::memory_order_release);
    }

    std::mutex mutex;
    std::vector<Entry> entries;
    std::atomic<size_t> count{0};
    std::atomic<uint64_t> presentIdConsumed{0};
};

// One swapchain context's identity in the gate. Destruction retires its
// tokens, so a later drain or reused semaphore handle never waits for it.
class ReleaseGateOwner {
public:
    ReleaseGateOwner() : owner(std::make_shared<ReleaseGate::Owner>()) {}
    ReleaseGateOwner(const ReleaseGateOwner&) = delete;
    ReleaseGateOwner& operator=(const ReleaseGateOwner&) = delete;
    ReleaseGateOwner(ReleaseGateOwner&& other) noexcept : owner(std::move(other.owner)) {}
    ReleaseGateOwner& operator=(ReleaseGateOwner&& other) noexcept {
        if (this != &other) {
            retire();
            owner = std::move(other.owner);
        }
        return *this;
    }
    ~ReleaseGateOwner() { retire(); }

    [[nodiscard]] bool active() const noexcept { return owner && owner->active(); }
    void arm(VkQueue queue, ReleaseGate::Clock::time_point deadline) const {
        if (owner) ReleaseGate::arm(owner, queue, deadline);
    }
    void recordPresentId(VkSwapchainKHR swapchain, uint64_t presentId,
            ReleaseGate::Clock::time_point deadline) const {
        if (owner) ReleaseGate::global().recordPresentId(owner, swapchain, presentId, deadline);
    }

private:
    void retire() noexcept {
        if (!owner) return;
        owner->alive.store(false, std::memory_order_relaxed);
        try { ReleaseGate::global().forget(*owner); } catch (...) {}
        owner.reset();
    }
    std::shared_ptr<ReleaseGate::Owner> owner;
};

// A present whose cap deadline lies more than `leadFrames` cap intervals after
// its arrival cannot be held by the release wait's lead bound. It sleeps until
// `leadFrames` intervals before the deadline and then counts as arriving at that
// wake time. Returns that point, or nothing when the lead is within the bound or
// the cap interval is not positive.
[[nodiscard]] inline std::optional<ReleaseGate::Clock::time_point> releaseGateCatchUp(
        ReleaseGate::Clock::time_point arrival, ReleaseGate::Clock::time_point deadline,
        ReleaseGate::Clock::duration capInterval, uint32_t leadFrames) {
    if (capInterval <= ReleaseGate::Clock::duration::zero() ||
            deadline - arrival <= leadFrames * capInterval)
        return std::nullopt;
    return deadline - leadFrames * capInterval;
}

}

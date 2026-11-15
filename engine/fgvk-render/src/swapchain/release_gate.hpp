/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>
#include <array>
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

// The GPU hold of one release token (L5, low_latency.hpp). `at` is the predicted release of the
// game's next frame (absolute). `after` is a stepping hold, measured from the first wait (or
// acquire) that finds the token satisfied, then fixed. Neither moves a release point earlier than
// its deadline.
struct GpuHold {
    std::optional<std::chrono::steady_clock::time_point> at{};
    std::chrono::steady_clock::duration after{};
};

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
//
// A present also records the timeline value of its thread's last submit on the
// presenting queue (SubmitMark). DXVK's finish thread waits for that value before it
// signals the frame, so the same hold applies (kind LastSubmit). It is recorded only
// while the application has not released through a semaphore or present ID token,
// because a VKD3D-Proton submission timeline also serves the game's own fence and
// queue-idle waits. An application that waits on no token at all releases through its
// next acquire instead (ReleaseGateOwner::acquireRelease).
//
// A token may carry a GPU hold (GpuHold, L5 in low_latency.hpp). Its release point is then the
// latest of its deadline, the predicted release `at` of the game's next frame, and the satisfying
// wait's return plus `after`. The point is bounded by maxDelay like the deadline alone.
class ReleaseGate {
public:
    using Clock = std::chrono::steady_clock;

    // Token kinds. Semaphore and LastSubmit tokens are both satisfied by a semaphore wait.
    enum class Kind : uint8_t { Semaphore, PresentId, LastSubmit };
    static constexpr uint32_t kindBit(Kind kind) noexcept {
        return uint32_t{1} << static_cast<uint32_t>(kind);
    }
    static constexpr uint32_t tokenKinds() noexcept {
        return kindBit(Kind::Semaphore) | kindBit(Kind::PresentId) | kindBit(Kind::LastSubmit);
    }
    // Owner::consumedKinds bit of the native acquire fallback having held an acquire.
    static constexpr uint32_t acquireBit = uint32_t{1} << 3;

    struct Owner {
        std::atomic_bool alive{true};
        std::atomic<uint32_t> consumedRun{0};
        std::atomic<uint32_t> outstanding{0};
        // kindBit of every token kind the application has waited on, plus acquireBit.
        std::atomic<uint32_t> consumedKinds{0};
        // Release point of the latest capped present, as Clock::rep; zero when none.
        std::atomic<Clock::rep> acquireRelease{0};
        // GPU hold of that present's acquire fallback: the absolute part and the stepping part.
        std::atomic<Clock::rep> acquireGpuAt{0};
        std::atomic<Clock::rep> acquireGpuAfter{0};
        // When the latest consumed release point returned to the application, as Clock::rep (L5
        // observation); zero once taken. The next present takes it as its frame's release time.
        std::atomic<Clock::rep> returnedAt{0};

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
        // The release point: the deadline, moved later by the token's GPU hold.
        Clock::time_point deadline{};
        // False when the wait's remaining timeout ends before the deadline:
        // the token stays registered and the caller reports VK_TIMEOUT.
        bool consumed{};
        // The owner of the token that set the point, when consumed (see noteReturned).
        std::shared_ptr<Owner> owner{};
    };

    static ReleaseGate& global() {
        static ReleaseGate gate;
        return gate;
    }

    // Presenting thread only. The next submit on this thread consumes it.
    static void arm(std::shared_ptr<Owner> owner, VkQueue queue, Clock::time_point deadline,
            GpuHold gpu = {}) {
        auto& slot = pending();
        slot.owner = std::move(owner);
        slot.queue = queue;
        slot.deadline = deadline;
        slot.gpu = gpu;
    }

    // The hook returned a consumed release point to the application at `now`, after its sleep.
    // The owner keeps that time for its next present (Owner::returnedAt).
    static void noteReturned(const Release& release, Clock::time_point now) noexcept {
        if (release.consumed && release.owner)
            release.owner->returnedAt.store(now.time_since_epoch().count(), std::memory_order_relaxed);
    }
    static void disarm() noexcept { pending().owner.reset(); }
    [[nodiscard]] static bool armed() noexcept { return static_cast<bool>(pending().owner); }

    // Called for every application vkQueueSubmit2. It stores this thread's submit
    // mark (a few stores, no lock). The unarmed path then returns after one
    // thread-local load. Any submit consumes the arm: only the submit that
    // immediately follows the present can carry its release token.
    void observeSubmit2(VkQueue queue, uint32_t submitCount,
            const VkSubmitInfo2* submits, VkFence fence, Clock::time_point now = Clock::now()) {
        noteSubmitMark(queue, submitCount, submits);
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
            slot.deadline, slot.gpu, std::move(slot.owner), now);
    }

    // A present with a non-zero application present ID: the token is that ID on
    // the swapchain. The caller records it when the present returns, so the
    // application's own wait for the ID is the release wait.
    void recordPresentId(std::shared_ptr<Owner> owner, VkSwapchainKHR swapchain, uint64_t presentId,
            Clock::time_point deadline, GpuHold gpu = {}, Clock::time_point now = Clock::now()) {
        record(Kind::PresentId, reinterpret_cast<uint64_t>(swapchain), presentId, deadline, gpu,
            std::move(owner), now);
    }

    // The timeline value of a present's last submit: the application's wait for it ends the frame.
    void recordLastSubmit(std::shared_ptr<Owner> owner, VkSemaphore semaphore, uint64_t value,
            Clock::time_point deadline, GpuHold gpu = {}, Clock::time_point now = Clock::now()) {
        record(Kind::LastSubmit, reinterpret_cast<uint64_t>(semaphore), value, deadline, gpu,
            std::move(owner), now);
    }

    [[nodiscard]] bool empty() const noexcept {
        return count.load(std::memory_order_acquire) == 0;
    }

    // The timeline signal that this thread's last submit on a queue made, since the previous
    // present on this thread. A present takes it on entry, so no present carries an
    // earlier present's submit.
    struct SubmitMark {
        VkQueue queue{};
        VkSemaphore semaphore{};
        uint64_t value{};
    };
    [[nodiscard]] static std::optional<SubmitMark> takeSubmitMark(VkQueue queue) noexcept {
        auto& marks = submitMarks();
        const auto slot = std::ranges::find(marks.slots, queue, &SubmitMark::queue);
        if (slot == marks.slots.end() || !slot->semaphore) return std::nullopt;
        const auto mark = *slot;
        *slot = {};
        return mark;
    }
    // A present that is not held by the gate (native forwarding) drops the mark it would take.
    static void forgetSubmitMark(VkQueue queue) noexcept {
        static_cast<void>(takeSubmitMark(queue));
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
            if (entry.kind == Kind::PresentId) return false;
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
    struct Entry {
        uint64_t key{};
        uint64_t value{};
        Clock::time_point deadline{}, registered{};
        GpuHold gpu{};
        std::weak_ptr<Owner> owner;
        Kind kind{Kind::Semaphore};
    };
    struct Pending {
        std::shared_ptr<Owner> owner;
        VkQueue queue{};
        Clock::time_point deadline{};
        GpuHold gpu{};
    };
    // The release point of a satisfied token at `now`: its deadline, moved later by its GPU hold.
    // A stepping hold is anchored at the first wait that finds the token satisfied: it becomes an
    // absolute point, so a caller retrying with short timeouts meets a fixed point, not one that
    // moves with every call.
    static Clock::time_point releasePoint(Entry& entry, Clock::time_point now) noexcept {
        if (entry.gpu.after > Clock::duration::zero()) {
            const auto anchored = now + entry.gpu.after;
            entry.gpu.at = entry.gpu.at ? std::max(*entry.gpu.at, anchored) : anchored;
            entry.gpu.after = Clock::duration::zero();
        }
        auto point = entry.deadline;
        if (entry.gpu.at) point = std::max(point, *entry.gpu.at);
        return std::min(point, now + maxDelay);
    }
    static Pending& pending() noexcept {
        static thread_local Pending slot;
        return slot;
    }
    // One slot per recent queue; a thread submits on few queues.
    struct SubmitMarks {
        std::array<SubmitMark, 4> slots{};
        size_t next{};
    };
    static SubmitMarks& submitMarks() noexcept {
        static thread_local SubmitMarks marks;
        return marks;
    }
    // The last signal with a non-zero value (a timeline value), or none.
    static void noteSubmitMark(VkQueue queue, uint32_t submitCount,
            const VkSubmitInfo2* submits) noexcept {
        if (submitCount == 0 || !submits) return;
        const auto& submit = submits[submitCount - 1];
        SubmitMark mark{queue};
        const uint32_t signals = submit.pSignalSemaphoreInfos ? submit.signalSemaphoreInfoCount : 0;
        for (uint32_t i = signals; i-- > 0;) {
            const auto& signal = submit.pSignalSemaphoreInfos[i];
            if (signal.semaphore && signal.value) {
                mark.semaphore = signal.semaphore;
                mark.value = signal.value;
                break;
            }
        }
        auto& marks = submitMarks();
        auto slot = std::ranges::find(marks.slots, queue, &SubmitMark::queue);
        if (slot == marks.slots.end()) {
            slot = marks.slots.begin() + static_cast<std::ptrdiff_t>(marks.next);
            marks.next = (marks.next + 1) % marks.slots.size();
        }
        *slot = mark;
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
        std::shared_ptr<Owner> latestOwner;
        for (auto& entry : entries) {
            if (!satisfied(entry) || now - entry.registered > maxAge) continue;
            const auto owner = entry.owner.lock();
            if (!owner || !owner->alive.load(std::memory_order_relaxed)) continue;
            const auto point = releasePoint(entry, now);
            if (!latest || point > *latest) {
                latest = point;
                latestOwner = owner;
            }
        }
        if (latest && *latest > limit)
            return Release{*latest, false, nullptr};
        bool any = false;
        std::erase_if(entries, [&](const Entry& entry) {
            if (!satisfied(entry)) return false;
            if (const auto owner = entry.owner.lock()) {
                release(*owner);
                owner->consumedRun.fetch_add(1, std::memory_order_relaxed);
                owner->consumedKinds.fetch_or(kindBit(entry.kind), std::memory_order_relaxed);
            }
            if (entry.kind == Kind::PresentId)
                presentIdConsumed.fetch_add(1, std::memory_order_relaxed);
            any = true;
            return true;
        });
        count.store(entries.size(), std::memory_order_release);
        if (!any) return std::nullopt;
        return latest ? std::optional{Release{*latest, true, std::move(latestOwner)}} : std::nullopt;
    }

    void record(Kind kind, uint64_t key, uint64_t value, Clock::time_point deadline, GpuHold gpu,
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
            entries.push_back({key, value, deadline, now, gpu, owner, kind});
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
    // gpu is the token's GPU hold (L5); the default keeps the release at its deadline.
    void arm(VkQueue queue, ReleaseGate::Clock::time_point deadline, GpuHold gpu = {}) const {
        if (owner) ReleaseGate::arm(owner, queue, deadline, gpu);
    }
    void recordPresentId(VkSwapchainKHR swapchain, uint64_t presentId,
            ReleaseGate::Clock::time_point deadline, GpuHold gpu = {}) const {
        if (owner) ReleaseGate::global().recordPresentId(owner, swapchain, presentId, deadline, gpu);
    }
    // A present's last submit (ReleaseGate::SubmitMark). Skipped once this application has
    // released through a semaphore or present ID token (see the header comment).
    void recordLastSubmit(VkSemaphore semaphore, uint64_t value,
            ReleaseGate::Clock::time_point deadline, GpuHold gpu = {}) const {
        constexpr uint32_t gatedBy = ReleaseGate::kindBit(ReleaseGate::Kind::Semaphore) |
            ReleaseGate::kindBit(ReleaseGate::Kind::PresentId);
        if (owner && !(owner->consumedKinds.load(std::memory_order_relaxed) & gatedBy))
            ReleaseGate::global().recordLastSubmit(owner, semaphore, value, deadline, gpu);
    }
    // Native acquire fallback: the release point of the latest capped present and its GPU hold.
    void setAcquireRelease(std::optional<ReleaseGate::Clock::time_point> release,
            GpuHold gpu = {}) const {
        if (!owner) return;
        owner->acquireRelease.store(
            release ? release->time_since_epoch().count() : 0, std::memory_order_relaxed);
        owner->acquireGpuAt.store(
            gpu.at ? gpu.at->time_since_epoch().count() : 0, std::memory_order_relaxed);
        owner->acquireGpuAfter.store(gpu.after.count(), std::memory_order_relaxed);
    }
    // The point the next acquire waits for, while no token kind has been consumed: an
    // application that releases through tokens starts its frames at their deadlines. The GPU hold
    // applies as in the gate: the predicted release, or the hold after `now` (the acquire's start).
    [[nodiscard]] std::optional<ReleaseGate::Clock::time_point> acquireRelease(
            ReleaseGate::Clock::time_point now = ReleaseGate::Clock::now()) const {
        if (!owner || (owner->consumedKinds.load(std::memory_order_relaxed) & ReleaseGate::tokenKinds()))
            return std::nullopt;
        const auto rep = owner->acquireRelease.load(std::memory_order_relaxed);
        if (rep == 0) return std::nullopt;
        auto release = ReleaseGate::Clock::time_point{ReleaseGate::Clock::duration{rep}};
        // A stepping hold is anchored at the first acquire after the present (as in the gate):
        // later acquires and retries meet the same absolute point.
        const auto after = ReleaseGate::Clock::duration{
            owner->acquireGpuAfter.exchange(0, std::memory_order_relaxed)};
        if (after > ReleaseGate::Clock::duration::zero()) {
            const auto anchored = (now + after).time_since_epoch().count();
            auto current = owner->acquireGpuAt.load(std::memory_order_relaxed);
            while (current < anchored &&
                !owner->acquireGpuAt.compare_exchange_weak(current, anchored, std::memory_order_relaxed)) {}
        }
        if (const auto gpuAt = owner->acquireGpuAt.load(std::memory_order_relaxed))
            release = std::max(release, ReleaseGate::Clock::time_point{ReleaseGate::Clock::duration{gpuAt}});
        return release;
    }
    // The acquire fallback returned to the application at `now`, after its hold.
    void noteAcquireReleased(ReleaseGate::Clock::time_point now) const {
        if (owner) owner->returnedAt.store(now.time_since_epoch().count(), std::memory_order_relaxed);
    }
    // The time the latest consumed release point returned to the application, taken once.
    [[nodiscard]] std::optional<ReleaseGate::Clock::time_point> takeReturnedAt() const {
        if (!owner) return std::nullopt;
        const auto rep = owner->returnedAt.exchange(0, std::memory_order_relaxed);
        if (rep == 0) return std::nullopt;
        return ReleaseGate::Clock::time_point{ReleaseGate::Clock::duration{rep}};
    }
    // Token kinds (ReleaseGate::kindBit) and acquireBit this application has used.
    [[nodiscard]] uint32_t consumedKinds() const {
        return owner ? owner->consumedKinds.load(std::memory_order_relaxed) : 0;
    }
    void noteAcquireHold() const {
        if (owner) owner->consumedKinds.fetch_or(ReleaseGate::acquireBit, std::memory_order_relaxed);
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

// A present whose cap deadline lies more than three cap intervals after its
// arrival cannot be held by the release wait's lead bound. It sleeps until
// three intervals before the deadline and then counts as arriving at that wake
// time. Returns that point, or nothing when the lead is within the bound or the
// cap interval is not positive.
[[nodiscard]] inline std::optional<ReleaseGate::Clock::time_point> releaseGateCatchUp(
        ReleaseGate::Clock::time_point arrival, ReleaseGate::Clock::time_point deadline,
        ReleaseGate::Clock::duration capInterval) {
    if (capInterval <= ReleaseGate::Clock::duration::zero() ||
            deadline - arrival <= 3 * capInterval)
        return std::nullopt;
    return deadline - 3 * capInterval;
}

}

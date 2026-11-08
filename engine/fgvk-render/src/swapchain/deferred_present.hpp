/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "generated_frame_plan.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

// A device-private queue from the same family as the application's graphics
// queue. The application never receives this queue's index. Multiple surfaces
// share the lock, but never hold it while sleeping.
struct DeferredPresentQueue {
    VkQueue queue{};
    std::mutex mutex;


    // Synchronous outputs use the exact same queue as the retained tail.
    // CPU call ordering across different queues is not present ordering.
    VkResult presentNow(PFN_vkQueuePresentKHR present, const VkPresentInfoKHR& info) {
        const std::lock_guard lock(mutex);
        return present(queue, &info);
    }
};

// One bounded source batch, with at most four generated outputs and its real.
// The worker owns values, never caller arrays, pNext pointers, a Swapchain or Vulkan
// wrappers. All binary signals are submitted before admission. The next
// source batch joins before resource reuse or reporting actual lower errors.
class DeferredPresent {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr size_t capacity = GeneratedFramePlan::capacity + 1;
    static constexpr uint32_t maxCopyImages = 8;
    struct Call {
        VkResult result{VK_SUCCESS};
        Clock::time_point started{}, finished{};
        Clock::time_point scheduled{};
        uint32_t imageIndex{};
        VkSwapchainKHR swapchain{};
        bool generated{}, copyChecked{};
        VkResult copyStatus{VK_NOT_READY};
        Clock::time_point copySampledAt{};
        Clock::time_point enqueued{}, queueLockStarted{}, queueLockAcquired{};
        bool readinessChecked{};
        Clock::time_point readinessStarted{}, readinessFinished{}, readinessDeadline{};
        VkResult readinessResult{VK_NOT_READY};
        VkQueue outputQueue{};
        VkSemaphore presentWait{};
        // A skipped generated output presented no image and has no output record.
        bool skipped{};
        // Count asynchronous copies only after their presented/skipped outcome.
        bool workerCopy{};
    };
    struct Completion : Call {
        uint64_t source{};
        std::array<Call, capacity> calls{};
        size_t count{};
        Clock::duration readinessShift{};
        bool readinessMiss{};
    };
    struct Readiness {
        VkDevice device{};
        VkSemaphore semaphore{};
        uint64_t value{};
        PFN_vkWaitSemaphoresKHR wait{};
        Clock::time_point deadline{Clock::time_point::max()};
        bool capOwned{};
        // Release-gated batches only (see release_gate.hpp): the producing
        // thread is free during the batch, and its next present or blocking
        // acquire must not sleep through output spacing. No output is
        // scheduled after `latestOutput`, except that a real output keeps at
        // least a quarter output interval after a late generated present
        // that started near the limit; a blocking join keeps spacing for
        // at most `joinTolerance`, then drops the remaining sleeps.
        // Readiness and semaphore order still hold.
        Clock::time_point latestOutput{Clock::time_point::max()};
        Clock::duration joinTolerance{};
        bool expediteOnJoin{};
        // Overlap is set only by the release gate. An overlapping batch may keep
        // outputting while one newer batch is admitted. Its planned spacing also
        // chains its first output after the previous batch's last output.
        bool overlap{};
        Clock::duration outputInterval{};
        // Optional profiling only: observe existing final-copy completion
        // without waiting or changing presentation/readiness policy.
        VkFence copyFence{};
        PFN_vkGetFenceStatus copyStatus{};
        // Optional observation owns its surface-level diagnostic state. No
        // borrowed Swapchain state or application arrays reach the worker.
        std::shared_ptr<void> observer;
        VkResult (*observe)(void*, PFN_vkQueuePresentKHR, VkQueue,
            const VkPresentInfoKHR&, uint64_t, uint64_t, bool){};
        uint64_t context{}, source{};
        bool trace{};
    };
    struct CompletionWait {
        Clock::duration waited{};
        bool capOwned{};
        Clock::time_point finished{};
    };
    // The generated image is acquired by the worker just before its output.
    // staged is signalled by the render slot's staging copy on the game queue and
    // is waited exactly once, by the worker's copy or skip. copies[k] is a
    // pre-recorded, resubmittable copy from the staging image into swapchain
    // image k and signals presentWaits[k] and continuation. On a skip an empty
    // submit waits staged and signals continuation and fence, so the real present
    // and the render slot never wait on an unsignalled object.
    struct GeneratedCopy {
        VkDevice device{};
        PFN_vkAcquireNextImageKHR acquire{};
        PFN_vkQueueSubmit submit{};
        VkSemaphore acquireSemaphore{};
        VkSemaphore staged{};
        VkSemaphore continuation{};
        VkFence fence{};
        uint32_t imageCount{};
        std::array<VkCommandBuffer, maxCopyImages> copies{};
        std::array<VkSemaphore, maxCopyImages> presentWaits{};
        Clock::duration giveUp{};
    };
    // The real output of an internal application image. The worker polls for a
    // lower image at its deadline and never skips the output. The callbacks get
    // owner.get(); this header names no swapchain or wrapper type. Each callback
    // runs with DeferredPresentQueue::mutex held and never sleeps.
    //   tryAcquire: non-blocking lower acquire. Returns VK_SUCCESS with the index,
    //     VK_NOT_READY/VK_TIMEOUT to retry, or an error to abandon.
    //   submit: copies the application image into the lower image, waiting
    //     continuation, and returns the semaphore the presentation waits.
    //   abandon: frees the application image without presenting, after continuation.
    struct InternalRealCopy {
        std::shared_ptr<void> owner;
        VkResult (*tryAcquire)(void* owner, uint32_t* lowerIndex) = nullptr;
        VkResult (*submit)(void* owner, VkQueue queue, uint32_t applicationIndex,
            uint32_t lowerIndex, VkSemaphore continuation, VkSemaphore* presentWait) = nullptr;
        VkResult (*abandon)(void* owner, VkQueue queue, uint32_t applicationIndex,
            VkSemaphore continuation) = nullptr;
        uint32_t applicationIndex{};
    };

    // Keep absolute slots through ordinary wake lateness; rephase a start
    // over half a slot late. Adding every wake or lower-call duration to the
    // next slot would gradually slow a continuous source/output clock.
    static Clock::time_point nextOutputDeadline(Clock::time_point requested,
            Clock::time_point previousRequested, Clock::time_point previousStarted) {
        const auto interval = std::max(Clock::duration{}, requested - previousRequested);
        return previousStarted > previousRequested + interval / 2
            ? std::max(requested, previousStarted + interval) : requested;
    }

    DeferredPresent(std::shared_ptr<DeferredPresentQueue> queue,
            PFN_vkQueuePresentKHR present, VkDevice device = VK_NULL_HANDLE) :
            device(device), queue(std::move(queue)), present(present),
            worker([this] { run(); }) {}
    DeferredPresent(const DeferredPresent&) = delete;
    DeferredPresent& operator=(const DeferredPresent&) = delete;
    ~DeferredPresent() {
        // Preserve the deadline and submit the retained image before teardown.
        static_cast<void>(finish(UINT64_MAX, false));
        {
            const std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_one();
        worker.join();
        if (registeredSwapchain) {
            const std::lock_guard lock(deliveriesMutex());
            const DeliveryKey key{device, registeredSwapchain};
            const auto found = deliveries().find(key);
            if (found != deliveries().end() && found->second == delivery)
                deliveries().erase(found);
        }
    }

    // A blocking wait on the latest retained real ID on the producing thread
    // serializes source production with this output clock. Retain ordinary
    // FIFO delivery for that client instead of learning our own output delay
    // as a progressively slower source period. Enabled features, zero-time
    // polls, older IDs and waits on other threads do not establish this case.
    static void applicationWait(VkSwapchainKHR swapchain, uint64_t id, uint64_t timeout) {
        const auto& target = waitTarget();
        if (timeout && id && target.swapchain == swapchain &&
                (target.id == id || target.id2 == id)) {
            if (auto state = target.serial.lock()) state->store(true, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] bool serialWaitObserved() const {
        return serialWait->load(std::memory_order_relaxed);
    }
    void completionPacingBlocksSource() { serialWait->store(true, std::memory_order_relaxed); }

    // A successful application present may still be retained by this worker.
    // The lower wait API may only see an ID after its lower present returned.
    // Join that handoff on any calling thread, then let the driver perform
    // the actual presentation wait with the remainder of the caller's budget.
    template<typename Wait>
    static VkResult waitForPresent(VkDevice device, VkSwapchainKHR swapchain,
            uint64_t id, uint64_t timeout, Wait&& wait) {
        const auto started = Clock::now();
        std::shared_ptr<Delivery> delivery;
        {
            const std::lock_guard lock(deliveriesMutex());
            const auto found = deliveries().find({device, swapchain});
            if (found != deliveries().end()) delivery = found->second;
        }
        if (!delivery || !id) return wait(timeout);
        bool waitedForDelivery{};
        {
            std::unique_lock lock(delivery->mutex);
            const auto delivered = [&] {
                return (delivery->id != id && delivery->id2 != id) || delivery->submitted;
            };
            if (!delivered()) {
                if (!timeout) return VK_TIMEOUT;
                waitedForDelivery = true;
                if (timeout == UINT64_MAX) delivery->changed.wait(lock, delivered);
                else {
                    const auto budget = std::chrono::nanoseconds(std::min<uint64_t>(timeout, INT64_MAX));
                    const auto remaining = std::max(Clock::duration{}, budget - (Clock::now() - started));
                    if (!delivery->changed.wait_for(lock, remaining, delivered)) return VK_TIMEOUT;
                }
            }
            if ((delivery->id == id || delivery->id2 == id) && delivery->result < 0)
                return delivery->result;
        }
        const auto deliveryWait = Clock::now() - started;
        if (timeout != UINT64_MAX) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count();
            timeout -= std::min(timeout, static_cast<uint64_t>(std::max<int64_t>(0, elapsed)));
        }
        const auto result = wait(timeout);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            const std::lock_guard lock(delivery->mutex);
            if (delivery->id == id || delivery->id2 == id) {
                if (!delivery->waitFinished) {
                    delivery->waitFinished = true;
                    delivery->waitFinishedAt = Clock::now();
                }
                if (waitedForDelivery) {
                    delivery->waitObserved = true;
                    delivery->completionDelay = std::min(delivery->workerWait, deliveryWait);
                }
            }
        }
        return result;
    }

    void applicationAcquire() {
        const std::lock_guard lock(delivery->mutex);
        if (!delivery->waitFinished) delivery->acquiredBeforeWait = true;
    }
    CompletionWait takeCompletionPacingWait() {
        const std::lock_guard lock(delivery->mutex);
        if (!delivery->waitObserved || !delivery->waitFinished || delivery->acquiredBeforeWait || delivery->delayTaken)
            return {};
        delivery->delayTaken = true;
        return {delivery->completionDelay, delivery->capOwned, delivery->waitFinishedAt};
    }

    // Open one source batch, or a newer one while every pending batch is sealed,
    // overlappable and ID-free, the ring has a free slot, and the completion queue
    // has an entry for every pending batch plus this one. Each admitted image's
    // binary signal must already be submitted. The producer seals on every
    // return/exception path.
    bool beginBatch(size_t limit, Readiness readiness) {
        const std::lock_guard lock(mutex);
        if (stopping || !limit || limit > capacity) return false;
        if (count == 0) {
            if (completedQueue[0]) return false;
        } else if (!readiness.overlap || count == slots || !retainableLocked() || !completionRoomLocked()) {
            return false;
        }
        openSlotLocked(limit, std::move(readiness), true);
        return true;
    }
    bool beginBatch(size_t limit) { return beginBatch(limit, Readiness{}); }
    void closeBatch() {
        {
            const std::lock_guard lock(mutex);
            if (count) batches[slotAt(count - 1)].open = false;
            retireFinishedHeadsLocked();
        }
        changed.notify_all();
    }

    // Storage of framegen-vulkan's own present-ID value. A node that points here is
    // forwarded to the lower present but is not an application wait.
    void setOwnedPresentIdStorage(const uint64_t* storage) { this->ownedIdStorage = storage; }
    // Only fixed-size metadata is admitted. Copy every scalar before returning
    // to the application; unknown nodes and caller-owned result arrays stay
    // synchronous. The same parser is used for policy and actual admission.
    [[nodiscard]] static bool supportsChain(const void* chain) {
        return snapshotChain(chain).has_value();
    }
    bool enqueue(const VkPresentInfoKHR& info, Clock::time_point deadline, bool generated = false) {
        return admit(info, deadline, generated, nullptr);
    }
    // The real output of an internal application image. Admission matches
    // enqueue. At its deadline the worker acquires a lower image, copies the
    // application image into it and presents it (see presentInternalReal).
    // info.pWaitSemaphores[0] is the continuation that copy waits.
    bool enqueueInternalReal(const VkPresentInfoKHR& info, Clock::time_point deadline,
            const InternalRealCopy& copy) {
        return copy.tryAcquire && copy.submit && copy.abandon && copy.owner &&
            admit(info, deadline, false, &copy);
    }
    // A generated output whose image the worker acquires at its deadline. Only
    // the newest open batch admits it, so the real output follows in that batch.
    bool enqueueGeneratedCopy(VkSwapchainKHR swapchain, const void* chain,
            Clock::time_point deadline, const GeneratedCopy& copy) {
        if (copy.imageCount == 0 || copy.imageCount > maxCopyImages || !copy.acquire || !copy.submit ||
                !copy.staged || !copy.continuation || !copy.fence)
            return false;
        const auto metadata = snapshotChain(chain, this->ownedIdStorage);
        if (!metadata || applicationIdentified(*metadata)) return false;
        {
            const std::lock_guard lock(mutex);
            if (stopping || !newestOpenLocked()) return false;
            Batch& batch = batches[slotAt(count - 1)];
            if (batch.produced == batch.limit) return false;
            batch.jobs[batch.produced++] = {swapchain, UINT32_MAX, VK_NULL_HANDLE, *metadata, deadline, true,
                batch.readiness.trace ? Clock::now() : Clock::time_point{}, true, copy};
        }
        changed.notify_all();
        return true;
    }

    // Timeout follows acquire semantics. A poll never blocks. Completed errors
    // remain the swapchain owner's responsibility, rather than being discarded
    // by another surface or a device-idle request.
    // retainNewest waits only for the batches older than the ones it keeps (see
    // retainedBatchesLocked). Kept batches remain outputting, so their join is
    // left to a later call.
    std::optional<Completion> finish(uint64_t timeoutNs, bool allowExpedite = true, bool retainNewest = false) {
        std::unique_lock lock(mutex);
        const size_t allowed = retainNewest ? retainedBatchesLocked() : 0;
        if (timeoutNs && allowExpedite && count > allowed) {
            bool expedited{};
            const auto now = Clock::now();
            for (size_t i = 0; i < count - allowed; ++i) {
                auto& batch = batches[slotAt(i)];
                if (batch.readiness.expediteOnJoin && batch.expediteAt == Clock::time_point::max()) {
                    batch.expediteAt = now + batch.readiness.joinTolerance;
                    expedited = true;
                }
            }
            if (expedited) changed.notify_all();
        }
        const auto settled = [this, allowed] { return count <= allowed; };
        if (!settled()) {
            if (timeoutNs == UINT64_MAX) changed.wait(lock, settled);
            else if (timeoutNs == 0 || !changed.wait_for(lock,
                    std::chrono::nanoseconds(std::min<uint64_t>(timeoutNs, INT64_MAX)),
                    settled)) return std::nullopt;
        }
        if (!completedQueue[0]) return Completion{};
        Completion result = std::move(*completedQueue[0]);
        popCompletionLocked();
        return result;
    }
    [[nodiscard]] bool hasCompletion() const {
        const std::lock_guard lock(mutex);
        return completedQueue[0].has_value();
    }
    // Any admitted batch not yet joined, including one that is still outputting.
    [[nodiscard]] bool pending() const {
        const std::lock_guard lock(mutex);
        return count > 0;
    }
    // Every pending batch is sealed, overlappable and ID-free, so they may keep
    // outputting while the next source is presented. Admission of a newer batch
    // additionally needs a free slot (see beginBatch).
    [[nodiscard]] bool retainable() const {
        const std::lock_guard lock(mutex);
        return retainableLocked();
    }
    // All overlap slots are pending. The next source's render slot is then the
    // one of the oldest batch, which must be joined before that slot is reused.
    [[nodiscard]] bool overlapRingFull() const {
        const std::lock_guard lock(mutex);
        return count == slots;
    }
    [[nodiscard]] bool newestOpen() const {
        const std::lock_guard lock(mutex);
        return newestOpenLocked();
    }
    // A synchronous output must wait for every older batch still outputting.
    [[nodiscard]] bool olderBatchPending() const {
        const std::lock_guard lock(mutex);
        return count > (newestOpenLocked() ? size_t{1} : size_t{0});
    }

    // Acquire accesses the same swapchain as the worker. Only poll under this
    // lock: waiting for an image while holding it could prevent the worker
    // from returning the retained image. Contention takes the ordinary join
    // path, without blocking a timeout-zero caller on the queue mutex.
    template<typename Acquire>
    VkResult pollAcquire(Acquire&& acquire) {
        const std::unique_lock lock(queue->mutex, std::try_to_lock);
        return lock.owns_lock() ? acquire() : VK_NOT_READY;
    }

    // Image pressure needs one available image, not an expedited drain of
    // every future output. Poll under the shared lock; compositor releases
    // need not notify this worker, so also retry after a bounded sleep.
    template<typename Acquire>
    VkResult waitAcquire(uint64_t timeoutNs, Acquire&& acquire) {
        const auto started = Clock::now();
        const auto budget = std::chrono::nanoseconds(std::min<uint64_t>(timeoutNs, INT64_MAX));
        for (;;) {
            {
                const std::lock_guard lock(mutex);
                for (const auto& completed : completedQueue)
                    if (completed && completed->result < 0) return completed->result;
                for (size_t offset = 0; offset < count; ++offset)
                    if (batches[slotAt(offset)].aggregate.result < 0)
                        return batches[slotAt(offset)].aggregate.result;
            }
            const auto result = this->pollAcquire(acquire);
            if (result != VK_NOT_READY && result != VK_TIMEOUT) return result;
            if (!timeoutNs) return result;
            auto sleep = Clock::duration{std::chrono::microseconds{250}};
            if (timeoutNs != UINT64_MAX) {
                const auto elapsed = Clock::now() - started;
                if (elapsed >= budget) return VK_TIMEOUT;
                sleep = std::min(sleep, budget - elapsed);
            }
            std::unique_lock lock(mutex);
            changed.wait_for(lock, sleep);
        }
    }

private:
    struct Delivery {
        std::mutex mutex;
        std::condition_variable changed;
        uint64_t id{}, id2{};
        bool submitted{};
        bool waitObserved{}, waitFinished{}, acquiredBeforeWait{}, delayTaken{};
        bool capOwned{};
        Clock::duration workerWait{}, completionDelay{};
        Clock::time_point waitFinishedAt{};
        VkResult result{VK_SUCCESS};
    };
    struct DeliveryKey {
        VkDevice device;
        VkSwapchainKHR swapchain;
        bool operator==(const DeliveryKey&) const = default;
    };
    struct DeliveryHash {
        size_t operator()(const DeliveryKey& key) const {
            return std::hash<VkDevice>{}(key.device) ^ (std::hash<VkSwapchainKHR>{}(key.swapchain) << 1);
        }
    };
    static std::unordered_map<DeliveryKey, std::shared_ptr<Delivery>, DeliveryHash>& deliveries() {
        static std::unordered_map<DeliveryKey, std::shared_ptr<Delivery>, DeliveryHash> values;
        return values;
    }
    static std::mutex& deliveriesMutex() {
        static std::mutex mutex;
        return mutex;
    }
    VkDevice device{};
    VkSwapchainKHR registeredSwapchain{};
    const uint64_t* ownedIdStorage{};
    std::shared_ptr<Delivery> delivery = std::make_shared<Delivery>();
    struct WaitTarget {
        std::weak_ptr<std::atomic_bool> serial;
        VkSwapchainKHR swapchain{};
        uint64_t id{}, id2{};
    };
    static WaitTarget& waitTarget() {
        static thread_local WaitTarget target;
        return target;
    }
    std::shared_ptr<std::atomic_bool> serialWait = std::make_shared<std::atomic_bool>(false);
    struct Metadata {
        VkFence fence{};
        uint64_t id{}, id2{};
        VkPresentModeKHR mode{};
        bool hasFence{}, hasId{}, hasId2{}, hasMode{};
        // ID2 whose value is framegen-vulkan's own probe storage, not an application wait.
        bool ownedId2{};
        bool hasMetering{};
        uint32_t framesPerBatch{};
    };
    // Only application IDs make a present identified: VkPresentIdKHR, or a
    // VkPresentId2KHR that framegen-vulkan did not supply. Identified presents register
    // the delivery and keep the overlapped batch ring from retaining.
    static bool applicationIdentified(const Metadata& metadata) {
        return (metadata.hasId && metadata.id) || (metadata.hasId2 && metadata.id2 && !metadata.ownedId2);
    }
    [[nodiscard]] static std::optional<Metadata> snapshotChain(const void* chain,
            const uint64_t* ownedIds = nullptr) {
        Metadata copy;
        // Four known node kinds, each at most once. Bound even malformed input.
        for (unsigned n = 0; chain; ++n) {
            if (n == 5) return std::nullopt;
            VkStructureType type{};
            std::memcpy(&type, chain, sizeof(type));
            switch (type) {
#if defined(VK_NV_present_metering)
            case VK_STRUCTURE_TYPE_SET_PRESENT_CONFIG_NV: {
                VkSetPresentConfigNV node{};
                std::memcpy(&node, chain, sizeof(node));
                if (copy.hasMetering || !node.numFramesPerBatch || node.numFramesPerBatch > 8) return std::nullopt;
                copy.hasMetering = true; copy.framesPerBatch = node.numFramesPerBatch;
                break;
            }
#endif
            case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT: {
                VkSwapchainPresentFenceInfoEXT node{};
                std::memcpy(&node, chain, sizeof(node));
                if (copy.hasFence || node.swapchainCount != 1 || !node.pFences) return std::nullopt;
                copy.hasFence = true; copy.fence = node.pFences[0];
                break;
            }
            case VK_STRUCTURE_TYPE_PRESENT_ID_KHR: {
                VkPresentIdKHR node{};
                std::memcpy(&node, chain, sizeof(node));
                if (copy.hasId || node.swapchainCount != 1 || !node.pPresentIds) return std::nullopt;
                copy.hasId = true; copy.id = node.pPresentIds[0];
                break;
            }
#if defined(VK_KHR_present_id2)
            case VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR: {
                VkPresentId2KHR node{};
                std::memcpy(&node, chain, sizeof(node));
                if (copy.hasId2 || node.swapchainCount != 1 || !node.pPresentIds) return std::nullopt;
                copy.hasId2 = true; copy.id2 = node.pPresentIds[0];
                copy.ownedId2 = ownedIds && node.pPresentIds == ownedIds;
                break;
            }
#endif
            case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT: {
                VkSwapchainPresentModeInfoEXT node{};
                std::memcpy(&node, chain, sizeof(node));
                if (copy.hasMode || node.swapchainCount != 1 || !node.pPresentModes) return std::nullopt;
                copy.hasMode = true; copy.mode = node.pPresentModes[0];
                break;
            }
            default: return std::nullopt;
            }
            const void* next{};
            std::memcpy(&next, static_cast<const std::byte*>(chain) +
                offsetof(VkBaseInStructure, pNext), sizeof(next));
            chain = next;
        }
        return copy;
    }
    struct Job {
        VkSwapchainKHR swapchain{};
        uint32_t image{};
        VkSemaphore wait{};
        Metadata metadata{};
        Clock::time_point deadline{};
        bool generated{};
        Clock::time_point enqueued{};
        bool copyActive{};
        GeneratedCopy copy{};
        bool internalActive{};
        InternalRealCopy internal{};
    };
    // Shared admission for enqueue and enqueueInternalReal. The metadata snapshot,
    // present-ID registration and batch rules are the same for both.
    bool admit(const VkPresentInfoKHR& info, Clock::time_point deadline, bool generated,
            const InternalRealCopy* internal) {
        if (info.swapchainCount != 1 || info.waitSemaphoreCount != 1 || info.pResults ||
                !info.pSwapchains || !info.pImageIndices || !info.pWaitSemaphores)
            return false;
        const auto metadata = snapshotChain(info.pNext, this->ownedIdStorage);
        if (!metadata) return false;
        const bool identified = applicationIdentified(*metadata);
        {
            const std::lock_guard lock(mutex);
            if (stopping) return false;
            const bool implicit = !newestOpenLocked();
            // The implicit single-image batch is sealed at creation and needs an
            // empty pipeline, as before overlap existed.
            if (implicit && (count || completedQueue[0])) return false;
            if (!implicit) {
                const Batch& open = batches[slotAt(count - 1)];
                // An overlapping batch behind a pending one cannot own the single present-ID registry.
                if (open.produced == open.limit || (count >= 2 && identified)) return false;
            }
            if (implicit) openSlotLocked(1, Readiness{}, false);
            Batch& batch = batches[slotAt(count - 1)];
            if (identified && !registerDeliveryLocked(*metadata, info.pSwapchains[0], batch.readiness.capOwned)) {
                if (implicit) --count;
                return false;
            }
            if (identified) batch.registersId = true;
            Job job{info.pSwapchains[0], info.pImageIndices[0], info.pWaitSemaphores[0],
                *metadata, deadline, generated, batch.readiness.trace ? Clock::now() : Clock::time_point{}};
            if (internal) {
                job.internalActive = true;
                job.internal = *internal;
            }
            batch.jobs[batch.produced++] = std::move(job);
        }
        waitTarget() = {serialWait, info.pSwapchains[0],
            metadata->hasId ? metadata->id : 0,
            metadata->hasId2 && !metadata->ownedId2 ? metadata->id2 : 0};
        changed.notify_all();
        return true;
    }
    struct Batch {
        std::array<Job, capacity> jobs{};
        size_t produced{}, consumed{}, limit{1};
        bool open{}, registersId{};
        Completion aggregate{};
        Readiness readiness{};
        Clock::time_point expediteAt{Clock::time_point::max()};
    };
    // The oldest pending batch is `head`; `count` pending batches follow it in
    // ring order. Only the newest pending batch can still be open.
    static constexpr size_t slots = 3;
    std::array<Batch, slots> batches{};
    size_t head{}, count{};
    // Finished batches not yet taken, oldest first. completionRoomLocked keeps
    // untaken completions plus pending batches within these entries, so every
    // retiring batch finds a free entry.
    std::array<std::optional<Completion>, 4> completedQueue{};
    std::shared_ptr<DeferredPresentQueue> queue;
    PFN_vkQueuePresentKHR present;
    mutable std::mutex mutex;
    std::condition_variable changed;
    bool stopping{};
    std::thread worker;

    [[nodiscard]] size_t slotAt(size_t offset) const { return (head + offset) % slots; }
    [[nodiscard]] bool newestOpenLocked() const { return count && batches[slotAt(count - 1)].open; }
    // Every pending batch must be sealed, overlappable and ID-free. Batches
    // retained this way are joined oldest first, keeping the newest two outputting.
    [[nodiscard]] bool retainableLocked() const {
        if (count == 0) return false;
        for (size_t offset = 0; offset < count; ++offset) {
            const Batch& batch = batches[slotAt(offset)];
            if (batch.open || !batch.readiness.overlap || batch.registersId) return false;
        }
        return true;
    }
    // A retained join keeps the newest two sealed batches outputting. An open
    // newest batch cannot be joined by its producer, so only the batches older
    // than it are awaited.
    [[nodiscard]] size_t retainedBatchesLocked() const {
        return newestOpenLocked() ? size_t{1} : std::min(count, size_t{2});
    }
    // Each pending batch retires into the completion queue, and the batch being
    // admitted needs one entry too. Untaken completions occupy the other entries.
    [[nodiscard]] bool completionRoomLocked() const {
        const auto untaken = static_cast<size_t>(std::count_if(completedQueue.begin(), completedQueue.end(),
            [](const std::optional<Completion>& entry) { return entry.has_value(); }));
        return untaken + count + 1 <= completedQueue.size();
    }
    void openSlotLocked(size_t limit, Readiness ready, bool open) {
        Batch& batch = batches[slotAt(count)];
        batch.produced = batch.consumed = 0;
        batch.limit = limit;
        batch.open = open;
        batch.registersId = false;
        batch.aggregate = {};
        batch.expediteAt = Clock::time_point::max();
        batch.readiness = std::move(ready);
        ++count;
    }
    void popCompletionLocked() {
        for (size_t i = 0; i + 1 < completedQueue.size(); ++i)
            completedQueue[i] = std::move(completedQueue[i + 1]);
        completedQueue.back().reset();
    }
    // Retires only the head: completions are reported in source order.
    void retireFinishedHeadsLocked() {
        while (count) {
            Batch& batch = batches[head];
            if (batch.open || batch.consumed != batch.produced) return;
            size_t free{};
            while (completedQueue.at(free)) ++free;
            completedQueue[free].emplace(std::move(batch.aggregate));
            head = (head + 1) % slots;
            --count;
        }
    }
    // One registry entry per live worker, replaced only after its previous
    // source batch completed. Waiters retain shared state without acquiring the
    // Swapchain owner or the queue lock.
    bool registerDeliveryLocked(const Metadata& metadata, VkSwapchainKHR swapchain, bool capOwned) {
        if (registeredSwapchain && registeredSwapchain != swapchain) return false;
        try {
            const std::lock_guard deliveryLock(deliveriesMutex());
            deliveries().insert_or_assign({device, swapchain}, delivery);
        } catch (...) { return false; }
        registeredSwapchain = swapchain;
        const std::lock_guard deliveryLock(delivery->mutex);
        delivery->id = metadata.hasId ? metadata.id : 0;
        delivery->id2 = metadata.hasId2 && !metadata.ownedId2 ? metadata.id2 : 0;
        delivery->submitted = false;
        delivery->result = VK_SUCCESS;
        delivery->waitObserved = delivery->waitFinished = delivery->acquiredBeforeWait = delivery->delayTaken = false;
        delivery->workerWait = delivery->completionDelay = {};
        delivery->waitFinishedAt = {};
        delivery->capOwned = capOwned;
        return true;
    }

    // Callers hold queue->mutex and never sleep there. Every submit waits the
    // binary staged semaphore, and the acquire too when it succeeded. A copy
    // signals that image's present wait and the continuation. A skip signals
    // only the continuation. Every submit signals the fence.
    static VkResult submitGeneratedWork(const VkQueue queue, const GeneratedCopy& copy,
            const bool acquired, const VkCommandBuffer command, const VkSemaphore presentWait) {
        // The staged wait is always the last wait and the continuation the last signal.
        const std::array<VkSemaphore, 2> waits{copy.acquireSemaphore, copy.staged};
        const std::array<VkPipelineStageFlags, 2> stages{
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
        const std::array<VkSemaphore, 2> signals{presentWait, copy.continuation};
        const uint32_t waitCount = acquired ? 2u : 1u;
        const uint32_t signalCount = presentWait != VK_NULL_HANDLE ? 2u : 1u;
        const VkSubmitInfo submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreCount = waitCount,
            .pWaitSemaphores = waits.data() + (2 - waitCount),
            .pWaitDstStageMask = stages.data() + (2 - waitCount),
            .commandBufferCount = command != VK_NULL_HANDLE ? 1u : 0u,
            .pCommandBuffers = command != VK_NULL_HANDLE ? &command : nullptr,
            .signalSemaphoreCount = signalCount,
            .pSignalSemaphores = signals.data() + (2 - signalCount),
        };
        return copy.submit(queue, 1, &submit, copy.fence);
    }

    // The real output of an internal application image, produced by this worker
    // at its deadline. It never gives up or skips: the loop ends when a lower image
    // is copied and presented, or when the application image is abandoned on a lower
    // error or while stopping. Each attempt holds queue->mutex for tryAcquire and
    // the copy/present only, and sleeps outside it.
    void presentInternalReal(Job& job, const Readiness& ready, const void* chain, Completion& result) {
        const InternalRealCopy& copy = job.internal;
        void* const owner = copy.owner.get();
        result.workerCopy = true;
        uint32_t lowerIndex{};
        VkSemaphore presentWait{};
        bool lockStarted{};
        for (;;) {
            bool stopped{};
            {
                const std::lock_guard lock(mutex);
                stopped = stopping;
            }
            if (ready.trace && !lockStarted) result.queueLockStarted = Clock::now();
            lockStarted = true;
            VkResult error = VK_SUCCESS;
            {
                const std::lock_guard queueLock(queue->mutex);
                if (ready.trace) result.queueLockAcquired = Clock::now();
                if (stopped) {
                    error = VK_ERROR_UNKNOWN;
                } else {
                    const VkResult acquired = copy.tryAcquire(owner, &lowerIndex);
                    if (acquired == VK_SUCCESS) {
                        const VkResult submitted = copy.submit(owner, queue->queue,
                            copy.applicationIndex, lowerIndex, job.wait, &presentWait);
                        if (submitted < 0) {
                            result.imageIndex = lowerIndex;
                            result.skipped = true;
                            result.started = result.finished = Clock::now();
                            result.result = submitted;
                            return;
                        }
                        const VkPresentInfoKHR info{
                            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                            .pNext = chain,
                            .waitSemaphoreCount = 1, .pWaitSemaphores = &presentWait,
                            .swapchainCount = 1, .pSwapchains = &job.swapchain,
                            .pImageIndices = &lowerIndex,
                        };
                        result.imageIndex = lowerIndex;
                        result.presentWait = presentWait;
                        result.started = Clock::now();
                        result.result = ready.observer && ready.observe
                            ? ready.observe(ready.observer.get(), present, queue->queue,
                                info, ready.context, ready.source, job.generated)
                            : present(queue->queue, &info);
                        result.finished = Clock::now();
                        if (ready.copyStatus && ready.copyFence) {
                            result.copyChecked = true;
                            result.copyStatus = ready.copyStatus(ready.device, ready.copyFence);
                            result.copySampledAt = Clock::now();
                        }
                        return;
                    }
                    if (acquired < 0) {
                        error = acquired;
                    }
                }
                if (error != VK_SUCCESS) {
                    // Frees the application image without a present. Nothing is left acquired.
                    static_cast<void>(copy.abandon(owner, queue->queue, copy.applicationIndex, job.wait));
                    result.skipped = true;
                    result.started = result.finished = Clock::now();
                    result.result = error;
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds{250});
        }
    }

    void run() {
        std::unique_lock lock(mutex);
        Clock::duration readinessShift{};
        // Shift from the batch's first output to its final deadline. Later
        // outputs of the same batch keep their requested spacing from it.
        Clock::duration batchShift{};
        bool readinessMiss{};
        VkResult readinessResult{VK_SUCCESS};
        Clock::duration workerWait{};
        Clock::time_point readinessStarted{}, readinessFinished{};
        // Last output of the previous batch on this worker, for cross-batch spacing.
        Clock::time_point previousStarted{}, previousFinished{}, previousScheduled{};
        bool havePrevious{};
        for (;;) {
            changed.wait(lock, [this] {
                return stopping || (count && batches[head].consumed < batches[head].produced);
            });
            if (stopping) return;
            Batch& batch = batches[head];
            const size_t index = batch.consumed;
            const auto current = batch.jobs[index];
            const bool first = index == 0;
            const auto ready = batch.readiness;
            const bool copyTail = index > 0 && batch.jobs[index - 1].copyActive &&
                !batch.aggregate.calls[index - 1].skipped;
            auto deadline = first ? current.deadline :
                nextOutputDeadline(current.deadline + batchShift,
                    batch.jobs[index - 1].deadline + batchShift,
                    batch.aggregate.calls[index - 1].started);
            if (first && havePrevious && ready.overlap && ready.outputInterval > Clock::duration::zero() &&
                    Clock::now() - previousFinished < ready.outputInterval)
                deadline = std::max(deadline, previousScheduled + ready.outputInterval - ready.outputInterval / 16);
            if (copyTail) {
                // The real after a generated output keeps full spacing, but a release limit
                // may pull it in: no later than latestOutput, and at least a quarter output
                // interval after the generated present started. Generated outputs may start
                // up to a quarter output interval before the batch limit. FIFO still shows
                // each image at least one refresh.
                deadline = std::max(deadline,
                    batch.aggregate.calls[index - 1].started + ready.outputInterval);
                if (ready.latestOutput != Clock::time_point::max())
                    deadline = std::min(deadline, std::max(ready.latestOutput,
                        batch.aggregate.calls[index - 1].started + ready.outputInterval / 4));
            } else {
                deadline = std::min(deadline, ready.latestOutput);
            }
            lock.unlock();
            if (first) {
                workerWait = {};
                readinessShift = {};
                batchShift = {};
                readinessMiss = false;
                readinessResult = VK_SUCCESS;
                readinessStarted = readinessFinished = {};
                if (ready.wait) {
                    const auto waitStarted = Clock::now();
                    readinessStarted = waitStarted;
                    const VkSemaphoreWaitInfo waitInfo{
                        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                        .semaphoreCount = 1, .pSemaphores = &ready.semaphore,
                        .pValues = &ready.value,
                    };
                    // Backend signals have already been submitted. Keep the
                    // producer free to start its next frame; timeout retains
                    // ordinary semaphore-protected delivery of every image.
                    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::max(Clock::duration{}, ready.deadline - Clock::now())).count();
                    readinessResult = ready.wait(ready.device, &waitInfo,
                        static_cast<uint64_t>(std::min(int64_t{50'000'000}, remaining)));
                    readinessFinished = Clock::now();
                    workerWait += readinessFinished - waitStarted;
                    readinessMiss = readinessResult != VK_SUCCESS;
                    if (!readinessMiss) {
                        readinessShift = std::max(Clock::duration{}, Clock::now() - deadline);
                        deadline += readinessShift;
                    }
                }
                batchShift = deadline - current.deadline;
            }
            if (!readinessMiss) {
                const auto waitStarted = Clock::now();
                if (ready.expediteOnJoin && !copyTail) {
                    lock.lock();
                    for (auto until = std::min(deadline, batch.expediteAt);
                            !stopping && Clock::now() < until; until = std::min(deadline, batch.expediteAt))
                        changed.wait_until(lock, until);
                    lock.unlock();
                } else {
                    std::this_thread::sleep_until(deadline);
                }
                workerWait += Clock::now() - waitStarted;
            }
            const auto& metadata = current.metadata;
            const void* chain{};
#if defined(VK_NV_present_metering)
            VkSetPresentConfigNV metering{
                .sType = VK_STRUCTURE_TYPE_SET_PRESENT_CONFIG_NV,
                .numFramesPerBatch = metadata.framesPerBatch,
            };
            if (metadata.hasMetering) chain = &metering;
#endif
            VkSwapchainPresentFenceInfoEXT fences{
                .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
                .swapchainCount = 1, .pFences = &metadata.fence,
            };
            if (metadata.hasFence) { fences.pNext = chain; chain = &fences; }
            VkPresentIdKHR id{
                .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
                .pNext = chain, .swapchainCount = 1, .pPresentIds = &metadata.id,
            };
            if (metadata.hasId) chain = &id;
#if defined(VK_KHR_present_id2)
            VkPresentId2KHR id2{
                .sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
                .pNext = chain, .swapchainCount = 1, .pPresentIds = &metadata.id2,
            };
            if (metadata.hasId2) chain = &id2;
#endif
            VkSwapchainPresentModeInfoEXT mode{
                .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT,
                .pNext = chain, .swapchainCount = 1, .pPresentModes = &metadata.mode,
            };
            if (metadata.hasMode) chain = &mode;
            auto job = current;
            Completion result;
            result.source = ready.source;
            result.enqueued = current.enqueued;
            result.readinessChecked = first && ready.wait;
            result.readinessStarted = readinessStarted;
            result.readinessFinished = readinessFinished;
            result.readinessDeadline = ready.deadline;
            result.readinessResult = readinessResult;
            result.scheduled = deadline;
            result.swapchain = job.swapchain;
            result.generated = job.generated;
            result.workerCopy = job.copyActive;
            result.outputQueue = queue->queue;
            // A generated output takes its image only at its deadline. Polling
            // never sleeps under queue->mutex, and every path below signals the
            // continuation and the render-slot fence.
            if (job.internalActive) {
                presentInternalReal(job, ready, chain, result);
            } else {
                VkResult skipResult = VK_SUCCESS;
                bool acquired{};
                bool copyPending{};
                if (job.copyActive) {
                    uint32_t image{};
                    VkResult acquireResult = VK_NOT_READY;
                    auto giveUpAt = deadline + job.copy.giveUp;
                    // Generated outputs may start up to a quarter output interval before the
                    // batch limit. FIFO still shows each image at least one refresh.
                    if (ready.latestOutput != Clock::time_point::max())
                        giveUpAt = std::min(giveUpAt, ready.latestOutput - ready.outputInterval / 4);
                    lock.lock();
                    const bool skipGenerated = readinessMiss || stopping ||
                        batch.expediteAt != Clock::time_point::max() ||
                        (ready.overlap && Clock::now() >= giveUpAt);
                    lock.unlock();
                    while (!skipGenerated) {
                        {
                            const std::lock_guard queueLock(queue->mutex);
                            acquireResult = job.copy.acquire(job.copy.device, job.swapchain, 0,
                                job.copy.acquireSemaphore, VK_NULL_HANDLE, &image);
                        }
                        if (acquireResult < 0 || acquireResult == VK_SUCCESS ||
                                acquireResult == VK_SUBOPTIMAL_KHR || Clock::now() >= giveUpAt)
                            break;
                        lock.lock();
                        const bool expedited = stopping || batch.expediteAt != Clock::time_point::max();
                        lock.unlock();
                        if (expedited) break;
                        std::this_thread::sleep_for(std::chrono::microseconds{250});
                    }
                    acquired = acquireResult == VK_SUCCESS || acquireResult == VK_SUBOPTIMAL_KHR;
                    if (acquireResult < 0) {
                        skipResult = acquireResult;
                    } else if (acquired && image < job.copy.imageCount) {
                        job.image = image;
                        job.wait = job.copy.presentWaits.at(image);
                        copyPending = true;
                    } else if (acquired) {
                        // The image cannot be returned, so it is reported as an error.
                        skipResult = VK_ERROR_UNKNOWN;
                    }
                }
                result.imageIndex = job.image;
                result.presentWait = job.wait;
                const VkPresentInfoKHR info{
                    .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                    .pNext = chain,
                    .waitSemaphoreCount = 1, .pWaitSemaphores = &job.wait,
                    .swapchainCount = 1, .pSwapchains = &job.swapchain,
                    .pImageIndices = &job.image,
                };
                {
                    if (ready.trace) result.queueLockStarted = Clock::now();
                    const std::lock_guard queueLock(queue->mutex);
                    if (ready.trace) result.queueLockAcquired = Clock::now();
                    bool presentOutput = !job.copyActive;
                    if (job.copyActive) {
                        if (copyPending) {
                            const VkResult copied = submitGeneratedWork(queue->queue, job.copy, true,
                                job.copy.copies.at(job.image), job.wait);
                            presentOutput = copied == VK_SUCCESS;
                            if (copied < 0) skipResult = copied;
                        }
                        if (!presentOutput) {
                            const VkResult signalled = submitGeneratedWork(queue->queue, job.copy,
                                acquired, VK_NULL_HANDLE, VK_NULL_HANDLE);
                            if (skipResult == VK_SUCCESS) skipResult = signalled;
                        }
                    }
                    if (presentOutput) {
                        result.started = Clock::now();
                        result.result = ready.observer && ready.observe
                            ? ready.observe(ready.observer.get(), present, queue->queue,
                                info, ready.context, ready.source, job.generated)
                            : present(queue->queue, &info);
                        result.finished = Clock::now();
                        if (ready.copyStatus && ready.copyFence) {
                            result.copyChecked = true;
                            result.copyStatus = ready.copyStatus(ready.device, ready.copyFence);
                            result.copySampledAt = Clock::now();
                        }
                    } else {
                        result.skipped = true;
                        result.started = Clock::now();
                        result.finished = result.started;
                        result.result = skipResult;
                    }
                }
            }
            if (applicationIdentified(metadata)) {
                {
                    const std::lock_guard deliveryLock(delivery->mutex);
                    delivery->submitted = true;
                    delivery->result = result.result;
                    delivery->workerWait = workerWait;
                }
                delivery->changed.notify_all();
            }
            lock.lock();
            batch.aggregate.source = ready.source;
            batch.aggregate.readinessShift = readinessShift;
            batch.aggregate.readinessMiss = readinessMiss;
            if (readinessResult < 0) batch.aggregate.result = readinessResult;
            if (batch.aggregate.count == 0) batch.aggregate.started = result.started;
            batch.aggregate.finished = result.finished;
            if (batch.aggregate.result >= 0 && (result.result < 0 || batch.aggregate.result == VK_SUCCESS))
                batch.aggregate.result = result.result;
            batch.aggregate.calls[batch.aggregate.count++] = result;
            ++batch.consumed;
            // The worker is done with an internal real job; drop its owner reference.
            batch.jobs[index].internal = {};
            if (!result.skipped) {
                previousStarted = result.started;
                previousFinished = result.finished;
                // An early (expedited) output anchors the next batch at its actual start; late wakes keep the schedule.
                previousScheduled = std::min(result.scheduled, result.started);
                havePrevious = true;
            }
            retireFinishedHeadsLocked();
            changed.notify_all();
        }
    }
};
}

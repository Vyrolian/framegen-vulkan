/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "generated_frame_plan.hpp"
#include "swapchain/batch_phase.hpp"
#include "swapchain/onscreen_ids.hpp"
#include "swapchain/onscreen_watch.hpp"
#include "swapchain/output_margin.hpp"
#include "swapchain/present_marker.hpp"
#include "swapchain/screen_samples.hpp"
#include "swapchain/tick_lock.hpp"
#include "swapchain/create_policy.hpp"
#include "swapchain/retirement.hpp"
#include "present_diagnostics.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

// D7b: what a private submit does, kept with the submit for the stall probe. Kind 0 is the
// unknown default. An index is UINT32_MAX when the submit has none.
inline constexpr uint8_t submitKindUnknown = 0;
inline constexpr uint8_t submitKindCopy = 1;
inline constexpr uint8_t submitKindRelease = 2;
inline constexpr uint8_t submitKindGeneratedCopy = 3;
inline constexpr uint8_t submitKindGeneratedSkip = 4;
struct SubmitTag {
    uint8_t kind{submitKindUnknown};
    uint32_t applicationIndex{UINT32_MAX};
    uint32_t lowerIndex{UINT32_MAX};
    // D7c: a generated skip's SkipReason (0 = none for other kinds), and the source batch
    // ordinal the submit belongs to. Zero when unknown. Diagnostics only.
    uint8_t reason{0};
    uint64_t source{0};
};
// One private submit that returned VK_SUCCESS. A reused fence shows its newest use: the
// handle is kept as submitted, and its status is read later by the stall probe.
struct SubmitRecord {
    uint64_t tail{};
    uint8_t kind{};
    uint32_t applicationIndex{};
    uint32_t lowerIndex{};
    // D7c: copied from the tag.
    uint8_t reason{0};
    uint64_t source{0};
    uint32_t waits{};
    VkFence fence{};
};

// D7d: the lower-image call a LowerRecord or an in-flight call describes. None (0) is no call.
// Acquires are the lower acquire of a real, of a generated output, and of a synchronous output
// (which runs without queue->mutex). Presents are the lower present of each of the three.
inline constexpr uint8_t lowerOpNone = 0;
inline constexpr uint8_t lowerOpAcquireReal = 1;
inline constexpr uint8_t lowerOpAcquireGenerated = 2;
inline constexpr uint8_t lowerOpAcquireSync = 3;
inline constexpr uint8_t lowerOpPresentReal = 4;
inline constexpr uint8_t lowerOpPresentGenerated = 5;
inline constexpr uint8_t lowerOpPresentSync = 6;
// D7d: one finished lower acquire or present, with the private queue's tail value at its record.
// A zero startNs marks an entry that was never written. Times are steady-clock nanoseconds.
struct LowerRecord {
    int64_t startNs{};
    int64_t endNs{};
    uint64_t tail{};
    uint64_t swapchain{};
    uint64_t source{};
    int32_t result{};
    uint32_t lowerIndex{UINT32_MAX};
    uint8_t op{};
};
// D7d: nanoseconds since the steady clock's epoch, the clock present_diagnostics uses.
inline int64_t steadyNanoseconds(const std::chrono::steady_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}
// D7d: a handle as an integer, whether the handle type is a pointer or an integer.
template<typename Handle>
uint64_t handleBits(const Handle handle) {
    if constexpr (std::is_pointer_v<Handle>) return reinterpret_cast<uintptr_t>(handle);
    else return static_cast<uint64_t>(handle);
}

// A device-private queue from the same family as the application's graphics
// queue. The application never receives this queue's index. Multiple surfaces
// share the lock, but never hold it while sleeping.
struct DeferredPresentQueue {
    VkQueue queue{};
    std::mutex mutex;
    // F10: set by the first failed host acquire signal (ImportSemaphoreFdKHR). It is never
    // cleared, so the device uses the queued acquire from then on.
    std::atomic<bool> hostAcquireDisabled{false};
    // F9c: a timeline that every tailed submit on `queue` signals with its next value (copies,
    // releases and generated work, through submitPrivate; the queued acquire is not tailed). F10: the emulated acquire reads tailSubmitted and this counter, and
    // signals on the host once both are reached. Null when the timeline could not be
    // created: then nothing is signalled or waited for it, and the host path is off.
    VkSemaphore tail{};
    // Last tail value submitted with VK_SUCCESS. Only read and written under mutex.
    uint64_t tailNext{0};
    // The same value, published after VK_SUCCESS so the acquire can wait it without mutex.
    std::atomic<uint64_t> tailSubmitted{0};
    // D7b, pacing profile only: when set, submitPrivate records each successful tailed submit
    // in recent, at recentNext % recentCount. Both are only touched under mutex. A zero tail
    // marks an entry that was never written.
    bool history{false};
    static constexpr uint32_t recentCount = 16;
    std::array<SubmitRecord, recentCount> recent{};
    uint32_t recentNext{0};
    // D7d, pacing profile only (history): the finished lower acquires and presents. Written by
    // noteLower under mutex at lowerNext % lowerCount; a zero startNs marks an entry never written.
    static constexpr uint32_t lowerCount = 24;
    std::array<LowerRecord, lowerCount> lower{};
    uint32_t lowerNext{0};
    // D7d: the lower call in flight, readable without mutex by the stall probe. callOp is
    // lowerOpNone when nothing is in flight. beginCall stores the start and the lower index before
    // the op (release), so a reader that sees the op also sees its start.
    std::atomic<uint8_t> callOp{lowerOpNone};
    std::atomic<uint32_t> callLower{UINT32_MAX};
    std::atomic<int64_t> callSinceNs{0};

    // T4, GPU split profile only: a GPU marker submitted on queue right after each observed
    // lower present, under mutex. Set once at device creation before any present, released at
    // device destruction; null otherwise.
    // S8b: also set with FGVK_REAL_OFFSET_US=auto; readMarkerSamples then returns finished markers.
    std::shared_ptr<void> marker;
    void (*markAfterPresent)(void*, VkQueue, uint64_t, uint64_t, uint64_t, bool) noexcept {};
    size_t (*readMarkerSamples)(void*, uint64_t*, PresentMarkerSample*, size_t) noexcept {};
    // Caller holds mutex. A present without a feedback id cannot be joined in the profile and is
    // not marked, unless the auto real offset needs every present.
    void markPresentLocked(const uint64_t context, const uint64_t source, const uint64_t feedback,
            const bool generated) noexcept {
        if (marker && markAfterPresent && (feedback != 0 || readMarkerSamples))
            markAfterPresent(marker.get(), queue, context, source, feedback, generated);
    }
    // Caller holds mutex. S8b: finished markers newer than *cursor; 0 without the auto offset.
    size_t readMarkerSamplesLocked(uint64_t* const cursor, PresentMarkerSample* const out,
            const size_t max) noexcept {
        return marker && readMarkerSamples ? readMarkerSamples(marker.get(), cursor, out, max) : 0;
    }

    // D7d: the in-flight call. Both are no-ops unless history is set.
    void beginCall(const uint8_t op, const uint32_t lowerIndex) {
        if (!history) return;
        callSinceNs.store(steadyNanoseconds(std::chrono::steady_clock::now()), std::memory_order_relaxed);
        callLower.store(lowerIndex, std::memory_order_relaxed);
        callOp.store(op, std::memory_order_release);
    }
    void endCall() {
        if (!history) return;
        callOp.store(lowerOpNone, std::memory_order_release);
    }
    // D7d: caller holds mutex. Records one finished lower call, only when history is set.
    void noteLower(const uint8_t op, const VkSwapchainKHR swapchain, const uint32_t lowerIndex,
            const VkResult result, const uint64_t source, const int64_t startNs, const int64_t endNs) {
        if (!history) return;
        lower[lowerNext++ % lowerCount] = LowerRecord{
            .startNs = startNs, .endNs = endNs, .tail = tailNext, .swapchain = handleBits(swapchain),
            .source = source, .result = static_cast<int32_t>(result), .lowerIndex = lowerIndex, .op = op,
        };
    }
    // D7d: caller holds mutex. Runs one synchronous present through `present`. With history set it
    // is noted as lowerOpPresentSync, with the first image of info (none when info has no image).
    template<typename Present>
    VkResult presentSyncLocked(const VkPresentInfoKHR& info, Present&& present) {
        if (!history) return present();
        const bool hasImage = info.swapchainCount > 0;
        const VkSwapchainKHR swapchain = hasImage ? info.pSwapchains[0] : VK_NULL_HANDLE;
        const uint32_t lowerIndex = hasImage ? info.pImageIndices[0] : UINT32_MAX;
        const auto started = std::chrono::steady_clock::now();
        beginCall(lowerOpPresentSync, lowerIndex);
        const VkResult result = present();
        endCall();
        noteLower(lowerOpPresentSync, swapchain, lowerIndex, result, 0,
            steadyNanoseconds(started), steadyNanoseconds(std::chrono::steady_clock::now()));
        return result;
    }

    // Synchronous outputs use the exact same queue as the retained tail.
    // CPU call ordering across different queues is not present ordering.
    VkResult presentNow(PFN_vkQueuePresentKHR present, const VkPresentInfoKHR& info) {
        const std::lock_guard lock(mutex);
        return presentSyncLocked(info, [&] { return present(queue, &info); });
    }
};

// F9c: submits on the private queue and, when the tail timeline exists, also signals
// it with the next value. The caller holds q.mutex, which keeps the tail values in
// submission order. tailSubmitted advances only after VK_SUCCESS, so an acquire that
// waits it waits a value whose signal was already submitted. The pNext chain must be
// empty or one VkTimelineSemaphoreSubmitInfo with no pNext. Any other chain is
// submitted unchanged, without the tail.
// D7b: with q.history set, a tailed submit that returns VK_SUCCESS also records its tail value,
// tag and wait count. An untailed submit records nothing, because it has no tail value.
inline VkResult submitPrivate(DeferredPresentQueue& q, const PFN_vkQueueSubmit submit,
        const VkSubmitInfo& info, const VkFence fence, const SubmitTag& tag = {}) {
    if (q.tail == VK_NULL_HANDLE) return submit(q.queue, 1, &info, fence);
    const VkTimelineSemaphoreSubmitInfo* original = nullptr;
    if (info.pNext) {
        if (static_cast<const VkBaseInStructure*>(info.pNext)->sType !=
                VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)
            return submit(q.queue, 1, &info, fence);
        original = static_cast<const VkTimelineSemaphoreSubmitInfo*>(info.pNext);
        // A signal value count must be zero or equal the signal count (VUID-03245).
        if (original->pNext || (original->signalSemaphoreValueCount != 0 &&
                original->signalSemaphoreValueCount != info.signalSemaphoreCount))
            return submit(q.queue, 1, &info, fence);
    }
    std::vector<VkSemaphore> signals;
    std::vector<uint64_t> values(info.signalSemaphoreCount, 0);
    signals.reserve(info.signalSemaphoreCount + 1u);
    for (uint32_t i = 0; i < info.signalSemaphoreCount; ++i)
        signals.push_back(info.pSignalSemaphores[i]);
    if (original && original->signalSemaphoreValueCount != 0)
        std::copy_n(original->pSignalSemaphoreValues, info.signalSemaphoreCount, values.begin());
    const uint64_t next = q.tailNext + 1;
    signals.push_back(q.tail);
    values.push_back(next);
    // Wait values are kept as they were. Binary waits have no value, so a chain
    // that had none uses an empty wait value list.
    const VkTimelineSemaphoreSubmitInfo timeline{
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreValueCount = original ? original->waitSemaphoreValueCount : 0u,
        .pWaitSemaphoreValues = original ? original->pWaitSemaphoreValues : nullptr,
        .signalSemaphoreValueCount = static_cast<uint32_t>(values.size()),
        .pSignalSemaphoreValues = values.data(),
    };
    VkSubmitInfo tailed = info;
    tailed.pNext = &timeline;
    tailed.signalSemaphoreCount = static_cast<uint32_t>(signals.size());
    tailed.pSignalSemaphores = signals.data();
    const VkResult result = submit(q.queue, 1, &tailed, fence);
    if (result == VK_SUCCESS) {
        q.tailNext = next;
        q.tailSubmitted.store(next, std::memory_order_release);
        if (q.history) {
            q.recent[q.recentNext++ % q.recentCount] = {
                next, tag.kind, tag.applicationIndex, tag.lowerIndex, tag.reason, tag.source,
                info.waitSemaphoreCount, fence};
        }
    }
    return result;
}

// One bounded source batch, with at most four generated outputs and its real.
// The worker owns values, never caller arrays, pNext pointers, a Swapchain or Vulkan
// wrappers. All binary signals are submitted before admission. The next
// source batch joins before resource reuse or reporting actual lower errors.
class DeferredPresent {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr size_t capacity = GeneratedFramePlan::capacity + 1;
    static constexpr uint32_t maxCopyImages = 8;
    // Why a call presented no image. None for every presented output. GiveUp is reserved
    // by the F7 taxonomy: a generated output whose give-up window ends with no lower image
    // is Acquire, so no worker path produces GiveUp today.
    enum class SkipReason { None, Readiness, Stopping, Expedite, GiveUp, Acquire, Error };

    // D7c: the name of a SkipReason value in the stall probe, in enum order.
    static const char* skipReasonName(const uint8_t reason) {
        static constexpr std::array<const char*, 7> names{
            "none", "readiness", "stopping", "expedite", "giveup", "acquire", "error"};
        static_assert(names.size() == static_cast<size_t>(SkipReason::Error) + 1);
        return reason < names.size() ? names[reason] : "unknown";
    }

    // Diagnostics only: what a generated output was rendered for. No default member
    // initialisers: the defaulted `= {}` arguments below need the type complete here.
    struct OutputIdentity {
        uint64_t backendFrame;
        double phase;
    };
    struct Call {
        VkResult result{VK_SUCCESS};
        Clock::time_point started{}, finished{};
        Clock::time_point scheduled{};
        // W2: the effective pacing correction of a generated output (its scheduled time minus its
        // unmoved deadline). Zero for reals, for skipped outputs and when the correction is off.
        Clock::duration offset{};
        uint32_t imageIndex{};
        VkSwapchainKHR swapchain{};
        bool generated{}, copyChecked{};
        VkResult copyStatus{VK_NOT_READY};
        Clock::time_point copySampledAt{};
        Clock::time_point copySubmitStarted{};
        Clock::time_point enqueued{}, queueLockStarted{}, queueLockAcquired{};
        bool readinessChecked{};
        Clock::time_point readinessStarted{}, readinessFinished{}, readinessDeadline{};
        VkResult readinessResult{VK_NOT_READY};
        VkQueue outputQueue{};
        VkSemaphore presentWait{};
        // A skipped generated output presented no image and has no output record.
        bool skipped{};
        // Set wherever skipped is set. Diagnostics only.
        SkipReason skipReason{SkipReason::None};
        // Count asynchronous copies only after their presented/skipped outcome.
        bool workerCopy{};
        // Diagnostics only: the first call of a batch whose readiness succeeded
        // only through the late-readiness extension.
        bool lateReadiness{};
        // Diagnostics only: the batch's limiter arrival, on its first call and only
        // when the pacing profile is enabled. Other calls keep the epoch.
        Clock::time_point arrival{};
        // Diagnostics only (F9b): the real was lower-prepared before its deadline.
        bool internalPrepared{};
        bool generatedPrepared{};
        // Diagnostics only (D6, pacing profile): the longest worker call made under
        // queue->mutex for this output, by kind. Zero without the profile.
        uint64_t lockAcquireNs{}, lockSubmitNs{}, lockPresentNs{};
        // Diagnostics only (pacing profile): the output's identity and the lower present it made.
        uint64_t backendFrame{}, presentId{}, feedback{};
        // GP1, diagnostics only: the application's own present ID for a real (0 otherwise).
        uint64_t appPresentId{};
        double phase{};
        // GT1: the tick snap of this output (its snapped deadline minus the deadline before the snap) and its
        // target tick. Both are zero without the tick lock.
        Clock::duration tickSnap{};
        Clock::time_point targetTick{};
        // GT1: the start in unsnapped time. The bookkeeping (BatchPhaseLock, W2, the chain) reads this, never
        // the actual start, so the snap is a quantization only. Zero snap: the actual start.
        [[nodiscard]] Clock::time_point startedUnsnapped() const { return started - tickSnap; }
        // The start in unmoved time: the unsnapped start less the W2/OS1 correction.
        [[nodiscard]] Clock::time_point unmovedStart() const { return started - tickSnap - offset; }
    };
    struct Completion : Call {
        uint64_t source{};
        std::array<Call, capacity> calls{};
        size_t count{};
        Clock::duration readinessShift{};
        Clock::time_point readyAt{};
        Clock::duration slack{};
        bool readinessMiss{};
        // Diagnostics only: output spacing of the batch.
        Clock::duration outputInterval{};
        // The batch started at its own readiness (Readiness::justInTime). Its readiness shift is
        // not carried into the planned phase of the next batch.
        bool justInTime{};
        // BH2: the batch took the phase-locked hold (Readiness::batchHold), just in time or not.
        bool held{};
        // Diagnostics only: the batch's start minus its readiness, from the phase lock. Zero for
        // a batch that took no hold and for a readiness miss.
        Clock::duration batchPhaseHold{};
        // Presented generated outputs whose following real output in the same batch
        // started less than outputInterval/2 after the generated start.
        [[nodiscard]] size_t compressedGeneratedPairs() const {
            size_t pairs{};
            for (size_t i = 0; i < count; ++i) {
                const Call& generatedCall = calls[i];
                if (!generatedCall.generated || generatedCall.skipped) continue;
                for (size_t j = i + 1; j < count; ++j) {
                    if (calls[j].generated) continue;
                    pairs += !calls[j].skipped &&
                        calls[j].started - generatedCall.started < outputInterval / 2;
                    break;
                }
            }
            return pairs;
        }
    };
    struct Readiness {
        VkDevice device{};
        VkSemaphore semaphore{};
        uint64_t value{};
        PFN_vkWaitSemaphoresKHR wait{};
        Clock::time_point deadline{Clock::time_point::max()};
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
        // Its generated outputs are never skipped for give-up or expedite: they
        // are skipped only for readiness, stopping, or no lower image at the end of
        // their give-up window. Late readiness gets one bounded extension.
        bool overlap{};
        // Just in time: released batches with low_latency or batch_hold, and detached ready batches with
        // low_latency or batch_hold (BH3). The first output starts at a phase-locked time just after the batch's
        // readiness (BatchPhaseLock). Only the previous real output's start plus a quarter interval can
        // hold it later (see run()). The chain floor from the previous batch's planned phase is not applied.
        bool justInTime{};
        // batch_hold: the phase lock adds its 2 ms margin to the batch's start, just in time or not. Off starts
        // the batch at its readiness, with no lock; a just-in-time batch keeps the quarter-interval guard.
        bool batchHold{true};
        bool copyBeforeReadiness{};
        Clock::duration outputInterval{};
        // GT1: the refresh period the cap uses, in nanoseconds (0 when unknown). The tick lock starts from it.
        int64_t refreshNs{};
        // Diagnostics only: limiter arrival of the batch. Set only with the pacing profile.
        Clock::time_point arrival{};
        // Optional profiling only: observe existing final-copy completion
        // without waiting or changing presentation/readiness policy.
        VkFence copyFence{};
        PFN_vkGetFenceStatus copyStatus{};
        // Optional observation owns its surface-level diagnostic state. No
        // borrowed Swapchain state or application arrays reach the worker.
        std::shared_ptr<void> observer;
        VkResult (*observe)(void*, PFN_vkQueuePresentKHR, VkQueue,
            const VkPresentInfoKHR&, uint64_t, uint64_t, bool, uint64_t*){};
        uint64_t context{}, source{};
        bool trace{};
        // Diagnostics only: the pacing profile is enabled, so the worker measures its
        // queue-lock holds (D6). Without it the worker reads no extra clocks.
        bool profile{};
    };
    struct CompletionWait {
        Clock::duration waited{};
        Clock::time_point finished{};
    };
    // The generated image is acquired by the worker before its output. Eligible
    // internal overlap jobs try early; other jobs acquire at their deadline.
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
        // Optional. The signal of the previous generated copy or skip of the same batch; this job's copy or skip waits it exactly once, so copies of one batch execute in order.
        VkSemaphore preceding{};
        VkSemaphore continuation{};
        // Optional: only the last generated output of a batch carries the render slot fence.
        VkFence fence{};
        uint32_t imageCount{};
        std::array<VkCommandBuffer, maxCopyImages> copies{};
        std::array<VkSemaphore, maxCopyImages> presentWaits{};
        Clock::duration giveUp{};
        // S2: only Fixed FG with internal application images may prepare early.
        bool internalImages{};
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

    // submit is the device's vkQueueSubmit. X3 uses it to signal an application present fence
    // when the job that carries the fence ends skipped. Production callers always pass it.
    DeferredPresent(std::shared_ptr<DeferredPresentQueue> queue,
            PFN_vkQueuePresentKHR present, VkDevice device = VK_NULL_HANDLE,
            PFN_vkQueueSubmit submit = nullptr) :
            device(device), queue(std::move(queue)), present(present), queueSubmit(submit),
            worker() {
        // Initialize and report the setting before the worker can use it.
        static_cast<void>(realOffsetAuto());
        worker = std::thread([this] { run(); });
    }
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
        // OS1: the producers may still hold the ring; after this their pushes are no-ops.
        if (screenRing) screenRing->close();
        if (registeredSwapchain) {
            const std::lock_guard lock(deliveriesMutex());
            const DeliveryKey key{device, registeredSwapchain};
            const auto found = deliveries().find(key);
            if (found != deliveries().end() && found->second == delivery)
                deliveries().erase(found);
        }
    }

    // A successful application present may still be retained by this worker.
    // The lower wait API may only see an ID after its lower present returned.
    // Join that handoff on any calling thread, then let the driver perform
    // the actual presentation wait with the remainder of the caller's budget.
    // completionPacing false (framegen-vulkan's own probe polls) leaves the
    // newest record's completion pacing to the application's waits.
    // GP1: `wait` takes (lower ID, timeout). Outside the on-screen gate the lower ID is `id`
    // itself; with the gate it is the translation of `id` (see resolveOnScreenId).
    template<typename Wait>
    static VkResult waitForPresent(VkDevice device, VkSwapchainKHR swapchain,
            uint64_t id, uint64_t timeout, Wait&& wait, const bool completionPacing = true) {
        const auto started = Clock::now();
        const uint64_t budget = timeout;
        std::shared_ptr<Delivery> delivery;
        {
            const std::lock_guard lock(deliveriesMutex());
            const auto found = deliveries().find({device, swapchain});
            if (found != deliveries().end()) delivery = found->second;
        }
        std::shared_ptr<OnScreenIds> onScreen = id && OnScreenRegistry::anyActive()
            ? OnScreenRegistry::find({handleBits(device), handleBits(swapchain)}) : nullptr;
        if (!delivery || !id) {
            uint64_t lowerId = id;
            if (onScreen && !resolveOnScreenId(*onScreen, delivery.get(), id, started, budget, lowerId))
                return VK_TIMEOUT;
            return wait(lowerId, timeout);
        }
        bool waitedForDelivery{};
        {
            std::unique_lock lock(delivery->mutex);
            // An ID with no record was never identified by this worker, or its record was
            // delivered and dropped: the driver answers for it.
            const auto delivered = [&] {
                const size_t index = delivery->indexOf(id);
                return index == delivery->recordCount || delivery->records[index].submitted;
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
            const size_t index = delivery->indexOf(id);
            if (index < delivery->recordCount && delivery->records[index].result < 0)
                return delivery->records[index].result;
        }
        const auto deliveryWait = Clock::now() - started;
        if (timeout != UINT64_MAX) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count();
            timeout -= std::min(timeout, static_cast<uint64_t>(std::max<int64_t>(0, elapsed)));
        }
        uint64_t lowerId = id;
        if (onScreen && !resolveOnScreenId(*onScreen, delivery.get(), id, started, budget, lowerId))
            return VK_TIMEOUT;
        const auto result = wait(lowerId, timeout);
        if (completionPacing && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)) {
            const std::lock_guard lock(delivery->mutex);
            const size_t index = delivery->indexOf(id);
            // Completion pacing follows the newest record only, the latest real.
            if (index < delivery->recordCount && index + 1 == delivery->recordCount) {
                if (!delivery->waitFinished) {
                    delivery->waitFinished = true;
                    delivery->waitFinishedAt = Clock::now();
                }
                if (waitedForDelivery) {
                    delivery->waitObserved = true;
                    delivery->completionDelay = std::min(delivery->records[index].workerWait, deliveryWait);
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
        return {delivery->completionDelay, delivery->waitFinishedAt};
    }

    // Open one source batch, or a newer one while every pending batch is sealed and
    // overlappable, the ring has a free slot, and the completion queue has an entry
    // for every pending batch plus this one. Each admitted image's
    // binary signal must already be submitted. The producer seals on every
    // return/exception path.
    bool beginBatch(size_t limit, Readiness readiness) {
        const std::lock_guard lock(mutex);
        if (stopping || !limit || limit > capacity) return false;
        if (count == 0) {
            // The last active batch may retire after the producer polls for completions.
            // A successful completion can stay queued while the next overlap batch starts.
            if (completedQueue[0] && (!readiness.overlap || !completionRoomLocked() ||
                    std::any_of(completedQueue.begin(), completedQueue.end(), [](const auto& completion) {
                        return completion && (completion->result != VK_SUCCESS || completion->readinessMiss);
                    })))
                return false;
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
    // GP1: the lower ID owner and watcher of an active on-screen gate; both null otherwise. Set once,
    // before the first job is admitted (the admission lock orders it before the worker reads it).
    void setOnScreen(OnScreenIds* ids, OnScreenWatch* watch) {
        this->onScreenIds = ids;
        this->onScreenWatch = watch;
    }
    // OS1: the screen ring this worker reads for its automatic correction, or null for none. Set once, before the
    // first job is admitted, like setOnScreen.
    void setScreenSamples(std::shared_ptr<ScreenSampleRing> ring) {
        this->screenRing = std::move(ring);
    }
    // GT1: the gamescope tick lock of this swapchain (see tick_lock.hpp). Set once, before the first job is
    // admitted, like setScreenSamples; with no screen samples the lock never snaps.
    void setTickLock(const bool enabled) {
        this->tickLockEnabled = enabled;
    }
    // Only fixed-size metadata is admitted. Copy every scalar before returning
    // to the application; unknown nodes and caller-owned result arrays stay
    // synchronous. The same parser is used for policy and actual admission.
    [[nodiscard]] static bool supportsChain(const void* chain) {
        return snapshotChain(chain).has_value();
    }
    // identity is diagnostics only: the backend frame and the interpolation position of a generated output.
    bool enqueue(const VkPresentInfoKHR& info, Clock::time_point deadline, bool generated = false,
            OutputIdentity identity = {}) {
        return admit(info, deadline, generated, nullptr, identity);
    }
    // The real output of an internal application image. Admission matches
    // enqueue. The worker acquires a lower image, copies the application image
    // into it and presents it: the copy is prepared as soon as the worker takes
    // the job when a lower image is free (see prepareInternalReal), otherwise at
    // the deadline (see presentInternalReal). info.pWaitSemaphores[0] is the
    // continuation that copy waits.
    bool enqueueInternalReal(const VkPresentInfoKHR& info, Clock::time_point deadline,
            const InternalRealCopy& copy, OutputIdentity identity = {}) {
        return copy.tryAcquire && copy.submit && copy.abandon && copy.owner &&
            admit(info, deadline, false, &copy, identity);
    }
    // A generated output whose lower acquire/copy belongs to the worker. Only
    // the newest open batch admits it, so the real output follows in that batch.
    bool enqueueGeneratedCopy(VkSwapchainKHR swapchain, const void* chain,
            Clock::time_point deadline, const GeneratedCopy& copy, OutputIdentity identity = {}) {
        if (copy.imageCount == 0 || copy.imageCount > maxCopyImages || !copy.acquire || !copy.submit ||
                !copy.staged || !copy.continuation)
            return false;
        const auto metadata = snapshotChain(chain, this->ownedIdStorage);
        if (!metadata || applicationIdentified(*metadata)) return false;
        {
            const std::lock_guard lock(mutex);
            if (stopping || !newestOpenLocked()) return false;
            Batch& batch = batches[slotAt(count - 1)];
            if (batch.produced == batch.limit) return false;
            batch.jobs[batch.produced++] = {swapchain, UINT32_MAX, VK_NULL_HANDLE, *metadata, deadline, true,
                batch.readiness.trace ? Clock::now() : Clock::time_point{}, true, copy, false, {}, identity.backendFrame, identity.phase};
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
    // Every pending batch is sealed and overlappable, so they may keep outputting
    // while the next source is presented. Admission of a newer batch additionally
    // needs a free slot (see beginBatch).
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
    // Test seam only. Production sets stopping in ~DeferredPresent, after every batch
    // has retired, so a test cannot reach a prepared real's wait through the public API.
    friend struct DeferredPresentTestAccess;
    // Identified batches are admitted up to slots at a time, so the registry keeps one
    // record per batch that is still pending or still named by a wait.
    static constexpr size_t slots = 3;
    // One identified batch: the application's IDs for its real, and the worker's outcome.
    struct DeliveryRecord {
        uint64_t id{}, id2{};
        bool submitted{};
        VkResult result{VK_SUCCESS};
        Clock::duration workerWait{};
    };
    struct Delivery {
        std::mutex mutex;
        std::condition_variable changed;
        // Registration order, oldest first.
        std::array<DeliveryRecord, slots> records{};
        size_t recordCount{};
        // Completion pacing (takeCompletionPacingWait, applicationAcquire) follows the
        // newest record only: the latest real.
        bool waitObserved{}, waitFinished{}, acquiredBeforeWait{}, delayTaken{};
        Clock::duration completionDelay{};
        Clock::time_point waitFinishedAt{};
        // Index of the record whose id or id2 is presentId, or recordCount when none.
        [[nodiscard]] size_t indexOf(uint64_t presentId) const {
            for (size_t i = 0; i < recordCount; ++i)
                if (records[i].id == presentId || records[i].id2 == presentId) return i;
            return recordCount;
        }
        // Marks the oldest unsubmitted record with these IDs as delivered by the worker.
        void submitLocked(uint64_t id, uint64_t id2, VkResult result, Clock::duration workerWait) {
            for (size_t i = 0; i < recordCount; ++i) {
                DeliveryRecord& record = records[i];
                if (record.submitted || record.id != id || record.id2 != id2) continue;
                record.submitted = true;
                record.result = result;
                record.workerWait = workerWait;
                return;
            }
        }
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
    // GP1: the on-screen gate's owner and watcher (see setOnScreen). Null when the gate is inactive.
    OnScreenIds* onScreenIds{};
    OnScreenWatch* onScreenWatch{};
    std::shared_ptr<Delivery> delivery = std::make_shared<Delivery>();
    struct Metadata {
        VkFence fence{};
        uint64_t id{}, id2{};
        VkPresentModeKHR mode{};
        bool hasFence{}, hasId{}, hasId2{}, hasMode{};
        // ID2 whose value is framegen-vulkan's own probe storage, not an application wait.
        bool ownedId2{};
        bool hasMetering{};
        uint32_t framesPerBatch{};
#if defined(VK_EXT_present_timing)
        // The application's VkPresentTimingInfoEXT, copied; the real present rebuilds
        // VkPresentTimingsInfoEXT around it.
        bool hasTiming{};
        VkPresentTimingInfoEXT timing{};
#endif
    };
    // Only application IDs make a present identified: VkPresentIdKHR, or a
    // VkPresentId2KHR that framegen-vulkan did not supply. An identified present
    // registers a delivery record, which a wait for its ID joins.
    static bool applicationIdentified(const Metadata& metadata) {
        return (metadata.hasId && metadata.id) || (metadata.hasId2 && metadata.id2 && !metadata.ownedId2);
    }
    [[nodiscard]] static std::optional<Metadata> snapshotChain(const void* chain,
            const uint64_t* ownedIds = nullptr) {
        Metadata copy;
        // Six known node kinds, each at most once. Bound even malformed input.
        for (unsigned n = 0; chain; ++n) {
            if (n == 6) return std::nullopt;
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
#if defined(VK_EXT_present_timing)
            case VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT: {
                // VKD3D-Proton's feedback and target-time request. Its one timing info is
                // copied by value, so the inner struct may not carry a pNext of its own.
                VkPresentTimingsInfoEXT node{};
                std::memcpy(&node, chain, sizeof(node));
                if (copy.hasTiming || node.swapchainCount != 1 || !node.pTimingInfos ||
                        node.pTimingInfos[0].pNext)
                    return std::nullopt;
                copy.hasTiming = true; copy.timing = node.pTimingInfos[0];
                break;
            }
#endif
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
        // Diagnostics only: the backend frame and interpolation position of a generated output.
        uint64_t backendFrame{};
        double phase{};
    };
    // Shared admission for enqueue and enqueueInternalReal. The metadata snapshot,
    // present-ID registration and batch rules are the same for both.
    bool admit(const VkPresentInfoKHR& info, Clock::time_point deadline, bool generated,
            const InternalRealCopy* internal, OutputIdentity identity = {}) {
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
                if (open.produced == open.limit) return false;
            }
            if (implicit) openSlotLocked(1, Readiness{}, false);
            Batch& batch = batches[slotAt(count - 1)];
            if (identified && !registerDeliveryLocked(*metadata, info.pSwapchains[0])) {
                if (implicit) --count;
                return false;
            }
            Job job{info.pSwapchains[0], info.pImageIndices[0], info.pWaitSemaphores[0],
                *metadata, deadline, generated, batch.readiness.trace ? Clock::now() : Clock::time_point{}};
            if (internal) {
                job.internalActive = true;
                job.internal = *internal;
            }
            job.backendFrame = identity.backendFrame;
            job.phase = identity.phase;
            batch.jobs[batch.produced++] = std::move(job);
        }
        changed.notify_all();
        return true;
    }
    struct Batch {
        std::array<Job, capacity> jobs{};
        size_t produced{}, consumed{}, limit{1};
        bool open{};
        Completion aggregate{};
        Readiness readiness{};
        Clock::time_point expediteAt{Clock::time_point::max()};
    };
    // The oldest pending batch is `head`; `count` pending batches follow it in
    // ring order. Only the newest pending batch can still be open.
    std::array<Batch, slots> batches{};
    size_t head{}, count{};
    // Finished batches not yet taken, oldest first. completionRoomLocked keeps
    // untaken completions plus pending batches within these entries, so every
    // retiring batch finds a free entry.
    std::array<std::optional<Completion>, 4> completedQueue{};
    std::shared_ptr<DeferredPresentQueue> queue;
    PFN_vkQueuePresentKHR present;
    // X3: the device's vkQueueSubmit, used only to signal a skipped job's application present fence.
    PFN_vkQueueSubmit queueSubmit{};
    mutable std::mutex mutex;
    std::condition_variable changed;
    bool stopping{};
    // W2, FGVK_REAL_OFFSET_US=auto: worker thread only; the cursor is used under queue->mutex.
    OutputOffsetController outputOffsetController;
    // L3, low_latency: worker thread only, like outputOffsetController. The phase of consecutive
    // batches that take the hold (BH2: just in time or not, with batch_hold on).
    BatchPhaseLock batchPhase;
    // BH2, worker thread only: whether the batch that last reached batchPhase was just in time.
    // A change between just in time and not starts the next batch from no phase.
    bool batchPhaseJustInTime{};
    uint64_t markerCursor{};
    // OS1, worker thread only (with outputOffsetController): the screen ring and its cursor, and the
    // producer mode of this context's correction.
    std::shared_ptr<ScreenSampleRing> screenRing;
    uint64_t screenCursor{};
    ScreenOffsetMode screenMode;
    // GT1, worker thread only (like the controller): the gamescope tick grid and lead. tickLockEnabled is set once,
    // before the first job is admitted (setTickLock).
    bool tickLockEnabled{};
    TickLock tickLock;
    // Caller holds queue->mutex, right after a worker present was marked. OS1: the screen samples of this
    // context are read first; while they drive the correction, the marker samples are drained and ignored.
    void observeOutputOffsetLocked(const uint64_t context) noexcept {
        if (!realOffsetAuto()) return;
        const bool screen = screenRing && observeScreenSamplesLocked(context);
        std::array<PresentMarkerSample, 16> samples;
        for (;;) {
            const auto count = queue->readMarkerSamplesLocked(&markerCursor, samples.data(), samples.size());
            for (size_t i = 0; i < count; ++i) {
                const auto& sample = samples[i];
                if (sample.context != context || screen) continue;
                const auto heldBefore = outputOffsetController.held;
                if (outputOffsetController.observe(sample.generated, sample.source,
                        std::chrono::nanoseconds{sample.gpuNs})) {
                    realOffsetAutoNs.store(outputOffsetController.equivalentRealOffset().count(),
                        std::memory_order_relaxed);
                    realOffsetAutoSamples.fetch_add(1, std::memory_order_relaxed);
                }
                if (outputOffsetController.held != heldBefore)
                    realOffsetAutoHeld.fetch_add(outputOffsetController.held - heldBefore, std::memory_order_relaxed);
            }
            if (count < samples.size()) return;
        }
    }
    // OS1, caller holds queue->mutex: feeds this context's screen samples to the controller in present order.
    // The first sample starts screen mode and each switch forgets the pending markers (offsets are kept). No
    // sample for ScreenOffsetMode::timeoutNs ends it. Returns whether screen mode is on after this call.
    bool observeScreenSamplesLocked(const uint64_t context) noexcept {
        const int64_t now = steadyNanoseconds(Clock::now());
        std::array<ScreenSample, 16> samples;
        for (;;) {
            const size_t count = screenRing->read(&screenCursor, samples.data(), samples.size());
            for (size_t i = 0; i < count; ++i) {
                const ScreenSample& sample = samples[i];
                if (sample.context != context) continue;
                if (screenMode.sample(now)) {
                    outputOffsetController.forget();
                    tickLock.resetOutputs();
                }
                if (tickLockEnabled) observeTickLockLocked(sample);
                if (outputOffsetController.observe(sample.generated, sample.source,
                        std::chrono::nanoseconds{sample.screenNs})) {
                    realOffsetScreenSamples.fetch_add(1, std::memory_order_relaxed);
                    realOffsetAutoNs.store(outputOffsetController.equivalentRealOffset().count(),
                        std::memory_order_relaxed);
                    realOffsetAutoSamples.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (count < samples.size()) break;
        }
        if (screenMode.expire(now)) {
            outputOffsetController.forget();
            tickLock.resetOutputs();
        }
        if (tickLockEnabled) {
            tickLockOn.store(tickLock.gridValid(), std::memory_order_relaxed);
            tickLockPeriodNs.store(tickLock.currentPeriod(), std::memory_order_relaxed);
            tickLockLeadNs.store(tickLock.currentLead(), std::memory_order_relaxed);
        }
        realOffsetScreen.store(screenMode.screen(), std::memory_order_relaxed);
        realOffsetScreenDropped.store(screenRing->dropped(), std::memory_order_relaxed);
        return screenMode.screen();
    }
    // GT1, caller holds queue->mutex: feeds one screen sample of this context to the tick grid and the lead, and counts
    // what a snapped output's sample says. Worker thread only; the counts are relaxed atomics read by the summary.
    void observeTickLockLocked(const ScreenSample& sample) noexcept {
        const auto verdict = tickLock.observe(TickSample{sample.screenNs, sample.startedNs, sample.targetNs});
        if (verdict == TickVerdict::miss) tickLockMisses.fetch_add(1, std::memory_order_relaxed);
        else if (verdict == TickVerdict::early) tickLockEarly.fetch_add(1, std::memory_order_relaxed);
    }
    std::thread worker;

    [[nodiscard]] size_t slotAt(size_t offset) const { return (head + offset) % slots; }
    [[nodiscard]] bool newestOpenLocked() const { return count && batches[slotAt(count - 1)].open; }
    // Every pending batch must be sealed and overlappable. Batches retained this
    // way are joined oldest first, keeping the newest two outputting.
    [[nodiscard]] bool retainableLocked() const {
        if (count == 0) return false;
        for (size_t offset = 0; offset < count; ++offset) {
            const Batch& batch = batches[slotAt(offset)];
            if (batch.open || !batch.readiness.overlap) return false;
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
    // GP1: the application's own present ID of a real output (the ID the application waits on), or 0.
    static uint64_t applicationIdOf(const Metadata& metadata) {
        if (metadata.hasId && metadata.id) return metadata.id;
        if (metadata.hasId2 && metadata.id2 && !metadata.ownedId2) return metadata.id2;
        return 0;
    }
    // GP1: the lower ID of an application ID after its delivery wait (present-wait semantics, see
    // OnScreenIds::translate). A miss is logged once and retried in 1 ms steps, on the delivery
    // condition when there is one, until the ID is recorded or the budget ends. A zero budget polls
    // once. Returns false when the budget ended first.
    static bool resolveOnScreenId(OnScreenIds& ids, Delivery* const delivery, const uint64_t appId,
            const Clock::time_point started, const uint64_t budget, uint64_t& lowerId) {
        const bool bounded = budget != UINT64_MAX;
        const auto deadline = started + std::chrono::nanoseconds(std::min<uint64_t>(budget, INT64_MAX));
        for (;;) {
            if (const auto found = ids.translate(appId)) {
                lowerId = *found;
                return true;
            }
            if (ids.firstMiss())
                std::clog << "framegen-vulkan: gamescope on-screen profile: translation miss app_id="
                          << appId << '\n';
            if (bounded && Clock::now() >= deadline) return false;
            if (delivery) {
                std::unique_lock lock(delivery->mutex);
                delivery->changed.wait_for(lock, std::chrono::milliseconds{1});
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
    }
    // GP1, caller holds queue->mutex: a successful present with an on-screen lower ID records a
    // real's application ID for translation and pushes the ID to the watcher. Inactive: nothing.
    // GT1: the watcher also gets the output's start and its target tick (zero when not snapped).
    void noteOnScreenLocked(const Job& job, const Readiness& ready, const uint64_t lowerId,
            const VkResult result, const Clock::time_point started, const Clock::time_point target) noexcept {
        if (!onScreenIds || !lowerId || result < 0) return;
        if (!job.generated) onScreenIds->record(applicationIdOf(job.metadata), lowerId);
        onScreenWatch->push(lowerId, ready.context, ready.source, job.generated,
            steadyNanoseconds(target), steadyNanoseconds(started));
    }

    // One registry entry per live worker, shared by all of its identified batches.
    // Each identified batch appends a record. A full list drops its oldest submitted
    // record, and refuses the batch while every record is still unsubmitted. Waiters
    // retain shared state without acquiring the Swapchain owner or the queue lock.
    bool registerDeliveryLocked(const Metadata& metadata, VkSwapchainKHR swapchain) {
        if (registeredSwapchain && registeredSwapchain != swapchain) return false;
        const std::lock_guard deliveryLock(delivery->mutex);
        if (delivery->recordCount == slots) {
            const auto oldest = std::find_if(delivery->records.begin(), delivery->records.end(),
                [](const DeliveryRecord& record) { return record.submitted; });
            if (oldest == delivery->records.end()) return false;
            std::move(oldest + 1, delivery->records.end(), oldest);
            --delivery->recordCount;
        }
        try {
            const std::lock_guard registryLock(deliveriesMutex());
            deliveries().insert_or_assign({device, swapchain}, delivery);
        } catch (...) { return false; }
        registeredSwapchain = swapchain;
        delivery->records[delivery->recordCount++] = DeliveryRecord{
            .id = metadata.hasId ? metadata.id : 0,
            .id2 = metadata.hasId2 && !metadata.ownedId2 ? metadata.id2 : 0,
        };
        // The newest record starts its completion pacing from scratch.
        delivery->waitObserved = delivery->waitFinished = delivery->acquiredBeforeWait = delivery->delayTaken = false;
        delivery->completionDelay = {};
        delivery->waitFinishedAt = {};
        return true;
    }

    // Callers hold queue->mutex and never sleep there. Every submit waits the
    // binary staged semaphore, the acquire too when it succeeded, and the preceding
    // copy of the same batch when set. A copy signals that image's present wait and
    // the continuation. A skip signals only the continuation. Every submit signals
    // the fence. The private queue's tail is signalled too (F9c, see submitPrivate).
    // D7b: lowerIndex is the lower image of a
    // copy and UINT32_MAX for a skip, which has no image. D7c: reason is the skip's SkipReason
    // (0 for a copy) and source the batch ordinal; both only label the submit's history record.
    static VkResult submitGeneratedWork(DeferredPresentQueue& queue, const GeneratedCopy& copy,
            const bool acquired, const VkCommandBuffer command, const VkSemaphore presentWait,
            const uint32_t lowerIndex, const uint8_t reason, const uint64_t source) {
        // Waits, in order: the acquire when acquired, the preceding copy when set, then
        // staged, which is always the last wait. Signals: the optional present wait,
        // then the continuation.
        std::array<VkSemaphore, 3> waits{};
        uint32_t waitCount = 0;
        if (acquired) waits[waitCount++] = copy.acquireSemaphore;
        if (copy.preceding != VK_NULL_HANDLE) waits[waitCount++] = copy.preceding;
        waits[waitCount++] = copy.staged;
        const std::array<VkPipelineStageFlags, 3> stages{
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
        const std::array<VkSemaphore, 2> signals{presentWait, copy.continuation};
        const uint32_t signalCount = presentWait == VK_NULL_HANDLE ? 1u : 2u;
        const VkSubmitInfo submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreCount = waitCount,
            .pWaitSemaphores = waits.data(),
            .pWaitDstStageMask = stages.data(),
            .commandBufferCount = command != VK_NULL_HANDLE ? 1u : 0u,
            .pCommandBuffers = command != VK_NULL_HANDLE ? &command : nullptr,
            .signalSemaphoreCount = signalCount,
            .pSignalSemaphores = signals.data() + (2 - signalCount),
        };
        const bool copying = command != VK_NULL_HANDLE;
        const SubmitTag tag{
            .kind = copying ? submitKindGeneratedCopy : submitKindGeneratedSkip,
            .applicationIndex = UINT32_MAX,
            .lowerIndex = copying ? lowerIndex : UINT32_MAX,
            .reason = reason,
            .source = source,
        };
        return submitPrivate(queue, copy.submit, submit, copy.fence, tag);
    }

    // D6: runs one worker call made under queue->mutex and keeps the longest such
    // call in longestNs, so a poll loop reports its worst attempt. Clock reads happen
    // only when profile is set.
    template<typename Fn>
    static auto profiledLockCall(const bool profile, uint64_t& longestNs, Fn&& fn) {
        if (!profile) return fn();
        const auto started = Clock::now();
        const auto value = fn();
        longestNs = std::max(longestNs, nanosecondsOf(Clock::now() - started));
        return value;
    }
    static uint64_t nanosecondsOf(const Clock::duration duration) {
        return static_cast<uint64_t>(std::max<int64_t>(0,
            std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count()));
    }

    // F9b: the outcome of one preparation attempt, kept by the worker until the deadline.
    struct InternalPreparation {
        // A lower image is acquired and its copy submitted: the deadline presents it.
        bool prepared{};
        bool acquired{};
        // An error ended the job before its wait: nothing waits and nothing presents.
        bool settled{};
        VkResult error{VK_SUCCESS};
        // Set when tryAcquire succeeded. A submit error reports it as the image index.
        uint32_t lowerIndex{};
        VkSemaphore presentWait{};
        uint64_t acquireNs{}, submitNs{};
        Clock::time_point copySubmitStarted{};
    };
    // F9b: one non-blocking attempt, made when the worker takes an internal real, so
    // its lower acquire and copy finish before the deadline. Not-ready results leave
    // the real to the at-deadline poll. A tryAcquire error abandons the application
    // image once. A submit error does not abandon: submitCopyLocked already freed the
    // image at the last submitted release value. Callers hold no lock and this never
    // sleeps. Cost: a prepared real holds one lower image from here until its deadline,
    // about one output interval. Right after a generated present the driver may often
    // have no free lower image, so some reals fall back to the at-deadline poll; the
    // internal_real_prepared= and internal_real_at_deadline= counters show the share.
    // A real that is late because the lower present itself blocks is not helped. No
    // latency is added: the present still happens at its deadline.
    InternalPreparation prepareInternalReal(const Job& job, const bool profile) {
        const InternalRealCopy& copy = job.internal;
        void* const owner = copy.owner.get();
        InternalPreparation prep;
        const std::lock_guard queueLock(queue->mutex);
        uint32_t lowerIndex{};
        const VkResult acquired = profiledLockCall(profile, prep.acquireNs, [&] {
            return copy.tryAcquire(owner, &lowerIndex);
        });
        if (acquired < 0) {
            prep.settled = true;
            prep.error = acquired;
            static_cast<void>(profiledLockCall(profile, prep.submitNs, [&] {
                return copy.abandon(owner, queue->queue, copy.applicationIndex, job.wait);
            }));
            return prep;
        }
        if (acquired != VK_SUCCESS) return prep;
        prep.lowerIndex = lowerIndex;
        const VkResult submitted = profiledLockCall(profile, prep.submitNs, [&] {
            return copy.submit(owner, queue->queue, copy.applicationIndex, lowerIndex,
                job.wait, &prep.presentWait);
        });
        if (submitted < 0) {
            prep.settled = true;
            prep.error = submitted;
            return prep;
        }
        prep.prepared = true;
        return prep;
    }

    // S2: prepare only the current generated output. The single worker finishes its
    // lower present before it takes the next job (including an internal real), so
    // preparations never accumulate across outputs or batches. An acquired copy is
    // presented even when stopping, exactly like an F9b prepared real.
    InternalPreparation prepareGenerated(const Job& job, const Readiness& ready) {
        InternalPreparation prep;
        const std::lock_guard queueLock(queue->mutex);
        const bool noted = queue->history;
        const auto started = noted ? Clock::now() : Clock::time_point{};
        queue->beginCall(lowerOpAcquireGenerated, UINT32_MAX);
        const auto acquired = profiledLockCall(ready.profile, prep.acquireNs, [&] {
            return job.copy.acquire(job.copy.device, job.swapchain, 0,
                job.copy.acquireSemaphore, VK_NULL_HANDLE, &prep.lowerIndex);
        });
        queue->endCall();
        prep.acquired = acquired == VK_SUCCESS || acquired == VK_SUBOPTIMAL_KHR;
        if (noted && acquired != VK_NOT_READY && acquired != VK_TIMEOUT)
            queue->noteLower(lowerOpAcquireGenerated, job.swapchain,
                prep.acquired ? prep.lowerIndex : UINT32_MAX, acquired, ready.source,
                steadyNanoseconds(started), steadyNanoseconds(Clock::now()));
        if (acquired < 0 || (prep.acquired && prep.lowerIndex >= job.copy.imageCount)) {
            prep.settled = true;
            prep.error = acquired < 0 ? acquired : VK_ERROR_UNKNOWN;
            return prep;
        }
        if (!prep.acquired) return prep; // The unchanged path retries at the deadline.
        prep.presentWait = job.copy.presentWaits.at(prep.lowerIndex);
        const auto submitted = profiledLockCall(ready.profile, prep.submitNs, [&] {
            if (ready.profile) prep.copySubmitStarted = Clock::now();
            return submitGeneratedWork(*queue, job.copy, true,
                job.copy.copies.at(prep.lowerIndex), prep.presentWait, prep.lowerIndex,
                0, ready.source);
        });
        if (submitted != VK_SUCCESS) {
            prep.settled = true;
            prep.error = submitted;
            return prep;
        }
        prep.prepared = true;
        return prep;
    }

    // The present half of an internal real, shared by the prepared and the at-deadline
    // paths, so the chain, observer and copy status are the same on both. Callers hold
    // queue->mutex. presentWait is the signal of the copy into lowerIndex.
    // onScreenId is the chain's on-screen ID node value when the gate is active (see run()), else null.
    void presentLower(const Job& job, const Readiness& ready, const void* chain,
            const uint32_t lowerIndex, const VkSemaphore presentWait, Completion& result,
            uint64_t* const onScreenId) {
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
        // GP1: the lower ID is handed out here, under queue->mutex, in the order the presents are made.
        if (onScreenIds) *onScreenId = onScreenIds->allocate();
        queue->beginCall(lowerOpPresentReal, lowerIndex);
        result.presentId = present_diagnostics::lowerPresentId(chain);
        result.result = ready.observer && ready.observe
            ? ready.observe(ready.observer.get(), present, queue->queue,
                info, ready.context, ready.source, job.generated, &result.feedback)
            : present(queue->queue, &info);
        queue->endCall();
        result.finished = Clock::now();
        queue->markPresentLocked(ready.context, ready.source, result.feedback, job.generated);
        observeOutputOffsetLocked(ready.context);
        if (onScreenIds) noteOnScreenLocked(job, ready, *onScreenId, result.result, result.started, result.targetTick);
        // D7d: the lower present is noted with its source, only when history is set.
        if (queue->history)
            queue->noteLower(lowerOpPresentReal, job.swapchain, lowerIndex, result.result, ready.source,
                steadyNanoseconds(result.started), steadyNanoseconds(result.finished));
        // The present call alone: started and finished bracket it.
        if (ready.profile) result.lockPresentNs = nanosecondsOf(result.finished - result.started);
        if (ready.copyStatus && ready.copyFence) {
            result.copyChecked = true;
            result.copyStatus = ready.copyStatus(ready.device, ready.copyFence);
            result.copySampledAt = Clock::now();
        }
    }

    // The real output of an internal application image, produced by this worker
    // at its deadline unless prepareInternalReal already copied it. It never gives up
    // or skips: the loop ends when a lower image is copied and presented, or when the
    // application image is abandoned on a lower error or while stopping. Each attempt
    // holds queue->mutex for tryAcquire and the copy/present only, and sleeps outside it.
    void presentInternalReal(Job& job, const Readiness& ready, const void* chain, Completion& result,
            uint64_t* const onScreenId) {
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
            SkipReason reason = SkipReason::Error;
            {
                const std::lock_guard queueLock(queue->mutex);
                if (ready.trace) result.queueLockAcquired = Clock::now();
                if (stopped) {
                    error = VK_ERROR_UNKNOWN;
                    reason = SkipReason::Stopping;
                } else {
                    const VkResult acquired = profiledLockCall(ready.profile, result.lockAcquireNs, [&] {
                        return copy.tryAcquire(owner, &lowerIndex);
                    });
                    if (acquired == VK_SUCCESS) {
                        const VkResult submitted = profiledLockCall(ready.profile, result.lockSubmitNs, [&] {
                            return copy.submit(owner, queue->queue, copy.applicationIndex, lowerIndex,
                                job.wait, &presentWait);
                        });
                        if (submitted < 0) {
                            result.imageIndex = lowerIndex;
                            result.skipped = true;
                            result.skipReason = SkipReason::Error;
                            result.started = result.finished = Clock::now();
                            result.result = submitted;
                            return;
                        }
                        presentLower(job, ready, chain, lowerIndex, presentWait, result, onScreenId);
                        return;
                    }
                    if (acquired < 0) {
                        error = acquired;
                    }
                }
                if (error != VK_SUCCESS) {
                    // Frees the application image without a present. Nothing is left acquired.
                    static_cast<void>(profiledLockCall(ready.profile, result.lockSubmitNs, [&] {
                        return copy.abandon(owner, queue->queue, copy.applicationIndex, job.wait);
                    }));
                    result.skipped = true;
                    result.skipReason = reason;
                    result.started = result.finished = Clock::now();
                    result.result = error;
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds{250});
        }
    }

    void run() {
        present_diagnostics::registerDeferredWorkerThread();
        std::unique_lock lock(mutex);
        Clock::duration readinessShift{};
        Clock::time_point readyAt{};
        Clock::duration slack{};
        // L3: a held batch's start minus readyAt (diagnostics only).
        Clock::duration batchPhaseHold{};
        // BH2: the first output's readiness block took the phase-locked hold.
        bool batchHeld{};
        // Shift from the batch's first output to its final deadline. Later
        // outputs of the same batch keep their requested spacing from it.
        Clock::duration batchShift{};
        bool readinessMiss{};
        // F7 rule 3: the first output's readiness succeeded only in the extension.
        bool lateReadiness{};
        VkResult readinessResult{VK_SUCCESS};
        Clock::duration workerWait{};
        Clock::time_point readinessStarted{}, readinessFinished{};
        // Last output of the previous batch on this worker, for cross-batch spacing. previousStarted is the
        // start in unsnapped time (GT1); previousActualStarted is the actual start, the floor of the tick snap.
        Clock::time_point previousStarted{}, previousActualStarted{}, previousFinished{}, previousScheduled{};
        bool havePrevious{};
        uint32_t heldLowerImages{};
        Clock::time_point lastBatchFinished{};
        for (;;) {
            changed.wait(lock, [this] {
                return stopping || (count && batches[head].consumed < batches[head].produced);
            });
            if (stopping) return;
            Batch& batch = batches[head];
            const size_t index = batch.consumed;
            const auto current = batch.jobs[index];
            const bool first = index == 0;
            if (first && (lastBatchFinished != Clock::time_point{} &&
                    Clock::now() - lastBatchFinished > std::chrono::milliseconds{250})) {
                outputOffsetController.forget();
                batchPhase.reset();
                tickLock.resetOutputs();
            }
            const auto ready = batch.readiness;
            // GT1: a new refresh period starts the tick grid again (rule 6). Only the first output of a batch asks.
            if (first && tickLockEnabled) tickLock.setRefresh(ready.refreshNs);
            const size_t limit = batch.limit;
            outputOffsetController.setExpected(limit > 1 ? static_cast<uint32_t>(limit - 1) : 1);
            const bool copyTail = index > 0 && batch.jobs[index - 1].copyActive &&
                !batch.aggregate.calls[index - 1].skipped;
            auto deadline = first ? current.deadline :
                nextOutputDeadline(current.deadline + batchShift,
                    batch.jobs[index - 1].deadline + batchShift,
                    batch.aggregate.calls[index - 1].unmovedStart());
            // A just-in-time batch does not chain from the previous batch's planned phase (see the readiness below).
            if (first && havePrevious && ready.overlap && !ready.justInTime &&
                    ready.outputInterval > Clock::duration::zero() &&
                    Clock::now() - previousFinished < ready.outputInterval)
                deadline = std::max(deadline, previousScheduled + ready.outputInterval -
                    ready.outputInterval / 16);
            // The previous output's start in unmoved time: its own correction is taken back.
            const auto unmovedGeneratedStart = copyTail
                ? batch.aggregate.calls[index - 1].unmovedStart()
                : Clock::time_point{};
            if (copyTail) {
                // The real after a generated output keeps full spacing, but a release limit
                // may pull it in: no later than latestOutput, and at least a quarter output
                // interval after the generated present started. Generated outputs may start
                // up to a quarter output interval before the batch limit. FIFO still shows
                // each image at least one refresh.
                deadline = std::max(deadline,
                    batch.aggregate.calls[index - 1].unmovedStart() + ready.outputInterval);
                const auto limitTail = [&](const Clock::time_point at) {
                    return ready.latestOutput == Clock::time_point::max() ? at
                        : std::min(at, std::max(ready.latestOutput, unmovedGeneratedStart + ready.outputInterval / 4));
                };
                deadline = limitTail(deadline);
            } else {
                deadline = std::min(deadline, ready.latestOutput);
            }
            // W2: every rule above is unmoved time. A generated output takes its own correction last: bounded
            // by the previous output's actual start (the first output of a stream has no bound), then by the
            // release limit (after a generated output, with its quarter-interval floor). A real output is never
            // moved. OS1c: a plain deferred generated present (no worker copy, `!copyActive`) takes it too. Its
            // lower image is already acquired and copied, so a later present holds only that image and changes
            // no semaphore order. OS2: the first output of a batch with a readiness wait takes it after that wait
            // instead, from its readiness start.
            const auto unmovedDeadline = deadline;
            Clock::duration commandedOffset{}, appliedOffset{};
            const bool offsetOutput = current.generated && (!current.copyActive || current.copy.internalImages) &&
                realOffsetAuto() && index < OutputOffsetController::maxPositions;
            // OS2: the first output of a batch with a readiness wait takes its correction after that wait (in the
            // readiness block below). Its move here would be replaced by the readiness start.
            const bool offsetAfterReadiness = offsetOutput && first && ready.wait;
            if (offsetOutput) commandedOffset = outputOffsetController.offset[index];
            if (offsetOutput && !offsetAfterReadiness) {
                const bool hasPrevious = index > 0 || havePrevious;
                const auto previousActual = index > 0 ? batch.aggregate.calls[index - 1].startedUnsnapped() : previousStarted;
                const auto moved = hasPrevious
                    ? deadlineWithOffset(unmovedDeadline, previousActual, ready.outputInterval, commandedOffset)
                    : unmovedDeadline + commandedOffset;
                const auto cap = copyTail
                    ? std::max(ready.latestOutput, unmovedGeneratedStart + ready.outputInterval / 4)
                    : ready.latestOutput;
                deadline = std::min(moved, cap);
                appliedOffset = deadline - unmovedDeadline;
            }
            lock.unlock();
            // A preparation holds at most one lower image for the current output.
            InternalPreparation prep{};
            const bool earlyGenerated = current.copyActive && current.copy.internalImages && ready.overlap;
            const auto prepare = [&] {
                {
                    const std::lock_guard stopLock(mutex);
                    if (stopping) return;
                }
                assert(heldLowerImages == 0);
                assert(heldLowerImages < layerHeldLowerImages);
                prep = current.internalActive ? prepareInternalReal(current, ready.profile)
                                               : prepareGenerated(current, ready);
                if (prep.prepared || prep.acquired) ++heldLowerImages;
                assert(heldLowerImages <= layerHeldLowerImages);
            };
            // The producer submitted the staging signal before publishing this job. A queued
            // copy waits on staging and acquire, and owns its present even if the host wait times out.
            if (ready.copyBeforeReadiness && first && limit == 2 && earlyGenerated && ready.wait)
                prepare();
            // The batch's unmoved deadline after its readiness (today's deadline with no correction).
            auto unmovedReady = unmovedDeadline;
            if (first) {
                workerWait = {};
                readinessShift = {};
                readyAt = {};
                slack = {};
                batchPhaseHold = {};
                batchHeld = false;
                // L3, BH2: a batch that takes no hold, or whose just-in-time mode differs from the previous batch's,
                // starts from no phase.
                if ((!ready.justInTime && !ready.batchHold) || ready.justInTime != batchPhaseJustInTime)
                    batchPhase.reset();
                batchPhaseJustInTime = ready.justInTime;
                batchShift = {};
                readinessMiss = false;
                lateReadiness = false;
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
                    // F7 rule 3: a released batch waits once more on the same value,
                    // bounded by its output budget and the 50 ms cap already spent.
                    // Only a timeout qualifies; errors and the second timeout stay misses.
                    if (readinessResult == VK_TIMEOUT && ready.overlap &&
                            ready.outputInterval > Clock::duration::zero()) {
                        const auto waited = readinessFinished - waitStarted;
                        const auto extension = std::min(ready.outputInterval * static_cast<int64_t>(limit),
                            std::chrono::milliseconds{50} - waited);
                        if (extension > Clock::duration::zero()) {
                            const auto extensionStarted = Clock::now();
                            readinessResult = ready.wait(ready.device, &waitInfo, static_cast<uint64_t>(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(extension).count()));
                            readinessFinished = Clock::now();
                            workerWait += readinessFinished - extensionStarted;
                            lateReadiness = readinessResult == VK_SUCCESS;
                        }
                    }
                    readinessMiss = readinessResult != VK_SUCCESS;
                    if (readinessMiss) {
                        batchPhase.reset();
                    } else {
                        readyAt = Clock::now();
                        slack = unmovedReady - readyAt;
                        // L3: a just-in-time batch starts at its phase-locked time after its readiness
                        // (batch_phase.hpp). Only the quarter-interval guard after the previous real
                        // output's actual start can hold it later, and the lock then follows that start.
                        // BH2: a batch that is not just in time (only the cap-scheduled path, whose deadlines are the
                        // cap grid, since BH3) takes the same phase-locked start with batch_hold on, and no guard
                        // (the schedule spaces those). Its scheduled deadline below can hold it later.
                        // batchShift below carries the start to the batch's later outputs.
                        // batch_hold off: the start is the readiness itself, with no lock and no margin.
                        // The lock stays reset, so a later batch_hold on starts from no phase.
                        auto start = readyAt;
                        if (ready.justInTime) {
                            if (ready.batchHold) {
                                start = batchPhase.start(readyAt, ready.outputInterval * static_cast<int64_t>(limit));
                            } else {
                                batchPhase.reset();
                            }
                            if (havePrevious) {
                                const auto guard = previousStarted + ready.outputInterval / 4;
                                if (guard > start) {
                                    start = guard;
                                    if (ready.batchHold) batchPhase.forceStart(start);
                                }
                            }
                        } else if (ready.batchHold) {
                            start = batchPhase.start(readyAt, ready.outputInterval * static_cast<int64_t>(limit));
                        }
                        batchHeld = ready.batchHold;
                        batchPhaseHold = start - readyAt;
                        readinessShift = std::max(Clock::duration{}, start - unmovedReady);
                        unmovedReady += readinessShift;
                        deadline = std::max(deadline, start);
                        // BH2: a held batch that is not just in time starts at the later of its schedule and the
                        // lock. When the schedule is later, the lock follows it, as the guard does. Never the unset maximum.
                        if (!ready.justInTime && ready.batchHold && deadline > start &&
                                deadline != Clock::time_point::max())
                            batchPhase.forceStart(deadline);
                        if (offsetAfterReadiness) {
                            // OS2: the correction moves the readiness start, never before readyAt. Its
                            // effective part is measured from unmovedReady (rule 3 below).
                            deadline = readyDeadlineWithOffset(readyAt, unmovedReady, previousStarted, havePrevious,
                                ready.latestOutput, ready.outputInterval, commandedOffset);
                            appliedOffset = deadline - unmovedReady;
                        }
                    }
                }
                batchShift = unmovedReady - current.deadline;
            }
            // F9b: try a lower image before the present deadline. An unavailable image on
            // the pre-readiness attempt can take this normal path.
            if ((current.internalActive || earlyGenerated) && !readinessMiss && !prep.prepared && !prep.settled)
                prepare();
            // GT1, rule 4: the snapped deadline is the last rule before the sleep, after OS2 and the W2 move. The deadline
            // before the snap stays the scheduled time and the bookkeeping reference (Call::unmovedStart). The floor is the
            // batch's readyAt for its first output and the previous output's actual start plus P/2 for a later one.
            const auto scheduledDeadline = deadline;
            Clock::duration tickSnap{};
            Clock::time_point targetTick{};
            if (!readinessMiss && !prep.settled && tickLockEnabled && tickLock.snapping() &&
                    scheduledDeadline != Clock::time_point::max()) {
                const int64_t floorNs = first ? (ready.wait ? steadyNanoseconds(readyAt) : int64_t{0})
                    : havePrevious ? steadyNanoseconds(previousActualStarted) + tickLock.currentPeriod() / 2
                    : int64_t{0};
                const auto slot = tickLock.snap(steadyNanoseconds(deadline), floorNs);
                targetTick = Clock::time_point{std::chrono::nanoseconds{slot.target}};
                deadline = Clock::time_point{std::chrono::nanoseconds{slot.deadline}};
                tickSnap = deadline - scheduledDeadline;
                tickLockSnapped.fetch_add(1, std::memory_order_relaxed);
            }
            if (!readinessMiss && !prep.settled) {
                const auto waitStarted = Clock::now();
                if (ready.expediteOnJoin && !copyTail) {
                    lock.lock();
                    for (auto until = std::min(deadline, batch.expediteAt);
                            !stopping && Clock::now() < until; until = std::min(deadline, batch.expediteAt))
                        changed.wait_until(lock, until);
                    lock.unlock();
                } else if (copyTail && ready.overlap && ready.expediteOnJoin) {
                    // F7 rule 4: a join during this tail pulls it in to at least a quarter
                    // output interval after its generated start. The bound is recomputed on
                    // every wake and never exceeds the normal deadline.
                    // W2: in unmoved time, like the copyTail floor and limitTail.
                    const auto generatedStarted = unmovedGeneratedStart;
                    lock.lock();
                    for (;;) {
                        const auto until = std::min(deadline, std::max(batch.expediteAt,
                            generatedStarted + ready.outputInterval / 4));
                        if (stopping || Clock::now() >= until) break;
                        changed.wait_until(lock, until);
                    }
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
            // GP1: with the on-screen gate the application's ID nodes are not forwarded. One node
            // carries the lower ID instead; its value is written under queue->mutex at the present.
            uint64_t onScreenLowerId{};
            VkPresentIdKHR id{
                .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
                .pNext = chain, .swapchainCount = 1, .pPresentIds = &metadata.id,
            };
            if (metadata.hasId && !onScreenIds) chain = &id;
#if defined(VK_KHR_present_id2)
            VkPresentId2KHR id2{
                .sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
                .pNext = chain, .swapchainCount = 1, .pPresentIds = &metadata.id2,
            };
            if (metadata.hasId2 && !onScreenIds) chain = &id2;
            VkPresentId2KHR onScreenNode{
                .sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
                .pNext = chain, .swapchainCount = 1, .pPresentIds = &onScreenLowerId,
            };
            if (onScreenIds) chain = &onScreenNode;
#endif
            VkSwapchainPresentModeInfoEXT mode{
                .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT,
                .pNext = chain, .swapchainCount = 1, .pPresentModes = &metadata.mode,
            };
            if (metadata.hasMode) chain = &mode;
#if defined(VK_EXT_present_timing)
            VkPresentTimingsInfoEXT timings{
                .sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT,
                .pNext = chain, .swapchainCount = 1, .pTimingInfos = &metadata.timing,
            };
            if (metadata.hasTiming) chain = &timings;
#endif
            auto job = current;
            Completion result;
            result.source = ready.source;
            result.enqueued = current.enqueued;
            result.readinessChecked = first && ready.wait;
            result.readinessStarted = readinessStarted;
            result.readinessFinished = readinessFinished;
            result.readinessDeadline = ready.deadline;
            result.readinessResult = readinessResult;
            result.scheduled = scheduledDeadline;
            result.tickSnap = tickSnap;
            result.targetTick = targetTick;
            result.swapchain = job.swapchain;
            result.generated = job.generated;
            result.appPresentId = job.generated ? 0 : applicationIdOf(metadata);
            result.backendFrame = current.backendFrame;
            result.phase = current.phase;
            result.workerCopy = job.copyActive;
            result.outputQueue = queue->queue;
            result.lateReadiness = first && lateReadiness;
            result.arrival = first ? ready.arrival : Clock::time_point{};
            // A generated output without an early preparation takes its image at the deadline. Polling
            // never sleeps under queue->mutex, and every path below signals the
            // continuation and the render-slot fence.
            if (job.internalActive && prep.settled) {
                // F9b: the preparation ended the job. Nothing was waited or presented.
                result.workerCopy = true;
                result.imageIndex = prep.lowerIndex;
                result.skipped = true;
                result.skipReason = SkipReason::Error;
                result.started = result.finished = Clock::now();
                result.result = prep.error;
                result.lockAcquireNs = prep.acquireNs;
                result.lockSubmitNs = prep.submitNs;
            } else if (job.internalActive && prep.prepared) {
                // The lower image was acquired and copied before the deadline: only the present remains.
                result.workerCopy = true;
                result.internalPrepared = true;
                result.lockAcquireNs = prep.acquireNs;
                result.lockSubmitNs = prep.submitNs;
                if (ready.trace) result.queueLockStarted = Clock::now();
                const std::lock_guard queueLock(queue->mutex);
                if (ready.trace) result.queueLockAcquired = Clock::now();
                presentLower(job, ready, chain, prep.lowerIndex, prep.presentWait, result, &onScreenLowerId);
            } else if (job.internalActive) {
                presentInternalReal(job, ready, chain, result, &onScreenLowerId);
            } else {
                VkResult skipResult = earlyGenerated && prep.settled ? prep.error : VK_SUCCESS;
                bool acquired = earlyGenerated && prep.acquired;
                bool copyPending{};
                const bool generatedPrepared = earlyGenerated && prep.prepared;
                if (earlyGenerated) {
                    result.lockAcquireNs = prep.acquireNs;
                    result.lockSubmitNs = prep.submitNs;
                }
                if (generatedPrepared) {
                    job.image = prep.lowerIndex;
                    job.wait = prep.presentWait;
                    result.generatedPrepared = true;
                    result.copySubmitStarted = prep.copySubmitStarted;
                }
                // Why this generated output presents no image, when it does not. Readiness
                // and stopping skip unprepared outputs. Expedite skips only a non-overlapped batch: an
                // overlapped batch keeps polling for its lower image (F7 rule 4). A window
                // that ends with no lower image is an acquire skip (F7 rule 2).
                SkipReason skipWhy = SkipReason::None;
                if (job.copyActive && !generatedPrepared && !prep.settled) {
                    uint32_t image{};
                    VkResult acquireResult = VK_NOT_READY;
                    // OS1b: the window follows the corrected slot, so a generated output moved later keeps
                    // its full acquire window (an earlier move keeps the unmoved one).
                    auto giveUpAt = unmovedReady + std::max(appliedOffset, Clock::duration{}) + job.copy.giveUp;
                    // Generated outputs may start up to a quarter output interval before the
                    // batch limit. FIFO still shows each image at least one refresh.
                    if (ready.latestOutput != Clock::time_point::max())
                        giveUpAt = std::min(giveUpAt, ready.latestOutput - ready.outputInterval / 4);
                    lock.lock();
                    if (readinessMiss) skipWhy = SkipReason::Readiness;
                    else if (stopping) skipWhy = SkipReason::Stopping;
                    else if (!ready.overlap && batch.expediteAt != Clock::time_point::max())
                        skipWhy = SkipReason::Expedite;
                    lock.unlock();
                    // At least one attempt is made unless readiness or stopping already skipped.
                    while (skipWhy == SkipReason::None) {
                        {
                            const std::lock_guard queueLock(queue->mutex);
                            // D7d: timed and noted only with history; not-ready and timed-out polls are not noted.
                            const bool noted = queue->history;
                            const auto acquireStarted = noted ? Clock::now() : Clock::time_point{};
                            queue->beginCall(lowerOpAcquireGenerated, UINT32_MAX);
                            acquireResult = profiledLockCall(ready.profile, result.lockAcquireNs, [&] {
                                return job.copy.acquire(job.copy.device, job.swapchain, 0,
                                    job.copy.acquireSemaphore, VK_NULL_HANDLE, &image);
                            });
                            queue->endCall();
                            if (noted && acquireResult != VK_NOT_READY && acquireResult != VK_TIMEOUT) {
                                const bool gotImage = acquireResult == VK_SUCCESS || acquireResult == VK_SUBOPTIMAL_KHR;
                                queue->noteLower(lowerOpAcquireGenerated, job.swapchain,
                                    gotImage ? image : UINT32_MAX, acquireResult, ready.source,
                                    steadyNanoseconds(acquireStarted), steadyNanoseconds(Clock::now()));
                            }
                        }
                        if (acquireResult < 0 || acquireResult == VK_SUCCESS ||
                                acquireResult == VK_SUBOPTIMAL_KHR)
                            break;
                        if (Clock::now() >= giveUpAt) {
                            // No lower image by the end of the window: the output is skipped.
                            skipWhy = SkipReason::Acquire;
                            break;
                        }
                        lock.lock();
                        const bool stopped = stopping;
                        const bool expedited = !ready.overlap && batch.expediteAt != Clock::time_point::max();
                        lock.unlock();
                        if (stopped) { skipWhy = SkipReason::Stopping; break; }
                        if (expedited) { skipWhy = SkipReason::Expedite; break; }
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
                    bool presentOutput = !job.copyActive || generatedPrepared;
                    if (job.copyActive && !generatedPrepared) {
                        if (copyPending) {
                            const VkResult copied = profiledLockCall(ready.profile, result.lockSubmitNs, [&] {
                                if (ready.profile) result.copySubmitStarted = Clock::now();
                                return submitGeneratedWork(*queue, job.copy, true,
                                    job.copy.copies.at(job.image), job.wait, job.image,
                                    0, ready.source);
                            });
                            presentOutput = copied == VK_SUCCESS;
                            if (copied < 0) skipResult = copied;
                        }
                        if (!presentOutput) {
                            // D7c: the same rule as result.skipReason below, computed before the signal.
                            const auto signalReason = static_cast<uint8_t>(
                                skipResult < 0 || skipWhy == SkipReason::None ? SkipReason::Error : skipWhy);
                            const VkResult signalled = profiledLockCall(ready.profile, result.lockSubmitNs, [&] {
                                return submitGeneratedWork(*queue, job.copy,
                                    acquired, VK_NULL_HANDLE, VK_NULL_HANDLE, UINT32_MAX,
                                    signalReason, ready.source);
                            });
                            if (skipResult == VK_SUCCESS) skipResult = signalled;
                        }
                    }
                    if (presentOutput) {
                        result.started = Clock::now();
                        // GP1: the lower ID of this present, handed out under queue->mutex.
                        if (onScreenIds) onScreenLowerId = onScreenIds->allocate();
                        queue->beginCall(lowerOpPresentGenerated, job.image);
                        result.presentId = present_diagnostics::lowerPresentId(chain);
                        result.result = ready.observer && ready.observe
                            ? ready.observe(ready.observer.get(), present, queue->queue,
                                info, ready.context, ready.source, job.generated, &result.feedback)
                            : present(queue->queue, &info);
                        queue->endCall();
                        result.finished = Clock::now();
                        queue->markPresentLocked(ready.context, ready.source, result.feedback, job.generated);
                        observeOutputOffsetLocked(ready.context);
                        if (onScreenIds)
                            noteOnScreenLocked(job, ready, onScreenLowerId, result.result, result.started, result.targetTick);
                        // D7d: the generated lower present, noted only with history.
                        if (queue->history)
                            queue->noteLower(lowerOpPresentGenerated, job.swapchain, job.image, result.result,
                                ready.source, steadyNanoseconds(result.started), steadyNanoseconds(result.finished));
                        if (ready.profile) result.lockPresentNs = nanosecondsOf(result.finished - result.started);
                        if (ready.copyStatus && ready.copyFence) {
                            result.copyChecked = true;
                            result.copyStatus = ready.copyStatus(ready.device, ready.copyFence);
                            result.copySampledAt = Clock::now();
                        }
                    } else {
                        result.skipped = true;
                        // An acquire, copy or signal error outranks the skip decision.
                        result.skipReason = skipResult < 0 || skipWhy == SkipReason::None
                            ? SkipReason::Error : skipWhy;
                        result.started = Clock::now();
                        result.finished = result.started;
                        result.result = skipResult;
                    }
                }
            }
            // W2: a generated output that presented keeps the effective part of its own correction.
            if (offsetOutput && !result.skipped) {
                result.offset = outputOffsetEffective(appliedOffset, unmovedReady, result.scheduled,
                    result.startedUnsnapped());
                outputOffsetController.limited[index] = outputOffsetLimited(commandedOffset, result.offset);
            }
            // A prepared output follows F9b: its lower present is always issued,
            // including during stop/retirement; no second release mechanism is needed.
            heldLowerImages = 0;
            // X3: every skipped job ends here without a lower present, so an application present
            // fence in its chain is signalled once, on the private queue after the job's own
            // submit on that queue (copy, abandon or skip). A presented job never gets this signal.
            if (result.skipped && metadata.hasFence && metadata.fence) {
                assert(queueSubmit != nullptr);
                const std::lock_guard queueLock(queue->mutex);
                static_cast<void>(signalUnforwardedPresentFence(queueSubmit, queue->queue, metadata.fence));
            }
            if (applicationIdentified(metadata)) {
                {
                    // Batches are presented in order, so this is the oldest unsubmitted record with these IDs.
                    const std::lock_guard deliveryLock(delivery->mutex);
                    delivery->submitLocked(metadata.hasId ? metadata.id : 0,
                        metadata.hasId2 && !metadata.ownedId2 ? metadata.id2 : 0, result.result, workerWait);
                }
                delivery->changed.notify_all();
            }
            lock.lock();
            batch.aggregate.source = ready.source;
            batch.aggregate.outputInterval = ready.outputInterval;
            batch.aggregate.justInTime = ready.justInTime;
            batch.aggregate.held = batchHeld;
            batch.aggregate.batchPhaseHold = batchPhaseHold;
            batch.aggregate.readinessShift = readinessShift;
            batch.aggregate.readyAt = readyAt;
            batch.aggregate.slack = slack;
            batch.aggregate.readinessMiss = readinessMiss;
            if (readinessResult < 0) batch.aggregate.result = readinessResult;
            if (batch.aggregate.count == 0) batch.aggregate.started = result.started;
            batch.aggregate.finished = result.finished;
            if (batch.aggregate.result >= 0 && (result.result < 0 || batch.aggregate.result == VK_SUCCESS))
                batch.aggregate.result = result.result;
            batch.aggregate.calls[batch.aggregate.count++] = result;
            ++batch.consumed;
            if (batch.consumed == batch.produced) lastBatchFinished = Clock::now();
            // The worker is done with an internal real job; drop its owner reference.
            batch.jobs[index].internal = {};
            // GT1: a skipped output presented nothing, so its tick snap is not a start: its bookkeeping stays as today.
            if (result.skipped) {
                result.tickSnap = {};
                result.targetTick = {};
            }
            if (!result.skipped) {
                // GT1: the bookkeeping starts are in unsnapped time; the tick floor reads the actual start.
                previousStarted = result.startedUnsnapped();
                previousActualStarted = result.started;
                previousFinished = result.finished;
                // An early (expedited) output anchors the next batch at its actual start; late wakes keep the schedule.
                // W2: the anchor is in unmoved time, so a generated output's own correction is taken back.
                previousScheduled = std::min(result.scheduled, result.startedUnsnapped()) - result.offset;
                havePrevious = true;
            }
            retireFinishedHeadsLocked();
            changed.notify_all();
        }
    }
};
}

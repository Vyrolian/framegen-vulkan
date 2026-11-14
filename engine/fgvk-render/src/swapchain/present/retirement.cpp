/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain/swapchain.hpp"
#include "swapchain/present/internal.hpp"
#include "swapchain/retirement.hpp"
#include "swapchain/create_policy.hpp"
#include "adaptive_scheduler.hpp"
#include "fgvk-common/helpers/errors.hpp"
#include "fgvk-common/vulkan/command_buffer.hpp"
#include "fgvk-common/vulkan/image.hpp"
#include "fgvk-common/vulkan/semaphore.hpp"
#include "fgvk-common/vulkan/vulkan.hpp"
#include "present_diagnostics.hpp"
#include "pnext_chain.hpp"
#include "wayland_presentation_feedback.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <sstream>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

using namespace fgvk;
using namespace fgvk::layer;

using namespace fgvk::layer::present_detail;

VkResult Swapchain::completeDeferredPresent(const uint64_t timeoutNs, const bool acquisition,
        const bool retainNewest) {
    if (!this->deferredPresent) return this->deferredPresentResult;
    const auto started = acquisition ? DiagnosticsClock::now() : DiagnosticsClock::time_point{};
    const auto first = this->deferredPresent->finish(timeoutNs, true, retainNewest);
    if (acquisition && (!first || first->finished > started))
        this->realFramePacer.recordAcquireWait(DiagnosticsClock::now() - started);
    if (!first) return timeoutNs == 0 ? VK_NOT_READY : VK_TIMEOUT;
    const auto report = [this](const DeferredPresent::Completion& completion) {
        if (!this->adaptiveScheduler)
            for (size_t i = 0; i < completion.count; ++i)
                // An internal real output is a worker copy too, but it is not a generated frame.
                if (completion.calls[i].workerCopy && completion.calls[i].generated) {
                    if (completion.calls[i].skipped)
                        ++this->diagnosticsState.fixedSkippedFrames;
                    else
                        ++this->diagnosticsState.fixedGeneratedFrames;
                }
        if (completion.result == VK_SUCCESS && completion.count && !completion.readinessMiss)
            this->realFramePacer.completeOutputBatch(completion.calls[completion.count - 1].started,
                completion.readinessShift);
        else if (completion.readinessMiss)
            this->realFramePacer.resetOutputs();
        else if (completion.result != VK_SUCCESS)
            this->realFramePacer.resetOutputs();
        if (this->pacingProfile && completion.count)
            this->pacingProfile->completedWorkerBatch(completion.readinessMiss);
        if (this->pacingProfile) {
            // Each generated output that presented no image, by its recorded cause. The
            // skip record time is the moment the worker decided, not the report time.
            using Profile = present_diagnostics::PacingProfile;
            for (size_t i = 0; i < completion.count; ++i) {
                const auto& call = completion.calls[i];
                if (!call.skipped || !call.generated) continue;
                Profile::GeneratedSkip kind = Profile::SkipOther;
                std::string_view reason = "error";
                switch (call.skipReason) {
                case DeferredPresent::SkipReason::Readiness:
                    kind = Profile::SkipReadiness; reason = "readiness"; break;
                case DeferredPresent::SkipReason::Expedite:
                    kind = Profile::SkipExpedite; reason = "expedite"; break;
                case DeferredPresent::SkipReason::GiveUp:
                    kind = Profile::SkipGiveUp; reason = "give-up"; break;
                case DeferredPresent::SkipReason::Acquire:
                    kind = Profile::SkipAcquire; reason = "acquire"; break;
                case DeferredPresent::SkipReason::Stopping:
                    reason = "stopping"; break;
                case DeferredPresent::SkipReason::Error:
                case DeferredPresent::SkipReason::None:
                    break;
                }
                this->pacingProfile->generatedSkipped(kind);
                present_diagnostics::logPacingSkip(this->diagnosticsState.contextId,
                    completion.source, reason, call.scheduled, call.started);
            }
            if (completion.count && completion.calls[0].lateReadiness)
                this->pacingProfile->lateReadinessBatch();
            this->pacingProfile->generatedCompressedPairs(completion.compressedGeneratedPairs());
            for (size_t i = 0; i < completion.count; ++i) {
                const auto& call = completion.calls[i];
                // Only an internal real sets workerCopy without generated, so this counts
                // the presented internal reals: prepared before the deadline, or polled at it.
                if (call.workerCopy && !call.generated && !call.skipped)
                    this->pacingProfile->internalRealPresented(call.internalPrepared);
                if (call.workerCopy && call.generated && !call.skipped)
                    this->pacingProfile->generatedPresented(call.generatedPrepared);
                if (call.lockAcquireNs || call.lockSubmitNs || call.lockPresentNs)
                    this->pacingProfile->workerLockSplit(call.lockAcquireNs, call.lockSubmitNs, call.lockPresentNs);
            }
        }
        if (this->pacingProfile)
            for (size_t i = 0; i < completion.count; ++i) {
                const auto& call = completion.calls[i];
                if (call.skipped) continue;
                const auto sample = this->pacingProfile->output(call.started, call.finished, call.result, {
                    .scheduled = call.scheduled, .imageIndex = call.imageIndex,
                    .generated = call.generated, .deferred = true,
                    .copyChecked = call.copyChecked, .copyStatus = call.copyStatus,
                    .copySampledAt = call.copySampledAt, .source = completion.source,
                    .enqueued = call.enqueued, .queueLockStarted = call.queueLockStarted,
                    .queueLockAcquired = call.queueLockAcquired, .readinessChecked = call.readinessChecked,
                    .readinessStarted = call.readinessStarted, .readinessFinished = call.readinessFinished,
                    .readinessDeadline = call.readinessDeadline, .readinessResult = call.readinessResult,
                    .queue = call.outputQueue, .presentWait = call.presentWait,
                    .arrival = call.arrival, .feedback = call.feedback,
                    .backendFrame = call.backendFrame, .phase = call.phase, .presentId = call.presentId,
                    .readyAt = i == 0 ? completion.readyAt : DiagnosticsClock::time_point{},
                    .slack = i == 0 ? completion.slack : DiagnosticsClock::duration{},
                });
                present_diagnostics::logPacingOutput(this->diagnosticsState.contextId, call.swapchain, sample);
            }
        if (completion.result != VK_SUCCESS)
            this->deferredPresentResult = completion.result;
    };
    report(*first);
    // Drain every other finished batch in the same call, oldest first.
    while (this->deferredPresent->hasCompletion()) {
        const auto next = this->deferredPresent->finish(0, false, true);
        if (!next) break;
        report(*next);
    }
    return this->deferredPresentResult;
}

bool Swapchain::beginDeferredPresentBatch(const vk::Vulkan& vk, const size_t limit,
        const uint64_t readyValue, const DiagnosticsClock::time_point readyDeadline, const bool capOwned,
        const std::optional<DiagnosticsClock::time_point> releasedOutputLimit,
        const DiagnosticsClock::duration releasedJoinTolerance,
        const bool overlap, const DiagnosticsClock::duration outputInterval,
        const DiagnosticsClock::time_point arrival) {
    if (!this->info.deferredPresentQueue) return false;
    if (!this->deferredPresent) {
        try {
            this->deferredPresent = std::make_unique<DeferredPresent>(
                this->info.deferredPresentQueue, vk.df().QueuePresentKHR, vk.dev());
            this->deferredPresent->setOwnedPresentIdStorage(&this->presentFeedbackTag);
        } catch (const std::exception&) { return false; }
    }
    return this->deferredPresent->beginBatch(limit, {
        .device = vk.dev(), .semaphore = this->syncSemaphore->handle(),
        .value = readyValue, .wait = vk.df().WaitSemaphoresKHR, .deadline = readyDeadline, .capOwned = capOwned,
        .latestOutput = releasedOutputLimit.value_or(DiagnosticsClock::time_point::max()),
        .joinTolerance = releasedJoinTolerance,
        .expediteOnJoin = releasedOutputLimit.has_value(),
        .overlap = overlap, .outputInterval = outputInterval,
        // Diagnostics only: the arrival is observed when the pacing profile is enabled.
        .arrival = this->pacingProfile ? arrival : DiagnosticsClock::time_point{},
        .copyFence = this->pacingProfile && present_diagnostics::pacingGpuProfileEnabled() && !this->renderSlots.empty()
            ? this->currentRenderSlot().fence.handle() : VK_NULL_HANDLE,
        .copyStatus = this->pacingProfile && present_diagnostics::pacingGpuProfileEnabled()
            ? vk.df().GetFenceStatus : nullptr,
        .observer = this->info.waylandPresentation,
        .observe = this->info.waylandPresentation ? &WaylandPresentationFeedback::observe : nullptr,
        .context = this->diagnosticsState.contextId,
        .source = this->pacingProfile ? this->pacingProfile->sourceOrdinal() : 0,
        .trace = present_diagnostics::pacingFrameTraceEnabled(),
        .profile = static_cast<bool>(this->pacingProfile),
    });
}

// F5 internal images. Outside a present with an internal descriptor these
// helpers return the existing lower-image contract unchanged.
VkImage Swapchain::applicationImageFor(const uint32_t applicationIndex) const {
    if (this->internalPresent)
        return this->internalPresent->image;
    return this->info.images.at(applicationIndex);
}

size_t Swapchain::presentationSlotFor(const uint32_t applicationIndex) const {
    return this->internalPresent ? this->internalPresent->realSlot : applicationIndex;
}

// The real output's presentation wait and retirement fence live after the lower
// slots. Grow them to lowerImages + private images on the first internal present.
void Swapchain::ensureInternalPresentSlots(const vk::Vulkan& vk) {
    if (!this->internalPresent || !this->internalPresent->owner)
        return;
    const size_t slots = this->info.images.size() +
        this->internalPresent->owner->applicationImageCount();
    while (this->postCopySemaphores.size() < slots) {
        this->postCopySemaphores.emplace_back(
            vk::Semaphore(vk), vk::Semaphore(vk)
        );
    }
    // Retirement fences exist only when maintenance1 retirement is enabled. An
    // empty list means no fence is used, so nothing grows.
    while (!this->presentRetirementFences.empty() &&
            this->presentRetirementFences.size() < slots) {
        this->presentRetirementFences.emplace_back(
            PresentRetirementFence{.fence = vk::Fence(vk)}
        );
    }
}

// T3b, diagnostics only: time the real copy of this application image on the GPU and tag
// it with the source the output record carries. No effect without the GPU split gate.
void Swapchain::noteInternalCopyProfile(const vk::Vulkan& vk, InternalPresentImage& image) {
    if (!this->pacingProfile || !present_diagnostics::pacingGpuSplitProfileEnabled() || !image.owner)
        return;
    const auto get = [&](const char* name) { return vk.fi().GetDeviceProcAddr(vk.dev(), name); };
    auto calibrate = this->info.copyTimestampCalibration
        ? reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(get("vkGetCalibratedTimestampsKHR")) : nullptr;
    if (this->info.copyTimestampCalibration && !calibrate)
        calibrate = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(get("vkGetCalibratedTimestampsEXT"));
    image.owner->enableCopyTimestamps(vk, calibrate, this->diagnosticsState.contextId);
    image.owner->noteCopySource(image.applicationIndex, this->pacingProfile->sourceOrdinal());
}

// Worker branch of the real output. The copy and the lower acquire happen on the
// worker at the deadline. Returns false when the worker refuses, and the caller
// then materializes synchronously.
bool Swapchain::enqueueInternalRealPresent(const vk::Vulkan& vk,
        const VkPresentInfoKHR& presentInfo, const DeferredPresent::Clock::time_point deadline) {
    InternalPresentImage& image = *this->internalPresent;
    if (!image.owner || !image.keepAlive || !this->deferredPresent ||
            presentInfo.waitSemaphoreCount != 1)
        return false;
    this->noteInternalCopyProfile(vk, image);
    if (image.owner->ensureCopies(vk, image.applicationIndex) != VK_SUCCESS)
        return false;
    const DeferredPresent::InternalRealCopy copy{
        .owner = image.keepAlive,
        .tryAcquire = &internalRealTryAcquire,
        .submit = &internalRealSubmit,
        .abandon = &internalRealAbandon,
        .applicationIndex = image.applicationIndex,
    };
    if (!this->deferredPresent->enqueueInternalReal(presentInfo, deadline, copy))
        return false;
    image.handedOff = true;
    return true;
}

// Synchronous branch of the real output. Acquires and copies the application
// image into a lower image after the present's waits, so the present waits the copy.
VkResult Swapchain::materializeInternalPresent(const vk::Vulkan& vk,
        const VkPresentInfoKHR& presentInfo, uint32_t* const lowerIndex, VkSemaphore* const copied) {
    InternalPresentImage& image = *this->internalPresent;
    if (!image.owner)
        return VK_ERROR_UNKNOWN;
    const auto waits = presentInfo.waitSemaphoreCount > 0
        ? std::span<const VkSemaphore>(presentInfo.pWaitSemaphores, presentInfo.waitSemaphoreCount)
        : std::span<const VkSemaphore>{};
    this->noteInternalCopyProfile(vk, image);
    const VkResult result = image.owner->materialize(vk, image, waits, lowerIndex, copied);
    if (result == VK_SUCCESS)
        image.handedOff = true;
    return result;
}

VkResult Swapchain::queuePresentWithRetirementFence(
        const vk::Vulkan& vk, const VkQueue queue,
        const VkPresentInfoKHR& incomingPresentInfo,
        const present_diagnostics::PacingProfile::OutputObservation& observation) {
    VkPresentInfoKHR presentInfo = incomingPresentInfo;
#if defined(VK_NV_present_metering)
    VkSetPresentConfigNV metering{
        .sType = VK_STRUCTURE_TYPE_SET_PRESENT_CONFIG_NV, .pNext = presentInfo.pNext,
        .numFramesPerBatch = static_cast<uint32_t>(this->bridgeOutputBatchSize),
    };
    if (this->info.presentMetering && observation.generated &&
            !this->profile.adaptive &&
            this->configuredFixedGeneratedFrames == 1 && this->bridgeOutputBatchSize == 2)
        presentInfo.pNext = &metering;
#endif
    using PacingProfile = present_diagnostics::PacingProfile;
    auto timingOwner = hasPresentTiming(incomingPresentInfo.pNext)
        ? PacingProfile::ApplicationTiming : PacingProfile::DriverTiming;
    std::optional<DiagnosticsClock::time_point> timingTarget;
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
    VkPresentTimingInfoEXT timing{};
    uint64_t timingId{};
    VkPresentId2KHR presentId{
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
        .pNext = presentInfo.pNext,
        .swapchainCount = 1,
        .pPresentIds = &timingId,
    };
    VkPresentTimingsInfoEXT timings{
        .sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT,
        .pNext = &presentId,
        .swapchainCount = 1,
        .pTimingInfos = &timing,
    };
    if (this->presentTiming.controlsOutput(0) &&
            this->info.presentMode == VK_PRESENT_MODE_FIFO_KHR &&
            presentInfo.swapchainCount == 1 && !hasPresentTiming(presentInfo.pNext)) {
        const double refresh = this->presentTiming.refreshRate(0);
        const auto ceiling = OrderedPresentTimeline::validRate(refresh)
            ? std::optional{static_cast<uint32_t>(std::round(refresh))} : std::nullopt;
        const auto policy = gamescopeBridgeOutputPolicy(this->profile, ceiling,
            this->adaptiveScheduler &&
                    fractionalNativePresentationEligible(this->profile)
                ? this->adaptiveScheduler->snapshot() : AdaptiveSchedulerSnapshot{});
        this->wsiPresentTimeline.observeSourceBatch(DiagnosticsClock::now(), this->frameState.realFrameIndex);
        const auto slot = this->wsiPresentTimeline.schedule(DiagnosticsClock::now(),
            this->presentTiming.outputRate(policy.outputFps), refresh,
            this->bridgeOutputBatchSize, policy.generatedTimeline, this->outputSourceDeadline);
        if (slot) {
            std::this_thread::sleep_until(slot->submitAt);
            if (this->presentTiming.prepare(slot->presentAt, this->wsiPresentTimeline, timing, timingId)) {
                this->orderedPresentationDeadline = slot->presentAt;
                presentInfo.pNext = &timings;
                timingOwner = PacingProfile::ExtTiming;
                timingTarget = slot->presentAt;
            }
        }
    }
#endif
    this->lastLowerPresentRetirementProtected = false;
    const auto lowerPresent = [&](VkPresentInfoKHR info) {
        if (this->pacingProfile)
            this->pacingProfile->outputTiming(timingOwner, timingTarget);
        // The real output of an internal application image: its lower image is
        // acquired and copied on the worker or synchronously below, never by the caller.
        const bool internalReal = this->internalPresent && observation.applicationImage;
        if (this->deferredPresentDeadline && this->info.deferredPresentQueue) {
            if (!this->deferredPresent) {
                try {
                    this->deferredPresent = std::make_unique<DeferredPresent>(
                        this->info.deferredPresentQueue, vk.df().QueuePresentKHR, vk.dev());
                    this->deferredPresent->setOwnedPresentIdStorage(&this->presentFeedbackTag);
                } catch (const std::exception&) {
                    // Thread/resource exhaustion retains synchronous delivery.
                }
            }
            const bool admitted = this->deferredPresent && (internalReal
                ? this->enqueueInternalRealPresent(vk, info, *this->deferredPresentDeadline)
                : this->deferredPresent->enqueue(info, *this->deferredPresentDeadline, observation.generated,
                    DeferredPresent::OutputIdentity{observation.backendFrame, observation.phase}));
            if (admitted) {
                this->deferredOutputQueuedThisFrame = true;
                this->deferredPresentDeadline.reset();
                return VK_SUCCESS;
            }
            // A failed tail admission must not let this synchronous output
            // overtake earlier retained images or overlap swapchain host use.
            if (this->deferredPresent) {
                this->deferredPresent->closeBatch();
                static_cast<void>(this->completeDeferredPresent(UINT64_MAX));
            }
            std::this_thread::sleep_until(*this->deferredPresentDeadline);
            this->deferredPresentDeadline.reset();
        }
        // A synchronous output must not overtake older retained images. An open
        // newest batch never completes, so wait for every older batch then.
        if (this->deferredPresent && this->deferredPresent->olderBatchPending())
            static_cast<void>(this->completeDeferredPresent(UINT64_MAX, false,
                this->deferredPresent->newestOpen()));
        // The synchronous real output: copy the application image into a lower image
        // first, then present that lower image after the copy. The copy takes over the
        // present's waits, so the lower image is never presented with a wait it did not consume.
        uint32_t materializedIndex{};
        VkSemaphore materializedWait{};
        if (internalReal) {
            const VkResult materialized = this->materializeInternalPresent(
                vk, info, &materializedIndex, &materializedWait);
            if (materialized < 0)
                return materialized;
            info.waitSemaphoreCount = 1;
            info.pWaitSemaphores = &materializedWait;
            info.pImageIndices = &materializedIndex;
        }
        // The feedback this lower present requested, for the output record's join to presented_ns.
        uint64_t feedbackId{};
        const auto submit = [&] {
            this->realFramePacer.synchronousOutput();
            const auto observedPresent = [&](VkQueue selectedQueue, const VkPresentInfoKHR& selectedInfo) {
                if (this->info.waylandPresentation)
                    return WaylandPresentationFeedback::observe(this->info.waylandPresentation.get(),
                        vk.df().QueuePresentKHR, selectedQueue, selectedInfo, this->diagnosticsState.contextId,
                        this->pacingProfile ? this->pacingProfile->sourceOrdinal() : 0, observation.generated,
                        &feedbackId);
                return vk.df().QueuePresentKHR(selectedQueue, &selectedInfo);
            };
            if (nativePrivatePresentQueueEligible(
                    static_cast<bool>(this->info.deferredPresentQueue))) {
                // Pin every native output from the first managed present,
                // including synchronous first/generated/native/fallback and
                // policy-off frames. Joining the worker proves host delivery,
                // not cross-queue presentation order at the driver.
                if (this->info.waylandPresentation) {
                    const std::lock_guard lock(this->info.deferredPresentQueue->mutex);
                    // D7d: noted as a synchronous lower present only when the pacing profile keeps history.
                    const VkResult presented = this->info.deferredPresentQueue->presentSyncLocked(info, [&] {
                        return observedPresent(this->info.deferredPresentQueue->queue, info);
                    });
                    // T4: the private queue marker, outside the timed present bracket.
                    this->info.deferredPresentQueue->markPresentLocked(this->diagnosticsState.contextId,
                        this->pacingProfile ? this->pacingProfile->sourceOrdinal() : 0, feedbackId,
                        observation.generated);
                    return presented;
                }
                return this->info.deferredPresentQueue->presentNow(vk.df().QueuePresentKHR, info);
            }
            return observedPresent(queue, info);
        };
        if (!this->pacingProfile) return submit();
        auto observed = observation;
        observed.queue = nativePrivatePresentQueueEligible(
            static_cast<bool>(this->info.deferredPresentQueue))
            ? this->info.deferredPresentQueue->queue : queue;
        if (info.waitSemaphoreCount == 1 && info.pWaitSemaphores)
            observed.presentWait = info.pWaitSemaphores[0];
        if (info.swapchainCount == 1 && info.pImageIndices)
            observed.imageIndex = info.pImageIndices[0];
        const auto started = DiagnosticsClock::now();
        const auto result = submit();
        const auto finished = DiagnosticsClock::now();
        observed.feedback = feedbackId;
        observed.presentId = present_diagnostics::lowerPresentId(info.pNext);
        if (observed.copyExpected && present_diagnostics::pacingGpuProfileEnabled() && !this->renderSlots.empty()) {
            observed.copyChecked = true;
            observed.copyStatus = vk.df().GetFenceStatus(vk.dev(), this->currentRenderSlot().fence.handle());
            observed.copySampledAt = DiagnosticsClock::now();
        }
        const auto sample = this->pacingProfile->output(started, finished, result, observed);
        present_diagnostics::logPacingOutput(this->diagnosticsState.contextId, info.swapchainCount == 1 ? info.pSwapchains[0] : VK_NULL_HANDLE, sample);
        return result;
    };
    if (this->presentRetirementFences.empty() ||
            presentInfo.swapchainCount != 1 ||
            !presentInfo.pImageIndices) {
        return lowerPresent(presentInfo);
    }

    if (const auto* upstreamFence =
            findSwapchainPresentFenceInfo(presentInfo.pNext)) {
        // Exactly one present fence can be associated with each swapchain in
        // a present operation. Preserve the upstream owner's fence unchanged.
        // For the final lower present it is already the spec-defined proof
        // that the caller may destroy this swapchain; framegen-vulkan retains only its
        // own earlier fences and the compositor grace period after destroy.
        if (!this->externalPresentFenceLogged) {
            std::cerr << "framegen-vulkan: upstream presentation fence observed; "
                         "preserving owner and enabling guarded live resource "
                         "recreation on protected final presents\n";
            this->externalPresentFenceLogged = true;
        }
        const auto result = lowerPresent(presentInfo);
        this->lastLowerPresentRetirementProtected =
            upstreamPresentFenceProtectsSwapchain(upstreamFence) &&
            presentFenceWillSignal(result);
        return result;
    }

    // The application image's real output uses its own slot, not the index of the lower image it will copy into.
    const uint32_t imageIndex = observation.applicationImage && this->internalPresent
        ? this->internalPresent->realSlot : presentInfo.pImageIndices[0];
    if (imageIndex >= this->presentRetirementFences.size())
        return lowerPresent(presentInfo);

    auto& slot = this->presentRetirementFences.at(imageIndex);
    try {
        if (slot.associated) {
            if (!slot.fence.wait(vk, 0)) {
                if (!this->presentRetirementBusyLogged) {
                    std::cerr << "framegen-vulkan: presentation retirement fence "
                                 "remained busy after image reacquisition; "
                                 "this present will not trigger live recreation\n";
                    this->presentRetirementBusyLogged = true;
                }
                return lowerPresent(presentInfo);
            }
            slot.associated = false;
        }
        if (slot.used)
            slot.fence.reset(vk);
    } catch (const std::exception& error) {
        std::cerr << "framegen-vulkan: presentation retirement fence preparation "
                     "failed; this present will not trigger live recreation: "
                  << error.what() << '\n';
        return lowerPresent(presentInfo);
    }

    const VkFence fence = slot.fence.handle();
    const VkSwapchainPresentFenceInfoEXT fenceInfo{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
        .pNext = presentInfo.pNext,
        .swapchainCount = 1,
        .pFences = &fence,
    };
    auto protectedPresentInfo = presentInfo;
    protectedPresentInfo.pNext = &fenceInfo;
    const auto result = lowerPresent(protectedPresentInfo);
    slot.used = true;
    slot.associated = presentFenceWillSignal(result);
    this->lastLowerPresentRetirementProtected = slot.associated;
    if (slot.associated)
        this->presentRetirementBusyLogged = false;
    return result;
}

bool Swapchain::waitForPresentRetirement(
        const vk::Vulkan& vk, const uint64_t timeoutNs) {
    const auto started = std::chrono::steady_clock::now();
    const auto deferred = this->completeDeferredPresent(timeoutNs);
    if (deferred == VK_NOT_READY || deferred == VK_TIMEOUT) return false;
    for (auto& slot : this->presentRetirementFences) {
        if (!slot.associated)
            continue;

        uint64_t remaining = timeoutNs;
        if (timeoutNs != UINT64_MAX) {
            const auto elapsed = std::chrono::duration_cast<
                std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started
                ).count();
            if (elapsed >= static_cast<int64_t>(timeoutNs))
                remaining = 0;
            else
                remaining = timeoutNs - static_cast<uint64_t>(elapsed);
        }
        try {
            if (!slot.fence.wait(vk, remaining))
                return false;
        } catch (const std::exception& error) {
            std::cerr << "framegen-vulkan: presentation retirement fence wait "
                         "failed: " << error.what() << '\n';
            return false;
        }
        slot.associated = false;
    }
    if (timeoutNs == 0) {
        // Presentation can finish while history-only backend work is still
        // running. Do not enter closeContext's bounded wait from a present.
        try {
            if (!this->sourceImages.empty() &&
                    (!this->instance ||
                     !this->instance->contextReady(this->ctx.get())))
                return false;
            if (!this->pollRenderSlotsIdle(vk))
                return false;
        } catch (const std::exception& error) {
            std::cerr << "framegen-vulkan: private work retirement poll failed: "
                      << error.what() << '\n';
            return false;
        }
    }
    return true;
}

VkResult Swapchain::retireAcquiredImagesAndPresent(const vk::Vulkan& vk,
        const VkQueue queue, const VkSwapchainKHR swapchain,
        const void* nextChain, const uint32_t originalImageIndex,
        const std::span<const VkSemaphore> applicationWaitSemaphores,
        const std::span<const uint32_t> acquiredImageIndices,
        const VkImage originalImage) {
    if (acquiredImageIndices.empty())
        throw ls::error("attempted to retire an empty acquired-image batch");

    // With an internal descriptor the application image is the private image, and
    // its real output uses the realSlot wait and fence.
    const VkImage originalSource = this->internalPresent
        ? this->internalPresent->image : originalImage;
    const size_t originalSlot = this->presentationSlotFor(originalImageIndex);

    const size_t semaphoreBase = (
        this->frameState.sequenceIndex + acquiredImageIndices.size() + 1
    ) % this->postCopySemaphores.size();

    for (size_t i = 0; i < acquiredImageIndices.size(); ++i) {
        const bool first = i == 0;
        const bool last = i + 1 == acquiredImageIndices.size();
        const size_t semaphoreIndex =
            (semaphoreBase + i) % this->postCopySemaphores.size();
        auto& pcs = this->postCopySemaphores.at(semaphoreIndex);
        auto& pass = this->currentRenderSlot().passes.at(i);
        const auto acquiredImage = this->info.images.at(
            acquiredImageIndices[i]
        );

        std::array<vk::Barrier, 2> preBarriers{};
        size_t preBarrierCount = 0;
        if (first) {
            preBarriers.at(preBarrierCount++) = barrierHelper(
                originalSource,
                VK_ACCESS_NONE,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
            );
        }
        preBarriers.at(preBarrierCount++) = barrierHelper(
            acquiredImage,
            VK_ACCESS_NONE,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
        );

        std::array<vk::Barrier, 2> postBarriers{};
        size_t postBarrierCount = 0;
        postBarriers.at(postBarrierCount++) = barrierHelper(
                acquiredImage,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_MEMORY_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
        );
        if (last) {
            postBarriers.at(postBarrierCount++) = barrierHelper(
                originalSource,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_MEMORY_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
            );
        }

        auto& commandBuffer = pass.commandBuffer;
        commandBuffer.begin(vk);
        commandBuffer.blitImage(
            vk, std::span{preBarriers}.first(preBarrierCount),
            {originalSource, acquiredImage}, this->info.extent,
            std::span{postBarriers}.first(postBarrierCount)
        );
        commandBuffer.end(vk);

        std::vector<VkSemaphore> waits{
            pass.acquireSemaphore.handle()
        };
        if (first) {
            waits.insert(
                waits.end(), applicationWaitSemaphores.begin(),
                applicationWaitSemaphores.end()
            );
        } else {
            const size_t previousSemaphoreIndex =
                (semaphoreBase + i - 1) % this->postCopySemaphores.size();
            waits.push_back(
                this->postCopySemaphores.at(previousSemaphoreIndex)
                    .second.handle()
            );
        }
        commandBuffer.submit(
            vk, waits, VK_NULL_HANDLE, 0,
            std::array{this->postCopySemaphores.at(acquiredImageIndices[i]).first.handle(),
                last ? this->postCopySemaphores.at(originalSlot).first.handle() : pcs.second.handle()},
            VK_NULL_HANDLE, 0,
            last ? this->currentRenderSlot().fence.handle() : VK_NULL_HANDLE
        );
    }
    this->currentRenderSlot().inFlight = true;

    for (size_t i = 0; i < acquiredImageIndices.size(); ++i) {
        const VkSemaphore waitSemaphore =
            this->postCopySemaphores.at(acquiredImageIndices[i]).first.handle();
        const uint32_t acquiredImageIndex = acquiredImageIndices[i];
        const VkPresentInfoKHR acquiredPresentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = nullptr,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &waitSemaphore,
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &acquiredImageIndex,
        };
        const auto acquiredResult = this->queuePresentWithRetirementFence(
            vk, queue, acquiredPresentInfo
        );
        if (acquiredResult != VK_SUCCESS &&
                acquiredResult != VK_SUBOPTIMAL_KHR) {
            throw ls::vulkan_error(
                acquiredResult, "vkQueuePresentKHR() failed"
            );
        }
    }

    const VkSemaphore originalWaitSemaphore =
        this->postCopySemaphores.at(originalSlot).first.handle();
    const VkPresentInfoKHR originalPresentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = nextChain,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &originalWaitSemaphore,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &originalImageIndex,
    };
    const auto result = this->queuePresentWithRetirementFence(
        vk, queue, originalPresentInfo, {.applicationImage = true}
    );
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        throw ls::vulkan_error(result, "vkQueuePresentKHR() failed");

    if (presentDiagnosticsEnabled()) {
        std::cerr << "framegen-vulkan: present diagnostics: "
                     "operation=retire-acquired-images"
                  << " context=" << this->diagnosticsState.contextId
                  << " images=" << acquiredImageIndices.size()
                  << " reason=backend-schedule-failure\n";
    }
    this->frameState.realFrameIndex++;
    return result;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain/swapchain.hpp"
#include "swapchain/create_policy.hpp"
#include "swapchain/present/internal.hpp"
#include "swapchain/retirement.hpp"
#include "fgvk-common/helpers/errors.hpp"
#include "fgvk-common/vulkan/command_buffer.hpp"
#include "fgvk-common/vulkan/image.hpp"
#include "fgvk-common/vulkan/semaphore.hpp"
#include "fgvk-common/vulkan/vulkan.hpp"
#include "present_diagnostics.hpp"
#include "pnext_chain.hpp"
#include "wayland_presentation_feedback.hpp"
#include "x11_refresh.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <locale>
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

namespace {
#if defined(VK_KHR_present_id2)
// The application's present-ID nodes (VkPresentIdKHR or VkPresentId2KHR): the largest
// non-zero ID, whether it came from VkPresentId2KHR, and whether any such node is present.
struct ApplicationPresentIds {
    std::optional<uint64_t> id;
    bool id2{};
    bool node{};
};
ApplicationPresentIds applicationPresentIds(const void* chain) {
    ApplicationPresentIds found;
    for (auto* node = static_cast<const VkBaseInStructure*>(chain); node; node = node->pNext) {
        // VkPresentIdKHR and VkPresentId2KHR share {sType, pNext, swapchainCount, pPresentIds}.
        if (node->sType != VK_STRUCTURE_TYPE_PRESENT_ID_KHR && node->sType != VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR)
            continue;
        found.node = true;
        const auto* ids = reinterpret_cast<const VkPresentId2KHR*>(node);
        if (ids->swapchainCount >= 1 && ids->pPresentIds && ids->pPresentIds[0] &&
                (!found.id || ids->pPresentIds[0] > *found.id)) {
            found.id = ids->pPresentIds[0];
            found.id2 = node->sType == VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR;
        }
    }
    return found;
}
#endif

// Once per process: the first present ID token of the release gate (diagnostics only).
void noteFirstPresentIdReleaseToken() {
    static std::atomic_bool logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::clog << "framegen-vulkan: release gate: present-id tokens active\n";
}
}

void Swapchain::PresentInvocation::waitForOutput(
        const DiagnosticsClock::time_point deadline) const {
    const auto waitStarted = DiagnosticsClock::now();
    const bool readyPacing = this->readyOutputPacing || this->capReadyOutputPacing;
    auto effectiveDeadline = readyPacing && this->previousOutput
        ? std::max(deadline, *this->previousOutput + this->pacedOutputs->interval) : deadline;
    std::this_thread::sleep_until(effectiveDeadline);
    const auto finished = DiagnosticsClock::now();
    this->outputWait += finished - waitStarted;
    if (this->profiler) this->profiler->wait(present_diagnostics::PacingProfile::OutputWakeLateness,
        finished - std::max(effectiveDeadline, waitStarted));
    if (readyPacing)
        this->previousOutput = finished;
    // Intentional pacing sleep is not present work. Excluding it keeps the
    // phase timing from reporting a capped frame as slow GPU work.
    this->started += finishPhaseTimer(waitStarted);
}

std::optional<bool> Swapchain::previousBatchBusy() const {
    if (!this->instance || this->destinationImages.empty()) return std::nullopt;
    try {
        return this->instance->previousBatchPending(this->ctx.get());
    } catch (const std::exception&) {
        // A failed status query is no observation: the hold keeps its value for this present.
        return std::nullopt;
    }
}

void Swapchain::resetGpuRelease() noexcept {
    this->gpuPredictor.reset();
    this->gpuStepping.reset();
    this->gpuArmed.reset();
}

// GPU release of the capped present that arrived at `arrival` (L5, low_latency.hpp). The frame's
// release token returned to the application at some time after the previous present (its wait
// hook, release_gate.hpp noteReturned). That return is the frame's release time: it is observed with
// this arrival, and the newest backend batch end is observed too. The release of the next frame is
// then predicted, to be armed on this present's token. Without split timestamps the stepping hold
// runs on a non-blocking fence query instead. Inactive presents reset both. Nothing waits here.
Swapchain::GpuRelease Swapchain::stepGpuRelease(const bool active,
        const std::chrono::steady_clock::time_point arrival,
        const std::chrono::steady_clock::duration capInterval) {
    using Clock = std::chrono::steady_clock;
    const auto returned = this->releaseGate.takeReturnedAt();
    const auto armed = std::exchange(this->gpuArmed, std::nullopt);
    const auto previousArrival = std::exchange(this->gpuPreviousArrival, arrival);
    const bool hasBackend = this->instance && !this->destinationImages.empty();
    if (!active || !hasBackend || capInterval <= Clock::duration::zero()) {
        this->resetGpuRelease();
        if (this->pacingProfile) this->pacingProfile->gpuRelease(GpuReleaseMode::Off, std::nullopt);
        return {};
    }
    // The return of this frame's token lies after the previous present and not after this arrival.
    const bool timed = returned && previousArrival && *returned > *previousArrival && *returned <= arrival;
    if (this->instance->splitProfileEnabled(this->ctx.get())) {
        if (!this->gpuPredictor) this->gpuPredictor.emplace(capInterval);
        auto& predictor = *this->gpuPredictor;
        this->gpuStepping.reset();
        // The batch index this present's source takes when it is scheduled.
        const uint64_t batch = this->frameState.backendFrameIndex;
        std::optional<Clock::duration> hold;
        if (timed) {
            if (armed) hold = std::max(*armed - *returned, Clock::duration::zero());
            if (const auto lead = predictor.observeArrival(batch, *returned, arrival, capInterval,
                    armed.has_value());
                    lead && this->pacingProfile)
                this->pacingProfile->gpuPrevEnd(*lead);
        }
        try {
            this->instance->pollSplitProfiles(this->ctx.get());
        } catch (const std::exception&) {
            // A failed status query is no observation; the next present polls again.
        }
        if (const auto end = this->instance->latestBatchEnd(this->ctx.get())) {
            const Clock::time_point endAt{Clock::duration{end->batchEndNs}};
            if (const auto lead = predictor.observeBatchEnd(end->backendFrameIndex, endAt, capInterval);
                    lead && this->pacingProfile)
                this->pacingProfile->gpuPrevEnd(*lead);
        }
        // The next frame's source takes the batch after this one, if this present schedules one.
        const auto release = predictor.release(batch + 1);
        this->gpuArmed = release;
        // Without a fresh enough batch end nothing is armed: the log says so instead of `pred`.
        const auto mode = release ? GpuReleaseMode::Predicted : GpuReleaseMode::Waiting;
        if (this->pacingProfile) this->pacingProfile->gpuRelease(mode, hold);
        return {mode, GpuHold{release, Clock::duration::zero()}};
    }
    this->gpuPredictor.reset();
    if (const auto busy = this->previousBatchBusy()) this->gpuStepping.observe(*busy, capInterval);
    const auto hold = this->gpuStepping.hold();
    if (this->pacingProfile) this->pacingProfile->gpuRelease(GpuReleaseMode::Fence, hold);
    return {GpuReleaseMode::Fence, GpuHold{std::nullopt, hold}};
}

VkResult Swapchain::present(const vk::Vulkan& vk,
        const VkQueue queue, const VkSwapchainKHR swapchain,
        void* nextChain, const uint32_t imageIndex,
        const std::span<const VkSemaphore> waitSemaphores,
        InternalPresentImage* const internal) {
    // The descriptor is visible to this call only. Clear it on every return and exception.
    struct InternalPresentBinding {
        InternalPresentImage*& slot;
        ~InternalPresentBinding() { slot = nullptr; }
    } internalBinding{this->internalPresent};
    this->internalPresent = internal;
    // X3: no application chain has reached a lower present yet in this call.
    this->applicationChainHandedOff = false;
    // Diagnostics only: marks this swapchain's pacing window as using F5 internal images.
    if (internal && this->pacingProfile) this->pacingProfile->internalImagesActive();
    if (internal && internal->applicationIndex != imageIndex)
        throw ls::vulkan_error(VK_ERROR_VALIDATION_FAILED_EXT,
            "internal image descriptor does not match the presented index");
    this->ensureInternalPresentSlots(vk);
    // Applications may acquire several images before their first present.
    // Such a caller can reach another present without an intervening acquire
    // to observe the retained operation's error. Return its current image and
    // consume the current waits exactly once before propagating that error.
    // A previous present's token must not attach to an unrelated submit.
    ReleaseGate::disarm();
    // This thread's last timeline submit on the presenting queue, taken before any work
    // of this present (release_gate.hpp, SubmitMark).
    const auto lastSubmit = ReleaseGate::takeSubmitMark(queue);
    const auto deferred = this->completeDeferredPresentRetainingOverlap();
    this->deferredOutputQueuedThisFrame = false;
    if (deferred < 0) {
        const VkPresentInfoKHR original{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = nextChain,
            .waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size()),
            .pWaitSemaphores = waitSemaphores.data(), .swapchainCount = 1,
            .pSwapchains = &swapchain, .pImageIndices = &imageIndex,
        };
        const auto drained = this->queuePresentWithRetirementFence(vk, queue, original,
            {.applicationImage = true});
        return drained < 0 ? drained : deferred;
    }
    using PacingProfile = present_diagnostics::PacingProfile;
    struct ProfileCompletion {
        Swapchain& owner;
        VkSwapchainKHR swapchain;
        ~ProfileCompletion() {
            if (!owner.pacingProfile) return;
            const auto now = DiagnosticsClock::now();
            if (present_diagnostics::pacingFrameTraceEnabled())
                present_diagnostics::logPacingSource(owner.diagnosticsState.contextId, swapchain,
                    owner.pacingProfile->sourceSample(now));
            const auto window = owner.pacingProfile->finishSource(now);
            if (window) present_diagnostics::logPacingProfile(
                owner.diagnosticsState.contextId, owner.currentPresentMode, now, *window, swapchain,
                surfaceOriginName(owner.info.surfaceOrigin), static_cast<bool>(owner.info.waylandPresentation));
            owner.pacingProfile->publicationCost(DiagnosticsClock::now() - now);
        }
    } profileCompletion{*this, swapchain};
    if (this->pacingProfile) {
        this->pacingProfile->beginSource(DiagnosticsClock::now());
        present_diagnostics::bindApplicationPresentSource(this->diagnosticsState.contextId,
            this->pacingProfile->sourceOrdinal());
    }
    if (this->pacingProfile && present_diagnostics::pacingGpuProfileEnabled() &&
            this->instance && !this->destinationImages.empty()) {
        // Already collected after the backend's normal previous-work fence
        // wait. Consuming the sample adds no Vulkan query or GPU wait here.
        if (auto gpu = this->instance->takeCompletedFrameProfile(this->ctx.get())) {
            using Profile = present_diagnostics::PacingProfile;
            if (present_diagnostics::pacingFrameTraceEnabled())
                present_diagnostics::logPacingGpuFrame(this->diagnosticsState.contextId,
                    this->pacingProfile->sourceForBackend(gpu->backendFrameIndex),
                    this->pacingProfile->backendGeneration(), gpu->backendFrameIndex,
                    gpu->cpuPreviousWaitUs, gpu->cpuPrepassSubmitUs, gpu->cpuGenerationSubmitUs,
                    gpu->gpuPrepassUs, gpu->gpuSpanUs, gpu->cpuReadbackUs, gpu->gpuGeneratedUs);
            this->pacingProfile->gpuMicroseconds(Profile::GpuPrepass, gpu->gpuPrepassUs);
            double generatedUs{};
            for (const double outputUs : gpu->gpuGeneratedUs) {
                generatedUs += outputUs;
                this->pacingProfile->gpuMicroseconds(Profile::GpuOutput, outputUs);
            }
            this->pacingProfile->gpuMicroseconds(Profile::GpuGeneration, generatedUs);
            this->pacingProfile->gpuMicroseconds(Profile::GpuSpan, gpu->gpuSpanUs);
            this->pacingProfile->gpuMicroseconds(Profile::GpuReadback, gpu->cpuReadbackUs);
        }
    }
    if (this->pacingProfile && present_diagnostics::pacingGpuSplitProfileEnabled() &&
            this->instance && !this->destinationImages.empty()) {
        // Samples were collected after their fences signalled; reading them adds no GPU wait. The
        // backend half can complete before the render half is collected (the L5 poll reads batches
        // as soon as they finish), so a sample waits up to splitRenderWaitPresents presents for it.
        for (const auto& split : this->instance->takeCompletedSplitProfiles(this->ctx.get()))
            this->splitAwaitingRender.push_back({split, 0});
        std::erase_if(this->splitAwaitingRender, [this](auto& pending) {
            const auto& split = pending.first;
            const auto source = this->pacingProfile->sourceForBackend(split.backendFrameIndex);
            const auto render = this->findRenderSplitHalf(source);
            if (render.gameDoneNs == 0 && source != 0 && pending.second++ < splitRenderWaitPresents)
                return false;
            present_diagnostics::logPacingGpuSplitFrame(this->diagnosticsState.contextId,
                source, split.backendFrameIndex,
                render.gameDoneNs, render.sourceCopyEndNs,
                split.fgStartNs, split.prepassEndNs, split.generatedEndNs, split.batchEndNs);
            return true;
        });
    }
    const DiagnosticsContextScope diagnosticsContext(
        this->diagnosticsState.contextId, this->pacingProfile.get()
    );
    this->bridgeOutputBatchSize = 1;
    this->outputSourceDeadline.reset();
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
    if (this->info.ownedPresentTiming && !this->presentTimingInitialized &&
            this->info.presentMode == VK_PRESENT_MODE_FIFO_KHR) {
        this->presentTimingInitialized = true;
        const auto get = [&](const char* name) { return vk.fi().GetDeviceProcAddr(vk.dev(), name); };
        const bool enabled = this->presentTiming.initialize(vk.dev(), swapchain, {
            .setSize = reinterpret_cast<PFN_vkSetSwapchainPresentTimingQueueSizeEXT>(get("vkSetSwapchainPresentTimingQueueSizeEXT")),
            .properties = reinterpret_cast<PFN_vkGetSwapchainTimingPropertiesEXT>(get("vkGetSwapchainTimingPropertiesEXT")),
            .domains = reinterpret_cast<PFN_vkGetSwapchainTimeDomainPropertiesEXT>(get("vkGetSwapchainTimeDomainPropertiesEXT")),
            .past = reinterpret_cast<PFN_vkGetPastPresentationTimingEXT>(get("vkGetPastPresentationTimingEXT")),
            .calibrate = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(get("vkGetCalibratedTimestampsKHR")),
        }, this->info.ownedPresentTimingTargetStage);
        std::clog << "framegen-vulkan: presentation timing initialized; owner=renderer; "
                     "transport=VK_EXT_present_timing; active=" << enabled << '\n';
    }
    const bool hadPresentTiming = this->presentTiming.active();
    this->presentTiming.update(this->wsiPresentTimeline, this->frameState.realFrameIndex);
    if (hadPresentTiming && !this->presentTiming.active())
        std::clog << "framegen-vulkan: presentation timing fallback; owner=renderer; "
                     "transport=VK_EXT_present_timing; reason=feedback-or-clock-unavailable\n";
#endif
    // Match the immutable create-time choice. Ordered SDR filters the
    // application's dynamic MAILBOX override so the private transport stays ordered.
    // Feedback cannot change the game-owned swapchain's creation contract.
    // Build a filtered copy of the present-chain prefix: Vulkan input chains
    // are const and may be shared or stored in read-only memory.
    const FilteredPresentPNextChain filteredPresentChain(
        nextChain, this->privateOrderedTransport
    );
    if (!filteredPresentChain.valid()) {
        throw ls::vulkan_error(
            VK_ERROR_UNKNOWN,
            "cannot safely filter an unknown VkPresentInfoKHR pNext prefix "
            "structure (sType=" + std::to_string(
                static_cast<int>(
                    filteredPresentChain.unsupportedStructureType()
                )
            ) + ")"
        );
    }
    const void* lowerNextChain = filteredPresentChain.head();
#if defined(VK_KHR_present_id2)
    // The application's own present ID, when its chain attached one.
    const auto applicationIds = applicationPresentIds(nextChain);
#endif
#if defined(VK_KHR_present_id2) && defined(VK_KHR_present_wait2) && defined(VK_KHR_present_wait)
    VkPresentId2KHR feedbackId{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR};
    if (this->info.applicationPresentFeedback) {
        const auto logProbeStop = [this](const int result) {
            const char* reason = this->presentFeedback.stopReason() == PresentFeedbackProbe::Stop::Collision
                ? "collision" : "error";
            std::clog << "framegen-vulkan: present feedback probe: state=stopped reason=" << reason
                      << " last_tagged=" << this->presentFeedback.lastTagged()
                      << " result=" << result << '\n';
        };
        if (this->presentFeedback.observeApplicationId(applicationIds.id, applicationIds.id2)) {
            if (this->presentFeedback.following())
                std::clog << "framegen-vulkan: present feedback probe: state=following reason=application-ids\n";
            else
                logProbeStop(0);
        }
        // Poll before tagging, never blocking.
        if (const auto target = this->presentFeedback.pollTarget()) {
            PresentFeedbackProbe::PollResult poll = PresentFeedbackProbe::PollResult::Error;
            VkResult waited = VK_ERROR_EXTENSION_NOT_PRESENT;
            bool polled = true;
            if (this->presentFeedback.following()) {
                // The application's own ID. The worker's registry answers a timeout-0 poll for an
                // ID it still holds (its real is not yet presented) without any driver call.
                // Without the driver function there is no poll at all.
                const bool id2 = this->presentFeedback.pollTargetIsId2();
                if (id2 && !this->presentWait2)
                    this->presentWait2 = reinterpret_cast<PFN_vkWaitForPresent2KHR>(
                        vk.fi().GetDeviceProcAddr(vk.dev(), "vkWaitForPresent2KHR"));
                if (!id2 && !this->presentWait1)
                    this->presentWait1 = reinterpret_cast<PFN_vkWaitForPresentKHR>(
                        vk.fi().GetDeviceProcAddr(vk.dev(), "vkWaitForPresentKHR"));
                polled = id2 ? this->presentWait2 != nullptr : this->presentWait1 != nullptr;
                if (polled)
                    waited = DeferredPresent::waitForPresent(vk.dev(), swapchain, *target, 0,
                        [&](const uint64_t timeout) {
                            if (id2) {
                                const VkPresentWait2InfoKHR wait{.sType = VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR, .presentId = *target, .timeout = timeout};
                                return this->presentWait2(vk.dev(), swapchain, &wait);
                            }
                            return this->presentWait1(vk.dev(), swapchain, *target, timeout);
                        }, false);
            } else {
                if (!this->presentWait2)
                    this->presentWait2 = reinterpret_cast<PFN_vkWaitForPresent2KHR>(
                        vk.fi().GetDeviceProcAddr(vk.dev(), "vkWaitForPresent2KHR"));
                if (this->presentWait2) {
                    const VkPresentWait2InfoKHR wait{.sType = VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR, .presentId = *target, .timeout = 0};
                    waited = this->presentWait2(vk.dev(), swapchain, &wait);
                }
            }
            if (polled) {
                poll = waited == VK_SUCCESS || waited == VK_SUBOPTIMAL_KHR ? PresentFeedbackProbe::PollResult::Success
                    : waited == VK_TIMEOUT || waited == VK_NOT_READY ? PresentFeedbackProbe::PollResult::Timeout
                    : PresentFeedbackProbe::PollResult::Error;
                if (this->presentFeedback.pollResult(*target, poll))
                    std::clog << "framegen-vulkan: present feedback probe: state=confirmed polls="
                              << this->presentFeedback.successes() << " id=" << *target << '\n';
                if (poll == PresentFeedbackProbe::PollResult::Error)
                    logProbeStop(static_cast<int>(waited));
            }
        }
        const bool firstTag = this->presentFeedbackTag == 0;
        // An application ID node of any value keeps this present untagged:
        // the deferred worker rejects a second ID node in one chain.
        std::optional<uint64_t> tag;
        if (!applicationIds.node)
            tag = this->presentFeedback.nextTag();
        if (tag) {
            if (firstTag)
                std::clog << "framegen-vulkan: present feedback probe: state=active\n";
            this->presentFeedbackTag = *tag;
            feedbackId.pNext = lowerNextChain;
            feedbackId.swapchainCount = 1;
            feedbackId.pPresentIds = &this->presentFeedbackTag;
            lowerNextChain = &feedbackId;
        }
    }
#endif
    for (auto* node = static_cast<const VkBaseInStructure*>(lowerNextChain); node; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT) {
            const auto* modes = reinterpret_cast<const VkSwapchainPresentModeInfoEXT*>(node);
            if (modes->swapchainCount == 1 && modes->pPresentModes &&
                    this->currentPresentMode != modes->pPresentModes[0]) {
                // P1: a per-present mode change (in-game VSync toggle) starts a new
                // output clock; batches already admitted keep the mode they were sent with.
                this->currentPresentMode = modes->pPresentModes[0];
                this->realFramePacer.resetOutputs();
            }
        }
    }
    {
        // -1: no per-present mode, the create-time mode applies.
        const auto chainMode = [](const void* chain) -> int64_t {
            const auto found = present_diagnostics::applicationPresentMode(
                VK_PRESENT_MODE_MAX_ENUM_KHR, chain);
            if (!found.dynamic) return -1;
            return found.mode < 0 ? VK_PRESENT_MODE_MAX_ENUM_KHR : found.mode;
        };
        const int64_t applicationMode = chainMode(nextChain);
        const int64_t realMode = chainMode(lowerNextChain);
        if (this->pacingProfile)
            this->pacingProfile->presentModes(applicationMode, realMode, this->info.presentMode);
        if (applicationMode != this->loggedApplicationPresentMode ||
                realMode != this->loggedRealPresentMode) {
            this->loggedApplicationPresentMode = applicationMode;
            this->loggedRealPresentMode = realMode;
            std::cerr << "framegen-vulkan: per-present mode: context=" << this->diagnosticsState.contextId
                      << " swapchain_present_mode=" << static_cast<int64_t>(this->info.presentMode)
                      << " application_present_mode=" << applicationMode
                      << " real_present_mode=" << realMode
                      << " tracked_present_mode=" << static_cast<int64_t>(this->currentPresentMode)
                      << " (-1 = none, create-time mode applies)\n";
        }
    }
    const bool directUnthrottled = directUnthrottledPresentation(
        this->currentPresentMode);
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
    this->presentTiming.observePresentMode(lowerNextChain);
#endif

    // Moved to the wake time by the release-gate catch-up below; the clock
    // (realFramePacer.schedule) still sees the arrival.
    auto limiterArrival = DiagnosticsClock::now();
    // Transport half of worker admission; the pacing half is decided below.
    const bool detachedOutputTransport =
        detachedOutputModeAdmits(directUnthrottled, this->currentPresentMode) &&
        DeferredPresent::supportsChain(lowerNextChain) &&
        this->info.deferredPresentQueue && queue == vk.queue();
    double baseFpsCap = effectiveBaseFpsCap(this->profile);
    // Refresh bounds the automatic cap only. Query on each of the first 64
    // presents, and every 8th after that, until a rate is known; then once per
    // 240 presents, from the owner that already has it.
    const bool query = UncappedAutoCap::refreshQueryDue(this->refreshQuerySource,
        this->applicationRefreshHz > 0);
    ++this->refreshQuerySource;
    if (query) {
        double refreshHz = 0.0;
        const char* refreshSource = "none";
        const char* branch = "none";
        const bool ownedTiming = this->presentTiming.active();
        int32_t timingResult = 0;
        uint64_t timingDuration = 0;
        uint64_t timingInterval = 0;
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
        if (ownedTiming) {
            refreshHz = this->presentTiming.refreshRate(0);
            refreshSource = "owned";
            branch = "owned";
        } else if (this->info.applicationSwapchainTiming) {
            branch = "application";
            if (!this->swapchainTimingProperties)
                this->swapchainTimingProperties = reinterpret_cast<PFN_vkGetSwapchainTimingPropertiesEXT>(
                    vk.fi().GetDeviceProcAddr(vk.dev(), "vkGetSwapchainTimingPropertiesEXT"));
            VkSwapchainTimingPropertiesEXT timing{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT};
            uint64_t counter{};
            VkResult timingQuery = VK_ERROR_EXTENSION_NOT_PRESENT;
            if (this->swapchainTimingProperties)
                timingQuery = this->swapchainTimingProperties(vk.dev(), swapchain, &timing, &counter);
            timingResult = static_cast<int32_t>(timingQuery);
            timingDuration = timing.refreshDuration;
            timingInterval = timing.refreshInterval;
            if (timingQuery == VK_SUCCESS) {
                refreshHz = UncappedAutoCap::refreshHzFromTiming(timing.refreshDuration, timing.refreshInterval);
                refreshSource = "application";
                // The driver reports refresh only after presentation feedback
                // was requested; keep the reported timing but distrust it until
                // timeout-0 polls have succeeded.
                if (!this->presentFeedback.confirmed()) {
                    refreshHz = 0.0;
                    refreshSource = "application-unconfirmed";
                }
            }
        }
#endif
        // X5: an X11 surface whose owned and application timing gave no rate takes the rate its detached
        // creation-time query has published (x11_refresh.cpp), through the same range check. The hz load
        // is acquire, so the source stored before it is visible; zero means not yet known.
        const double x11RefreshHz = this->x11Refresh
            ? this->x11Refresh->hz.load(std::memory_order_acquire) : 0.0;
        if (refreshHz <= 0 && x11RefreshHz > 0) {
            refreshHz = UncappedAutoCap::refreshHzInRange(x11RefreshHz);
            if (refreshHz > 0) {
                branch = "x11-randr";
                refreshSource = this->x11Refresh->source.load(std::memory_order_relaxed);
            }
        }
        // X6: a native Wayland surface with no Vulkan rate takes the output refresh its wp_presentation
        // feedback reported (the observer's lock-free value; 0 until the first presented event).
        if (refreshHz <= 0 && this->info.waylandPresentation) {
            refreshHz = UncappedAutoCap::refreshHzFromWaylandNs(this->info.waylandPresentation->refreshNs());
            if (refreshHz > 0) {
                branch = "wayland-presentation";
                refreshSource = "wp-presentation";
            }
        }
        if (refreshHz <= 0)
            refreshSource = "none";
        const bool feedbackConfirmed = this->presentFeedback.confirmed();
        if (!this->refreshQueryPrinted || timingResult != this->refreshQueryLastResult ||
                timingDuration != this->refreshQueryLastDuration ||
                timingInterval != this->refreshQueryLastInterval ||
                feedbackConfirmed != this->refreshQueryConfirmed) {
            std::clog << "framegen-vulkan: uncapped auto cap refresh query: branch=" << branch
                      << " application_timing=" << (this->info.applicationSwapchainTiming ? 1 : 0)
                      << " owned_timing=" << (ownedTiming ? 1 : 0)
                      << " result=" << timingResult
                      << " refresh_duration=" << timingDuration
                      << " refresh_interval=" << timingInterval
                      << " hz=" << refreshHz
                      << " feedback=" << (feedbackConfirmed ? 1 : 0)
                      << " feedback_polls=" << this->presentFeedback.successes()
                      << " presents=" << this->refreshQuerySource << '\n';
            this->refreshQueryLastResult = timingResult;
            this->refreshQueryLastDuration = timingDuration;
            this->refreshQueryLastInterval = timingInterval;
            this->refreshQueryConfirmed = feedbackConfirmed;
            this->refreshQueryPrinted = true;
        }
        if (std::abs(refreshHz - this->applicationRefreshHz) > 0.5) {
            std::clog << "framegen-vulkan: uncapped auto cap refresh: hz=" << refreshHz
                      << " source=" << refreshSource << '\n';
        }
        this->applicationRefreshHz = refreshHz;
    }
    // Uncapped Fixed frame generation gets an automatic cap so the proven
    // capped path (release gate, worker spacing) runs. A binding user cap wins.
    const double autoCapCeiling = this->applicationRefreshHz > 0 && this->profile.multiplier > 0
        ? this->applicationRefreshHz / static_cast<double>(this->profile.multiplier) : 0.0;
    const bool uncappedAutoCapEligible =
        effectiveFrameGenerationEnabled(this->profile) &&
        !this->recoveryState.orderedAcquireRecovery.active() &&
        detachedOutputTransport && this->profile.multiplier >= 2;
    const bool userCapBinding = baseFpsCap > 0 &&
        (autoCapCeiling <= 0 || baseFpsCap <= autoCapCeiling);
    bool autoCapApplied = false;
    if (uncappedAutoCapEligible && !userCapBinding) {
        const double autoCap = autoCapCeiling;
        if (autoCap > 0) {
            baseFpsCap = autoCap;
            autoCapApplied = true;
        }
        if (std::abs(autoCap - this->uncappedAutoCapLogged) > 0.01) {
            std::clog << "framegen-vulkan: uncapped auto cap: fps=" << autoCap
                      << " ceiling=" << autoCapCeiling
                      << " configured=" << this->profile.base_fps_cap
                      << " reason=refresh" << '\n';
            this->uncappedAutoCapLogged = autoCap;
        }
    } else {
        this->uncappedAutoCapLogged = 0.0;
    }
    const bool autoCapRetune = autoCapApplied && this->uncappedAutoCapApplied;
    this->uncappedAutoCapApplied = autoCapApplied;
    if (this->pacingProfile) this->pacingProfile->policy(
        baseFpsCap,
        this->currentPresentMode == VK_PRESENT_MODE_FIFO_KHR &&
            this->info.applicationPresentTiming);
    if (this->pacingProfile) this->pacingProfile->settings(
        this->profile.frame_generation_enabled, false,
        this->profile.multiplier, this->profile.base_fps_cap);
    // Frame generation at a cap puts the cap's interval under the application's Reflex limiter, so
    // the game itself holds the cap. Only a changed interval reaches the driver (reflex_frame_limit.hpp).
    this->reflexFrameLimit(swapchain).observe(swapchain, effectiveFrameGenerationEnabled(this->profile), baseFpsCap,
        [&](const ReflexFrameLimit::Forward& forward) {
            const auto setSleepMode = reinterpret_cast<PFN_vkSetLatencySleepModeNV>(
                vk.fi().GetDeviceProcAddr(vk.dev(), "vkSetLatencySleepModeNV"));
            return setSleepMode ? setSleepMode(vk.dev(), swapchain, &forward.info)
                                : VK_ERROR_EXTENSION_NOT_PRESENT;
        });
    // An automatic cap change between two automatic values must move the next
    // source boundary, not grant a free frame; a free frame deepens the GPU
    // queue by one frame and costs about 0.3 s of render-fence holds near the
    // GPU limit.
    const auto limiterDeadline = this->realFramePacer.schedule(
        limiterArrival, baseFpsCap, autoCapRetune
    );
    const bool ownedTimingControlsOutput = this->presentTiming.controlsOutput(0);
    std::optional<uint32_t> sourceRefreshHz;
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
    if (ownedTimingControlsOutput) {
        const double refresh = this->presentTiming.refreshRate(0);
        if (OrderedPresentTimeline::validRate(refresh))
            sourceRefreshHz = static_cast<uint32_t>(std::round(refresh));
    }
#endif
    // These transports already own an output clock. Preparing inside a
    // target-matched automatic cap must not add another CPU output clock.
    const bool compositorOutputTiming = ownedTimingControlsOutput;
    const bool applicationTimingOwner = hasPresentTiming(lowerNextChain);
    const bool nativeFifoOutputPacing = !compositorOutputTiming &&
        nativeFifoOutputPacingEligible(this->currentPresentMode);
    const auto capInterval = baseFpsCap > 0
        ? std::chrono::duration_cast<DiagnosticsClock::duration>(
            std::chrono::duration<double>(1.0 / baseFpsCap))
        : DiagnosticsClock::duration{};
    // The application releases its producer through its own wait for a
    // token recorded after this present (VKD3D-Proton without a present
    // ID). Apply the cap there instead of holding its queue-submission
    // thread. With a cap the release wait enforces it. Without one the
    // present still returns without joining the worker, and the release wait
    // is not delayed. The release wait holds only a deadline within three cap
    // intervals of the present. A present arriving earlier sleeps until three
    // intervals before its deadline and then counts as arriving at that wake
    // time, so every later read of limiterArrival sees the wake time. The clock
    // and the gate's deadline are unchanged, and the gate is still armed at the
    // deadline.
    const bool releaseGateHolds = this->releaseGate.active() &&
        detachedOutputTransport &&
        !this->recoveryState.orderedAcquireRecovery.active();
    if (releaseGateHolds) {
        if (const auto catchUp = releaseGateCatchUp(limiterArrival, limiterDeadline, capInterval)) {
            const auto waitStarted = DiagnosticsClock::now();
            std::this_thread::sleep_until(*catchUp);
            limiterArrival = DiagnosticsClock::now();
            if (this->pacingProfile) {
                this->pacingProfile->wait(PacingProfile::SourceCapWait, limiterArrival - waitStarted);
                this->pacingProfile->wait(PacingProfile::SourceCapWakeLateness,
                    limiterArrival - std::max(*catchUp, waitStarted));
            }
        }
    }
    const bool releaseGated = releaseGateHolds &&
        limiterDeadline - limiterArrival <= 3 * capInterval;
    // Just-in-time release (low_latency; low_latency.hpp). A capped source present sets the
    // delay of its release point from its lead after the catch-up above. The cap clock and
    // limiterDeadline do not move. The delay is zero, and every hold it adds is off, without a
    // cap or frame generation and while the application's Reflex limiter holds the game. The
    // just-in-time batch start stays on with Reflex (latency.justInTime).
    const auto latency = lowLatencyPlan(this->profile.low_latency, baseFpsCap > 0,
        effectiveFrameGenerationEnabled(this->profile),
        this->reflexFrameLimit(swapchain).reflexLowLatency());
    const auto releaseDelay = this->lowLatencyRelease.update(latency.releaseDelay,
        limiterDeadline - limiterArrival, capInterval);
    const auto releasePoint = limiterDeadline + releaseDelay;
    // GPU release (L5, low_latency.hpp): while the release delay runs, the next frame's release
    // point is also no earlier than the predicted GPU release, or a stepping hold without split
    // timestamps. Otherwise every token keeps its deadline.
    const auto gpuRelease = this->stepGpuRelease(latency.releaseDelay, limiterArrival, capInterval);
    // The native acquire fallback holds the application's next acquire until this point, one
    // cap interval before this present's release point (entrypoint.cpp).
    this->releaseGate.setAcquireRelease(latency.releaseDelay
        ? std::optional{releasePoint - capInterval} : std::nullopt, gpuRelease.hold);
    // Recorded before the sleeps below: a game wait for this submit may complete during them.
    if (latency.releaseDelay && lastSubmit)
        this->releaseGate.recordLastSubmit(lastSubmit->semaphore, lastSubmit->value, releasePoint,
            gpuRelease.hold);
    if (this->pacingProfile) this->pacingProfile->lowLatency(latency.justInTime, releaseDelay,
        this->releaseGate.consumedKinds() | (latency.reflexHolds ? reflexKindBit : 0u),
        latency.justInTime);
    const bool readyOutputPacing = fixedReadyOutputPacingEligible(
        this->profile, directUnthrottled || nativeFifoOutputPacing,
        compositorOutputTiming || applicationTimingOwner,
        this->recoveryState.orderedAcquireRecovery.active(),
        detachedOutputTransport);
    const auto completionWait = this->deferredPresent
        ? this->deferredPresent->takeCompletionPacingWait() : DeferredPresent::CompletionWait{};
    const auto completionDelay = completionWait.finished <= limiterArrival
        ? completionWait.waited : DiagnosticsClock::duration{};
    this->realFramePacer.recordCompletionPacingWait(completionDelay);
    this->realFramePacer.observeSource(limiterArrival, readyOutputPacing || releaseGated,
        this->configuredFixedGeneratedFrames + 1, releaseGated);
    // Log only (completion_spiral.hpp): it reads the same completion wait and learned period
    // that pacing used above and changes nothing.
    const auto spiral = this->completionSpiral.observeSource(limiterArrival, completionDelay,
        this->realFramePacer.learnedSourcePeriod());
    if (spiral.event == CompletionSpiralDetector::Event::suspected)
        std::cerr << "framegen-vulkan: completion spiral suspected: period_ms=" << spiral.periodMs
                  << " completion_wait_ms=" << spiral.meanCompletionWaitMs << " windows=" << spiral.windows
                  << " context=" << this->diagnosticsState.contextId << '\n';
    else if (spiral.event == CompletionSpiralDetector::Event::cleared)
        std::cerr << "framegen-vulkan: completion spiral cleared: period_ms=" << spiral.periodMs
                  << " context=" << this->diagnosticsState.contextId << '\n';
    const bool sourceDeadlinePreparation = !releaseGated && limiterDeadline > limiterArrival &&
        baseCapPacesOutputs(
            this->profile, baseFpsCap,
            (this->privateOrderedTransport || ownedTimingControlsOutput || directUnthrottled ||
                nativeFifoOutputPacing) &&
                !this->recoveryState.orderedAcquireRecovery.active(),
            sourceRefreshHz, directUnthrottled, nativeFifoOutputPacing,
            applicationTimingOwner) &&
        orderedGeneratedBatchCanPrepareBeforeCap(
            this->info.requestedMinImageCount, this->info.images.size(),
            generatedFrameCapacityForActivePolicy(this->profile),
            this->recoveryState.generatedImageAdmission.underPressure());
    // Bridge and owned Vulkan timing already space outputs in the compositor.
    // Prepare inside the cap interval without adding a second output clock.
    const bool sourceDeadlinePacing = sourceDeadlinePreparation &&
        !compositorOutputTiming;
    if (sourceDeadlinePreparation && compositorOutputTiming)
        this->outputSourceDeadline = limiterDeadline;
    if (!sourceDeadlinePacing && !readyOutputPacing && !releaseGated)
        this->realFramePacer.resetOutputs();
    // The application's own present ID, when its chain attached one. Such a present
    // is released through that ID instead of the next submit (see ReleaseArm).
    std::optional<uint64_t> applicationPresentId;
#if defined(VK_KHR_present_id2)
    applicationPresentId = applicationIds.id;
#endif
    // With the release delay on, an ID on an immediate or mailbox present is not one the
    // application waits on (DXVK attaches an ID to every present but waits on it only in FIFO
    // modes). It would only fill the gate's backlog, so the present is released through its
    // submit like a present without an ID.
    if (latency.releaseDelay && (this->currentPresentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ||
            this->currentPresentMode == VK_PRESENT_MODE_MAILBOX_KHR))
        applicationPresentId.reset();
    // Destroyed after the return-deadline sleep below. Arm on every capped
    // return, including holds, so the release wait can be observed first. An
    // uncapped detached return arms at its arrival: that deadline never delays
    // the release wait, and arming lets the gate see the producer's token wait.
    // A present with an application ID records that ID as its token instead: the
    // application's wait for the ID is then the release wait, and no submit is armed.
    struct ReleaseArm {
        const ReleaseGateOwner& gate;
        VkQueue queue;
        VkSwapchainKHR swapchain;
        std::optional<DiagnosticsClock::time_point> deadline;
        std::optional<uint64_t> presentId;
        // The GPU hold of this token (L5); empty keeps the release at the deadline.
        GpuHold gpu;
        ~ReleaseArm() {
            if (!deadline) return;
            try {
                if (presentId) {
                    noteFirstPresentIdReleaseToken();
                    gate.recordPresentId(swapchain, *presentId, *deadline, gpu);
                } else {
                    gate.arm(queue, *deadline, gpu);
                }
            } catch (...) {}
        }
    } releaseArm{this->releaseGate, queue, swapchain,
        baseFpsCap > 0 ? std::optional{releasePoint}
            : detachedOutputTransport ? std::optional{limiterArrival}
            : std::nullopt,
        applicationPresentId, gpuRelease.hold};
    if (!releaseGated && !sourceDeadlinePreparation && limiterDeadline > limiterArrival) {
        const auto waitStarted = this->pacingProfile ? DiagnosticsClock::now() : DiagnosticsClock::time_point{};
        std::this_thread::sleep_until(limiterDeadline);
        if (this->pacingProfile) {
            const auto finished = DiagnosticsClock::now();
            this->pacingProfile->wait(PacingProfile::SourceCapWait, finished - waitStarted);
            this->pacingProfile->wait(PacingProfile::SourceCapWakeLateness,
                finished - std::max(limiterDeadline, waitStarted));
            this->pacingProfile->wait(PacingProfile::SourceDeadlineLateness,
                std::max(DiagnosticsClock::duration{}, finished - limiterDeadline));
        }
    }
    // Output deadlines may put the last image before the cap boundary. Keep
    // the existing application-return deadline on every exit, including
    // fallback and exception paths, without adding another frame of latency.
    struct SourceCapCompletion {
        std::optional<DiagnosticsClock::time_point> deadline;
        PacingProfile* profiler;
        ~SourceCapCompletion() {
            if (deadline) {
                const auto waitStarted = profiler ? DiagnosticsClock::now() : DiagnosticsClock::time_point{};
                std::this_thread::sleep_until(*deadline);
                if (profiler) {
                    const auto finished = DiagnosticsClock::now();
                    profiler->wait(PacingProfile::SourceCapWait, finished - waitStarted);
                    profiler->wait(PacingProfile::SourceCapWakeLateness,
                        finished - std::max(*deadline, waitStarted));
                    profiler->wait(PacingProfile::SourceDeadlineLateness,
                        std::max(DiagnosticsClock::duration{}, finished - *deadline));
                }
            }
        }
    } sourceCapCompletion{sourceDeadlinePreparation
        ? std::optional{limiterDeadline} : std::nullopt, this->pacingProfile.get()};

    const auto presentNow = DiagnosticsClock::now();
    // Observe the reserved real-frame cadence, while resource/recovery clocks
    // continue using wall time. Capped work uses the existing limiter's
    // slack; control returns to the game no earlier than that same deadline.
    const auto cadenceNow = sourceDeadlinePreparation
        ? std::max(limiterDeadline, presentNow) : presentNow;
    PresentInvocation invocation{
        .vk = vk,
        .queue = queue,
        .swapchain = swapchain,
        .nextChain = lowerNextChain,
        .imageIndex = imageIndex,
        .waitSemaphores = waitSemaphores,
        .cadenceStarted = cadenceNow,
        .started = startPhaseTimer(),
        .limiterArrival = limiterArrival,
        .originalPresentDeadline = sourceDeadlinePacing
            ? std::optional{limiterDeadline} : std::nullopt,
        .deferredOutputEligible = (readyOutputPacing || releaseGated ||
            sourceDeadlinePacing) && detachedOutputTransport,
        .profiler = this->pacingProfile.get(),
    };
    struct OutputWaitObservation {
        RealFramePacer& pacer;
        const PresentInvocation& invocation;
        PacingProfile* profiler;
        const bool& detachedOutput;
        ~OutputWaitObservation() {
            pacer.recordOutputWait(invocation.outputWait, detachedOutput,
                invocation.pacedOutputs.has_value());
            if (profiler) {
                profiler->wait(PacingProfile::ReadinessWait, invocation.readinessWait);
                profiler->wait(PacingProfile::OutputWait, invocation.outputWait - invocation.readinessWait);
                profiler->pacedBatch(invocation.readyOutputPacing,
                    invocation.pacedOutputs.has_value() && !invocation.readyOutputPacing);
                if (invocation.pacedOutputs) profiler->wait(PacingProfile::PlannedOutputInterval,
                    invocation.pacedOutputs->interval);
            }
        }
    } outputWaitObservation{this->realFramePacer, invocation, this->pacingProfile.get(),
        this->deferredOutputQueuedThisFrame};
    if (!this->applyPendingFrameGenerationResources(vk))
        return this->presentNativeFrame(invocation);

    // Frame generation is live-disabled; hand the game's own image directly
    // to the driver without copies, model scheduling or generated images. A
    // scaling-engine process may still attach its preallocated WSI-retirement
    // fence so natural resolution changes remain safe.
    if (!effectiveFrameGenerationEnabled(this->profile) ||
            !this->colorPipeline.generationSupported || !internal) {
        return this->presentNativeFrame(invocation);
    }
    const auto replacementStabilization =
        this->recoveryState.replacementBackendStabilization.beforeFrame(
            presentNow
        );
    if (replacementStabilization.bypassBackend)
        return this->presentNativeFrame(invocation);
    if (replacementStabilization.resumed)
        this->ensureHistoryWarmup();
    if (!this->recoverBackendIfReady(vk))
        return this->presentNativeFrame(invocation);

    bool orderedAcquireRecoveryProbe = false;
    bool boundedOrderedAcquireProbe = false;
    if (this->privateOrderedTransport) {
        const auto recovery =
            this->recoveryState.orderedAcquireRecovery.beforePresent(
                presentNow
            );
        if (recovery.bypassGeneration) {
            // No backend work or synthetic swapchain acquire is attempted
            // until the direct-failure retry deadline.
            return this->presentNativeFrame(invocation);
        }
        orderedAcquireRecoveryProbe = recovery.limitGeneratedFrames;
        boundedOrderedAcquireProbe = recovery.boundedAcquireProbe;
        if (recovery.beginHistoryWarmup) {
            this->ensureHistoryWarmup();
            this->fixedRefreshBudget.reset();
        }
    }

    const auto swapchainImage = this->applicationImageFor(imageIndex);
    // Presentation counters continue across live-off intervals, while the
    // backend's temporal history does not. Index sources by frames actually
    // submitted to the backend so native-only frames cannot invert history.
    const auto& sourceImage = this->sourceImages.at(
        this->frameState.backendFrameIndex % 2
    );
    auto plan = this->prepareFramePlan(
        cadenceNow, orderedAcquireRecoveryProbe
    );
    plan.boundedOrderedAcquireProbe = boundedOrderedAcquireProbe;

    this->frameState.nextSourceReleaseTimelineValue = 0;
    // Resolve prior work before acquiring any generated images or reusing
    // their acquire semaphores. A fence-budget miss must leave no new image
    // held by this plan when it falls back to native presentation.
    const auto renderFenceStarted = startPhaseTimer();
    const bool renderFenceReady = this->prepareRenderFence(vk);
    plan.renderFenceWaitDuration = finishPhaseTimer(
        renderFenceStarted
    );
    if (!renderFenceReady) {
        this->handleRenderFenceBudgetMiss();
        return this->presentNativeFrame(invocation);
    }

    if (this->privateOrderedTransport) {
        plan.configuredAcquireTimeout = orderedGeneratedBatchAcquireBudget(
            this->info.requestedMinImageCount, this->info.images.size(),
            plan.requestedGeneratedFrames.size(), plan.configuredAcquireTimeout
        );
    }
    const bool headroomTightOrderedBatch =
        this->privateOrderedTransport && !orderedAcquireRecoveryProbe &&
        orderedGeneratedBatchNeedsNonblockingAdmission(
            this->info.requestedMinImageCount,
            this->info.images.size(),
            plan.requestedGeneratedFrames.size()
        );
    const bool nativeFirstOrderedBatch = headroomTightOrderedBatch;
    if (nativeFirstOrderedBatch) {
        this->preacquireGeneratedImages(
            invocation, plan, true, 0, true
        );
    }
    size_t scheduledGeneratedFrameCount = plan.generatedImagesPreacquired
        ? plan.admittedGeneratedFrameCount
        : plan.requestedGeneratedFrames.size();
    plan.scheduledGeneratedFrames = scheduleAdmittedGeneratedFrames(
        plan.requestedGeneratedFrames, scheduledGeneratedFrameCount
    );
    bool bypassGeneratedFrames = plan.historyWarmupActive ||
        plan.scheduledGeneratedFrames.empty();

    if (orderedAcquireProbeEligible(orderedAcquireRecoveryProbe,
            plan.historyWarmupActive, plan.requestedGeneratedFrames.size())) {
        // After a genuine native drain, one single-image probe uses the
        // configured acquire contract or one confirmed display period. Failure
        // is terminal for this attempt and returns to backoff instead of
        // leaving recovery permanently probe-pending.
        const uint64_t recoveryAcquireTimeout = boundedOrderedAcquireProbe
            ? orderedRecoveryAcquireTimeout(
                std::nullopt, plan.configuredAcquireTimeout
            )
            : 0;
        this->preacquireGeneratedImages(
            invocation, plan, false, recoveryAcquireTimeout
        );
        if (plan.admittedGeneratedFrameCount == 0) {
            const auto probeFinishedAt = DiagnosticsClock::now();
            const auto miss = this->recoveryState.orderedAcquireRecovery
                .reportNonblockingProbeUnavailable(probeFinishedAt);
            if (miss.quarantined)
                this->fixedRefreshBudget.reset();
            return this->presentNativeFrame(invocation);
        }
        scheduledGeneratedFrameCount = plan.admittedGeneratedFrameCount;
        plan.scheduledGeneratedFrames = scheduleAdmittedGeneratedFrames(
            plan.requestedGeneratedFrames, scheduledGeneratedFrameCount
        );
        bypassGeneratedFrames = false;
    }

    if (!bypassGeneratedFrames) {
        const auto scheduleStarted = startPhaseTimer();
        try {
            this->instance->scheduleFrames(
                this->ctx.get(), plan.scheduledGeneratedFrames.timestamps()
            );
            // The source copy has not advanced the timeline yet, so S is the
            // value it will signal. The backend's last generated frame signals
            // S + G, after which the batch no longer reads the source image.
            this->frameState.nextSourceReleaseTimelineValue =
                this->frameState.backendTimelineIndex +
                plan.scheduledGeneratedFrames.size();
        } catch (const std::exception& error) {
            std::cerr << "framegen-vulkan: temporarily bypassing frame generation after "
                         "backend scheduling failure; native presentation retained: "
                      << error.what() << '\n';
            this->recoveryState.backendPending = true;
            this->recoveryState.historyWarmupRemaining = 0;
            if (!preacquiredImagesRequireRetirement(
                    plan.generatedImagesPreacquired,
                    plan.admittedGeneratedFrameCount)) {
                return this->presentNativeFrame(invocation);
            }
            if (invocation.originalPresentDeadline)
                invocation.waitForOutput(*invocation.originalPresentDeadline);
            return this->retireAcquiredImagesAndPresent(
                vk, queue, swapchain, lowerNextChain,
                imageIndex, waitSemaphores,
                std::span<const uint32_t>(
                    plan.preacquiredGeneratedImages.data(),
                    plan.admittedGeneratedFrameCount
                ),
                swapchainImage
            );
        }
        if (this->pacingProfile) this->pacingProfile->backendScheduled(this->frameState.backendFrameIndex);
        this->frameState.backendFrameIndex++;
        plan.scheduleFramesDuration = finishPhaseTimer(
            scheduleStarted
        );
    }

    const auto sourceCopyStarted = startPhaseTimer();
    this->submitSourceCopy(invocation, swapchainImage, sourceImage);
    plan.submitSourceCopyDuration = finishPhaseTimer(
        sourceCopyStarted
    );
    if (sourceDeadlinePacing && !plan.historyWarmupActive &&
            plan.scheduledGeneratedFrames.size() ==
                this->configuredFixedGeneratedFrames &&
            plan.scheduledGeneratedFrames.size() ==
                plan.requestedGeneratedFrames.size()) {
        // Prepare inside the cap interval in native FIFO too. Sleeping before
        // preparation leaves no cap slack and sends generated/real images as
        // one burst.
        invocation.pacedOutputs = this->realFramePacer.scheduleOutputs(
            DiagnosticsClock::now(), limiterDeadline,
            baseFpsCap * this->profile.multiplier,
            plan.scheduledGeneratedFrames.size() + 1,
            false,
            invocation.deferredOutputEligible);
        if (invocation.pacedOutputs) {
            invocation.capReadyOutputPacing = invocation.deferredOutputEligible;
            // The worker can overlap a full source period with next-frame
            // production. The shorter remaining return-cap slack is not its
            // readiness budget: using it discards spacing after CPU jitter.
            invocation.readinessDeadline = limiterArrival +
                std::chrono::duration_cast<DiagnosticsClock::duration>(
                    std::chrono::duration<double>(1.0 / baseFpsCap));
            invocation.originalPresentDeadline = invocation.pacedOutputs->at(
                invocation.pacedOutputs->count - 1);
        }
    } else if (!readyOutputPacing && !releaseGated) {
        this->realFramePacer.resetOutputs();
    }
    if (releaseGated && invocation.deferredOutputEligible && !invocation.pacedOutputs &&
            !bypassGeneratedFrames && !plan.historyWarmupActive &&
            plan.scheduledGeneratedFrames.size() == this->configuredFixedGeneratedFrames &&
            plan.scheduledGeneratedFrames.size() == plan.requestedGeneratedFrames.size()) {
        // The worker spaces the batch from readiness on the cap's output
        // interval, or on the measured source period when a GPU-bound source
        // is slower, while the game continues; the release wait owns the cap.
        const auto count = plan.scheduledGeneratedFrames.size() + 1;
        invocation.pacedOutputs = this->realFramePacer.scheduleReleasedOutputs(
            DiagnosticsClock::now(), capInterval, count, latency.justInTime);
        if (invocation.pacedOutputs) {
            const auto batchPeriod = invocation.pacedOutputs->interval * static_cast<int64_t>(count);
            invocation.readyOutputPacing = true;
            // Two older batches may keep spacing while the next two sources are
            // admitted, so a GPU frame ending up to about two batch periods
            // after Present keeps even spacing. Late readiness otherwise shifts
            // the real output past the next source present, whose start-of-
            // present join then sleeps through output spacing on VKD3D's D3D12
            // queue thread. The join at the third source after is still bounded:
            // at most a quarter output slot of spacing is kept.
            invocation.readinessDeadline = limiterArrival + 3 * batchPeriod;
            invocation.releasedOverlap = true;
            // With low_latency the worker starts this batch at its own readiness (see
            // RealFramePacer::scheduleReleasedOutputs and the worker's justInTime rule).
            // Reflex does not change this.
            invocation.justInTime = latency.justInTime;
            invocation.releasedJoinTolerance = invocation.pacedOutputs->interval / 4;
            invocation.releasedOutputLimit = limiterArrival + 3 * batchPeriod +
                invocation.pacedOutputs->interval / 8;
            invocation.originalPresentDeadline = invocation.pacedOutputs->at(count - 1);
        }
    }
    if (readyOutputPacing && !invocation.pacedOutputs && !bypassGeneratedFrames &&
            !plan.historyWarmupActive &&
            plan.scheduledGeneratedFrames.size() == this->configuredFixedGeneratedFrames &&
            plan.scheduledGeneratedFrames.size() == plan.requestedGeneratedFrames.size()) {
        const auto waiting = DiagnosticsClock::now();
        const auto budget = this->realFramePacer.sourceReadinessBudget(
            waiting, plan.scheduledGeneratedFrames.size() + 1, invocation.deferredOutputEligible);
        if (budget > DiagnosticsClock::duration::zero()) {
            // A retained batch waits on the worker. Waiting here would add
            // GPU time after the game's limiter, then teach our estimator
            // that the combined delay was the game's new source cadence.
            const bool ready = invocation.deferredOutputEligible || this->syncSemaphore->wait(vk, this->frameState.backendTimelineIndex,
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(budget).count()));
            const auto waited = DiagnosticsClock::now() - waiting;
            invocation.readinessWait += waited;
            invocation.outputWait += waited;
            if (ready) {
                invocation.pacedOutputs = this->realFramePacer.scheduleReadyOutputs(
                    DiagnosticsClock::now(), plan.scheduledGeneratedFrames.size() + 1,
                    invocation.deferredOutputEligible);
                if (invocation.pacedOutputs) {
                    invocation.readyOutputPacing = true;
                    invocation.readinessDeadline = waiting + budget;
                    invocation.originalPresentDeadline = invocation.pacedOutputs->at(
                        invocation.pacedOutputs->count - 1);
                }
            }
        }
    }
    if (bypassGeneratedFrames)
        return this->presentHistoryOnly(invocation, plan);
    this->bridgeOutputBatchSize = plan.scheduledGeneratedFrames.size() + 1;
    return this->presentGeneratedFrames(
        invocation, plan
    );
}

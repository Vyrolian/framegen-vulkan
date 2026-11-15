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
    // The old source-cap sleep preceded present diagnostics. Moving that
    // wait between outputs must not report intentional limiting as slow GPU
    // work or emit a slow-present record on every capped frame.
    this->started += finishPresentDiagnostic(waitStarted);
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
        // Samples were collected after the backend's existing fence wait; reading them adds no GPU wait.
        for (const auto& split : this->instance->takeCompletedSplitProfiles(this->ctx.get())) {
            const auto source = this->pacingProfile->sourceForBackend(split.backendFrameIndex);
            const auto render = this->findRenderSplitHalf(source);
            present_diagnostics::logPacingGpuSplitFrame(this->diagnosticsState.contextId,
                source, split.backendFrameIndex,
                render.gameDoneNs, render.sourceCopyEndNs,
                split.fgStartNs, split.prepassEndNs, split.generatedEndNs, split.batchEndNs);
        }
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
    if (presentDiagnosticsEnabled()) {
        const auto requested = present_diagnostics::applicationPresentMode(
            this->info.incomingPresentMode, nextChain);
        if (this->diagnosticsState.applicationPresentMode != requested) {
            this->diagnosticsState.applicationPresentMode = requested;
            // Publish a complete record so the background health line cannot
            // split this startup/live-mode observation between flushed fields.
            try {
                std::ostringstream line;
                line << "framegen-vulkan: present diagnostics: operation=application-present-mode"
                      << " context=" << this->diagnosticsState.contextId
                      << " swapchain=" << swapchain
                      << " requested_present_mode=" << requested.mode
                      << " dynamic_override=" << requested.dynamic
                      << " effective_present_mode=" << static_cast<int64_t>(this->info.presentMode)
                      << " ordered_transport=" << this->privateOrderedTransport
                      << " action=diagnostic-only\n";
                std::cerr << line.str();
            } catch (...) {}
        }
    }
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
        !(this->deferredPresent && this->deferredPresent->serialWaitObserved()) &&
        detachedOutputModeAdmits(directUnthrottled, this->currentPresentMode) &&
        DeferredPresent::supportsChain(lowerNextChain) &&
        this->info.deferredPresentQueue && queue == vk.queue();
    double baseFpsCap = effectiveBaseFpsCap(this->profile);
    // Refresh bounds the automatic cap only. Query on each of the first 64
    // presents until a rate is known, then once per 240 presents, from the
    // owner that already has it.
    const bool query = this->refreshQuerySource < 64
        ? this->applicationRefreshHz <= 0 : this->refreshQuerySource % 240 == 0;
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
        !this->profile.adaptive_stable_cadence &&
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
        this->profile.adaptive_stable_cadence, baseFpsCap,
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
    bool readyOutputPacing = fixedReadyOutputPacingEligible(
        this->profile, directUnthrottled || nativeFifoOutputPacing,
        compositorOutputTiming || applicationTimingOwner,
        this->recoveryState.orderedAcquireRecovery.active(),
        detachedOutputTransport) &&
        !(this->deferredPresent && this->deferredPresent->serialWaitObserved());
    const auto completionWait = this->deferredPresent
        ? this->deferredPresent->takeCompletionPacingWait() : DeferredPresent::CompletionWait{};
    const auto completionDelay = completionWait.finished <= limiterArrival
        ? completionWait.waited : DiagnosticsClock::duration{};
    const bool completionHoldsCap = completionDelay > DiagnosticsClock::duration{} &&
        baseFpsCap > 0 && completionWait.capOwned &&
        std::chrono::duration<double>(limiterArrival - completionWait.finished).count() <= 2.0 / baseFpsCap;
    this->realFramePacer.recordCompletionPacingWait(completionDelay);
    this->realFramePacer.observeSource(limiterArrival, readyOutputPacing || releaseGated,
        this->configuredFixedGeneratedFrames + 1, releaseGated);
    if (this->deferredPresent &&
            (completionHoldsCap || this->realFramePacer.completionPacingBlocksSource())) {
        // VKD3D can signal a frame-latency event from a separate wait thread.
        // Two slower sources with no acquisition overlapping that wait show
        // that our retained output is gating production through that event.
        // An established framegen-vulkan cap already supplies the output interval; a
        // completion-gated producer cannot overlap that worker, including
        // with Smooth Cadence off. Keep its synchronous cap-owned delivery.
        this->deferredPresent->completionPacingBlocksSource();
        readyOutputPacing = false;
    }
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
        ~ReleaseArm() {
            if (!deadline) return;
            try {
                if (presentId) {
                    noteFirstPresentIdReleaseToken();
                    gate.recordPresentId(swapchain, *presentId, *deadline);
                } else {
                    gate.arm(queue, *deadline);
                }
            } catch (...) {}
        }
    } releaseArm{this->releaseGate, queue, swapchain,
        baseFpsCap > 0 ? std::optional{limiterDeadline}
            : detachedOutputTransport ? std::optional{limiterArrival}
            : std::nullopt,
        applicationPresentId};
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
        .started = startPresentDiagnostic(),
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
    this->recordPresentCadence(cadenceNow);

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
    if (replacementStabilization.bypassBackend) {
        if (replacementStabilization.diagnostic &&
                presentDiagnosticsEnabled()) {
            std::cerr << "framegen-vulkan: present diagnostics: "
                         "operation=replacement-backend-stabilization"
                      << " context=" << this->diagnosticsState.contextId
                      << " duration_ms="
                      << std::chrono::duration<double, std::milli>(
                             ReplacementBackendStabilization::duration
                         ).count()
                      << " action=scaled-real-frame-only\n";
        }
        return this->presentNativeFrame(invocation);
    }
    if (replacementStabilization.resumed) {
        this->ensureHistoryWarmup();
        if (presentDiagnosticsEnabled()) {
            std::cerr << "framegen-vulkan: present diagnostics: "
                         "operation=replacement-backend-stabilization-complete"
                      << " context=" << this->diagnosticsState.contextId
                      << " history_warmup_frames="
                      << historyWarmupFrameCount
                      << " action=resume-private-backend\n";
        }
    }
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
            this->diagnosticsState.fixedSkippedFrames +=
                this->configuredFixedGeneratedFrames;
            return this->presentNativeFrame(invocation);
        }
        orderedAcquireRecoveryProbe = recovery.limitGeneratedFrames;
        boundedOrderedAcquireProbe = recovery.boundedAcquireProbe;
        if (recovery.beginHistoryWarmup) {
            this->ensureHistoryWarmup();
            this->fixedRefreshBudget.reset();
            if (presentDiagnosticsEnabled()) {
                std::cerr << "framegen-vulkan: present diagnostics: "
                             "operation=ordered-acquire-retry"
                          << " context=" << this->diagnosticsState.contextId
                          << " phase=history-warmup"
                          << " consecutive_failures="
                          << recovery.consecutiveFailures
                          << " bypassed_frames="
                          << recovery.bypassedFrames
                          << " drain_ms="
                          << std::chrono::duration<double, std::milli>(
                                 recovery.drainDuration
                             ).count()
                          << " history_warmup_frames="
                          << historyWarmupFrameCount
                          << " requested_generated=0"
                          << " admitted_generated=0"
                          << " presented_generated=0"
                          << " frame=" << this->frameState.realFrameIndex
                          << " sequence=" << this->frameState.sequenceIndex
                          << " action=warm-history-before-one-frame-probe\n";
            }
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

    if (plan.historyWarmupActive)
        this->diagnosticsState.fixedSkippedFrames +=
            plan.requestedGeneratedFrames.size();

    this->frameState.nextSourceReleaseTimelineValue = 0;
    // Resolve prior work before acquiring any generated images or reusing
    // their acquire semaphores. A fence-budget miss must leave no new image
    // held by this plan when it falls back to native presentation.
    const auto renderFenceStarted = startPresentDiagnostic();
    const bool renderFenceReady = this->prepareRenderFence(vk);
    plan.renderFenceWaitDuration = finishPresentDiagnostic(
        renderFenceStarted
    );
    if (!renderFenceReady) {
        this->handleRenderFenceBudgetMiss(plan);
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
    if (nativeFirstOrderedBatch &&
            !this->diagnosticsState.orderedGeneratedAdmissionPolicyLogged &&
            presentDiagnosticsEnabled()) {
        this->diagnosticsState.orderedGeneratedAdmissionPolicyLogged = true;
        std::cerr << "framegen-vulkan: present diagnostics: "
                     "operation=ordered-generated-admission-policy"
                  << " context=" << this->diagnosticsState.contextId
                  << " requested_min_images="
                  << this->info.requestedMinImageCount
                  << " images=" << this->info.images.size()
                  << " planned=" << plan.requestedGeneratedFrames.size()
                  << " acquire_timeout_ns=0"
                  << " action=native-first\n";
    }
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
            if (miss.quarantined) {
                this->fixedRefreshBudget.reset();
                if (presentDiagnosticsEnabled()) {
                    std::cerr << "framegen-vulkan: present diagnostics: "
                                 "operation=ordered-acquire-quarantine"
                              << " context="
                              << this->diagnosticsState.contextId
                              << " phase=native-drain"
                              << " reason=bounded-recovery-probe-timeout"
                              << " acquire_timeout_ns="
                              << recoveryAcquireTimeout
                              << " acquire_ms="
                              << std::chrono::duration<double, std::milli>(
                                     plan.preacquireDuration
                                 ).count()
                              << " consecutive_failures="
                              << miss.consecutiveFailures
                              << " retry_ms="
                              << std::chrono::duration<double, std::milli>(
                                     miss.retryDelay
                                 ).count()
                              << " bypassed_frames="
                              << miss.bypassedFrames
                              << " recovery_ms="
                              << std::chrono::duration<double, std::milli>(
                                     miss.recoveryDuration
                                 ).count()
                              << " requested_generated="
                              << plan.requestedGeneratedFrames.size()
                              << " admitted_generated=0"
                              << " presented_generated=0"
                              << " frame="
                              << this->frameState.realFrameIndex
                              << " sequence="
                              << this->frameState.sequenceIndex
                              << " action=native-drain\n";
                }
            }
            return this->presentNativeFrame(invocation);
        }
        scheduledGeneratedFrameCount = plan.admittedGeneratedFrameCount;
        plan.scheduledGeneratedFrames = scheduleAdmittedGeneratedFrames(
            plan.requestedGeneratedFrames, scheduledGeneratedFrameCount
        );
        bypassGeneratedFrames = false;
    }

    if (!bypassGeneratedFrames) {
        const auto scheduleStarted = startPresentDiagnostic();
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
            this->diagnosticsState.fixedSkippedFrames +=
                plan.admittedGeneratedFrameCount;
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
        plan.scheduleFramesDuration = finishPresentDiagnostic(
            scheduleStarted
        );
        logSlowPresentOperation(
            "schedule-frames", this->frameState.realFrameIndex,
            this->frameState.sequenceIndex, scheduleStarted
        );
    }

    const auto sourceCopyStarted = startPresentDiagnostic();
    this->submitSourceCopy(invocation, swapchainImage, sourceImage);
    plan.submitSourceCopyDuration = finishPresentDiagnostic(
        sourceCopyStarted
    );
    if (sourceDeadlinePacing && !plan.historyWarmupActive &&
            plan.scheduledGeneratedFrames.size() ==
                this->configuredFixedGeneratedFrames &&
            plan.scheduledGeneratedFrames.size() ==
                plan.requestedGeneratedFrames.size()) {
        // Prepare inside the cap interval in native FIFO too, independent of
        // Smooth Cadence. Sleeping before preparation leaves no cap slack
        // and sends generated/real images as one burst.
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
            DiagnosticsClock::now(), capInterval, count);
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

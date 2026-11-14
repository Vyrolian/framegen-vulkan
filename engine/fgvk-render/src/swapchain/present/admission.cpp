/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain/swapchain.hpp"
#include "frame_content_capture.hpp"
#include "swapchain/present/internal.hpp"
#include "swapchain/render_slot_ring.hpp"
#include "swapchain/retirement.hpp"
#include "adaptive_scheduler.hpp"
#include "fgvk-common/helpers/errors.hpp"
#include "fgvk-common/vulkan/command_buffer.hpp"
#include "fgvk-common/vulkan/image.hpp"
#include "fgvk-common/vulkan/semaphore.hpp"
#include "fgvk-common/vulkan/vulkan.hpp"
#include "present_diagnostics.hpp"
#include "pnext_chain.hpp"

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

namespace {
    constexpr auto renderFenceWaitBudget = std::chrono::milliseconds(150);
    constexpr uint64_t renderFenceWaitBudgetNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            renderFenceWaitBudget
        ).count()
    );

    /// Presents each render slot's in-flight flag as an indexed flag range for
    /// the shared drain.
    template <class Slot>
    class InFlightFlags {
    public:
        explicit InFlightFlags(std::vector<Slot>& renderSlots) : slots(renderSlots) {}
        [[nodiscard]] size_t size() const noexcept { return this->slots.size(); }
        [[nodiscard]] bool& operator[](const size_t index) {
            return this->slots.at(index).inFlight;
        }
    private:
        std::vector<Slot>& slots;
    };
}

bool Swapchain::recoverBackendIfReady(const vk::Vulkan& vk) {
    if (!this->recoveryState.backendPending)
        return true;

    bool backendReady = false;
    try {
        backendReady = this->instance &&
            this->instance->contextReady(this->ctx.get());
    } catch (const std::exception& error) {
        std::cerr << "framegen-vulkan: backend recovery poll failed; native "
                     "presentation retained: " << error.what() << '\n';
    }
    if (!backendReady)
        return false;

    // A fence-budget miss leaves the previous application-device submission
    // in flight. Backend readiness alone does not make that fence reusable,
    // so retain nonblocking native presentation until both sides are idle.
    if (this->renderSlotsInFlight()) {
        try {
            if (!this->pollRenderSlotsIdle(vk))
                return false;
            if (this->nativeCopyProfile) this->nativeCopyProfile->completed();
        } catch (const std::exception& error) {
            std::cerr << "framegen-vulkan: render fence recovery poll failed; "
                         "native presentation retained: "
                      << error.what() << '\n';
            return false;
        }
    }

    this->recoveryState.backendPending = false;
    this->recoveryState.generatedImageAdmission.reset();
    if (!this->resetGenerationScheduler(
            DiagnosticsClock::now(), "backend-recovery")) {
        this->recoveryState.historyWarmupRemaining =
            AdaptiveScheduler::historyWarmupFrameCount();
    }

    std::cerr << "framegen-vulkan: backend work recovered; warming temporal "
                 "history before resuming frame generation\n";
    return true;
}

void Swapchain::ensureHistoryWarmup(const bool restart) {
    const size_t warmupFrames = AdaptiveScheduler::historyWarmupFrameCount();
    if (this->adaptiveScheduler) {
        if (restart)
            this->adaptiveScheduler->beginHistoryWarmup(warmupFrames, true);
        else
            this->adaptiveScheduler->ensureHistoryWarmup(warmupFrames, true);
    } else {
        this->recoveryState.historyWarmupRemaining = historyWarmupFramesAfterRequest(
            this->recoveryState.historyWarmupRemaining, warmupFrames, restart);
    }
}

Swapchain::PresentationFramePlan Swapchain::prepareFramePlan(
        const DiagnosticsClock::time_point presentNow,
        const bool orderedAcquireRecoveryProbe) {
    PresentationFramePlan plan;
    plan.orderedAcquireRecoveryProbe = orderedAcquireRecoveryProbe;
    plan.historyWarmupActive =
        this->recoveryState.historyWarmupRemaining > 0 ||
        (this->adaptiveScheduler &&
            this->adaptiveScheduler->historyWarmupActive());
    const bool schedulerEnabled = this->adaptiveScheduler.has_value();
    const auto adaptivePlan = schedulerEnabled &&
            !plan.historyWarmupActive
        ? this->adaptiveScheduler->planFrame(
            presentNow, orderedAcquireRecoveryProbe,
            std::nullopt,
            this->profile.adaptive && !this->smoothCadencePacerHandoff.active()
                ? this->smoothCadenceBaseCap.activeGenerationLimit()
                : std::nullopt,
            this->orderedPresentationDeadline
        )
        : AdaptiveFramePlan{};
    const size_t fixedGeneratedFrameCount = schedulerEnabled
        ? 0
        : this->fixedRefreshBudget.plan(
            presentNow, std::nullopt,
            this->configuredFixedGeneratedFrames,
            false
        );
    // Fixed is a user-selected workload. Only explicit menu/lifecycle
    // transitions and direct transport failures may interrupt it; ordinary
    // gameplay cadence is never used to infer a recovery episode.
    if (!schedulerEnabled &&
            fixedGeneratedFrameCount <
                this->configuredFixedGeneratedFrames) {
        this->diagnosticsState.fixedSkippedFrames +=
            this->configuredFixedGeneratedFrames -
                fixedGeneratedFrameCount;
    }
    plan.requestedGeneratedFrames = schedulerEnabled
        ? adaptivePlan
        : GeneratedFramePlan::evenlySpaced(
            fixedGeneratedFrameCount
        );
    if (orderedAcquireProbeEligible(orderedAcquireRecoveryProbe,
            plan.historyWarmupActive, plan.requestedGeneratedFrames.size())) {
        // A successful native drain proves only that one image can traverse
        // the ordered FIFO again. Do not turn that narrow observation into a
        // full normal plan before the recovery state has seen it complete.
        plan.requestedGeneratedFrames = GeneratedFramePlan::evenlySpaced(1);
    }
    plan.admittedGeneratedFrameCount = plan.requestedGeneratedFrames.size();
    plan.configuredAcquireTimeout = generatedImageAcquireTimeoutNs();
    return plan;
}

void Swapchain::reportAdaptiveDelivery(
        const PresentationFramePlan& plan,
        const size_t acceptedForPresentation) {
    if (!this->adaptiveScheduler || plan.requestedGeneratedFrames.empty())
        return;
    // Ordered acquire recovery owns the cadence boundary until its one-frame
    // probe completes. Its synthetic delivery must not advance an unrelated
    // Adaptive ramp or stable-cadence evaluation.
    if (this->privateOrderedTransport &&
            (plan.orderedAcquireRecoveryProbe ||
             this->recoveryState.orderedAcquireRecovery.active())) {
        return;
    }
    this->adaptiveScheduler->reportGeneratedFrameDelivery({
        .requested = plan.requestedGeneratedFrames.size(),
        .acceptedForPresentation = acceptedForPresentation,
    });
}

bool Swapchain::renderSlotsInFlight() const {
    return std::any_of(this->renderSlots.begin(), this->renderSlots.end(),
        [](const RenderSlot& slot) { return slot.inFlight; });
}

bool Swapchain::pollRenderSlotsIdle(const vk::Vulkan& vk) {
    bool idle = true;
    for (auto& slot : this->renderSlots) {
        if (!slot.inFlight)
            continue;
        if (slot.fence.wait(vk, 0))
            slot.inFlight = false;
        else
            idle = false;
    }
    return idle;
}

void Swapchain::ensureSplitRenderClock(const vk::Vulkan& vk, const uint32_t validBits, const float periodNs) {
    if (this->splitRenderClock)
        return;
    // The application device may not have enabled calibration. Then this
    // clock stays uncalibrated and the split halves report zeros.
    const auto get = [&](const char* name) {
        return vk.fi().GetDeviceProcAddr(vk.dev(), name);
    };
    auto calibrate = this->info.copyTimestampCalibration
        ? reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(get("vkGetCalibratedTimestampsKHR")) : nullptr;
    if (this->info.copyTimestampCalibration && !calibrate)
        calibrate = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(get("vkGetCalibratedTimestampsEXT"));
    this->splitRenderClock.emplace(vk.dev(), calibrate, validBits, periodNs);
}

void Swapchain::refreshSplitRenderCalibration() {
    if (!this->splitRenderClock)
        return;
    // Refresh the calibration at most once per second; a failed call keeps the previous pair.
    const auto now = present_diagnostics::Clock::now();
    if (now - this->splitRenderCalibratedAt >= std::chrono::seconds(1)) {
        this->splitRenderCalibratedAt = now;
        static_cast<void>(this->splitRenderClock->recalibrate());
    }
}

void Swapchain::recordOutputGpuSplit(const vk::Vulkan& vk, const RenderSlot& slot) {
    bool available = false;
    int64_t copyStartNs{};
    int64_t copyEndNs{};
    try {
        if (slot.outputQueries) {
            this->ensureSplitRenderClock(vk, slot.outputQueries->timestampValidBits(),
                slot.outputQueries->timestampPeriodNanoseconds());
            this->refreshSplitRenderCalibration();
            // No WAIT flag: a skipped or unfinished copy returns no values, so this output is unavailable.
            if (const auto ticks = slot.outputQueries->tryTimestamps(2);
                    ticks && this->splitRenderClock && this->splitRenderClock->calibrated()) {
                copyStartNs = this->splitRenderClock->monotonicNanoseconds(ticks->at(0));
                copyEndNs = this->splitRenderClock->monotonicNanoseconds(ticks->at(1));
                available = copyStartNs != 0 && copyEndNs != 0;
            }
        }
    } catch (const std::exception&) {
        // A failed diagnostic read affects only this output's record.
    }
    present_diagnostics::logPacingOutputGpu(this->diagnosticsState.contextId, slot.outputSource,
        available, copyStartNs, copyEndNs);
}

void Swapchain::collectRenderSplitTimestamps(const vk::Vulkan& vk) {
    if (!present_diagnostics::pacingGpuSplitProfileEnabled())
        return;
    for (auto& slot : this->renderSlots) {
        // inFlight is cleared only after an observed signal, so a pending slot
        // that is not in flight has a copy whose completion the fence proves.
        if (slot.inFlight)
            continue;
        if (slot.splitPending && slot.splitQueries) {
            slot.splitPending = false;
            present_diagnostics::SplitRenderHalf half{.source = slot.splitSource};
            try {
                if (this->splitRenderClock) {
                    this->refreshSplitRenderCalibration();
                    // No WAIT flag: an incomplete query returns no values and this source keeps zeros.
                    if (const auto ticks = slot.splitQueries->tryTimestamps(2)) {
                        half.gameDoneNs = this->splitRenderClock->monotonicNanoseconds(ticks->at(0));
                        half.sourceCopyEndNs = this->splitRenderClock->monotonicNanoseconds(ticks->at(1));
                    }
                }
            } catch (const std::exception&) {
                // A failed diagnostic read affects only this source's render half.
            }
            this->splitRenderHalves.at(this->splitRenderHalfNext) = half;
            this->splitRenderHalfNext = (this->splitRenderHalfNext + 1) % this->splitRenderHalves.size();
        }
        // The output half does not depend on the render half or on the backend's split state.
        if (slot.outputPending) {
            slot.outputPending = false;
            this->recordOutputGpuSplit(vk, slot);
        }
    }
}

bool Swapchain::prepareRenderFence(const vk::Vulkan& vk) {
    const size_t next = nextRenderSlot(this->renderSlotIndex, renderSlotDepth);
    if (!this->frameContentCapture &&
            !this->nativeCopyProfile &&
            this->frameState.sourceReleaseTimelineValue != 0) {
        // Source-image reuse is ordered on the GPU by the timeline, so only the
        // batch three presents ago must retire before its slot is reused.
        // A worker copy or skip signals that slot's fence only when its submit
        // completes, after the slot's acquire semaphore and staged signal were
        // consumed; a copy has also read the slot's staging image by then.
        if (this->deferredPresent && this->deferredPresent->overlapRingFull()) {
            // With all three overlap slots pending, the batch three presents ago
            // is the oldest one and may still be waiting for this slot. Its
            // fence is not submitted until its output deadline, so join only
            // that batch rather than letting the fence wait sleep through it.
            static_cast<void>(this->completeDeferredPresent(UINT64_MAX, false, true));
        }
        auto& slot = this->renderSlots.at(next);
        if (!admitRenderSlot(slot.inFlight, renderFenceWaitBudgetNs,
                [&slot, &vk](const uint64_t budgetNs) {
                    return slot.fence.wait(vk, budgetNs);
                }))
            return false;
        this->collectRenderSplitTimestamps(vk);
        this->renderSlotIndex = next;
        slot.fence.reset(vk);
        return true;
    }

    // Other paths keep the CPU drain. Every in-flight slot shares one budget,
    // and a timeout leaves the ring index and fences unchanged.
    // A deferred generated copy submits its slot's fence on the worker, so every
    // admitted batch must be joined before any in-flight fence is waited.
    if (this->deferredPresent && this->deferredPresent->pending())
        static_cast<void>(this->completeDeferredPresent(UINT64_MAX));
    InFlightFlags flags{this->renderSlots};
    bool waitedForInFlight = false;
    if (!drainRenderSlots(flags, renderFenceWaitBudget,
            [this, &vk](const size_t index, const uint64_t remainingNs) {
                return this->renderSlots.at(index).fence.wait(vk, remainingNs);
            },
            [] { return DiagnosticsClock::now(); },
            waitedForInFlight))
        return false;
    if (waitedForInFlight) {
        if (this->frameContentCapture)
            this->frameContentCapture->completed(vk);
        if (this->nativeCopyProfile)
            this->nativeCopyProfile->completed();
    }
    this->collectRenderSplitTimestamps(vk);
    this->renderSlotIndex = next;
    this->renderSlots.at(next).fence.reset(vk);
    return true;
}

void Swapchain::handleRenderFenceBudgetMiss(
        const PresentationFramePlan& plan) {
    std::cerr << "framegen-vulkan: previous render work missed the "
              << renderFenceWaitBudget.count() << " ms "
                 "fence budget; bypassing frame generation for this present; "
                 "native presentation retained\n";
    if (this->adaptiveScheduler) {
        this->reportAdaptiveDelivery(plan, 0);
        this->adaptiveScheduler->cancelHistoryWarmup();
    } else if (!plan.historyWarmupActive) {
        // Fixed history warmup was already recorded as skipped when the plan
        // was built, so count only a delivery that was still intended.
        this->diagnosticsState.fixedSkippedFrames +=
            plan.requestedGeneratedFrames.size();
    }
    this->recoveryState.backendPending = true;
    this->recoveryState.historyWarmupRemaining = 0;
    if (presentDiagnosticsEnabled()) {
        std::cerr << "framegen-vulkan: present diagnostics: "
                     "operation=render-fence-budget-missed"
                  << " context=" << this->diagnosticsState.contextId
                  << " planned=" << plan.requestedGeneratedFrames.size()
                  << " action=native-present\n";
    }
}

void Swapchain::handleGeneratedImageAdmissionPressure(
        const PresentationFramePlan& plan,
        const size_t admittedGeneratedFrames,
        const bool logPressure,
        const bool retainPartialAdmissionCapacity,
        const bool classifyAdaptiveLoadFailure,
        const uint64_t acquireTimeoutNanoseconds,
        const char* const action) {
    this->recoveryState.generatedImageAdmission.reportBypassedFrame();
    const bool higherMultiplierEvaluationActive =
        this->adaptiveScheduler &&
        this->adaptiveScheduler->snapshot().rampEvaluationActive;
    const bool adaptiveOrderedAdmissionMiss =
        classifyAdaptiveLoadFailure && this->adaptiveScheduler &&
        adaptiveOrderedDeliveryMissRequiresFallback(
            this->profile.adaptive,
            this->privateOrderedTransport,
            plan.requestedGeneratedFrames.size(),
            admittedGeneratedFrames,
            higherMultiplierEvaluationActive
        );
    if (adaptiveOrderedAdmissionMiss) {
        static_cast<void>(
            this->adaptiveScheduler->rejectActiveRampForTransportMiss(
                DiagnosticsClock::now()
            )
        );
    }
    const auto constrainedAdaptiveOrderedWsiCapacity =
        retainPartialAdmissionCapacity
            ? adaptiveOrderedWsiLimitAfterPartialAdmission(
                this->profile.adaptive,
                this->privateOrderedTransport,
                adaptiveOrderedAdmissionMiss,
                plan.requestedGeneratedFrames.size(),
                admittedGeneratedFrames,
                this->adaptiveOrderedWsiGeneratedCapacityLimit
            )
            : std::nullopt;
    if (constrainedAdaptiveOrderedWsiCapacity) {
        const size_t previousGeneratedCapacity =
            this->adaptiveOrderedWsiGeneratedCapacityLimit.value_or(
                this->destinationImages.size()
            );
        this->adaptiveOrderedWsiGeneratedCapacityLimit =
            *constrainedAdaptiveOrderedWsiCapacity;
        if (presentDiagnosticsEnabled()) {
            std::cerr << "framegen-vulkan: present diagnostics: "
                         "operation=adaptive-wsi-headroom-limit"
                      << " context=" << this->diagnosticsState.contextId
                      << " requested_min_images="
                      << this->info.requestedMinImageCount
                      << " images=" << this->info.images.size()
                      << " requested_generated="
                      << plan.requestedGeneratedFrames.size()
                      << " admitted_generated="
                      << admittedGeneratedFrames
                      << " previous_generated_capacity="
                      << previousGeneratedCapacity
                      << " effective_generated_capacity="
                      << *this->adaptiveOrderedWsiGeneratedCapacityLimit
                      << " evidence=higher-multiplier-partial-admission"
                      << " action=retain-proven-generated-capacity\n";
        }
        this->recoveryState.generatedImageAdmission.reset();
        static_cast<void>(this->resetGenerationScheduler(
            DiagnosticsClock::now(), "ordered-wsi-partial-admission"
        ));
    }
    if (!this->adaptiveScheduler) {
        this->diagnosticsState.fixedSkippedFrames +=
            plan.requestedGeneratedFrames.size() -
                admittedGeneratedFrames;
    }
    if (logPressure && presentDiagnosticsEnabled()) {
        std::cerr << "framegen-vulkan: present diagnostics: "
                     "operation=generated-admission-pressure"
                  << " context=" << this->diagnosticsState.contextId
                  << " planned=" << plan.requestedGeneratedFrames.size()
                  << " admitted=" << admittedGeneratedFrames
                  << " acquire_timeout_ns=" << acquireTimeoutNanoseconds
                  << " action=" << action << '\n';
    }
}

void Swapchain::reportGeneratedImageAdmissionAvailable(
        const size_t requiredStableBatches) {
    const auto recovery =
        this->recoveryState.generatedImageAdmission.reportAvailable(
            requiredStableBatches
        );
    if (recovery.resumed && presentDiagnosticsEnabled()) {
        std::cerr << "framegen-vulkan: present diagnostics: "
                     "operation=generated-admission-recovered"
                  << " context=" << this->diagnosticsState.contextId
                  << " missed_attempts=" << recovery.missedAttempts
                  << " bypassed_frames=" << recovery.bypassedFrames
                  << " stable_batches=" << recovery.stableBatches
                  << " required_stable_batches="
                  << recovery.requiredStableBatches
                  << '\n';
    }
}

void Swapchain::preacquireGeneratedImages(
        const PresentInvocation& invocation,
        PresentationFramePlan& plan, const bool trackNonblockingAdmission,
        const bool classifyAdaptiveLoadFailure,
        const uint64_t acquireTimeout,
        const bool reportAvailableOnFullAdmission) {
    if (plan.requestedGeneratedFrames.empty() || plan.historyWarmupActive)
        return;

    plan.generatedImagesPreacquired = true;
    plan.admittedGeneratedFrameCount = 0;
    bool logPressure = false;
    VkResult lastAcquireResult = VK_SUCCESS;
    for (size_t i = 0; i < plan.requestedGeneratedFrames.size(); ++i) {
        uint32_t acquiredImage{};
        const auto acquireStarted = startPresentDiagnostic();
        lastAcquireResult = invocation.vk.df().AcquireNextImageKHR(
            invocation.vk.dev(), invocation.swapchain,
            acquireTimeout,
            this->currentRenderSlot().passes.at(i).acquireSemaphore.handle(),
            VK_NULL_HANDLE,
            &acquiredImage
        );
        plan.preacquireDuration += finishPresentDiagnostic(acquireStarted);
        if (lastAcquireResult == VK_SUCCESS ||
                lastAcquireResult == VK_SUBOPTIMAL_KHR) {
            plan.preacquiredGeneratedImages.at(
                plan.admittedGeneratedFrameCount++
            ) = acquiredImage;
            continue;
        }
        if (lastAcquireResult == VK_NOT_READY ||
                lastAcquireResult == VK_TIMEOUT) {
            logPressure = trackNonblockingAdmission &&
                this->recoveryState.generatedImageAdmission.reportUnavailable();
            if (logPressure) {
                logSlowPresentOperation(
                    "acquire-generated-image",
                    this->frameState.realFrameIndex,
                    this->frameState.sequenceIndex,
                    acquireStarted, lastAcquireResult, i, acquiredImage
                );
            }
            break;
        }
        throw ls::vulkan_error(
            lastAcquireResult, "vkAcquireNextImageKHR() failed"
        );
    }

    if (plan.admittedGeneratedFrameCount ==
            plan.requestedGeneratedFrames.size()) {
        if (trackNonblockingAdmission && reportAvailableOnFullAdmission)
            this->reportGeneratedImageAdmissionAvailable();
        return;
    }

    if (trackNonblockingAdmission) {
        this->handleGeneratedImageAdmissionPressure(
            plan, plan.admittedGeneratedFrameCount, logPressure, true,
            classifyAdaptiveLoadFailure, 0,
            classifyAdaptiveLoadFailure
                ? "native-first" : "adaptive-pressure-retry"
        );
    }
}

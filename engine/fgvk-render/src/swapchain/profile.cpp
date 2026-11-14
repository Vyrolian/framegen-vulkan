/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain/swapchain.hpp"
#include "swapchain/create_policy.hpp"
#include "swapchain/retirement.hpp"
#include "present_diagnostics.hpp"
#include "layer_role.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

#include <vulkan/vulkan_core.h>

using namespace fgvk;
using namespace fgvk::layer;

namespace {
    using DiagnosticsClock = present_diagnostics::Clock;
}

bool Swapchain::resetGenerationScheduler(
        const DiagnosticsClock::time_point now,
        const std::string_view reason) {
    const auto policy = generationSchedulerPolicy(
        this->profile
    );
    if (!policy || this->destinationImages.empty()) {
        this->adaptiveScheduler.reset();
        return false;
    }

    const size_t schedulerGeneratedFrameCapacity =
        this->profile.adaptive &&
            this->adaptiveOrderedWsiGeneratedCapacityLimit
        ? std::min(
            this->destinationImages.size(),
            *this->adaptiveOrderedWsiGeneratedCapacityLimit
        )
        : this->destinationImages.size();

    this->adaptiveScheduler.emplace(
        AdaptiveSchedulerConfig{
            .targetFps = policy->targetFps,
            .maximumMultiplier = policy->maximumMultiplier,
            .generatedFrameCapacity = schedulerGeneratedFrameCapacity,
            .stableCadence = policy->stableCadence,
            .automaticBaseFpsCap =
                this->profile.adaptive_auto_base_fps_cap,
            .nearTargetNativePreference =
                policy->nearTargetNativePreference,
            .qualifyTargetNativeCadence = policy->qualifyTargetNativeCadence,
            .validateIntegerPacing = this->profile.adaptive &&
                policy->stableCadence && adaptiveTargetMatchesRefresh(
                    policy->targetFps, std::nullopt) &&
                (this->profile.adaptive_auto_base_fps_cap ||
                 effectiveBaseFpsCap(this->profile)
                    <= 0.0),
            .dynamicCadenceRecovery = policy->dynamicCadenceRecovery,
            .dynamicCadenceProbeInterval =
                ls::dynamicCadenceProbeIntervalDuration(
                    policy->dynamicCadenceProbeIntervalSeconds
                ),
            .displayRefreshFps = std::nullopt,
            .recoveryPolicy = this->privateOrderedTransport
                ? AdaptiveRecoveryPolicy::OrderedSdr
                : AdaptiveRecoveryPolicy::Conservative,
        },
        &present_diagnostics::adaptiveScheduler()
    );
    this->adaptiveScheduler->beginStabilization(now, reason);
    this->recoveryState.historyWarmupRemaining = 0;
    return true;
}

ProfileUpdateDecision Swapchain::updateProfile(
        const ls::GameConf& nextProfile,
        const uint64_t runtimeStateRevision,
        const ls::GameConf* requestedProfile,
        const bool processRestartPending) {
    this->runtimeStatusState.requestedProfile = requestedProfile
        ? *requestedProfile : nextProfile;
    this->runtimeStatusState.stateRevision = runtimeStateRevision;
    this->runtimeStatusState.processRestartPending = processRestartPending;
    this->runtimeStatusState.error.reset();
    const bool resourcesAvailable = this->sourceImages.size() == 2 &&
        !this->destinationImages.empty() && this->syncSemaphore.has_value();
    const size_t requestedGeneratedFrameCapacity =
        generatedFrameCapacityForActivePolicy(nextProfile);
    const size_t existingSwapchainGeneratedFrameCapacity =
        generatedFrameCapacitySupportedByExistingSwapchain(
            this->info.requestedMinImageCount, this->info.images.size()
        );
    const bool generatedFrameCapacityGrowthFitsExistingSwapchain =
        requestedGeneratedFrameCapacity <= this->destinationImages.size() ||
        requestedGeneratedFrameCapacity <=
            existingSwapchainGeneratedFrameCapacity;
    const bool privateFrameGenerationRebuildAvailable =
        this->instance && resourcesAvailable &&
        this->colorPipeline.generationSupported &&
        generatedFrameCapacityGrowthFitsExistingSwapchain;
    auto plan = planProfileUpdate(
        this->profile, nextProfile, this->destinationImages.size(),
        resourcesAvailable,
        privateFrameGenerationRebuildAvailable,
        std::nullopt
    );
    auto decision = plan.decision;
    if (decision.frameGenerationPrivateRebuild) {
        const FrameGenerationResourceRequest request{
            .profile = nextProfile,
        };
        if (this->frameGenerationTransition.pendingRequest() &&
                !(this->frameGenerationTransition.value() == request)) {
            this->preparedFrameGenerationResources.reset();
        }
        this->frameGenerationTransition.request(
            request, runtimeStateRevision, std::chrono::milliseconds(500)
        );
        if (present_diagnostics::enabled()) {
            std::cerr << "framegen-vulkan: present diagnostics: "
                         "operation=runtime-transition-pending"
                      << " context=" << this->diagnosticsState.contextId
                      << " state_revision=" << runtimeStateRevision
                      << " reason=frame-generation-resources"
                      << " flow_scale_pending="
                      << (ls::effectiveFlowScale(this->profile) !=
                            ls::effectiveFlowScale(nextProfile))
                      << " lighter_model_pending="
                      << (ls::effectivePerformanceMode(this->profile) !=
                            ls::effectivePerformanceMode(nextProfile))
                      << " generated_capacity_pending="
                      << decision.generatedFrameCapacityExceeded
                      << " active_generated_capacity="
                      << this->destinationImages.size()
                      << " requested_generated_capacity="
                      << generatedFrameCapacityForProfile(nextProfile)
                      << " action=prepare-private-context\n";
        }
    } else if (ls::effectiveFlowScale(this->profile) ==
                ls::effectiveFlowScale(nextProfile) &&
            ls::effectivePerformanceMode(this->profile) ==
                ls::effectivePerformanceMode(nextProfile) &&
            requestedGeneratedFrameCapacity <=
                this->destinationImages.size()) {
        this->frameGenerationTransition.cancel();
        this->preparedFrameGenerationResources.reset();
    } else if (!generatedFrameCapacityGrowthFitsExistingSwapchain) {
        // A newer request that exceeds the WSI queue invalidates any older
        // debounced private candidate. The requested profile remains pending
        // at Root and will construct at the next game-owned recreation.
        this->frameGenerationTransition.cancel();
        this->preparedFrameGenerationResources.reset();
    }
    const bool liveRecreationAvailable =
        guardedLiveProfileResourceRecreationAvailable(
            decision,
            this->instance && resourcesAvailable &&
                this->colorPipeline.generationSupported,
            this->presentRetirementEnabled()
        );
    this->liveProfileResourceRecreation.update(
        this->profile,
        liveRecreationAvailable ? nextProfile : this->profile,
        runtimeStateRevision
    );
    decision.swapchainRecreationRequested = liveRecreationAvailable &&
        this->liveProfileResourceRecreation.armed();
    if (decision.swapchainRecreationRequested &&
            decision.action != ProfileUpdateAction::ApplyLive &&
            !decision.processRestartDeferred) {
        decision.action = ProfileUpdateAction::RequestSwapchainRecreation;
    }

    const auto logPendingRecreation = [&]() {
        if ((!decision.swapchainRecreationDeferred &&
             !decision.processRestartDeferred) ||
                !present_diagnostics::enabled()) {
            return;
        }
        std::cerr << "framegen-vulkan: present diagnostics: "
                     "operation=runtime-transition-pending"
                  << " context=" << this->diagnosticsState.contextId
                  << " role=" << layerRoleName
                  << " state_revision=" << runtimeStateRevision
                  << " reason=profile-resources"
                  << " frame_generation_backend_pending="
                  << decision.frameGenerationBackendChanged
                  << " flow_scale_pending="
                  << (ls::effectiveFlowScale(this->profile) !=
                        ls::effectiveFlowScale(nextProfile))
                  << " lighter_model_pending="
                  << (ls::effectivePerformanceMode(this->profile) !=
                        ls::effectivePerformanceMode(nextProfile))
                  << " generated_capacity_pending="
                  << decision.generatedFrameCapacityExceeded
                  << " available_generated_capacity="
                  << this->destinationImages.size()
                  << " available_wsi_generated_capacity="
                  << existingSwapchainGeneratedFrameCapacity
                  << " requested_generated_capacity="
                  << requestedGeneratedFrameCapacity
                  << " process_restart_required="
                  << decision.processRestartDeferred
                  << " action="
                  << (decision.swapchainRecreationRequested
                        ? "signal-out-of-date-after-retirement-fenced-present"
                        : decision.processRestartDeferred
                        ? "wait-for-process-restart"
                        : "wait-for-natural-swapchain-recreation")
                  << '\n';
    };
    logPendingRecreation();
    this->runtimeStatusState.swapchainRecreationPending =
        decision.swapchainRecreationDeferred;

    if (!this->colorPipeline.generationSupported || !this->instance ||
            !resourcesAvailable) {
        this->profile = std::move(plan.appliedProfile);
        this->publishRuntimeStatus("configuration-update");
        return decision;
    }

    if (decision.action == ProfileUpdateAction::NoRuntimeChange ||
            decision.action ==
                ProfileUpdateAction::DeferUntilSwapchainRecreation ||
            decision.action == ProfileUpdateAction::DeferUntilProcessRestart ||
            decision.action == ProfileUpdateAction::RequestSwapchainRecreation) {
        // Keep harmless metadata and dormant-policy values current even when
        // the active policy itself must wait for a documented boundary.
        this->profile = std::move(plan.appliedProfile);
        this->publishRuntimeStatus("configuration-update");
        return decision;
    }

    const bool hadGenerationScheduler = this->adaptiveScheduler.has_value();
    const bool generationWasEnabled = effectiveFrameGenerationEnabled(
        this->profile
    );
    const bool generationWillBeEnabled = effectiveFrameGenerationEnabled(
        plan.appliedProfile
    );
    const bool enabling = !generationWasEnabled && generationWillBeEnabled;
    const bool disabling = generationWasEnabled && !generationWillBeEnabled;
    const bool currentFractionalPriorityActive =
        fractionalRealFramePriorityActive(this->profile);
    const bool nextFractionalPriorityActive =
        fractionalRealFramePriorityActive(plan.appliedProfile);
    const bool fractionalPriorityStyleChanged =
        currentFractionalPriorityActive != nextFractionalPriorityActive ||
        (currentFractionalPriorityActive && nextFractionalPriorityActive &&
         this->profile.adaptive_fractional_real_frame_priority !=
            plan.appliedProfile.adaptive_fractional_real_frame_priority);
    const bool cadenceStyleChanged = generationWasEnabled &&
        generationWillBeEnabled && this->profile.adaptive &&
        plan.appliedProfile.adaptive &&
        (this->profile.adaptive_auto_base_fps_cap !=
            plan.appliedProfile.adaptive_auto_base_fps_cap ||
         fractionalPriorityStyleChanged);
    if (decision.gpuHeadroomChanged && plan.appliedProfile.gpu_headroom_percent == 0 &&
            this->gpuHeadroomResult.capFps > 0 && plan.appliedProfile.adaptive && this->adaptiveScheduler)
        this->adaptiveScheduler->requestHeadroomRecovery(DiagnosticsClock::now(), this->gpuHeadroom.sourceCeilingFps());
    if (decision.gpuHeadroomChanged || decision.generationModeChanged ||
            decision.baseFpsCapChanged || decision.generationPolicyChanged || enabling || disabling) {
        this->gpuHeadroom.reset();
        this->gpuHeadroomResult = {};
        this->lastGpuHeadroomDiagnostic.reset();
    }
    if ((decision.gpuHeadroomChanged || enabling) && !this->gpuLoadMonitor)
        this->gpuLoadMonitorInitialized = false;
    const auto profileUpdateNow = DiagnosticsClock::now();
    this->profile = std::move(plan.appliedProfile);
    this->configuredFixedGeneratedFrames = fixedGeneratedFrameCount(
        this->profile.multiplier, this->destinationImages.size()
    );
    if (decision.generationModeChanged || decision.fixedMultiplierChanged ||
            decision.baseFpsCapChanged || enabling || disabling) {
        this->fixedRefreshBudget.reset();
        this->diagnosticsState.fixedWindowStarted.reset();
        this->diagnosticsState.fixedRealFrames = 0;
        this->diagnosticsState.fixedGeneratedFrames = 0;
        this->diagnosticsState.fixedSkippedFrames = 0;
    }
    if (decision.gpuHeadroomChanged || decision.baseFpsCapChanged || decision.generationPolicyChanged ||
            decision.generationModeChanged || enabling || disabling) {
        this->realFramePacer.reset();
        this->smoothCadenceBaseCap.reset();
        this->smoothCadencePacerHandoff.reset();
    }

    if (profileUpdateInvalidatesTransientGenerationRecovery(decision)) {
        // A new Fixed/Adaptive policy must not inherit a retry or delivery-
        // pressure sample produced by the previous requested workload.
        this->recoveryState.generatedImageAdmission.reset();
        this->recoveryState.orderedAcquireRecovery.reset();
    }

    if (disabling) {
        this->recoveryState.historyWarmupRemaining = 0;
        this->recoveryState.orderedAcquireRecovery.reset();
        if (this->adaptiveScheduler)
            this->adaptiveScheduler->cancelHistoryWarmup();
    }

    if (enabling) {
        this->recoveryState.generatedImageAdmission.reset();
        this->recoveryState.orderedAcquireRecovery.reset();
        this->recoveryState.historyWarmupRemaining =
            generationSchedulerPolicy(this->profile)
            ? 0
            : AdaptiveScheduler::historyWarmupFrameCount();
    }

    const bool transportLoadTransition = this->privateOrderedTransport &&
        effectiveFrameGenerationEnabled(
            this->profile
        ) && (decision.generationPolicyChanged ||
              decision.generationModeChanged ||
              decision.fixedMultiplierChanged || cadenceStyleChanged ||
              enabling);
    if (transportLoadTransition) {
        this->recoveryState.orderedAcquireRecovery
            .armTransitionRecreation();
        if (present_diagnostics::enabled()) {
            std::cerr << "framegen-vulkan: present diagnostics: "
                         "operation=ordered-acquire-transition-recovery-armed"
                      << " context=" << this->diagnosticsState.contextId
                      << " evidence=explicit-profile-transition"
                      << " action=one-shot-on-acquire-failure\n";
        }
    }

    const bool schedulerPolicyAvailable = generationSchedulerPolicy(
        this->profile
    ).has_value();
    const bool fixedSchedulerPolicyChanged = !this->profile.adaptive &&
        decision.fixedMultiplierChanged;
    const bool resetSchedulerPolicy = schedulerPolicyAvailable &&
            (decision.generationPolicyChanged ||
             decision.generationModeChanged ||
             fixedSchedulerPolicyChanged ||
             decision.baseFpsCapChanged || enabling);
    if (resetSchedulerPolicy) {
        static_cast<void>(this->resetGenerationScheduler(
            profileUpdateNow, "configuration-update"
        ));
    } else if (hadGenerationScheduler && !schedulerPolicyAvailable) {
        this->adaptiveScheduler.reset();
        this->recoveryState.historyWarmupRemaining =
            AdaptiveScheduler::historyWarmupFrameCount();
    } else if (decision.dynamicCadenceProbeIntervalChanged &&
            this->adaptiveScheduler) {
        this->adaptiveScheduler->updateDynamicCadenceProbeInterval(
            profileUpdateNow,
            ls::dynamicCadenceProbeIntervalDuration(
                this->profile.dynamic_cadence_probe_interval_seconds
            )
        );
    }

    const auto updatedGenerationPolicy = generationSchedulerPolicy(
        this->profile
    );
    const bool updatedDynamicCadenceRecoveryActive =
        this->privateOrderedTransport && updatedGenerationPolicy &&
        updatedGenerationPolicy->dynamicCadenceRecovery;
    if (present_diagnostics::enabled()) {
        std::cerr << "framegen-vulkan: present diagnostics: operation=runtime-state-applied"
                  << " context=" << this->diagnosticsState.contextId
                  << " role=" << layerRoleName
                  << " state_revision=" << runtimeStateRevision
                  << " transition=live"
                  << " frame_generation_enabled="
                  << this->profile.frame_generation_enabled
                  << " effective_frame_generation_enabled="
                  << effectiveFrameGenerationEnabled(
                        this->profile
                     )
                  << " adaptive=" << this->profile.adaptive
                  << " target_fps=" << this->profile.target_fps
                  << " multiplier=" << this->profile.multiplier
                  << " gpu_headroom_percent=" << this->profile.gpu_headroom_percent
                  << " base_fps_cap=" << this->profile.base_fps_cap
                  << " adaptive_auto_base_fps_cap="
                  << this->profile.adaptive_auto_base_fps_cap
                  << " adaptive_fractional_real_frame_priority="
                  << ls::adaptiveFractionalRealFramePriorityName(
                      this->profile.adaptive_fractional_real_frame_priority
                  )
                  << " effective_base_fps_cap="
                  << effectiveBaseFpsCap(this->profile)
                  << " adaptive_max_multiplier="
                  << this->profile.adaptive_max_multiplier
                  << " stable_cadence="
                  << this->profile.adaptive_stable_cadence
                  << " dynamic_cadence_recovery="
                  << this->profile.dynamic_cadence_recovery
                  << " dynamic_cadence_probe_interval_seconds="
                  << this->profile.dynamic_cadence_probe_interval_seconds
                  << " effective_dynamic_cadence_recovery="
                  << updatedDynamicCadenceRecoveryActive
                  << " effective_flow_scale="
                  << ls::effectiveFlowScale(this->profile)
                  << " lighter_model="
                  << ls::effectivePerformanceMode(this->profile)
                  << " frame_generation_resources_available="
                  << (this->sourceImages.size() == 2 &&
                        !this->destinationImages.empty() &&
                        this->syncSemaphore.has_value())
                  << " generated_frame_capacity="
                  << this->destinationImages.size()
                  << " hdr=" << this->colorPipeline.hdr
                  << '\n';
    }

    this->publishRuntimeStatus("configuration-update");
    return decision;
}

bool Swapchain::requestLiveProfileResourceRecreationAfterPresent(
        const VkResult lowerPresentResult) {
    if (lowerPresentResult != VK_SUCCESS &&
            lowerPresentResult != VK_SUBOPTIMAL_KHR) {
        return false;
    }
    if (!this->lastLowerPresentRetirementProtected)
        return false;
    const auto runtimeStateRevision =
        this->liveProfileResourceRecreation.signalAfterSuccessfulPresent();
    if (!runtimeStateRevision)
        return false;

    std::cerr << "framegen-vulkan: live profile resource change requested a "
                 "game-owned swapchain recreation after one "
                 "maintenance1-fenced lower present\n";
    if (present_diagnostics::enabled()) {
        std::cerr << "framegen-vulkan: present diagnostics: "
                     "operation=runtime-transition-recreation-requested"
                  << " context=" << this->diagnosticsState.contextId
                  << " state_revision=" << *runtimeStateRevision
                  << " reason=profile-resources"
                  << " lower_present_result=" << lowerPresentResult
                  << " signal=VK_ERROR_OUT_OF_DATE_KHR"
                  << " delivery=one-shot-after-retirement-fence-attachment\n";
    }
    return true;
}

bool Swapchain::requestOrderedAcquireRecreationAfterPresent(
        const VkResult lowerPresentResult,
        const bool surfaceRequestAvailable) {
    if (!surfaceRequestAvailable ||
            !this->privateOrderedTransport ||
            !effectiveFrameGenerationEnabled(
                this->profile
            ) ||
            (lowerPresentResult != VK_SUCCESS &&
             lowerPresentResult != VK_SUBOPTIMAL_KHR) ||
            !this->lastLowerPresentRetirementProtected ||
            !this->recoveryState.orderedAcquireRecovery.signalRecreation()) {
        return false;
    }

    std::cerr << "framegen-vulkan: a generated-image acquire failed after an "
                 "explicit transition; requesting one game-owned swapchain "
                 "recreation\n";
    if (present_diagnostics::enabled()) {
        std::cerr << "framegen-vulkan: present diagnostics: "
                     "operation=ordered-acquire-recreation-requested"
                  << " context=" << this->diagnosticsState.contextId
                  << " reason=transition-scoped-generated-image-timeout"
                  << " lower_present_result=" << lowerPresentResult
                  << " signal=VK_ERROR_OUT_OF_DATE_KHR"
                  << " delivery=one-shot-per-context-after-retirement-fence-attachment\n";
    }
    return true;
}

void Swapchain::disableFrameGeneration() {
    if (!this->profile.frame_generation_enabled)
        return;

    this->profile.frame_generation_enabled = false;
    this->realFramePacer.reset();
    this->smoothCadenceBaseCap.reset();
    this->smoothCadencePacerHandoff.reset();
    this->recoveryState.historyWarmupRemaining = 0;
    this->recoveryState.orderedAcquireRecovery.reset();
    if (this->adaptiveScheduler)
        this->adaptiveScheduler->cancelHistoryWarmup();
    this->publishRuntimeStatus("profile-unmatched");
}

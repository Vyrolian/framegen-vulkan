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

ProfileUpdateDecision Swapchain::updateProfile(
        const ls::GameConf& incomingProfile,
        const uint64_t runtimeStateRevision) {
    ls::GameConf unavailableProfile;
    if (this->frameGenerationUnavailableReason) {
        unavailableProfile = incomingProfile;
        unavailableProfile.frame_generation_enabled = false;
        if (std::string_view(this->frameGenerationUnavailableReason) == "frame-generation-disabled" &&
                this->liveEnableRecreation.update(effectiveFrameGenerationEnabled(incomingProfile))) {
            std::cerr << "framegen-vulkan: frame generation was enabled live on a swapchain created "
                         "without it; requesting one game-owned swapchain recreation\n";
        }
    }
    const ls::GameConf& nextProfile = this->frameGenerationUnavailableReason
        ? unavailableProfile : incomingProfile;
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
        privateFrameGenerationRebuildAvailable
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

    if (!this->colorPipeline.generationSupported || !this->instance ||
            !resourcesAvailable) {
        this->profile = std::move(plan.appliedProfile);
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
        return decision;
    }

    const bool generationWasEnabled = effectiveFrameGenerationEnabled(
        this->profile
    );
    const bool generationWillBeEnabled = effectiveFrameGenerationEnabled(
        plan.appliedProfile
    );
    const bool enabling = !generationWasEnabled && generationWillBeEnabled;
    const bool disabling = generationWasEnabled && !generationWillBeEnabled;
    this->profile = std::move(plan.appliedProfile);
    this->configuredFixedGeneratedFrames = fixedGeneratedFrameCount(
        this->profile.multiplier, this->destinationImages.size()
    );
    if (decision.fixedMultiplierChanged ||
            decision.baseFpsCapChanged || enabling || disabling) {
        this->fixedRefreshBudget.reset();
    }
    if (decision.baseFpsCapChanged || enabling || disabling) {
        this->realFramePacer.reset();
    }

    if (profileUpdateInvalidatesTransientGenerationRecovery(decision)) {
        // A new Fixed policy must not inherit a retry or delivery-
        // pressure sample produced by the previous requested workload.
        this->recoveryState.generatedImageAdmission.reset();
        this->recoveryState.orderedAcquireRecovery.reset();
    }

    if (disabling) {
        this->recoveryState.historyWarmupRemaining = 0;
        this->recoveryState.orderedAcquireRecovery.reset();
    }

    if (enabling) {
        this->recoveryState.generatedImageAdmission.reset();
        this->recoveryState.orderedAcquireRecovery.reset();
        this->recoveryState.historyWarmupRemaining =
            historyWarmupFrameCount;
    }

    const bool transportLoadTransition = this->privateOrderedTransport &&
        effectiveFrameGenerationEnabled(
            this->profile
        ) && (decision.fixedMultiplierChanged || enabling);
    if (transportLoadTransition) {
        this->recoveryState.orderedAcquireRecovery
            .armTransitionRecreation();
    }

    return decision;
}

bool Swapchain::requestLiveProfileResourceRecreationAfterPresent(
        const VkResult lowerPresentResult) {
    if (lowerPresentResult != VK_SUCCESS &&
            lowerPresentResult != VK_SUBOPTIMAL_KHR) {
        return false;
    }
    if (this->liveEnableRecreation.armed()) {
        if (this->liveEnableRecreation.signalAfterSuccessfulPresent(
                this->lastLowerPresentRetirementProtected)) {
            std::cerr << "framegen-vulkan: requested a game-owned swapchain recreation so frame "
                         "generation can start with internal images\n";
            return true;
        }
        if (!this->lastLowerPresentRetirementProtected && !this->liveEnableRecreationWaitLogged) {
            this->liveEnableRecreationWaitLogged = true;
            std::cerr << "framegen-vulkan: no retirement-fenced present yet; frame generation starts "
                         "at the game's next swapchain recreation\n";
        }
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
    return true;
}

void Swapchain::setFrameGenerationUnavailable(const char* const reason) {
    this->frameGenerationUnavailableReason = reason;
    if (std::string_view(reason) != "frame-generation-disabled") {
        std::cerr << "framegen-vulkan: frame generation unavailable for this swapchain: reason="
                  << reason << '\n';
    }
    this->disableFrameGeneration();
}

void Swapchain::disableFrameGeneration() {
    if (!this->profile.frame_generation_enabled)
        return;

    this->profile.frame_generation_enabled = false;
    this->realFramePacer.reset();
    this->recoveryState.historyWarmupRemaining = 0;
    this->recoveryState.orderedAcquireRecovery.reset();
}

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain/swapchain.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

#include <vulkan/vulkan_core.h>

using namespace fgvk;
using namespace fgvk::layer;

void Swapchain::publishRuntimeStatus(const std::string_view reason) noexcept {
    const auto frameGenerationPhase = this->frameGenerationTransition.phase();
    const auto phaseIs = [&](const PrivateResourceTransitionPhase phase) {
        return frameGenerationPhase == phase;
    };

    RuntimeApplicationPhase phase = RuntimeApplicationPhase::Active;
    if (this->runtimeStatusState.error ||
            phaseIs(PrivateResourceTransitionPhase::Failed)) {
        phase = RuntimeApplicationPhase::Failed;
    } else if (this->runtimeStatusState.processRestartPending) {
        phase = RuntimeApplicationPhase::ProcessRestart;
    } else if (this->runtimeStatusState.swapchainRecreationPending) {
        phase = RuntimeApplicationPhase::SwapchainRecreation;
    } else if (phaseIs(PrivateResourceTransitionPhase::Draining)) {
        phase = RuntimeApplicationPhase::Draining;
    } else if (phaseIs(PrivateResourceTransitionPhase::Preparing)) {
        phase = RuntimeApplicationPhase::Preparing;
    } else if (phaseIs(PrivateResourceTransitionPhase::Debouncing)) {
        phase = RuntimeApplicationPhase::Debouncing;
    }

    this->runtimeStatusPublisher.publish(RuntimeStatusRecord{
        .phase = phase,
        .reason = std::string(reason),
        .stateRevision = this->runtimeStatusState.stateRevision,
        .requestedProfile = this->runtimeStatusState.requestedProfile,
        .appliedProfile = this->profile,
        .appliedGeneratedCapacity = this->destinationImages.size(),
        .frameGenerationActive = effectiveFrameGenerationEnabled(
            this->profile, this->gamescopeRefreshHz
        ) && !this->destinationImages.empty(),
        .gpuHeadroomState = this->gpuHeadroomResult.state,
        .frameGenerationPrivatePending =
            this->frameGenerationTransition.pendingRequest(),
        .swapchainRecreationPending =
            this->runtimeStatusState.swapchainRecreationPending,
        .processRestartPending =
            this->runtimeStatusState.processRestartPending,
        .error = this->runtimeStatusState.error,
    });
}

/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "fgvk-common/configuration/config.hpp"
#include "presentation_policy.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace fgvk::layer {

    enum class ProfileUpdateAction : uint8_t {
        NoRuntimeChange,
        ApplyLive,
        DeferUntilSwapchainRecreation,
        DeferUntilProcessRestart,
        RequestSwapchainRecreation,
    };

    struct ProfileUpdateDecision {
        ProfileUpdateAction action{ProfileUpdateAction::NoRuntimeChange};
        bool frameGenerationChanged{false};
        bool generationPolicyChanged{false};
        bool fixedMultiplierChanged{false};
        bool baseFpsCapChanged{false};
        bool frameGenerationBackendChanged{false};
        bool generatedFrameCapacityExceeded{false};
        bool frameGenerationPrivateRebuild{false};
        bool swapchainRecreationDeferred{false};
        bool processRestartDeferred{false};
        bool swapchainRecreationRequested{false};
    };

    struct ProfileUpdatePlan {
        ls::GameConf appliedProfile;
        ProfileUpdateDecision decision;
    };

    /// A live change which reshapes generated work starts a new transport
    /// policy episode. Incomplete evidence from the old Fixed plan cannot
    /// classify the new plan as unhealthy. Resource-retirement and
    /// per-context recreation one-shot state remain independently owned.
    [[nodiscard]] constexpr bool
    profileUpdateInvalidatesTransientGenerationRecovery(
            const ProfileUpdateDecision& decision) noexcept {
        return decision.generationPolicyChanged ||
            decision.fixedMultiplierChanged ||
            decision.baseFpsCapChanged;
    }

    struct ProcessStaticProfileProjection {
        ls::GameConf runtimeProfile;
        bool gpuSelectionPending{false};
        bool frameGenerationProvisioningPending{false};
        bool ultraPerformancePending{false};
        bool swapchainImageCountCompatibilityPending{false};

        [[nodiscard]] bool restartRequired() const {
            return this->gpuSelectionPending ||
                this->frameGenerationProvisioningPending ||
                this->ultraPerformancePending ||
                this->swapchainImageCountCompatibilityPending;
        }
    };

    /// Retain settings selected during process/device/backend construction
    /// while allowing unrelated fields from the requested profile to continue
    /// through the normal live-update path.
    [[nodiscard]] inline ProcessStaticProfileProjection
    projectProcessStaticProfileForLiveUpdate(
            const ls::GameConf& current,
            const ls::GameConf& requested,
            const bool frameGenerationProvisionedAtStartup,
            const bool swapchainImageCountCompatibilityConfiguredAtStartup) {
        ProcessStaticProfileProjection projection{
            .runtimeProfile = requested,
            .gpuSelectionPending = requested.gpu != current.gpu,
            .frameGenerationProvisioningPending =
                requested.frame_generation_provisioned !=
                    frameGenerationProvisionedAtStartup,
            .ultraPerformancePending = requested.ultra_performance !=
                current.ultra_performance,
            .swapchainImageCountCompatibilityPending =
                requested.swapchain_image_count_compatibility !=
                    swapchainImageCountCompatibilityConfiguredAtStartup,
        };
        if (projection.gpuSelectionPending)
            projection.runtimeProfile.gpu = current.gpu;
        if (projection.frameGenerationProvisioningPending) {
            projection.runtimeProfile.frame_generation_provisioned =
                frameGenerationProvisionedAtStartup;
        }
        if (projection.ultraPerformancePending) {
            projection.runtimeProfile.ultra_performance =
                current.ultra_performance;
            projection.runtimeProfile.flow_scale = current.flow_scale;
            projection.runtimeProfile.performance_mode =
                current.performance_mode;
        }
        if (projection.swapchainImageCountCompatibilityPending) {
            projection.runtimeProfile.swapchain_image_count_compatibility =
                swapchainImageCountCompatibilityConfiguredAtStartup;
        }
        return projection;
    }

    /// Private generated outputs are selected by the Fixed multiplier. A
    /// context with Frame Generation Off has no active generated-output
    /// requirement.
    [[nodiscard]] inline size_t generatedFrameCapacityForActivePolicy(
            const ls::GameConf& profile) {
        if (!profile.frame_generation_provisioned ||
                !profile.frame_generation_enabled)
            return 0;
        return profile.multiplier - 1;
    }

    struct RecreatedProfileResourceKey {
        float effectiveFlowScale{ls::GameConfDefaults::flowScale};
        bool effectivePerformanceMode{false};
        size_t requiredGeneratedFrameCapacity{0};

        friend bool operator==(
            const RecreatedProfileResourceKey&,
            const RecreatedProfileResourceKey&) = default;
    };

    [[nodiscard]] inline RecreatedProfileResourceKey recreatedProfileResourceKey(
            const ls::GameConf& profile) {
        return {
            .effectiveFlowScale = ls::effectiveFlowScale(profile),
            .effectivePerformanceMode = ls::effectivePerformanceMode(profile),
            .requiredGeneratedFrameCapacity =
                generatedFrameCapacityForActivePolicy(profile),
        };
    }

    /// Numeric resource controls retain a quiet period to coalesce slider
    /// movement and avoid recreation storms.
    [[nodiscard]] inline std::chrono::milliseconds
    recreatedProfileResourceQuietPeriod() noexcept {
        return std::chrono::milliseconds(500);
    }

    /// Frame-generation context construction cannot be mutated while a
    /// swapchain is active. Arm one spec-defined OUT_OF_DATE
    /// signal after the final lower present has accepted framegen-vulkan's maintenance1
    /// retirement fence. Repeated polls of the same requested resources do not
    /// signal again, while a distinct request receives one fresh signal.
    class LiveProfileResourceRecreation {
    public:
        using Clock = std::chrono::steady_clock;
        static constexpr auto quietPeriod = std::chrono::milliseconds(500);

        void update(const ls::GameConf& current,
                const ls::GameConf& requested,
                const uint64_t runtimeStateRevision,
                const Clock::time_point now = Clock::now()) {
            const auto currentKey = recreatedProfileResourceKey(current);
            const auto requestedKey = recreatedProfileResourceKey(requested);
            if (currentKey == requestedKey) {
                this->request.reset();
                return;
            }
            if (this->request && this->request->profile == requestedKey)
                return;
            this->request = Request{
                .profile = requestedKey,
                .runtimeStateRevision = runtimeStateRevision,
                .signalAfter = now + recreatedProfileResourceQuietPeriod(),
            };
        }

        [[nodiscard]] bool pending() const {
            return this->request.has_value();
        }

        [[nodiscard]] bool armed() const {
            return this->request && !this->request->signaled;
        }

        [[nodiscard]] std::optional<uint64_t> signalAfterSuccessfulPresent(
                const Clock::time_point now = Clock::now()) {
            if (!this->request || this->request->signaled ||
                    now < this->request->signalAfter)
                return std::nullopt;
            this->request->signaled = true;
            return this->request->runtimeStateRevision;
        }

    private:
        struct Request {
            RecreatedProfileResourceKey profile;
            uint64_t runtimeStateRevision{0};
            Clock::time_point signalAfter{};
            bool signaled{false};
        };
        std::optional<Request> request;
    };

    /// A swapchain created while frame generation was off has no internal
    /// application images and presents real frames only. When generation is
    /// switched on live, ask the game for one swapchain recreation (the same
    /// one-shot VK_ERROR_OUT_OF_DATE_KHR as LiveProfileResourceRecreation)
    /// so the replacement is created with internal images.
    class LiveEnableRecreation {
    public:
        using Clock = std::chrono::steady_clock;

        /// @param generationWanted the requested profile has generation provisioned and enabled
        /// @return true when this call armed the request
        bool update(const bool generationWanted, const Clock::time_point now = Clock::now()) {
            if (this->signaled)
                return false;
            if (!generationWanted) {
                this->signalAfter.reset();
                return false;
            }
            if (this->signalAfter)
                return false;
            this->signalAfter = now + recreatedProfileResourceQuietPeriod();
            return true;
        }

        [[nodiscard]] bool armed() const {
            return this->signalAfter.has_value() && !this->signaled;
        }

        /// Call after a successful lower present. Signals at most once per swapchain.
        [[nodiscard]] bool signalAfterSuccessfulPresent(const bool retirementProtected,
                const Clock::time_point now = Clock::now()) {
            if (!this->armed() || !retirementProtected || now < *this->signalAfter)
                return false;
            this->signaled = true;
            return true;
        }

    private:
        std::optional<Clock::time_point> signalAfter;
        bool signaled{false};
    };

    /// Flow Scale and Lighter FG Model require the process to have
    /// retained its frame-generation device interop and private context.
    [[nodiscard]] inline bool liveProfileResourceRecreationAvailable(
            const ProfileUpdateDecision& decision,
            const bool frameGenerationResourcesAvailable) {
        if (decision.processRestartDeferred)
            return false;
        return (decision.frameGenerationBackendChanged ||
              decision.generatedFrameCapacityExceeded) &&
             !decision.frameGenerationPrivateRebuild &&
             frameGenerationResourcesAvailable;
    }

    /// A recreation request is safe only when the resource owner's final
    /// present is protected by swapchain-maintenance1 retirement proof.
    [[nodiscard]] inline bool guardedLiveProfileResourceRecreationAvailable(
            const ProfileUpdateDecision& decision,
            const bool frameGenerationResourcesAvailable,
            const bool presentRetirementEnabled) {
        if (!presentRetirementEnabled ||
                !liveProfileResourceRecreationAvailable(
                    decision, frameGenerationResourcesAvailable
                )) {
            return false;
        }
        return true;
    }

    /// A natural swapchain recreation reuses Root's process-wide backend. Keep
    /// its actual GPU identity in the context profile so a requested GPU change
    /// remains visibly pending until the process restarts.
    [[nodiscard]] inline ls::GameConf profileForExistingBackend(
            const ls::GameConf& requested,
            const ls::GameConf& backendProfile) {
        auto applied = requested;
        applied.gpu = backendProfile.gpu;
        applied.ultra_performance = backendProfile.ultra_performance;
        return applied;
    }

    [[nodiscard]] inline bool backendGlobalChangePending(
            const std::optional<ls::GlobalConf>& backendGlobal,
            const ls::GlobalConf& requestedGlobal) {
        return backendGlobal && (
            backendGlobal->dll != requestedGlobal.dll ||
            backendGlobal->allow_fp16 != requestedGlobal.allow_fp16
        );
    }

    [[nodiscard]] inline bool backendProfileChangePending(
            const std::optional<ls::GameConf>& backendProfile,
            const ls::GameConf& requestedProfile) {
        return backendProfile && (
            backendProfile->gpu != requestedProfile.gpu ||
            backendProfile->ultra_performance !=
                requestedProfile.ultra_performance
        );
    }

    /// Preserve the user's live Frame Generation switch.
    [[nodiscard]] inline bool effectiveFrameGenerationEnabled(
            const ls::GameConf& profile) {
        return profile.frame_generation_provisioned &&
            profile.frame_generation_enabled;
    }

    /// Smooth Cadence opts into ready-output spacing. A Fixed batch that the
    /// private worker can deliver detached is eligible regardless of cap:
    /// a positive nonbinding cap leaves no cap-owned output slots. The worker
    /// spaces that batch without waiting in the application's present call.
    [[nodiscard]] inline bool fixedReadyOutputPacingEligible(
            const ls::GameConf& profile, const bool nativeOutputPacingEligible,
            const bool anotherTimingOwner, const bool recovering,
            const bool detachedOutput = false) {
        return nativeOutputPacingEligible && !anotherTimingOwner && !recovering &&
            effectiveFrameGenerationEnabled(profile) &&
            (profile.adaptive_stable_cadence || detachedOutput);
    }

    /// Use the existing cap interval for exact Fixed batches that fit the
    /// known refresh. Native Fixed FIFO and VSync-off output use the
    /// configured cap without requiring a compositor refresh estimate. Native
    /// Fixed can space its existing cap even with caller timing: those targets
    /// remain on the real image and do not supply deadlines for generated
    /// images. Other timing owners retain their existing rules.
    [[nodiscard]] inline bool baseCapPacesOutputs(
            const ls::GameConf& profile, const double effectiveCap,
            const bool outputSpacingEligible,
            const std::optional<uint32_t> displayRefreshHz = std::nullopt,
            const bool directUnthrottled = false,
            const bool nativeFifo = false,
            const bool applicationTimingOwner = false) {
        if (!effectiveFrameGenerationEnabled(profile) ||
                !outputSpacingEligible || !std::isfinite(effectiveCap) ||
                effectiveCap <= 0.0)
            return false;
        if (applicationTimingOwner && !nativeFifo)
            return false;
        return profile.base_fps_cap > 0 &&
            profile.multiplier >= ls::GameConfLimits::minimumMultiplier &&
            profile.multiplier <= ls::GameConfLimits::maximumMultiplier &&
            (directUnthrottled || nativeFifo || (displayRefreshHz &&
                effectiveCap * profile.multiplier <= *displayRefreshHz));
    }

    /// A provisioned and enabled Fixed profile applies its saved base cap.
    /// Explicit Off keeps all caps dormant.
    [[nodiscard]] inline double effectiveBaseFpsCap(
            const ls::GameConf& profile) {
        if (!profile.frame_generation_provisioned ||
                !profile.frame_generation_enabled)
            return 0.0;
        return static_cast<double>(profile.base_fps_cap);
    }

    /// Apply only the policy established by a committed private FG resource set.
    /// Live edits may have changed during preparation.
    inline void applyFrameGenerationResourceProfile(ls::GameConf& applied,
            const ls::GameConf& resources) {
        applied.flow_scale = resources.flow_scale;
        applied.performance_mode = resources.performance_mode;
        applied.multiplier = resources.multiplier;
    }

    struct GamescopeBridgeOutputPolicy {
        double outputFps{0.0};
        bool generatedTimeline{false};
    };

    /// Bridge output follows the saved Fixed cap times the multiplier when a
    /// generated timeline is effective. Otherwise the refresh ceiling stands.
    [[nodiscard]] inline GamescopeBridgeOutputPolicy gamescopeBridgeOutputPolicy(
            const ls::GameConf& profile, const std::optional<uint32_t> refreshHz) {
        const double baseFpsCap = effectiveBaseFpsCap(profile);
        const bool generatedTimeline = effectiveFrameGenerationEnabled(profile);
        double outputFps = static_cast<double>(refreshHz.value_or(0));
        if (generatedTimeline && baseFpsCap > 0.0)
            outputFps = std::min(outputFps,
                baseFpsCap * static_cast<double>(profile.multiplier));
        return {outputFps, generatedTimeline};
    }

    /// Reserve the private output set for the Fixed multiplier. A live
    /// multiplier change that needs more capacity prepares and atomically
    /// replaces framegen-vulkan's private frame-generation context.
    [[nodiscard]] inline size_t generatedFrameCapacityForProfile(
            const ls::GameConf& profile) {
        if (!profile.frame_generation_provisioned)
            return 0;
        return profile.multiplier - 1;
    }

    [[nodiscard]] inline size_t fixedGeneratedFrameCount(
            const size_t multiplier, const size_t generatedFrameCapacity) {
        return std::min(multiplier - 1, generatedFrameCapacity);
    }

    /// Build the profile that can be applied to an existing context while
    /// retaining requested resource-shape and process-static fields for their
    /// documented boundary. A pending restart-only edit must not prevent an
    /// unrelated cap, mode, target, or live generation switch from applying.
    [[nodiscard]] inline ProfileUpdatePlan planProfileUpdate(
            const ls::GameConf& current, const ls::GameConf& next,
            const size_t generatedFrameCapacity,
            const bool frameGenerationResourcesAvailable,
            const bool frameGenerationPrivateRebuildAvailable = false) {
        ls::GameConf applied = next;
        bool swapchainRecreationDeferred = false;
        bool processRestartDeferred = false;

        // GPU, Frame Generation provisioning, and Ultra Performance participate
        // in process-wide backend or Vulkan-layer construction. A natural
        // application swapchain recreation reuses those process owners and
        // therefore cannot satisfy any of these changes.
        if (current.gpu != next.gpu) {
            applied.gpu = current.gpu;
            processRestartDeferred = true;
        }
        if (current.frame_generation_provisioned !=
                next.frame_generation_provisioned) {
            applied.frame_generation_provisioned =
                current.frame_generation_provisioned;
            processRestartDeferred = true;
        }
        if (current.ultra_performance != next.ultra_performance) {
            applied.ultra_performance = current.ultra_performance;
            applied.flow_scale = current.flow_scale;
            applied.performance_mode = current.performance_mode;
            processRestartDeferred = true;
        }

        const bool frameGenerationBackendChanged =
            (current.ultra_performance == next.ultra_performance && (
            ls::effectiveFlowScale(current) != ls::effectiveFlowScale(next) ||
            ls::effectivePerformanceMode(current) !=
                ls::effectivePerformanceMode(next)));
        const bool generatedCapacityExceeded =
            generatedFrameCapacityForActivePolicy(next) >
                generatedFrameCapacity;
        const bool frameGenerationPrivateRebuild =
            frameGenerationPrivateRebuildAvailable &&
            frameGenerationResourcesAvailable &&
            !processRestartDeferred &&
            (frameGenerationBackendChanged || generatedCapacityExceeded);

        // Flow Scale and model selection shape private resources, while
        // pacing shapes the game-owned swapchain. They remain at their
        // actually applied values until recreation completes inside an
        // already compatible process.
        if (ls::effectiveFlowScale(current) !=
                ls::effectiveFlowScale(applied)) {
            applied.flow_scale = current.flow_scale;
            swapchainRecreationDeferred |= !frameGenerationPrivateRebuild;
        }
        if (ls::effectivePerformanceMode(current) !=
                ls::effectivePerformanceMode(applied)) {
            applied.performance_mode = current.performance_mode;
            swapchainRecreationDeferred |= !frameGenerationPrivateRebuild;
        }
        if (current.pacing != next.pacing) {
            applied.pacing = current.pacing;
            swapchainRecreationDeferred = true;
        }

        // Keep the currently active multiplier when the requested selection
        // needs more generated images. The requested multiplier becomes active
        // after recreation.
        if (generatedFrameCapacityForActivePolicy(applied) >
                generatedFrameCapacity) {
            applied.multiplier = current.multiplier;
            swapchainRecreationDeferred |= !frameGenerationPrivateRebuild;
        }

        if (!current.frame_generation_enabled &&
                next.frame_generation_enabled &&
                !frameGenerationResourcesAvailable) {
            applied.frame_generation_enabled = false;
            processRestartDeferred = true;
        }

        const bool frameGenerationChanged =
            current.frame_generation_enabled !=
                applied.frame_generation_enabled;
        const bool fixedMultiplierChanged =
            current.multiplier != applied.multiplier;
        const bool baseFpsCapChanged =
            effectiveBaseFpsCap(current) !=
                effectiveBaseFpsCap(applied);
        const bool generationPolicyChanged =
            current.adaptive_stable_cadence !=
                applied.adaptive_stable_cadence;
        const bool liveChange = frameGenerationChanged ||
                generationPolicyChanged ||
                fixedMultiplierChanged ||
                baseFpsCapChanged ||
                frameGenerationPrivateRebuild;

        ProfileUpdateAction action = ProfileUpdateAction::NoRuntimeChange;
        if (liveChange)
            action = ProfileUpdateAction::ApplyLive;
        else if (processRestartDeferred)
            action = ProfileUpdateAction::DeferUntilProcessRestart;
        else if (swapchainRecreationDeferred)
            action = ProfileUpdateAction::DeferUntilSwapchainRecreation;
        return {
            .appliedProfile = std::move(applied),
            .decision = {
                .action = action,
                .frameGenerationChanged = frameGenerationChanged,
                .generationPolicyChanged = generationPolicyChanged,
                .fixedMultiplierChanged = fixedMultiplierChanged,
                .baseFpsCapChanged = baseFpsCapChanged,
                .frameGenerationBackendChanged =
                    frameGenerationBackendChanged,
                .generatedFrameCapacityExceeded =
                    generatedCapacityExceeded,
                .frameGenerationPrivateRebuild =
                    frameGenerationPrivateRebuild,
                .swapchainRecreationDeferred =
                    swapchainRecreationDeferred,
                .processRestartDeferred = processRestartDeferred,
            },
        };
    }

    /// Classify a requested change without exposing the merged applied profile.
    /// Tests and callers that only need transition metadata can use this helper.
    [[nodiscard]] inline ProfileUpdateDecision classifyProfileUpdate(
            const ls::GameConf& current, const ls::GameConf& next,
            const size_t generatedFrameCapacity,
            const bool frameGenerationResourcesAvailable) {
        return planProfileUpdate(
            current, next, generatedFrameCapacity,
            frameGenerationResourcesAvailable
        ).decision;
    }

}

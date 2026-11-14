/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "adaptive_policy_limits.hpp"
#include "adaptive_scheduler.hpp"
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
        bool generationModeChanged{false};
        bool fixedMultiplierChanged{false};
        bool baseFpsCapChanged{false};
        bool gpuHeadroomChanged{false};
        bool dynamicCadenceProbeIntervalChanged{false};
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
    /// policy episode. Incomplete evidence from the old Fixed/Adaptive plan
    /// cannot classify the new plan as unhealthy. Resource-retirement and
    /// per-context recreation one-shot state remain independently owned.
    [[nodiscard]] constexpr bool
    profileUpdateInvalidatesTransientGenerationRecovery(
            const ProfileUpdateDecision& decision) noexcept {
        return decision.generationPolicyChanged ||
            decision.generationModeChanged ||
            decision.fixedMultiplierChanged ||
            decision.baseFpsCapChanged || decision.gpuHeadroomChanged;
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

    /// Private generated outputs are selected by the active Fixed/Adaptive
    /// policy. Initial construction may reserve more for the inactive policy,
    /// but the active requirement is the boundary used for live updates. A
    /// context with Frame Generation Off has no active generated-output
    /// requirement.
    [[nodiscard]] inline size_t generatedFrameCapacityForActivePolicy(
            const ls::GameConf& profile) {
        if (!profile.frame_generation_provisioned ||
                !profile.frame_generation_enabled)
            return 0;
        const size_t activeMultiplier = profile.adaptive
            ? profile.adaptive_max_multiplier
            : profile.multiplier;
        return activeMultiplier - 1;
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

    /// Return the Fractional Adaptive cap selected from short, stable
    /// real/generated ratios. Auto leaves the saved manual cap authoritative.
    [[nodiscard]] inline double fractionalRealFramePriorityCap(
            const ls::GameConf& profile) {
        return ls::adaptiveFractionalRealFramePriorityCap(
            profile.adaptive_fractional_real_frame_priority,
            profile.target_fps
        );
    }

    [[nodiscard]] inline bool fractionalRealFramePriorityActive(
            const ls::GameConf& profile) {
        return profile.adaptive && !profile.adaptive_auto_base_fps_cap &&
            profile.adaptive_fractional_real_frame_priority !=
                ls::AdaptiveFractionalRealFramePriority::Auto;
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
            !profile.adaptive && (profile.adaptive_stable_cadence || detachedOutput) &&
            !profile.dynamic_cadence_recovery && profile.gpu_headroom_percent == 0;
    }

    /// Use the existing cap interval for Fractional, below-refresh automatic
    /// caps, and exact Fixed batches that fit the known refresh. A compositor
    /// output clock can also prepare inside a target-matched automatic cap;
    /// native Fixed FIFO and VSync-off output use the configured cap without
    /// requiring a compositor refresh estimate. Native Fixed can space its
    /// existing cap even with caller timing: those targets remain on the real
    /// image and do not supply deadlines for generated images. Other timing
    /// owners and Adaptive automatic-cap policy retain their existing rules.
    [[nodiscard]] inline bool baseCapPacesOutputs(
            const ls::GameConf& profile, const double effectiveCap,
            const bool outputSpacingEligible,
            const std::optional<uint32_t> displayRefreshHz = std::nullopt,
            const bool compositorOutputTiming = false,
            const bool directUnthrottled = false,
            const bool nativeFifo = false,
            const bool applicationTimingOwner = false) {
        if (!effectiveFrameGenerationEnabled(profile) ||
                !outputSpacingEligible || !std::isfinite(effectiveCap) ||
                effectiveCap <= 0.0)
            return false;
        if (applicationTimingOwner && !(nativeFifo && !profile.adaptive))
            return false;
        if (profile.adaptive)
            return static_cast<double>(profile.target_fps) > effectiveCap &&
                (!profile.adaptive_auto_base_fps_cap || directUnthrottled ||
                    (displayRefreshHz &&
                        (profile.target_fps < *displayRefreshHz ||
                         (compositorOutputTiming &&
                          profile.target_fps == *displayRefreshHz))));
        return !profile.dynamic_cadence_recovery &&
            (profile.base_fps_cap > 0 || profile.gpu_headroom_percent > 0) &&
            profile.multiplier >= ls::GameConfLimits::minimumMultiplier &&
            profile.multiplier <= ls::GameConfLimits::maximumMultiplier &&
            (directUnthrottled || nativeFifo || (displayRefreshHz &&
                effectiveCap * profile.multiplier <= *displayRefreshHz));
    }

    /// Auto-cap aligns the common healthy path with an exact 2x cadence.
    /// Explicit Fractional priority instead selects its target-relative cap.
    /// Adaptive can still raise its multiplier when the game falls below this
    /// ceiling. Keep the engine's 10 FPS policy floor for unusually low targets.
    /// Explicit Off keeps all caps dormant.
    [[nodiscard]] inline double effectiveBaseFpsCap(
            const ls::GameConf& profile) {
        if (!profile.frame_generation_provisioned ||
                !profile.frame_generation_enabled)
            return 0.0;
        if (profile.adaptive && profile.adaptive_auto_base_fps_cap) {
            return std::max(
                adaptiveMinimumBaseFps,
                static_cast<double>(profile.target_fps) / 2.0
            );
        }
        if (fractionalRealFramePriorityActive(profile))
            return fractionalRealFramePriorityCap(profile);
        return static_cast<double>(profile.base_fps_cap);
    }

    /// A proven Ordered-SDR collapse may ask the presentation pacer to release
    /// only Adaptive's automatic half-target cap. Manual and Fixed caps remain
    /// authoritative. The release lasts only for the bounded rescue sample;
    /// verification runs after the configured cap has been restored.
    [[nodiscard]] inline double effectiveBaseFpsCap(
            const ls::GameConf& profile,
            const AdaptiveSchedulerSnapshot& scheduler) {
        if (!effectiveFrameGenerationEnabled(profile))
            return effectiveBaseFpsCap(profile);
        if (profile.adaptive && profile.adaptive_auto_base_fps_cap &&
                scheduler.automaticBaseCapSuppressed) {
            return 0.0;
        }
        return effectiveBaseFpsCap(profile);
    }

    [[nodiscard]] inline bool dynamicCadenceRecoveryEnabled(
            const ls::GameConf& profile) {
        return profile.dynamic_cadence_recovery &&
            effectiveBaseFpsCap(profile) <= 0.0;
    }

    /// A proven Steady 2x cadence keeps its established FIFO handoff. Probe and
    /// transport guards restore pacing.
    [[nodiscard]] inline std::optional<size_t>
    smoothCadencePacerHandoffGenerationLimit(
            const ls::GameConf& profile,
            const bool privateOrderedTransport,
            const bool orderedAcquireRecoveryActive,
            const std::optional<uint32_t> displayRefreshHz,
            const AdaptiveSchedulerSnapshot& scheduler,
            const std::optional<size_t> activeGenerationLimit = std::nullopt) {
        const size_t generationLimit = scheduler.stableCadenceLimit.value_or(
            scheduler.validatedGenerationLimit
        );
        if (generationLimit == 0 ||
                generationLimit >= profile.adaptive_max_multiplier)
            return std::nullopt;
        const double targetFps = static_cast<double>(profile.target_fps);
        const double projectedOutputFps = scheduler.smoothedBaseFps *
            static_cast<double>(generationLimit + 1);
        const bool provenTwoX = generationLimit == 1 &&
            scheduler.phase == AdaptiveSchedulerPhase::StableCadence &&
            !scheduler.stableCadenceEvaluationActive;
        const bool entryCadenceMatched =
            projectedOutputFps >= targetFps * 0.98 &&
            projectedOutputFps <= targetFps *
                (generationLimit == 1 ? 1.02 : 1.25);
        // Entry needs a close target match. Once accepted, the scheduler's
        // sustained cadence/delivery qualification owns retention; applying
        // entry thresholds again would turn one noisy estimate into a failed
        // handoff and a minute-long cooldown. All eligibility guards still apply.
        const bool retainActiveHandoff =
            activeGenerationLimit == generationLimit;
        const bool eligible = profile.adaptive &&
            profile.adaptive_stable_cadence &&
            effectiveFrameGenerationEnabled(profile) &&
            privateOrderedTransport &&
            !orderedAcquireRecoveryActive &&
            adaptiveTargetMatchesRefresh(
                profile.target_fps, displayRefreshHz
            ) &&
            profile.adaptive_auto_base_fps_cap && provenTwoX &&
            (entryCadenceMatched || retainActiveHandoff);
        return eligible ? std::optional<size_t>{generationLimit} : std::nullopt;
    }

    /// Integer-cadence base-cap refinement is limited to the same ordered,
    /// target-matched Steady path as the FIFO handoff. Fractional Adaptive,
    /// recovery and unmatched displays retain their existing
    /// pacing policy.
    [[nodiscard]] inline bool smoothCadenceBaseCapEligible(
            const ls::GameConf& profile,
            const bool privateOrderedTransport,
            const bool orderedAcquireRecoveryActive,
            const std::optional<uint32_t> displayRefreshHz) {
        return profile.adaptive &&
            profile.adaptive_auto_base_fps_cap &&
            profile.adaptive_stable_cadence &&
            effectiveFrameGenerationEnabled(profile) &&
            privateOrderedTransport &&
            !orderedAcquireRecoveryActive &&
            adaptiveTargetMatchesRefresh(
                profile.target_fps, displayRefreshHz
            );
    }

    /// Only Fractional Automatic can release the generated presentation ceiling.
    /// A target-capable manual cap remains enforced by the source pacer.
    [[nodiscard]] inline bool fractionalNativePresentationEligible(
            const ls::GameConf& profile) {
        return profile.adaptive && !profile.adaptive_auto_base_fps_cap &&
            profile.adaptive_fractional_real_frame_priority ==
                ls::AdaptiveFractionalRealFramePriority::Auto &&
            (profile.base_fps_cap == 0 || profile.base_fps_cap >= profile.target_fps);
    }

    struct GenerationSchedulerPolicy {
        uint32_t targetFps{0};
        size_t maximumMultiplier{0};
        bool stableCadence{false};
        bool nearTargetNativePreference{false};
        bool qualifyTargetNativeCadence{false};
        bool dynamicCadenceRecovery{false};
        float dynamicCadenceProbeIntervalSeconds{
            ls::GameConfDefaults::dynamicCadenceProbeIntervalSeconds
        };
    };

    /// Resolve only an opted-in Adaptive target. The saved fallback is never
    /// overwritten by display feedback; the scheduler retains its supported range.
    [[nodiscard]] constexpr uint32_t adaptiveTargetFpsForDisplay(
            const uint32_t fallbackTargetFps, const bool adaptive,
            const bool matchDisplayRefresh,
            const std::optional<uint32_t> displayRefreshHz) noexcept {
        if (!adaptive || !matchDisplayRefresh || !displayRefreshHz ||
                *displayRefreshHz == 0)
            return fallbackTargetFps;
        return std::clamp(*displayRefreshHz,
            ls::GameConfLimits::minimumTargetFps,
            ls::GameConfLimits::maximumTargetFps);
    }

    /// Apply only the policy established by a committed private FG resource set.
    /// Live target edits and compositor feedback may have changed during preparation.
    inline void applyFrameGenerationResourceProfile(ls::GameConf& applied,
            const ls::GameConf& resources, const ls::GameConf& requested,
            const std::optional<uint32_t> displayRefreshHz) {
        applied.flow_scale = resources.flow_scale;
        applied.performance_mode = resources.performance_mode;
        applied.adaptive = resources.adaptive;
        applied.multiplier = resources.multiplier;
        applied.adaptive_max_multiplier = resources.adaptive_max_multiplier;
        applied.target_fps = adaptiveTargetFpsForDisplay(
            requested.target_fps, applied.adaptive,
            applied.adaptive_target_refresh_rate, displayRefreshHz
        );
    }

    /// Adaptive owns the resolved applied output target. Fixed keeps the
    /// selected multiplier exact rather than borrowing a hidden target.
    [[nodiscard]] inline std::optional<GenerationSchedulerPolicy>
    generationSchedulerPolicy(const ls::GameConf& profile) {
        if (profile.adaptive) {
            return GenerationSchedulerPolicy{
                .targetFps = profile.target_fps,
                .maximumMultiplier = profile.adaptive_max_multiplier,
                .stableCadence = profile.adaptive_stable_cadence,
                .nearTargetNativePreference = true,
                .qualifyTargetNativeCadence =
                    fractionalNativePresentationEligible(profile),
                .dynamicCadenceRecovery =
                    dynamicCadenceRecoveryEnabled(profile),
                .dynamicCadenceProbeIntervalSeconds =
                    profile.dynamic_cadence_probe_interval_seconds,
            };
        }
        return std::nullopt;
    }

    struct GamescopeBridgeOutputPolicy {
        double outputFps{0.0};
        bool generatedTimeline{false};
    };

    /// Qualified native Automatic presentation releases both the Adaptive
    /// ceiling and generated-work lead. A sparse Fractional turn or temporary
    /// native probe still belongs to the generated timeline. A manual cap at
    /// or above target remains enforced by the source pacer. Below-target caps,
    /// explicit priorities and Steady retain their existing output policy.
    [[nodiscard]] inline GamescopeBridgeOutputPolicy gamescopeBridgeOutputPolicy(
            const ls::GameConf& profile, const std::optional<uint32_t> refreshHz,
            const AdaptiveSchedulerSnapshot& scheduler = {},
            const double gpuHeadroomCap = 0.0) {
        double baseFpsCap = effectiveBaseFpsCap(profile);
        if (gpuHeadroomCap > 0.0)
            baseFpsCap = baseFpsCap > 0.0 ? std::min(baseFpsCap, gpuHeadroomCap) : gpuHeadroomCap;
        const bool nativeAutomatic = fractionalNativePresentationEligible(profile) &&
            scheduler.phase == AdaptiveSchedulerPhase::NearTargetNative &&
            scheduler.generationLimit == 0;
        const bool generatedTimeline =
            effectiveFrameGenerationEnabled(profile) && !nativeAutomatic;
        double outputFps = static_cast<double>(refreshHz.value_or(0));
        if (generatedTimeline) {
            if (profile.adaptive)
                outputFps = std::min(outputFps,
                    static_cast<double>(profile.target_fps));
            else if (baseFpsCap > 0.0)
                outputFps = std::min(outputFps,
                    baseFpsCap * static_cast<double>(profile.multiplier));
        }
        return {outputFps, generatedTimeline};
    }

    /// Reserve one private output set that can serve both Fixed and Adaptive.
    /// Ultra Performance deliberately keeps only the active policy. A live
    /// mode or multiplier change that needs a different capacity prepares and
    /// atomically replaces framegen-vulkan's private frame-generation context.
    [[nodiscard]] inline size_t generatedFrameCapacityForProfile(
            const ls::GameConf& profile) {
        if (!profile.frame_generation_provisioned)
            return 0;
        if (profile.ultra_performance) {
            const size_t activeMultiplier = profile.adaptive
                ? profile.adaptive_max_multiplier
                : profile.multiplier;
            return activeMultiplier - 1;
        }
        return std::max(profile.multiplier, profile.adaptive_max_multiplier) - 1;
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
            const bool frameGenerationPrivateRebuildAvailable = false,
            const std::optional<uint32_t> displayRefreshHz = std::nullopt) {
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

        // Keep the currently active policy when the requested Fixed/Adaptive
        // selection needs more generated images. Dormant settings can still be
        // retained in the applied profile and become active after recreation.
        if (generatedFrameCapacityForActivePolicy(applied) >
                generatedFrameCapacity) {
            applied.adaptive = current.adaptive;
            if (current.adaptive)
                applied.adaptive_max_multiplier =
                    current.adaptive_max_multiplier;
            else
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
        applied.target_fps = adaptiveTargetFpsForDisplay(
            next.target_fps, applied.adaptive,
            applied.adaptive_target_refresh_rate, displayRefreshHz
        );
        const bool generationModeChanged = current.adaptive != applied.adaptive;
        const bool fixedMultiplierChanged =
            current.multiplier != applied.multiplier &&
            (!current.adaptive || !applied.adaptive);
        const bool baseFpsCapChanged =
            effectiveBaseFpsCap(current) !=
                effectiveBaseFpsCap(applied);
        const bool gpuHeadroomChanged = current.gpu_headroom_percent != applied.gpu_headroom_percent &&
            (effectiveFrameGenerationEnabled(current) ||
             effectiveFrameGenerationEnabled(applied));
        const bool dynamicCadenceProbeIntervalChanged =
            current.dynamic_cadence_probe_interval_seconds !=
                applied.dynamic_cadence_probe_interval_seconds &&
            (current.dynamic_cadence_recovery ||
             applied.dynamic_cadence_recovery);
        const bool generationPolicyChanged =
            current.dynamic_cadence_recovery !=
                applied.dynamic_cadence_recovery ||
            current.adaptive_stable_cadence !=
                applied.adaptive_stable_cadence ||
            (current.adaptive && applied.adaptive && (
                // Equal numeric caps can still change scheduler ownership
                // between Fractional and Steady.
                (current.adaptive_auto_base_fps_cap !=
                    applied.adaptive_auto_base_fps_cap &&
                 (effectiveFrameGenerationEnabled(current) ||
                  effectiveFrameGenerationEnabled(applied))) ||
                current.target_fps != applied.target_fps ||
                current.adaptive_max_multiplier !=
                    applied.adaptive_max_multiplier
            ));
        const bool liveChange = frameGenerationChanged ||
                generationPolicyChanged ||
                generationModeChanged || fixedMultiplierChanged ||
                baseFpsCapChanged || gpuHeadroomChanged || dynamicCadenceProbeIntervalChanged ||
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
                .generationModeChanged = generationModeChanged,
                .fixedMultiplierChanged = fixedMultiplierChanged,
                .baseFpsCapChanged = baseFpsCapChanged,
                .gpuHeadroomChanged = gpuHeadroomChanged,
                .dynamicCadenceProbeIntervalChanged =
                    dynamicCadenceProbeIntervalChanged,
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

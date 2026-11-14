/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "profile_update.hpp"
#include "swapchain/create_policy.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

using namespace fgvk::layer;

namespace {
    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }

    ls::GameConf adaptiveProfile() {
        return {
            .name = "Adaptive",
            .active_in = {"game"},
            .multiplier = 2,
            .frame_generation_enabled = true,
            .base_fps_cap = 0,
            .adaptive = true,
            .adaptive_auto_base_fps_cap = false,
            .adaptive_fractional_real_frame_priority =
                ls::AdaptiveFractionalRealFramePriority::Auto,
            .target_fps = 90,
            .adaptive_max_multiplier = 3,
            .adaptive_stable_cadence = false,
            .dynamic_cadence_recovery = false,
            .dynamic_cadence_probe_interval_seconds = 2.0F,
            .flow_scale = 1.0F,
            .performance_mode = false,
            .pacing = ls::Pacing::None,
        };
    }
}

int main() {
    {
        ls::GameConf fixed;
        fixed.multiplier = 2;
        auto enabled = fixed;
        enabled.gpu_headroom_percent = 10;
        auto plan = planProfileUpdate(fixed, enabled, 2, true);
        expect(plan.decision.gpuHeadroomChanged && plan.decision.action == ProfileUpdateAction::ApplyLive &&
            !plan.decision.frameGenerationPrivateRebuild && !plan.decision.generationPolicyChanged,
            "Headroom is live without private resource or scheduler replacement");
        expect(profileUpdateInvalidatesTransientGenerationRecovery(plan.decision),
            "Headroom toggle starts a new source pacing episode");
        expect(baseCapPacesOutputs(enabled, 35, true, 90),
            "Fixed headroom participates in existing evenly spaced output preparation");
        expect(gamescopeBridgeOutputPolicy(enabled, 90, {}, 35).outputFps == 70,
            "Owned output clocks follow Fixed headroom's actual source cap");
        enabled.adaptive = true;
        enabled.target_fps = 90;
        expect(gamescopeBridgeOutputPolicy(enabled, 90, {}, 35).outputFps == 90,
            "Adaptive retains its generated output target");
        fixed.frame_generation_enabled = false;
        enabled = fixed;
        enabled.gpu_headroom_percent = 20;
        plan = planProfileUpdate(fixed, enabled, 2, true);
        expect(!plan.decision.gpuHeadroomChanged && plan.appliedProfile.gpu_headroom_percent == 20,
            "Headroom is saved dormant with Frame Generation disabled");
    }

    const auto resolveDisplayTarget = [](const ls::GameConf& applied,
            const ls::GameConf& requested, std::optional<uint32_t> refresh) {
        return planProfileUpdate(applied, requested, 3, true, false, refresh);
    };
    auto savedTarget = adaptiveProfile();
    expect(resolveDisplayTarget(savedTarget, savedTarget, 60)
                .appliedProfile.target_fps == 90,
        "Refresh targeting must default off and preserve existing targets");
    auto automaticTarget = savedTarget;
    automaticTarget.adaptive_target_refresh_rate = true;
    auto displayApplied = savedTarget;
    for (const auto refresh : {90U, 60U, 100U, 144U, 360U, 5U, 2000U}) {
        const auto plan = resolveDisplayTarget(displayApplied, automaticTarget, refresh);
        const auto target = std::clamp(refresh,
            ls::GameConfLimits::minimumTargetFps,
            ls::GameConfLimits::maximumTargetFps);
        expect(plan.appliedProfile.target_fps == target &&
                plan.appliedProfile.adaptive_target_refresh_rate &&
                !plan.decision.swapchainRecreationDeferred &&
                !plan.decision.processRestartDeferred,
            "Display transitions must apply a bounded target without rebuilding resources");
        expect(plan.decision.generationPolicyChanged ==
                (displayApplied.target_fps != target),
            "Display transitions must reset Fractional scheduling only when the target changes");
        displayApplied = plan.appliedProfile;
        expect(generationSchedulerPolicy(displayApplied)->targetFps == target,
            "Scheduler policy must consume the applied display target");
        auto steady = displayApplied;
        steady.adaptive_auto_base_fps_cap = true;
        expect(effectiveBaseFpsCap(steady) == std::max(adaptiveMinimumBaseFps, target / 2.0),
            "Steady's automatic cap must track the applied display target");
        auto fractional = displayApplied;
        fractional.adaptive_fractional_real_frame_priority =
            ls::AdaptiveFractionalRealFramePriority::High;
        expect(effectiveBaseFpsCap(fractional) == target * 3.0 / 4.0,
            "Fractional real-frame priority must track the applied display target");
    }
    for (const auto missing : {std::optional<uint32_t>{}, std::optional<uint32_t>{0}}) {
        const auto plan = resolveDisplayTarget(displayApplied, automaticTarget, missing);
        expect(plan.appliedProfile.target_fps == 90 &&
                plan.decision.generationPolicyChanged,
            "Missing or zero refresh must restore the saved fallback live");
    }
    automaticTarget.target_fps = 120;
    const auto changedFallback = resolveDisplayTarget(displayApplied, automaticTarget, 100);
    expect(changedFallback.appliedProfile.target_fps == 100 &&
            automaticTarget.target_fps == 120,
        "Display targeting must never overwrite the requested manual fallback");
    const auto repeated = resolveDisplayTarget(changedFallback.appliedProfile, automaticTarget, 100);
    expect(repeated.decision.action == ProfileUpdateAction::NoRuntimeChange,
        "Repeated profile reads must retain the resolved target without resetting policy");
    automaticTarget.adaptive_target_refresh_rate = false;
    expect(resolveDisplayTarget(changedFallback.appliedProfile, automaticTarget, 100)
                .appliedProfile.target_fps == 120,
        "Disabling display targeting must restore the saved manual target");
    automaticTarget.adaptive_target_refresh_rate = true;
    automaticTarget.adaptive = false;
    expect(resolveDisplayTarget(savedTarget, automaticTarget, 60)
                .appliedProfile.target_fps == 120,
        "Fixed mode must ignore the dormant Adaptive refresh choice");

    auto fixedBeforeGrowth = automaticTarget;
    auto adaptiveAfterGrowth = fixedBeforeGrowth;
    adaptiveAfterGrowth.adaptive = true;
    adaptiveAfterGrowth.adaptive_max_multiplier = 5;
    const auto pendingGrowth = planProfileUpdate(fixedBeforeGrowth, adaptiveAfterGrowth, 2, true, true, 60);
    expect(pendingGrowth.decision.frameGenerationPrivateRebuild &&
            !pendingGrowth.appliedProfile.adaptive &&
            pendingGrowth.appliedProfile.target_fps == 120,
        "Private growth must retain Fixed policy until resources commit");
    for (const auto refresh : {std::optional<uint32_t>{60},
            std::optional<uint32_t>{144}, std::optional<uint32_t>{0},
            std::optional<uint32_t>{}}) {
        auto committed = pendingGrowth.appliedProfile;
        applyFrameGenerationResourceProfile(
            committed, adaptiveAfterGrowth, adaptiveAfterGrowth, refresh
        );
        const auto target = refresh && *refresh ? *refresh : 120U;
        expect(committed.adaptive && committed.target_fps == target &&
                generationSchedulerPolicy(committed)->targetFps == target,
            "Private commit must resolve the new Adaptive policy from current refresh");
        committed.adaptive_auto_base_fps_cap = true;
        expect(effectiveBaseFpsCap(committed) == target / 2.0,
            "Private commit must align Steady caps with the resolved target");
    }
    auto latestTarget = adaptiveAfterGrowth;
    latestTarget.target_fps = 165;
    latestTarget.adaptive_target_refresh_rate = false;
    auto liveWhilePreparing = pendingGrowth.appliedProfile;
    liveWhilePreparing.adaptive_target_refresh_rate = false;
    liveWhilePreparing.base_fps_cap = 40;
    applyFrameGenerationResourceProfile(
        liveWhilePreparing, adaptiveAfterGrowth, latestTarget, 60
    );
    expect(liveWhilePreparing.target_fps == 165 &&
            !liveWhilePreparing.adaptive_target_refresh_rate &&
            liveWhilePreparing.base_fps_cap == 40,
        "Resource commit must preserve newer live edits and use the latest fallback");
    latestTarget.adaptive_target_refresh_rate = true;
    liveWhilePreparing.adaptive_target_refresh_rate = true;
    applyFrameGenerationResourceProfile(
        liveWhilePreparing, fixedBeforeGrowth, latestTarget, 60
    );
    expect(!liveWhilePreparing.adaptive && liveWhilePreparing.target_fps == 165,
        "A private commit back to Fixed must restore the latest saved fallback");

    auto fractionalPacing = adaptiveProfile();
    fractionalPacing.frame_generation_provisioned = true;
    for (const bool smooth : {false, true}) {
        fractionalPacing.adaptive_stable_cadence = smooth;
        expect(baseCapPacesOutputs(fractionalPacing, 60.0, true),
            "Fractional source-cap slack must be usable with either Smooth Cadence setting");
        fractionalPacing.adaptive = false;
        expect(!baseCapPacesOutputs(fractionalPacing, 60.0, true),
            "uncapped Fixed must retain its original source pacing");
        fractionalPacing.adaptive = true;
        fractionalPacing.adaptive_auto_base_fps_cap = true;
        expect(!baseCapPacesOutputs(fractionalPacing, 45.0, true) &&
                !baseCapPacesOutputs(fractionalPacing, 45.0, true, 90) &&
                !baseCapPacesOutputs(fractionalPacing, 45.0, true, 60),
            "automatic caps must preserve target-matched FIFO and unknown-refresh pacing");
        expect(baseCapPacesOutputs(fractionalPacing, 45.0, true, 120),
            "below-refresh automatic caps must space their existing generated outputs");
        expect(baseCapPacesOutputs(fractionalPacing, 45.0, true, 90, true),
            "compositor-owned timing must prepare inside a target-matched automatic cap");
        expect(!baseCapPacesOutputs(fractionalPacing, 45.0, false, 90, true) &&
                !baseCapPacesOutputs(fractionalPacing, 45.0, true, {}, true) &&
                !baseCapPacesOutputs(fractionalPacing, 45.0, true, 60, true),
            "timed preparation bypassed transport, refresh or output-ceiling guards");
        fractionalPacing.adaptive_auto_base_fps_cap = false;
    }
    expect(!baseCapPacesOutputs(fractionalPacing, 60.0, false) &&
            !baseCapPacesOutputs(fractionalPacing, 0.0, true) &&
            !baseCapPacesOutputs(fractionalPacing, 90.0, true) &&
            !baseCapPacesOutputs(fractionalPacing, 120.0, true),
        "Fractional output pacing must not invent a source cap or override another transport");
    fractionalPacing.frame_generation_enabled = false;
    expect(!baseCapPacesOutputs(fractionalPacing, 60.0, true),
        "disabled generation must not defer its source cap");
    fractionalPacing.frame_generation_enabled = true;
    fractionalPacing.frame_generation_provisioned = false;
    expect(!baseCapPacesOutputs(fractionalPacing, 60.0, true),
        "unprovisioned generation must not defer its source cap");
    auto cappedFixedPacing = adaptiveProfile();
    cappedFixedPacing.frame_generation_provisioned = true;
    cappedFixedPacing.adaptive = false;
    for (const bool smooth : {false, true}) {
        cappedFixedPacing.adaptive_stable_cadence = smooth;
        for (const int multiplier : {2, 3, 4, 5}) {
            cappedFixedPacing.multiplier = multiplier;
            cappedFixedPacing.base_fps_cap = 120 / multiplier;
            for (const int target : {30, 120, 240}) {
                cappedFixedPacing.target_fps = target;
                expect(baseCapPacesOutputs(cappedFixedPacing,
                        cappedFixedPacing.base_fps_cap, true, 120),
                    "capped Fixed output pacing must not depend on the Adaptive target");
            }
        }
    }
    cappedFixedPacing.multiplier = 2;
    cappedFixedPacing.base_fps_cap = 30;
    for (const int cap : {0, 30, 60, 610}) {
        auto direct = cappedFixedPacing;
        direct.base_fps_cap = cap;
        expect(fixedReadyOutputPacingEligible(direct, true, false, false),
            "Fixed Smooth Cadence must support external and combined source caps");
        expect(!fixedReadyOutputPacingEligible(direct, false, false, false) &&
                !fixedReadyOutputPacingEligible(direct, true, true, false) &&
                !fixedReadyOutputPacingEligible(direct, true, false, true),
            "ready-output pacing replaced an ineligible transport, explicit timing or recovery");
        direct.adaptive_stable_cadence = false;
        expect(!fixedReadyOutputPacingEligible(direct, true, false, false), "Smooth Cadence Off added output waits");
        expect(fixedReadyOutputPacingEligible(direct, true, false, false, true),
            "detached Fixed output stayed unspaced with Smooth Cadence Off");
        expect(!fixedReadyOutputPacingEligible(direct, false, false, false, true) &&
                !fixedReadyOutputPacingEligible(direct, true, true, false, true) &&
                !fixedReadyOutputPacingEligible(direct, true, false, true, true),
            "detached spacing replaced an ineligible transport, explicit timing or recovery");
        direct.adaptive_stable_cadence = true;
        direct.adaptive = true;
        expect(!fixedReadyOutputPacingEligible(direct, true, false, false), "Fixed pacing replaced Adaptive");
        expect(!fixedReadyOutputPacingEligible(direct, true, false, false, true), "detached pacing replaced Adaptive");
        direct.adaptive = false;
        direct.dynamic_cadence_recovery = true;
        expect(!fixedReadyOutputPacingEligible(direct, true, false, false), "Fixed pacing replaced cadence recovery");
        expect(!fixedReadyOutputPacingEligible(direct, true, false, false, true), "detached pacing replaced cadence recovery");
        direct.dynamic_cadence_recovery = false;
        direct.gpu_headroom_percent = 10;
        expect(!fixedReadyOutputPacingEligible(direct, true, false, false), "Fixed pacing replaced GPU Headroom");
        expect(!fixedReadyOutputPacingEligible(direct, true, false, false, true), "detached pacing replaced GPU Headroom");
        direct.gpu_headroom_percent = 0;
        direct.frame_generation_enabled = false;
        expect(!fixedReadyOutputPacingEligible(direct, true, false, false), "generation Off added output waits");
        expect(!fixedReadyOutputPacingEligible(direct, true, false, false, true), "generation Off added detached pacing");
    }
    // Minecraft: Smooth Cadence off, game-owned limiter, and a deliberately
    // nonbinding 610 FPS cap. Exercise qualification and ownership together.
    for (const int cap : {0, 610, 60}) {
        using Clock = RealFramePacer::Clock;
        using namespace std::chrono_literals;
        auto native = cappedFixedPacing;
        native.adaptive_stable_cadence = false;
        native.base_fps_cap = cap;
        RealFramePacer pacer;
        auto arrival = Clock::time_point{10s};
        auto returnAt = arrival;
        bool capBatchSeen = false;
        bool readyAfterCap = false;
        for (const int gameFps : {60, 30, 100, 30}) {
            const auto sourcePeriod = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(1.0 / gameFps));
            const auto outputPeriod = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(1.0 / (2 * (cap > 0 ? std::min(cap, gameFps) : gameFps))));
            for (size_t frame = 0; frame < 120; ++frame) {
                arrival = std::max(arrival + sourcePeriod, returnAt);
                const auto deadline = pacer.schedule(arrival, cap);
                const bool eligible = fixedReadyOutputPacingEligible(
                    native, true, false, false, true);
                pacer.observeSource(arrival, eligible, 2);
                std::optional<RealFramePacer::OutputBatch> batch;
                bool capOwned = false;
                if (deadline > arrival && baseCapPacesOutputs(
                        native, cap, true, {}, false, true)) {
                    batch = pacer.scheduleOutputs(arrival + 100us, deadline,
                        2.0 * cap, 2, false, true);
                    capOwned = batch.has_value();
                    capBatchSeen |= capOwned;
                }
                if (!batch && eligible)
                    batch = pacer.scheduleReadyOutputs(arrival + 100us, 2, true);
                if (frame >= 40) {
                    expect(batch.has_value(), "native Fixed cap/source transition lost output spacing");
                    expect(std::abs(std::chrono::duration<double>(batch->interval - outputPeriod).count()) < 0.00001,
                        "native Fixed output spacing followed a nonbinding cap or stale source period");
                    if (capBatchSeen && !capOwned) readyAfterCap = true;
                }
                if (cap == 0 || cap == 610)
                    expect(deadline <= arrival, "nonbinding cap delayed the game-owned source");
                pacer.recordOutputWait(Clock::duration{}, batch.has_value(), batch.has_value());
                if (batch) pacer.completeOutputBatch(batch->at(1));
                returnAt = std::max(arrival, deadline);
            }
        }
        if (cap == 60)
            expect(capBatchSeen && readyAfterCap, "binding cap failed to take over and release output ownership");
    }
    expect(baseCapPacesOutputs(cappedFixedPacing, 30., true, {}, false, true) &&
            baseCapPacesOutputs(cappedFixedPacing, 30., true, 50, false, true),
        "VSync-off Fixed output spacing must not depend on a refresh ceiling");
    for (const bool smooth : {false, true}) {
        auto native = cappedFixedPacing;
        native.adaptive_stable_cadence = smooth;
        expect(baseCapPacesOutputs(native, 30., true, {}, false, false, true),
            "Native FIFO must prepare within the cap with Smooth Cadence on or off");
        for (const uint32_t multiplier : {2U, 3U, 4U, 5U}) {
            native.multiplier = multiplier;
            expect(baseCapPacesOutputs(native, 30., true, {}, false, false, true, true),
                "caller real-frame targets must not turn an active native Fixed cap into generated/real bursts");
        }
        native.base_fps_cap = 0;
        expect(!baseCapPacesOutputs(native, 30., true, {}, false, false, true, true),
            "caller timing must not invent an unconfigured source cap");
        native.base_fps_cap = cappedFixedPacing.base_fps_cap;
        native.dynamic_cadence_recovery = true;
        expect(!baseCapPacesOutputs(native, 30., true, {}, false, false, true, true),
            "caller-timed Fixed recovery must retain its own clock");
        native.dynamic_cadence_recovery = false;
        expect(!baseCapPacesOutputs(native, 30., false, {}, false, false, true) &&
                !baseCapPacesOutputs(native, 0., true, {}, false, false, true),
            "Native FIFO cap preparation must preserve timing ownership and require a cap");
    }
    expect(!baseCapPacesOutputs(cappedFixedPacing, 30., false, {}, false, true) &&
            !baseCapPacesOutputs(cappedFixedPacing, 0., true, {}, false, true),
        "VSync-off pacing must preserve application timing and require a source cap");
    auto directAdaptive = adaptiveProfile();
    directAdaptive.frame_generation_provisioned = true;
    directAdaptive.adaptive_auto_base_fps_cap = true;
    expect(baseCapPacesOutputs(directAdaptive, 45., true, {}, false, true),
        "VSync-off Adaptive automatic caps must space outputs without compositor feedback");
    expect(!baseCapPacesOutputs(cappedFixedPacing, 30., true, {}, false, true, false, true) &&
            !baseCapPacesOutputs(cappedFixedPacing, 30., true, 120, true, false, false, true) &&
            !baseCapPacesOutputs(directAdaptive, 45., true, 120, false, false, true, true),
        "caller timing must retain non-FIFO, compositor and Adaptive output ownership");
    expect(baseCapPacesOutputs(cappedFixedPacing, 30., true, 120),
        "below-refresh Fixed must use its existing source-cap slack");
    expect(!baseCapPacesOutputs(cappedFixedPacing, 30., false, 120) &&
            !baseCapPacesOutputs(cappedFixedPacing, 30., true) &&
            !baseCapPacesOutputs(cappedFixedPacing, 30., true, 50) &&
            !baseCapPacesOutputs(cappedFixedPacing, 0., true, 120),
        "Fixed output pacing must preserve transport, refresh and source-cap guards");
    for (const double invalid : {-1., std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN()}) {
        expect(!baseCapPacesOutputs(cappedFixedPacing, invalid, true, 120),
            "invalid Fixed source rates must not acquire output pacing");
    }
    for (const int multiplier : {0, 1, 6}) {
        cappedFixedPacing.multiplier = multiplier;
        expect(!baseCapPacesOutputs(cappedFixedPacing, 30., true, 240),
            "unsupported Fixed batches must not acquire output pacing");
    }
    cappedFixedPacing.multiplier = 2;
    cappedFixedPacing.dynamic_cadence_recovery = true;
    expect(!baseCapPacesOutputs(cappedFixedPacing, 30., true, 120),
        "Fixed recovery must retain its own timing boundary");
    cappedFixedPacing.dynamic_cadence_recovery = false;
    cappedFixedPacing.frame_generation_enabled = false;
    expect(!baseCapPacesOutputs(cappedFixedPacing, 30., true, 120),
        "disabled Fixed generation must not acquire output pacing");
    cappedFixedPacing.frame_generation_enabled = true;
    auto bridgeProfile = ls::GameConf{};
    bridgeProfile.frame_generation_provisioned = true;
    bridgeProfile.frame_generation_enabled = true;
    bridgeProfile.adaptive = true;
    bridgeProfile.target_fps = 90;
    expect(gamescopeBridgeOutputPolicy(bridgeProfile, 120).outputFps == 90 &&
            gamescopeBridgeOutputPolicy(bridgeProfile, 60).outputFps == 60 &&
            gamescopeBridgeOutputPolicy(bridgeProfile, 120).generatedTimeline,
        "bridge must respect Adaptive target and display ceiling");
    bridgeProfile.adaptive_auto_base_fps_cap = false;
    bridgeProfile.adaptive_fractional_real_frame_priority =
        ls::AdaptiveFractionalRealFramePriority::Auto;
    bridgeProfile.base_fps_cap = 0;
    AdaptiveSchedulerSnapshot nativeScheduler{
        .phase = AdaptiveSchedulerPhase::NearTargetNative,
        .nearTargetNativePreference = true,
    };
    const auto nativeOutput = gamescopeBridgeOutputPolicy(
        bridgeProfile, 120, nativeScheduler);
    expect(nativeOutput.outputFps == 120 && !nativeOutput.generatedTimeline &&
            gamescopeBridgeOutputPolicy(bridgeProfile, 60, nativeScheduler).outputFps == 60,
        "qualified native Automatic must release target and generated lead within refresh");
    for (const auto phase : {AdaptiveSchedulerPhase::Uninitialized,
            AdaptiveSchedulerPhase::HistoryWarmup, AdaptiveSchedulerPhase::Stabilizing,
            AdaptiveSchedulerPhase::DiscontinuityRecovery,
            AdaptiveSchedulerPhase::RescueMeasurement, AdaptiveSchedulerPhase::RearmCooldown,
            AdaptiveSchedulerPhase::RampEvaluation, AdaptiveSchedulerPhase::StableCadence,
            AdaptiveSchedulerPhase::Active, AdaptiveSchedulerPhase::NativeCadenceProbe}) {
        auto temporaryNative = nativeScheduler;
        temporaryNative.phase = phase;
        const auto output = gamescopeBridgeOutputPolicy(bridgeProfile, 120, temporaryNative);
        expect(output.outputFps == 90 && output.generatedTimeline,
            "unqualified or temporary native turns must retain generated timing");
    }
    for (const auto priority : {ls::AdaptiveFractionalRealFramePriority::Low,
            ls::AdaptiveFractionalRealFramePriority::Medium,
            ls::AdaptiveFractionalRealFramePriority::High,
            ls::AdaptiveFractionalRealFramePriority::VeryHigh}) {
        bridgeProfile.adaptive_fractional_real_frame_priority = priority;
        const auto output = gamescopeBridgeOutputPolicy(bridgeProfile, 120, nativeScheduler);
        expect(output.outputFps == 90 && output.generatedTimeline,
            "native preference must not override explicit Fractional priorities");
    }
    bridgeProfile.adaptive_fractional_real_frame_priority =
        ls::AdaptiveFractionalRealFramePriority::Auto;
    for (const uint32_t cap : {90U, 100U, 144U}) {
        bridgeProfile.base_fps_cap = cap;
        const auto output = gamescopeBridgeOutputPolicy(bridgeProfile, 120, nativeScheduler);
        expect(output.outputFps == 120 && !output.generatedTimeline &&
                effectiveBaseFpsCap(bridgeProfile, nativeScheduler) == cap,
            "target-capable manual caps must release native output timing without releasing the cap");
        const auto unqualified = gamescopeBridgeOutputPolicy(bridgeProfile, 120);
        expect(unqualified.outputFps == 90 && unqualified.generatedTimeline,
            "a high manual cap must still require qualified native cadence");
    }
    for (const uint32_t cap : {45U, 89U}) {
        bridgeProfile.base_fps_cap = cap;
        const auto output = gamescopeBridgeOutputPolicy(bridgeProfile, 120, nativeScheduler);
        expect(output.outputFps == 90 && output.generatedTimeline,
            "a cap changed below target must restore generated timing despite an old native snapshot");
    }
    bridgeProfile.base_fps_cap = 0;
    bridgeProfile.adaptive_auto_base_fps_cap = true;
    expect(gamescopeBridgeOutputPolicy(bridgeProfile, 120, nativeScheduler).outputFps == 90,
        "Steady Adaptive must retain its existing output policy");
    bridgeProfile.adaptive = false;
    bridgeProfile.multiplier = 3;
    bridgeProfile.base_fps_cap = 30;
    expect(gamescopeBridgeOutputPolicy(bridgeProfile, 120, nativeScheduler).outputFps == 90,
        "bridge must retain exact Fixed below-refresh output");
    bridgeProfile.base_fps_cap = 0;
    expect(gamescopeBridgeOutputPolicy(bridgeProfile, 144).outputFps == 144,
        "uncapped Fixed must follow refresh without borrowing Adaptive target");
    bridgeProfile.frame_generation_enabled = false;
    bridgeProfile.adaptive = true;
    bridgeProfile.target_fps = 30;
    expect(gamescopeBridgeOutputPolicy(bridgeProfile, 120).outputFps == 120 &&
            !gamescopeBridgeOutputPolicy(bridgeProfile, 120).generatedTimeline,
        "live disabled generation must release its output ceiling");
    bridgeProfile.frame_generation_enabled = true;
    // The existing source-cap transition must rebuild native eligibility in
    // both directions; no separate live-update or cap owner is needed.
    auto belowTargetCap = adaptiveProfile();
    belowTargetCap.frame_generation_provisioned = true;
    belowTargetCap.target_fps = 60;
    belowTargetCap.base_fps_cap = 30;
    auto aboveTargetCap = belowTargetCap;
    aboveTargetCap.base_fps_cap = 90;
    for (const bool allowNative : {false, true}) {
        const auto changedCap = planProfileUpdate(
            allowNative ? belowTargetCap : aboveTargetCap,
            allowNative ? aboveTargetCap : belowTargetCap, 4, true);
        expect(changedCap.decision.baseFpsCapChanged &&
                generationSchedulerPolicy(changedCap.appliedProfile)->
                    qualifyTargetNativeCadence == allowNative,
            "live cap changes must rebuild the scheduler with the new native eligibility");
    }
    belowTargetCap.frame_generation_enabled = false;
    aboveTargetCap.frame_generation_enabled = false;
    const auto dormantNativeOwnership = planProfileUpdate(
        belowTargetCap, aboveTargetCap, 4, true);
    expect(dormantNativeOwnership.decision.action == ProfileUpdateAction::NoRuntimeChange,
        "disabled generation must leave native qualification edits dormant");

    expect(!profileUpdateInvalidatesTransientGenerationRecovery({}) &&
            profileUpdateInvalidatesTransientGenerationRecovery({
                .generationPolicyChanged = true,
            }) &&
            profileUpdateInvalidatesTransientGenerationRecovery({
                .generationModeChanged = true,
            }) &&
            profileUpdateInvalidatesTransientGenerationRecovery({
                .fixedMultiplierChanged = true,
            }) &&
            profileUpdateInvalidatesTransientGenerationRecovery({
                .baseFpsCapChanged = true,
            }) &&
            !profileUpdateInvalidatesTransientGenerationRecovery({
                .dynamicCadenceProbeIntervalChanged = true,
            }),
        "Only workload-shaping live policy changes must invalidate transient recovery");

    const auto current = adaptiveProfile();
    auto staticRequest = current;
    staticRequest.gpu = "1002:744c";
    staticRequest.ultra_performance = true;
    staticRequest.flow_scale = ls::GameConfDefaults::ultraPerformanceFlowScale;
    staticRequest.performance_mode = true;
    staticRequest.target_fps = 120;
    staticRequest.frame_generation_provisioned = false;
    staticRequest.swapchain_image_count_compatibility = true;
    const auto staticProjection = projectProcessStaticProfileForLiveUpdate(current, staticRequest, current.frame_generation_provisioned, current.swapchain_image_count_compatibility);
    expect(staticProjection.restartRequired() &&
            staticProjection.gpuSelectionPending &&
            staticProjection.frameGenerationProvisioningPending &&
            staticProjection.ultraPerformancePending &&
            staticProjection.swapchainImageCountCompatibilityPending &&
            staticProjection.runtimeProfile.gpu == current.gpu &&
            staticProjection.runtimeProfile.frame_generation_provisioned ==
                current.frame_generation_provisioned &&
            staticProjection.runtimeProfile.ultra_performance ==
                current.ultra_performance &&
            staticProjection.runtimeProfile.flow_scale == current.flow_scale &&
            staticProjection.runtimeProfile.performance_mode ==
                current.performance_mode &&
            staticProjection.runtimeProfile
                .swapchain_image_count_compatibility ==
                    current.swapchain_image_count_compatibility &&
            staticProjection.runtimeProfile.target_fps == 120,
        "Process-static projection must retain construction fields and apply live policy");
    auto scalingOnlyCurrent = current;
    scalingOnlyCurrent.frame_generation_enabled = false;
    const auto liveEnableProjection = projectProcessStaticProfileForLiveUpdate(scalingOnlyCurrent, current, scalingOnlyCurrent.frame_generation_provisioned, scalingOnlyCurrent.swapchain_image_count_compatibility);
    expect(!liveEnableProjection.restartRequired() &&
            liveEnableProjection.runtimeProfile.frame_generation_enabled,
        "A matched process must preserve live Frame Generation enablement");

    auto backendProfile = current;
    backendProfile.gpu = "1002:164e";
    auto requestedBackendProfile = current;
    requestedBackendProfile.gpu = "1002:15bf";
    const auto existingBackendProfile = profileForExistingBackend(
        requestedBackendProfile, backendProfile
    );
    expect(existingBackendProfile.gpu == backendProfile.gpu,
        "A swapchain recreation lost the process-wide backend GPU identity");

    const ls::GlobalConf backendGlobal{
        .dll = "/first/Lossless.dll",
        .allow_fp16 = true,
    };
    auto requestedGlobal = backendGlobal;
    requestedGlobal.allow_fp16 = false;
    expect(backendGlobalChangePending(backendGlobal, requestedGlobal),
        "A process-static global change was not retained as pending");
    expect(!backendGlobalChangePending(backendGlobal, backendGlobal) &&
            !backendGlobalChangePending(std::nullopt, requestedGlobal),
        "An applied or not-yet-constructed backend was marked pending");
    expect(backendProfileChangePending(
            backendProfile, requestedBackendProfile),
        "A process-static profile change was not retained as pending");
    expect(!backendProfileChangePending(backendProfile, backendProfile) &&
            !backendProfileChangePending(
                std::nullopt, requestedBackendProfile),
        "An applied or not-yet-constructed profile was marked pending");


    const auto adaptiveSchedulerPolicy = generationSchedulerPolicy(current);
    expect(adaptiveSchedulerPolicy &&
            adaptiveSchedulerPolicy->targetFps == current.target_fps &&
            adaptiveSchedulerPolicy->maximumMultiplier ==
                current.adaptive_max_multiplier &&
            adaptiveSchedulerPolicy->nearTargetNativePreference,
        "Adaptive must retain its configured target when refresh is available");

    auto fractionalSmooth = current;
    fractionalSmooth.adaptive_auto_base_fps_cap = false;
    fractionalSmooth.adaptive_stable_cadence = true;
    const auto fractionalSmoothPolicy = generationSchedulerPolicy(fractionalSmooth);
    expect(fractionalSmoothPolicy && fractionalSmoothPolicy->stableCadence,
        "Fractional Adaptive must pass Smooth Cadence into its scheduler");

    auto steadyPacing = current;
    steadyPacing.target_fps = 120;
    steadyPacing.adaptive_auto_base_fps_cap = true;
    steadyPacing.adaptive_stable_cadence = true;
    AdaptiveSchedulerSnapshot acceptedTwoX{
        .phase = AdaptiveSchedulerPhase::StableCadence,
        .generationLimit = 1,
        .validatedGenerationLimit = 1,
        .stableCadenceLimit = 1,
        .stableCadenceEvaluationActive = false,
        .smoothedBaseFps = 60.0,
    };
    expect(smoothCadencePacerHandoffGenerationLimit(
            steadyPacing, true, false, 120, acceptedTwoX) == 1,
        "accepted target-matched Steady 2x did not hand pacing to ordered FIFO");
    expect(!smoothCadencePacerHandoffGenerationLimit(
            steadyPacing, true, false, 90, acceptedTwoX),
        "target-mismatched refresh incorrectly bypassed the Steady base cap");
    acceptedTwoX.stableCadenceEvaluationActive = true;
    expect(!smoothCadencePacerHandoffGenerationLimit(
            steadyPacing, true, false, 120, acceptedTwoX),
        "unproven Smooth Cadence evaluation bypassed the Steady base cap");
    acceptedTwoX.stableCadenceEvaluationActive = false;
    expect(!smoothCadencePacerHandoffGenerationLimit(
            steadyPacing, true, true, 120, acceptedTwoX),
        "ordered-acquire recovery did not restore the Steady base cap");
    expect(!smoothCadencePacerHandoffGenerationLimit(
            steadyPacing, false, false, 120, acceptedTwoX),
        "non-ordered transport received an ordered-FIFO pacing handoff");
    acceptedTwoX.smoothedBaseFps = 62.0;
    expect(!smoothCadencePacerHandoffGenerationLimit(
            steadyPacing, true, false, 120, acceptedTwoX),
        "Steady pacing handoff remained active outside its target window");
    expect(smoothCadencePacerHandoffGenerationLimit(
            steadyPacing, true, false, 120, acceptedTwoX, 1) == 1,
        "fixed-refresh handoff ignored its retained scheduler qualification");
    {
        using namespace std::chrono_literals;
        // Replay both initial turnover and later estimator jitter. Entry
        // remains strict; accepted cadence belongs to the scheduler's
        // sustained exit policy, not a second instantaneous threshold.
        SmoothCadencePacerHandoff handoff;
        const auto start = SmoothCadencePacerHandoff::TimePoint{10s};
        auto snapshot = acceptedTwoX;
        const auto step = [&](auto elapsed, double fps, bool pressure = false) {
            snapshot.smoothedBaseFps = fps;
            const auto now = start + elapsed;
            return handoff.update(now, smoothCadencePacerHandoffGenerationLimit(
                steadyPacing, true, pressure, 120, snapshot,
                handoff.activeGenerationLimit()));
        };
        expect(!step(0ms, 68.5929).active,
            "handoff retention admitted an unqualified initial cadence");
        expect(step(1ms, 60).active && step(17ms, 68.5929).active &&
                step(100ms, 62.36).active && step(251ms, 60).active &&
                step(1001ms, 60).active,
            "handoff queue turnover caused a spurious minute-long cooldown");
        expect(step(1100ms, 58.3101).active && step(2100ms, 60).active,
            "post-settling jitter invented a failed FIFO lifetime");
        snapshot.phase = AdaptiveSchedulerPhase::Active;
        snapshot.stableCadenceLimit.reset();
        expect(!step(2200ms, 54).active,
            "lost scheduler qualification did not release fixed-refresh FIFO");
        snapshot.phase = AdaptiveSchedulerPhase::StableCadence;
        snapshot.stableCadenceLimit = 1;
        expect(!step(2300ms, 60).active,
            "a genuine failed handoff bypassed its retry delay");
        handoff.reset();
        expect(step(0ms, 60).active && !step(10ms, 60, true).active,
            "cadence retention delayed the acquire-recovery safety exit");
        handoff.reset();
        expect(step(0ms, 60).active, "handoff did not restart after reset");
        snapshot.stableCadenceEvaluationActive = true;
        expect(!step(10ms, 60).active,
            "cadence retention bypassed an active scheduler probe");
    }
    acceptedTwoX.smoothedBaseFps = 60.0;
    steadyPacing.adaptive_auto_base_fps_cap = false;
    expect(!smoothCadencePacerHandoffGenerationLimit(
            steadyPacing, true, false, 120, acceptedTwoX),
        "Fractional Adaptive incorrectly bypassed a nonexistent Steady cap");
    steadyPacing.adaptive_auto_base_fps_cap = true;
    expect(smoothCadenceBaseCapEligible(
            steadyPacing, true, false, 120),
        "target-matched ordered Steady mode did not enable integer-cap qualification");
    expect(!smoothCadenceBaseCapEligible(
            steadyPacing, false, false, 120),
        "non-ordered transport enabled the Steady integer-cap ladder");
    expect(!smoothCadenceBaseCapEligible(
            steadyPacing, true, true, 120),
        "ordered-acquire recovery did not suspend the integer-cap ladder");
    expect(!smoothCadenceBaseCapEligible(
            steadyPacing, true, false, 90),
        "target-mismatched refresh enabled the Steady integer-cap ladder");
    steadyPacing.adaptive_stable_cadence = false;
    expect(!smoothCadenceBaseCapEligible(
            steadyPacing, true, false, 120),
        "Fractional policy enabled the Steady integer-cap ladder");
    steadyPacing.adaptive_stable_cadence = true;


    auto next = current;
    next.target_fps = 120;
    auto decision = classifyProfileUpdate(current, next, 3, true);
    expect(decision.action == ProfileUpdateAction::ApplyLive,
        "Adaptive target changes must apply without rebuilding GPU resources");
    expect(decision.generationPolicyChanged &&
            profileUpdateInvalidatesTransientGenerationRecovery(decision),
        "Adaptive target changes must reset scheduler policy and old recovery");

    next = current;
    next.adaptive_stable_cadence = true;
    decision = classifyProfileUpdate(current, next, 3, true);
    expect(decision.action == ProfileUpdateAction::ApplyLive &&
            profileUpdateInvalidatesTransientGenerationRecovery(decision),
        "Smooth Cadence must start a fresh recovery episode");

    next = current;
    next.dynamic_cadence_recovery = true;
    decision = classifyProfileUpdate(current, next, 3, true);
    expect(decision.action == ProfileUpdateAction::ApplyLive &&
            decision.generationPolicyChanged &&
            profileUpdateInvalidatesTransientGenerationRecovery(decision),
        "Adaptive Dynamic Cadence Recovery must start a fresh recovery episode");
    expect(dynamicCadenceRecoveryEnabled(next),
        "An uncapped Adaptive profile must allow dynamic cadence recovery");
    next.base_fps_cap = 30;
    expect(!dynamicCadenceRecoveryEnabled(next),
        "A base FPS cap must suppress native-cadence probes");
    next.base_fps_cap = 0;
    next.adaptive_auto_base_fps_cap = true;
    expect(!dynamicCadenceRecoveryEnabled(next),
        "Adaptive auto-cap must suppress native-cadence probes");
    next.adaptive_auto_base_fps_cap = false;
    next.adaptive = false;
    expect(dynamicCadenceRecoveryEnabled(next),
        "Fixed mode must allow global cadence recovery");

    auto fixedWithoutRecovery = current;
    fixedWithoutRecovery.adaptive = false;
    auto fixedWithRecovery = fixedWithoutRecovery;
    fixedWithRecovery.dynamic_cadence_recovery = true;
    decision = classifyProfileUpdate(
        fixedWithoutRecovery, fixedWithRecovery, 3, true
    );
    expect(decision.action == ProfileUpdateAction::ApplyLive &&
            decision.generationPolicyChanged &&
            profileUpdateInvalidatesTransientGenerationRecovery(decision),
        "Fixed Dynamic Cadence Recovery must start a fresh recovery episode");

    auto fasterRecovery = next;
    fasterRecovery.multiplier = 2;
    fasterRecovery.dynamic_cadence_probe_interval_seconds = 0.25F;
    next.multiplier = 2;
    decision = classifyProfileUpdate(next, fasterRecovery, 3, true);
    expect(decision.action == ProfileUpdateAction::ApplyLive &&
            decision.dynamicCadenceProbeIntervalChanged &&
            !decision.generationPolicyChanged,
        "Probe-interval changes must apply live without resetting policy");

    auto dormantRecoveryInterval = current;
    dormantRecoveryInterval.dynamic_cadence_probe_interval_seconds = 3;
    decision = classifyProfileUpdate(current, dormantRecoveryInterval, 3, true);
    expect(decision.action == ProfileUpdateAction::NoRuntimeChange &&
            !decision.dynamicCadenceProbeIntervalChanged,
        "A dormant probe interval must remain saved without disturbing runtime");

    next = current;
    next.adaptive_max_multiplier = 4;
    decision = classifyProfileUpdate(current, next, 3, true);
    expect(decision.action == ProfileUpdateAction::ApplyLive,
        "Adaptive 4x must use the capacity already reserved by Adaptive");
    decision = classifyProfileUpdate(current, next, 2, true);
    expect(decision.action == ProfileUpdateAction::DeferUntilSwapchainRecreation &&
            decision.generatedFrameCapacityExceeded &&
            liveProfileResourceRecreationAvailable(decision, true),
        "Adaptive capacity growth must request recreation when images are unavailable");
    expect(!liveProfileResourceRecreationAvailable(decision, false),
        "Adaptive capacity growth must require retained frame-generation resources");

    next = current;
    next.adaptive_max_multiplier = 5;
    decision = classifyProfileUpdate(current, next, 4, true);
    expect(decision.action == ProfileUpdateAction::ApplyLive,
        "Adaptive 5x must use four reserved generated-frame outputs");
    decision = classifyProfileUpdate(current, next, 3, true);
    expect(decision.action == ProfileUpdateAction::DeferUntilSwapchainRecreation &&
            decision.generatedFrameCapacityExceeded,
        "Adaptive 5x must recreate when its fourth generated output is unavailable");

    next = current;
    next.base_fps_cap = 60;
    decision = classifyProfileUpdate(current, next, 3, true);
    expect(decision.action == ProfileUpdateAction::ApplyLive,
        "Base FPS cap changes must not rebuild GPU resources");
    expect(decision.baseFpsCapChanged,
        "Base FPS cap changes must reset presentation timing");

    for (const auto target : {90U, 120U, 144U}) {
        for (const bool smooth : {false, true}) {
            auto fractional = adaptiveProfile();
            fractional.target_fps = target;
            fractional.base_fps_cap = target / 2;
            fractional.adaptive_stable_cadence = smooth;
            auto steady = fractional;
            steady.adaptive_auto_base_fps_cap = true;
            expect(effectiveBaseFpsCap(fractional) == effectiveBaseFpsCap(steady),
                "Live cadence-style regression must keep the numeric cap unchanged");
            for (const bool toSteady : {true, false}) {
                const auto& before = toSteady ? fractional : steady;
                const auto& after = toSteady ? steady : fractional;
                const auto plan = planProfileUpdate(before, after, 3, true);
                expect(plan.decision.action == ProfileUpdateAction::ApplyLive &&
                        plan.decision.generationPolicyChanged &&
                        !plan.decision.baseFpsCapChanged &&
                        !plan.decision.swapchainRecreationDeferred &&
                        !plan.decision.processRestartDeferred &&
                        profileUpdateInvalidatesTransientGenerationRecovery(
                            plan.decision) &&
                        plan.appliedProfile.adaptive_auto_base_fps_cap == toSteady,
                    "Equal-cap Fractional/Steady switches must reset live scheduler policy and recovery");
                expect(planProfileUpdate(plan.appliedProfile, after, 3, true)
                        .decision.action == ProfileUpdateAction::NoRuntimeChange,
                    "An applied cadence-style switch must not repeatedly reset policy");
            }
            fractional.frame_generation_enabled = false;
            steady.frame_generation_enabled = false;
            for (const bool toSteady : {true, false}) {
                const auto& before = toSteady ? fractional : steady;
                const auto& after = toSteady ? steady : fractional;
                const auto dormant = planProfileUpdate(before, after, 3, true);
                expect(dormant.decision.action == ProfileUpdateAction::NoRuntimeChange &&
                        dormant.appliedProfile.adaptive_auto_base_fps_cap == toSteady,
                    "Disabled generation must save the cadence style without resetting policy");
            }
            fractional.frame_generation_enabled = true;
            steady.frame_generation_enabled = true;
            fractional.adaptive = false;
            steady.adaptive = false;
            expect(planProfileUpdate(fractional, steady, 3, true)
                    .decision.action == ProfileUpdateAction::NoRuntimeChange,
                "Fixed must keep the saved Adaptive cadence style dormant");
        }
    }

    next = current;
    next.adaptive_auto_base_fps_cap = true;
    decision = classifyProfileUpdate(current, next, 3, true);
    expect(decision.action == ProfileUpdateAction::ApplyLive,
        "Adaptive auto-cap changes must apply without rebuilding resources");
    expect(decision.baseFpsCapChanged &&
            profileUpdateInvalidatesTransientGenerationRecovery(decision),
        "Adaptive Steady mode must reset presentation timing and old recovery");
    expect(effectiveBaseFpsCap(next) == 45.0,
        "Adaptive auto-cap did not derive half of the 90 FPS target");
    const AdaptiveSchedulerSnapshot collapsedAutomaticCap{
        .automaticBaseCapSuppressed = true,
    };
    expect(effectiveBaseFpsCap(next, collapsedAutomaticCap) == 0.0,
        "A proven Ordered-SDR collapse did not release the automatic cap");

    auto overlappingCaps = next;
    overlappingCaps.base_fps_cap = 30;
    expect(effectiveBaseFpsCap(overlappingCaps) == 45.0,
        "Adaptive auto-cap must take priority over the saved manual cap");

    auto resumedManualCap = overlappingCaps;
    resumedManualCap.adaptive_auto_base_fps_cap = false;
    expect(effectiveBaseFpsCap(resumedManualCap) == 30.0,
        "Disabling Adaptive auto-cap must restore the saved manual cap");
    expect(effectiveBaseFpsCap(
                resumedManualCap, collapsedAutomaticCap) == 30.0,
        "Scheduler headroom incorrectly released an explicit manual cap");
    decision = classifyProfileUpdate(
        overlappingCaps, resumedManualCap, 3, true
    );
    expect(decision.action == ProfileUpdateAction::ApplyLive &&
            decision.baseFpsCapChanged,
        "Restoring the manual cap must reset presentation timing live");

    auto fractionalPriority = resumedManualCap;
    fractionalPriority.target_fps = 120;
    fractionalPriority.adaptive_fractional_real_frame_priority =
        ls::AdaptiveFractionalRealFramePriority::Low;
    expect(effectiveBaseFpsCap(fractionalPriority) == 72.0,
        "Low Fractional priority did not select the stable 3:2 ratio");
    fractionalPriority.adaptive_fractional_real_frame_priority =
        ls::AdaptiveFractionalRealFramePriority::Medium;
    expect(effectiveBaseFpsCap(fractionalPriority) == 80.0,
        "Medium Fractional priority did not select the stable 2:1 ratio");
    fractionalPriority.adaptive_fractional_real_frame_priority =
        ls::AdaptiveFractionalRealFramePriority::High;
    expect(effectiveBaseFpsCap(fractionalPriority) == 90.0,
        "High Fractional priority did not select the stable 3:1 ratio");
    fractionalPriority.adaptive_fractional_real_frame_priority =
        ls::AdaptiveFractionalRealFramePriority::VeryHigh;
    expect(effectiveBaseFpsCap(fractionalPriority) == 96.0,
        "Very High Fractional priority did not select the stable 4:1 ratio");
    expect(fractionalRealFramePriorityActive(fractionalPriority),
        "An explicit Fractional priority was not recognized as cap owner");
    decision = classifyProfileUpdate(
        resumedManualCap, fractionalPriority, 3, true
    );
    expect(decision.action == ProfileUpdateAction::ApplyLive &&
            decision.baseFpsCapChanged,
        "Fractional priority changes must reset presentation timing live");

    fractionalPriority.adaptive_fractional_real_frame_priority =
        ls::AdaptiveFractionalRealFramePriority::Auto;
    expect(!fractionalRealFramePriorityActive(fractionalPriority) &&
            effectiveBaseFpsCap(fractionalPriority) == 30.0,
        "Automatic Fractional priority changed today's manual-cap behavior");
    fractionalPriority.adaptive = false;
    fractionalPriority.adaptive_fractional_real_frame_priority =
        ls::AdaptiveFractionalRealFramePriority::VeryHigh;
    expect(!fractionalRealFramePriorityActive(fractionalPriority) &&
            effectiveBaseFpsCap(fractionalPriority) == 30.0,
        "Dormant Fractional priority overrode Fixed mode's manual cap");
    auto fixedPriorityChange = fractionalPriority;
    fixedPriorityChange.adaptive_fractional_real_frame_priority =
        ls::AdaptiveFractionalRealFramePriority::Low;
    decision = classifyProfileUpdate(
        fractionalPriority, fixedPriorityChange, 3, true
    );
    expect(decision.action == ProfileUpdateAction::NoRuntimeChange,
        "Dormant Fractional priority triggered a Fixed runtime transition");

    auto oddTarget = next;
    oddTarget.target_fps = 165;
    expect(effectiveBaseFpsCap(oddTarget) == 82.5,
        "Adaptive auto-cap lost a fractional half-target cadence");

    auto fixedAutoCap = next;
    fixedAutoCap.adaptive = false;
    fixedAutoCap.base_fps_cap = 30;
    expect(effectiveBaseFpsCap(fixedAutoCap) == 30.0,
        "Adaptive auto-cap incorrectly overrode Fixed mode's manual cap");
    expect(effectiveBaseFpsCap(
                fixedAutoCap, collapsedAutomaticCap) == 30.0,
        "Adaptive scheduler headroom incorrectly changed Fixed pacing");

    next = current;
    next.frame_generation_provisioned = false;
    const auto provisioningPlan = planProfileUpdate(current, next, 3, true);
    expect(provisioningPlan.decision.action ==
                ProfileUpdateAction::DeferUntilProcessRestart &&
            provisioningPlan.decision.processRestartDeferred &&
            provisioningPlan.appliedProfile.frame_generation_provisioned &&
            provisioningPlan.appliedProfile.frame_generation_enabled,
        "Frame Generation provisioning must remain applied until restart");

    next = current;
    next.frame_generation_enabled = false;
    decision = classifyProfileUpdate(current, next, 3, true);
    expect(decision.action == ProfileUpdateAction::ApplyLive,
        "Turning generation off must not tear down the context");

    auto disabled = current;
    disabled.frame_generation_enabled = false;
    disabled.base_fps_cap = 30;
    expect(effectiveBaseFpsCap(disabled) == 0.0,
        "Frame Generation Off must make the saved Base FPS Cap dormant");
    auto enabledWithSavedCap = disabled;
    enabledWithSavedCap.frame_generation_enabled = true;
    decision = classifyProfileUpdate(disabled, enabledWithSavedCap, 3, true);
    expect(decision.action == ProfileUpdateAction::ApplyLive &&
            decision.baseFpsCapChanged &&
            effectiveBaseFpsCap(enabledWithSavedCap) == 30.0,
        "Turning generation back on must restore and reset the saved cap");
    decision = classifyProfileUpdate(disabled, current, 3, true);
    expect(decision.action == ProfileUpdateAction::ApplyLive,
        "Turning generation back on must reuse retained resources");
    decision = classifyProfileUpdate(disabled, current, 0, false);
    expect(decision.action == ProfileUpdateAction::DeferUntilProcessRestart &&
            decision.processRestartDeferred &&
            !liveProfileResourceRecreationAvailable(decision, false),
        "Turning generation on without resources must wait for process restart");
    auto unavailableEnableWithCap = current;
    unavailableEnableWithCap.base_fps_cap = 48;
    auto unavailableEnablePlan = planProfileUpdate(
        disabled, unavailableEnableWithCap, 0, false
    );
    expect(unavailableEnablePlan.decision.action ==
                ProfileUpdateAction::DeferUntilProcessRestart &&
            unavailableEnablePlan.decision.processRestartDeferred &&
            !unavailableEnablePlan.decision.baseFpsCapChanged &&
            !unavailableEnablePlan.appliedProfile.frame_generation_enabled &&
            unavailableEnablePlan.appliedProfile.base_fps_cap == 48 &&
            effectiveBaseFpsCap(unavailableEnablePlan.appliedProfile) == 0.0,
        "An unavailable generation enable must save its cap dormant until restart");

    next = current;
    next.flow_scale = 0.75F;
    decision = classifyProfileUpdate(current, next, 3, true);
    expect(decision.action ==
            ProfileUpdateAction::DeferUntilSwapchainRecreation &&
            decision.frameGenerationBackendChanged &&
            decision.swapchainRecreationDeferred,
        "Flow-scale changes alter backend model construction");
    expect(liveProfileResourceRecreationAvailable(decision, true) &&
            !liveProfileResourceRecreationAvailable(decision, false),
        "Flow-scale recreation must require retained frame-generation resources");

    next = current;
    next.performance_mode = true;
    decision = classifyProfileUpdate(current, next, 3, true);
    expect(decision.action ==
            ProfileUpdateAction::DeferUntilSwapchainRecreation &&
            decision.frameGenerationBackendChanged &&
            decision.swapchainRecreationDeferred,
        "Performance-mode changes alter backend model construction");

    auto mixedDeferred = current;
    mixedDeferred.flow_scale = 0.75F;
    mixedDeferred.base_fps_cap = 47;
    auto mixedPlan = planProfileUpdate(current, mixedDeferred, 3, true);
    expect(mixedPlan.decision.action == ProfileUpdateAction::ApplyLive &&
            mixedPlan.decision.swapchainRecreationDeferred &&
            mixedPlan.appliedProfile.flow_scale == current.flow_scale &&
            mixedPlan.appliedProfile.base_fps_cap == 47,
        "A pending flow-scale change blocked an unrelated live cap");

    auto laterLiveChange = mixedDeferred;
    laterLiveChange.target_fps = 144;
    mixedPlan = planProfileUpdate(
        mixedPlan.appliedProfile, laterLiveChange, 3, true
    );
    expect(mixedPlan.decision.action == ProfileUpdateAction::ApplyLive &&
            mixedPlan.decision.swapchainRecreationDeferred &&
            mixedPlan.appliedProfile.flow_scale == current.flow_scale &&
            mixedPlan.appliedProfile.target_fps == 144,
        "A retained recreation change blocked a later live target update");

    auto mixedDisabled = mixedDeferred;
    mixedDisabled.frame_generation_enabled = false;
    mixedPlan = planProfileUpdate(current, mixedDisabled, 3, true);
    expect(mixedPlan.decision.action == ProfileUpdateAction::ApplyLive &&
            !mixedPlan.appliedProfile.frame_generation_enabled &&
            mixedPlan.decision.swapchainRecreationDeferred,
        "A pending recreation change blocked the live generation-off switch");
    mixedPlan = planProfileUpdate(
        mixedPlan.appliedProfile, mixedDeferred, 3, true
    );
    expect(mixedPlan.decision.action == ProfileUpdateAction::ApplyLive &&
            mixedPlan.appliedProfile.frame_generation_enabled &&
            mixedPlan.decision.swapchainRecreationDeferred,
        "A pending recreation change blocked live generation re-enable");

    auto revertedDeferred = mixedDeferred;
    revertedDeferred.flow_scale = current.flow_scale;
    mixedPlan = planProfileUpdate(current, revertedDeferred, 3, true);
    expect(mixedPlan.decision.action == ProfileUpdateAction::ApplyLive &&
            !mixedPlan.decision.swapchainRecreationDeferred &&
            !mixedPlan.decision.processRestartDeferred,
        "Reverting a pending recreation field did not clear its deferral");

    auto gpuAndCap = current;
    gpuAndCap.gpu = "1002:164e";
    gpuAndCap.base_fps_cap = 55;
    mixedPlan = planProfileUpdate(current, gpuAndCap, 3, true);
    expect(mixedPlan.decision.action == ProfileUpdateAction::ApplyLive &&
            mixedPlan.decision.processRestartDeferred &&
            mixedPlan.appliedProfile.gpu == current.gpu &&
            mixedPlan.appliedProfile.base_fps_cap == 55,
        "A process-static GPU change blocked an unrelated live cap");

    auto gpuFlowAndCap = gpuAndCap;
    gpuFlowAndCap.flow_scale = 0.75F;
    mixedPlan = planProfileUpdate(current, gpuFlowAndCap, 3, true);
    expect(mixedPlan.decision.action == ProfileUpdateAction::ApplyLive &&
            mixedPlan.decision.swapchainRecreationDeferred &&
            mixedPlan.decision.processRestartDeferred &&
            mixedPlan.appliedProfile.gpu == current.gpu &&
            mixedPlan.appliedProfile.flow_scale == current.flow_scale &&
            mixedPlan.appliedProfile.base_fps_cap == 55,
        "Independent recreation and process deferrals were not both retained");

    next = current;
    next.ultra_performance = true;
    expect(ls::effectiveFlowScale(next) ==
            ls::GameConfDefaults::ultraPerformanceFlowScale &&
            ls::effectivePerformanceMode(next),
        "Ultra Performance must force 70% flow and the lighter model");
    decision = classifyProfileUpdate(current, next, 3, true);
    expect(decision.action ==
            ProfileUpdateAction::DeferUntilProcessRestart &&
            decision.processRestartDeferred &&
            !decision.swapchainRecreationDeferred &&
            !liveProfileResourceRecreationAvailable(decision, true),
        "Ultra Performance changes must never apply to a live context");

    auto runningUltra = current;
    runningUltra.ultra_performance = true;
    runningUltra.flow_scale = ls::GameConfDefaults::ultraPerformanceFlowScale;
    runningUltra.performance_mode = true;
    auto updatedRunningUltra = runningUltra;
    updatedRunningUltra.target_fps = 120;
    decision = classifyProfileUpdate(runningUltra, updatedRunningUltra, 2, true);
    expect(decision.action == ProfileUpdateAction::ApplyLive &&
            decision.generationPolicyChanged &&
            !decision.processRestartDeferred,
        "Compatible scheduler changes must remain live inside Ultra Performance");
    updatedRunningUltra.adaptive_max_multiplier = 4;
    decision = classifyProfileUpdate(runningUltra, updatedRunningUltra, 2, true);
    expect(decision.action == ProfileUpdateAction::ApplyLive &&
            decision.swapchainRecreationDeferred &&
            decision.generatedFrameCapacityExceeded &&
            liveProfileResourceRecreationAvailable(decision, true),
        "Ultra Performance capacity growth must preserve live policy while requesting recreation");

    LiveProfileResourceRecreation resourceRecreation;
    const auto resourceChangeStarted =
        LiveProfileResourceRecreation::Clock::time_point{};
    auto combinedFixedCurrent = current;
    combinedFixedCurrent.adaptive = false;
    combinedFixedCurrent.multiplier = 2;
    combinedFixedCurrent.adaptive_max_multiplier = 2;

    auto fixedCapacityOnlyRequest = combinedFixedCurrent;
    fixedCapacityOnlyRequest.multiplier = 3;
    const auto fixedCapacityOnlyPlan = planProfileUpdate(combinedFixedCurrent, fixedCapacityOnlyRequest, 1, true, true);
    expect(fixedCapacityOnlyPlan.decision.frameGenerationPrivateRebuild &&
            !guardedLiveProfileResourceRecreationAvailable(fixedCapacityOnlyPlan.decision, true, true),
        "A Fixed capacity-only change must remain private and never request game-owned recreation");

    auto flowScaleRequest = current;
    flowScaleRequest.flow_scale = 0.75F;
    const auto flowScaleDecision = planProfileUpdate(current, flowScaleRequest, 2, true, false).decision;
    expect(guardedLiveProfileResourceRecreationAvailable(flowScaleDecision, true, true),
        "The native context must retain the guarded fallback recreation path");

    auto frameGenerationBackendRequest = current;
    frameGenerationBackendRequest.flow_scale = 0.75F;
    frameGenerationBackendRequest.performance_mode = true;
    resourceRecreation.update(current, frameGenerationBackendRequest, 11,
        resourceChangeStarted + std::chrono::seconds(3));
    expect(resourceRecreation.armed() &&
            resourceRecreation.signalAfterSuccessfulPresent(
                resourceChangeStarted + std::chrono::seconds(3) +
                    LiveProfileResourceRecreation::quietPeriod) == 11,
        "Flow-scale and model changes must share the live recreation boundary");

    const auto privateBackendPlan = planProfileUpdate(current, frameGenerationBackendRequest, 2, true, true);
    expect(privateBackendPlan.decision.action ==
                ProfileUpdateAction::ApplyLive &&
            privateBackendPlan.decision.frameGenerationPrivateRebuild &&
            !privateBackendPlan.decision.swapchainRecreationDeferred &&
            privateBackendPlan.appliedProfile.flow_scale ==
                current.flow_scale &&
            privateBackendPlan.appliedProfile.performance_mode ==
                current.performance_mode,
        "Flow Scale and model changes must prepare private resources without marking them active early");

    auto capacityRequest = current;
    capacityRequest.adaptive_max_multiplier = 4;
    resourceRecreation.update(current, capacityRequest, 12,
        resourceChangeStarted + std::chrono::seconds(4));
    expect(resourceRecreation.armed() &&
            resourceRecreation.signalAfterSuccessfulPresent(
                resourceChangeStarted + std::chrono::seconds(4) +
                    LiveProfileResourceRecreation::quietPeriod) == 12,
        "Generated-capacity growth must share the live recreation boundary");

    next = current;
    next.frame_generation_enabled = false;
    next.flow_scale = 0.75F;
    next.performance_mode = true;
    decision = classifyProfileUpdate(current, next, 3, true);
    expect(decision.action == ProfileUpdateAction::ApplyLive &&
            decision.swapchainRecreationDeferred &&
            decision.frameGenerationBackendChanged &&
            decision.frameGenerationChanged,
        "A combined resource and frame-generation update must apply the safe subset and retain every deferral");

    auto equivalentUltra = current;
    equivalentUltra.flow_scale = ls::GameConfDefaults::ultraPerformanceFlowScale;
    equivalentUltra.performance_mode = true;
    auto equivalentUltraEnabled = equivalentUltra;
    equivalentUltraEnabled.ultra_performance = true;
    decision = classifyProfileUpdate(
        equivalentUltra, equivalentUltraEnabled, 3, true
    );
    expect(decision.action == ProfileUpdateAction::DeferUntilProcessRestart &&
            decision.processRestartDeferred &&
            !decision.swapchainRecreationDeferred,
        "Ultra Performance must require restart even when model settings already match");

    next = current;
    next.name = "Renamed";
    next.active_in = {"renamed-game"};
    expect(classifyProfileUpdate(current, next, 3, true).action ==
            ProfileUpdateAction::NoRuntimeChange,
        "Profile metadata must not disturb a running context");

    auto fixed = current;
    fixed.adaptive = false;
    fixed.multiplier = 2;

    auto fixedWithDormantFourX = fixed;
    fixedWithDormantFourX.adaptive_max_multiplier = 4;
    expect(classifyProfileUpdate(
            current, fixedWithDormantFourX, 2, true).action ==
            ProfileUpdateAction::ApplyLive,
        "Adaptive-to-Fixed must ignore dormant Adaptive capacity growth");
    expect(generatedFrameCapacityForActivePolicy(fixedWithDormantFourX) == 1,
        "Fixed 2x active capacity must ignore the dormant Adaptive ceiling");

    auto adaptiveFourX = fixedWithDormantFourX;
    adaptiveFourX.adaptive = true;
    expect(classifyProfileUpdate(
            fixedWithDormantFourX, adaptiveFourX, 2, true).action ==
            ProfileUpdateAction::DeferUntilSwapchainRecreation,
        "Fixed-to-Adaptive 4x must recreate when active capacity is unavailable");
    expect(classifyProfileUpdate(
            fixedWithDormantFourX, adaptiveFourX, 3, true).action ==
            ProfileUpdateAction::ApplyLive,
        "Fixed-to-Adaptive 4x must apply when active capacity is available");
    const auto privateCapacityPlan = planProfileUpdate(fixedWithDormantFourX, adaptiveFourX, 2, true, true);
    expect(privateCapacityPlan.decision.action ==
                ProfileUpdateAction::ApplyLive &&
            privateCapacityPlan.decision.frameGenerationPrivateRebuild &&
            privateCapacityPlan.decision.generatedFrameCapacityExceeded &&
            !privateCapacityPlan.decision.swapchainRecreationDeferred &&
            !privateCapacityPlan.appliedProfile.adaptive,
        "Generated-capacity growth must use a private rebuild while the old policy remains active");
    expect(generatedFrameCapacityForActivePolicy(adaptiveFourX) == 3,
        "Adaptive 4x active capacity was not selected");
    const bool privateCapacityFitsThreeImageSwapchain =
        generatedFrameCapacityForActivePolicy(adaptiveFourX) <=
            generatedFrameCapacitySupportedByExistingSwapchain(3, 3);
    const auto wsiConstrainedCapacityPlan = planProfileUpdate(fixedWithDormantFourX, adaptiveFourX, 2, true, privateCapacityFitsThreeImageSwapchain);
    expect(wsiConstrainedCapacityPlan.decision.action ==
                ProfileUpdateAction::DeferUntilSwapchainRecreation &&
            !wsiConstrainedCapacityPlan.decision.frameGenerationPrivateRebuild &&
            wsiConstrainedCapacityPlan.decision.generatedFrameCapacityExceeded &&
            wsiConstrainedCapacityPlan.decision.swapchainRecreationDeferred &&
            !wsiConstrainedCapacityPlan.appliedProfile.adaptive,
        "Live generated-capacity growth must defer when private resources would exceed existing WSI headroom");
    auto adaptiveTwoX = adaptiveFourX;
    adaptiveTwoX.adaptive_max_multiplier = 2;
    auto adaptiveThreeX = adaptiveTwoX;
    adaptiveThreeX.adaptive_max_multiplier = 3;
    const bool threeXCapacityFitsFiveImageSwapchain =
        generatedFrameCapacityForActivePolicy(adaptiveThreeX) <=
            generatedFrameCapacitySupportedByExistingSwapchain(3, 5);
    const auto exactLiveGrowthRegressionPlan = planProfileUpdate(adaptiveTwoX, adaptiveThreeX, 1, true, threeXCapacityFitsFiveImageSwapchain);
    expect(exactLiveGrowthRegressionPlan.decision.action ==
                ProfileUpdateAction::ApplyLive &&
            exactLiveGrowthRegressionPlan.decision.generatedFrameCapacityExceeded &&
            exactLiveGrowthRegressionPlan.decision.frameGenerationPrivateRebuild &&
            !exactLiveGrowthRegressionPlan.decision.swapchainRecreationDeferred &&
            exactLiveGrowthRegressionPlan.appliedProfile.adaptive_max_multiplier == 2,
        "A live 2x-to-3x request must fit a five-image pool with two generated outputs, rebuild privately, and retain the active policy until replacement completes");

    auto adaptiveFiveX = adaptiveThreeX;
    adaptiveFiveX.adaptive_max_multiplier = 5;
    const bool fiveXCapacityFitsFiveImageSwapchain =
        generatedFrameCapacityForActivePolicy(adaptiveFiveX) <=
            generatedFrameCapacitySupportedByExistingSwapchain(3, 5);
    const auto trueWsiGrowthBoundaryPlan = planProfileUpdate(adaptiveThreeX, adaptiveFiveX, 2, true, fiveXCapacityFitsFiveImageSwapchain);
    expect(trueWsiGrowthBoundaryPlan.decision.action ==
                ProfileUpdateAction::DeferUntilSwapchainRecreation &&
            trueWsiGrowthBoundaryPlan.decision.generatedFrameCapacityExceeded &&
            !trueWsiGrowthBoundaryPlan.decision.frameGenerationPrivateRebuild &&
            trueWsiGrowthBoundaryPlan.appliedProfile.adaptive_max_multiplier == 3,
        "A live 3x-to-5x request must wait when four generated outputs exceed a five-image pool's two-image headroom");

    auto generationDisabled = adaptiveFourX;
    generationDisabled.frame_generation_enabled = false;
    expect(generatedFrameCapacityForActivePolicy(generationDisabled) == 0,
        "Frame Generation Off must not claim generated-image capacity");
    auto disabledCapacityEdit = generationDisabled;
    disabledCapacityEdit.adaptive_max_multiplier = 5;
    const auto disabledCapacityPlan = planProfileUpdate(
        generationDisabled, disabledCapacityEdit, 0, false
    );
    expect(!disabledCapacityPlan.decision.generatedFrameCapacityExceeded &&
            !disabledCapacityPlan.decision.swapchainRecreationDeferred,
        "Frame Generation Off must not defer a dormant capacity edit");

    auto adaptiveFourXWithLiveTarget = adaptiveFourX;
    adaptiveFourXWithLiveTarget.target_fps = 120;
    adaptiveFourXWithLiveTarget.base_fps_cap = 49;
    mixedPlan = planProfileUpdate(
        fixedWithDormantFourX, adaptiveFourXWithLiveTarget, 2, true
    );
    expect(mixedPlan.decision.action == ProfileUpdateAction::ApplyLive &&
            mixedPlan.decision.swapchainRecreationDeferred &&
            !mixedPlan.appliedProfile.adaptive &&
            mixedPlan.appliedProfile.target_fps == 120 &&
            mixedPlan.appliedProfile.base_fps_cap == 49,
        "Unavailable Adaptive capacity blocked an unrelated live cap");

    auto unavailableFixedFourX = current;
    unavailableFixedFourX.adaptive = false;
    unavailableFixedFourX.multiplier = 4;
    mixedPlan = planProfileUpdate(current, unavailableFixedFourX, 2, true);
    expect(mixedPlan.decision.action ==
                ProfileUpdateAction::DeferUntilSwapchainRecreation &&
            mixedPlan.decision.swapchainRecreationDeferred &&
            mixedPlan.appliedProfile.adaptive &&
            mixedPlan.appliedProfile.multiplier == 4,
        "A dormant Fixed multiplier was misclassified as active while its mode switch waited");

    next = current;
    expect(classifyProfileUpdate(fixed, next, 1, true).action ==
            ProfileUpdateAction::DeferUntilSwapchainRecreation,
        "Fixed-to-Adaptive must recreate when the fixed context lacks Adaptive capacity");
    const auto fixedToAdaptive = classifyProfileUpdate(
        fixed, next, 2, true
    );
    expect(fixedToAdaptive.action == ProfileUpdateAction::ApplyLive &&
            profileUpdateInvalidatesTransientGenerationRecovery(
                fixedToAdaptive),
        "Fixed-to-Adaptive must apply live with fresh recovery when shared capacity is available");

    next = fixed;
    next.multiplier = 3;
    expect(classifyProfileUpdate(fixed, next, 1, true).action ==
            ProfileUpdateAction::DeferUntilSwapchainRecreation,
        "Fixed multiplier growth must recreate when shared capacity is unavailable");
    expect(classifyProfileUpdate(fixed, next, 2, true).action ==
            ProfileUpdateAction::ApplyLive,
        "Fixed multiplier changes must apply live within shared capacity");

    auto fixedThreeX = fixed;
    fixedThreeX.multiplier = 3;
    const auto fixedThreeToTwo = planProfileUpdate(
        fixedThreeX, fixed, 2, true
    );
    expect(fixedThreeToTwo.decision.action == ProfileUpdateAction::ApplyLive &&
            fixedThreeToTwo.decision.fixedMultiplierChanged &&
            profileUpdateInvalidatesTransientGenerationRecovery(
                fixedThreeToTwo.decision) &&
            !fixedThreeToTwo.decision.swapchainRecreationDeferred &&
            fixedThreeToTwo.appliedProfile.multiplier == 2,
        "Fixed 3x-to-2x must apply live with fresh recovery and no recreation");

    expect(generatedFrameCapacityForProfile(fixed) == 2,
        "Fixed 2x should reserve the configured Adaptive 3x capacity");
    auto unprovisioned = fixed;
    unprovisioned.frame_generation_provisioned = false;
    expect(generatedFrameCapacityForProfile(unprovisioned) == 0 &&
            generatedFrameCapacityForActivePolicy(unprovisioned) == 0 &&
            !effectiveFrameGenerationEnabled(unprovisioned),
        "an unprovisioned profile must own no generated-frame work");
    auto fixedUltra = fixed;
    fixedUltra.ultra_performance = true;
    expect(generatedFrameCapacityForProfile(fixedUltra) == 1,
        "Fixed 2x Ultra Performance must allocate only its active output");
    fixedUltra.adaptive = true;
    expect(generatedFrameCapacityForProfile(fixedUltra) == 2,
        "Adaptive 3x Ultra Performance must allocate only its active ceiling");
    expect(fixedGeneratedFrameCount(2, 2) == 1,
        "Fixed 2x must schedule one frame even with two reserved outputs");
    expect(fixedGeneratedFrameCount(3, 2) == 2,
        "Fixed 3x must use two reserved outputs");
    fixed.multiplier = 4;
    expect(generatedFrameCapacityForProfile(fixed) == 3,
        "Fixed 4x should reserve its larger Fixed capacity");
    expect(fixedGeneratedFrameCount(4, 3) == 3,
        "Fixed 4x must use three reserved outputs");
    fixed.multiplier = 5;
    expect(generatedFrameCapacityForProfile(fixed) == 4,
        "Fixed 5x should reserve its fourth generated output");
    expect(fixedGeneratedFrameCount(5, 4) == 4,
        "Fixed 5x must use four reserved outputs");

    std::cout << "profile update tests passed\n";
    return 0;
}

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

    ls::GameConf fixedProfile() {
        return {
            .name = "Fixed",
            .active_in = {"game"},
            .multiplier = 2,
            .frame_generation_enabled = true,
            .base_fps_cap = 0,
            .flow_scale = 1.0F,
            .performance_mode = false,
            .pacing = ls::Pacing::None,
        };
    }
}

int main() {
    auto cappedFixedPacing = fixedProfile();
    cappedFixedPacing.frame_generation_provisioned = true;
    for (const int multiplier : {2, 3, 4, 5}) {
        cappedFixedPacing.multiplier = multiplier;
        cappedFixedPacing.base_fps_cap = 120 / multiplier;
        expect(baseCapPacesOutputs(cappedFixedPacing,
                cappedFixedPacing.base_fps_cap, true, 120),
            "capped Fixed output pacing must follow the saved cap and refresh");
    }
    cappedFixedPacing.multiplier = 2;
    cappedFixedPacing.base_fps_cap = 30;
    for (const int cap : {0, 30, 60, 610}) {
        auto direct = cappedFixedPacing;
        direct.base_fps_cap = cap;
        expect(!fixedReadyOutputPacingEligible(direct, true, false, false),
            "a non-detached Fixed output added ready-output waits");
        expect(!fixedReadyOutputPacingEligible(direct, false, false, false) &&
                !fixedReadyOutputPacingEligible(direct, true, true, false) &&
                !fixedReadyOutputPacingEligible(direct, true, false, true),
            "ready-output pacing replaced an ineligible transport, explicit timing or recovery");
        expect(fixedReadyOutputPacingEligible(direct, true, false, false, true),
            "detached Fixed output stayed unspaced");
        expect(!fixedReadyOutputPacingEligible(direct, false, false, false, true) &&
                !fixedReadyOutputPacingEligible(direct, true, true, false, true) &&
                !fixedReadyOutputPacingEligible(direct, true, false, true, true),
            "detached spacing replaced an ineligible transport, explicit timing or recovery");
        direct.frame_generation_enabled = false;
        expect(!fixedReadyOutputPacingEligible(direct, true, false, false), "generation Off added output waits");
        expect(!fixedReadyOutputPacingEligible(direct, true, false, false, true), "generation Off added detached pacing");
    }
    // Minecraft: game-owned limiter, and a deliberately nonbinding 610 FPS
    // cap. Exercise qualification and ownership together.
    for (const int cap : {0, 610, 60}) {
        using Clock = RealFramePacer::Clock;
        using namespace std::chrono_literals;
        auto native = cappedFixedPacing;
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
                        native, cap, true, {}, true)) {
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
    expect(baseCapPacesOutputs(cappedFixedPacing, 30., true, {}, true) &&
            baseCapPacesOutputs(cappedFixedPacing, 30., true, 50, true),
        "VSync-off Fixed output spacing must not depend on a refresh ceiling");
    auto nativeFifo = cappedFixedPacing;
    expect(baseCapPacesOutputs(nativeFifo, 30., true, {}, false, true),
        "Native FIFO must prepare within the cap");
    for (const uint32_t multiplier : {2U, 3U, 4U, 5U}) {
        nativeFifo.multiplier = multiplier;
        expect(baseCapPacesOutputs(nativeFifo, 30., true, {}, false, true, true),
            "caller real-frame targets must not turn an active native Fixed cap into generated/real bursts");
    }
    nativeFifo.base_fps_cap = 0;
    expect(!baseCapPacesOutputs(nativeFifo, 30., true, {}, false, true, true),
        "caller timing must not invent an unconfigured source cap");
    nativeFifo.base_fps_cap = cappedFixedPacing.base_fps_cap;
    expect(!baseCapPacesOutputs(nativeFifo, 30., false, {}, false, true) &&
            !baseCapPacesOutputs(nativeFifo, 0., true, {}, false, true),
        "Native FIFO cap preparation must preserve timing ownership and require a cap");
    expect(!baseCapPacesOutputs(cappedFixedPacing, 30., false, {}, true) &&
            !baseCapPacesOutputs(cappedFixedPacing, 0., true, {}, true),
        "VSync-off pacing must preserve application timing and require a source cap");
    expect(!baseCapPacesOutputs(cappedFixedPacing, 30., true, {}, false, false, true) &&
            !baseCapPacesOutputs(cappedFixedPacing, 30., true, 120, false, false, true),
        "caller timing must retain non-FIFO and compositor output ownership");
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
    cappedFixedPacing.frame_generation_enabled = false;
    expect(!baseCapPacesOutputs(cappedFixedPacing, 30., true, 120),
        "disabled Fixed generation must not acquire output pacing");
    cappedFixedPacing.frame_generation_enabled = true;

    auto bridgeProfile = ls::GameConf{};
    bridgeProfile.frame_generation_provisioned = true;
    bridgeProfile.frame_generation_enabled = true;
    bridgeProfile.multiplier = 2;
    bridgeProfile.base_fps_cap = 0;
    expect(gamescopeBridgeOutputPolicy(bridgeProfile, 120).outputFps == 120 &&
            gamescopeBridgeOutputPolicy(bridgeProfile, 120).generatedTimeline,
        "uncapped Fixed bridge must follow the display refresh on the generated timeline");
    bridgeProfile.base_fps_cap = 30;
    expect(gamescopeBridgeOutputPolicy(bridgeProfile, 120).outputFps == 60 &&
            gamescopeBridgeOutputPolicy(bridgeProfile, 120).generatedTimeline,
        "a Fixed 2x cap must bound the bridge output to cap times multiplier");
    expect(gamescopeBridgeOutputPolicy(bridgeProfile, 60).outputFps == 60,
        "a Fixed bridge output must not exceed the display refresh");
    bridgeProfile.multiplier = 3;
    expect(gamescopeBridgeOutputPolicy(bridgeProfile, 120).outputFps == 90,
        "bridge must retain exact Fixed below-refresh output");
    bridgeProfile.multiplier = 2;
    bridgeProfile.base_fps_cap = 0;
    expect(gamescopeBridgeOutputPolicy(bridgeProfile, 144).outputFps == 144,
        "uncapped Fixed must follow refresh");
    bridgeProfile.frame_generation_enabled = false;
    expect(gamescopeBridgeOutputPolicy(bridgeProfile, 120).outputFps == 120 &&
            !gamescopeBridgeOutputPolicy(bridgeProfile, 120).generatedTimeline,
        "live disabled generation must release its output ceiling");
    bridgeProfile.frame_generation_enabled = true;

    // The existing source-cap transition must apply in both directions while
    // generation is effective, and a dormant cap edit must not act.
    auto belowTargetCap = fixedProfile();
    belowTargetCap.frame_generation_provisioned = true;
    belowTargetCap.base_fps_cap = 30;
    auto aboveTargetCap = belowTargetCap;
    aboveTargetCap.base_fps_cap = 90;
    for (const bool raise : {false, true}) {
        const auto changedCap = planProfileUpdate(
            raise ? belowTargetCap : aboveTargetCap,
            raise ? aboveTargetCap : belowTargetCap, 4, true);
        expect(changedCap.decision.baseFpsCapChanged &&
                changedCap.decision.action == ProfileUpdateAction::ApplyLive,
            "live cap changes must reset the Fixed presentation timing");
    }
    belowTargetCap.frame_generation_enabled = false;
    aboveTargetCap.frame_generation_enabled = false;
    const auto dormantCapOwnership = planProfileUpdate(
        belowTargetCap, aboveTargetCap, 4, true);
    expect(dormantCapOwnership.decision.action == ProfileUpdateAction::NoRuntimeChange,
        "disabled generation must leave cap edits dormant");

    expect(!profileUpdateInvalidatesTransientGenerationRecovery({}) &&
            profileUpdateInvalidatesTransientGenerationRecovery({
                .fixedMultiplierChanged = true,
            }) &&
            profileUpdateInvalidatesTransientGenerationRecovery({
                .baseFpsCapChanged = true,
            }),
        "Only workload-shaping live policy changes must invalidate transient recovery");

    const auto current = fixedProfile();
    auto staticRequest = current;
    staticRequest.gpu = "1002:744c";
    staticRequest.ultra_performance = true;
    staticRequest.flow_scale = ls::GameConfDefaults::ultraPerformanceFlowScale;
    staticRequest.performance_mode = true;
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
                    current.swapchain_image_count_compatibility,
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

    auto next = current;
    next.multiplier = 3;
    auto decision = classifyProfileUpdate(current, next, 3, true);
    expect(decision.action == ProfileUpdateAction::ApplyLive &&
            decision.fixedMultiplierChanged &&
            profileUpdateInvalidatesTransientGenerationRecovery(decision),
        "Fixed multiplier changes must apply live with a fresh recovery episode");
    expect(generatedFrameCapacityForActivePolicy(next) == 2,
        "Fixed 3x active capacity must be exactly two generated outputs");

    next = current;
    next.base_fps_cap = 60;
    decision = classifyProfileUpdate(current, next, 3, true);
    expect(decision.action == ProfileUpdateAction::ApplyLive,
        "Base FPS cap changes must not rebuild GPU resources");
    expect(decision.baseFpsCapChanged,
        "Base FPS cap changes must reset presentation timing");

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
    laterLiveChange.base_fps_cap = 144;
    mixedPlan = planProfileUpdate(
        mixedPlan.appliedProfile, laterLiveChange, 3, true
    );
    expect(mixedPlan.decision.action == ProfileUpdateAction::ApplyLive &&
            mixedPlan.decision.swapchainRecreationDeferred &&
            mixedPlan.appliedProfile.flow_scale == current.flow_scale &&
            mixedPlan.appliedProfile.base_fps_cap == 144,
        "A retained recreation change blocked a later live cap update");

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
    updatedRunningUltra.base_fps_cap = 60;
    decision = classifyProfileUpdate(runningUltra, updatedRunningUltra, 2, true);
    expect(decision.action == ProfileUpdateAction::ApplyLive &&
            decision.baseFpsCapChanged &&
            !decision.processRestartDeferred,
        "Compatible cap changes must remain live inside Ultra Performance");
    updatedRunningUltra.multiplier = 4;
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
    combinedFixedCurrent.multiplier = 2;

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
    capacityRequest.multiplier = 4;
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
    fixed.multiplier = 2;

    auto fixedFourX = fixed;
    fixedFourX.multiplier = 4;
    expect(generatedFrameCapacityForActivePolicy(fixed) == 1,
        "Fixed 2x active capacity must be one generated output");
    expect(generatedFrameCapacityForActivePolicy(fixedFourX) == 3,
        "Fixed 4x active capacity was not selected");
    const auto privateCapacityPlan = planProfileUpdate(fixed, fixedFourX, 2, true, true);
    expect(privateCapacityPlan.decision.action ==
                ProfileUpdateAction::ApplyLive &&
            privateCapacityPlan.decision.frameGenerationPrivateRebuild &&
            privateCapacityPlan.decision.generatedFrameCapacityExceeded &&
            !privateCapacityPlan.decision.swapchainRecreationDeferred &&
            privateCapacityPlan.appliedProfile.multiplier == 2,
        "Generated-capacity growth must use a private rebuild while the old multiplier remains active");
    const bool privateCapacityFitsThreeImageSwapchain =
        generatedFrameCapacityForActivePolicy(fixedFourX) <=
            generatedFrameCapacitySupportedByExistingSwapchain(3, 3);
    const auto wsiConstrainedCapacityPlan = planProfileUpdate(fixed, fixedFourX, 2, true, privateCapacityFitsThreeImageSwapchain);
    expect(wsiConstrainedCapacityPlan.decision.action ==
                ProfileUpdateAction::DeferUntilSwapchainRecreation &&
            !wsiConstrainedCapacityPlan.decision.frameGenerationPrivateRebuild &&
            wsiConstrainedCapacityPlan.decision.generatedFrameCapacityExceeded &&
            wsiConstrainedCapacityPlan.decision.swapchainRecreationDeferred &&
            wsiConstrainedCapacityPlan.appliedProfile.multiplier == 2,
        "Live generated-capacity growth must defer when private resources would exceed existing WSI headroom");

    auto fixedThree = fixed;
    fixedThree.multiplier = 3;
    const bool threeXCapacityFitsFiveImageSwapchain =
        generatedFrameCapacityForActivePolicy(fixedThree) <=
            generatedFrameCapacitySupportedByExistingSwapchain(3, 5);
    const auto exactLiveGrowthRegressionPlan = planProfileUpdate(fixed, fixedThree, 1, true, threeXCapacityFitsFiveImageSwapchain);
    expect(exactLiveGrowthRegressionPlan.decision.action ==
                ProfileUpdateAction::ApplyLive &&
            exactLiveGrowthRegressionPlan.decision.generatedFrameCapacityExceeded &&
            exactLiveGrowthRegressionPlan.decision.frameGenerationPrivateRebuild &&
            !exactLiveGrowthRegressionPlan.decision.swapchainRecreationDeferred &&
            exactLiveGrowthRegressionPlan.appliedProfile.multiplier == 2,
        "A live 2x-to-3x request must fit a five-image pool with two generated outputs, rebuild privately, and retain the active multiplier until replacement completes");

    auto fixedFive = fixedThree;
    fixedFive.multiplier = 5;
    const bool fiveXCapacityFitsFiveImageSwapchain =
        generatedFrameCapacityForActivePolicy(fixedFive) <=
            generatedFrameCapacitySupportedByExistingSwapchain(3, 5);
    const auto trueWsiGrowthBoundaryPlan = planProfileUpdate(fixedThree, fixedFive, 2, true, fiveXCapacityFitsFiveImageSwapchain);
    expect(trueWsiGrowthBoundaryPlan.decision.action ==
                ProfileUpdateAction::DeferUntilSwapchainRecreation &&
            trueWsiGrowthBoundaryPlan.decision.generatedFrameCapacityExceeded &&
            !trueWsiGrowthBoundaryPlan.decision.frameGenerationPrivateRebuild &&
            trueWsiGrowthBoundaryPlan.appliedProfile.multiplier == 3,
        "A live 3x-to-5x request must wait when four generated outputs exceed a five-image pool's two-image headroom");

    auto generationDisabled = fixedFourX;
    generationDisabled.frame_generation_enabled = false;
    expect(generatedFrameCapacityForActivePolicy(generationDisabled) == 0,
        "Frame Generation Off must not claim generated-image capacity");
    auto disabledCapacityEdit = generationDisabled;
    disabledCapacityEdit.multiplier = 5;
    const auto disabledCapacityPlan = planProfileUpdate(
        generationDisabled, disabledCapacityEdit, 0, false
    );
    expect(!disabledCapacityPlan.decision.generatedFrameCapacityExceeded &&
            !disabledCapacityPlan.decision.swapchainRecreationDeferred,
        "Frame Generation Off must not defer a dormant capacity edit");

    auto fixedFourXWithLiveCap = fixedFourX;
    fixedFourXWithLiveCap.base_fps_cap = 49;
    mixedPlan = planProfileUpdate(
        fixed, fixedFourXWithLiveCap, 2, true
    );
    expect(mixedPlan.decision.action == ProfileUpdateAction::ApplyLive &&
            mixedPlan.decision.swapchainRecreationDeferred &&
            mixedPlan.appliedProfile.multiplier == 2 &&
            mixedPlan.appliedProfile.base_fps_cap == 49,
        "Unavailable Fixed capacity blocked an unrelated live cap");

    mixedPlan = planProfileUpdate(current, fixedFourX, 2, true);
    expect(mixedPlan.decision.action ==
                ProfileUpdateAction::DeferUntilSwapchainRecreation &&
            mixedPlan.decision.swapchainRecreationDeferred &&
            mixedPlan.appliedProfile.multiplier == 2,
        "A Fixed multiplier growth waited for recreation with the old multiplier still active");

    expect(classifyProfileUpdate(fixed, fixedThree, 1, true).action ==
            ProfileUpdateAction::DeferUntilSwapchainRecreation,
        "Fixed multiplier growth must recreate when shared capacity is unavailable");
    expect(classifyProfileUpdate(fixed, fixedThree, 2, true).action ==
            ProfileUpdateAction::ApplyLive,
        "Fixed multiplier changes must apply live within shared capacity");

    const auto fixedThreeToTwo = planProfileUpdate(
        fixedThree, fixed, 2, true
    );
    expect(fixedThreeToTwo.decision.action == ProfileUpdateAction::ApplyLive &&
            fixedThreeToTwo.decision.fixedMultiplierChanged &&
            profileUpdateInvalidatesTransientGenerationRecovery(
                fixedThreeToTwo.decision) &&
            !fixedThreeToTwo.decision.swapchainRecreationDeferred &&
            fixedThreeToTwo.appliedProfile.multiplier == 2,
        "Fixed 3x-to-2x must apply live with fresh recovery and no recreation");

    expect(generatedFrameCapacityForProfile(fixed) == 1,
        "Fixed 2x should reserve exactly one generated output");
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

    {
        using Clock = LiveEnableRecreation::Clock;
        const Clock::time_point start{};
        const auto quiet = recreatedProfileResourceQuietPeriod();
        LiveEnableRecreation enable;
        expect(!enable.armed(), "Live enable must not be armed initially");
        expect(!enable.update(false, start) && !enable.armed(),
            "Generation staying off must not arm a live-enable recreation");
        expect(enable.update(true, start) && enable.armed(),
            "Enabling generation must arm one live-enable recreation");
        expect(!enable.update(true, start + std::chrono::seconds(1)) && enable.armed(),
            "A repeated enable must not arm again");
        expect(!enable.signalAfterSuccessfulPresent(
                true, start + quiet - std::chrono::milliseconds(1)),
            "Live enable must not signal before the quiet period");
        expect(!enable.signalAfterSuccessfulPresent(false, start + quiet),
            "Live enable must not signal on an unprotected present");
        expect(enable.signalAfterSuccessfulPresent(true, start + quiet),
            "Live enable must signal on a protected present after the quiet period, "
            "measured from the first enable");
        expect(!enable.signalAfterSuccessfulPresent(true, start + quiet * 2),
            "Live enable must signal exactly once");
        expect(!enable.armed(), "Live enable must not stay armed after its signal");
        expect(!enable.update(true, start + quiet * 3) && !enable.armed(),
            "Live enable must not arm again after its signal");

        LiveEnableRecreation rearmed;
        expect(rearmed.update(true, start), "Enabling generation must arm");
        expect(!rearmed.update(false, start + quiet) && !rearmed.armed(),
            "Disabling generation before the signal must disarm");
        expect(!rearmed.signalAfterSuccessfulPresent(true, start + quiet * 2),
            "A disarmed live enable must not signal");
        expect(rearmed.update(true, start + quiet * 2) && rearmed.armed(),
            "Enabling again must arm again");
        expect(!rearmed.signalAfterSuccessfulPresent(true, start + quiet * 2),
            "A re-armed live enable must wait for its new deadline");
        expect(rearmed.signalAfterSuccessfulPresent(true, start + quiet * 3),
            "A re-armed live enable must signal at its new deadline");
    }

    {
        // low_latency is live: a change alone applies without a transition, a restart or a recreation.
        auto current = fixedProfile();
        current.base_fps_cap = 60;
        auto next = current;
        next.low_latency = false;
        const auto plan = planProfileUpdate(current, next, 3, true);
        expect(!plan.appliedProfile.low_latency &&
                plan.decision.action == ProfileUpdateAction::NoRuntimeChange &&
                !plan.decision.swapchainRecreationDeferred && !plan.decision.processRestartDeferred,
            "A low_latency change must apply live without a recreation or restart");
        expect(ls::GameConf{}.low_latency, "low_latency must default to on");
    }

    {
        // batch_hold is live too: a change alone applies without a transition, a restart or a recreation.
        auto current = fixedProfile();
        current.base_fps_cap = 60;
        auto next = current;
        next.batch_hold = false;
        const auto plan = planProfileUpdate(current, next, 3, true);
        expect(!plan.appliedProfile.batch_hold &&
                plan.decision.action == ProfileUpdateAction::NoRuntimeChange &&
                !plan.decision.swapchainRecreationDeferred && !plan.decision.processRestartDeferred,
            "A batch_hold change must apply live without a recreation or restart");
        expect(ls::GameConf{}.batch_hold, "batch_hold must default to on");
    }

    std::cout << "profile update tests passed\n";
    return 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "profile_update.hpp"
#include "fgvk-common/configuration/detection.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace fgvk::layer;

namespace {
    void expect(const bool condition, const std::string_view message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            std::exit(1);
        }
    }

    // Compare through the canonical writer so new native fields participate
    // without another power-specific field list in this test.
    std::string serialized(const ls::GameConf& profile,
            const std::filesystem::path& path) {
        ls::ConfigFile config;
        config.profiles() = {profile};
        config.write(path);
        std::ifstream input(path);
        return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
    }

    ls::GameConf selected(const ls::ConfigFile& config) {
        ls::Identification identity;
        identity.override = "game";
        const auto match = ls::findProfile(config, identity, false);
        expect(match.has_value(), "Power selection lost the matching game");
        return match->second;
    }
}

int main() {
    auto pattern = (std::filesystem::temp_directory_path() /
        "fgvk-power-update-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const auto* temporary = mkdtemp(buffer.data());
    expect(temporary != nullptr, "Unable to create power-update fixture");
    const std::filesystem::path directory(temporary);
    const auto path = directory / "conf.toml";
    const auto comparison = directory / "comparison.toml";

    ls::GameConf base;
    base.name = "game";
    base.active_in = {"Game.exe"};
    base.frame_generation_provisioned = true;
    base.frame_generation_enabled = true;
    base.adaptive = true;
    base.adaptive_max_multiplier = 3;
    base.adaptive_auto_base_fps_cap = false;
    base.target_fps = 90;

    auto handheld = base;
    handheld.target_fps = 60;
    handheld.base_fps_cap = 30;
    auto docked = base;
    docked.target_fps = 144;
    docked.base_fps_cap = 90;
    docked.adaptive_stable_cadence = true;
    base.power_profiles = {handheld, docked};
    ls::ConfigFile file;
    file.profiles() = {base};
    file.write(path);
    auto config = ls::ConfigFile(path);

    for (const auto source : {ls::PowerSource::Unknown,
            ls::PowerSource::Handheld, ls::PowerSource::Docked}) {
        config.power_source = source;
        auto expected = source == ls::PowerSource::Unknown ? base
            : source == ls::PowerSource::Handheld ? handheld : docked;
        expected.power_profiles.clear();
        const auto expectedText = serialized(expected, comparison);
        std::vector<ls::Identification> identities(5);
        identities[0].override = "game";
        identities[1].executable = "/games/Game.exe";
        identities[2].wine_executable = "/games/Game.exe";
        identities[3].process_name = "Game.exe";
        identities[4].fallback = "game";
        for (const auto& identity : identities) {
            const auto match = ls::findProfile(config, identity, false);
            expect(match && match->second.power_profiles.empty() &&
                    serialized(match->second, comparison) == expectedText,
                "Every matching path must forward the complete power set and shared identity");
        }
    }

    config.power_source = ls::PowerSource::Handheld;
    auto applied = selected(config);
    config.power_source = ls::PowerSource::Docked;
    auto requested = selected(config);
    auto plan = planProfileUpdate(applied, requested, 3, true, true);
    expect(plan.decision.action == ProfileUpdateAction::ApplyLive &&
            plan.appliedProfile.target_fps == 144 &&
            effectiveBaseFpsCap(plan.appliedProfile) == 90 &&
            plan.appliedProfile.adaptive_stable_cadence &&
            !plan.decision.processRestartDeferred &&
            !plan.decision.swapchainRecreationDeferred,
        "An AC switch with the same profile name must use the normal live-policy planner");
    applied = plan.appliedProfile;
    expect(planProfileUpdate(applied, requested, 3, true, true)
            .decision.action == ProfileUpdateAction::NoRuntimeChange,
        "Unchanged power settings must not repeatedly reset live policy");

    // An inactive Battery edit must survive saving without changing AC.
    config.profiles().front().power_profiles[0].target_fps = 72;
    config.write(path);
    config = ls::ConfigFile(path);
    config.power_source = ls::PowerSource::Docked;
    expect(planProfileUpdate(applied, selected(config), 3, true, true)
            .decision.action == ProfileUpdateAction::NoRuntimeChange,
        "Editing inactive Battery settings must not reset the active AC policy");
    config.power_source = ls::PowerSource::Handheld;
    plan = planProfileUpdate(applied, selected(config), 3, true, true);
    expect(plan.appliedProfile.target_fps == 72 &&
            plan.appliedProfile.base_fps_cap == 30 &&
            plan.decision.action == ProfileUpdateAction::ApplyLive,
        "Returning to Battery must apply its latest saved live values");
    applied = plan.appliedProfile;

    // Process-static differences must not block unrelated private or policy
    // changes. Root projects these fields before the same context planner.
    auto& ac = config.profiles().front().power_profiles[1];
    ac.gpu = "Other GPU";
    ac.frame_generation_provisioned = false;
    ac.swapchain_image_count_compatibility = true;
    ac.flow_scale = 0.8F;
    ac.performance_mode = true;
    config.power_source = ls::PowerSource::Docked;
    requested = selected(config);
    const auto projection = projectProcessStaticProfileForLiveUpdate(
        applied, requested, true, false);
    plan = planProfileUpdate(applied, projection.runtimeProfile, 3, true, true);
    expect(projection.restartRequired() && plan.decision.frameGenerationPrivateRebuild &&
            plan.decision.action == ProfileUpdateAction::ApplyLive &&
            plan.appliedProfile.target_fps == 144 &&
            plan.appliedProfile.gpu == applied.gpu &&
            plan.appliedProfile.frame_generation_provisioned &&
            plan.appliedProfile.flow_scale == applied.flow_scale,
        "Pending startup changes must retain live policy and normal private-resource handoffs");

    config.power_source = ls::PowerSource::Handheld;
    const auto reverted = planProfileUpdate(applied, selected(config), 3, true, true);
    expect(reverted.decision.action == ProfileUpdateAction::NoRuntimeChange &&
            !reverted.decision.processRestartDeferred &&
            !reverted.decision.swapchainRecreationDeferred,
        "Reverting power must not leave a superseded AC request pending");

    applied.frame_generation_provisioned = false;
    applied.frame_generation_enabled = false;
    requested = handheld;
    requested.target_fps = 100;
    const auto unprovisioned = projectProcessStaticProfileForLiveUpdate(
        applied, requested, false, false);
    plan = planProfileUpdate(applied, unprovisioned.runtimeProfile, 0, false);
    expect(unprovisioned.restartRequired() && plan.decision.processRestartDeferred &&
            !plan.appliedProfile.frame_generation_enabled &&
            !plan.appliedProfile.frame_generation_provisioned &&
            plan.appliedProfile.target_fps == 100,
        "A power switch must not invent FG resources omitted at startup");

    // Saving a clone under a new name changes identity through the same matcher
    // and live planner, including power modes and startup-only projections.
    config.profiles() = {base, base};
    auto& clone = config.profiles()[1];
    clone.name = "Future streams";
    for (auto& mode : clone.power_profiles)
        mode.name = clone.name;
    config.write(path);
    config = ls::ConfigFile(path);
    config.power_source = ls::PowerSource::Docked;
    ls::Identification cloneIdentity;
    cloneIdentity.override = "Future streams";
    auto cloneProfile = ls::findProfile(config, cloneIdentity, false)->second;
    applied = ls::profileForPowerSource(base, ls::PowerSource::Docked);
    plan = planProfileUpdate(applied, cloneProfile, 3, true, true);
    expect(plan.appliedProfile.name == "Future streams" &&
            plan.decision.action == ProfileUpdateAction::NoRuntimeChange,
        "Saving identical settings under a new name must update identity without resetting policy");
    applied = plan.appliedProfile;
    config.profiles()[1].power_profiles[1].target_fps = 120;
    config.profiles()[1].gpu = "Other GPU";
    config.profiles()[1].power_profiles[1].gpu = "Other GPU";
    config.write(path);
    config = ls::ConfigFile(path);
    config.power_source = ls::PowerSource::Docked;
    cloneProfile = ls::findProfile(config, cloneIdentity, false)->second;
    const auto cloneProjection = projectProcessStaticProfileForLiveUpdate(
        applied, cloneProfile, true, false);
    plan = planProfileUpdate(applied, cloneProjection.runtimeProfile, 3, true, true);
    expect(cloneProjection.restartRequired() &&
            plan.appliedProfile.target_fps == 120 && plan.appliedProfile.gpu == applied.gpu &&
            plan.decision.action == ProfileUpdateAction::ApplyLive,
        "Edits to the renamed profile must apply live fields and retain startup boundaries");

    std::filesystem::remove_all(directory);
    std::cout << "power-profile live-update integration tests passed\n";
}

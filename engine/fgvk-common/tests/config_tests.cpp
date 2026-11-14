/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "fgvk-common/configuration/config.hpp"
#include "fgvk-common/configuration/detection.hpp"
#include "fgvk-common/configuration/launch.hpp"
#include "configuration/launcher_exclusions_generated.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>
#include <sys/resource.h>

namespace {
    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }

    void writeText(const std::filesystem::path& path, const std::string_view text) {
        std::ofstream output(path, std::ios::trunc);
        output << text;
        output.close();
    }

    std::string readText(const std::filesystem::path& path) {
        std::ifstream input(path);
        return {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
    }

    bool sameGameConf(const ls::GameConf& left, const ls::GameConf& right) {
        return left.name == right.name &&
            left.active_in == right.active_in &&
            left.gpu == right.gpu &&
            left.multiplier == right.multiplier &&
            left.frame_generation_provisioned ==
                right.frame_generation_provisioned &&
            left.frame_generation_enabled == right.frame_generation_enabled &&
            left.swapchain_image_count_compatibility ==
                right.swapchain_image_count_compatibility &&
            left.base_fps_cap == right.base_fps_cap &&
            left.gpu_headroom_percent == right.gpu_headroom_percent &&
            left.adaptive == right.adaptive &&
            left.adaptive_auto_base_fps_cap ==
                right.adaptive_auto_base_fps_cap &&
            left.adaptive_fractional_real_frame_priority ==
                right.adaptive_fractional_real_frame_priority &&
            left.target_fps == right.target_fps &&
            left.adaptive_target_refresh_rate ==
                right.adaptive_target_refresh_rate &&
            left.adaptive_max_multiplier == right.adaptive_max_multiplier &&
            left.adaptive_stable_cadence == right.adaptive_stable_cadence &&
            left.dynamic_cadence_recovery == right.dynamic_cadence_recovery &&
            left.dynamic_cadence_probe_interval_seconds ==
                right.dynamic_cadence_probe_interval_seconds &&
            left.ultra_performance == right.ultra_performance &&
            left.flow_scale == right.flow_scale &&
            left.performance_mode == right.performance_mode &&
            left.pacing == right.pacing &&
            left.power_profiles.size() == right.power_profiles.size() &&
            std::equal(left.power_profiles.begin(), left.power_profiles.end(),
                right.power_profiles.begin(), sameGameConf);
    }

    void testGpuHeadroom(const std::filesystem::path& directory) {
        const auto path = directory / "headroom.toml";
        for (const auto reserve : ls::GameConfLimits::gpuHeadroomPercentPresets) {
            ls::ConfigFile config;
            ls::GameConf profile;
            profile.name = "headroom";
            profile.gpu_headroom_percent = reserve;
            auto handheld = profile;
            handheld.name = "handheld";
            handheld.gpu_headroom_percent = 20;
            auto docked = profile;
            docked.name = "docked";
            docked.gpu_headroom_percent = 5;
            profile.power_profiles = {handheld, docked};
            config.profiles() = {profile};
            config.write(path);
            const ls::ConfigFile loaded(path);
            expect(loaded.profiles().size() == 1 &&
                    loaded.profiles().front().gpu_headroom_percent == reserve &&
                    loaded.profiles().front().power_profiles.size() == 2 &&
                    loaded.profiles().front().power_profiles.front().gpu_headroom_percent == 20 &&
                    loaded.profiles().front().power_profiles.back().gpu_headroom_percent == 5,
                "Headroom survives profile and power-profile persistence");
        }
        for (const auto value : {"-5", "21", "7", "5.5", "true", "'10'", "4294967306"}) {
            writeText(path, "version = 2\n[[profile]]\nname = 'invalid'\ngpu_headroom_percent = " + std::string(value) + "\n");
            bool rejected = false;
            try { const ls::ConfigFile invalid(path); } catch (const std::exception&) { rejected = true; }
            expect(rejected, "Invalid TOML headroom must not silently enable a different reserve");
        }
        setenv("FGVK_ENV", "1", 1);
        setenv("FGVK_GPU_HEADROOM_PERCENT", "15", 1);
        expect(ls::WatchedConfig{}.get().profiles().front().gpu_headroom_percent == 15,
            "Environment supports GPU Headroom");
        setenv("FGVK_DYNAMIC_CADENCE_RECOVERY", "1", 1);
        expect(ls::WatchedConfig{}.get().profiles().front().gpu_headroom_percent == 0,
            "Dynamic Cadence Recovery remains exclusive with headroom");
        unsetenv("FGVK_DYNAMIC_CADENCE_RECOVERY");
        for (const auto value : {"-5", "7", "5.5", "10junk", "4294967306"}) {
            setenv("FGVK_GPU_HEADROOM_PERCENT", value, 1);
            bool rejected = false;
            try { const ls::WatchedConfig invalid; } catch (const std::exception&) { rejected = true; }
            expect(rejected, "Malformed or overflowing environment reserve is rejected");
        }
        unsetenv("FGVK_GPU_HEADROOM_PERCENT");
        unsetenv("FGVK_ENV");
    }

    void testOptionalLosslessDll(const std::filesystem::path& directory) {
        const auto dll = directory / "removed-Lossless.dll";
        const auto path = directory / "optional-lossless.toml";
        ls::ConfigFile fixture;
        fixture.global().dll = dll.string();
        ls::GameConf game;
        game.name = "dll-optional";
        game.active_in = {"Game.exe"};
        game.frame_generation_provisioned = false;
        fixture.profiles() = {game};
        fixture.write(path);
        for (const bool installed : {false, true, false}) {
            if (installed) writeText(dll, "synthetic input");
            else std::filesystem::remove(dll);
            ls::ConfigFile parsed(path);
            expect(parsed.global().dll == dll.string() && sameGameConf(parsed.profiles().front(), game),
                "DLL removal must not invalidate or reset a profile without frame generation");
            parsed.write(path);
        }
        ls::Identification identity;
        identity.override = game.name;
        expect(ls::findProfile(ls::ConfigFile(path), identity).has_value(),
            "A missing DLL must not prevent native profile matching");
        setenv("FGVK_ENV", "1", 1);
        setenv("FGVK_DLL_PATH", dll.c_str(), 1);
        setenv("FGVK_FRAME_GENERATION_PROVISIONED", "0", 1);
        const ls::WatchedConfig environment;
        expect(environment.get().global().dll == dll.string() &&
                !environment.get().profiles().front().frame_generation_provisioned,
            "A missing environment DLL must not block a profile without frame generation");
        unsetenv("FGVK_ENV");
        unsetenv("FGVK_DLL_PATH");
        unsetenv("FGVK_FRAME_GENERATION_PROVISIONED");
    }

    void testPowerProfiles(const std::filesystem::path& directory) {
        const auto root = directory / "power_supply";
        std::filesystem::create_directories(root / "AC");
        writeText(root / "AC/type", "USB_C\n");
        writeText(root / "AC/online", "0\n");
        expect(ls::detectPowerSource(root) == ls::PowerSource::Unknown,
            "An offline supply without a system battery must be unknown");
        std::filesystem::create_directories(root / "BAT");
        writeText(root / "BAT/type", "Battery\n");
        writeText(root / "BAT/status", "Full\n");
        expect(ls::detectPowerSource(root) == ls::PowerSource::Handheld,
            "A full battery must not imply AC power");
        writeText(root / "BAT/scope", "Device\n");
        expect(ls::detectPowerSource(root) == ls::PowerSource::Unknown,
            "Peripheral batteries must not select Handheld");
        writeText(root / "BAT/scope", "System\n");
        const auto path = directory / "power.toml";
        writeText(path, R"(version = 2
[global]
allow_fp16 = false
[[profile]]
name = "game"
active_in = "Game.exe"
gpu = "Other GPU"
adaptive = true
target_fps = 90
[profile.handheld]
target_fps = 60
adaptive_target_refresh_rate = true
gpu = ""
[profile.docked]
target_fps = 144
)");
        ls::ConfigFile parsed(path);
        expect(parsed.profiles().front().power_profiles.size() == 2,
            "Both native power tables must be parsed");
        parsed.write(path);
        const ls::ConfigFile roundTrip(path);
        expect(roundTrip.profiles().front().power_profiles[0].adaptive &&
                roundTrip.profiles().front().power_profiles[0]
                    .adaptive_target_refresh_rate &&
                !roundTrip.profiles().front().adaptive_target_refresh_rate &&
                !roundTrip.profiles().front().power_profiles[1]
                    .adaptive_target_refresh_rate &&
                !roundTrip.profiles().front().power_profiles[0].gpu &&
                roundTrip.profiles().front().power_profiles[1].target_fps == 144,
            "Sparse power settings must inherit and round-trip, including cleared GPUs");
        ls::Identification identity;
        identity.override = "game";
        for (const auto source : {ls::PowerSource::Unknown, ls::PowerSource::Handheld, ls::PowerSource::Docked}) {
            parsed.power_source = source;
            const auto selected = ls::findProfile(parsed, identity);
            expect(selected && selected->second.target_fps ==
                    (source == ls::PowerSource::Unknown ? 90U : source == ls::PowerSource::Handheld ? 60U : 144U) &&
                    selected->second.name == "game" && selected->second.active_in.front() == "Game.exe" &&
                    selected->second.power_profiles.empty(),
                "Explicit profile selection must resolve power without changing identity");
        }
        setenv("FGVK_CONFIG", path.c_str(), 1);
        ls::WatchedConfig watched(root);
        expect(watched.get().power_source == ls::PowerSource::Handheld,
            "The startup watcher must select battery settings");
        const auto timestamp = std::filesystem::last_write_time(path);
        writeText(root / "AC/online", "1\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(2050));
        bool changed = watched.update();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (watched.get().power_source != ls::PowerSource::Docked &&
                std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            changed = watched.update() || changed;
        }
        expect(changed && watched.get().power_source == ls::PowerSource::Docked &&
                std::filesystem::last_write_time(path) == timestamp,
            "A power transition must reload selection without rewriting TOML");
        writeText(root / "AC/online", "unknown\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(2050));
        expect(!watched.update() && watched.get().power_source == ls::PowerSource::Docked,
            "Transient power read failures must retain the confirmed runtime source");
        for (const auto invalid : {"[profile.handheld]\ntarget_fps = 60\n",
                "[profile.handheld]\nname = 'other'\n[profile.docked]\n",
                "[profile.handheld]\ntarget_fps = 0\n[profile.docked]\n"}) {
            writeText(path, std::string("version = 2\n[[profile]]\nname = 'game'\n") + invalid);
            bool rejected = false;
            try { static_cast<void>(ls::ConfigFile(path)); }
            catch (const std::exception&) { rejected = true; }
            expect(rejected, "Invalid power profiles must be rejected");
        }
    }

    // Native Remote Play used to write a root selection key. Files that still
    // contain it must load, and matching must ignore it.
    void testLegacyRootSelectionIsIgnored(const std::filesystem::path& directory) {
        const auto path = directory / "legacy-root-selection.toml";
        writeText(path, R"(version = 2
current_profile = "fgvk"

[[profile]]
name = "fgvk"

[[profile]]
name = "captured"
active_in = "CoolGame.exe"
)");
        const ls::ConfigFile legacy(path);
        expect(legacy.profiles().size() == 2,
            "A legacy root current_profile key must not reject the configuration");
        ls::Identification game;
        game.executable = "/games/CoolGame.exe";
        game.process_name = "CoolGame.exe";
        const auto captured = ls::findProfile(legacy, game, false);
        expect(captured && captured->first == ls::IdentType::EXECUTABLE &&
                captured->second.name == "captured",
            "A legacy root current_profile key must not change active_in matching");
        ls::Identification fallback;
        fallback.fallback = "fgvk";
        const auto defaulted = ls::findProfile(legacy, fallback, false);
        expect(defaulted && defaulted->first == ls::IdentType::FALLBACK &&
                defaulted->second.name == "fgvk",
            "A legacy root current_profile key must not select the fallback profile");
    }

    constexpr std::string_view validConfiguration = R"(version = 2
[global]
allow_fp16 = true
removed_global_option = "inert"

[[profile]]
name = "test"
active_in = "game"
removed_profile_option = true
adaptive = false
frame_generation_provisioned = false
swapchain_image_count_compatibility = true
base_fps_cap = 60
adaptive_auto_base_fps_cap = true
adaptive_fractional_real_frame_priority = "high"
target_fps = 144
adaptive_target_refresh_rate = true
adaptive_max_multiplier = 4
dynamic_cadence_recovery = true
dynamic_cadence_probe_interval_seconds = 0.1
ultra_performance = true
flow_scale = 0.95
performance_mode = false
)";
}

int main() {
    const ls::GameConf defaults;
    expect(defaults.multiplier == ls::GameConfDefaults::multiplier &&
            defaults.frame_generation_provisioned ==
                ls::GameConfDefaults::frameGenerationProvisioned &&
            defaults.frame_generation_enabled ==
                ls::GameConfDefaults::frameGenerationEnabled &&
            defaults.swapchain_image_count_compatibility ==
                ls::GameConfDefaults::swapchainImageCountCompatibility &&
            defaults.base_fps_cap == ls::GameConfDefaults::baseFpsCap &&
            defaults.adaptive == ls::GameConfDefaults::adaptive &&
            defaults.adaptive_auto_base_fps_cap ==
                ls::GameConfDefaults::adaptiveAutoBaseFpsCap &&
            defaults.adaptive_fractional_real_frame_priority ==
                ls::GameConfDefaults::adaptiveFractionalRealFramePriority &&
            defaults.target_fps == ls::GameConfDefaults::targetFps &&
            !defaults.adaptive_target_refresh_rate &&
            defaults.adaptive_max_multiplier ==
                ls::GameConfDefaults::adaptiveMaxMultiplier &&
            defaults.adaptive_stable_cadence ==
                ls::GameConfDefaults::adaptiveStableCadence &&
            defaults.dynamic_cadence_recovery ==
                ls::GameConfDefaults::dynamicCadenceRecovery &&
            defaults.dynamic_cadence_probe_interval_seconds ==
                ls::GameConfDefaults::dynamicCadenceProbeIntervalSeconds &&
            defaults.ultra_performance ==
                ls::GameConfDefaults::ultraPerformance &&
            defaults.flow_scale == ls::GameConfDefaults::flowScale &&
            defaults.performance_mode ==
                ls::GameConfDefaults::performanceMode &&
            defaults.pacing == ls::GameConfDefaults::pacing,
        "GameConf must use the Renderer profile defaults");
    expect(ls::GameConfDefaults::flowScale == 0.8F,
        "new Renderer profiles must default to the 80% Flow Scale");
    expect(ls::GameConfDefaults::adaptiveStableCadence,
        "Smooth Cadence must default to on for new standalone Renderer profiles");

    const auto directory = std::filesystem::temp_directory_path() /
        ("fgvk-config-test-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::create_directories(directory);

    unsetenv("FGVK_ENV");
    testGpuHeadroom(directory);
    testOptionalLosslessDll(directory);
    testPowerProfiles(directory);
    testLegacyRootSelectionIsIgnored(directory);

    const auto defaultPath = directory / "default.toml";
    ls::ConfigFile::createDefaultConfigFile(defaultPath);
    const ls::ConfigFile generatedDefaults(defaultPath);
    const ls::ConfigFile inMemoryDefaults;
    expect(ls::GlobalConf{}.allow_fp16 &&
            generatedDefaults.global().allow_fp16 &&
            inMemoryDefaults.global().allow_fp16,
        "Renderer defaults must allow FP16 on supported hardware");
    expect(generatedDefaults.global().dll == inMemoryDefaults.global().dll &&
            generatedDefaults.global().allow_fp16 ==
                inMemoryDefaults.global().allow_fp16 &&
            std::ranges::equal(
                generatedDefaults.profiles(), inMemoryDefaults.profiles(),
                sameGameConf
            ),
        "The documented default TOML and in-memory examples must stay equivalent");

    const auto precisionPath = directory / "precision.toml";
    writeText(precisionPath, "version = 2\n[[profile]]\nname = 'legacy'\nhdr_quality = 'performance'\nmultiplier = 3\n");
    expect(ls::ConfigFile(precisionPath).profiles().front().multiplier == 3,
        "A retired profile key must not prevent loading supported settings");
    for (const std::string_view global : {"", "[global]\n", "[global]\nallow_fp16 = true\n"}) {
        writeText(precisionPath, "version = 2\n" + std::string(global) + "[[profile]]\n");
        expect(ls::ConfigFile(precisionPath).global().allow_fp16,
            "Omitting the global section or FP16 setting must still allow FP16");
    }
    writeText(precisionPath, "version = 2\n[global]\nallow_fp16 = false\n[[profile]]\n");
    const ls::ConfigFile fp32Config(precisionPath);
    expect(!fp32Config.global().allow_fp16,
        "An explicit FP32 setting must override the FP16 default");
    fp32Config.write(precisionPath);
    expect(!ls::ConfigFile(precisionPath).global().allow_fp16,
        "Saving an existing FP32 configuration must preserve its precision");
    expect(ls::GlobalConf{}.pacing_correction && generatedDefaults.global().pacing_correction &&
            inMemoryDefaults.global().pacing_correction && fp32Config.global().pacing_correction,
        "Pacing correction must default on, also when the key is missing");
    writeText(precisionPath, "version = 2\n[global]\npacing_correction = false\n[[profile]]\n");
    const ls::ConfigFile uncorrected(precisionPath);
    expect(!uncorrected.global().pacing_correction && uncorrected.global().allow_fp16,
        "An explicit pacing_correction = false must be read");
    uncorrected.write(precisionPath);
    expect(!ls::ConfigFile(precisionPath).global().pacing_correction,
        "Saving must preserve pacing_correction = false");

    const auto path = directory / "conf.toml";
    writeText(path, validConfiguration);

    unsetenv("FGVK_ENV");
    setenv("FGVK_CONFIG", path.c_str(), 1);

    ls::WatchedConfig config;
    expect(!config.update(),
        "The configuration parsed by the constructor must not reload immediately");

    const auto initialTimestamp = std::filesystem::last_write_time(path);
    writeText(path, "version = [partial");
    std::filesystem::last_write_time(path, initialTimestamp + std::chrono::seconds(2));

    bool firstParseFailed = false;
    try {
        static_cast<void>(config.update());
    } catch (const std::exception&) {
        firstParseFailed = true;
    }
    expect(firstParseFailed, "A partial configuration must be rejected");

    expect(!config.update(),
        "A failed parse must be briefly backed off instead of retried every frame");

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    bool sameVersionRetried = false;
    try {
        static_cast<void>(config.update());
    } catch (const std::exception&) {
        sameVersionRetried = true;
    }
    expect(sameVersionRetried,
        "A failed parse must retain its timestamp so the same version is retried");

    writeText(path, validConfiguration);
    std::filesystem::last_write_time(path, initialTimestamp + std::chrono::seconds(4));
    expect(config.update(), "A subsequent complete configuration must be accepted");
    expect(config.get().profiles().size() == 1,
        "The accepted configuration must replace the previous profile set");
    expect(config.get().profiles().front().target_fps == 144,
        "The accepted configuration must expose its new policy");
    expect(config.get().profiles().front().adaptive_target_refresh_rate,
        "The accepted configuration must expose automatic refresh targeting");
    expect(config.get().profiles().front().dynamic_cadence_recovery,
        "The accepted configuration must expose dynamic cadence recovery");
    expect(config.get().profiles().front().dynamic_cadence_probe_interval_seconds == 0.1F,
        "The accepted configuration must expose the cadence probe interval");
    expect(!config.get().profiles().front().frame_generation_provisioned &&
            config.get().profiles().front()
                .swapchain_image_count_compatibility,
        "The accepted configuration must expose provisioning and image-count compatibility");
    expect(config.get().profiles().front().ultra_performance &&
            config.get().profiles().front().flow_scale == 0.95F &&
            !config.get().profiles().front().performance_mode &&
            ls::effectiveFlowScale(config.get().profiles().front()) ==
                ls::GameConfDefaults::ultraPerformanceFlowScale &&
            ls::effectivePerformanceMode(config.get().profiles().front()),
        "Ultra Performance must preserve saved settings while overriding backend construction");
    expect(config.get().profiles().front().base_fps_cap == 0 &&
            !config.get().profiles().front().adaptive_auto_base_fps_cap &&
            config.get().profiles().front()
                    .adaptive_fractional_real_frame_priority ==
                ls::AdaptiveFractionalRealFramePriority::Auto,
        "Dynamic cadence recovery must disable every base FPS cap");
    expect(config.get().profiles().front().multiplier == 2,
        "Unknown legacy options must be inert without disturbing known defaults");

    const auto canonicalPath = directory / "canonical.toml";
    config.get().write(canonicalPath);
    const auto canonicalConfiguration = readText(canonicalPath);
    expect(canonicalConfiguration.find("removed_global_option") == std::string::npos &&
            canonicalConfiguration.find("removed_profile_option") == std::string::npos,
        "A canonical Renderer write must remove unknown legacy options");
    const ls::ConfigFile canonicalConfig(canonicalPath);
    expect(std::ranges::equal(
            config.get().profiles(), canonicalConfig.profiles(), sameGameConf
        ),
        "A canonical Renderer write must preserve every accepted profile setting");

    // Real short writes must preserve the previous file, including when the
    // process has permission to open it. Exercise every configuration writer.
    for (const int writer : {0, 1, 2}) {
        const auto failurePath = directory / "write-failure.conf";
        writeText(failurePath, "previous configuration\n");
        struct rlimit previousLimit {};
        expect(::getrlimit(RLIMIT_FSIZE, &previousLimit) == 0,
            "Could not read file-size limit for write failure test");
        auto limited = previousLimit;
        limited.rlim_cur = 16;
        const auto previousSignal = std::signal(SIGXFSZ, SIG_IGN);
        expect(::setrlimit(RLIMIT_FSIZE, &limited) == 0,
            "Could not constrain file writes for failure test");
        bool failed = false;
        try {
            if (writer == 0) canonicalConfig.write(failurePath);
            else if (writer == 1) ls::ConfigFile::createDefaultConfigFile(failurePath);
            else ls::LaunchConfigFile{}.write(failurePath);
        } catch (const std::exception&) {
            failed = true;
        }
        const auto restored = ::setrlimit(RLIMIT_FSIZE, &previousLimit);
        std::signal(SIGXFSZ, previousSignal);
        expect(restored == 0, "Could not restore file-size limit");
        expect(failed, "Configuration writer silently accepted a short write");
        expect(readText(failurePath) == "previous configuration\n",
            "A failed configuration write damaged the previous file");
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            expect(!entry.path().filename().string().starts_with(".write-failure.conf."),
                "A failed configuration write left a temporary file");
        }
    }

    const auto linkedPath = directory / "linked.toml";
    std::filesystem::create_symlink(canonicalPath, linkedPath);
    canonicalConfig.write(linkedPath);
    expect(std::filesystem::is_symlink(linkedPath) &&
            readText(canonicalPath) == canonicalConfiguration,
        "Atomic configuration writes must preserve configured symlinks");
    const auto missingTarget = directory / "new-target.toml";
    const auto danglingPath = directory / "new-link.toml";
    std::filesystem::create_symlink(missingTarget.filename(), danglingPath);
    canonicalConfig.write(danglingPath);
    expect(std::filesystem::is_symlink(danglingPath) &&
            readText(missingTarget) == canonicalConfiguration,
        "First-time configuration writes must preserve dangling relative symlinks");
    std::filesystem::permissions(canonicalPath, std::filesystem::perms::owner_read);
    bool readOnlyRejected = false;
    try {
        canonicalConfig.write(canonicalPath);
    } catch (const std::exception&) {
        readOnlyRejected = true;
    }
    expect(readOnlyRejected && readText(canonicalPath) == canonicalConfiguration,
        "A read-only configuration must be preserved without changing permissions");
    std::filesystem::permissions(canonicalPath,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);

    const auto fixedMultiplierPath = directory / "fixed-multiplier.toml";
    writeText(fixedMultiplierPath, R"(version = 2
[[profile]]
multiplier = 5
)");
    const ls::ConfigFile fixedMultiplierConfiguration(fixedMultiplierPath);
    expect(fixedMultiplierConfiguration.profiles().front().multiplier == 5,
        "The fixed multiplier must accept the supported 5x maximum");

    const auto invalidFixedMultiplierPath = directory / "invalid-fixed-multiplier.toml";
    writeText(invalidFixedMultiplierPath, R"(version = 2
[[profile]]
multiplier = 6
)");
    bool invalidFixedMultiplierRejected = false;
    try {
        static_cast<void>(ls::ConfigFile(invalidFixedMultiplierPath));
    } catch (const std::exception&) {
        invalidFixedMultiplierRejected = true;
    }
    expect(invalidFixedMultiplierRejected,
        "Fixed multipliers above the supported 5x maximum must be rejected");

    const auto priorityPath = directory / "fractional-priority.toml";
    writeText(priorityPath, R"(version = 2
[[profile]]
adaptive = true
adaptive_auto_base_fps_cap = false
adaptive_fractional_real_frame_priority = "very-high"
)");
    const ls::ConfigFile priorityConfiguration(priorityPath);
    expect(priorityConfiguration.profiles().front()
                .adaptive_fractional_real_frame_priority ==
            ls::AdaptiveFractionalRealFramePriority::VeryHigh,
        "Fractional real-frame priority did not preserve its stable token");

    const auto invalidPriorityPath = directory / "invalid-fractional-priority.toml";
    writeText(invalidPriorityPath, R"(version = 2
[[profile]]
adaptive_fractional_real_frame_priority = "maximum"
)");
    bool invalidPriorityRejected = false;
    try {
        static_cast<void>(ls::ConfigFile(invalidPriorityPath));
    } catch (const std::exception&) {
        invalidPriorityRejected = true;
    }
    expect(invalidPriorityRejected,
        "Unknown Fractional real-frame priorities must be rejected");


    for (const std::string_view invalidInterval : {"0.09", "4"}) {
        const auto invalidIntervalPath = directory /
            ("invalid-probe-interval-" + std::string(invalidInterval) +
             ".toml");
        writeText(invalidIntervalPath,
            "version = 2\n[[profile]]\n"
            "dynamic_cadence_probe_interval_seconds = " +
            std::string(invalidInterval) + "\n");
        bool invalidIntervalRejected = false;
        try {
            static_cast<void>(ls::ConfigFile(invalidIntervalPath));
        } catch (const std::exception&) {
            invalidIntervalRejected = true;
        }
        expect(invalidIntervalRejected,
            "Cadence probe intervals outside 0.1-3 seconds must be rejected");
    }

    // Retired scaling keys stay accepted and ignored: an old file loads, a
    // rewrite drops them, and out-of-range values cannot reject the file.
    const auto retiredScalingPath = directory / "retired-scaling.toml";
    writeText(retiredScalingPath, R"(version = 2
[global]
allow_fp16 = true
[[profile]]
name = "retired-scaling"
frame_generation_provisioned = false
frame_generation_enabled = false
scaling_enabled = true
swapchain_image_count_compatibility = true
scaling_method = "ls1"
scaling_factor = 1.75
scaling_supersampling = true
scaling_sharpness = 0.6
multiplier = 3
flow_scale = 0.95
)");
    const ls::ConfigFile retiredScaling(retiredScalingPath);
    const auto& retiredProfile = retiredScaling.profiles().front();
    expect(!retiredProfile.frame_generation_provisioned &&
            !retiredProfile.frame_generation_enabled &&
            retiredProfile.swapchain_image_count_compatibility &&
            retiredProfile.multiplier == 3 && retiredProfile.flow_scale == 0.95F,
        "Retired scaling keys must not prevent loading the supported settings");
    retiredScaling.write(retiredScalingPath);
    expect(readText(retiredScalingPath).find("scaling_") == std::string::npos,
        "Rewriting a configuration must drop the retired scaling keys");
    expect(std::ranges::equal(retiredScaling.profiles(),
            ls::ConfigFile(retiredScalingPath).profiles(), sameGameConf),
        "Retired scaling keys must round-trip as absent settings");
    const auto invalidRetiredPath = directory / "invalid-retired-scaling.toml";
    writeText(invalidRetiredPath, R"(version = 2
[[profile]]
name = "invalid-retired-scaling"
scaling_method = "bogus"
scaling_factor = 9.0
scaling_sharpness = 7.0
)");
    expect(ls::ConfigFile(invalidRetiredPath).profiles().front().name ==
            "invalid-retired-scaling",
        "Out-of-range retired scaling values must not reject the file");

    // Retired Gamescope keys stay accepted and ignored: an old file loads, a
    // rewrite drops them, and the supported settings still round-trip.
    const auto retiredGamescopePath = directory / "retired-gamescope.toml";
    writeText(retiredGamescopePath, R"(version = 2
[global]
allow_fp16 = true
[[profile]]
name = "retired-gamescope"
frame_generation_provisioned = false
frame_generation_enabled = false
frame_generation_refresh_threshold = 0
gamescope_vrr_mode = "follow-steam"
multiplier = 3
flow_scale = 0.95
)");
    const ls::ConfigFile retiredGamescope(retiredGamescopePath);
    const auto& retiredGamescopeProfile = retiredGamescope.profiles().front();
    expect(retiredGamescopeProfile.name == "retired-gamescope" &&
            !retiredGamescopeProfile.frame_generation_provisioned &&
            retiredGamescopeProfile.multiplier == 3 &&
            retiredGamescopeProfile.flow_scale == 0.95F,
        "Retired Gamescope keys must not prevent loading the supported settings");
    retiredGamescope.write(retiredGamescopePath);
    const auto rewrittenGamescope = readText(retiredGamescopePath);
    expect(rewrittenGamescope.find("frame_generation_refresh_threshold") == std::string::npos &&
            rewrittenGamescope.find("gamescope_vrr_mode") == std::string::npos,
        "Rewriting a configuration must drop the retired Gamescope keys");
    expect(std::ranges::equal(retiredGamescope.profiles(),
            ls::ConfigFile(retiredGamescopePath).profiles(), sameGameConf),
        "Retired Gamescope keys must round-trip as absent settings");

    ls::ConfigFile detectionConfig;
    detectionConfig.profiles() = {
        ls::GameConf{.name = "fgvk"},
        ls::GameConf{.name = "captured", .active_in = {"CoolGame.exe"}},
    };

    setenv("FGVK_PROFILE", "captured", 1);
    const auto environmentIdentification = ls::identify();
    expect(environmentIdentification.override.has_value() &&
            environmentIdentification.override.value() == "captured",
        "FGVK_PROFILE must populate the explicit profile override");
    unsetenv("FGVK_PROFILE");

    ls::Identification identification{
        .fallback = "fgvk",
        .executable = "/games/CoolGame.exe",
        .process_name = "CoolGame.exe",
    };
    auto detectedProfile = ls::findProfile(detectionConfig, identification);
    expect(detectedProfile.has_value() &&
            detectedProfile->first == ls::IdentType::EXECUTABLE &&
            detectedProfile->second.name == "captured",
        "A captured process profile must supersede the launch-time fallback");

    identification.executable = "/games/UnknownGame";
    identification.process_name = "UnknownGame";
    detectedProfile = ls::findProfile(detectionConfig, identification);
    expect(detectedProfile.has_value() &&
            detectedProfile->first == ls::IdentType::FALLBACK &&
            detectedProfile->second.name == "fgvk",
        "An unknown game must retain the default renderer context");

    identification.override = "fgvk";
    identification.executable = "/games/CoolGame.exe";
    identification.process_name = "CoolGame.exe";
    detectedProfile = ls::findProfile(detectionConfig, identification);
    expect(detectedProfile.has_value() &&
            detectedProfile->first == ls::IdentType::OVERRIDE &&
            detectedProfile->second.name == "fgvk",
        "An explicit caller profile must remain a hard override");

    // Launchers inherit exactly the same profile environment as their game.
    // Neither that environment nor an older captured launcher alias may make
    // framegen-vulkan change the launcher's Vulkan device or presentation resources.
    const auto gameIdentification = identification;
    std::vector<std::string> launchers;
    for (const auto name : ls::detail::excludedWindowsLauncherExecutables) {
        launchers.emplace_back(name);
        auto upper = std::string(name);
        std::transform(upper.begin(), upper.end(), upper.begin(), [](const char c) {
            return c >= 'a' && c <= 'z' ? static_cast<char>(c - ('a' - 'A')) : c;
        });
        launchers.push_back(std::move(upper));
    }
    for (const auto& launcher : launchers) {
        detectionConfig.profiles()[1].active_in.emplace_back(launcher);
        const ls::Identification launcherIdentification{
            .override = "captured",
            .fallback = "fgvk",
            .executable = "/proton/files/bin/wine64-preloader",
            .wine_executable = std::string("C:\\Launcher\\") + launcher,
            .process_name = "GameThread",
        };
        expect(!ls::findProfile(detectionConfig, launcherIdentification),
            "An excluded launcher must stay native despite an inherited override");
        auto fallbackLauncher = launcherIdentification;
        fallbackLauncher.override.reset();
        expect(!ls::findProfile(detectionConfig, fallbackLauncher),
            "Captured launcher aliases and the default fallback must not activate framegen-vulkan");
        auto fallbackOnlyConfig = detectionConfig;
        fallbackOnlyConfig.profiles()[1].active_in.clear();
        expect(!ls::findProfile(fallbackOnlyConfig, fallbackLauncher),
            "An uncaptured launcher must not activate through the default fallback");
        auto matchedLauncher = fallbackLauncher;
        matchedLauncher.fallback.reset();
        expect(!ls::findProfile(detectionConfig, matchedLauncher),
            "An explicit launcher alias alone must not activate framegen-vulkan");
        auto directLauncher = launcherIdentification;
        directLauncher.wine_executable.reset();
        directLauncher.executable = std::string("/Launcher/") + launcher;
        expect(!ls::findProfile(detectionConfig, directLauncher),
            "An exact launcher executable must stay native without a Wine path");
        setenv("FGVK_ENV", "1", 1);
        expect(!ls::findProfile(detectionConfig, launcherIdentification),
            "Environment-only profiles must not activate framegen-vulkan in a launcher");
        expect(std::getenv("FGVK_ENV") &&
                std::string_view(std::getenv("FGVK_ENV")) == "1",
            "Launcher exclusion must not strip the child's inherited activation");
        expect(ls::findProfile(detectionConfig, gameIdentification).has_value(),
            "Skipping a launcher must leave its child's environment profile active");
        unsetenv("FGVK_ENV");
    }
    for (const auto* executable : {
            "/Ubisoft/UbisoftConnect.exe/TheCrewMotorfest.exe",
            "/games/MyUbisoftConnect.exe", "/games/upc.exe.backup",
            "/games/UplayWebCoreGame.exe", "/games/UnknownGame",
            "/CDPR/REDlauncher.exe/witcher3.exe", "/games/witcher3.exe",
            "/games/MyREDlauncher.exe", "/games/REDprelauncher.exe.backup",
            "/Rockstar Games/Launcher/MaxPayne3.exe", "/games/PlayMaxPayne3.exe",
            "/games/GTA5.exe", "/games/RDR2.exe", "/games/Launcher.exe",
            "/games/MyRockstarService.exe", "/games/EADesktop.exe.backup",
        }) {
        auto game = gameIdentification;
        game.executable = "/proton/files/bin/wine64-preloader";
        game.wine_executable = executable;
        game.process_name = "upc.exe"; // A thread name is not executable proof.
        const auto childProfile = ls::findProfile(detectionConfig, game);
        expect(childProfile && childProfile->second.name == "fgvk",
            "Launcher directories, partial names, and inherited thread names must not block a game");
    }

    setenv("FGVK_ENV", "1", 1);
    setenv("FGVK_ADAPTIVE", "0", 1);
    unsetenv("FGVK_NO_FP16");
    expect(ls::WatchedConfig{}.get().global().allow_fp16,
        "Environment-only configuration must allow FP16 by default");
    setenv("FGVK_NO_FP16", "1", 1);
    expect(!ls::WatchedConfig{}.get().global().allow_fp16,
        "FGVK_NO_FP16=1 must preserve the explicit FP32 override");
    setenv("FGVK_NO_FP16", "0", 1);
    expect(ls::WatchedConfig{}.get().global().allow_fp16,
        "FGVK_NO_FP16=0 must allow FP16");
    unsetenv("FGVK_NO_FP16");
    setenv("FGVK_BASE_FPS_CAP", "30", 1);
    setenv("FGVK_ADAPTIVE_AUTO_BASE_FPS_CAP", "1", 1);
    setenv("FGVK_ADAPTIVE_FRACTIONAL_REAL_FRAME_PRIORITY", "high", 1);
    setenv("FGVK_ADAPTIVE_TARGET_REFRESH_RATE", "1", 1);
    setenv("FGVK_DYNAMIC_CADENCE_RECOVERY", "1", 1);
    setenv("FGVK_DYNAMIC_CADENCE_PROBE_INTERVAL_SECONDS", "0.5", 1);
    setenv("FGVK_FRAME_GENERATION_PROVISIONED", "0", 1);
    setenv("FGVK_ULTRA_PERFORMANCE", "1", 1);
    setenv("FGVK_SWAPCHAIN_IMAGE_COUNT_COMPATIBILITY", "1", 1);
    const ls::WatchedConfig environmentConfig;
    expect(environmentConfig.get().profiles().front().adaptive_target_refresh_rate,
        "Environment profiles must accept automatic refresh targeting");
    setenv("FGVK_ADAPTIVE_TARGET_REFRESH_RATE", "0", 1);
    expect(!ls::WatchedConfig{}.get().profiles().front().adaptive_target_refresh_rate,
        "Environment profiles must accept explicitly disabled refresh targeting");
    expect(environmentConfig.get().profiles().front().dynamic_cadence_recovery &&
            environmentConfig.get().profiles().front()
                .dynamic_cadence_probe_interval_seconds == 0.5F &&
            !environmentConfig.get().profiles().front()
                .frame_generation_provisioned &&
            environmentConfig.get().profiles().front().base_fps_cap == 0 &&
            !environmentConfig.get().profiles().front().adaptive_auto_base_fps_cap &&
            environmentConfig.get().profiles().front()
                    .adaptive_fractional_real_frame_priority ==
                ls::AdaptiveFractionalRealFramePriority::Auto &&
            environmentConfig.get().profiles().front().ultra_performance &&
            environmentConfig.get().profiles().front()
                .swapchain_image_count_compatibility,
        "Environment configuration must expose cadence and swapchain policy");
    unsetenv("FGVK_DYNAMIC_CADENCE_RECOVERY");
    unsetenv("FGVK_DYNAMIC_CADENCE_PROBE_INTERVAL_SECONDS");
    unsetenv("FGVK_FRAME_GENERATION_PROVISIONED");
    unsetenv("FGVK_ULTRA_PERFORMANCE");
    unsetenv("FGVK_SWAPCHAIN_IMAGE_COUNT_COMPATIBILITY");
    unsetenv("FGVK_ADAPTIVE");
    unsetenv("FGVK_ADAPTIVE_AUTO_BASE_FPS_CAP");
    unsetenv("FGVK_ADAPTIVE_FRACTIONAL_REAL_FRAME_PRIORITY");
    unsetenv("FGVK_ADAPTIVE_TARGET_REFRESH_RATE");
    unsetenv("FGVK_BASE_FPS_CAP");
    unsetenv("FGVK_ENV");

    std::filesystem::remove_all(directory);
    std::cout << "configuration watcher tests passed\n";
    return 0;
}

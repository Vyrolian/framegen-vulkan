/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "fgvk-common/configuration/launch.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#include <unistd.h>

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
    }

    std::string readText(const std::filesystem::path& path) {
        std::ifstream input(path);
        return {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
    }

    bool rejects(const std::filesystem::path& path, const std::string_view content) {
        writeText(path, content);
        try {
            static_cast<void>(ls::LaunchConfigFile(path));
        } catch (const std::exception&) {
            return true;
        }
        return false;
    }

}

int main() {
    const ls::LaunchConfigFile defaults;
    expect(defaults.settings().disable_steam_overlay &&
            !defaults.settings().enable_zink &&
            !defaults.settings().force_alsa_audio,
        "standalone launcher defaults must disable Steam overlay and preserve other defaults");

    const auto directory = std::filesystem::temp_directory_path() /
        ("fgvk-launch-config-test-" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::create_directories(directory);

    const auto& logging = defaults.settings();
    expect(!logging.log_pacing && !logging.log_trace && !logging.log_wayland &&
            !logging.log_gpu_cost,
        "logging switches must default to off");

    ls::LaunchConfigFile configured;
    configured.settings().disable_steam_overlay = false;
    configured.settings().enable_zink = true;
    configured.settings().force_alsa_audio = true;
    configured.settings().log_pacing = true;
    configured.settings().log_trace = false;
    configured.settings().log_wayland = true;
    configured.settings().log_gpu_cost = true;
    const auto canonicalPath = directory / "launcher.conf";
    configured.write(canonicalPath);

    expect(readText(canonicalPath) ==
            "version=1\n"
            "disable_steam_overlay=0\n"
            "enable_zink=1\n"
            "force_alsa_audio=1\n"
            "log_pacing=1\n"
            "log_trace=0\n"
            "log_wayland=1\n"
            "log_gpu_cost=1\n",
        "launcher configuration writer must retain its canonical shell-safe format");

    const ls::LaunchConfigFile restored(canonicalPath);
    expect(!restored.settings().disable_steam_overlay &&
            restored.settings().enable_zink &&
            restored.settings().force_alsa_audio,
        "launcher configuration must round-trip every supported setting");
    expect(restored.settings().log_pacing && !restored.settings().log_trace &&
            restored.settings().log_wayland && restored.settings().log_gpu_cost,
        "launcher configuration must round-trip every logging switch");

    for (const auto value : {"0", "1"}) {
        writeText(canonicalPath, std::string("version=1\ndisable_hdr_exposure=") + value +
            "\nenable_zink=1\n");
        expect(ls::LaunchConfigFile(canonicalPath).settings().enable_zink,
            "the retired launcher key must parse without changing supported settings");
    }
    for (const auto value : {"0", "1"}) {
        writeText(canonicalPath, std::string("version=1\nlog_present=") + value +
            "\nenable_zink=1\n");
        const ls::LaunchConfigFile retired(canonicalPath);
        expect(retired.settings().enable_zink && !retired.settings().log_pacing &&
                !retired.settings().log_trace && !retired.settings().log_wayland &&
                !retired.settings().log_gpu_cost,
            "the retired log_present key must parse without changing any supported setting");
    }
    expect(rejects(directory / "duplicate-present.conf", "version=1\nlog_present=1\nlog_present=0\n"),
        "duplicate retired log_present settings must fail closed");
    expect(rejects(directory / "duplicate-hdr.conf",
            "version=1\ndisable_hdr_exposure=1\ndisable_hdr_exposure=0\n"),
        "duplicate HDR settings must fail closed");
    expect(rejects(directory / "invalid-hdr.conf",
            "version=1\ndisable_hdr_exposure=true\n"),
        "HDR settings must use canonical booleans");
    const auto legacyPath = directory / "legacy-launcher.conf";
    writeText(legacyPath, "version=1\nenable_zink=1\nforce_alsa_audio=0\n");
    expect(ls::LaunchConfigFile(legacyPath).settings().disable_steam_overlay,
        "an existing launcher configuration must use the new overlay default");
    const ls::LaunchConfigFile legacyFile(legacyPath);
    const auto& legacy = legacyFile.settings();
    expect(!legacy.log_pacing && !legacy.log_trace && !legacy.log_wayland &&
            !legacy.log_gpu_cost,
        "a launcher configuration without logging switches must load them as off");
    expect(rejects(directory / "duplicate-logging.conf", "version=1\nlog_pacing=1\nlog_pacing=0\n") &&
            rejects(directory / "duplicate-trace.conf", "version=1\nlog_trace=1\nlog_trace=0\n") &&
            rejects(directory / "duplicate-wayland.conf", "version=1\nlog_wayland=1\nlog_wayland=0\n") &&
            rejects(directory / "duplicate-gpu-cost.conf", "version=1\nlog_gpu_cost=1\nlog_gpu_cost=0\n"),
        "duplicate logging switches must be rejected");
    expect(rejects(directory / "invalid-logging.conf", "version=1\nlog_trace=true\n") &&
            rejects(directory / "invalid-present.conf", "version=1\nlog_present=2\n"),
        "logging switches must use canonical booleans");
    expect(rejects(directory / "invalid-overlay.conf",
            "version=1\ndisable_steam_overlay=true\n"),
        "overlay launcher policy must accept only canonical booleans");
    expect(rejects(directory / "duplicate-overlay.conf",
            "version=1\ndisable_steam_overlay=1\ndisable_steam_overlay=0\n"),
        "duplicate overlay policies must be rejected");

    expect(rejects(directory / "missing-version.conf", "enable_zink=1\n"),
        "launcher configuration without a version must be rejected");
    expect(rejects(directory / "future-version.conf", "version=2\n"),
        "future launcher configuration versions must be rejected");
    expect(rejects(directory / "invalid-boolean.conf",
            "version=1\nenable_zink=true\n"),
        "launcher booleans outside 0/1 must be rejected");
    expect(rejects(directory / "unknown-setting.conf",
            "version=1\nunknown_setting=1\n"),
        "unknown launcher settings must remain inert");
    expect(rejects(directory / "duplicate-setting.conf",
            "version=1\nenable_zink=1\nenable_zink=0\n"),
        "duplicate launcher settings must be rejected");

    setenv("FGVK_LAUNCH_CONFIG", canonicalPath.c_str(), 1);
    expect(ls::findLaunchConfigurationFile() == canonicalPath,
        "FGVK_LAUNCH_CONFIG must override launcher configuration discovery");
    unsetenv("FGVK_LAUNCH_CONFIG");

    std::filesystem::remove_all(directory);
    std::cout << "standalone launcher configuration tests passed\n";
    return 0;
}

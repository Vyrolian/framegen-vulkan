/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "fgvk-common/configuration/config.hpp"
#include "atomic_write.hpp"
#include "fgvk-common/helpers/errors.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <toml.hpp>

using namespace ls;

void ConfigFile::createDefaultConfigFile(const std::filesystem::path& path) {
    try {
        std::ostringstream ofs;

        ofs << "version = " << ConfigFile::formatVersion << R"(

[global]
# dll = '/media/games/Lossless Scaling/Lossless.dll' # if you don't have LS in the default location
allow_fp16 = true # request FP16 model arithmetic; false selects FP32
pacing_correction = true # centre generated frames from measured GPU present times
real_present_lead = true # present real frames earlier by their measured GPU wait

[[profile]]
name = "4x FG / 85% [Performance]"
active_in = [ # see the wiki for more info
    'vkcube',
    'vkcubepp'
]
# gpu = 'NVIDIA GeForce RTX 5080' # see the wiki for more info
multiplier = 4
frame_generation_provisioned = true
frame_generation_enabled = true
swapchain_image_count_compatibility = false
base_fps_cap = 0
low_latency = true # release each capped frame just in time at the cap
reflex_dynamic_cap = true # with the game's Reflex on, keep the GPU just under full load
batch_hold = true # start each output batch about 2 ms after its frames are ready
ultra_performance = false
flow_scale = 0.85
performance_mode = true
pacing = 'none' # see the wiki for more info

[[profile]]
name = "2x FG / 100%"
active_in = 'GenshinImpact.exe'
gpu = 'NVIDIA GeForce RTX 5080'
multiplier = 2
)";
        detail::writeConfigurationAtomically(path, ofs.str());
    } catch (const std::exception& e) {
        throw ls::error("unable to create default configuration file", e);
    }
}

ConfigFile::ConfigFile() {
    this->globalConf = {
        .allow_fp16 = true,
        .pacing_correction = true,
        .real_present_lead = true
    };
    this->profileConfs.emplace_back(GameConf {
        .name = "4x FG / 85% [Performance]",
        .active_in = {
            "vkcube",
            "vkcubepp"
        },
        .multiplier = 4,
        .frame_generation_provisioned =
            GameConfDefaults::frameGenerationProvisioned,
        .frame_generation_enabled = GameConfDefaults::frameGenerationEnabled,
        .swapchain_image_count_compatibility =
            GameConfDefaults::swapchainImageCountCompatibility,
        .base_fps_cap = GameConfDefaults::baseFpsCap,
        .low_latency = GameConfDefaults::lowLatency,
        .reflex_dynamic_cap = GameConfDefaults::reflexDynamicCap,
        .batch_hold = GameConfDefaults::batchHold,
        .ultra_performance = GameConfDefaults::ultraPerformance,
        .flow_scale = 0.85F,
        .performance_mode = true,
        .pacing = GameConfDefaults::pacing
    });
    this->profileConfs.emplace_back(GameConf {
        .name = "2x FG / 100%",
        .active_in = {
            "GenshinImpact.exe"
        },
        .gpu = "NVIDIA GeForce RTX 5080",
        .multiplier = GameConfDefaults::multiplier
    });
}

namespace {
    constexpr auto configurationParseRetryDelay = std::chrono::milliseconds(50);

    std::string formatFloatingLimit(const float value) {
        auto result = std::to_string(value);
        while (result.ends_with('0'))
            result.pop_back();
        if (result.ends_with('.'))
            result.push_back('0');
        return result;
    }

    /// parse an activity array from toml value
    std::vector<std::string> activityFromString(const toml::node_view<const toml::node>& val) {
        std::vector<std::string> active_in{};

        if (const auto& as_str = val.value<std::string>()) {
            active_in.push_back(*as_str);
        }

        if (const auto& as_arr = val.as_array()) {
            for (const auto& item : *as_arr) {
                if (const auto& item_str = item.value<std::string>())
                    active_in.push_back(*item_str);
            }
        }

        return active_in;
    }
    /// parse a pacing method from string
    Pacing parcingFromString(const std::string& str) {
        if (str == "none")
            return Pacing::None;
        throw ls::error("unknown pacing method: " + str);
    }
    /// validate the shared file/environment game-profile contract
    void validateGameConf(const GameConf& conf) {
        if (conf.multiplier < GameConfLimits::minimumMultiplier ||
                conf.multiplier > GameConfLimits::maximumMultiplier)
            throw ls::error(
                "multiplier must be between " + std::to_string(
                    GameConfLimits::minimumMultiplier
                ) + " and " + std::to_string(
                    GameConfLimits::maximumMultiplier
                )
            );
        if (conf.base_fps_cap > GameConfLimits::maximumBaseFpsCap)
            throw ls::error(
                "base_fps_cap must be " + std::to_string(
                    GameConfLimits::minimumBaseFpsCap
                ) + " or between " + std::to_string(
                    GameConfLimits::minimumBaseFpsCap + 1
                ) + " and " + std::to_string(
                    GameConfLimits::maximumBaseFpsCap
                )
            );
        if (conf.flow_scale < GameConfLimits::minimumFlowScale ||
                conf.flow_scale > GameConfLimits::maximumFlowScale) {
            throw ls::error(
                "flow_scale must be between " + formatFloatingLimit(
                    GameConfLimits::minimumFlowScale
                ) + " and " + formatFloatingLimit(
                    GameConfLimits::maximumFlowScale
                )
            );
        }
    }
    /// parse the global configuration
    GlobalConf parseGlobalConf(const toml::table& tbl) {
        const GlobalConf conf{
            .dll = tbl["dll"].value<std::string>(),
            .allow_fp16 = tbl["allow_fp16"].value_or(true),
            .pacing_correction = tbl["pacing_correction"].value_or(true),
            .real_present_lead = tbl["real_present_lead"].value_or(true),
            .default_profile = tbl["default_profile"].value<std::string>()
        };

        // Availability belongs to LSFG loading, not profile validity.
        // Preserve missing paths so the global section still loads.
        return conf;
    }
    /// parse a game profile configuration
    GameConf parseGameConf(const toml::table& tbl) {
        GameConf conf{
            .name = tbl["name"].value_or<std::string>("unnamed"),
            .active_in = activityFromString(tbl["active_in"]),
            .gpu = tbl["gpu"].value<std::string>(),
            .multiplier = tbl["multiplier"].value_or(GameConfDefaults::multiplier),
            .frame_generation_provisioned =
                tbl["frame_generation_provisioned"].value_or(
                    GameConfDefaults::frameGenerationProvisioned
                ),
            .frame_generation_enabled = tbl["frame_generation_enabled"].value_or(
                GameConfDefaults::frameGenerationEnabled
            ),
            .swapchain_image_count_compatibility =
                tbl["swapchain_image_count_compatibility"].value_or(
                    GameConfDefaults::swapchainImageCountCompatibility
                ),
            .base_fps_cap = tbl["base_fps_cap"].value_or(GameConfDefaults::baseFpsCap),
            .low_latency = tbl["low_latency"].value_or(GameConfDefaults::lowLatency),
            .reflex_dynamic_cap = tbl["reflex_dynamic_cap"].value_or(GameConfDefaults::reflexDynamicCap),
            .batch_hold = tbl["batch_hold"].value_or(GameConfDefaults::batchHold),
            .ultra_performance = tbl["ultra_performance"].value_or(
                GameConfDefaults::ultraPerformance
            ),
            .flow_scale = tbl["flow_scale"].value_or(GameConfDefaults::flowScale),
            .performance_mode = tbl["performance_mode"].value_or(
                GameConfDefaults::performanceMode
            ),
            .pacing = parcingFromString(tbl["pacing"].value_or<std::string>("none"))
        };

        if (conf.gpu && conf.gpu->empty()) conf.gpu.reset();
        validateGameConf(conf);
        return conf;
    }
    toml::table gameConfTable(const GameConf& conf) {
        toml::table profile;
        profile.insert("name", conf.name);

        if (!conf.active_in.empty()) {
            if (conf.active_in.size() == 1) {
                profile.insert("active_in", conf.active_in.front());
            } else {
                toml::array active_in;
                for (const auto& entry : conf.active_in)
                    active_in.push_back(entry);
                profile.insert("active_in", active_in);
            }
        }
        if (conf.gpu)
            profile.insert("gpu", conf.gpu.value_or(""));
        profile.insert("multiplier", static_cast<int64_t>(conf.multiplier));
        profile.insert(
            "frame_generation_provisioned",
            conf.frame_generation_provisioned
        );
        profile.insert("frame_generation_enabled", conf.frame_generation_enabled);
        profile.insert(
            "swapchain_image_count_compatibility",
            conf.swapchain_image_count_compatibility
        );
        profile.insert("base_fps_cap", static_cast<int64_t>(conf.base_fps_cap));
        profile.insert("low_latency", conf.low_latency);
        profile.insert("reflex_dynamic_cap", conf.reflex_dynamic_cap);
        profile.insert("batch_hold", conf.batch_hold);
        profile.insert("ultra_performance", conf.ultra_performance);
        profile.insert("flow_scale", conf.flow_scale);
        profile.insert("performance_mode", conf.performance_mode);
        switch (conf.pacing) {
            case Pacing::None:
                profile.insert("pacing", "none");
                break;
        }

        return profile;
    }
    /// parse the global configuration from the environment
    GlobalConf parseGlobalConfFromEnv() {
        GlobalConf conf{
            .dll = std::nullopt,
            .allow_fp16 = true,
            .pacing_correction = true,
            .real_present_lead = true
        };

        const char* dll = std::getenv("FGVK_DLL_PATH");
        if (dll && *dll != '\0')
            conf.dll = std::string(dll);
        const char* no_fp16 = std::getenv("FGVK_NO_FP16");
        if (no_fp16 && *no_fp16 != '\0')
            conf.allow_fp16 = std::string(no_fp16) != "1";

        return conf;
    }
    /// parse a game profile configuration from the environment
    GameConf parseGameConfFromEnv() {
        GameConf conf{
            .name = "(environment)",
            .active_in = {},
            .gpu = std::nullopt,

            .multiplier = GameConfDefaults::multiplier,
            .frame_generation_provisioned =
                GameConfDefaults::frameGenerationProvisioned,
            .frame_generation_enabled = GameConfDefaults::frameGenerationEnabled,
            .swapchain_image_count_compatibility =
                GameConfDefaults::swapchainImageCountCompatibility,
            .base_fps_cap = GameConfDefaults::baseFpsCap,
            .low_latency = GameConfDefaults::lowLatency,
            .reflex_dynamic_cap = GameConfDefaults::reflexDynamicCap,
            .batch_hold = GameConfDefaults::batchHold,
            .ultra_performance = GameConfDefaults::ultraPerformance,
            .flow_scale = GameConfDefaults::flowScale,
            .performance_mode = GameConfDefaults::performanceMode,
            .pacing = GameConfDefaults::pacing
        };

        const char* gpu = std::getenv("FGVK_GPU");
        if (gpu) conf.gpu = std::string(gpu);
        const char* multiplier = std::getenv("FGVK_MULTIPLIER");
        if (multiplier) conf.multiplier = static_cast<size_t>(std::stoul(multiplier));
        const char* frame_generation_provisioned =
            std::getenv("FGVK_FRAME_GENERATION_PROVISIONED");
        if (frame_generation_provisioned) {
            conf.frame_generation_provisioned =
                std::string(frame_generation_provisioned) != "0";
        }
        const char* frame_generation_enabled = std::getenv("FGVK_FRAME_GENERATION_ENABLED");
        if (frame_generation_enabled)
            conf.frame_generation_enabled = std::string(frame_generation_enabled) != "0";
        const char* swapchain_image_count_compatibility =
            std::getenv("FGVK_SWAPCHAIN_IMAGE_COUNT_COMPATIBILITY");
        if (swapchain_image_count_compatibility) {
            conf.swapchain_image_count_compatibility =
                std::string(swapchain_image_count_compatibility) != "0";
        }
        const char* base_fps_cap = std::getenv("FGVK_BASE_FPS_CAP");
        if (base_fps_cap)
            conf.base_fps_cap = static_cast<uint32_t>(std::stoul(base_fps_cap));
        const char* flow_scale = std::getenv("FGVK_FLOW_SCALE");
        if (flow_scale) conf.flow_scale = std::stof(flow_scale);
        const char* ultra_performance = std::getenv("FGVK_ULTRA_PERFORMANCE");
        if (ultra_performance)
            conf.ultra_performance = std::string(ultra_performance) == "1";
        const char* performance = std::getenv("FGVK_PERFORMANCE_MODE");
        if (performance) conf.performance_mode = std::string(performance) == "1";
        const char* pacing = std::getenv("FGVK_PACING");
        if (pacing) conf.pacing = parcingFromString(std::string(pacing));

        validateGameConf(conf);
        return conf;
    }
}

ConfigFile::ConfigFile(const std::filesystem::path& path) {
    toml::table table;
    try {
        table = toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
        throw ls::error("unable to parse configuration", e);
    }

    auto version = table["version"];
    if (!version || !version.is_integer() ||
            *version.as_integer() != ConfigFile::formatVersion) {
        throw ls::error("unsupported configuration version");
    }

    auto global = table["global"];
    if (global && global.is_table()) {
        this->globalConf = parseGlobalConf(*global.as_table());
    }

    auto profiles = table["profile"];
    if (profiles && profiles.is_array_of_tables()) {
        for (const auto& profile : *profiles.as_array()) {
            const auto& base = *profile.as_table();
            auto conf = parseGameConf(base);
            this->profileConfs.push_back(std::move(conf));
        }
    }
}

void ConfigFile::write(const std::filesystem::path& path) const {
    toml::table table;
    table.insert("version", ConfigFile::formatVersion);

    toml::table global;
    if (this->globalConf.dll)
        global.insert("dll", *this->globalConf.dll);
    global.insert("allow_fp16", this->globalConf.allow_fp16);
    global.insert("pacing_correction", this->globalConf.pacing_correction);
    global.insert("real_present_lead", this->globalConf.real_present_lead);
    if (this->globalConf.default_profile)
        global.insert("default_profile", *this->globalConf.default_profile);
    table.insert("global", global);

    toml::array profiles;
    for (const auto& conf : this->profileConfs) {
        auto profile = gameConfTable(conf);

        profiles.push_back(profile);
    }
    table.insert("profile", profiles);

    try {
        std::ostringstream ofs;

        ofs << toml::toml_formatter {
            table,
            toml::v3::format_flags::relaxed_float_precision
        } << '\n';
        detail::writeConfigurationAtomically(path, ofs.str());
    } catch (const std::exception& e) {
        throw ls::error("unable to write configuration file", e);
    }
}

WatchedConfig::WatchedConfig()
    : path(findConfigurationFile()) {
    if (std::getenv("FGVK_ENV")) {
        auto& config = this->configFile;
        config.global() = parseGlobalConfFromEnv();
        config.profiles() = { parseGameConfFromEnv() };

        return;
    }

    if (!std::filesystem::exists(this->path))
        ConfigFile::createDefaultConfigFile(this->path);

    this->configFile = ConfigFile(this->path);
    this->last_timestamp = std::filesystem::last_write_time(this->path);
}

WatchedConfig::~WatchedConfig() = default;
WatchedConfig::WatchedConfig(WatchedConfig&&) noexcept = default;
WatchedConfig& WatchedConfig::operator=(WatchedConfig&&) noexcept = default;

bool WatchedConfig::update() {
    if (std::getenv("FGVK_ENV"))
        return false;

    const auto now = std::filesystem::last_write_time(this->path);
    if (now == this->last_timestamp)
        return false;

    const auto retryNow = std::chrono::steady_clock::now();
    if (this->failed_timestamp && *this->failed_timestamp == now &&
            retryNow < this->next_parse_retry)
        return false;

    // Advance the observed timestamp only after parsing succeeds. An external
    // editor may briefly expose a partially-written TOML file; retaining the old stamp
    // makes the same version retryable instead of silently losing the update.
    ConfigFile new_config;
    try {
        new_config = ConfigFile(this->path);
    } catch (...) {
        this->failed_timestamp = now;
        this->next_parse_retry = retryNow + configurationParseRetryDelay;
        throw;
    }
    this->configFile = std::move(new_config);
    this->last_timestamp = now;
    this->failed_timestamp.reset();
    return true;
}

std::filesystem::path ls::findConfigurationFile() {
    // always honor FGVK_CONFIG if set
    const char* envPath = std::getenv("FGVK_CONFIG");
    if (envPath && *envPath != '\0')
        return{envPath};

    // then check the XDG overriden location
    const char* xdgPath = std::getenv("XDG_CONFIG_HOME");
    if (xdgPath && *xdgPath != '\0')
        return std::filesystem::path(xdgPath)
            / "framegen-vulkan" / "conf.toml";

    // fallback to typical user home
    const char* homePath = std::getenv("HOME");
    if (homePath && *homePath != '\0')
        return std::filesystem::path(homePath)
            / ".config" / "framegen-vulkan" / "conf.toml";

    // finally, use system-wide config
    return "/etc/framegen-vulkan/conf.toml";
}

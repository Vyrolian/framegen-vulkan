/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ls {

    /// global configuration
    struct GlobalConf {
        /// optional dll override
        std::optional<std::string> dll;
        /// request supported FP16 model arithmetic; false selects FP32
        bool allow_fp16{true};
        /// place each generated output on its own ideal point between its reals, from measured GPU
        /// present times; real outputs are never moved (FGVK_REAL_OFFSET_US=auto turns it on, any
        /// other value off); read once when the layer starts
        bool pacing_correction{true};
        /// present each real output after a generated output earlier by its measured GPU wait;
        /// FGVK_REAL_PRESENT_LEAD=1 or 0 overrides it; read on startup and on reload
        bool real_present_lead{true};
        /// optional profile for unmatched games launched through fgvk
        std::optional<std::string> default_profile{};
    };

    /// pacing methods
    enum class Pacing : uint8_t {
        /// do not perform any pacing (vsync+novrr)
        None
    };

    /// Renderer defaults used when a game profile omits fields
    struct GameConfDefaults {
        static constexpr size_t multiplier = 2;
        static constexpr bool frameGenerationProvisioned = true;
        static constexpr bool frameGenerationEnabled = true;
        static constexpr bool swapchainImageCountCompatibility = false;
        static constexpr uint32_t baseFpsCap = 0;
        static constexpr bool lowLatency = true;
        static constexpr bool reflexDynamicCap = true;
        static constexpr bool batchHold = true;
        static constexpr bool ultraPerformance = false;
        static constexpr float ultraPerformanceFlowScale = 0.7F;
        static constexpr float flowScale = 0.8F;
        static constexpr bool performanceMode = false;
        static constexpr Pacing pacing = Pacing::None;
    };

    /// ranges accepted by the Renderer configuration parser
    struct GameConfLimits {
        static constexpr size_t minimumMultiplier = 2;
        static constexpr size_t maximumMultiplier = 5;
        static constexpr uint32_t minimumBaseFpsCap = 0;
        static constexpr uint32_t maximumBaseFpsCap = 1000;
        static constexpr float minimumFlowScale = 0.25F;
        static constexpr float maximumFlowScale = 1.0F;
    };

    /// game profile configuration
    struct GameConf {
        /// name of the profile
        std::string name{"Profile"};
        /// optional activation string/array
        std::vector<std::string> active_in;
        /// gpu to use (in case of multiple)
        std::optional<std::string> gpu;
        /// multiplier for frame generation
        size_t multiplier{GameConfDefaults::multiplier};
        /// provision LSFG device interop and backend resources at process start
        bool frame_generation_provisioned{
            GameConfDefaults::frameGenerationProvisioned
        };
        /// allow frame synthesis to be toggled live without changing its mode
        bool frame_generation_enabled{GameConfDefaults::frameGenerationEnabled};
        /// preserve the application's requested swapchain image minimum
        bool swapchain_image_count_compatibility{
            GameConfDefaults::swapchainImageCountCompatibility
        };
        /// maximum application-present rate before frame generation; zero disables it
        uint32_t base_fps_cap{GameConfDefaults::baseFpsCap};
        /// release each capped frame just in time at its cap deadline instead of letting the game
        /// queue frames ahead; off keeps the release at the deadline (swapchain/low_latency.hpp)
        bool low_latency{GameConfDefaults::lowLatency};
        /// with the game's Reflex on and frame generation at a cap, keep the GPU just under full load so the game's
        /// frames do not queue in the GPU; never below the cap's interval; FGVK_REFLEX_DYNAMIC_CAP overrides it
        /// (swapchain/reflex_dynamic_cap.hpp)
        bool reflex_dynamic_cap{GameConfDefaults::reflexDynamicCap};
        /// start each released output batch about 2 ms after its frames are ready, phase-locked to the
        /// batch cadence, with low_latency on or off; off starts it at readiness (swapchain/batch_phase.hpp)
        bool batch_hold{GameConfDefaults::batchHold};
        /// trade live profile reloads for the lowest supported resource cost
        bool ultra_performance{GameConfDefaults::ultraPerformance};
        /// non-inverted flow scale
        float flow_scale{GameConfDefaults::flowScale};
        /// use performance mode
        bool performance_mode{GameConfDefaults::performanceMode};
        /// pacing method
        Pacing pacing{GameConfDefaults::pacing};
    };

    /// Flow scale selected for backend construction after applying presets.
    [[nodiscard]] constexpr float effectiveFlowScale(
            const GameConf& profile) noexcept {
        return profile.ultra_performance
            ? GameConfDefaults::ultraPerformanceFlowScale
            : profile.flow_scale;
    }

    /// Model selected for backend construction after applying presets.
    [[nodiscard]] constexpr bool effectivePerformanceMode(
            const GameConf& profile) noexcept {
        return profile.ultra_performance || profile.performance_mode;
    }

    /// parsed configuration file
    class ConfigFile {
    public:
        static constexpr int64_t formatVersion = 2;

        /// create a default configuration file at the given path
        /// @param path path to configuration file
        /// @throws ls::error on failure
        static void createDefaultConfigFile(const std::filesystem::path& path);

        /// load the default configuration
        /// @throws ls::error on failure
        ConfigFile();
        /// load configuration from file
        /// @param path path to configuration file
        /// @throws ls::error on failure
        ConfigFile(const std::filesystem::path& path);

        /// get the global configuration
        /// @return global configuration
        [[nodiscard]] auto& global() { return this->globalConf; }
        /// get the game profiles
        /// @return list of game profiles
        [[nodiscard]] auto& profiles() { return this->profileConfs; }

        /// get the global configuration
        /// @return global configuration
        [[nodiscard]] const auto& global() const { return this->globalConf; }
        /// get the game profiles
        /// @return list of game profiles
        [[nodiscard]] const auto& profiles() const { return this->profileConfs; }

        /// write the configuration back to file
        /// @param path path to configuration file
        /// @throws ls::error on failure
        void write(const std::filesystem::path& path) const;
    private:
        GlobalConf globalConf{};
        std::vector<GameConf> profileConfs;
    };

    /// configuration watcher with additional environment support
    class WatchedConfig {
    public:
        /// create a new configuration watcher
        /// @throws ls::error on failure
        explicit WatchedConfig();
        ~WatchedConfig();

        WatchedConfig(const WatchedConfig&) = delete;
        WatchedConfig& operator=(const WatchedConfig&) = delete;
        WatchedConfig(WatchedConfig&&) noexcept;
        WatchedConfig& operator=(WatchedConfig&&) noexcept;

        /// reload the configuration from disk if it has changed
        /// @throws ls::error on failure
        /// @return true if the configuration was reloaded
        bool update();

        /// access the underlying configuration file
        /// @return configuration file
        [[nodiscard]] const auto& get() const { return this->configFile; }
    private:
        ConfigFile configFile;

        std::filesystem::path path;
        std::chrono::time_point<std::chrono::file_clock> last_timestamp;
        std::optional<std::chrono::time_point<std::chrono::file_clock>>
            failed_timestamp;
        std::chrono::steady_clock::time_point next_parse_retry;
    };

    /// find the configuration file in the most common locations
    /// @return path to configuration file
    std::filesystem::path findConfigurationFile();

}

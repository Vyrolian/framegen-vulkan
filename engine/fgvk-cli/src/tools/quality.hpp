/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace fgvk::cli::quality {

    /// options for the "quality-regression" command
    struct Options {
        std::optional<std::string> dll;
        bool allow_fp16{true};
        std::optional<std::string> gpu;
        std::optional<std::filesystem::path> output;
        std::string scene{"motion-boundary"};
        float interpolation{0.5F};
        std::optional<std::string> sequence_plan;
        std::optional<uint32_t> width;
        std::optional<uint32_t> height;
        float flow_scale{1.0F};
        bool performance_mode{false};
    };

    /// run the deterministic GPU image-quality regression
    int run(const Options& opts);

    /// Options for proving that synchronization validation is active.
    struct SynchronizationCanaryOptions {
        std::optional<std::string> gpu;
    };

    /// Record one intentional transfer hazard for validation-tooling checks.
    int runSynchronizationCanary(const SynchronizationCanaryOptions& opts);
}

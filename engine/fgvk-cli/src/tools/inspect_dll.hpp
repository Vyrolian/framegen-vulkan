/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "fgvk-backend/ls1.hpp"

#include <filesystem>
#include <optional>

namespace fgvk::cli::inspect_dll {

    struct Options {
        std::filesystem::path dll;
        std::optional<backend::Ls1Mode> ls1Mode;
        float sharpness{0.8F};
        bool lsfg{false};
        bool allowFp16{true};
    };

    int run(const Options& options);

}

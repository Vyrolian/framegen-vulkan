/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <filesystem>

namespace fgvk::cli::inspect_dll {

    struct Options {
        std::filesystem::path dll;
        bool lsfg{false};
        bool allowFp16{true};
    };

    int run(const Options& options);

}

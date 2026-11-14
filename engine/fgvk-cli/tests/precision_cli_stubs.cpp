/* SPDX-License-Identifier: GPL-3.0-or-later */

// Link the real CLI parser to tool-entry probes so defaults and explicit
// precision choices can be checked without initializing Vulkan or a model.
#include "i18n.hpp"
#include "tools/inspect_dll.hpp"
#include "tools/validate.hpp"

#include <iostream>

namespace {
    int reportPrecision(const bool allowFp16) {
        std::cout << (allowFp16 ? "fp16-allowed" : "fp32") << '\n';
        return 0;
    }
}

namespace fgvk::cli {
    int inspect_dll::run(const Options& opts) {
        return opts.lsfg ? reportPrecision(opts.allowFp16) : 2;
    }
    int validate::run(const Options&, i18n::Language) { return 2; }
}

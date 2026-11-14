/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "inspect_dll.hpp"

#include "fgvk-backend/dll_inspection.hpp"

#include <exception>
#include <iostream>
#include <string>

namespace {
    void printStatus(const std::string& name,
            const fgvk::backend::ModelCompatibility& status) {
        std::cout << "  " << name << ": "
                  << (status.compatible ? "compatible" : "unsupported");
        if (!status.reason.empty())
            std::cout << " (" << status.reason << ')';
        std::cout << '\n';
    }
}

int fgvk::cli::inspect_dll::run(const Options& options) {
    if (options.lsfg) {
        const auto selected = fgvk::backend::inspectLsfgRegistry(
            options.dll, options.allowFp16);
        const bool compatible = selected.compatible;
        if (!compatible)
            std::cerr << "framegen-vulkan: LSFG availability inspection failed: "
                      << selected.reason << '\n';
        std::cout << "{\"schema_version\":1,\"compatible\":"
                  << (compatible ? "true" : "false") << "}\n";
        return compatible ? 0 : 1;
    }
    try {
        const auto inspection = fgvk::backend::inspectLosslessDll(options.dll);
        std::cout << "framegen-vulkan model DLL inspection\n"
                  << "  dll_sha256: " << inspection.fileSha256 << '\n'
                  << "  resource_layout_sha256: "
                  << inspection.resourceLayoutSha256 << '\n'
                  << "  file_size: " << inspection.fileSize << '\n'
                  << "  resource_count: " << inspection.resourceCount << '\n';
        printStatus("LSFG FP32 Quality", inspection.lsfgFp32Quality);
        printStatus("LSFG FP32 Performance", inspection.lsfgFp32Performance);
        printStatus("LSFG FP16 Quality", inspection.lsfgFp16Quality);
        printStatus("LSFG FP16 Performance", inspection.lsfgFp16Performance);

        const bool coreCompatible =
            inspection.lsfgFp32Quality.compatible &&
            inspection.lsfgFp32Performance.compatible;
        std::cout << "  result: "
                  << (coreCompatible ? "compatible" : "partially compatible")
                  << '\n';
        return coreCompatible ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "framegen-vulkan model DLL inspection failed: " << error.what() << '\n';
        return 1;
    }
}

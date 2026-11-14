/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "dll_reader.hpp"

#include <cstdint>
#include <optional>

namespace fgvk::backend {

    /// One coherent resource table in an immutable DLL archive. An offset
    /// preserves the supported table's stage/variant ordering; it never mixes
    /// individually plausible shaders from different tables or files.
    struct ModelResourceSelection {
        int64_t idOffset{0};

        [[nodiscard]] uint32_t resourceId(uint32_t canonicalId) const;
        [[nodiscard]] const std::vector<uint8_t>& resource(
            const DllResourceArchive& archive, uint32_t canonicalId) const;
    };

    /// Prefer the selected canonical model, then discover a unique relocated
    /// complete table. Cache successes and failures on the immutable archive.
    /// With no mode, resolve both modes together for the runtime registry so
    /// shared stages cannot come from a different table. Call only at setup.
    [[nodiscard]] ModelResourceSelection resolveLsfgModelResources(
        const DllResourceArchive& archive, bool fp16,
        std::optional<bool> performance = std::nullopt);

    /// DirectX fallback requires exact SM5.0 reflection even at canonical IDs.
    /// Discovery preserves both modes as one table; there is no FP16 lane.
    [[nodiscard]] ModelResourceSelection resolveLsfgDxbcModelResources(
        const DllResourceArchive& archive,
        std::optional<bool> performance = std::nullopt);

}

/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "fgvk-backend/fgvk.hpp"

namespace fgvk::backend {
    /// PQ BT.2020 input and output are converted around the model's linear
    /// scRGB BT.709 space. Linear scRGB is already the model's space.
    [[nodiscard]] constexpr bool requiresPqConversion(
            FrameEncoding encoding) noexcept {
        return encoding == FrameEncoding::Hdr10Pq;
    }

    /// Model constants select linear HDR/scRGB semantics for both HDR encodings.
    [[nodiscard]] constexpr bool usesHdrModel(FrameEncoding encoding) noexcept {
        return encoding == FrameEncoding::ScRgbLinear ||
            requiresPqConversion(encoding);
    }
}

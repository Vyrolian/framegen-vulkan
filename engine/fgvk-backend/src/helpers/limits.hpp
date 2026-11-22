/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "fgvk-common/vulkan/descriptor_pool.hpp"

#include <cstddef>

namespace fgvk::backend {
    /// calculate limits for descriptor pools
    /// @param count number of images
    /// @param perf whether performance mode is enabled
    /// @param colorConversion whether PQ conversion stages are present
    /// @return calculated limits
    vk::Limits calculateDescriptorPoolLimits(
        size_t count, bool perf, bool colorConversion = false
    );
    /// DG1: limits for the direct write sets of `count` lower images, two generate sets each. These sets come from a
    /// pool of their own, so the context pool keeps its size.
    /// @param count number of lower images
    /// @return calculated limits
    vk::Limits calculateDirectWriteDescriptorLimits(size_t count);
}

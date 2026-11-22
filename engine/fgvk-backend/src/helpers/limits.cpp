/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "limits.hpp"

#include "fgvk-common/vulkan/descriptor_pool.hpp"

#include <cstddef>
#include <cstdint>

using namespace fgvk;
using namespace fgvk::backend;

namespace {
    const vk::Limits BASE_LIMITS{
        .sets = 51,
        .uniform_buffers = 3,
        .samplers = 51,
        .sampled_images = 165,
        .storage_images = 172
    };
    const vk::Limits BASE_LIMITS_PERF{
        .sampled_images = 91,
        .storage_images = 102
    };
    const vk::Limits GEN_LIMITS{
        .sets = 93,
        .uniform_buffers = 54,
        .samplers = 147,
        .sampled_images = 567,
        .storage_images = 261
    };
    const vk::Limits GEN_LIMITS_PERF{
        .sampled_images = 339,
        .storage_images = 183
    };
}

vk::Limits backend::calculateDirectWriteDescriptorLimits(size_t count) {
    // Each Generate builds two sets, and each set binds five sampled images, one storage image, two samplers and one
    // uniform buffer (see Generate's constructor).
    const auto m = static_cast<uint32_t>(count);
    return vk::Limits{
        .sets = 2 * m,
        .uniform_buffers = 2 * m,
        .samplers = 4 * m,
        .sampled_images = 10 * m,
        .storage_images = 2 * m,
    };
}

vk::Limits backend::calculateDescriptorPoolLimits(
        size_t count, bool perf, bool colorConversion) {
    const auto m = static_cast<uint16_t>(count);

    vk::Limits a{BASE_LIMITS};
    vk::Limits b{GEN_LIMITS};
    if (perf) {
        a.sampled_images = BASE_LIMITS_PERF.sampled_images;
        b.sampled_images = GEN_LIMITS_PERF.sampled_images;
        a.storage_images = BASE_LIMITS_PERF.storage_images;
        b.storage_images = GEN_LIMITS_PERF.storage_images;
    }

    a.sets += b.sets * m;
    a.uniform_buffers += b.uniform_buffers * m;
    a.samplers += b.samplers * m;
    a.sampled_images += b.sampled_images * m;
    a.storage_images += b.storage_images * m;

    if (colorConversion) {
        // Two source decoders plus one encoder per generated-frame image.
        const auto conversionSets = static_cast<uint16_t>(2 + count);
        a.sets += conversionSets;
        a.samplers += conversionSets;
        a.sampled_images += conversionSets;
        a.storage_images += conversionSets;
    }
    return a;
}

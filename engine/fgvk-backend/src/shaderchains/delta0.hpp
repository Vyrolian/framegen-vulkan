/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "../helpers/managed_shader.hpp"
#include "../helpers/utils.hpp"
#include "fgvk-common/vulkan/command_buffer.hpp"
#include "fgvk-common/vulkan/image.hpp"
#include "fgvk-common/vulkan/vulkan.hpp"

#include <functional>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace ctx { struct Ctx; }

namespace fgvk::backend {
    /// delta shaderchain
    class Delta0 {
    public:
        /// create a delta shaderchain
        /// @param ctx context
        /// @param idx generated frame index
        /// @param sourceImages source images
        /// @param outputImages0 same-extent output scratch from Gamma0
        /// @param outputImages1 same-extent output scratch from Gamma1
        /// @param additionalInput0 additional input image
        /// @param additionalInput1 additional input image
        Delta0(const Ctx& ctx, size_t idx,
            const std::vector<std::vector<vk::Image>>& sourceImages,
            const std::vector<vk::Image>& outputImages0,
            const std::vector<vk::Image>& outputImages1,
            const vk::Image& additionalInput0,
            const vk::Image& additionalInput1);

        /// render the delta shaderchain
        /// @param vk the vulkan instance
        /// @param cmd command buffer
        /// @param idx frame index
        void render(const vk::Vulkan& vk, const vk::CommandBuffer& cmd, size_t idx) const;

        /// The dispatches render(idx) records, in order, for grouped recording.
        [[nodiscard]] std::vector<ManagedDispatch> dispatches(size_t idx) const {
            return {
                ManagedDispatch{std::cref(this->sets0.at(idx % this->sets0.size())), this->dispatchExtent},
                ManagedDispatch{std::cref(this->sets1.at(idx % this->sets1.size())), this->dispatchExtent},
            };
        }

    private:
        std::vector<ManagedShader> sets0;
        std::vector<ManagedShader> sets1;
        VkExtent2D dispatchExtent{};
    };
}

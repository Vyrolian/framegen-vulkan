/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "../helpers/managed_shader.hpp"
#include "../helpers/utils.hpp"
#include "fgvk-common/vulkan/command_buffer.hpp"
#include "fgvk-common/vulkan/image.hpp"
#include "fgvk-common/vulkan/vulkan.hpp"

#include <functional>
#include <memory>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace ctx { struct Ctx; }

namespace fgvk::backend {
    /// gamma shaderchain
    class Gamma0 {
    public:
        /// create a gamma shaderchain
        /// @param ctx context
        /// @param idx generated frame index
        /// @param sourceImages source images
        /// @param additionalInput additional input image
        /// @param sharedScratch same-context first-output resources, or null to own them
        Gamma0(const Ctx& ctx, size_t idx,
            const std::vector<std::vector<vk::Image>>& sourceImages,
            const vk::Image& additionalInput,
            const Gamma0* sharedScratch = nullptr);

        /// prepare the shaderchain initially
        /// @param images vector to fill with image handles
        void prepare(std::vector<VkImage>& images) const;

        /// render the gamma shaderchain
        /// @param vk the vulkan instance
        /// @param cmd command buffer
        /// @param idx frame index
        void render(const vk::Vulkan& vk, const vk::CommandBuffer& cmd, size_t idx) const;

        /// The dispatch render(idx) records, for grouped recording.
        [[nodiscard]] std::vector<ManagedDispatch> dispatches(size_t idx) const {
            return {ManagedDispatch{std::cref(this->sets.at(idx % this->sets.size())), this->dispatchExtent}};
        }

        /// get the generated images
        /// @return vector of images
        [[nodiscard]] const auto& getImages() const { return *this->images; }
    private:
        std::shared_ptr<std::vector<vk::Image>> images;

        std::vector<ManagedShader> sets;
        VkExtent2D dispatchExtent{};
    };
}

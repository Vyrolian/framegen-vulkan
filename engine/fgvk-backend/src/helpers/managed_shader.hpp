/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "fgvk-common/helpers/pointers.hpp"
#include "fgvk-common/vulkan/command_buffer.hpp"
#include "fgvk-common/vulkan/descriptor_pool.hpp"
#include "fgvk-common/vulkan/descriptor_set.hpp"
#include "fgvk-common/vulkan/shader.hpp"

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace fgvk::backend {

    /// GP2 switch: FGVK_BACKEND_OVERLAP=1, read once per process. When off, every chain
    /// records its serial dispatches exactly as before.
    [[nodiscard]] bool backendOverlapEnabled();

    class ManagedShader;
    struct ManagedDispatch {
        ls::R<const ManagedShader> shader;
        VkExtent2D extent;
    };

    /// managed shader handling dispatch and barriers
    /// this class is NOT memory-safe
    class ManagedShader {
        friend class ManagedShaderBuilder;
    public:
        /// dispatch the managed shader
        /// @param vk the vulkan instance
        /// @param cmd command buffer to use
        /// @param extent dispatch size
        /// @throws ls::vulkan_error on failure
        void dispatch(const vk::Vulkan& vk,
            const vk::CommandBuffer& cmd, VkExtent2D extent) const;
        /// Group only dispatches with no shared read/write or write/write images.
        static void dispatchIndependent(const vk::Vulkan& vk,
            const vk::CommandBuffer& cmd, std::span<const ManagedDispatch> dispatches);
        /// Record dispatches in their given order. Each maximal in-order run of
        /// independent dispatches shares one barrier; a dependence only starts a new
        /// run, so the recorded order and every input stay exactly as listed.
        /// @param afterRun called after each recorded run with its [begin, end) range
        static void dispatchGroups(const vk::Vulkan& vk,
            const vk::CommandBuffer& cmd, std::span<const ManagedDispatch> dispatches,
            const std::function<void(size_t, size_t)>& afterRun = {});
    private:
        // Records one run whose members the caller has proven independent.
        static void recordIndependentRun(const vk::Vulkan& vk,
            const vk::CommandBuffer& cmd, std::span<const ManagedDispatch> dispatches);

        ls::R<const vk::Shader> shader;

        std::vector<vk::Barrier> barriers;
        vk::DescriptorSet descriptorSet;

        // simple move constructor
        ManagedShader(ls::R<const vk::Shader> shader,
                std::vector<vk::Barrier> barriers,
                vk::DescriptorSet descriptorSet) :
            shader(shader),
            barriers(std::move(barriers)),
            descriptorSet(std::move(descriptorSet)) {
        }
    };

    /// class for building managed shaders
    /// this class is NOT memory-safe
    class ManagedShaderBuilder {
    public:
        /// default constructor
        ManagedShaderBuilder() = default;

        /// add a sampled image
        /// @param image image to add
        [[nodiscard]] ManagedShaderBuilder& sampled(const vk::Image& image);
        /// add multiple sampled images
        /// @param images images to add
        /// @param offset offset into images
        /// @param count number of images to add (0 = all)
        [[nodiscard]] ManagedShaderBuilder& sampleds(const std::vector<vk::Image>& images,
            size_t offset = 0, size_t count = 0);
        /// add an exact non-owning range of sampled images
        /// @param images images to add
        [[nodiscard]] ManagedShaderBuilder& sampleds(std::span<const vk::Image> images);

        /// add a storage image
        /// @param image image to add
        [[nodiscard]] ManagedShaderBuilder& storage(const vk::Image& image);
        /// add multiple storage images
        /// @param images images to add
        /// @param offset offset into images
        /// @param count number of images to add (0 = all)
        [[nodiscard]] ManagedShaderBuilder& storages(const std::vector<vk::Image>& images,
            size_t offset = 0, size_t count = 0);
        /// add an exact non-owning range of storage images
        /// @param images images to add
        [[nodiscard]] ManagedShaderBuilder& storages(std::span<const vk::Image> images);

        /// add a sampler
        /// @param sampler sampler to add
        [[nodiscard]] ManagedShaderBuilder& sampler(const vk::Sampler& sampler);
        /// add multiple samplers
        /// @param samplers samplers to add
        [[nodiscard]] ManagedShaderBuilder& samplers(const std::vector<vk::Sampler>& samplers);

        /// add a buffer
        /// @param buffer buffer to add
        [[nodiscard]] ManagedShaderBuilder& buffer(const vk::Buffer& buffer);

        /// build the managed shader
        /// @param vk the vulkan instance
        /// @param pool the descriptor pool to use
        /// @param shader the shader to use
        /// @returns the built managed shader
        [[nodiscard]] ManagedShader build(const vk::Vulkan& vk,
            const vk::DescriptorPool& pool, const vk::Shader& shader) const;
    private:
        std::vector<ls::R<const vk::Image>> sampledImages;
        std::vector<ls::R<const vk::Image>> storageImages;
        std::vector<ls::R<const vk::Sampler>> imageSamplers;
        std::vector<ls::R<const vk::Buffer>> constantBuffers;
    };

}

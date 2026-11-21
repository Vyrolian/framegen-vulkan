/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "managed_shader.hpp"
#include "independent_dispatch.hpp"
#include "fgvk-common/vulkan/backend_overlap.hpp"
#include "fgvk-common/vulkan/buffer.hpp"
#include "fgvk-common/vulkan/command_buffer.hpp"
#include "fgvk-common/vulkan/descriptor_pool.hpp"
#include "fgvk-common/vulkan/image.hpp"
#include "fgvk-common/vulkan/sampler.hpp"
#include "fgvk-common/vulkan/shader.hpp"
#include "fgvk-common/vulkan/vulkan.hpp"

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

using namespace fgvk;
using namespace fgvk::backend;

ManagedShaderBuilder& ManagedShaderBuilder::sampled(const vk::Image& image) {
    this->sampledImages.push_back(std::ref(image));
    return *this;
}

ManagedShaderBuilder& ManagedShaderBuilder::sampleds(
        const std::vector<vk::Image>& images,
        size_t offset, size_t count) {
    if (count == 0 || offset + count > images.size())
        count = images.size() - offset;

    return this->sampleds(std::span<const vk::Image>{images}.subspan(offset, count));
}

ManagedShaderBuilder& ManagedShaderBuilder::sampleds(
        const std::span<const vk::Image> images) {
    for (const auto& image : images)
        this->sampledImages.push_back(std::ref(image));
    return *this;
}


ManagedShaderBuilder& ManagedShaderBuilder::storage(const vk::Image& image) {
    this->storageImages.push_back(std::ref(image));
    return *this;
}

ManagedShaderBuilder& ManagedShaderBuilder::storages(
        const std::vector<vk::Image>& images,
        size_t offset, size_t count) {
    if (count == 0 || offset + count > images.size())
        count = images.size() - offset;

    return this->storages(std::span<const vk::Image>{images}.subspan(offset, count));
}

ManagedShaderBuilder& ManagedShaderBuilder::storages(
        const std::span<const vk::Image> images) {
    for (const auto& image : images)
        this->storageImages.push_back(std::ref(image));
    return *this;
}

ManagedShaderBuilder& ManagedShaderBuilder::sampler(const vk::Sampler& sampler) {
    this->imageSamplers.push_back(std::ref(sampler));
    return *this;
}

ManagedShaderBuilder& ManagedShaderBuilder::samplers(
        const std::vector<vk::Sampler>& samplers) {
    for (const auto& sampler : samplers)
        this->imageSamplers.push_back(std::ref(sampler));
    return *this;
}

ManagedShaderBuilder& ManagedShaderBuilder::buffer(const vk::Buffer& buffer) {
    this->constantBuffers.push_back(std::ref(buffer));
    return *this;
}

ManagedShader ManagedShaderBuilder::build(const vk::Vulkan& vk,
        const vk::DescriptorPool& pool, const vk::Shader& shader) const {
    std::vector<vk::Barrier> barriers;
    barriers.reserve(this->storageImages.size() + this->sampledImages.size());

    for (const auto& img : this->sampledImages)
        barriers.push_back({
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = img.get().handle(),
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1
            }
        });
    for (const auto& img : this->storageImages)
        barriers.push_back({
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = img.get().handle(),
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1
            }
        });

    return {
        std::ref(shader),
        std::move(barriers),
        vk::DescriptorSet(vk, pool, shader,
            this->sampledImages,
            this->storageImages,
            this->imageSamplers,
            this->constantBuffers)
    };
}

void ManagedShader::dispatch(const vk::Vulkan& vk, const vk::CommandBuffer& cmd,
        VkExtent2D extent) const {
    cmd.dispatch(vk, this->shader,
        this->descriptorSet,
        this->barriers,
        extent.width, extent.height, 1
    );
}

bool fgvk::backend::backendOverlapEnabled() {
    static const bool value = vk::parseBackendOverlap(std::getenv("FGVK_BACKEND_OVERLAP"));
    return value;
}

void ManagedShader::recordIndependentRun(const vk::Vulkan& vk,
        const vk::CommandBuffer& cmd, std::span<const ManagedDispatch> dispatches) {
    std::vector<vk::Barrier> barriers;
    for (const auto& dispatch : dispatches) {
        const auto& access = dispatch.shader.get().barriers;
        barriers.insert(barriers.end(), access.begin(), access.end());
    }
    if (dispatches.empty()) return;
    cmd.insertComputeBarriers(vk, barriers);
    for (const auto& dispatch : dispatches) {
        const auto& shader = dispatch.shader.get();
        cmd.dispatchAfterBarrier(vk, shader.shader, shader.descriptorSet,
            dispatch.extent.width, dispatch.extent.height, 1);
    }
}

void ManagedShader::dispatchIndependent(const vk::Vulkan& vk,
        const vk::CommandBuffer& cmd, std::span<const ManagedDispatch> dispatches) {
    std::vector<std::span<const vk::Barrier>> accesses;
    accesses.reserve(dispatches.size());
    for (const auto& dispatch : dispatches)
        accesses.emplace_back(dispatch.shader.get().barriers);
    if (!independentComputeDispatches(accesses))
        throw std::logic_error("Dependent shaders in an independent dispatch group");

    recordIndependentRun(vk, cmd, dispatches);
}

void ManagedShader::dispatchGroups(const vk::Vulkan& vk,
        const vk::CommandBuffer& cmd, std::span<const ManagedDispatch> dispatches,
        const std::function<void(size_t, size_t)>& afterRun) {
    std::vector<std::span<const vk::Barrier>> accesses;
    accesses.reserve(dispatches.size());
    for (const auto& dispatch : dispatches)
        accesses.emplace_back(dispatch.shader.get().barriers);

    size_t begin = 0;
    for (const size_t end : independentRunEnds(accesses)) {
        const auto run = dispatches.subspan(begin, end - begin);
        if (run.size() == 1)
            run.front().shader.get().dispatch(vk, cmd, run.front().extent);
        else
            recordIndependentRun(vk, cmd, run);
        if (afterRun)
            afterRun(begin, end);
        begin = end;
    }
}

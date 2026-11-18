/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace fgvk::layer::present_layout {
    // A framebuffer can select a compatible render pass long after the application's
    // create info has expired. Own the input arrays and extension chains required to
    // recreate it. Creation feedback writes only into the original application's call.
    class RenderPassDescription {
    public:
        explicit RenderPassDescription(const VkRenderPassCreateInfo& source) {
            legacy = source;
            legacy.pNext = copyChain(source.pNext);
            legacy.pAttachments = copy(source.pAttachments, source.attachmentCount);
            legacy.pDependencies = copy(source.pDependencies, source.dependencyCount);
            auto* subpasses = copy(source.pSubpasses, source.subpassCount);
            legacy.pSubpasses = subpasses;
            for (uint32_t i = 0; i < source.subpassCount; ++i)
                copySubpass(subpasses[i]);
        }

        explicit RenderPassDescription(const VkRenderPassCreateInfo2& source) {
            modern = source;
            modern.pNext = copyChain(source.pNext);
            modern.pAttachments = copyChainArray(source.pAttachments, source.attachmentCount);
            modern.pDependencies = copyChainArray(source.pDependencies, source.dependencyCount);
            modern.pCorrelatedViewMasks = copy(source.pCorrelatedViewMasks, source.correlatedViewMaskCount);
            auto* subpasses = copyChainArray(source.pSubpasses, source.subpassCount);
            modern.pSubpasses = subpasses;
            for (uint32_t i = 0; i < source.subpassCount; ++i)
                copySubpass(subpasses[i]);
        }

        VkRenderPassCreateInfo legacy{};
        VkRenderPassCreateInfo2 modern{};

    private:
        std::vector<std::shared_ptr<void>> allocations;

        template<typename T>
        T* copy(const T* input, uint32_t count) {
            if (!input || count == 0) return nullptr;
            auto array = std::make_shared<std::vector<T>>(input, input + count);
            auto* data = array->data();
            allocations.push_back(std::move(array));
            return data;
        }

        template<typename T>
        T* copyChainArray(const T* input, uint32_t count) {
            auto* output = copy(input, count);
            if (output) {
                for (uint32_t i = 0; i < count; ++i)
                    output[i].pNext = copyChain(output[i].pNext);
            }
            return output;
        }

        template<typename T>
        void copySubpass(T& subpass) {
            const auto attachments = [this](auto* refs, uint32_t count) {
                using Reference = std::remove_cv_t<std::remove_pointer_t<decltype(refs)>>;
                if constexpr (std::is_same_v<Reference, VkAttachmentReference2>)
                    return copyChainArray(refs, count);
                else
                    return copy(refs, count);
            };
            subpass.pInputAttachments = attachments(subpass.pInputAttachments, subpass.inputAttachmentCount);
            subpass.pColorAttachments = attachments(subpass.pColorAttachments, subpass.colorAttachmentCount);
            subpass.pResolveAttachments = attachments(subpass.pResolveAttachments, subpass.colorAttachmentCount);
            subpass.pDepthStencilAttachment = attachments(subpass.pDepthStencilAttachment, 1);
            subpass.pPreserveAttachments = copy(subpass.pPreserveAttachments, subpass.preserveAttachmentCount);
        }

        template<typename T>
        T* copyNode(const VkBaseInStructure* node) {
            auto* output = copy(reinterpret_cast<const T*>(node), 1);
            const void* next = copyChain(node->pNext);
            output->pNext = const_cast<void*>(next);
            return output;
        }

        const void* copyChain(const void* chain) {
            const auto* node = static_cast<const VkBaseInStructure*>(chain);
            if (!node) return nullptr;
            switch (node->sType) {
            case VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO: {
                auto* info = copyNode<VkRenderPassMultiviewCreateInfo>(node);
                info->pViewMasks = copy(info->pViewMasks, info->subpassCount);
                info->pViewOffsets = copy(info->pViewOffsets, info->dependencyCount);
                info->pCorrelationMasks = copy(info->pCorrelationMasks, info->correlationMaskCount);
                return info;
            }
            case VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO: {
                auto* info = copyNode<VkRenderPassInputAttachmentAspectCreateInfo>(node);
                info->pAspectReferences = copy(info->pAspectReferences, info->aspectReferenceCount);
                return info;
            }
            case VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE: {
                auto* info = copyNode<VkSubpassDescriptionDepthStencilResolve>(node);
                info->pDepthStencilResolveAttachment = copyChainArray(info->pDepthStencilResolveAttachment, 1);
                return info;
            }
            case VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR: {
                auto* info = copyNode<VkFragmentShadingRateAttachmentInfoKHR>(node);
                info->pFragmentShadingRateAttachment = copyChainArray(info->pFragmentShadingRateAttachment, 1);
                return info;
            }
            case VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT:
                return copyNode<VkRenderPassFragmentDensityMapCreateInfoEXT>(node);
            case VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT:
                return copyNode<VkAttachmentReferenceStencilLayout>(node);
            case VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT:
                return copyNode<VkAttachmentDescriptionStencilLayout>(node);
            case VK_STRUCTURE_TYPE_MEMORY_BARRIER_2:
                return copyNode<VkMemoryBarrier2>(node);
#ifdef VK_KHR_maintenance8
            case VK_STRUCTURE_TYPE_MEMORY_BARRIER_ACCESS_FLAGS_3_KHR:
                return copyNode<VkMemoryBarrierAccessFlags3KHR>(node);
#endif
            case VK_STRUCTURE_TYPE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT:
                return copyNode<VkMultisampledRenderToSingleSampledInfoEXT>(node);
            case VK_STRUCTURE_TYPE_RENDER_PASS_CREATION_CONTROL_EXT:
                return copyNode<VkRenderPassCreationControlEXT>(node);
            case VK_STRUCTURE_TYPE_RENDER_PASS_CREATION_FEEDBACK_CREATE_INFO_EXT:
            case VK_STRUCTURE_TYPE_RENDER_PASS_SUBPASS_FEEDBACK_CREATE_INFO_EXT:
                return copyChain(node->pNext);
#ifdef VK_QCOM_tile_memory_heap
            case VK_STRUCTURE_TYPE_TILE_MEMORY_SIZE_INFO_QCOM:
                return copyNode<VkTileMemorySizeInfoQCOM>(node);
#endif
#ifdef VK_QCOM_tile_shading
            case VK_STRUCTURE_TYPE_RENDER_PASS_TILE_SHADING_CREATE_INFO_QCOM:
                return copyNode<VkRenderPassTileShadingCreateInfoQCOM>(node);
#endif
#ifdef VK_ANDROID_external_memory_android_hardware_buffer
            case VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID:
                return copyNode<VkExternalFormatANDROID>(node);
#endif
#ifdef VK_OHOS_external_memory
            case VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_OHOS:
                return copyNode<VkExternalFormatOHOS>(node);
#endif
            default:
                throw std::invalid_argument("unsupported render pass extension");
            }
        }
    };
}

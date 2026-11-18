/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain/present_layout.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <map>
#include <cstdint>
#include <type_traits>
#include <string_view>
#include <vector>

using namespace fgvk::layer::present_layout;

namespace {
    void expect(bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            std::exit(1);
        }
    }

    template<typename Handle>
    Handle handle(uintptr_t value) {
        if constexpr (std::is_pointer_v<Handle>) return reinterpret_cast<Handle>(value);
        else return static_cast<Handle>(value);
    }

    struct Dispatchable {
        const void* table;
    };
    int table1, table2;
    Dispatchable device1{&table1}, device2{&table2}, cmd1{&table1}, cmd2{&table2};

    VkImage privateImage = handle<VkImage>(1);
    VkImage nativeImage = handle<VkImage>(2);
    uintptr_t nextHandle = 1;
    struct PassCall {
        std::vector<VkImageLayout> initial;
        std::vector<VkImageLayout> final;
        uint32_t viewMask{};
    };
    std::map<VkRenderPass, PassCall> passes;
    VkRenderPass recordedPass;
    std::vector<VkImageMemoryBarrier> barriers;
    std::vector<VkImageMemoryBarrier2> barriers2;
    VkResult nextPassError = VK_SUCCESS;
#ifdef VK_EXT_host_image_copy
    std::vector<VkHostImageLayoutTransitionInfoEXT> hostTransitions;

    VkResult VKAPI_CALL lowerHostTransition(VkDevice, uint32_t count,
            const VkHostImageLayoutTransitionInfoEXT* input) {
        hostTransitions.assign(input, input + count);
        return VK_SUCCESS;
    }
#endif

    template<typename Info>
    VkResult VKAPI_CALL lowerCreatePass(VkDevice, const Info* info,
            const VkAllocationCallbacks*, VkRenderPass* pass) {
        if (nextPassError != VK_SUCCESS) {
            *pass = VK_NULL_HANDLE;
            return nextPassError;
        }
        *pass = handle<VkRenderPass>(nextHandle++);
        auto& seen = passes[*pass];
        for (const auto& attachment : std::span{info->pAttachments, info->attachmentCount}) {
            seen.initial.push_back(attachment.initialLayout);
            seen.final.push_back(attachment.finalLayout);
        }
        if constexpr (std::is_same_v<Info, VkRenderPassCreateInfo>) {
            if (const auto* multiview = static_cast<const VkRenderPassMultiviewCreateInfo*>(info->pNext))
                seen.viewMask = multiview->pViewMasks[0];
        } else {
            seen.viewMask = info->pSubpasses[0].viewMask;
        }
        return VK_SUCCESS;
    }

    void VKAPI_CALL lowerDestroyPass(VkDevice, VkRenderPass pass, const VkAllocationCallbacks*) {
        expect(passes.erase(pass) == 1, "destroyed one render pass twice");
    }

    template<typename Info, typename Handle>
    VkResult VKAPI_CALL lowerCreate(VkDevice, const Info*, const VkAllocationCallbacks*, Handle* object) {
        *object = handle<Handle>(nextHandle++);
        return VK_SUCCESS;
    }

    template<typename Handle>
    void VKAPI_CALL lowerDestroy(VkDevice, Handle, const VkAllocationCallbacks*) {}

    template<typename Contents>
    void VKAPI_CALL lowerBeginPass(VkCommandBuffer, const VkRenderPassBeginInfo* info, Contents) {
        expect(passes.contains(info->renderPass), "render pass was not alive");
        recordedPass = info->renderPass;
    }

    VkResult VKAPI_CALL lowerBeginBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*) {
        return VK_SUCCESS;
    }

    VkResult VKAPI_CALL lowerEndBuffer(VkCommandBuffer) {
        return VK_SUCCESS;
    }

    void VKAPI_CALL lowerBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
            uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*,
            uint32_t count, const VkImageMemoryBarrier* images) {
        barriers.assign(images, images + count);
    }

    void VKAPI_CALL lowerWait(VkCommandBuffer buffer, uint32_t, const VkEvent*, VkPipelineStageFlags src,
            VkPipelineStageFlags dst, uint32_t memoryCount, const VkMemoryBarrier* memory,
            uint32_t bufferCount, const VkBufferMemoryBarrier* buffers,
            uint32_t imageCount, const VkImageMemoryBarrier* images) {
        lowerBarrier(buffer, src, dst, 0, memoryCount, memory, bufferCount, buffers, imageCount, images);
    }

    void VKAPI_CALL lowerBarrier2(VkCommandBuffer, const VkDependencyInfo* info) {
        barriers2.assign(info->pImageMemoryBarriers, info->pImageMemoryBarriers + info->imageMemoryBarrierCount);
    }

    void VKAPI_CALL lowerSet2(VkCommandBuffer buffer, VkEvent, const VkDependencyInfo* info) {
        lowerBarrier2(buffer, info);
    }

    void VKAPI_CALL lowerWait2(VkCommandBuffer buffer, uint32_t count, const VkEvent*, const VkDependencyInfo* infos) {
        expect(count == 2, "event dependency count changed");
        expect(infos[0].pImageMemoryBarriers->oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            "native dependency was modified");
        lowerBarrier2(buffer, &infos[1]);
    }

    PFN_vkVoidFunction VKAPI_CALL lower(VkDevice, const char* entry) {
        const std::string_view name(entry);
#define LOWER(vkName, ...) if (name == #vkName) return reinterpret_cast<PFN_vkVoidFunction>(__VA_ARGS__)
        LOWER(vkCreateRenderPass, lowerCreatePass<VkRenderPassCreateInfo>);
        LOWER(vkCreateRenderPass2KHR, lowerCreatePass<VkRenderPassCreateInfo2>);
        LOWER(vkCreateRenderPass2, lowerCreatePass<VkRenderPassCreateInfo2>);
        LOWER(vkDestroyRenderPass, lowerDestroyPass);
        LOWER(vkCreateImageView, lowerCreate<VkImageViewCreateInfo, VkImageView>);
        LOWER(vkDestroyImageView, lowerDestroy<VkImageView>);
        LOWER(vkCreateFramebuffer, lowerCreate<VkFramebufferCreateInfo, VkFramebuffer>);
        LOWER(vkDestroyFramebuffer, lowerDestroy<VkFramebuffer>);
        LOWER(vkCmdBeginRenderPass, lowerBeginPass<VkSubpassContents>);
        LOWER(vkCmdBeginRenderPass2KHR, lowerBeginPass<const VkSubpassBeginInfo*>);
        LOWER(vkCmdBeginRenderPass2, lowerBeginPass<const VkSubpassBeginInfo*>);
        LOWER(vkBeginCommandBuffer, lowerBeginBuffer);
        LOWER(vkEndCommandBuffer, lowerEndBuffer);
        LOWER(vkCmdPipelineBarrier, lowerBarrier);
        LOWER(vkCmdWaitEvents, lowerWait);
        LOWER(vkCmdPipelineBarrier2, lowerBarrier2);
        LOWER(vkCmdPipelineBarrier2KHR, lowerBarrier2);
        LOWER(vkCmdSetEvent2, lowerSet2);
        LOWER(vkCmdSetEvent2KHR, lowerSet2);
        LOWER(vkCmdWaitEvents2, lowerWait2);
        LOWER(vkCmdWaitEvents2KHR, lowerWait2);
#ifdef VK_EXT_host_image_copy
        LOWER(vkTransitionImageLayout, lowerHostTransition);
        LOWER(vkTransitionImageLayoutEXT, lowerHostTransition);
#endif
#undef LOWER
        return nullptr;
    }

    template<typename Fn>
    Fn hook(const char* name) {
        const auto pointer = command(name);
        expect(pointer != nullptr, "command override is absent");
        return reinterpret_cast<Fn>(pointer);
    }

    void testBarriers(VkCommandBuffer app, VkCommandBuffer otherDevice) {
        const auto present = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        constexpr auto attachment = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        std::array<VkImageMemoryBarrier, 9> input{};
        input.fill({.oldLayout = present, .newLayout = attachment, .srcQueueFamilyIndex = 5,
            .dstQueueFamilyIndex = 7, .image = nativeImage});
        input.back().image = privateImage;
        const auto barrier = hook<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
        barrier(app, 0, 0, 0, 0, nullptr, 0, nullptr, input.size(), input.data());
        expect(barriers.front().oldLayout == present, "native barrier was modified");
        expect(barriers.back().oldLayout == backingLayout && barriers.back().newLayout == attachment,
            "legacy source layout did not translate");
        expect(barriers.back().srcQueueFamilyIndex == 5 && barriers.back().dstQueueFamilyIndex == 7,
            "application queue family indices changed");
        expect(input.back().oldLayout == present, "caller barrier storage changed");
        barrier(otherDevice, 0, 0, 0, 0, nullptr, 0, nullptr, input.size(), input.data());
        expect(barriers.back().oldLayout == present, "another device's identical image handle was translated");

        const auto wait = hook<PFN_vkCmdWaitEvents>("vkCmdWaitEvents");
        wait(app, 0, nullptr, 0, 0, 0, nullptr, 0, nullptr, 1, &input.back());
        expect(barriers.front().oldLayout == backingLayout, "legacy wait did not translate");

        const std::array<VkImageMemoryBarrier2, 2> modern{{
            {.oldLayout = present, .newLayout = attachment, .image = nativeImage},
            {.oldLayout = attachment, .newLayout = present, .image = privateImage},
        }};
        const VkDependencyInfo info{.imageMemoryBarrierCount = modern.size(), .pImageMemoryBarriers = modern.data()};
        auto check = [&] {
            expect(barriers2[0].oldLayout == present && barriers2[1].oldLayout == attachment &&
                barriers2[1].newLayout == backingLayout, "dependency translated the wrong layouts");
        };
        for (const auto* name : {"vkCmdPipelineBarrier2", "vkCmdPipelineBarrier2KHR"}) {
            hook<PFN_vkCmdPipelineBarrier2>(name)(app, &info);
            check();
        }
        for (const auto* name : {"vkCmdSetEvent2", "vkCmdSetEvent2KHR"}) {
            hook<PFN_vkCmdSetEvent2>(name)(app, VK_NULL_HANDLE, &info);
            check();
        }
        const std::array dependencies{
            VkDependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = modern.data()}, info};
        for (const auto* name : {"vkCmdWaitEvents2", "vkCmdWaitEvents2KHR"}) {
            hook<PFN_vkCmdWaitEvents2>(name)(app, dependencies.size(), nullptr, dependencies.data());
            check();
        }
    }

    void testPasses(VkDevice device, VkCommandBuffer app) {
        const auto present = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkRenderPass pass{};
        {
            uint32_t viewMask = 3;
            const VkRenderPassMultiviewCreateInfo views{
                .sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,
                .subpassCount = 1, .pViewMasks = &viewMask};
            VkSubpassDescription subpass{};
            std::array<VkAttachmentDescription, 2> attachments{};
            attachments.fill({.initialLayout = present, .finalLayout = present});
            const VkRenderPassCreateInfo create{.pNext = &views, .attachmentCount = attachments.size(),
                .pAttachments = attachments.data(), .subpassCount = 1, .pSubpasses = &subpass};
            expect(hook<PFN_vkCreateRenderPass>("vkCreateRenderPass")(device, &create, nullptr, &pass) ==
                VK_SUCCESS, "legacy render pass creation failed");
            expect(attachments[0].initialLayout == present, "create modified app storage");
            viewMask = 7;
        }

        const auto createView = hook<PFN_vkCreateImageView>("vkCreateImageView");
        const VkImageViewCreateInfo privateViewInfo{.image = privateImage};
        const VkImageViewCreateInfo nativeViewInfo{.image = nativeImage};
        VkImageView privateView{}, nativeView{};
        expect(createView(device, &privateViewInfo, nullptr, &privateView) == VK_SUCCESS &&
            createView(device, &nativeViewInfo, nullptr, &nativeView) == VK_SUCCESS, "view creation failed");

        const auto createFramebuffer = hook<PFN_vkCreateFramebuffer>("vkCreateFramebuffer");
        const std::array attachments{privateView, nativeView};
        VkFramebuffer framebuffer{};
        const VkFramebufferCreateInfo fbInfo{.renderPass = pass, .attachmentCount = attachments.size(),
            .pAttachments = attachments.data()};
        expect(createFramebuffer(device, &fbInfo, nullptr, &framebuffer) == VK_SUCCESS, "framebuffer creation failed");

        VkFramebuffer imageless{};
        const VkFramebufferCreateInfo imagelessInfo{.flags = VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT,
            .renderPass = pass, .attachmentCount = attachments.size()};
        expect(createFramebuffer(device, &imagelessInfo, nullptr, &imageless) == VK_SUCCESS,
            "imageless framebuffer creation failed");
        const auto begin = hook<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass");
        VkRenderPassBeginInfo beginInfo{.renderPass = pass, .framebuffer = framebuffer};
        begin(app, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
        const auto alternate = recordedPass;
        expect(alternate != pass && beginInfo.renderPass == pass, "begin did not choose a compatible variant");
        expect(passes.at(alternate).viewMask == 3, "render pass extension data was borrowed");
        expect(passes.at(alternate).initial == std::vector{backingLayout, present} &&
            passes.at(alternate).final == std::vector{backingLayout, present},
            "mixed framebuffer translated the wrong implicit transitions");
        const VkRenderPassAttachmentBeginInfo emptyAttachments{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO};
        beginInfo.pNext = &emptyAttachments;
        begin(app, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
        expect(recordedPass == alternate && passes.size() == 2,
            "fixed framebuffer with an empty attachment node did not reuse the variant");
        const std::array nativeOnly{nativeView, nativeView};
        const VkRenderPassAttachmentBeginInfo nativeBegin{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO,
            .attachmentCount = nativeOnly.size(), .pAttachments = nativeOnly.data()};
        beginInfo.pNext = &nativeBegin;
        beginInfo.framebuffer = imageless;
        begin(app, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
        expect(recordedPass == pass, "native-only imageless framebuffer used a variant");

        const std::array reversed{nativeView, privateView};
        const VkRenderPassAttachmentBeginInfo beginAttachments{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO,
            .attachmentCount = reversed.size(), .pAttachments = reversed.data()};
        beginInfo.pNext = &beginAttachments;
        nextPassError = VK_ERROR_OUT_OF_HOST_MEMORY;
        begin(app, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
        const auto end = hook<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");
        expect(end(app) == VK_ERROR_OUT_OF_HOST_MEMORY, "variant allocation failure was hidden");
        nextPassError = VK_SUCCESS;
        begin(app, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
        expect(passes.at(recordedPass).final == std::vector{present, backingLayout},
            "imageless framebuffer used the wrong attachment indices");
        expect(end(app) == VK_SUCCESS, "successful command recording retained an earlier failure");

        const auto destroyPass = hook<PFN_vkDestroyRenderPass>("vkDestroyRenderPass");
        destroyPass(device, pass, nullptr);
        expect(passes.empty(), "variant outlived its application's render pass");

        for (const auto* createName : {"vkCreateRenderPass2", "vkCreateRenderPass2KHR"}) {
            VkAttachmentDescription2 attachment2{.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = present};
            VkSubpassDescription2 subpass{.viewMask = 5};
            const VkRenderPassCreateInfo2 create2{.attachmentCount = 1, .pAttachments = &attachment2,
                .subpassCount = 1, .pSubpasses = &subpass};
            expect(hook<PFN_vkCreateRenderPass2>(createName)(device, &create2, nullptr, &pass) == VK_SUCCESS,
                "renderpass2 creation failed");
            attachment2.finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            subpass.viewMask = 8;
            beginInfo = {.renderPass = pass, .framebuffer = framebuffer};
            for (const auto* name : {"vkCmdBeginRenderPass2", "vkCmdBeginRenderPass2KHR"}) {
                hook<PFN_vkCmdBeginRenderPass2>(name)(app, &beginInfo, nullptr);
                expect(passes.at(recordedPass).viewMask == 5 &&
                    passes.at(recordedPass).initial == std::vector{VK_IMAGE_LAYOUT_UNDEFINED} &&
                    passes.at(recordedPass).final == std::vector{backingLayout},
                    "renderpass2 changed a non-present layout or did not retain its input");
            }
            destroyPass(device, pass, nullptr);
        }
        const auto destroyFramebuffer = hook<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer");
        destroyFramebuffer(device, framebuffer, nullptr);
        destroyFramebuffer(device, imageless, nullptr);
        const auto destroyView = hook<PFN_vkDestroyImageView>("vkDestroyImageView");
        destroyView(device, privateView, nullptr);
        destroyView(device, nativeView, nullptr);
    }
}

int main() {
    const auto device = reinterpret_cast<VkDevice>(&device1);
    const auto other = reinterpret_cast<VkDevice>(&device2);
    const auto buffer = reinterpret_cast<VkCommandBuffer>(&cmd1);
    const auto otherBuffer = reinterpret_cast<VkCommandBuffer>(&cmd2);
    expect(registerDevice(device, lower) && registerDevice(other, lower), "device registration failed");
    expect(registerImages(device, std::span{&privateImage, 1}), "private image registration failed");
    testBarriers(buffer, otherBuffer);
    testPasses(device, buffer);
#ifdef VK_EXT_host_image_copy
    const std::array<VkHostImageLayoutTransitionInfoEXT, 2> host{{
        {.image = privateImage, .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR},
        {.image = nativeImage, .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR},
    }};
    for (const auto* name : {"vkTransitionImageLayout", "vkTransitionImageLayoutEXT"}) {
        const auto transition = hook<PFN_vkTransitionImageLayoutEXT>(name);
        expect(transition(device, host.size(), host.data()) == VK_SUCCESS &&
            hostTransitions[0].oldLayout == backingLayout &&
            hostTransitions[1].oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            "host layout transition translated the wrong image");
    }
#endif
    unregisterImages(device, std::span{&privateImage, 1});
    const VkImageMemoryBarrier barrier{.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, .image = privateImage};
    hook<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(buffer, 0, 0, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    expect(barriers.front().oldLayout == barrier.oldLayout, "retired private handle still got translated");
    unregisterDevice(device);
    unregisterDevice(other);
    expect(registerDevice(device, lower), "loader dispatch table address could not be reused");
    hook<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(buffer, 0, 0, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    expect(barriers.front().oldLayout == barrier.oldLayout, "retired device state survived table address reuse");
    unregisterDevice(device);
}

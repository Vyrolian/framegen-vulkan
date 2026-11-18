/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain/present_layout.hpp"
#include "swapchain/render_pass_description.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fgvk::layer::present_layout {
namespace {
    constexpr bool isPresent(VkImageLayout layout) noexcept {
        return layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }

    template<typename T>
    bool usesPresent(const T& attachment) {
        return isPresent(attachment.initialLayout) || isPresent(attachment.finalLayout);
    }

    // A small render pass fits without allocating during recording. The spill
    // path accepts any attachment count the application can create.
    class AttachmentIndices {
    public:
        void push(uint32_t index) {
            if (size < local.size()) {
                local[size++] = index;
                return;
            }
            if (overflow.empty()) overflow.assign(local.begin(), local.end());
            overflow.push_back(index);
            ++size;
        }

        std::span<const uint32_t> indices() const {
            return overflow.empty() ? std::span{local}.first(size) : std::span{overflow};
        }

    private:
        std::array<uint32_t, 8> local{};
        size_t size{};
        std::vector<uint32_t> overflow;
    };

    struct CompareIndices {
        using is_transparent = void;
        bool operator()(std::span<const uint32_t> a, std::span<const uint32_t> b) const {
            return std::ranges::lexicographical_compare(a, b);
        }
    };

    struct Pass {
        template<typename Info, typename Create>
        Pass(const Info& info, Create create) {
            try {
                description.emplace(info);
            } catch (const std::invalid_argument&) {
                // A newer extension can still run on native framebuffers.
            }
            if constexpr (std::is_same_v<Info, VkRenderPassCreateInfo2>) create2 = create;
            for (uint32_t i = 0; i < info.attachmentCount; ++i) {
                if (usesPresent(info.pAttachments[i])) presentAttachments.push_back(i);
            }
        }

        template<typename Info>
        static Info adjust(Info info, std::span<const uint32_t> indices,
                std::vector<std::remove_cv_t<std::remove_pointer_t<decltype(info.pAttachments)>>>& attachments) {
            attachments.assign(info.pAttachments, info.pAttachments + info.attachmentCount);
            for (const auto i : indices) {
                auto& attachment = attachments[i];
                attachment.initialLayout = substitute(attachment.initialLayout);
                attachment.finalLayout = substitute(attachment.finalLayout);
            }
            info.pAttachments = attachments.data();
            return info;
        }

        VkResult variant(VkDevice device, PFN_vkCreateRenderPass create,
                std::span<const uint32_t> indices, VkRenderPass& result) {
            if (!description) return VK_ERROR_UNKNOWN;
            {
                const std::shared_lock lock(mutex);
                if (auto found = variants.find(indices); found != variants.end()) {
                    result = found->second;
                    return VK_SUCCESS;
                }
            }
            const std::unique_lock lock(mutex);
            if (auto found = variants.find(indices); found != variants.end()) {
                result = found->second;
                return VK_SUCCESS;
            }
            auto slot = variants.emplace(std::vector<uint32_t>(indices.begin(), indices.end()),
                VK_NULL_HANDLE).first;
            try {
                VkResult status;
                if (create2) {
                    std::vector<VkAttachmentDescription2> attachments;
                    const auto adjusted = adjust(description->modern, indices, attachments);
                    status = create2(device, &adjusted, nullptr, &slot->second);
                } else {
                    std::vector<VkAttachmentDescription> attachments;
                    const auto adjusted = adjust(description->legacy, indices, attachments);
                    status = create(device, &adjusted, nullptr, &slot->second);
                }
                if (status == VK_SUCCESS) result = slot->second;
                else variants.erase(slot);
                return status;
            } catch (...) {
                variants.erase(slot);
                throw;
            }
        }

        std::optional<RenderPassDescription> description;
        size_t references{1};
        PFN_vkCreateRenderPass2 create2{};
        std::vector<uint32_t> presentAttachments;
        std::shared_mutex mutex;
        std::map<std::vector<uint32_t>, VkRenderPass, CompareIndices> variants;
    };

    struct Device {
        Device(VkDevice handle, PFN_vkGetDeviceProcAddr get) : handle(handle), get(get),
            createPass(reinterpret_cast<PFN_vkCreateRenderPass>(get(handle, "vkCreateRenderPass"))),
            destroyPass(reinterpret_cast<PFN_vkDestroyRenderPass>(get(handle, "vkDestroyRenderPass"))) {}

        VkDevice handle;
        PFN_vkGetDeviceProcAddr get;
        PFN_vkCreateRenderPass createPass;
        PFN_vkDestroyRenderPass destroyPass;
        std::atomic_size_t imageCount{0};
        std::shared_mutex objects;
        std::unordered_set<VkImage> images;
        // Vulkan permits distinct creates to return the same non-dispatchable handle.
        std::unordered_map<VkImageView, size_t> views;
        struct Framebuffer {
            std::vector<uint32_t> privateAttachments;
            size_t references{1};
        };
        std::unordered_map<VkFramebuffer, Framebuffer> framebuffers;
        std::unordered_map<VkRenderPass, std::unique_ptr<Pass>> passes;
        std::mutex recordingErrorsMutex;
        std::unordered_map<VkCommandBuffer, VkResult> recordingErrors;
        std::atomic_bool hasRecordingErrors{false};
        std::atomic_bool lostRecordingError{false};

        void recordError(VkCommandBuffer buffer, VkResult result) noexcept {
            const std::lock_guard lock(recordingErrorsMutex);
            try {
                recordingErrors.try_emplace(buffer, result);
                hasRecordingErrors.store(true, std::memory_order_release);
            } catch (const std::bad_alloc&) {
                lostRecordingError.store(true, std::memory_order_release);
            }
        }

        VkResult consumeError(VkCommandBuffer buffer) {
            if (lostRecordingError.load(std::memory_order_acquire)) return VK_ERROR_OUT_OF_HOST_MEMORY;
            if (!hasRecordingErrors.load(std::memory_order_acquire)) return VK_SUCCESS;
            const std::lock_guard lock(recordingErrorsMutex);
            const auto entry = recordingErrors.extract(buffer);
            hasRecordingErrors.store(!recordingErrors.empty(), std::memory_order_release);
            return entry.empty() ? VK_SUCCESS : entry.mapped();
        }
    };

    std::shared_mutex devicesMutex;
    std::unordered_map<void*, std::unique_ptr<Device>> devices;
    std::atomic_uint64_t deviceEpoch{0};

    // The Vulkan loader puts the same dispatch-table pointer at the head of a
    // device and its command buffers. The next layer still receives the original handle.
    template<typename Handle>
    void* dispatchKey(Handle handle) {
        void* key;
        std::memcpy(&key, handle, sizeof(key));
        return key;
    }

    Device& deviceFor(void* key, uint64_t epoch) {
        struct Cache { void* key{}; uint64_t epoch{UINT64_MAX}; Device* state{}; };
        static thread_local Cache cache;
        if (cache.key != key || cache.epoch != epoch) {
            const std::shared_lock lock(devicesMutex);
            cache = {key, epoch, devices.at(key).get()};
        }
        return *cache.state;
    }

    template<typename Function, const char* Name, typename Handle>
    auto dispatch(Handle handle) {
        struct Cache {
            void* key{};
            uint64_t epoch{UINT64_MAX};
            Device* state{};
            Function next{};
        };
        static thread_local Cache cache;
        const auto key = dispatchKey(handle);
        const auto epoch = deviceEpoch.load(std::memory_order_acquire);
        if (cache.key != key || cache.epoch != epoch) {
            auto& state = deviceFor(key, epoch);
            cache = {key, epoch, &state, reinterpret_cast<Function>(state.get(state.handle, Name))};
        }
        return std::pair<Device&, Function>{*cache.state, cache.next};
    }

    constexpr char createViewName[] = "vkCreateImageView";
    VkResult VKAPI_CALL createView(VkDevice device, const VkImageViewCreateInfo* info,
            const VkAllocationCallbacks* alloc, VkImageView* view) {
        auto [state, next] = dispatch<PFN_vkCreateImageView, createViewName>(device);
        bool track = false;
        if (state.imageCount.load(std::memory_order_acquire)) {
            const std::shared_lock lock(state.objects);
            track = state.images.contains(info->image);
        }
        const auto status = next(device, info, alloc, view);
        if (status != VK_SUCCESS || !track) return status;
        try {
            const std::unique_lock lock(state.objects);
            ++state.views[*view];
            return status;
        } catch (const std::bad_alloc&) {
            const auto destroy = reinterpret_cast<PFN_vkDestroyImageView>(
                state.get(device, "vkDestroyImageView"));
            destroy(device, *view, alloc);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    constexpr char destroyViewName[] = "vkDestroyImageView";
    void VKAPI_CALL destroyView(VkDevice device, VkImageView view, const VkAllocationCallbacks* alloc) {
        auto [state, next] = dispatch<PFN_vkDestroyImageView, destroyViewName>(device);
        {
            const std::unique_lock lock(state.objects);
            const auto found = state.views.find(view);
            if (found != state.views.end() && --found->second == 0)
                state.views.erase(found);
        }
        next(device, view, alloc);
    }

    constexpr char createFramebufferName[] = "vkCreateFramebuffer";
    VkResult VKAPI_CALL createFramebuffer(VkDevice device, const VkFramebufferCreateInfo* info,
            const VkAllocationCallbacks* alloc, VkFramebuffer* framebuffer) {
        auto [state, next] = dispatch<PFN_vkCreateFramebuffer, createFramebufferName>(device);
        try {
            std::vector<uint32_t> indices;
            if (!(info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) &&
                    state.imageCount.load(std::memory_order_acquire)) {
                const std::shared_lock lock(state.objects);
                for (uint32_t i = 0; i < info->attachmentCount; ++i) {
                    if (state.views.contains(info->pAttachments[i])) indices.push_back(i);
                }
            }
            const auto status = next(device, info, alloc, framebuffer);
            if (status != VK_SUCCESS || indices.empty()) return status;
            try {
                const std::unique_lock lock(state.objects);
                const auto [entry, inserted] = state.framebuffers.try_emplace(
                    *framebuffer, Device::Framebuffer{std::move(indices)});
                if (!inserted) ++entry->second.references;
            } catch (const std::bad_alloc&) {
                const auto destroy = reinterpret_cast<PFN_vkDestroyFramebuffer>(
                    state.get(device, "vkDestroyFramebuffer"));
                destroy(device, *framebuffer, alloc);
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            return status;
        } catch (const std::bad_alloc&) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    constexpr char destroyFramebufferName[] = "vkDestroyFramebuffer";
    void VKAPI_CALL destroyFramebuffer(VkDevice device, VkFramebuffer framebuffer,
            const VkAllocationCallbacks* alloc) {
        auto [state, next] = dispatch<PFN_vkDestroyFramebuffer, destroyFramebufferName>(device);
        {
            const std::unique_lock lock(state.objects);
            const auto found = state.framebuffers.find(framebuffer);
            if (found != state.framebuffers.end() && --found->second.references == 0)
                state.framebuffers.erase(found);
        }
        next(device, framebuffer, alloc);
    }

    constexpr char createPassName[] = "vkCreateRenderPass";
    constexpr char createPass2Name[] = "vkCreateRenderPass2";
    constexpr char createPass2KhrName[] = "vkCreateRenderPass2KHR";

    template<const char* Name, typename Info, typename Function>
    VkResult VKAPI_CALL createRenderPass(VkDevice device, const Info* info,
            const VkAllocationCallbacks* alloc, VkRenderPass* renderPass) {
        auto [state, next] = dispatch<Function, Name>(device);
        const auto attachments = std::span(info->pAttachments, info->attachmentCount);
        if (std::ranges::none_of(attachments, usesPresent<typename decltype(attachments)::value_type>))
            return next(device, info, alloc, renderPass);

        try {
            auto pass = std::make_unique<Pass>(*info, next);
            const auto status = next(device, info, alloc, renderPass);
            if (status != VK_SUCCESS) return status;

            try {
                const std::unique_lock lock(state.objects);
                const auto [entry, inserted] = state.passes.try_emplace(*renderPass, std::move(pass));
                if (!inserted) ++entry->second->references;
            } catch (const std::bad_alloc&) {
                state.destroyPass(device, *renderPass, alloc);
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            return VK_SUCCESS;
        } catch (const std::bad_alloc&) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    constexpr char destroyPassName[] = "vkDestroyRenderPass";
    void VKAPI_CALL destroyRenderPass(VkDevice device, VkRenderPass pass, const VkAllocationCallbacks* alloc) {
        auto [state, next] = dispatch<PFN_vkDestroyRenderPass, destroyPassName>(device);
        std::unique_ptr<Pass> owner;
        {
            const std::unique_lock lock(state.objects);
            const auto entry = state.passes.find(pass);
            if (entry != state.passes.end() && --entry->second->references == 0) {
                owner = std::move(entry->second);
                state.passes.erase(entry);
            }
        }
        if (owner) {
            for (const auto& variant : owner->variants)
                next(device, variant.second, nullptr);
        }
        next(device, pass, alloc);
    }

    const VkRenderPassAttachmentBeginInfo* imagelessAttachments(const void* chain) {
        for (auto* node = static_cast<const VkBaseInStructure*>(chain); node; node = node->pNext) {
            if (node->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO)
                return reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(node);
        }
        return nullptr;
    }

    VkResult selectPass(Device& state, const VkRenderPassBeginInfo& info, VkRenderPass& renderPass) {
        if (!state.imageCount.load(std::memory_order_acquire)) return VK_SUCCESS;
        AttachmentIndices indices;
        Pass* pass;
        {
            const std::shared_lock lock(state.objects);
            auto it = state.passes.find(info.renderPass);
            if (it == state.passes.end()) return VK_SUCCESS;
            pass = it->second.get(); // Externally synchronized against vkDestroyRenderPass.

            if (const auto framebuffer = state.framebuffers.find(info.framebuffer);
                    framebuffer != state.framebuffers.end()) {
                for (const auto i : pass->presentAttachments) {
                    if (std::ranges::binary_search(framebuffer->second.privateAttachments, i)) indices.push(i);
                }
            } else if (const auto* attachments = imagelessAttachments(info.pNext)) {
                for (const auto i : pass->presentAttachments) {
                    if (i < attachments->attachmentCount && state.views.contains(attachments->pAttachments[i]))
                        indices.push(i);
                }
            }
        }
        if (indices.indices().empty()) return VK_SUCCESS;
        return pass->variant(state.handle, state.createPass, indices.indices(), renderPass);
    }

    constexpr char beginPassName[] = "vkCmdBeginRenderPass";
    constexpr char beginPass2Name[] = "vkCmdBeginRenderPass2";
    constexpr char beginPass2KhrName[] = "vkCmdBeginRenderPass2KHR";

    template<const char* Name, typename Function, typename Contents>
    void VKAPI_CALL beginRenderPass(VkCommandBuffer buffer, const VkRenderPassBeginInfo* info, Contents contents) {
        auto [state, next] = dispatch<Function, Name>(buffer);
        auto adjusted = *info;
        try {
            const auto result = selectPass(state, *info, adjusted.renderPass);
            if (result != VK_SUCCESS) state.recordError(buffer, result);
        } catch (const std::bad_alloc&) {
            state.recordError(buffer, VK_ERROR_OUT_OF_HOST_MEMORY);
        }
        // Still record a structurally complete render pass if its variant could
        // not be created. EndCommandBuffer reports that recording failure.
        next(buffer, &adjusted, contents);
    }

    constexpr char beginBufferName[] = "vkBeginCommandBuffer";
    VkResult VKAPI_CALL beginCommandBuffer(VkCommandBuffer buffer, const VkCommandBufferBeginInfo* info) {
        auto [state, next] = dispatch<PFN_vkBeginCommandBuffer, beginBufferName>(buffer);
        state.consumeError(buffer);
        return next(buffer, info);
    }

    constexpr char endBufferName[] = "vkEndCommandBuffer";
    VkResult VKAPI_CALL endCommandBuffer(VkCommandBuffer buffer) {
        auto [state, next] = dispatch<PFN_vkEndCommandBuffer, endBufferName>(buffer);
        const auto status = next(buffer);
        const auto error = state.consumeError(buffer);
        return status == VK_SUCCESS ? error : status;
    }

    template<typename Barrier, typename Call>
    decltype(auto) withBarriers(Device& state, uint32_t count, const Barrier* barriers, Call call) {
        std::array<Barrier, 8> local;
        std::vector<Barrier> overflow;
        Barrier* translated = nullptr;
        {
            std::shared_lock lock(state.objects, std::defer_lock);
            for (const auto& b : std::span{barriers, count}) {
                if (!isPresent(b.oldLayout) && !isPresent(b.newLayout)) continue;
                if (!lock.owns_lock()) lock.lock();
                if (!state.images.contains(b.image)) continue;
                if (!translated) {
                    if (count <= local.size()) {
                        std::copy_n(barriers, count, local.data());
                        translated = local.data();
                    } else {
                        overflow.assign(barriers, barriers + count);
                        translated = overflow.data();
                    }
                }
                auto& adjusted = translated[&b - barriers];
                adjusted.oldLayout = substitute(b.oldLayout);
                adjusted.newLayout = substitute(b.newLayout);
            }
        }
        return call(translated ? translated : barriers);
    }

    template<typename Barrier, typename Call>
    void translate(Device& state, VkCommandBuffer buffer, uint32_t count, const Barrier* barriers, Call call) {
        try {
            if (count && state.imageCount.load(std::memory_order_acquire))
                return withBarriers(state, count, barriers, call);
        } catch (const std::bad_alloc&) {
            state.recordError(buffer, VK_ERROR_OUT_OF_HOST_MEMORY);
        }
        call(barriers);
    }

    constexpr char barrierName[] = "vkCmdPipelineBarrier";
    void VKAPI_CALL pipelineBarrier(VkCommandBuffer buffer, VkPipelineStageFlags src, VkPipelineStageFlags dst,
            VkDependencyFlags flags, uint32_t memoryCount, const VkMemoryBarrier* memory,
            uint32_t bufferCount, const VkBufferMemoryBarrier* buffers,
            uint32_t imageCount, const VkImageMemoryBarrier* images) {
        auto [state, next] = dispatch<PFN_vkCmdPipelineBarrier, barrierName>(buffer);
        translate(state, buffer, imageCount, images, [&](const auto* translated) {
            next(buffer, src, dst, flags, memoryCount, memory, bufferCount, buffers, imageCount, translated);
        });
    }

    constexpr char waitEventsName[] = "vkCmdWaitEvents";
    void VKAPI_CALL waitEvents(VkCommandBuffer buffer, uint32_t count, const VkEvent* events,
            VkPipelineStageFlags src, VkPipelineStageFlags dst, uint32_t memoryCount, const VkMemoryBarrier* memory,
            uint32_t bufferCount, const VkBufferMemoryBarrier* buffers,
            uint32_t imageCount, const VkImageMemoryBarrier* images) {
        auto [state, next] = dispatch<PFN_vkCmdWaitEvents, waitEventsName>(buffer);
        translate(state, buffer, imageCount, images, [&](const auto* translated) {
            next(buffer, count, events, src, dst, memoryCount, memory, bufferCount, buffers, imageCount, translated);
        });
    }

    constexpr char barrier2Name[] = "vkCmdPipelineBarrier2";
    constexpr char barrier2KhrName[] = "vkCmdPipelineBarrier2KHR";
    template<const char* Name>
    void VKAPI_CALL pipelineBarrier2(VkCommandBuffer buffer, const VkDependencyInfo* info) {
        auto [state, next] = dispatch<PFN_vkCmdPipelineBarrier2, Name>(buffer);
        translate(state, buffer, info->imageMemoryBarrierCount, info->pImageMemoryBarriers, [&](const auto* images) {
            auto adjusted = *info;
            adjusted.pImageMemoryBarriers = images;
            next(buffer, &adjusted);
        });
    }

    constexpr char setEvent2Name[] = "vkCmdSetEvent2";
    constexpr char setEvent2KhrName[] = "vkCmdSetEvent2KHR";
    template<const char* Name>
    void VKAPI_CALL setEvent2(VkCommandBuffer buffer, VkEvent event, const VkDependencyInfo* info) {
        auto [state, next] = dispatch<PFN_vkCmdSetEvent2, Name>(buffer);
        translate(state, buffer, info->imageMemoryBarrierCount, info->pImageMemoryBarriers, [&](const auto* images) {
            auto adjusted = *info;
            adjusted.pImageMemoryBarriers = images;
            next(buffer, event, &adjusted);
        });
    }

    constexpr char waitEvents2Name[] = "vkCmdWaitEvents2";
    constexpr char waitEvents2KhrName[] = "vkCmdWaitEvents2KHR";
    template<const char* Name>
    void VKAPI_CALL waitEvents2(VkCommandBuffer buffer, uint32_t count, const VkEvent* events,
            const VkDependencyInfo* infos) {
        auto [state, next] = dispatch<PFN_vkCmdWaitEvents2, Name>(buffer);
        if (!state.imageCount.load(std::memory_order_acquire)) return next(buffer, count, events, infos);
        try {
            std::vector<VkDependencyInfo> adjusted;
            std::vector<std::vector<VkImageMemoryBarrier2>> copied;
            for (uint32_t i = 0; i < count; ++i) {
                const auto& info = infos[i];
                withBarriers(state, info.imageMemoryBarrierCount, info.pImageMemoryBarriers, [&](const auto* images) {
                    if (images == info.pImageMemoryBarriers) return;
                    if (adjusted.empty()) {
                        adjusted.assign(infos, infos + count);
                        copied.resize(count);
                    }
                    copied[i].assign(images, images + info.imageMemoryBarrierCount);
                    adjusted[i].pImageMemoryBarriers = copied[i].data();
                });
            }
            if (!adjusted.empty()) return next(buffer, count, events, adjusted.data());
        } catch (const std::bad_alloc&) {
            state.recordError(buffer, VK_ERROR_OUT_OF_HOST_MEMORY);
        }
        next(buffer, count, events, infos);
    }
#ifdef VK_EXT_host_image_copy
    constexpr char transitionName[] = "vkTransitionImageLayout";
    constexpr char transitionExtName[] = "vkTransitionImageLayoutEXT";
    template<const char* Name>
    VkResult VKAPI_CALL transitionImageLayout(VkDevice device, uint32_t count,
            const VkHostImageLayoutTransitionInfoEXT* transitions) {
        auto [state, next] = dispatch<PFN_vkTransitionImageLayoutEXT, Name>(device);
        if (!state.imageCount.load(std::memory_order_acquire)) return next(device, count, transitions);
        try {
            return withBarriers(state, count, transitions,
                [&](const auto* translated) { return next(device, count, translated); });
        } catch (const std::bad_alloc&) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
#endif

}

    PFN_vkVoidFunction command(std::string_view name) {
#define FGVK_COMMAND(nameString, ...) \
        if (name == nameString) return reinterpret_cast<PFN_vkVoidFunction>(__VA_ARGS__)
        FGVK_COMMAND(createViewName, createView);
        FGVK_COMMAND(destroyViewName, destroyView);
        FGVK_COMMAND(createFramebufferName, createFramebuffer);
        FGVK_COMMAND(destroyFramebufferName, destroyFramebuffer);
        FGVK_COMMAND(createPassName, createRenderPass<createPassName, VkRenderPassCreateInfo, PFN_vkCreateRenderPass>);
        FGVK_COMMAND(createPass2Name, createRenderPass<createPass2Name, VkRenderPassCreateInfo2, PFN_vkCreateRenderPass2>);
        FGVK_COMMAND(createPass2KhrName, createRenderPass<createPass2KhrName, VkRenderPassCreateInfo2, PFN_vkCreateRenderPass2>);
        FGVK_COMMAND(destroyPassName, destroyRenderPass);
        FGVK_COMMAND(beginBufferName, beginCommandBuffer);
        FGVK_COMMAND(endBufferName, endCommandBuffer);
        FGVK_COMMAND(beginPassName, beginRenderPass<beginPassName, PFN_vkCmdBeginRenderPass, VkSubpassContents>);
        FGVK_COMMAND(beginPass2Name, beginRenderPass<beginPass2Name, PFN_vkCmdBeginRenderPass2, const VkSubpassBeginInfo*>);
        FGVK_COMMAND(beginPass2KhrName, beginRenderPass<beginPass2KhrName, PFN_vkCmdBeginRenderPass2, const VkSubpassBeginInfo*>);
        FGVK_COMMAND(barrierName, pipelineBarrier);
        FGVK_COMMAND(waitEventsName, waitEvents);
        FGVK_COMMAND(barrier2Name, pipelineBarrier2<barrier2Name>);
        FGVK_COMMAND(barrier2KhrName, pipelineBarrier2<barrier2KhrName>);
        FGVK_COMMAND(setEvent2Name, setEvent2<setEvent2Name>);
        FGVK_COMMAND(setEvent2KhrName, setEvent2<setEvent2KhrName>);
        FGVK_COMMAND(waitEvents2Name, waitEvents2<waitEvents2Name>);
        FGVK_COMMAND(waitEvents2KhrName, waitEvents2<waitEvents2KhrName>);
#ifdef VK_EXT_host_image_copy
        FGVK_COMMAND(transitionName, transitionImageLayout<transitionName>);
        FGVK_COMMAND(transitionExtName, transitionImageLayout<transitionExtName>);
#endif
#undef FGVK_COMMAND
        return nullptr;
    }

    bool registerDevice(VkDevice device, PFN_vkGetDeviceProcAddr next) noexcept {
        try {
            auto state = std::make_unique<Device>(device, next);
            const std::unique_lock lock(devicesMutex);
            return devices.emplace(dispatchKey(device), std::move(state)).second;
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    void unregisterDevice(VkDevice device) noexcept {
        const std::unique_lock lock(devicesMutex);
        devices.erase(dispatchKey(device));
        deviceEpoch.fetch_add(1, std::memory_order_release);
    }

    bool registerImages(VkDevice device, std::span<const VkImage> images) noexcept {
        auto& state = deviceFor(dispatchKey(device), deviceEpoch.load(std::memory_order_acquire));
        const std::unique_lock lock(state.objects);
        try {
            state.images.insert(images.begin(), images.end());
            state.imageCount.store(state.images.size(), std::memory_order_release);
            return true;
        } catch (const std::bad_alloc&) {
            for (const auto image : images) state.images.erase(image);
            return false;
        }
    }

    void unregisterImages(VkDevice device, std::span<const VkImage> images) noexcept {
        auto& state = deviceFor(dispatchKey(device), deviceEpoch.load(std::memory_order_acquire));
        const std::unique_lock lock(state.objects);
        for (const auto image : images) state.images.erase(image);
        state.imageCount.store(state.images.size(), std::memory_order_release);
    }
}

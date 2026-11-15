/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "pnext_chain.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

    /// Lifetime protection follows the resources/transport provisioned by
    /// framegen-vulkan, independently of which user-facing feature enabled them.
    [[nodiscard]] constexpr bool presentationRetirementRequired(
            const bool frameGenerationProvisioned) noexcept {
        return frameGenerationProvisioned;
    }

    // VK_KHR_swapchain_maintenance1 promoted the EXT extension without
    // changing its structure layouts or sType values. Freedesktop 23.08's
    // Vulkan headers predate the KHR spelling, so keep the ABI-facing types on
    // the EXT names while still negotiating the promoted runtime name.
    inline constexpr char khrSwapchainMaintenance1ExtensionName[] =
        "VK_KHR_swapchain_maintenance1";
    inline constexpr char khrSurfaceMaintenance1ExtensionName[] =
        "VK_KHR_surface_maintenance1";
    inline constexpr char extSurfaceMaintenance1ExtensionName[] =
        "VK_EXT_surface_maintenance1";

    struct SurfaceMaintenance1Support {
        bool khr{false};
        bool ext{false};
    };

    struct InstanceExtensionQuery {
        VkResult result{VK_ERROR_INITIALIZATION_FAILED};
        std::vector<VkExtensionProperties> properties;
    };

    [[nodiscard]] inline InstanceExtensionQuery queryPresentationInstanceExtensions(
            PFN_vkEnumerateInstanceExtensionProperties enumerate,
            const PFN_vkEnumerateInstanceExtensionProperties loaderEnumerate) {
        InstanceExtensionQuery query;
        // Some layers (including some compositor WSI layers) do not expose global
        // enumeration at all. The loader owns driver enumeration before
        // an instance exists, so use it when the layer has no entrypoint.
        if (!enumerate)
            enumerate = loaderEnumerate;
        if (!enumerate)
            return query;
        uint32_t count{};
        query.result = enumerate(nullptr, &count, nullptr);
        // vkBasalt and other layers can expose only layer-specific global
        // enumeration. Ask the loader for driver capabilities in that case;
        // never infer optional support from a rejected layer-name query.
        if (query.result == VK_ERROR_LAYER_NOT_PRESENT && loaderEnumerate) {
            enumerate = loaderEnumerate;
            count = 0;
            query.result = enumerate(nullptr, &count, nullptr);
        }
        if (query.result == VK_SUCCESS) {
            query.properties.resize(count);
            query.result = enumerate(nullptr, &count, query.properties.data());
            if (query.result == VK_SUCCESS)
                query.properties.resize(count);
            else
                query.properties.clear();
        }
        return query;
    }

    [[nodiscard]] inline SurfaceMaintenance1Support surfaceMaintenance1Support(
            const std::vector<VkExtensionProperties>& extensions) {
        const auto has = [&](const char* name) {
            return std::ranges::any_of(extensions, [&](const auto& extension) {
                return std::strcmp(extension.extensionName, name) == 0;
            });
        };
        if (!has("VK_KHR_surface") || !has("VK_KHR_get_surface_capabilities2"))
            return {};
        return {has(khrSurfaceMaintenance1ExtensionName),
            has(extSurfaceMaintenance1ExtensionName)};
    }

#ifdef VK_KHR_swapchain_maintenance1
    static_assert(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT ==
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR
    );
    static_assert(
        VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT ==
        VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR
    );
    static_assert(
        sizeof(VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT) ==
        sizeof(VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR)
    );
    static_assert(
        sizeof(VkSwapchainPresentFenceInfoEXT) ==
        sizeof(VkSwapchainPresentFenceInfoKHR)
    );
#endif

    // The maintenance fence is the Vulkan lifetime proof. The grace interval
    // additionally lets an upper compositor WSI/protocol event loop observe a
    // replacement presentation before the retired lower WSI is destroyed.
    inline constexpr auto swapchainRetirementGracePeriod =
        std::chrono::milliseconds(50);

    /// Presentation polling must not wait behind a terminal retirement.
    /// Nonzero timeouts belong to destruction boundaries and retain the
    /// existing blocking mutex ownership before waiting for completion.
    [[nodiscard]] inline std::unique_lock<std::mutex>
    lockSwapchainRetirement(std::mutex& mutex, const uint64_t timeoutNs) {
        std::unique_lock lock(mutex, std::defer_lock);
        if (timeoutNs == 0)
            static_cast<void>(lock.try_lock());
        else
            lock.lock();
        return lock;
    }

    /// Surface destruction is a terminal boundary only for lower swapchains
    /// created from that exact application-visible surface. A missing mapping
    /// must remain deferred for device teardown rather than match null handles.
    [[nodiscard]] constexpr bool retiredSwapchainBelongsToSurface(
            const VkSurfaceKHR retiredSurface,
            const VkSurfaceKHR destroyedSurface) noexcept {
        return retiredSurface != VK_NULL_HANDLE &&
            retiredSurface == destroyedSurface;
    }

    /// An upper WSI may destroy its application-visible swapchain before
    /// creating the replacement and consequently pass a null oldSwapchain to
    /// the lower layer. If framegen-vulkan retained that exact lower swapchain for
    /// maintenance-fence completion, finish its proven retirement before the
    /// replacement create. Passing the retained handle back through a compositor
    /// WSI layer as oldSwapchain can deadlock its WSI replacement path.
    [[nodiscard]] constexpr bool
    shouldRetireRetainedSwapchainBeforeNullOldReplacement(
            const VkSwapchainKHR requestedOldSwapchain,
            const VkDevice createDevice,
            const VkSurfaceKHR createSurface,
            const VkDevice retiredDevice,
            const VkSurfaceKHR retiredSurface) noexcept {
        return requestedOldSwapchain == VK_NULL_HANDLE &&
            createDevice != VK_NULL_HANDLE &&
            createDevice == retiredDevice &&
            createSurface != VK_NULL_HANDLE &&
            createSurface == retiredSurface;
    }

    /// Replacement recovery is independent of the handle passed to the lower
    /// WSI. A completed destroy-before-create retirement remains a known
    /// replacement even though the compositor WSI layer must receive a null oldSwapchain.
    [[nodiscard]] constexpr bool swapchainCreateIsReplacement(
            const VkSwapchainKHR requestedOldSwapchain,
            const bool retiredNullOldReplacement) noexcept {
        return requestedOldSwapchain != VK_NULL_HANDLE ||
            retiredNullOldReplacement;
    }

    /// Prefer the promoted extension name when the driver advertises it, then
    /// fall back to the EXT predecessor used by current RADV stacks.
    /// The feature bit is mandatory for either spelling.
    [[nodiscard]] constexpr const char*
    selectSwapchainMaintenance1Extension(
            const bool khrExtensionSupported,
            const bool extExtensionSupported,
            const bool featureSupported) noexcept {
        if (!featureSupported)
            return nullptr;
        if (khrExtensionSupported)
            return khrSwapchainMaintenance1ExtensionName;
        if (extExtensionSupported)
            return VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
        return nullptr;
    }

    struct VulkanPNextHeader {
        VkStructureType sType{};
        const void* pNext{nullptr};
    };

    static_assert(
        sizeof(VulkanPNextHeader) == sizeof(VkBaseInStructure) &&
        offsetof(VulkanPNextHeader, sType) ==
            offsetof(VkBaseInStructure, sType) &&
        offsetof(VulkanPNextHeader, pNext) ==
            offsetof(VkBaseInStructure, pNext)
    );

    // Vulkan guarantees that every extensible structure starts with sType and
    // pNext, but C++ strict-aliasing rules do not make an arbitrary Vulkan
    // structure a VkBaseInStructure object. Copy the common ABI header into a
    // neutral representation so optimized builds can traverse caller-owned
    // chains without teaching the compiler that every node has one C++ type.
    [[nodiscard]] inline VulkanPNextHeader pNextHeader(
            const void* node) noexcept {
        VulkanPNextHeader header{};
        if (node) {
            const auto* bytes = static_cast<const std::byte*>(node);
            std::memcpy(
                &header.sType,
                bytes + offsetof(VulkanPNextHeader, sType),
                sizeof(header.sType)
            );
            std::memcpy(
                &header.pNext,
                bytes + offsetof(VulkanPNextHeader, pNext),
                sizeof(header.pNext)
            );
        }
        return header;
    }

    /// framegen-vulkan owns presentation only on application devices that enabled the
    /// swapchain extension. Frame generation creates a private compute-only
    /// device in the same process; in a split chain that device still passes
    /// through the lower WSI, but the layer may not attach swapchain hooks or
    /// frame-generation resources to it.
    [[nodiscard]] inline bool swapchainPresentationEnabled(
            const VkDeviceCreateInfo& createInfo) noexcept {
        if (createInfo.enabledExtensionCount > 0 &&
                !createInfo.ppEnabledExtensionNames) {
            return false;
        }
        for (uint32_t index = 0;
                index < createInfo.enabledExtensionCount; ++index) {
            const char* const extension =
                createInfo.ppEnabledExtensionNames[index];
            if (extension && std::strcmp(
                    extension, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                return true;
            }
        }
        return false;
    }

    /// An upper WSI layer enables swapchain-maintenance1 before calling
    /// framegen-vulkan. Preserve that negotiated device contract explicitly rather than
    /// inferring support later from the physical device or environment.
    [[nodiscard]] inline bool swapchainMaintenance1Enabled(
            const VkDeviceCreateInfo& createInfo) noexcept {
        if (createInfo.enabledExtensionCount > 0 &&
                !createInfo.ppEnabledExtensionNames) {
            return false;
        }
        bool extensionEnabled = false;
        for (uint32_t index = 0;
                index < createInfo.enabledExtensionCount; ++index) {
            const char* const extension =
                createInfo.ppEnabledExtensionNames[index];
            if (extension &&
                    (std::strcmp(
                        extension,
                        VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME
                    ) == 0 ||
                     std::strcmp(
                        extension,
                        khrSwapchainMaintenance1ExtensionName
                    ) == 0)) {
                extensionEnabled = true;
                break;
            }
        }
        if (!extensionEnabled)
            return false;

        for (const void* node = createInfo.pNext; node;) {
            const auto header = pNextHeader(node);
            if (header.sType ==
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT) {
                const auto* features = reinterpret_cast<
                    const VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT*>(
                        node
                    );
                return features->swapchainMaintenance1 == VK_TRUE;
            }
            node = header.pNext;
        }
        return false;
    }

    [[nodiscard]] inline const VkSwapchainPresentFenceInfoEXT*
    findSwapchainPresentFenceInfo(const void* chain) noexcept {
        for (const void* node = chain; node;) {
            const auto header = pNextHeader(node);
            if (header.sType ==
                    VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT) {
                return reinterpret_cast<
                    const VkSwapchainPresentFenceInfoEXT*>(node);
            }
            node = header.pNext;
        }
        return nullptr;
    }

    /// The application's present fence for swapchain `index` of a present (pFences[index]),
    /// or VK_NULL_HANDLE when the chain has no VkSwapchainPresentFenceInfoEXT for it.
    [[nodiscard]] inline VkFence applicationPresentFence(const VkPresentInfoKHR& info,
            const uint32_t index) noexcept {
        const auto* fences = findSwapchainPresentFenceInfo(info.pNext);
        if (!fences || !fences->pFences || index >= fences->swapchainCount)
            return VK_NULL_HANDLE;
        return fences->pFences[index];
    }

    /// X3: an application present fence that no lower present received is signalled by the
    /// layer exactly once, with an empty submit on `queue`. The caller submits it after the
    /// work that consumed the present's waits on that queue. VK_NULL_HANDLE is a no-op.
    [[nodiscard]] inline VkResult signalUnforwardedPresentFence(const PFN_vkQueueSubmit submit,
            const VkQueue queue, const VkFence fence) {
        if (fence == VK_NULL_HANDLE)
            return VK_SUCCESS;
        return submit(queue, 0, nullptr, fence);
    }

    /// WSI surface loss and out-of-date still enqueue the present's waits and
    /// maintenance1 fences. A bridge rejection occurs before lower WSI, so
    /// complete those queue operations exactly once without presenting to a
    /// retired association. Storage is allocated only on this failure path.
    [[nodiscard]] inline VkResult completeRejectedPresent(
            PFN_vkQueueSubmit submit, VkQueue queue,
            const VkPresentInfoKHR& info, VkResult rejection) {
        std::vector<VkPipelineStageFlags> stages;
        try {
            stages.assign(info.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        } catch (const std::bad_alloc&) {
            if (info.pResults)
                std::fill_n(info.pResults, info.swapchainCount, VK_ERROR_OUT_OF_HOST_MEMORY);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        VkSubmitInfo submission{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = info.waitSemaphoreCount,
            .pWaitSemaphores = info.pWaitSemaphores,
            .pWaitDstStageMask = stages.data(),
        };
        VkResult result = VK_SUCCESS;
        const auto* fences = findSwapchainPresentFenceInfo(info.pNext);
        bool submitted = false;
        if (fences && fences->pFences) {
            for (uint32_t i = 0; i < fences->swapchainCount; ++i) {
                if (fences->pFences[i] == VK_NULL_HANDLE)
                    continue;
                result = submit(queue, 1, &submission, fences->pFences[i]);
                if (result != VK_SUCCESS) {
                    // Earlier submissions already changed synchronization.
                    // An allocation failure may promise no side effects only
                    // when the first submission itself failed.
                    if (submitted && (result == VK_ERROR_OUT_OF_HOST_MEMORY ||
                            result == VK_ERROR_OUT_OF_DEVICE_MEMORY))
                        result = VK_ERROR_DEVICE_LOST;
                    submitted = true;
                    break;
                }
                submitted = true;
                submission.waitSemaphoreCount = 0;
                submission.pWaitSemaphores = nullptr;
                submission.pWaitDstStageMask = nullptr;
            }
        }
        if (!submitted && info.waitSemaphoreCount)
            result = submit(queue, 1, &submission, VK_NULL_HANDLE);
        if (result == VK_SUCCESS)
            result = rejection;
        if (info.pResults)
            std::fill_n(info.pResults, info.swapchainCount, result);
        return result;
    }

    /// vkQueuePresentKHR writes every swapchain's result, VK_SUCCESS included.
    /// Callers may pass uninitialised storage: VKD3D-Proton passes a stack
    /// VkResult and treats a non-success value there as the present's
    /// result. Skipping VK_SUCCESS leaves an earlier OUT_OF_DATE in place, so
    /// the game recreates its swapchain on every present.
    inline void recordPresentResult(const VkPresentInfoKHR& info,
            const size_t index, const VkResult result) {
        if (info.pResults && index < info.swapchainCount)
            info.pResults[index] = result;
    }

    /// A non-null upstream fence for this swapchain is the caller's Vulkan
    /// lifetime proof for the final lower present. framegen-vulkan never borrows, resets,
    /// or replaces that fence; it only preserves the chain and records that
    /// the one-shot recreation result followed a retirement-protected present.
    [[nodiscard]] inline bool upstreamPresentFenceProtectsSwapchain(
            const VkSwapchainPresentFenceInfoEXT* const info,
            const uint32_t swapchainIndex = 0) noexcept {
        return info && info->pFences &&
            swapchainIndex < info->swapchainCount &&
            info->pFences[swapchainIndex] != VK_NULL_HANDLE;
    }

    /// These results retain the queued present operation, including its
    /// maintenance1 fence signal. Allocation or device-loss failures do not
    /// provide that guarantee and must not leave an unsignalable fence tracked.
    [[nodiscard]] constexpr bool presentFenceWillSignal(
            const VkResult result) noexcept {
        return result == VK_SUCCESS ||
            result == VK_SUBOPTIMAL_KHR ||
            result == VK_ERROR_OUT_OF_DATE_KHR ||
            result == VK_ERROR_SURFACE_LOST_KHR ||
            result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT;
    }

}

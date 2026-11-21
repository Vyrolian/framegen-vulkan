/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "instance.hpp"
#include "color_pipeline.hpp"
#include "layer_role.hpp"
#include "fgvk-common/helpers/errors.hpp"
#include "fgvk-common/helpers/pointers.hpp"
#include "fgvk-common/vulkan/vulkan.hpp"
#include "fgvk-common/vulkan/device_features.hpp"
#include "present_diagnostics.hpp"
#include "wayland_presentation_feedback.hpp"
#include "surface_formats.hpp"
#include "swapchain/swapchain.hpp"
#include "swapchain/create_policy.hpp"
#include "swapchain/retirement.hpp"
#include "swapchain/private_present_queue.hpp"
#include "swapchain/release_gate.hpp"
#include "swapchain/low_latency.hpp"
#include "swapchain/internal_images.hpp"
#include "swapchain/present_layout.hpp"
#include "swapchain/present_marker.hpp"
#include "swapchain/reflex_out_of_band.hpp"
#include "swapchain/submit_tag_tracker.hpp"
#include "pnext_chain.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <span>
#include <link.h>
#include <optional>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <new>
#include <string_view>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_wayland.h>

// X11 create-info types remain opaque. Native Wayland's borrowed handles are
// used only by the opt-in presentation observer; client APIs resolve at runtime.
struct VkXcbSurfaceCreateInfoKHR;
struct VkXlibSurfaceCreateInfoKHR;

using namespace fgvk::layer;

namespace {
    // global layer info initialized at layer negotiation
    struct LayerInfo {
        std::unordered_map<std::string, PFN_vkVoidFunction> map; //!< function pointer override map
        PFN_vkGetInstanceProcAddr GetInstanceProcAddr;

        Root root;
    }* layer_info; // NOLINT (global variable)

    // instance-wide info initialized at instance creation(s)
    struct InstanceInfo {
        std::vector<VkInstance> handles; // there may be several instances
        vk::VulkanInstanceFuncs funcs;
        std::string engineName;
        SurfaceMaintenance1Support surfaceMaintenance1;
        bool surfaceCapabilities2{false};

        std::unordered_map<VkDevice, vk::Vulkan> devices;
        std::unordered_map<VkDevice, std::shared_ptr<DeferredPresentQueue>> deferredPresentQueues;
        std::unordered_set<VkDevice> nativeDevices;
        std::unordered_set<VkDevice> presentRetirementDevices;
        std::unordered_set<VkDevice> presentTimingDevices;
        std::unordered_set<VkDevice> presentMeteringDevices;
        std::unordered_set<VkDevice> copyCalibrationDevices;
        std::unordered_set<VkDevice> applicationTimingDevices;
        std::unordered_set<VkDevice> presentFeedbackDevices;
        struct QueueIdentity {
            VkDevice device{VK_NULL_HANDLE};
            uint32_t familyIndex{UINT32_MAX};
        };
        std::unordered_map<VkQueue, QueueIdentity> queueIdentities;
        std::mutex surfaceStateMutex; // guards surfaceOrigins, surfaceX11Windows and waylandPresentation
        std::unordered_map<VkSurfaceKHR, SurfaceOrigin> surfaceOrigins;
        std::unordered_map<VkSurfaceKHR, uint64_t> surfaceX11Windows; // X5: window XID of Xlib/XCB surfaces
        std::unordered_map<VkSurfaceKHR, std::shared_ptr<WaylandPresentationFeedback>> waylandPresentation;
        std::unordered_map<VkSwapchainKHR, ls::R<vk::Vulkan>> swapchains;
        std::unordered_map<VkSwapchainKHR, SwapchainInfo> swapchainInfos;
        std::unordered_set<VkSwapchainKHR> nativeSwapchains;
        // F5 layer-owned application images, keyed like swapchainInfos.
        std::unordered_map<VkSwapchainKHR, std::shared_ptr<InternalSwapchainImages>> internalSwapchains;
        struct RetiredSwapchain {
            VkDevice device{VK_NULL_HANDLE};
            VkSurfaceKHR surface{VK_NULL_HANDLE};
            ls::R<vk::Vulkan> vk;
            std::optional<Swapchain> context;
            std::chrono::steady_clock::time_point notBefore{};
            // Lower presents of a retired context may still wait the copied semaphores.
            std::shared_ptr<InternalSwapchainImages> internalImages;
        };
        std::atomic_size_t retiredSwapchainCount{0};
        std::mutex retiredSwapchainsMutex;
        std::unordered_map<VkSwapchainKHR, RetiredSwapchain>
            retiredSwapchains;
    }* instance_info; // NOLINT (global variable)

    bool initializeLayerInfo();

    bool finalizeRetiredSwapchain(
            const VkSwapchainKHR swapchain, const uint64_t timeoutNs) {
        const auto retirementLock = lockSwapchainRetirement(
            instance_info->retiredSwapchainsMutex, timeoutNs
        );
        if (!retirementLock.owns_lock())
            return false;
        const auto retired = instance_info->retiredSwapchains.find(swapchain);
        if (retired == instance_info->retiredSwapchains.end())
            return true;
        uint64_t remainingTimeoutNs = timeoutNs;
        const auto now = std::chrono::steady_clock::now();
        if (now < retired->second.notBefore) {
            if (timeoutNs == 0)
                return false;
            const auto delay = std::chrono::duration_cast<
                std::chrono::nanoseconds>(
                    retired->second.notBefore - now
                );
            if (timeoutNs != UINT64_MAX &&
                    static_cast<uint64_t>(delay.count()) > timeoutNs) {
                return false;
            }
            std::this_thread::sleep_for(delay);
            if (timeoutNs != UINT64_MAX)
                remainingTimeoutNs -= static_cast<uint64_t>(delay.count());
        }
        if (retired->second.context &&
                !retired->second.context->waitForPresentRetirement(
                    retired->second.vk.get(), remainingTimeoutNs
                )) {
            return false;
        }

        auto node = instance_info->retiredSwapchains.extract(retired);
        auto& state = node.mapped();
        // GP1: the on-screen watcher is joined before the lower swapchain is destroyed.
        if (state.context) state.context->stopOnScreenProfile();
        state.context.reset();
        state.vk.get().df().DestroySwapchainKHR(
            state.device, swapchain, VK_NULL_HANDLE
        );
        instance_info->retiredSwapchainCount.fetch_sub(
            1, std::memory_order_acq_rel
        );
        return true;
    }

    void collectRetiredSwapchains(
            const VkDevice device, const uint64_t timeoutNs) {
        if (instance_info->retiredSwapchainCount.load(
                std::memory_order_acquire) == 0) {
            return;
        }
        const auto started = std::chrono::steady_clock::now();
        std::vector<VkSwapchainKHR> candidates;
        {
            const std::lock_guard retirementLock(
                instance_info->retiredSwapchainsMutex
            );
            candidates.reserve(instance_info->retiredSwapchains.size());
            for (const auto& [swapchain, retired] :
                    instance_info->retiredSwapchains) {
                if (retired.device == device)
                    candidates.push_back(swapchain);
            }
        }
        for (const auto swapchain : candidates) {
            uint64_t remainingTimeoutNs = timeoutNs;
            if (timeoutNs != UINT64_MAX) {
                const auto elapsed = std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - started
                    ).count();
                remainingTimeoutNs = elapsed >=
                        static_cast<int64_t>(timeoutNs)
                    ? 0
                    : timeoutNs - static_cast<uint64_t>(elapsed);
            }
            static_cast<void>(finalizeRetiredSwapchain(
                swapchain, remainingTimeoutNs
            ));
        }
    }

    void collectRetiredSwapchainsAfterPresent(
            const VkQueue queue, const VkPresentInfoKHR& presentInfo) {
        if (instance_info->retiredSwapchainCount.load(
                std::memory_order_acquire) == 0) {
            return;
        }
        const auto queueIdentity = instance_info->queueIdentities.find(queue);
        if (queueIdentity == instance_info->queueIdentities.end())
            return;
        const auto device = queueIdentity->second.device;
        std::vector<VkSwapchainKHR> candidates;
        {
            const auto retirementLock = lockSwapchainRetirement(
                instance_info->retiredSwapchainsMutex, 0
            );
            if (!retirementLock.owns_lock())
                return;
            candidates.reserve(instance_info->retiredSwapchains.size());
            for (const auto& [retiredHandle, retired] :
                    instance_info->retiredSwapchains) {
                if (retired.device != device)
                    continue;
                for (uint32_t index = 0;
                        index < presentInfo.swapchainCount; ++index) {
                    const auto live = instance_info->swapchainInfos.find(
                        presentInfo.pSwapchains[index]
                    );
                    if (live != instance_info->swapchainInfos.end() &&
                            retiredSwapchainBelongsToSurface(
                                retired.surface, live->second.surface)) {
                        candidates.push_back(retiredHandle);
                        break;
                    }
                }
            }
        }
        for (const auto swapchain : candidates) {
            static_cast<void>(finalizeRetiredSwapchain(
                swapchain, 0
            ));
        }
    }

    std::optional<VkSwapchainKHR>
    retainedSwapchainBeforeNullOldReplacement(
            const VkDevice device, const VkSurfaceKHR surface,
            const VkSwapchainKHR requestedOldSwapchain) {
        const std::lock_guard retirementLock(
            instance_info->retiredSwapchainsMutex
        );
        for (const auto& [swapchain, retired] :
                instance_info->retiredSwapchains) {
            if (!shouldRetireRetainedSwapchainBeforeNullOldReplacement(
                    requestedOldSwapchain,
                    device,
                    surface,
                    retired.device,
                    retired.surface)) {
                continue;
            }
            return swapchain;
        }
        return std::nullopt;
    }

    void forceFinalizeRetiredSwapchains(const VkDevice device) {
        std::vector<VkSwapchainKHR> candidates;
        {
            const std::lock_guard retirementLock(
                instance_info->retiredSwapchainsMutex
            );
            for (const auto& [swapchain, retired] :
                    instance_info->retiredSwapchains) {
                if (retired.device == device)
                    candidates.push_back(swapchain);
            }
        }
        for (const auto swapchain : candidates) {
            const std::lock_guard retirementLock(
                instance_info->retiredSwapchainsMutex
            );
            auto node = instance_info->retiredSwapchains.extract(swapchain);
            if (node.empty())
                continue;
            auto& state = node.mapped();
            std::cerr << "framegen-vulkan: forcing pending swapchain retirement "
                         "during device destruction\n";
            // GP1: the on-screen watcher is joined before the lower swapchain is destroyed.
            if (state.context) state.context->stopOnScreenProfile();
            state.context.reset();
            state.vk.get().df().DestroySwapchainKHR(
                state.device, swapchain, VK_NULL_HANDLE
            );
            instance_info->retiredSwapchainCount.fetch_sub(
                1, std::memory_order_acq_rel
            );
        }
    }

    void collectRetiredSwapchainsForSurface(const VkSurfaceKHR surface) {
        if (instance_info->retiredSwapchainCount.load(
                std::memory_order_acquire) == 0) {
            return;
        }
        std::vector<VkSwapchainKHR> candidates;
        {
            const std::lock_guard retirementLock(
                instance_info->retiredSwapchainsMutex
            );
            candidates.reserve(instance_info->retiredSwapchains.size());
            for (const auto& [swapchain, retired] :
                    instance_info->retiredSwapchains) {
                if (retiredSwapchainBelongsToSurface(
                        retired.surface, surface)) {
                    candidates.push_back(swapchain);
                }
            }
        }
        for (const auto swapchain : candidates)
            static_cast<void>(finalizeRetiredSwapchain(
                swapchain, UINT64_MAX
            ));
    }

    std::optional<uint32_t> selectLayerQueueFamily(
            const VkPhysicalDevice physicalDevice,
            const VkDeviceCreateInfo& createInfo) {
        uint32_t familyCount{};
        instance_info->funcs.GetPhysicalDeviceQueueFamilyProperties(
            physicalDevice, &familyCount, nullptr
        );
        std::vector<VkQueueFamilyProperties> families(familyCount);
        instance_info->funcs.GetPhysicalDeviceQueueFamilyProperties(
            physicalDevice, &familyCount, families.data()
        );
        const VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT;
        for (uint32_t i = 0; i < createInfo.queueCreateInfoCount; ++i) {
            const auto& requested = createInfo.pQueueCreateInfos[i];
            if (requested.queueCount == 0 || requested.flags != 0 ||
                    requested.queueFamilyIndex >= familyCount) {
                continue;
            }
            if ((families.at(requested.queueFamilyIndex).queueFlags &
                    required) == required) {
                return requested.queueFamilyIndex;
            }
        }
        return std::nullopt;
    }

    std::optional<const char*> supportedSwapchainMaintenance1Extension(
            const VkPhysicalDevice physicalDevice,
            const bool presentationResourcesProvisioned) {
        if (!presentationResourcesProvisioned ||
                !instance_info->funcs.GetPhysicalDeviceFeatures2) {
            return std::nullopt;
        }

        uint32_t extensionCount{};
        auto result = instance_info->funcs.EnumerateDeviceExtensionProperties(
            physicalDevice, nullptr, &extensionCount, nullptr
        );
        if (result != VK_SUCCESS)
            return std::nullopt;
        std::vector<VkExtensionProperties> extensions(extensionCount);
        result = instance_info->funcs.EnumerateDeviceExtensionProperties(
            physicalDevice, nullptr, &extensionCount, extensions.data()
        );
        if (result != VK_SUCCESS)
            return std::nullopt;

        const auto hasExtension = [&](const char* const name) {
            return std::ranges::any_of(
                extensions,
                [name](const VkExtensionProperties& extension) {
                    return std::strcmp(extension.extensionName, name) == 0;
                }
            );
        };
        const bool hasKhr = hasExtension(
            khrSwapchainMaintenance1ExtensionName
        ) && instance_info->surfaceMaintenance1.khr;
        const bool hasExt = hasExtension(
            VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME
        ) && instance_info->surfaceMaintenance1.ext;
        if (!hasKhr && !hasExt)
            return std::nullopt;

        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance1{
            .sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT,
        };
        VkPhysicalDeviceFeatures2 features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &maintenance1,
        };
        instance_info->funcs.GetPhysicalDeviceFeatures2(
            physicalDevice, &features
        );
        const char* const selected = selectSwapchainMaintenance1Extension(
            hasKhr, hasExt, maintenance1.swapchainMaintenance1 == VK_TRUE
        );
        if (!selected)
            return std::nullopt;
        return selected;
    }

    // create instance
    VkResult myvkCreateInstance(
            const VkInstanceCreateInfo* info,
            const VkAllocationCallbacks* alloc,
            VkInstance* instance) {
        // apply layer chaining
        auto* layerInfo = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(info->pNext));
        while (layerInfo && (layerInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO
                || layerInfo->function != VK_LAYER_LINK_INFO)) {
            layerInfo = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(layerInfo->pNext));
        }
        if (!layerInfo) {
            std::cerr << "framegen-vulkan: no layer info found in pNext chain, "
                "the previous layer does not follow spec\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto* linkInfo = layerInfo->u.pLayerInfo;
        if (!linkInfo) {
            std::cerr << "framegen-vulkan: link info is null, "
                "the previous layer does not follow spec\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        layer_info->GetInstanceProcAddr = linkInfo->pfnNextGetInstanceProcAddr;
        if (!layer_info->GetInstanceProcAddr) {
            std::cerr << "framegen-vulkan: next layer's vkGetInstanceProcAddr is null, "
                "the previous layer does not follow spec\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        layerInfo->u.pLayerInfo = linkInfo->pNext; // advance for next layer

        // create instance
        auto* vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
            layer_info->GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
        if (!vkCreateInstance) {
            std::cerr << "framegen-vulkan: failed to get next layer's vkCreateInstance, "
                "the previous layer does not follow spec\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        bool lowerInstanceCreated = false;
        const auto rollbackCreatedInstance = [&]() noexcept {
            if (!lowerInstanceCreated || !instance || *instance == VK_NULL_HANDLE)
                return;

            auto* vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
                layer_info->GetInstanceProcAddr(*instance, "vkDestroyInstance"));
            if (vkDestroyInstance) {
                vkDestroyInstance(*instance, alloc);
                *instance = VK_NULL_HANDLE;
            } else {
                std::cerr << "framegen-vulkan: failed to roll back a lower Vulkan instance "
                             "after layer initialization failed\n";
            }
            lowerInstanceCreated = false;
        };
        const auto releaseIdleLayerState = []() noexcept {
            if (instance_info)
                return;
            delete layer_info; // NOLINT (memory management)
            layer_info = nullptr;
        };

        try {
            const bool retirementRequested = presentationRetirementRequired(
                layer_info->root.frameGenerationInteropProvisioned());
            InstanceExtensionQuery extensionQuery;
            if (retirementRequested) {
                // Global enumeration is a loader entrypoint: a lower layer's
                // vkGetInstanceProcAddr(NULL, ...) may return a pointer that is
                // unsafe to call (Mesa device_select dereferences missing
                // instance state and crashes), so always ask the loader.
                extensionQuery = queryPresentationInstanceExtensions(
                    nullptr, vk::enumerateLoaderInstanceExtensionProperties);
            }
            const auto& available = extensionQuery.properties;
            VkInstanceCreateInfo newInfo = *info;
            std::vector<const char*> extensions;
            if (info->enabledExtensionCount)
                extensions.assign(info->ppEnabledExtensionNames,
                    info->ppEnabledExtensionNames + info->enabledExtensionCount);
            const auto appendExtension = [&](const char* name) {
                if (!std::ranges::any_of(extensions, [name](const char* existing) {
                        return std::strcmp(existing, name) == 0;
                    }))
                    extensions.push_back(name);
            };
            const auto maintenance1 = surfaceMaintenance1Support(available);
            const bool timingCapabilities = retirementRequested &&
                std::ranges::any_of(available, [](const auto& extension) {
                    return std::string_view(extension.extensionName) == "VK_KHR_get_surface_capabilities2";
                });
            if (timingCapabilities) {
                appendExtension("VK_KHR_surface");
                appendExtension("VK_KHR_get_surface_capabilities2");
            }
            if (retirementRequested) {
                if (maintenance1.khr || maintenance1.ext) {
                    appendExtension("VK_KHR_surface");
                    appendExtension("VK_KHR_get_surface_capabilities2");
                    if (maintenance1.khr)
                        appendExtension(khrSurfaceMaintenance1ExtensionName);
                    if (maintenance1.ext)
                        appendExtension(extSurfaceMaintenance1ExtensionName);
                }
            }
            newInfo.ppEnabledExtensionNames = extensions.data();
            newInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
            layer_info->root.modifyInstanceCreateInfo(newInfo,
                [&, newInfo = &newInfo]() {
                    auto res = vkCreateInstance(newInfo, alloc, instance);
                    present_diagnostics::traceVulkanSetup(layerRoleName, *info, *newInfo,
                        res, res == VK_SUCCESS ? *instance : VK_NULL_HANDLE);
                    if (res != VK_SUCCESS)
                        throw ls::vulkan_error(res, "vkCreateInstance() failed");
                    lowerInstanceCreated = true;
                }
            );

            if (!instance_info)
                instance_info = new InstanceInfo{ // NOLINT (memory management)
                    .funcs = vk::initVulkanInstanceFuncs(*instance,
                        layer_info->GetInstanceProcAddr, true),
                    .engineName = info->pApplicationInfo &&
                            info->pApplicationInfo->pEngineName
                        ? info->pApplicationInfo->pEngineName : "",
                    .surfaceMaintenance1 = maintenance1,
                    .surfaceCapabilities2 = timingCapabilities,
                };
            else {
                // Shared dispatch may serve several Wine instances. Only use
                // a spelling whose prerequisites were enabled on every one.
                instance_info->surfaceMaintenance1.khr &= maintenance1.khr;
                instance_info->surfaceMaintenance1.ext &= maintenance1.ext;
                instance_info->surfaceCapabilities2 &= timingCapabilities;
                if (instance_info->engineName.empty() &&
                        info->pApplicationInfo &&
                        info->pApplicationInfo->pEngineName) {
                    instance_info->engineName =
                        info->pApplicationInfo->pEngineName;
                }
            }

            instance_info->handles.push_back(*instance);
            lowerInstanceCreated = false;

            return VK_SUCCESS;
        } catch (const ls::vulkan_error& e) {
            rollbackCreatedInstance();
            releaseIdleLayerState();
            if (e.error() == VK_ERROR_EXTENSION_NOT_PRESENT)
                std::cerr << "framegen-vulkan: required Vulkan instance extensions are not present. "
                    "Your GPU driver is not supported.\n";
            return e.error();
        } catch (const std::exception& e) {
            rollbackCreatedInstance();
            releaseIdleLayerState();
            std::cerr << "framegen-vulkan: instance initialization failed:\n";
            std::cerr << "- " << e.what() << '\n';
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    // create device
    std::vector<VkExtensionProperties> lowerDeviceExtensions(VkPhysicalDevice physicalDevice) {
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            uint32_t count{};
            auto result = instance_info->funcs.EnumerateDeviceExtensionProperties(
                physicalDevice, nullptr, &count, nullptr);
            if (result != VK_SUCCESS || count > 4096)
                throw ls::vulkan_error(result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result,
                    "device extension enumeration failed");
            std::vector<VkExtensionProperties> extensions(count);
            result = instance_info->funcs.EnumerateDeviceExtensionProperties(
                physicalDevice, nullptr, &count, extensions.data());
            if (result == VK_SUCCESS && count <= extensions.size()) {
                extensions.resize(count);
                return extensions;
            }
            if (result != VK_INCOMPLETE)
                throw ls::vulkan_error(result, "device extension enumeration failed");
        }
        throw ls::vulkan_error(VK_INCOMPLETE, "device extensions changed during enumeration");
    }

    void myvkGetPhysicalDeviceFeatures2(
            VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* features) {
        // initVulkanInstanceFuncs resolves the core spelling with the KHR
        // alias as fallback. GPA below only exposes supported entrypoints.
        instance_info->funcs.GetPhysicalDeviceFeatures2(physicalDevice, features);
    }

    // Surface format queries are pure pass-throughs: the list, count and VkResult are the lower
    // driver's, unchanged. A complete answer (VK_SUCCESS with an array) is logged once per distinct
    // list, at most maxLoggedSurfaceFormatLists per process. Count-only and
    // VK_INCOMPLETE answers are not logged, and logging cannot change the answer.
    SurfaceFormatListLog surfaceFormatLists; // NOLINT (global variable)

    template <typename Entry, typename PairOf>
    void noteOfferedSurfaceFormats(const VkResult result, const uint32_t* count,
            const Entry* entries, PairOf pairOf) noexcept {
        const bool arrayReturned = count != nullptr && entries != nullptr;
        if (!logsSurfaceFormatQuery(result, arrayReturned))
            return;
        try {
            // A repeated list is keyed straight from the array and never copied.
            SurfaceFormatListKey key(*count);
            for (uint32_t index = 0; index < *count; ++index)
                key.add(pairOf(entries[index]));
            if (!surfaceFormatLists.claim(key.value()))
                return;
            std::vector<SurfaceFormatPair> pairs;
            pairs.reserve(*count);
            for (uint32_t index = 0; index < *count; ++index)
                pairs.push_back(pairOf(entries[index]));
            std::cerr << "framegen-vulkan: surface formats offered: "
                      << surfaceFormatListLine(pairs) << '\n';
        } catch (...) {} // Diagnostics cannot change the query's answer.
    }

    VkResult VKAPI_CALL myvkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice,
            VkSurfaceKHR surface, uint32_t* pSurfaceFormatCount, VkSurfaceFormatKHR* pSurfaceFormats) {
        const auto next = instance_info
            ? instance_info->funcs.GetPhysicalDeviceSurfaceFormatsKHR : nullptr;
        if (!next)
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        const auto result = next(physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);
        noteOfferedSurfaceFormats(result, pSurfaceFormatCount, pSurfaceFormats,
            [](const VkSurfaceFormatKHR& format) {
                return SurfaceFormatPair{static_cast<uint32_t>(format.format),
                    static_cast<uint32_t>(format.colorSpace)};
            });
        return result;
    }

    VkResult VKAPI_CALL myvkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice physicalDevice,
            const VkPhysicalDeviceSurfaceInfo2KHR* surfaceInfo, uint32_t* pSurfaceFormatCount,
            VkSurfaceFormat2KHR* pSurfaceFormats) {
        const auto next = instance_info
            ? instance_info->funcs.GetPhysicalDeviceSurfaceFormats2KHR : nullptr;
        if (!next)
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        const auto result = next(physicalDevice, surfaceInfo, pSurfaceFormatCount, pSurfaceFormats);
        noteOfferedSurfaceFormats(result, pSurfaceFormatCount, pSurfaceFormats,
            [](const VkSurfaceFormat2KHR& format) {
                return SurfaceFormatPair{static_cast<uint32_t>(format.surfaceFormat.format),
                    static_cast<uint32_t>(format.surfaceFormat.colorSpace)};
            });
        return result;
    }

    VkResult myvkCreateDevice(
            VkPhysicalDevice physdev,
            const VkDeviceCreateInfo* info,
            const VkAllocationCallbacks* alloc,
            VkDevice* device) {
        if (!layer_info || !instance_info) {
            std::cerr << "framegen-vulkan: device creation requested before "
                         "instance initialization\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        // apply layer chaining
        auto* layerInfo = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(info->pNext));
        while (layerInfo && (layerInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
                || layerInfo->function != VK_LAYER_LINK_INFO)) {
            layerInfo = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(layerInfo->pNext));
        }
        if (!layerInfo) {
            std::cerr << "framegen-vulkan: no layer info found in pNext chain, "
                "the previous layer does not follow spec\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto* linkInfo = layerInfo->u.pLayerInfo;
        if (!linkInfo) {
            std::cerr << "framegen-vulkan: link info is null, "
                "the previous layer does not follow spec\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        instance_info->funcs.GetDeviceProcAddr = linkInfo->pfnNextGetDeviceProcAddr;
        if (!linkInfo->pfnNextGetDeviceProcAddr) {
            std::cerr << "framegen-vulkan: next layer's vkGetDeviceProcAddr is null, "
                "the previous layer does not follow spec\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        layerInfo->u.pLayerInfo = linkInfo->pNext; // advance for next layer

        // fetch device loader functions
        layerInfo = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(info->pNext));
        while (layerInfo && (layerInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
                || layerInfo->function != VK_LOADER_DATA_CALLBACK)) {
            layerInfo = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(layerInfo->pNext));
        }
        if (!layerInfo) {
            std::cerr << "framegen-vulkan: no layer loader data found in pNext chain.\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto* setLoaderData = layerInfo->u.pfnSetDeviceLoaderData;
        if (!setLoaderData) {
            std::cerr << "framegen-vulkan: instance loader data function is null.\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const bool presentationDevice =
            swapchainPresentationEnabled(*info);
        const bool frameGenerationInteropEnabled = presentationDevice &&
            layer_info->root.frameGenerationInteropProvisioned();
        const auto swapchainMaintenance1Extension =
            supportedSwapchainMaintenance1Extension(
                physdev, presentationDevice && presentationRetirementRequired(
                    frameGenerationInteropEnabled)
            );
        const auto layerQueueFamily = presentationDevice
            ? selectLayerQueueFamily(physdev, *info)
            : std::nullopt;
        uint32_t privateFamilyCount{};
        instance_info->funcs.GetPhysicalDeviceQueueFamilyProperties(physdev, &privateFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> privateFamilies(privateFamilyCount);
        instance_info->funcs.GetPhysicalDeviceQueueFamilyProperties(physdev, &privateFamilyCount, privateFamilies.data());
        const PrivatePresentQueueRequest privatePresentQueue(*info, privateFamilies,
            frameGenerationInteropEnabled ? layerQueueFamily : std::nullopt);
        bool ownedPresentTiming = false;
        bool presentMeteringEnabled = false;
#if defined(VK_NV_present_metering)
        if (frameGenerationInteropEnabled && instance_info->funcs.GetPhysicalDeviceFeatures2) {
            const auto extensions = lowerDeviceExtensions(physdev);
            bool supplied = false;
            for (auto* node = static_cast<const VkBaseInStructure*>(info->pNext); node; node = node->pNext)
                supplied |= node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_METERING_FEATURES_NV;
            if (!supplied && std::ranges::any_of(extensions, [](const auto& extension) {
                    return std::strcmp(extension.extensionName, VK_NV_PRESENT_METERING_EXTENSION_NAME) == 0;
                })) {
                VkPhysicalDevicePresentMeteringFeaturesNV metering{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_METERING_FEATURES_NV};
                VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &metering};
                instance_info->funcs.GetPhysicalDeviceFeatures2(physdev, &features);
                presentMeteringEnabled = metering.presentMetering;
            }
        }
#endif
        bool copyCalibrationEnabled = false;
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
        if (frameGenerationInteropEnabled && instance_info->surfaceCapabilities2 &&
                OwnedPresentTiming::ownsNamespace(*info) &&
                instance_info->funcs.GetPhysicalDeviceFeatures2) {
            const auto extensions = lowerDeviceExtensions(physdev);
            const auto supported = [&](const char* name) {
                return std::ranges::any_of(extensions, [name](const auto& extension) {
                    return std::strcmp(extension.extensionName, name) == 0;
                });
            };
            if (supported(VK_EXT_PRESENT_TIMING_EXTENSION_NAME) &&
                    supported(VK_KHR_PRESENT_ID_2_EXTENSION_NAME) &&
                    supported(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)) {
                VkPhysicalDevicePresentId2FeaturesKHR ids{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR,
                };
                VkPhysicalDevicePresentTimingFeaturesEXT timing{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT,
                    .pNext = &ids,
                };
                VkPhysicalDeviceFeatures2 features{
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                    .pNext = &timing,
                };
                instance_info->funcs.GetPhysicalDeviceFeatures2(physdev, &features);
                ownedPresentTiming = ids.presentId2 && timing.presentTiming && timing.presentAtAbsoluteTime;
            }
        }
#endif
        bool presentRetirementEnabled = false;
        bool shaderFloat16Enabled = false;
        bool lowerDeviceCreated = false;
        const auto rollbackCreatedDevice = [&]() noexcept {
            if (!lowerDeviceCreated || !device || *device == VK_NULL_HANDLE)
                return;

            auto* vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(
                instance_info->funcs.GetDeviceProcAddr(
                    *device, "vkDestroyDevice"
                )
            );
            if (vkDestroyDevice) {
                vkDestroyDevice(*device, alloc);
                *device = VK_NULL_HANDLE;
            } else {
                std::cerr << "framegen-vulkan: failed to roll back a lower Vulkan device "
                             "after layer initialization failed\n";
            }
            lowerDeviceCreated = false;
        };

        // create device
        try {
            VkDeviceCreateInfo newInfo = *info;
            privatePresentQueue.apply(newInfo);
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
            std::vector<const char*> timingExtensions;
            VkPhysicalDevicePresentId2FeaturesKHR timingIds{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR,
                .pNext = const_cast<void*>(newInfo.pNext),
                .presentId2 = VK_TRUE,
            };
            VkPhysicalDevicePresentTimingFeaturesEXT timingFeatures{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT,
                .pNext = &timingIds,
                .presentTiming = VK_TRUE,
                .presentAtAbsoluteTime = VK_TRUE,
            };
            if (ownedPresentTiming) {
                for (uint32_t i = 0; i < newInfo.enabledExtensionCount; ++i)
                    timingExtensions.push_back(newInfo.ppEnabledExtensionNames[i]);
                for (const auto* name : {VK_EXT_PRESENT_TIMING_EXTENSION_NAME,
                        VK_KHR_PRESENT_ID_2_EXTENSION_NAME, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME}) {
                    if (!std::ranges::any_of(timingExtensions, [name](const auto* existing) {
                            return std::strcmp(name, existing) == 0;
                        }))
                        timingExtensions.push_back(name);
                }
                newInfo.enabledExtensionCount = static_cast<uint32_t>(timingExtensions.size());
                newInfo.ppEnabledExtensionNames = timingExtensions.data();
                newInfo.pNext = &timingFeatures;
            }
#endif
#if defined(VK_NV_present_metering)
            std::vector<const char*> meteringExtensions;
            VkPhysicalDevicePresentMeteringFeaturesNV meteringFeatures{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_METERING_FEATURES_NV,
                .pNext = const_cast<void*>(newInfo.pNext), .presentMetering = VK_TRUE,
            };
            if (presentMeteringEnabled) {
                for (uint32_t i = 0; i < newInfo.enabledExtensionCount; ++i)
                    meteringExtensions.push_back(newInfo.ppEnabledExtensionNames[i]);
                if (std::ranges::none_of(meteringExtensions, [](const char* name) {
                        return std::strcmp(name, VK_NV_PRESENT_METERING_EXTENSION_NAME) == 0;
                    })) meteringExtensions.push_back(VK_NV_PRESENT_METERING_EXTENSION_NAME);
                newInfo.enabledExtensionCount = static_cast<uint32_t>(meteringExtensions.size());
                newInfo.ppEnabledExtensionNames = meteringExtensions.data();
                newInfo.pNext = &meteringFeatures;
            }
#endif
            // GPU split diagnostics only (FGVK_PACING_PROFILE_GPU_SPLIT=1). Negotiate a supported
            // calibration alias and known clock domains without adding any feature node.
            std::vector<const char*> copyCalibrationExtensions;
            if (frameGenerationInteropEnabled &&
                    present_diagnostics::pacingGpuSplitProfileEnabled()) {
                const auto supported = lowerDeviceExtensions(physdev);
                const char* calibrationExtension{};
                for (const auto* name : {VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME})
                    if (!calibrationExtension && std::ranges::any_of(supported, [name](const auto& extension) {
                            return std::strcmp(extension.extensionName, name) == 0;
                        })) calibrationExtension = name;
                auto domains = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR>(
                    layer_info->GetInstanceProcAddr(instance_info->handles.front(), "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR"));
                if (!domains) domains = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR>(
                    layer_info->GetInstanceProcAddr(instance_info->handles.front(), "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
                std::array<VkTimeDomainKHR, 8> supportedDomains{};
                uint32_t count = supportedDomains.size();
                if (calibrationExtension && domains && domains(physdev, &count, supportedDomains.data()) == VK_SUCCESS && count <= supportedDomains.size()) {
                    const auto available = std::span{supportedDomains}.first(count);
                    copyCalibrationEnabled = std::ranges::find(available, VK_TIME_DOMAIN_DEVICE_KHR) != available.end() &&
                        std::ranges::find(available, VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR) != available.end();
                }
                if (copyCalibrationEnabled) {
                    for (uint32_t i = 0; i < newInfo.enabledExtensionCount; ++i)
                        copyCalibrationExtensions.push_back(newInfo.ppEnabledExtensionNames[i]);
                    if (!std::ranges::any_of(copyCalibrationExtensions, [calibrationExtension](const char* name) {
                            return std::strcmp(name, calibrationExtension) == 0;
                        })) copyCalibrationExtensions.push_back(calibrationExtension);
                    newInfo.enabledExtensionCount = static_cast<uint32_t>(copyCalibrationExtensions.size());
                    newInfo.ppEnabledExtensionNames = copyCalibrationExtensions.data();
                }
            }
            layer_info->root.modifyDeviceCreateInfo(
                newInfo,
                swapchainMaintenance1Extension.value_or(nullptr),
                [&, newInfo = &newInfo]() {
                    presentRetirementEnabled =
                        swapchainMaintenance1Enabled(*newInfo);
                    shaderFloat16Enabled = vk::shaderFloat16Enabled(newInfo->pNext);
                    auto res = instance_info->funcs.CreateDevice(physdev, newInfo, alloc, device);
                    present_diagnostics::traceVulkanSetup(layerRoleName, *info, *newInfo,
                        res, res == VK_SUCCESS ? *device : VK_NULL_HANDLE, physdev);
                    if (res != VK_SUCCESS)
                        throw ls::vulkan_error(res, "vkCreateDevice() failed");
                    lowerDeviceCreated = true;
                }
            );
        } catch (const ls::vulkan_error& e) {
            rollbackCreatedDevice();
            if (e.error() == VK_ERROR_EXTENSION_NOT_PRESENT)
                std::cerr << "framegen-vulkan: required Vulkan device extensions are not present. "
                    "Your GPU driver is not supported.\n";
            return e.error();
        } catch (const std::exception& e) {
            rollbackCreatedDevice();
            std::cerr << "framegen-vulkan: device creation failed:\n";
            std::cerr << "- " << e.what() << '\n';
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (layer_info->root.frameGenerationInteropProvisioned() &&
                !present_layout::registerDevice(*device, instance_info->funcs.GetDeviceProcAddr)) {
            rollbackCreatedDevice();
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        if (presentRetirementEnabled)
            instance_info->presentRetirementDevices.insert(*device);
        if (ownedPresentTiming)
            instance_info->presentTimingDevices.insert(*device);
        if (presentMeteringEnabled)
            instance_info->presentMeteringDevices.insert(*device);
        if (copyCalibrationEnabled)
            instance_info->copyCalibrationDevices.insert(*device);
        if (applicationOwnsPresentationTiming(*info))
            instance_info->applicationTimingDevices.insert(*device);
        if (enablesApplicationPresentId2Wait2(info->pNext))
            instance_info->presentFeedbackDevices.insert(*device);

        // No game profile matched when this device was created. Keep only the
        // layer lifecycle hooks needed to chain and clean up correctly; all
        // presentation entrypoints are forwarded directly by the GPA paths.
        if (!layer_info->root.active() || !presentationDevice) {
            instance_info->nativeDevices.insert(*device);
            lowerDeviceCreated = false;
            return VK_SUCCESS;
        }

        // create layer instance
        bool layerDeviceInserted = false;
        const auto discardLayerDevice = [&]() noexcept {
            std::erase_if(
                instance_info->queueIdentities,
                [deviceHandle = *device](const auto& entry) {
                    return entry.second.device == deviceHandle;
                }
            );
            if (layerDeviceInserted) {
                instance_info->devices.erase(*device);
                layerDeviceInserted = false;
            }
            instance_info->presentRetirementDevices.erase(*device);
            instance_info->presentTimingDevices.erase(*device);
            instance_info->presentMeteringDevices.erase(*device);
            instance_info->copyCalibrationDevices.erase(*device);
            instance_info->applicationTimingDevices.erase(*device);
            instance_info->presentFeedbackDevices.erase(*device);
            instance_info->deferredPresentQueues.erase(*device);
        };
        try {
            if (!layerQueueFamily) {
                throw ls::error("the application did not create an ordinary graphics queue family");
            }
            auto [wrapper, inserted] = instance_info->devices.emplace(
                *device,
                vk::Vulkan(
                    instance_info->handles.front(), *device, physdev,
                    instance_info->funcs, vk::initVulkanDeviceFuncs(instance_info->funcs, *device,
                        true, frameGenerationInteropEnabled),
                    *layerQueueFamily,
                    frameGenerationInteropEnabled,
                    true, setLoaderData, std::nullopt, shaderFloat16Enabled
                )
            );
            if (!inserted)
                throw ls::error("duplicate Vulkan device handle");
            layerDeviceInserted = true;

            if (privatePresentQueue.index) {
                auto privateQueue = std::make_shared<DeferredPresentQueue>();
                wrapper->second.df().GetDeviceQueue(*device, *layerQueueFamily,
                    *privatePresentQueue.index, &privateQueue->queue);
                if (privateQueue->queue && wrapper->second.loaderdatafunc()) {
                    const auto result = (*wrapper->second.loaderdatafunc())(*device, privateQueue->queue);
                    if (result != VK_SUCCESS) privateQueue->queue = VK_NULL_HANDLE;
                }
                // F9c: the private queue's tail timeline. Every submit on that queue signals
                // it, and the host acquire signal reads it. On failure it stays null and the
                // acquire is always queued.
                if (privateQueue->queue) {
                    const VkSemaphoreTypeCreateInfo tailType{
                        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                        .initialValue = 0,
                    };
                    const VkSemaphoreCreateInfo tailInfo{
                        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                        .pNext = &tailType,
                    };
                    VkSemaphore tail{VK_NULL_HANDLE};
                    if (wrapper->second.df().CreateSemaphore(*device, &tailInfo, nullptr, &tail) == VK_SUCCESS)
                        privateQueue->tail = tail;
                    // D7b: the submit history exists only with the pacing profile.
                    privateQueue->history = present_diagnostics::pacingProfileEnabled();
                }
                // F10: the host acquire signal needs the tail timeline and both counter functions.
                // It is also refused for the device's lifetime after a failed import.
                const bool hostAcquireAvailable = privateQueue->tail != VK_NULL_HANDLE &&
                    wrapper->second.df().GetSemaphoreCounterValueKHR != nullptr &&
                    wrapper->second.df().ImportSemaphoreFdKHR != nullptr;
                std::cerr << "framegen-vulkan: internal acquire queue: internal_prepare="
                          << 1
                          << " acquire_tail=" << (privateQueue->tail != VK_NULL_HANDLE ? 1 : 0)
                          << " host_acquire=" << (hostAcquireAvailable ? 1 : 0)
                          << '\n';
                // T4, GPU split profile, and S8b auto real offset: the marker after each lower present.
                if (privateQueue->queue) {
                    privateQueue->marker = createPresentMarker(wrapper->second,
                        instance_info->copyCalibrationDevices.contains(*device));
                    if (privateQueue->marker) {
                        privateQueue->markAfterPresent = &presentMarkerAfterPresent;
                        if (realOffsetMayEnable()) privateQueue->readMarkerSamples = &readPresentMarkerSamples;
                    }
                }
                // RL2: an out-of-band queue type on the private present queue only (never the application's queues),
                // and only when the application enabled VK_NV_low_latency2 on this device.
                const auto requested = reflexOutOfBandRequest();
                if (privateQueue->queue && requested != ReflexOutOfBandType::None) {
                    const auto applicationExtensions = std::span{info->ppEnabledExtensionNames, info->enabledExtensionCount};
                    const bool enabled = std::ranges::any_of(applicationExtensions, [](const char* name) {
                        return std::strcmp(name, VK_NV_LOW_LATENCY_2_EXTENSION_NAME) == 0;
                    });
                    const auto notify = enabled ? reinterpret_cast<PFN_vkQueueNotifyOutOfBandNV>(
                        instance_info->funcs.GetDeviceProcAddr(*device, "vkQueueNotifyOutOfBandNV")) : nullptr;
                    const VkOutOfBandQueueTypeInfoNV typeInfo{
                        .sType = VK_STRUCTURE_TYPE_OUT_OF_BAND_QUEUE_TYPE_INFO_NV,
                        .pNext = nullptr,
                        .queueType = requested == ReflexOutOfBandType::Present
                            ? VK_OUT_OF_BAND_QUEUE_TYPE_PRESENT_NV : VK_OUT_OF_BAND_QUEUE_TYPE_RENDER_NV,
                    };
                    const bool applied = notify != nullptr;
                    if (applied) notify(privateQueue->queue, &typeInfo);
                    reflexOutOfBandApplied.store(applied ? static_cast<uint32_t>(requested) : 0u,
                        std::memory_order_relaxed);
                    std::cerr << "framegen-vulkan: reflex out-of-band queue: type="
                              << (requested == ReflexOutOfBandType::Present ? "present" : "render")
                              << " applied=" << (applied ? 1 : 0) << '\n';
                }
                if (privateQueue->queue)
                    instance_info->deferredPresentQueues.emplace(*device, std::move(privateQueue));
            }

            // Queue handles are stable for the device lifetime. Register
            // every ordinary queue the application requested so scaling can
            // submit only on the exact externally-synchronized presentation
            // queue and only with a compatible command-pool family.
            for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i) {
                const auto& requested = info->pQueueCreateInfos[i];
                if (requested.flags != 0) {
                    continue;
                }
                for (uint32_t queueIndex = 0;
                        queueIndex < requested.queueCount; ++queueIndex) {
                    VkQueue queue{VK_NULL_HANDLE};
                    wrapper->second.df().GetDeviceQueue(
                        *device, requested.queueFamilyIndex,
                        queueIndex, &queue
                    );
                    if (queue == VK_NULL_HANDLE)
                        continue;
                    if (queue != wrapper->second.queue() &&
                            wrapper->second.loaderdatafunc()) {
                        const auto loaderResult =
                            (*wrapper->second.loaderdatafunc())(*device, queue);
                        if (loaderResult != VK_SUCCESS) {
                            throw ls::vulkan_error(
                                loaderResult,
                                "vkSetDeviceLoaderData() failed for application queue"
                            );
                        }
                    }
                    instance_info->queueIdentities.insert_or_assign(
                        queue,
                        InstanceInfo::QueueIdentity{
                            .device = *device,
                            .familyIndex = requested.queueFamilyIndex,
                        }
                    );
                }
            }
        } catch (const std::exception& e) {
            // Destroy layer-owned command pools/cache while the valid lower
            // VkDevice still exists; only then may rollback destroy the device
            // or native fallback expose it without a stale wrapper.
            discardLayerDevice();
            // The lower layer already returned a valid VkDevice. Do not leave
            // it exposed to framegen-vulkan's swapchain hooks without a matching Vulkan
            // wrapper: retain the device as a completely native pass-through.
            instance_info->nativeDevices.insert(*device);
            std::cerr << "framegen-vulkan: device initialization failed; "
                         "native Vulkan device retained:\n";
            std::cerr << "- " << e.what() << '\n';
        }

        lowerDeviceCreated = false;

        return VK_SUCCESS;
    }

    VkResult completeDeferredPresent(VkSwapchainKHR swapchain, uint64_t timeout) {
        const auto mapping = instance_info->swapchains.find(swapchain);
        if (mapping == instance_info->swapchains.end() ||
                instance_info->nativeSwapchains.contains(swapchain)) return VK_SUCCESS;
        try {
            return layer_info->root.getSwapchainContext(swapchain)
                .completeDeferredPresent(timeout);
        } catch (const std::exception&) { return VK_ERROR_INITIALIZATION_FAILED; }
    }

    // An application present may keep one sealed overlapped batch spacing:
    // resource replacement joins it before retiring anything it waits on.
    void drainDeferredDevice(VkDevice device, bool retainOverlap = false) {
        for (const auto& [swapchain, wrapper] : instance_info->swapchains) {
            if (wrapper.get().dev() != device) continue;
            if (!retainOverlap) {
                static_cast<void>(completeDeferredPresent(swapchain, UINT64_MAX));
                continue;
            }
            if (instance_info->nativeSwapchains.contains(swapchain)) continue;
            try {
                static_cast<void>(layer_info->root.getSwapchainContext(swapchain)
                    .completeDeferredPresentRetainingOverlap());
            } catch (const std::exception&) {}
        }
    }

    VkResult myvkDeviceWaitIdleBody(VkDevice device);
    VkResult myvkDeviceWaitIdle(VkDevice device) {
        present_diagnostics::LayerCallSpan span(present_diagnostics::LayerCall::DeviceWaitIdle);
        return span.returned(myvkDeviceWaitIdleBody(device));
    }
    VkResult myvkDeviceWaitIdleBody(VkDevice device) {
        drainDeferredDevice(device);
        const auto lower = reinterpret_cast<PFN_vkDeviceWaitIdle>(
            instance_info->funcs.GetDeviceProcAddr(device, "vkDeviceWaitIdle"));
        // vkDeviceWaitIdle host-synchronises every queue of the device, so it holds
        // the private queue's lock too.
        if (const auto owned = instance_info->deferredPresentQueues.find(device);
                owned != instance_info->deferredPresentQueues.end()) {
            const std::lock_guard queueLock(owned->second->mutex);
            return lower(device);
        }
        return lower(device);
    }

    VkResult myvkQueueWaitIdleBody(VkQueue queue);
    VkResult myvkQueueWaitIdle(VkQueue queue) {
        present_diagnostics::LayerCallSpan span(present_diagnostics::LayerCall::QueueWaitIdle);
        return span.returned(myvkQueueWaitIdleBody(queue));
    }
    VkResult myvkQueueWaitIdleBody(VkQueue queue) {
        const auto identity = instance_info->queueIdentities.find(queue);
        if (identity == instance_info->queueIdentities.end()) {
            const auto lower = reinterpret_cast<PFN_vkQueueWaitIdle>(
                layer_info->GetInstanceProcAddr(instance_info->handles.front(), "vkQueueWaitIdle"));
            return lower(queue);
        }
        const auto device = identity->second.device;
        drainDeferredDevice(device);
        const auto lower = reinterpret_cast<PFN_vkQueueWaitIdle>(
            instance_info->funcs.GetDeviceProcAddr(device, "vkQueueWaitIdle"));
        if (const auto owned = instance_info->deferredPresentQueues.find(device);
                owned != instance_info->deferredPresentQueues.end()) {
            {
                const std::lock_guard queueLock(owned->second->mutex);
                const auto result = lower(owned->second->queue);
                if (result != VK_SUCCESS) return result;
            }
        }
        return lower(queue);
    }

    // Lower pointers cached per thread, so an unarmed application submit
    // or wait costs thread-local loads instead of a map lookup and GDPA.
    // Device destruction invalidates every cache: handles can be reused.
    std::atomic<uint64_t> deviceDispatchEpoch{0};

    template<typename Function, typename Handle>
    struct CachedLower {
        Handle handle{};
        uint64_t epoch{UINT64_MAX};
        Function function{};
        template<typename Resolve>
        Function get(const Handle current, Resolve&& resolve) {
            const auto now = deviceDispatchEpoch.load(std::memory_order_acquire);
            if (current != handle || epoch != now || !function) {
                function = reinterpret_cast<Function>(resolve());
                handle = current;
                epoch = now;
            }
            return function;
        }
    };

    PFN_vkVoidFunction lowerQueueCommand(const VkQueue queue, const char* name) {
        const auto identity = instance_info->queueIdentities.find(queue);
        if (identity == instance_info->queueIdentities.end())
            return layer_info->GetInstanceProcAddr(instance_info->handles.front(), name);
        return instance_info->funcs.GetDeviceProcAddr(identity->second.device, name);
    }

    // RX1: each game submit's frame tag, observed with the pacing profile only. A change of a queue's tag is
    // recorded. Nothing here changes the submit.
    void observeSubmitTags(const VkQueue queue, const uint32_t submitCount, const VkSubmitInfo2* submits) {
        static SubmitTagTracker tracker;
        for (uint32_t index = 0; submits && index < submitCount; ++index) {
            const uint64_t presentId = submissionPresentId(submits[index].pNext);
            if (const auto transition = tracker.observe(queue, presentId))
                present_diagnostics::recordSubmitTag(queue, presentId, *transition);
        }
    }

    // See swapchain/release_gate.hpp: record the cap deadline against the
    // signal-only submit that follows an application present.
    template<const char* Name>
    VkResult VKAPI_CALL myvkQueueSubmit2CommonBody(VkQueue queue, uint32_t submitCount,
            const VkSubmitInfo2* submits, VkFence fence);
    template<const char* Name>
    VkResult VKAPI_CALL myvkQueueSubmit2Common(VkQueue queue, uint32_t submitCount,
            const VkSubmitInfo2* submits, VkFence fence) {
        present_diagnostics::LayerCallSpan span(present_diagnostics::LayerCall::QueueSubmit2);
        return span.returned(myvkQueueSubmit2CommonBody<Name>(queue, submitCount, submits, fence));
    }
    template<const char* Name>
    VkResult VKAPI_CALL myvkQueueSubmit2CommonBody(VkQueue queue, uint32_t submitCount,
            const VkSubmitInfo2* submits, VkFence fence) {
        static thread_local CachedLower<PFN_vkQueueSubmit2, VkQueue> lower;
        const auto next = lower.get(queue, [&] { return lowerQueueCommand(queue, Name); });
        ReleaseGate::global().observeSubmit2(queue, submitCount, submits, fence);
        if (present_diagnostics::pacingProfileEnabled()) observeSubmitTags(queue, submitCount, submits);
        return next(queue, submitCount, submits, fence);
    }
    constexpr char queueSubmit2Name[] = "vkQueueSubmit2";
    constexpr char queueSubmit2KhrName[] = "vkQueueSubmit2KHR";

    // Delay a successful application wait on a recorded release token until
    // its cap deadline. A deadline after the caller's timeout keeps the token
    // and reports VK_TIMEOUT, as if the value had not yet been reached.
    template<const char* Name>
    VkResult VKAPI_CALL myvkWaitSemaphoresCommonBody(VkDevice device,
            const VkSemaphoreWaitInfo* info, uint64_t timeout);
    template<const char* Name>
    VkResult VKAPI_CALL myvkWaitSemaphoresCommon(VkDevice device,
            const VkSemaphoreWaitInfo* info, uint64_t timeout) {
        present_diagnostics::LayerCallSpan span(present_diagnostics::LayerCall::WaitSemaphores);
        if (info) span.semaphoreWait(info->semaphoreCount, timeout);
        return span.returned(myvkWaitSemaphoresCommonBody<Name>(device, info, timeout));
    }
    template<const char* Name>
    VkResult VKAPI_CALL myvkWaitSemaphoresCommonBody(VkDevice device,
            const VkSemaphoreWaitInfo* info, uint64_t timeout) {
        static thread_local CachedLower<PFN_vkWaitSemaphores, VkDevice> lower;
        const auto next = lower.get(device, [&] {
            return instance_info->funcs.GetDeviceProcAddr(device, Name); });
        auto& gate = ReleaseGate::global();
        if (gate.empty() || !info) return next(device, info, timeout);
        using Clock = ReleaseGate::Clock;
        const auto started = Clock::now();
        const auto result = next(device, info, timeout);
        if (result != VK_SUCCESS) return result;
        const auto now = Clock::now();
        const auto limit = timeout >= uint64_t{3'600'000'000'000}
            ? Clock::time_point::max() : started + std::chrono::nanoseconds(timeout);
        const auto release = gate.released(*info, now, limit);
        if (!release) return result;
        std::this_thread::sleep_until(release->consumed ? release->deadline : limit);
        if (release->consumed) ReleaseGate::noteReturned(*release, Clock::now());
        return release->consumed ? VK_SUCCESS : VK_TIMEOUT;
    }
    constexpr char waitSemaphoresName[] = "vkWaitSemaphores";
    constexpr char waitSemaphoresKhrName[] = "vkWaitSemaphoresKHR";

    // A successful application wait on a present ID is held to the deadline of the
    // present's release token, as the semaphore wait above is (see release_gate.hpp).
    // The sleep is on the waiting thread. A deadline past the caller's timeout keeps the
    // token and reports VK_TIMEOUT.
    VkResult holdPresentRelease(const VkSwapchainKHR swapchain, const uint64_t presentId,
            const uint64_t timeout, const ReleaseGate::Clock::time_point started, const VkResult result) {
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return result;
        auto& gate = ReleaseGate::global();
        if (gate.empty()) return result;
        using Clock = ReleaseGate::Clock;
        const auto now = Clock::now();
        const auto limit = timeout >= uint64_t{3'600'000'000'000}
            ? Clock::time_point::max() : started + std::chrono::nanoseconds(timeout);
        const auto release = gate.releasedPresent(swapchain, presentId, now, limit);
        if (!release) return result;
        std::this_thread::sleep_until(release->consumed ? release->deadline : limit);
        if (release->consumed) ReleaseGate::noteReturned(*release, Clock::now());
        return release->consumed ? result : VK_TIMEOUT;
    }

    bool isOptionalReleaseCommand(const std::string_view name) {
        return name == queueSubmit2Name || name == queueSubmit2KhrName ||
            name == waitSemaphoresName || name == waitSemaphoresKhrName;
    }

    // Exported only where the lower instance exports it, like the features2 query: an
    // unmatched process gets the lower pointer, and a matched one gets the logging wrapper.
    bool isOptionalSurfaceFormatCommand(const std::string_view name) {
        return name == "vkGetPhysicalDeviceSurfaceFormatsKHR" ||
            name == "vkGetPhysicalDeviceSurfaceFormats2KHR";
    }

    // Layer-owned swapchain images intercept these only when the lower
    // device exposes the matching release command.
    bool isOptionalSwapchainReleaseCommand(const std::string_view name) {
        return name == "vkReleaseSwapchainImagesKHR" ||
            name == "vkReleaseSwapchainImagesEXT";
    }

    // destroy device
    void myvkDestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
        if (!instance_info)
            return;
        deviceDispatchEpoch.fetch_add(1, std::memory_order_acq_rel);

        collectRetiredSwapchains(device, 250'000'000);
        forceFinalizeRetiredSwapchains(device);
        drainDeferredDevice(device);
        // F9c: the tail timeline goes with its queue. Device destruction requires the
        // queues to be idle, so no wait is needed before it is destroyed.
        if (const auto owned = instance_info->deferredPresentQueues.find(device);
                owned != instance_info->deferredPresentQueues.end() && owned->second->tail != VK_NULL_HANDLE) {
            if (const auto wrapper = instance_info->devices.find(device); wrapper != instance_info->devices.end())
                wrapper->second.df().DestroySemaphore(device, owned->second->tail, nullptr);
            owned->second->tail = VK_NULL_HANDLE;
        }
        // T4: the marker's pools and fences go before the device. The device is idle here.
        if (const auto owned = instance_info->deferredPresentQueues.find(device);
                owned != instance_info->deferredPresentQueues.end()) {
            const std::lock_guard lock(owned->second->mutex);
            owned->second->markAfterPresent = nullptr;
            owned->second->readMarkerSamples = nullptr;
            owned->second->marker.reset();
        }
        instance_info->deferredPresentQueues.erase(device);

        // destroy layer instance
        auto it = instance_info->devices.find(device);
        if (it != instance_info->devices.end())
            instance_info->devices.erase(it);
        std::erase_if(
            instance_info->queueIdentities,
            [device](const auto& entry) {
                return entry.second.device == device;
            }
        );
        instance_info->nativeDevices.erase(device);
        instance_info->presentRetirementDevices.erase(device);
        instance_info->presentTimingDevices.erase(device);
        instance_info->presentMeteringDevices.erase(device);
        instance_info->copyCalibrationDevices.erase(device);
        instance_info->applicationTimingDevices.erase(device);
        instance_info->presentFeedbackDevices.erase(device);

        // destroy device
        auto vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(
            instance_info->funcs.GetDeviceProcAddr(device, "vkDestroyDevice"));
        if (!vkDestroyDevice) {
            std::cerr << "framegen-vulkan: failed to get next layer's vkDestroyDevice, "
                "the previous layer does not follow spec\n";
            return;
        }

        if (layer_info->root.frameGenerationInteropProvisioned())
            present_layout::unregisterDevice(device);
        vkDestroyDevice(device, alloc);
    }

    // destroy instance
    void myvkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* alloc) {
        // remove instance handle
        auto it = std::ranges::find(instance_info->handles, instance);
        if (it != instance_info->handles.end())
            instance_info->handles.erase(it);

        const bool lastInstance = instance_info->handles.empty();

        // destroy instance
        auto vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
            layer_info->GetInstanceProcAddr(instance, "vkDestroyInstance"));
        if (!vkDestroyInstance) {
            std::cerr << "framegen-vulkan: failed to get next layer's vkDestroyInstance, "
                "the previous layer does not follow spec\n";

            if (lastInstance) {
                delete instance_info; // NOLINT (memory management)
                instance_info = nullptr;
                delete layer_info; // NOLINT (memory management)
                layer_info = nullptr;
            }
            return;
        }

        vkDestroyInstance(instance, alloc);

        // The Vulkan loader may unload this shared object as soon as the final
        // vkDestroyInstance() wrapper returns. Root and its deferred-present
        // workers own threads, so leaving the process-global LayerInfo allocated
        // lets those workers resume inside unmapped layer code after dlclose().
        // Destroy all layer-owned state, including joining the monitor, while
        // our code is still resident. A later loader negotiation can recreate
        // the state if the application opens another Vulkan instance.
        if (lastInstance) {
            delete instance_info; // NOLINT (memory management)
            instance_info = nullptr;
            delete layer_info; // NOLINT (memory management)
            layer_info = nullptr;
        }
    }

    bool isPresentWaitCommand(const std::string_view name) {
        return name == "vkWaitForPresentKHR"
#if defined(VK_KHR_present_wait2)
            || name == "vkWaitForPresent2KHR"
#endif
            ;
    }

    bool isOptionalPresentationCommand(const std::string_view name) {
        return name == "vkAcquireNextImage2KHR" || isPresentWaitCommand(name);
    }

    InternalSwapchainImages* internalImagesFor(VkSwapchainKHR swapchain);

    // X1: a swapchain error from a present wait also becomes sticky on the layer's
    // internal images, so an application acquire blocked on them returns it. The
    // result itself is returned unchanged.
    VkResult noteInternalPresentWaitError(const VkSwapchainKHR swapchain, const VkResult result) {
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR ||
                result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
            if (auto* internal = internalImagesFor(swapchain))
                internal->noteLowerError(result);
        }
        return result;
    }

    // Instrument only an application-invoked wait. Do not invent completion,
    // shorten timeouts, reinterpret present IDs, or take a swapchain-owner lock
    // across the driver's blocking call. GPA gates retain optional support.
    VkResult VKAPI_CALL myvkWaitForPresentKHRBody(VkDevice device,
            VkSwapchainKHR swapchain, uint64_t presentId, uint64_t timeout);
    VkResult VKAPI_CALL myvkWaitForPresentKHR(VkDevice device,
            VkSwapchainKHR swapchain, uint64_t presentId, uint64_t timeout) {
        present_diagnostics::LayerCallSpan span(present_diagnostics::LayerCall::WaitForPresent);
        span.presentWait(presentId, timeout);
        return span.returned(myvkWaitForPresentKHRBody(device, swapchain, presentId, timeout));
    }
    VkResult VKAPI_CALL myvkWaitForPresentKHRBody(VkDevice device,
            VkSwapchainKHR swapchain, uint64_t presentId, uint64_t timeout) {
        const auto next = reinterpret_cast<PFN_vkWaitForPresentKHR>(
            instance_info->funcs.GetDeviceProcAddr(device, "vkWaitForPresentKHR"));
        return present_diagnostics::observeApplicationPresentWait(device, swapchain,
            present_diagnostics::PresentWaitApi::Khr, presentId, timeout,
            [&] {
                const auto started = ReleaseGate::Clock::now();
                // GP1: the lower ID (the application's own ID outside the on-screen gate).
                const auto result = DeferredPresent::waitForPresent(device, swapchain, presentId, timeout,
                    [&](uint64_t lowerId, uint64_t remaining) { return next(device, swapchain, lowerId, remaining); });
                return noteInternalPresentWaitError(swapchain,
                    holdPresentRelease(swapchain, presentId, timeout, started, result));
            });
    }

#if defined(VK_KHR_present_wait2)
    VkResult VKAPI_CALL myvkWaitForPresent2KHRBody(VkDevice device,
            VkSwapchainKHR swapchain, const VkPresentWait2InfoKHR* info);
    VkResult VKAPI_CALL myvkWaitForPresent2KHR(VkDevice device,
            VkSwapchainKHR swapchain, const VkPresentWait2InfoKHR* info) {
        present_diagnostics::LayerCallSpan span(present_diagnostics::LayerCall::WaitForPresent2);
        if (info) span.presentWait(info->presentId, info->timeout);
        return span.returned(myvkWaitForPresent2KHRBody(device, swapchain, info));
    }
    VkResult VKAPI_CALL myvkWaitForPresent2KHRBody(VkDevice device,
            VkSwapchainKHR swapchain, const VkPresentWait2InfoKHR* info) {
        const auto next = reinterpret_cast<PFN_vkWaitForPresent2KHR>(
            instance_info->funcs.GetDeviceProcAddr(device, "vkWaitForPresent2KHR"));
        return present_diagnostics::observeApplicationPresentWait(device, swapchain,
            present_diagnostics::PresentWaitApi::Khr2, info->presentId, info->timeout,
            [&] {
                const auto started = ReleaseGate::Clock::now();
                const auto result = DeferredPresent::waitForPresent(device, swapchain, info->presentId, info->timeout,
                    [&](uint64_t lowerId, uint64_t remaining) {
                        // GP1: the lower ID (the application's own ID outside the on-screen gate).
                        auto adjusted = *info;
                        adjusted.presentId = lowerId;
                        adjusted.timeout = remaining;
                        return next(device, swapchain, &adjusted);
                    });
                return noteInternalPresentWaitError(swapchain,
                    holdPresentRelease(swapchain, info->presentId, info->timeout, started, result));
            });
    }
#endif

    // get optional function pointer override
    PFN_vkVoidFunction getProcAddr(const std::string& name) {
        // The application role also joins its present waits for completion pacing
        // when diagnostics are off.
        auto it = layer_info->map.find(name);
        if (it == layer_info->map.end()) return nullptr;

        if (layer_info->root.active()) return it->second;

        // A dormant layer still owns its instance/device lifecycle wrappers,
        // which establish the next-layer pointers and release process state.
        // Swapchain and present entrypoints must bypass framegen-vulkan completely because
        // an unmatched application did not enable framegen-vulkan's required extensions.
        if (name == "vkCreateInstance" || name == "vkDestroyInstance"
                || name == "vkCreateDevice" || name == "vkDestroyDevice")
            return it->second;

        return nullptr;
    }

    // get instance-level function pointers
    PFN_vkVoidFunction myvkGetInstanceProcAddr(VkInstance instance, const char* name) {
        if (!name) return nullptr;
        // A loader may cache the negotiated GPA function across multiple
        // VkInstance lifetimes without negotiating again. The final instance
        // teardown destroys Root so its monitor thread is joined before a
        // possible dlclose; recreate that process state on the next GPA query.
        if (!layer_info && !initializeLayerInfo()) return nullptr;

        if (std::string_view(name) == "vkGetPhysicalDeviceFeatures2" ||
                std::string_view(name) == "vkGetPhysicalDeviceFeatures2KHR" ||
                isOptionalSurfaceFormatCommand(name) ||
                isOptionalPresentationCommand(name) || isOptionalReleaseCommand(name) ||
                isOptionalSwapchainReleaseCommand(name)) {
            if (!instance || !layer_info->GetInstanceProcAddr ||
                    !layer_info->GetInstanceProcAddr(instance, name)) {
                return nullptr;
            }
        }

        auto func = getProcAddr(name);
        if (func) return func;

        if (!layer_info->GetInstanceProcAddr) return nullptr;
        const auto lower = layer_info->GetInstanceProcAddr(instance, name);
        if (lower && instance && layer_info->root.frameGenerationInteropProvisioned()) {
            if (auto override = present_layout::command(name)) return override;
        }
        return lower;
    }

    // get device-level function pointers
    // Reflex wrappers. A request on a framegen-vulkan swapchain is remembered and forwarded with the
    // frame limit applied (reflex_frame_limit.hpp); any other request is forwarded unchanged. The
    // extension is never advertised; these answer only an app that asks for it.
    VkResult VKAPI_CALL myvkSetLatencySleepModeNV(VkDevice device, VkSwapchainKHR swapchain,
            const VkLatencySleepModeInfoNV* info) {
        static thread_local CachedLower<PFN_vkSetLatencySleepModeNV, VkDevice> lower;
        const auto next = lower.get(device, [&] {
            return instance_info->funcs.GetDeviceProcAddr(device, "vkSetLatencySleepModeNV"); });
        if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
        const auto limit = ReflexFrameLimitRegistry::get(swapchain);
        const auto result = info
            ? limit->applicationRequest(swapchain, *info, [&](const ReflexFrameLimit::Forward& forward) {
                return next(device, swapchain, &forward.info);
            })
            : limit->clearRequest([&] { return next(device, swapchain, nullptr); });
        present_diagnostics::recordReflexMode(info ? info->lowLatencyMode : VK_FALSE,
            info ? info->lowLatencyBoost : VK_FALSE, info ? info->minimumIntervalUs : 0, result);
        return result;
    }

    // Driver Reflex frame reports of a managed swapchain, read with the pacing profile on. Every 16th sleep makes two
    // vkGetLatencyTimingsNV calls (the count, then up to 64 reports); each report with a new present ID is recorded.
    // Nothing here changes the sleep's result, and no other path calls it.
    void recordReflexTimings(const VkDevice device, const VkSwapchainKHR swapchain) {
        if (!present_diagnostics::pacingProfileEnabled()) return;
        const auto attribution = ReflexPresentAttributionRegistry::find(swapchain);
        if (!attribution || !attribution->timing().due()) return;
        static thread_local CachedLower<PFN_vkGetLatencyTimingsNV, VkDevice> lower;
        const auto query = lower.get(device, [&] {
            return instance_info->funcs.GetDeviceProcAddr(device, "vkGetLatencyTimingsNV"); });
        if (!query) return;
        constexpr uint32_t capacity = 64;
        const auto started = present_diagnostics::Clock::now();
        VkGetLatencyMarkerInfoNV info{
            .sType = VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV, .pNext = nullptr,
            .timingCount = 0, .pTimings = nullptr,
        };
        query(device, swapchain, &info);
        std::array<VkLatencyTimingsFrameReportNV, capacity> reports{};
        for (auto& report : reports) report.sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV;
        info.timingCount = std::min(info.timingCount, capacity);
        info.pTimings = reports.data();
        query(device, swapchain, &info);
        const auto queryNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            present_diagnostics::Clock::now() - started).count();
        const auto returned = std::min(info.timingCount, capacity);
        for (uint32_t index = 0; index < returned; ++index) {
            const auto& report = reports[index];
            if (report.presentID != 0 && attribution->timing().fresh(report.presentID))
                present_diagnostics::recordReflexTiming(report, queryNs);
        }
    }

    VkResult VKAPI_CALL myvkLatencySleepNV(VkDevice device, VkSwapchainKHR swapchain,
            const VkLatencySleepInfoNV* info) {
        static thread_local CachedLower<PFN_vkLatencySleepNV, VkDevice> lower;
        const auto next = lower.get(device, [&] {
            return instance_info->funcs.GetDeviceProcAddr(device, "vkLatencySleepNV"); });
        if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
        const auto entered = present_diagnostics::Clock::now();
        const auto result = next(device, swapchain, info);
        present_diagnostics::recordReflexSleep(entered, present_diagnostics::Clock::now(), result);
        recordReflexTimings(device, swapchain);
        return result;
    }

    void VKAPI_CALL myvkSetLatencyMarkerNV(VkDevice device, VkSwapchainKHR swapchain,
            const VkSetLatencyMarkerInfoNV* info) {
        static thread_local CachedLower<PFN_vkSetLatencyMarkerNV, VkDevice> lower;
        const auto next = lower.get(device, [&] {
            return instance_info->funcs.GetDeviceProcAddr(device, "vkSetLatencyMarkerNV"); });
        if (!next) return;
        if (info) present_diagnostics::recordReflexMarker(info->presentID, info->marker,
            present_diagnostics::Clock::now());
        // A managed swapchain's out-of-band pair is fgvk's own (reflex_present_attribution.hpp).
        if (info) {
            const auto attribution = ReflexPresentAttributionRegistry::find(swapchain);
            if (attribution && !attribution->applicationMarker(info->marker, info->presentID)) return;
        }
        next(device, swapchain, info);
    }

    // The Reflex wrapper for a command the next layer exports. A next layer without the command
    // returns nothing and the lookup stays unchanged. Recording inside the wrappers is still
    // gated by the pacing profile.
    PFN_vkVoidFunction reflexPassThrough(VkDevice device, const char* name) {
        PFN_vkVoidFunction wrapper{};
        if (std::string_view(name) == "vkSetLatencySleepModeNV")
            wrapper = reinterpret_cast<PFN_vkVoidFunction>(myvkSetLatencySleepModeNV);
        else if (std::string_view(name) == "vkLatencySleepNV")
            wrapper = reinterpret_cast<PFN_vkVoidFunction>(myvkLatencySleepNV);
        else if (std::string_view(name) == "vkSetLatencyMarkerNV")
            wrapper = reinterpret_cast<PFN_vkVoidFunction>(myvkSetLatencyMarkerNV);
        if (!wrapper || !instance_info->funcs.GetDeviceProcAddr(device, name)) return nullptr;
        return wrapper;
    }

    PFN_vkVoidFunction myvkGetDeviceProcAddr(VkDevice device, const char* name) {
        if (!name) return nullptr;
        if (!layer_info || !instance_info) return nullptr;

        if (!instance_info->funcs.GetDeviceProcAddr) return nullptr;

        if ((isOptionalPresentationCommand(name) || isOptionalReleaseCommand(name) ||
                isOptionalSwapchainReleaseCommand(name)) &&
                !instance_info->funcs.GetDeviceProcAddr(device, name))
            return nullptr;

        // A lower VkDevice can outlive a failed optional framegen-vulkan wrapper
        // initialization. Forward every command for that device directly,
        // retaining only our destruction hook so the pass-through registry is
        // cleaned before the driver destroys the handle.
        if (instance_info->nativeDevices.contains(device)) {
            if (std::string(name) == "vkDestroyDevice")
                return reinterpret_cast<PFN_vkVoidFunction>(myvkDestroyDevice);
            return instance_info->funcs.GetDeviceProcAddr(device, name);
        }

        if (const auto reflex = reflexPassThrough(device, name)) return reflex;
        auto func = getProcAddr(name);
        if (func) return func;

        const auto lower = instance_info->funcs.GetDeviceProcAddr(device, name);
        if (lower && layer_info->root.frameGenerationInteropProvisioned()) {
            if (auto override = present_layout::command(name)) return override;
        }
        return lower;
    }
}

namespace {
    // Layout mirrors of VkXcbSurfaceCreateInfoKHR (vulkan_xcb.h) and VkXlibSurfaceCreateInfoKHR
    // (vulkan_xlib.h). The layer keeps the X create-info types opaque, so only the window XID is read,
    // after the lower driver accepted the create info. Both structs end with the window.
    struct XcbSurfaceCreateInfoLayout {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        void* connection;
        uint32_t window;
    };
    struct XlibSurfaceCreateInfoLayout {
        VkStructureType sType;
        const void* pNext;
        VkFlags flags;
        void* display;
        unsigned long window;
    };

    template <typename Layout, typename CreateInfo>
    [[nodiscard]] uint64_t x11WindowOf(const CreateInfo* createInfo) {
        return createInfo
            ? static_cast<uint64_t>(reinterpret_cast<const Layout*>(createInfo)->window)
            : 0;
    }

    void recordSurfaceOrigin(
            const VkSurfaceKHR surface,
            const SurfaceOrigin origin,
            const uint64_t x11Window = 0) {
        if (!instance_info || surface == VK_NULL_HANDLE)
            return;
        const std::lock_guard lock(instance_info->surfaceStateMutex);
        instance_info->surfaceOrigins.emplace(surface, origin);
        if (x11Window != 0)
            instance_info->surfaceX11Windows.insert_or_assign(surface, x11Window);
    }

    [[nodiscard]] SurfaceOrigin surfaceOrigin(
            const VkSurfaceKHR surface) {
        if (!instance_info)
            return SurfaceOrigin::Unknown;
        const std::lock_guard lock(instance_info->surfaceStateMutex);
        const auto origin = instance_info->surfaceOrigins.find(surface);
        return origin == instance_info->surfaceOrigins.end()
            ? SurfaceOrigin::Unknown : origin->second;
    }

    [[nodiscard]] uint64_t surfaceX11Window(
            const VkSurfaceKHR surface) {
        if (!instance_info)
            return 0;
        const std::lock_guard lock(instance_info->surfaceStateMutex);
        const auto window = instance_info->surfaceX11Windows.find(surface);
        return window == instance_info->surfaceX11Windows.end() ? 0 : window->second;
    }

    void myvkDestroySurfaceKHR(
            const VkInstance instance, const VkSurfaceKHR surface,
            const VkAllocationCallbacks* alloc) {
        if (instance_info) {
            // Destroying the application surface is an explicit terminal
            // boundary: no replacement swapchain on this surface can follow.
            // Drain the maintenance1 lifetime proof and release every deferred
            // lower swapchain before forwarding the surface destruction.
            collectRetiredSwapchainsForSurface(surface);
            {
                const std::lock_guard lock(instance_info->surfaceStateMutex);
                instance_info->surfaceOrigins.erase(surface);
                instance_info->surfaceX11Windows.erase(surface);
                if (const auto feedback = instance_info->waylandPresentation.find(surface);
                        feedback != instance_info->waylandPresentation.end()) {
                    feedback->second->stop();
                    instance_info->waylandPresentation.erase(feedback);
                }
            }
        }
        const auto lower = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
            layer_info->GetInstanceProcAddr(instance, "vkDestroySurfaceKHR")
        );
        if (lower)
            lower(instance, surface, alloc);
    }

    using CreateWaylandSurface = VkResult (VKAPI_PTR *)(
        VkInstance, const VkWaylandSurfaceCreateInfoKHR*,
        const VkAllocationCallbacks*, VkSurfaceKHR*
    );
    using CreateXcbSurface = VkResult (VKAPI_PTR *)(
        VkInstance, const VkXcbSurfaceCreateInfoKHR*,
        const VkAllocationCallbacks*, VkSurfaceKHR*
    );
    using CreateXlibSurface = VkResult (VKAPI_PTR *)(
        VkInstance, const VkXlibSurfaceCreateInfoKHR*,
        const VkAllocationCallbacks*, VkSurfaceKHR*
    );

    template <typename CreateSurface, typename CreateInfo>
    VkResult createSurfaceAndRecord(
            const VkInstance instance,
            const CreateInfo* createInfo,
            const VkAllocationCallbacks* alloc,
            VkSurfaceKHR* surface,
            const char* entrypoint,
            const SurfaceOrigin origin,
            const uint64_t x11Window = 0) {
        const auto lower = reinterpret_cast<CreateSurface>(
            layer_info->GetInstanceProcAddr(instance, entrypoint)
        );
        if (!lower)
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        const auto result = lower(instance, createInfo, alloc, surface);
        present_diagnostics::traceVulkanSurface(layerRoleName, entrypoint, instance,
            result == VK_SUCCESS && surface ? *surface : VK_NULL_HANDLE, result);
        if (result == VK_SUCCESS && surface)
            recordSurfaceOrigin(*surface, origin, x11Window);
        return result;
    }

    VkResult myvkCreateWaylandSurfaceKHR(
            const VkInstance instance,
            const VkWaylandSurfaceCreateInfoKHR* createInfo,
            const VkAllocationCallbacks* alloc,
            VkSurfaceKHR* surface) {
        const auto result = createSurfaceAndRecord<
            CreateWaylandSurface
        >(instance, createInfo, alloc, surface, "vkCreateWaylandSurfaceKHR",
            SurfaceOrigin::Wayland);
        // Every native Wayland surface gets the observer (X6 refresh source). diagnostics only decides
        // whether its records are kept; FGVK_PACING_PROFILE_DISPLAY=0 keeps the profile but leaves the display probe out (A/B run).
        const bool diagnostics = present_diagnostics::pacingWaylandProfileEnabled() ||
            present_diagnostics::pacingDisplayProbeEnabled();
        if (result == VK_SUCCESS && createInfo && surface && instance_info) {
            try {
                auto observer = WaylandPresentationFeedback::create(createInfo->display, createInfo->surface, diagnostics);
                if (observer) {
                    const std::lock_guard lock(instance_info->surfaceStateMutex);
                    instance_info->waylandPresentation.insert_or_assign(*surface, std::move(observer));
                }
            } catch (...) {} // Diagnostics cannot change surface creation.
        }
        return result;
    }

    VkResult myvkCreateXcbSurfaceKHR(
            const VkInstance instance,
            const VkXcbSurfaceCreateInfoKHR* createInfo,
            const VkAllocationCallbacks* alloc,
            VkSurfaceKHR* surface) {
        return createSurfaceAndRecord<
            CreateXcbSurface
        >(instance, createInfo, alloc, surface, "vkCreateXcbSurfaceKHR",
            SurfaceOrigin::Xcb, x11WindowOf<XcbSurfaceCreateInfoLayout>(createInfo));
    }

    VkResult myvkCreateXlibSurfaceKHR(
            const VkInstance instance,
            const VkXlibSurfaceCreateInfoKHR* createInfo,
            const VkAllocationCallbacks* alloc,
            VkSurfaceKHR* surface) {
        return createSurfaceAndRecord<
            CreateXlibSurface
        >(instance, createInfo, alloc, surface, "vkCreateXlibSurfaceKHR",
            SurfaceOrigin::Xlib, x11WindowOf<XlibSurfaceCreateInfoLayout>(createInfo));
    }

    InternalSwapchainImages* internalImagesFor(const VkSwapchainKHR swapchain) {
        const auto found = instance_info->internalSwapchains.find(swapchain);
        return found == instance_info->internalSwapchains.end()
            ? nullptr : found->second.get();
    }

    // F5 gate and creation for one context-backed swapchain. A swapchain
    // without internal images presents real frames only. The decision is
    // logged once for each swapchain created with a Frame Generation context.
    void installInternalSwapchainImages(vk::Vulkan& vk,
            const VkSwapchainCreateInfoKHR& application,
            const VkSwapchainKHR swapchain, const SwapchainInfo& swapchainInfo) {
        const char* reason = nullptr;
        uint32_t imageCount = 0;
        std::shared_ptr<InternalSwapchainImages> images;
        try {
            auto& context = layer_info->root.getSwapchainContext(swapchain);
            const auto& profile = context.appliedProfile();
            const auto viewFormats = imageFormatListOf(application.pNext);
            const auto support = queryInternalImageSupport(
                vk, application.imageFormat, application.imageUsage,
                internalImageCreateFlags(application.flags), viewFormats);
            const InternalImageGateInput gate{
                .frameGenerationEnabled = profile.frame_generation_enabled,
                .deferredPresentQueue = static_cast<bool>(swapchainInfo.deferredPresentQueue),
                .applicationExtent = swapchainInfo.applicationExtent,
                .lowerExtent = swapchainInfo.extent,
                .applicationFormat = application.imageFormat,
                .lowerFormat = swapchainInfo.format,
                .imageArrayLayers = application.imageArrayLayers,
                .applicationFlags = application.flags,
                .applicationViewFormatCount = static_cast<uint32_t>(viewFormats.size()),
                .applicationMinImageCount = application.minImageCount,
                .lowerImageCount = static_cast<uint32_t>(swapchainInfo.images.size()),
                .formatBlitCapable = support.formatBlitCapable,
                .imageFormatSupported = support.imageFormatSupported,
            };
            reason = internalImagesIneligibleReason(gate);
            if (!reason) {
                imageCount = internalImageCount(application.minImageCount,
                    static_cast<uint32_t>(swapchainInfo.images.size()));
                images = InternalSwapchainImages::create(vk, swapchain, application,
                    swapchainInfo.applicationExtent, swapchainInfo.images,
                    imageCount, swapchainInfo.deferredPresentQueue);
                if (!images) {
                    reason = "allocation-failed";
                }
            }
            if (!images)
                context.setFrameGenerationUnavailable(reason);
        } catch (const std::exception&) {
            images.reset();
            reason = "context-not-created";
        }
        if (images) {
            instance_info->internalSwapchains.emplace(swapchain, images);
            if (!present_layout::registerImages(vk.dev(), images->images())) {
                instance_info->internalSwapchains.erase(swapchain);
                images.reset();
                reason = "layout-registration-failed";
                layer_info->root.getSwapchainContext(swapchain).setFrameGenerationUnavailable(reason);
            }
        }
        std::cerr << "framegen-vulkan: internal swapchain images: active="
                  << (images ? 1 : 0)
                  << " reason=" << (reason ? reason : "eligible")
                  << " images=" << (images ? imageCount : 0)
                  << " lower_images=" << swapchainInfo.images.size()
                  << '\n';
    }

    VkResult myvkCreateSwapchainKHR(
            VkDevice device,
            const VkSwapchainCreateInfoKHR* info,
            const VkAllocationCallbacks* alloc,
            VkSwapchainKHR* swapchain) {
        const auto& it = instance_info->devices.find(device);
        if (it == instance_info->devices.end())
            return VK_ERROR_INITIALIZATION_FAILED;

        VkSwapchainKHR createdSwapchain{VK_NULL_HANDLE};
        bool lowerSwapchainCreated{false};
        const auto rollbackCreatedSwapchain = [&]() noexcept {
            if (!lowerSwapchainCreated || createdSwapchain == VK_NULL_HANDLE)
                return;

            instance_info->swapchains.erase(createdSwapchain);
            instance_info->swapchainInfos.erase(createdSwapchain);
            instance_info->nativeSwapchains.erase(createdSwapchain);
            ReflexPresentAttributionRegistry::erase(createdSwapchain);
            if (auto internal = instance_info->internalSwapchains.extract(createdSwapchain); !internal.empty())
                present_layout::unregisterImages(device, internal.mapped()->images());
            try {
                // GP1: the on-screen watcher is joined before the lower swapchain is destroyed below.
                if (auto* context = layer_info->root.findSwapchainContext(createdSwapchain))
                    context->stopOnScreenProfile();
                layer_info->root.removeSwapchainContext(createdSwapchain);
            } catch (const std::exception& e) {
                std::cerr << "framegen-vulkan: failed to retire a partially-created "
                             "swapchain context: " << e.what() << '\n';
            }
            it->second.df().DestroySwapchainKHR(
                device, createdSwapchain, alloc
            );
            *swapchain = VK_NULL_HANDLE;
            lowerSwapchainCreated = false;
        };

        try {
            // `oldSwapchain` is retired for new acquisition by the lower WSI,
            // but images acquired before retirement may still be presented.
            // Keep framegen-vulkan's old context resources alive until the
            // application explicitly destroys that handle.

            // A live factor change can make the upper role request recreation
            // immediately after it observes a new profile. Force the lower
            // role past the present-path polling interval here so both split
            // layers build the replacement from the same saved revision.
            layer_info->root.update(true);

            // create swapchain
            VkSwapchainCreateInfoKHR newInfo = *info;
            bool ownedPresentTiming = false;
            uint32_t ownedPresentTimingTargetStage{};
            // The application already destroyed this null-old predecessor.
            // Complete its existing fence/grace-protected retirement before
            // the replacement is created. A non-null oldSwapchain context
            // remains application-owned and is not retired here.
            bool retiredNullOldReplacement{false};
            const auto retainedOldSwapchain =
                retainedSwapchainBeforeNullOldReplacement(
                    device,
                    info->surface,
                    info->oldSwapchain
                );
            if (retainedOldSwapchain) {
                if (!finalizeRetiredSwapchain(
                        *retainedOldSwapchain,
                        UINT64_MAX)) {
                    throw ls::vulkan_error(
                        VK_ERROR_INITIALIZATION_FAILED,
                        "retained swapchain retirement failed "
                        "before null-old replacement"
                    );
                }
                retiredNullOldReplacement = true;
            }
            // H3: Wine maps HDR10/PQ and scRGB to PASS_THROUGH on a native Wayland surface.
            const bool winePassThroughHdr = layer_info->root.wineProcess() &&
                surfaceOrigin(info->surface) == SurfaceOrigin::Wayland;
            auto modification =
                layer_info->root.modifySwapchainCreateInfo(
                    it->second, newInfo, winePassThroughHdr,
                    [&, newInfo = &newInfo]() {
                        auto driverInfo = *newInfo;
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
                        // Native surfaces negotiate their own Vulkan timing contract.
                        if (instance_info->presentTimingDevices.contains(device) &&
                                driverInfo.presentMode == VK_PRESENT_MODE_FIFO_KHR) {
                            const auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
                                layer_info->GetInstanceProcAddr(it->second.inst(),
                                    "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
                            VkSurfaceCapabilitiesPresentId2KHR ids{
                                .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR,
                            };
                            VkPresentTimingSurfaceCapabilitiesEXT timing{
                                .sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT,
                                .pNext = &ids,
                            };
                            VkSurfaceCapabilities2KHR caps{
                                .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
                                .pNext = &timing,
                            };
                            const VkPhysicalDeviceSurfaceInfo2KHR surface{
                                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
                                .surface = driverInfo.surface,
                            };
                            ownedPresentTiming = query &&
                                query(it->second.physdev(), &surface, &caps) == VK_SUCCESS &&
                                OwnedPresentTiming::surfaceSupported(timing, ids);
                            if (ownedPresentTiming)
                                ownedPresentTimingTargetStage =
                                    OwnedPresentTiming::preferredTargetStage(timing.presentStageQueries);
                            if (ownedPresentTiming)
                                driverInfo.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT |
                                    VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
                        }
#endif
                        const uint32_t provisionedMinImages =
                            newInfo->minImageCount;
                        auto res = it->second.df().CreateSwapchainKHR(
                            device, &driverInfo, alloc, swapchain);
                        present_diagnostics::traceVulkanSetup(layerRoleName, *info, driverInfo,
                            res, res == VK_SUCCESS ? *swapchain : VK_NULL_HANDLE, device);
                        if (shouldRetrySwapchainWithApplicationMinimum(
                                res, info->minImageCount,
                                provisionedMinImages)) {
                            VkSwapchainCreateInfoKHR retryInfo = driverInfo;
                            retryInfo.minImageCount = info->minImageCount;
                            *swapchain = VK_NULL_HANDLE;
                            res = it->second.df().CreateSwapchainKHR(
                                device, &retryInfo, alloc, swapchain
                            );
                            present_diagnostics::traceVulkanSetup(layerRoleName, *info, retryInfo,
                                res, res == VK_SUCCESS ? *swapchain : VK_NULL_HANDLE, device);
                            if (res == VK_SUCCESS)
                                newInfo->minImageCount = retryInfo.minImageCount;
                        }
                        if (res != VK_SUCCESS) {
                            throw ls::vulkan_error(
                                res, "vkCreateSwapchainKHR() failed"
                            );
                        }
                        createdSwapchain = *swapchain;
                        lowerSwapchainCreated = true;
                    }
                );

            // get all swapchain images
            uint32_t imageCount{};
            auto res = it->second.df().GetSwapchainImagesKHR(device, *swapchain,
                &imageCount, VK_NULL_HANDLE);
            if (res != VK_SUCCESS)
                throw ls::vulkan_error(res, "vkGetSwapchainImagesKHR() failed");
            if (imageCount == 0)
                throw ls::vulkan_error(
                    VK_ERROR_INITIALIZATION_FAILED,
                    "vkGetSwapchainImagesKHR() returned no images"
                );

            std::vector<VkImage> swapchainImages(imageCount);
            res = it->second.df().GetSwapchainImagesKHR(device, *swapchain,
                &imageCount, swapchainImages.data());
            if (res != VK_SUCCESS)
                throw ls::vulkan_error(res, "vkGetSwapchainImagesKHR() failed");

            std::shared_ptr<WaylandPresentationFeedback> waylandPresentation;
            {
                const std::lock_guard lock(instance_info->surfaceStateMutex);
                const auto observer = instance_info->waylandPresentation.find(info->surface);
                if (observer != instance_info->waylandPresentation.end()) waylandPresentation = observer->second;
            }
            auto& swapchainInfo = instance_info->swapchainInfos.emplace(*swapchain, SwapchainInfo {
                .images = std::move(swapchainImages),
                .surface = info->surface,
                .surfaceOrigin = surfaceOrigin(info->surface),
                .x11Window = surfaceX11Window(info->surface),
                .winePassThroughHdr = winePassThroughHdr,
                .waylandPresentation = std::move(waylandPresentation),
                .deferredPresentQueue = instance_info->deferredPresentQueues.contains(device)
                    ? instance_info->deferredPresentQueues.at(device) : nullptr,
                .ownedPresentTiming = ownedPresentTiming,
                .presentMetering = instance_info->presentMeteringDevices.contains(device),
                .applicationPresentTiming = instance_info->applicationTimingDevices.contains(device),
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
                // The forwarded create flags are driverInfo inside the create
                // lambda: framegen-vulkan's owned timing bit (ownedPresentTiming) or the
                // application's own bit in newInfo.
                .applicationSwapchainTiming = instance_info->applicationTimingDevices.contains(device) &&
                    (ownedPresentTiming || (newInfo.flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT) != 0),
#if defined(VK_KHR_present_wait2)
                .applicationPresentFeedback = instance_info->presentFeedbackDevices.contains(device) &&
                    (newInfo.flags & VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR) != 0 &&
                    (newInfo.flags & VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR) != 0,
#endif
#endif
                .copyTimestampCalibration = instance_info->copyCalibrationDevices.contains(device),
                .ownedPresentTimingTargetStage = ownedPresentTimingTargetStage,
                .format = newInfo.imageFormat,
                .colorSpace = newInfo.imageColorSpace,
                .requestedMinImageCount = info->minImageCount,
                .provisionedMinImageCount = newInfo.minImageCount,
                .surfaceMinImageCount = modification.surfaceMinImageCount,
                .swapchainImageCountCompatibility =
                    modification.swapchainImageCountCompatibility,
                .applicationOldSwapchainProvided =
                    info->oldSwapchain != VK_NULL_HANDLE,
                .applicationExtent = modification.applicationExtent,
                .extent = newInfo.imageExtent,
                .incomingPresentMode = info->presentMode,
                .presentMode = newInfo.presentMode,
                .privateOrderedTransport =
                    modification.privateOrderedTransport,
                .replacement = swapchainCreateIsReplacement(
                    newInfo.oldSwapchain, retiredNullOldReplacement
                ),
            }).first->second;

            // An enabled implicit layer can run in a process that does not
            // match a configured game profile (launchers and helper processes
            // are common examples). Keep that process on its original Vulkan
            // presentation path without attempting to construct framegen-vulkan's
            // optional backend.
            if (!layer_info->root.active()) {
                instance_info->swapchains.emplace(
                    *swapchain, ls::R<vk::Vulkan>(it->second)
                );
                instance_info->nativeSwapchains.insert(*swapchain);
                lowerSwapchainCreated = false;
                return res;
            }

            // Creating interpolation resources is optional. If the game's
            // lower swapchain is valid but framegen-vulkan's private backend cannot start
            // (for example an unavailable DLL or unsupported private-device
            // capability), retain the real-frame presentation path instead of
            // turning an engine failure into a game startup failure.
            try {
                layer_info->root.createSwapchainContext(
                    it->second, *swapchain, swapchainInfo,
                    instance_info->presentRetirementDevices.contains(device)
                );
            } catch (const std::exception& e) {
                instance_info->swapchains.emplace(
                    *swapchain, ls::R<vk::Vulkan>(it->second)
                );
                instance_info->nativeSwapchains.insert(*swapchain);
                lowerSwapchainCreated = false;
                std::cerr << "framegen-vulkan: frame-generation context initialization "
                             "failed; native presentation retained:\n"
                          << "- " << e.what() << '\n';
                return res;
            }

#if defined(VK_KHR_present_wait2) && defined(VK_KHR_present_id2)
            // OS1: the screen source of the automatic correction is chosen from the gate's result below.
            bool screenWatcher{};
            // GT1: the gate's session is a gamescope session (the tick lock's session input).
            bool screenGamescope{};
            const char* screenGateReason{"headers"};
            // GP1: the gamescope on-screen gate, decided once per lower swapchain from the application's own
            // create flags. Owned present timing already adds a VkPresentId2KHR node of its own, so that
            // case stays unavailable until its ID is owned too (see the GP1 report).
            {
                const auto nextWaitForPresent2 = reinterpret_cast<PFN_vkWaitForPresent2KHR>(
                    instance_info->funcs.GetDeviceProcAddr(device, "vkWaitForPresent2KHR"));
                const bool profiling = present_diagnostics::pacingProfileEnabled();
                // Keep the lower present ID mapping stable if correction is enabled after creation.
                const bool correction = realOffsetMayEnable() && onScreenCorrectionEnvironment();
                // The session kind: a gamescope session first, else an X11 surface (Xwayland or plain X11).
                // Native Wayland surfaces outside gamescope have no kind and never take the watcher.
                const auto origin = surfaceOrigin(info->surface);
                const OnScreenKind kind = layer_info->root.gamescopeSession() ? OnScreenKind::Gamescope
                    : origin == SurfaceOrigin::Xlib || origin == SurfaceOrigin::Xcb ? OnScreenKind::X11
                    : OnScreenKind::None;
                // The gate's enable: the profile (records) or the correction (OS1).
                auto gate = onScreenGate(profiling || correction, kind,
                    (newInfo.flags & VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR) != 0,
                    (newInfo.flags & VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR) != 0,
                    nextWaitForPresent2 != nullptr);
                if (gate.active && ownedPresentTiming) gate = {false, "owned-present-timing", kind};
                if (gate.active) {
                    try {
                        if (auto* context = layer_info->root.findSwapchainContext(*swapchain))
                            context->startOnScreenProfile(device, *swapchain, nextWaitForPresent2);
                    } catch (const std::exception& e) {
                        gate = {false, "watcher-thread", kind};
                        std::cerr << "framegen-vulkan: gamescope on-screen watcher could not start: "
                                  << e.what() << '\n';
                    }
                }
                if (profiling) {
                    std::clog << "framegen-vulkan: gamescope on-screen profile: state=";
                    if (gate.active)
                        std::clog << "active";
                    else
                        std::clog << "unavailable reason=" << gate.reason;
                    if (kind != OnScreenKind::None)
                        std::clog << " kind=" << onScreenKindName(kind);
                    std::clog << '\n';
                }
                screenWatcher = gate.active;
                screenGateReason = gate.reason;
                screenGamescope = kind == OnScreenKind::Gamescope;
            }
#endif
            if (auto* context = layer_info->root.findSwapchainContext(*swapchain))
                context->configureScreenCorrection(screenWatcher, screenGateReason, screenGamescope);
            installInternalSwapchainImages(it->second, *info, *swapchain,
                swapchainInfo);

            // RA1: the Reflex attribution entry of this managed swapchain (native paths have none).
            static_cast<void>(ReflexPresentAttributionRegistry::get(*swapchain));
            instance_info->swapchains.emplace(*swapchain,
                ls::R<vk::Vulkan>(it->second));

            lowerSwapchainCreated = false;
            return res;
        } catch (const ls::vulkan_error& e) {
            rollbackCreatedSwapchain();
            std::cerr << "framegen-vulkan: swapchain creation failed:\n";
            std::cerr << "- " << e.what() << '\n';
            return e.error();
        } catch (const std::exception& e) {
            rollbackCreatedSwapchain();
            std::cerr << "framegen-vulkan: swapchain creation failed:\n";
            std::cerr << "- " << e.what() << '\n';
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    // Defined after the acquire helpers, which it shares with the lower path.
    VkResult presentInternalSwapchainImage(Swapchain& context, vk::Vulkan& vk,
        VkQueue queue, VkSwapchainKHR swapchain,
        const std::shared_ptr<InternalSwapchainImages>& internal,
        const VkPresentInfoKHR& presentInfo, uint32_t presentIndex);

    VkResult myvkQueuePresentKHRBody(VkQueue queue, const VkPresentInfoKHR* info);
    VkResult myvkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
        present_diagnostics::LayerCallSpan span(present_diagnostics::LayerCall::QueuePresent);
        return span.returned(myvkQueuePresentKHRBody(queue, info));
    }
    VkResult myvkQueuePresentKHRBody(VkQueue queue, const VkPresentInfoKHR* info) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
        // D7: the stall probe's present clock. Only the pacing profile stores it.
        present_diagnostics::noteApplicationPresent();
        present_diagnostics::ApplicationPresentScope timingScope(
            queue, info->swapchainCount == 1 ? info->pSwapchains[0] : VK_NULL_HANDLE,
            info->swapchainCount == 1 ? std::optional<uint32_t>{info->pImageIndices[0]} : std::nullopt
        );
        const auto present = [&]() -> VkResult {
        VkResult result = VK_SUCCESS;
        bool swapchainOutOfDate = false;
        // Configuration polling can replace private resources across surfaces.
        // Complete retained CPU submissions before any such transition.
        const auto queueIdentity = instance_info->queueIdentities.find(queue);
        if (queueIdentity != instance_info->queueIdentities.end()) {
            timingScope.deferredJoinStarted();
            drainDeferredDevice(queueIdentity->second.device, true);
            timingScope.deferredJoinFinished();
        }

        // Binary application wait semaphores belong to the whole present
        // batch and may be consumed only once. The existing per-swapchain FG
        // transport cannot yet preserve that contract for any managed framegen-vulkan
        // context, so reject before submitting any work rather than wait
        // twice, deadlock, or misassociate per-swapchain pNext arrays.
        if (info->swapchainCount > 1) {
            bool containsManagedFgvkContext = false;
            for (uint32_t i = 0; i < info->swapchainCount; ++i) {
                const auto metadata = instance_info->swapchainInfos.find(
                    info->pSwapchains[i]
                );
                containsManagedFgvkContext = containsManagedFgvkContext ||
                    (metadata != instance_info->swapchainInfos.end() &&
                     !instance_info->nativeSwapchains.contains(
                        info->pSwapchains[i]
                     ));
            }
            if (shouldRejectManagedMultiSwapchainPresent(
                    info->swapchainCount, containsManagedFgvkContext)) {
                // X3: nothing in this batch is presented, so each application present fence is
                // signalled here, through its swapchain's device. A swapchain the layer does not
                // know has no device to submit on.
                for (uint32_t i = 0; i < info->swapchainCount; ++i) {
                    const auto mapping = instance_info->swapchains.find(info->pSwapchains[i]);
                    if (mapping != instance_info->swapchains.end())
                        static_cast<void>(signalUnforwardedPresentFence(
                            mapping->second.get().df().QueueSubmit, queue,
                            applicationPresentFence(*info, i)));
                }
                constexpr VkResult unsupported =
                    VK_ERROR_UNKNOWN;
                if (info->pResults) {
                    std::fill_n(
                        info->pResults, info->swapchainCount, unsupported
                    );
                }
                std::cerr << "framegen-vulkan: multi-swapchain managed present "
                             "rejected before semaphore consumption; batch "
                             "frame generation and scaling are not supported\n";
                return unsupported;
            }
        }

        // A context that failed after the lower swapchain was created remains
        // a normal application swapchain. Bypass framegen-vulkan for the whole batch so
        // the driver's wait-semaphore and per-swapchain result semantics stay
        // exactly as the application supplied them.
        for (size_t i = 0; i < info->swapchainCount; ++i) {
            const auto swapchain = info->pSwapchains[i];
            if (!instance_info->nativeSwapchains.contains(swapchain))
                continue;

            const auto mapping = instance_info->swapchains.find(swapchain);
            if (mapping == instance_info->swapchains.end()) {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            // A native present takes no release token: drop the submit mark a later present would take.
            ReleaseGate::forgetSubmitMark(queue);
            const VkResult nativeResult =
                mapping->second.get().df().QueuePresentKHR(queue, info);
            // A prior managed swapchain may still be retiring on this surface.
            // Preserve native forwarding, then check its completion without waiting.
            collectRetiredSwapchainsAfterPresent(queue, *info);
            return nativeResult;
        }

        // ensure layer config is up to date
        ConfigurationUpdateResult configurationUpdate;
        timingScope.configurationUpdateStarted();
        try {
            configurationUpdate = layer_info->root.update();
        } catch (const std::exception&) {
            configurationUpdate = {}; // retain the last valid configuration
        }
        timingScope.configurationUpdateFinished();

        if (configurationUpdate.reloaded) {
            std::cerr << "framegen-vulkan: updated configuration in place; contexts="
                      << configurationUpdate.liveContextsUpdated << '\n';
            if (configurationUpdate.swapchainRecreationDeferredContexts > 0)
                std::cerr << "framegen-vulkan: configuration changes requiring GPU resource "
                             "reconstruction remain pending until a game-owned "
                             "swapchain recreation; contexts="
                          << configurationUpdate.swapchainRecreationDeferredContexts
                          << '\n';
            if (configurationUpdate.recreationRequestedContexts > 0)
                std::cerr << "framegen-vulkan: live profile-resource changes will request "
                             "game-owned swapchain recreation after the current lower "
                             "present accepts a maintenance1 retirement fence; contexts="
                          << configurationUpdate.recreationRequestedContexts
                          << '\n';
            if (configurationUpdate.processRestartDeferredContexts > 0 ||
                    configurationUpdate.processProfileChangeDeferred ||
                    configurationUpdate.globalChangeDeferred) {
                std::cerr << "framegen-vulkan: process-static backend configuration remains "
                             "pending until the game restarts; contexts="
                          << configurationUpdate.processRestartDeferredContexts
                          << "; profile="
                          << configurationUpdate.processProfileChangeDeferred
                          << "; global="
                          << configurationUpdate.globalChangeDeferred << '\n';
            }
        }

        // present each swapchain
        for (size_t i = 0; i < info->swapchainCount; i++) {
            const auto& swapchain = info->pSwapchains[i];

            const auto& it = instance_info->swapchains.find(swapchain);
            if (it == instance_info->swapchains.end()) {
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            std::optional<uint64_t> presentingContextId;
            // X3: the swapchain's context once it is known, and whether this swapchain took the
            // internal path, which signals its own application fence (presentInternalSwapchainImage).
            Swapchain* presentingContext = nullptr;
            bool internalPresent = false;
            try {
                const auto waitSemaphores = info->waitSemaphoreCount > 0
                    ? std::span<const VkSemaphore>(
                        info->pWaitSemaphores, info->waitSemaphoreCount
                    )
                    : std::span<const VkSemaphore>{};

                auto& context = layer_info->root.getSwapchainContext(swapchain);
                presentingContext = &context;
                presentingContextId = context.diagnosticsId();
                if (const auto found = instance_info->internalSwapchains.find(swapchain);
                        found != instance_info->internalSwapchains.end()) {
                    // A copy keeps the images alive for this whole present call.
                    const auto internal = found->second;
                    internalPresent = true;
                    result = presentInternalSwapchainImage(context, it->second,
                        queue, swapchain, internal, *info, static_cast<uint32_t>(i));
                } else {
                    result = context.present(it->second,
                        queue, swapchain,
                        const_cast<void*>(info->pNext),
                        info->pImageIndices[i],
                        waitSemaphores
                    );
                }
                const bool liveProfileRecreationRequested =
                    context.requestLiveProfileResourceRecreationAfterPresent(
                        result
                    );
                const bool orderedAcquireRecoveryRecreationRequested =
                    context.requestOrderedAcquireRecreationAfterPresent(
                        result,
                        !liveProfileRecreationRequested
                    );
                if (liveProfileRecreationRequested ||
                        orderedAcquireRecoveryRecreationRequested) {
                    // This role owns the guarded request only after
                    // Swapchain::present() returned from a successful lower
                    // present. The application's acquired real image has
                    // therefore already reached the presentation engine; make
                    // that proof explicit before replacing the successful
                    // result with the one-shot recreation signal.
                    result = VK_ERROR_OUT_OF_DATE_KHR;
                }
            } catch (const ls::vulkan_error& e) {
                if (e.error() != VK_ERROR_OUT_OF_DATE_KHR) {
                    std::cerr << "framegen-vulkan: swapchain presentation failed:\n";
                    std::cerr << "- " << e.what() << '\n';
                }

                result = e.error();
            } catch (const std::exception& e) {
                std::cerr << "framegen-vulkan: swapchain presentation failed:\n";
                std::cerr << "- " << e.what() << '\n';
                result = VK_ERROR_UNKNOWN;
            }

            // X3: a present whose application chain never reached a lower present or the deferred
            // worker (a throw or early return inside Swapchain::present, or no context at all) signals
            // the application's present fence here, once. A lower present that received it never does.
            if (!internalPresent && (!presentingContext || !presentingContext->applicationChainWasHandedOff()))
                static_cast<void>(signalUnforwardedPresentFence(it->second.get().df().QueueSubmit, queue,
                    applicationPresentFence(*info, static_cast<uint32_t>(i))));

            recordPresentResult(*info, i, result);

            if (result == VK_ERROR_OUT_OF_DATE_KHR)
                swapchainOutOfDate = true;
        }

        collectRetiredSwapchainsAfterPresent(queue, *info);

        // Preserve a genuine game/driver out-of-date result, or framegen-vulkan's guarded
        // one-shot live-scaling recreation request, across the present batch.
        return swapchainOutOfDate ? VK_ERROR_OUT_OF_DATE_KHR : result;
        };
        return timingScope.returned(present());
#pragma clang diagnostic pop
    }

    // observeApplication is false when the layer itself performs the lower
    // acquire on the present path, so the deferred join is not counted as an
    // application acquire.
    template<typename Acquire>
    VkResult acquireAfterDeferredPresent(VkSwapchainKHR swapchain,
            uint64_t timeout, Acquire&& acquire, bool observeApplication = true) {
        const auto mapping = instance_info->swapchains.find(swapchain);
        if (mapping == instance_info->swapchains.end() ||
                instance_info->nativeSwapchains.contains(swapchain)) return acquire(timeout);
        auto& context = layer_info->root.getSwapchainContext(swapchain);
        present_diagnostics::bindApplicationAcquireContext(context.diagnosticsId());
        if (!context.hasDeferredPresent()) return acquire(timeout);
        if (observeApplication) context.observeDeferredApplicationAcquire();
        const auto started = std::chrono::steady_clock::now();
        const auto completed = context.completeDeferredPresent(0);
        if (completed != VK_SUCCESS && completed != VK_SUBOPTIMAL_KHR && completed != VK_NOT_READY) return completed;
        // An older finished completion can return success while a newer overlapped
        // batch keeps outputting. Its spare image is still acquirable without a join.
        if (completed == VK_NOT_READY || context.deferredPresentPending()) {
            const auto polled = context.pollDeferredAcquire([&] { return acquire(0); });
            if (polled != VK_NOT_READY && polled != VK_TIMEOUT)
                return polled == VK_SUCCESS && completed == VK_SUBOPTIMAL_KHR ? VK_SUBOPTIMAL_KHR : polled;
        }
        if (context.deferredOverlapPending()) {
            auto remaining = timeout;
            if (remaining != UINT64_MAX) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started).count();
                remaining -= std::min(remaining, static_cast<uint64_t>(std::max<int64_t>(0, elapsed)));
            }
            const auto result = context.waitDeferredAcquire(remaining, [&] { return acquire(0); });
            return result == VK_SUCCESS && completed == VK_SUBOPTIMAL_KHR ? VK_SUBOPTIMAL_KHR : result;
        }
        const auto deferred = context.completeDeferredPresent(timeout, true);
        if (deferred != VK_SUCCESS && deferred != VK_SUBOPTIMAL_KHR) return deferred;
        if (timeout != UINT64_MAX) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started).count();
            timeout -= std::min(timeout, static_cast<uint64_t>(std::max<int64_t>(0, elapsed)));
        }
        const auto result = acquire(timeout);
        return result == VK_SUCCESS && deferred == VK_SUBOPTIMAL_KHR ? deferred : result;
    }

    // Emulated acquire for an F5 swapchain. The Frame Generation deferred
    // join runs as it does for the lower path. The lower driver is not called,
    // so the application's image comes only from the private ring.
    VkResult acquireInternalSwapchainImage(VkSwapchainKHR swapchain,
            InternalSwapchainImages& internal, uint64_t timeout,
            VkSemaphore semaphore, VkFence fence, uint32_t* imageIndex) {
        const auto mapping = instance_info->swapchains.find(swapchain);
        if (mapping == instance_info->swapchains.end())
            return VK_ERROR_INITIALIZATION_FAILED;
        auto& context = layer_info->root.getSwapchainContext(swapchain);
        present_diagnostics::bindApplicationAcquireContext(context.diagnosticsId());
        context.observeDeferredApplicationAcquire();
        const auto completed = context.completeDeferredPresent(0);
        if (completed != VK_SUCCESS && completed != VK_SUBOPTIMAL_KHR &&
                completed != VK_NOT_READY) {
            return completed;
        }
        return internal.acquireApplicationImage(timeout, semaphore, fence,
            completed == VK_SUBOPTIMAL_KHR, imageIndex);
    }

    // The application presents a private image. The real output's lower acquire,
    // copy and present happen on the deferred worker at the output deadline, or
    // synchronously when the present is not deferred. The descriptor carries
    // what either path needs. A copy that was never handed off releases the
    // image here, and the swapchain turns OUT_OF_DATE so the game recreates it.
    VkResult presentInternalSwapchainImage(Swapchain& context, vk::Vulkan& vk,
            const VkQueue queue, const VkSwapchainKHR swapchain,
            const std::shared_ptr<InternalSwapchainImages>& internal,
            const VkPresentInfoKHR& presentInfo, const uint32_t presentIndex) {
        InternalSwapchainImages& images = *internal;
        const uint32_t applicationIndex = presentInfo.pImageIndices[presentIndex];
        const auto applicationWaits = presentInfo.waitSemaphoreCount > 0
            ? std::span<const VkSemaphore>(
                presentInfo.pWaitSemaphores, presentInfo.waitSemaphoreCount)
            : std::span<const VkSemaphore>{};
        // X3: every return below that issues no lower present with the application chain signals the
        // application's present fence for this image once: on the application queue, except the sticky
        // return, which signals it on the private queue after the consumed waits. Only those
        // returns look the fence up, so the normal path does not walk the pNext chain.
        const auto signalUnforwardedFence = [&] {
            static_cast<void>(signalUnforwardedPresentFence(vk.df().QueueSubmit, queue,
                applicationPresentFence(presentInfo, presentIndex)));
        };
        if (!images.isAcquired(applicationIndex)) {
            signalUnforwardedFence();
            return VK_ERROR_VALIDATION_FAILED_EXT;
        }
        if (const VkResult sticky = images.stickyError(); sticky != VK_SUCCESS) {
            // X3: the waits are consumed on the private queue, so the fence follows them there.
            static_cast<void>(images.consumeWithoutCopy(applicationIndex, applicationWaits,
                applicationPresentFence(presentInfo, presentIndex)));
            return sticky;
        }

        images.markPending(applicationIndex);
        InternalPresentImage descriptor{
            .applicationIndex = applicationIndex,
            .image = images.images()[applicationIndex],
            .realSlot = images.lowerImageCount() + applicationIndex,
            .owner = &images,
            .keepAlive = internal,
            .lowerAcquire = [&](const uint64_t timeout, const VkSemaphore semaphore,
                    uint32_t* const index) {
                return acquireAfterDeferredPresent(swapchain, timeout, [&](uint64_t remaining) {
                    // A poll (timeout 0) reports NOT_READY while the slot is busy.
                    const VkResult waited = images.waitLowerAcquireSlot(remaining);
                    if (waited != VK_SUCCESS) return waited;
                    const VkResult lower = vk.df().AcquireNextImageKHR(vk.dev(), swapchain,
                        remaining, semaphore, VK_NULL_HANDLE, index);
                    if (lower == VK_SUCCESS || lower == VK_SUBOPTIMAL_KHR)
                        static_cast<void>(images.commitLowerAcquire());
                    return lower;
                }, false);
            },
        };

        VkResult result = VK_SUCCESS;
        try {
            result = context.present(vk, queue, swapchain,
                const_cast<void*>(presentInfo.pNext), applicationIndex,
                applicationWaits, &descriptor);
        } catch (...) {
            if (!descriptor.handedOff) {
                images.releaseNotHandedOff(queue, applicationIndex);
                signalUnforwardedFence();
            }
            throw;
        }
        // handedOff is the complete record: the lower present with the application chain (or the
        // worker's job for it) exists exactly when it is set. See Swapchain::queuePresentWithRetirementFence.
        if (!descriptor.handedOff) {
            images.releaseNotHandedOff(queue, applicationIndex);
            signalUnforwardedFence();
            return result < 0 ? result : VK_ERROR_OUT_OF_DATE_KHR;
        }
        return result;
    }

    // Native acquire fallback (low_latency; swapchain/low_latency.hpp): an application that
    // waits on no release token starts its next frame at the swapchain's last release point,
    // so its acquire waits for that point. Returns VK_TIMEOUT when the point is past the
    // caller's timeout. Otherwise the timeout is lowered by the time held.
    std::optional<VkResult> holdAcquireForRelease(const VkSwapchainKHR swapchain, uint64_t& timeout) {
        const auto mapping = instance_info->swapchains.find(swapchain);
        if (mapping == instance_info->swapchains.end() ||
                instance_info->nativeSwapchains.contains(swapchain))
            return std::nullopt;
        auto* found = layer_info->root.findSwapchainContext(swapchain);
        if (!found) return std::nullopt;
        auto& context = *found;
        using Clock = ReleaseGate::Clock;
        const auto now = Clock::now();
        const auto release = context.acquireRelease(now);
        if (!release) return std::nullopt;
        const auto hold = acquireHold(now, *release, timeout);
        if (hold.wait > Clock::duration::zero()) {
            context.noteAcquireHold();
            std::this_thread::sleep_for(hold.wait);
        }
        if (hold.expired) return VK_TIMEOUT;
        context.noteAcquireReleased(Clock::now());
        if (timeout != UINT64_MAX) timeout -= static_cast<uint64_t>(hold.wait.count());
        return std::nullopt;
    }

    VkResult myvkAcquireNextImageKHRBody(
            VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
            VkSemaphore semaphore, VkFence fence, uint32_t* imageIndex);
    VkResult myvkAcquireNextImageKHR(
            VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
            VkSemaphore semaphore, VkFence fence, uint32_t* imageIndex) {
        present_diagnostics::LayerCallSpan span(present_diagnostics::LayerCall::AcquireNextImage);
        return span.returned(myvkAcquireNextImageKHRBody(device, swapchain, timeout, semaphore, fence, imageIndex));
    }
    VkResult myvkAcquireNextImageKHRBody(
            VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
            VkSemaphore semaphore, VkFence fence, uint32_t* imageIndex) {
        const auto mapping = instance_info->swapchains.find(swapchain);
        if (mapping == instance_info->swapchains.end())
            return VK_ERROR_INITIALIZATION_FAILED;
        return present_diagnostics::observeApplicationAcquire(
            device, swapchain, present_diagnostics::ApplicationFrameApi::AcquireKhr,
            timeout, [&] {
                auto timeoutLeft = timeout;
                if (const auto held = holdAcquireForRelease(swapchain, timeoutLeft)) return *held;
                if (auto* internal = internalImagesFor(swapchain)) {
                    return acquireInternalSwapchainImage(swapchain, *internal, timeoutLeft,
                        semaphore, fence, imageIndex);
                }
                return acquireAfterDeferredPresent(swapchain, timeoutLeft, [&](uint64_t remaining) {
                    return present_diagnostics::observeApplicationAcquireDriver(remaining, [&] {
                        return mapping->second.get().df().AcquireNextImageKHR(
                            device, swapchain, remaining, semaphore, fence, imageIndex);
                    }, imageIndex);
                });
            }, imageIndex);
    }

    VkResult myvkAcquireNextImage2KHRBody(VkDevice device,
            const VkAcquireNextImageInfoKHR* info, uint32_t* imageIndex);
    VkResult myvkAcquireNextImage2KHR(VkDevice device,
            const VkAcquireNextImageInfoKHR* info, uint32_t* imageIndex) {
        present_diagnostics::LayerCallSpan span(present_diagnostics::LayerCall::AcquireNextImage2);
        return span.returned(myvkAcquireNextImage2KHRBody(device, info, imageIndex));
    }
    VkResult myvkAcquireNextImage2KHRBody(VkDevice device,
            const VkAcquireNextImageInfoKHR* info, uint32_t* imageIndex) {
        if (!instance_info || !info) return VK_ERROR_INITIALIZATION_FAILED;
        const auto mapping = instance_info->swapchains.find(info->swapchain);
        if (mapping == instance_info->swapchains.end()) return VK_ERROR_INITIALIZATION_FAILED;
        const auto lower = mapping->second.get().df().AcquireNextImage2KHR;
        if (!lower) return VK_ERROR_EXTENSION_NOT_PRESENT;
        return present_diagnostics::observeApplicationAcquire(
            device, info->swapchain, present_diagnostics::ApplicationFrameApi::AcquireKhr2,
            info->timeout, [&] {
                auto timeoutLeft = info->timeout;
                if (const auto held = holdAcquireForRelease(info->swapchain, timeoutLeft)) return *held;
                if (auto* internal = internalImagesFor(info->swapchain)) {
                    return acquireInternalSwapchainImage(info->swapchain, *internal,
                        timeoutLeft, info->semaphore, info->fence, imageIndex);
                }
                return acquireAfterDeferredPresent(info->swapchain, timeoutLeft, [&](uint64_t remaining) {
                    auto adjusted = *info;
                    adjusted.timeout = remaining;
                    return present_diagnostics::observeApplicationAcquireDriver(remaining,
                        [&] { return lower(device, &adjusted, imageIndex); }, imageIndex);
                });
            }, imageIndex);
    }

    // Lower result-count semantics for the private image set: a null array
    // reports the count; a short array receives a prefix and VK_INCOMPLETE.
    VkResult reportInternalSwapchainImages(std::span<const VkImage> source,
            uint32_t* count, VkImage* images) {
        if (!count) return VK_ERROR_INITIALIZATION_FAILED;
        if (!images) {
            *count = static_cast<uint32_t>(source.size());
            return VK_SUCCESS;
        }
        const size_t copied = std::min<size_t>(*count, source.size());
        std::copy_n(source.begin(), copied, images);
        *count = static_cast<uint32_t>(copied);
        return copied < source.size() ? VK_INCOMPLETE : VK_SUCCESS;
    }

    VkResult myvkGetSwapchainImagesKHRBody(VkDevice device, VkSwapchainKHR swapchain,
            uint32_t* count, VkImage* images);
    VkResult myvkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
            uint32_t* count, VkImage* images) {
        present_diagnostics::LayerCallSpan span(present_diagnostics::LayerCall::GetSwapchainImages);
        return span.returned(myvkGetSwapchainImagesKHRBody(device, swapchain, count, images));
    }
    VkResult myvkGetSwapchainImagesKHRBody(VkDevice device, VkSwapchainKHR swapchain,
            uint32_t* count, VkImage* images) {
        if (!instance_info) return VK_ERROR_INITIALIZATION_FAILED;
        if (const auto* internal = internalImagesFor(swapchain))
            return reportInternalSwapchainImages(internal->images(), count, images);
        const auto lower = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
            instance_info->funcs.GetDeviceProcAddr(device, "vkGetSwapchainImagesKHR"));
        if (!lower) return VK_ERROR_EXTENSION_NOT_PRESENT;
        return lower(device, swapchain, count, images);
    }

#if defined(VK_KHR_swapchain_maintenance1) || defined(VK_EXT_swapchain_maintenance1)
    // Shared by both spellings, which use the same info struct layout.
    VkResult releaseSwapchainImagesCommon(VkDevice device,
            const VkReleaseSwapchainImagesInfoKHR* info, const char* name) {
        if (!instance_info || !info) return VK_ERROR_INITIALIZATION_FAILED;
        if (auto* internal = internalImagesFor(info->swapchain)) {
            return internal->releaseApplicationImages(std::span<const uint32_t>(
                info->pImageIndices, info->imageIndexCount));
        }
        const auto lower = reinterpret_cast<PFN_vkReleaseSwapchainImagesKHR>(
            instance_info->funcs.GetDeviceProcAddr(device, name));
        if (!lower) return VK_ERROR_EXTENSION_NOT_PRESENT;
        return lower(device, info);
    }
#endif

#if defined(VK_KHR_swapchain_maintenance1)
    VkResult myvkReleaseSwapchainImagesKHRBody(VkDevice device,
            const VkReleaseSwapchainImagesInfoKHR* info);
    VkResult myvkReleaseSwapchainImagesKHR(VkDevice device,
            const VkReleaseSwapchainImagesInfoKHR* info) {
        present_diagnostics::LayerCallSpan span(present_diagnostics::LayerCall::ReleaseSwapchainImages);
        return span.returned(myvkReleaseSwapchainImagesKHRBody(device, info));
    }
    VkResult myvkReleaseSwapchainImagesKHRBody(VkDevice device,
            const VkReleaseSwapchainImagesInfoKHR* info) {
        return releaseSwapchainImagesCommon(device, info, "vkReleaseSwapchainImagesKHR");
    }
#endif

#if defined(VK_EXT_swapchain_maintenance1)
    VkResult myvkReleaseSwapchainImagesEXTBody(VkDevice device,
            const VkReleaseSwapchainImagesInfoEXT* info);
    VkResult myvkReleaseSwapchainImagesEXT(VkDevice device,
            const VkReleaseSwapchainImagesInfoEXT* info) {
        present_diagnostics::LayerCallSpan span(present_diagnostics::LayerCall::ReleaseSwapchainImages);
        return span.returned(myvkReleaseSwapchainImagesEXTBody(device, info));
    }
    VkResult myvkReleaseSwapchainImagesEXTBody(VkDevice device,
            const VkReleaseSwapchainImagesInfoEXT* info) {
        return releaseSwapchainImagesCommon(device, info, "vkReleaseSwapchainImagesEXT");
    }
#endif

    void myvkDestroySwapchainKHR(
            VkDevice device,
            VkSwapchainKHR swapchain,
            const VkAllocationCallbacks* alloc) {
        const auto& it = instance_info->devices.find(device);
        if (it == instance_info->devices.end())
            return;
        {
            const std::lock_guard retirementLock(
                instance_info->retiredSwapchainsMutex
            );
            if (instance_info->retiredSwapchains.contains(swapchain)) {
                std::cerr << "framegen-vulkan: duplicate deferred swapchain "
                             "destruction ignored\n";
                return;
            }
        }

        const auto swapchainMetadata =
            instance_info->swapchainInfos.find(swapchain);
        const VkSurfaceKHR surface = swapchainMetadata ==
                instance_info->swapchainInfos.end()
            ? VK_NULL_HANDLE
            : swapchainMetadata->second.surface;
        const bool native =
            instance_info->nativeSwapchains.contains(swapchain);
        auto context = layer_info->root.takeSwapchainContext(swapchain);
        ReflexFrameLimitRegistry::erase(swapchain);
        ReflexPresentAttributionRegistry::erase(swapchain);
        instance_info->swapchainInfos.erase(swapchain);
        instance_info->swapchains.erase(swapchain);
        instance_info->nativeSwapchains.erase(swapchain);
        // Removed before the lower destroy. A retired context keeps the object
        // so a pending lower present can still wait its copied semaphore.
        std::shared_ptr<InternalSwapchainImages> internalImages;
        if (const auto found = instance_info->internalSwapchains.find(swapchain);
                found != instance_info->internalSwapchains.end()) {
            internalImages = std::move(found->second);
            present_layout::unregisterImages(device, internalImages->images());
            instance_info->internalSwapchains.erase(found);
        }

        if (!native && context && context->presentRetirementEnabled()) {
            if (!alloc) {
                // C1: a native Wayland lower swapchain must not outlive the
                // application's destroy (colour-management lifetime). Destroy it
                // now after its proven retirement; keep the deferred path only
                // if that bounded wait times out.
                if (!retainLowerSwapchainAfterDestroy(
                        surfaceOrigin(surface),
                        layer_info->root.gamescopeSession())) {
                    if (context->waitForPresentRetirement(
                            it->second, 1'000'000'000)) {
                        static std::atomic_bool synchronousLogged{false};
                        if (!synchronousLogged.exchange(true,
                                std::memory_order_relaxed)) {
                            std::cerr << "framegen-vulkan: native Wayland swapchain "
                                         "destroyed synchronously (colour-management "
                                         "lifetime)\n";
                        }
                        // GP1: the on-screen watcher is joined before the lower swapchain is destroyed.
                        if (context) context->stopOnScreenProfile();
                        context.reset();
                        it->second.df().DestroySwapchainKHR(device, swapchain, alloc);
                        return;
                    }
                    std::cerr << "framegen-vulkan: swapchain retirement timed out "
                                 "on native Wayland; deferring lower destruction\n";
                }
                const auto now = std::chrono::steady_clock::now();
                bool inserted = false;
                {
                    const std::lock_guard retirementLock(
                        instance_info->retiredSwapchainsMutex
                    );
                    inserted = instance_info->retiredSwapchains.emplace(
                            swapchain,
                            InstanceInfo::RetiredSwapchain{
                                .device = device,
                                .surface = surface,
                                .vk = ls::R<vk::Vulkan>(it->second),
                                .context = std::move(context),
                                .notBefore = now + swapchainRetirementGracePeriod,
                                .internalImages = std::move(internalImages),
                            }
                        ).second;
                    if (inserted) {
                        instance_info->retiredSwapchainCount.fetch_add(
                            1, std::memory_order_release
                        );
                    }
                }
                if (!inserted) {
                    std::cerr << "framegen-vulkan: duplicate deferred swapchain "
                                 "retirement ignored\n";
                    return;
                }
                return;
            }
        }

        // A custom allocator cannot be retained because its pUserData lifetime
        // ends with this call. The same synchronous wait also covers
        // layer-owned fences recorded before an upstream present-fence owner
        // was observed. A valid owner has already made this wait immediately
        // satisfiable before requesting destruction.
        if (context && !context->waitForPresentRetirement(
                it->second, UINT64_MAX)) {
            std::cerr << "framegen-vulkan: synchronous swapchain retirement "
                         "failed; forwarding lower destruction without a "
                         "completed layer-owned fence\n";
        }
        // GP1: the on-screen watcher is joined before the lower swapchain is destroyed.
        if (context) context->stopOnScreenProfile();
        context.reset();
        it->second.df().DestroySwapchainKHR(device, swapchain, alloc);
    }
}

namespace {
    bool initializeLayerInfo() {
        if (layer_info) return true;

        try {
            layer_info = new LayerInfo { // NOLINT (memory management)
                .map = {
#define VKPTR(name) reinterpret_cast<PFN_vkVoidFunction>(name)
                    { "vkCreateInstance", VKPTR(myvkCreateInstance) },
                    { "vkCreateDevice", VKPTR(myvkCreateDevice) },
                    { "vkGetPhysicalDeviceFeatures2", VKPTR(myvkGetPhysicalDeviceFeatures2) },
                    { "vkGetPhysicalDeviceFeatures2KHR", VKPTR(myvkGetPhysicalDeviceFeatures2) },
                    { "vkDestroyDevice", VKPTR(myvkDestroyDevice) },
                    { "vkDeviceWaitIdle", VKPTR(myvkDeviceWaitIdle) },
                    { "vkQueueWaitIdle", VKPTR(myvkQueueWaitIdle) },
                    { queueSubmit2Name, VKPTR(myvkQueueSubmit2Common<queueSubmit2Name>) },
                    { queueSubmit2KhrName, VKPTR(myvkQueueSubmit2Common<queueSubmit2KhrName>) },
                    { waitSemaphoresName, VKPTR(myvkWaitSemaphoresCommon<waitSemaphoresName>) },
                    { waitSemaphoresKhrName, VKPTR(myvkWaitSemaphoresCommon<waitSemaphoresKhrName>) },
                    { "vkDestroyInstance", VKPTR(myvkDestroyInstance) },
                    { "vkCreateWaylandSurfaceKHR",
                        VKPTR(myvkCreateWaylandSurfaceKHR) },
                    { "vkCreateXcbSurfaceKHR",
                        VKPTR(myvkCreateXcbSurfaceKHR) },
                    { "vkCreateXlibSurfaceKHR",
                        VKPTR(myvkCreateXlibSurfaceKHR) },
                    { "vkDestroySurfaceKHR", VKPTR(myvkDestroySurfaceKHR) },
                    { "vkGetPhysicalDeviceSurfaceFormatsKHR", VKPTR(myvkGetPhysicalDeviceSurfaceFormatsKHR) },
                    { "vkGetPhysicalDeviceSurfaceFormats2KHR", VKPTR(myvkGetPhysicalDeviceSurfaceFormats2KHR) },
                    { "vkCreateSwapchainKHR", VKPTR(myvkCreateSwapchainKHR) },
                    { "vkAcquireNextImageKHR", VKPTR(myvkAcquireNextImageKHR) },
                    { "vkAcquireNextImage2KHR", VKPTR(myvkAcquireNextImage2KHR) },
                    { "vkGetSwapchainImagesKHR", VKPTR(myvkGetSwapchainImagesKHR) },
#if defined(VK_KHR_swapchain_maintenance1)
                    { "vkReleaseSwapchainImagesKHR", VKPTR(myvkReleaseSwapchainImagesKHR) },
#endif
#if defined(VK_EXT_swapchain_maintenance1)
                    { "vkReleaseSwapchainImagesEXT", VKPTR(myvkReleaseSwapchainImagesEXT) },
#endif
                    { "vkQueuePresentKHR", VKPTR(myvkQueuePresentKHR) },
                    { "vkWaitForPresentKHR", VKPTR(myvkWaitForPresentKHR) },
#if defined(VK_KHR_present_wait2)
                    { "vkWaitForPresent2KHR", VKPTR(myvkWaitForPresent2KHR) },
#endif
                    { "vkDestroySwapchainKHR", VKPTR(myvkDestroySwapchainKHR) }
#undef VKPTR
                },
                .root = Root()
            };

        } catch (const std::exception& e) {
            std::cerr << "framegen-vulkan: layer initialization failed:\n";
            std::cerr << "- " << e.what() << '\n';
            return false;
        }

        return true;
    }
}

// Interface-version negotiation is the modern layer ABI. Keep the traditional
// proc-address exports as a compatibility path for older loaders.
__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
        VkInstance instance, const char* name) {
    return myvkGetInstanceProcAddr(instance, name);
}

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
        VkDevice device, const char* name) {
    return myvkGetDeviceProcAddr(device, name);
}

/// Vulkan layer entrypoint
__attribute__((visibility("default")))
VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
        VkNegotiateLayerInterface* pVersionStruct) {
    // ensure loader compatibility
    if (!pVersionStruct
        || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT
        || pVersionStruct->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;

    // if the layer has already been initialized, skip
    if (layer_info) {
        pVersionStruct->loaderLayerInterfaceVersion = 2;
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
        pVersionStruct->pfnGetDeviceProcAddr = myvkGetDeviceProcAddr;
        pVersionStruct->pfnGetInstanceProcAddr = myvkGetInstanceProcAddr;
        return VK_SUCCESS;
    }

    // load the layer configuration
    if (!initializeLayerInfo())
        return VK_ERROR_INITIALIZATION_FAILED;

    // emplace function pointers/version
    pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    pVersionStruct->pfnGetDeviceProcAddr = myvkGetDeviceProcAddr;
    pVersionStruct->pfnGetInstanceProcAddr = myvkGetInstanceProcAddr;
    return VK_SUCCESS;
}

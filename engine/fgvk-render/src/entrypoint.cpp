/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "instance.hpp"
#include "gamescope_scaling_surface.hpp"
#include "color_pipeline.hpp"
#include "layer_role.hpp"
#include "fgvk-common/helpers/errors.hpp"
#include "fgvk-common/helpers/pointers.hpp"
#include "fgvk-common/vulkan/vulkan.hpp"
#include "fgvk-common/vulkan/device_features.hpp"
#include "present_diagnostics.hpp"
#include "wayland_presentation_feedback.hpp"
#include "swapchain/swapchain.hpp"
#include "swapchain/create_policy.hpp"
#include "swapchain/retirement.hpp"
#include "swapchain/private_present_queue.hpp"
#include "swapchain/release_gate.hpp"
#include "swapchain/internal_images.hpp"
#include "swapchain/present_marker.hpp"

#include <algorithm>
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
        std::unique_ptr<GamescopeScalingSurface> scalingSurfaces;
        SurfaceMaintenance1Support surfaceMaintenance1;
        bool surfaceCapabilities2{false};

        std::unordered_map<VkDevice, vk::Vulkan> devices;
        std::unordered_map<VkDevice, std::shared_ptr<DeferredPresentQueue>> deferredPresentQueues;
        std::unordered_set<VkDevice> nativeDevices;
        std::unordered_set<VkDevice> presentRetirementDevices;
        std::unordered_set<VkDevice> gamescopeTimingDevices;
        std::unordered_set<VkDevice> presentTimingDevices;
        std::unordered_set<VkDevice> presentMeteringDevices;
        std::unordered_set<VkDevice> copyCalibrationDevices;
        std::unordered_set<VkDevice> applicationTimingDevices;
        std::unordered_set<VkDevice> presentFeedbackDevices;
        std::unordered_set<VkDevice> hdrMetadataDevices;
        std::unordered_set<VkDevice> lowerHdrMetadataDevices;
        struct QueueIdentity {
            VkDevice device{VK_NULL_HANDLE};
            uint32_t familyIndex{UINT32_MAX};
        };
        std::unordered_map<VkQueue, QueueIdentity> queueIdentities;
        struct SurfaceScalingEligibility {
            bool supported{false};
            bool queueSupported{false};
            uint32_t advertisedFormatCount{0};
            uint32_t compatibleFormatCount{0};
        };
        std::mutex surfaceScalingMutex;
        std::unordered_map<
            VkPhysicalDevice,
            std::unordered_map<VkSurfaceKHR, SurfaceScalingEligibility>
        > surfaceScalingEligibility;
        uint64_t nextSurfaceScalingQueryGeneration{1};
        std::unordered_map<
            VkPhysicalDevice,
            std::unordered_map<VkSurfaceKHR, FixedSurfaceScalingContract>
        > fixedSurfaceScalingContracts;
        std::unordered_map<VkSurfaceKHR, SpatialScalingExtents>
            variableSurfaceScalingExtents;
        std::unordered_map<VkSurfaceKHR, SpatialScalingExtents>
            variableSurfaceRollbackExtents;
        std::unordered_map<VkSurfaceKHR, SpatialSurfaceOrigin> surfaceOrigins;
        std::unordered_map<VkSurfaceKHR, std::shared_ptr<WaylandPresentationFeedback>> waylandPresentation;
        std::unordered_set<VkSurfaceKHR> unprovenSplitSurfacesLogged;
        std::unordered_map<
            VkSurfaceKHR, ScalingAdmissionRetrySurfaceBudget
        > scalingAdmissionRetryStates;
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

    enum class FixedSurfaceCapabilityRelayOperation : uint32_t {
        BeginCapability,
        ConsumeCapability,
        BeginCreate,
        ConsumeCreate,
    };

#if defined(FGVK_LAYER_ROLE_SPATIAL_SCALING)
    thread_local FixedSurfaceCapabilityRelaySlot fixedSurfaceCapabilityRelay;
    thread_local FixedSurfaceCapabilityRelaySlot spatialCreateRelay;
#endif

    bool initializeLayerInfo();

    bool finalizeRetiredSwapchain(
            const VkSwapchainKHR swapchain, const uint64_t timeoutNs,
            const std::string_view trigger) {
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
        state.context.reset();
        state.vk.get().df().DestroySwapchainKHR(
            state.device, swapchain, VK_NULL_HANDLE
        );
        instance_info->retiredSwapchainCount.fetch_sub(
            1, std::memory_order_acq_rel
        );
        if (present_diagnostics::enabled()) {
            std::cerr << "framegen-vulkan: present diagnostics: "
                         "operation=swapchain-retirement-complete"
                      << " role=" << layerRoleName
                      << " swapchain=" << swapchain
                      << " trigger=" << trigger
                      << " pending="
                      << instance_info->retiredSwapchains.size() << '\n';
        }
        return true;
    }

    void collectRetiredSwapchains(
            const VkDevice device, const uint64_t timeoutNs,
            const std::string_view trigger) {
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
                swapchain, remainingTimeoutNs, trigger
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
                swapchain, 0, "same-surface-present"
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
                swapchain, UINT64_MAX, "surface-destroy"
            ));
    }

    std::optional<uint32_t> selectLayerQueueFamily(
            const VkPhysicalDevice physicalDevice,
            const VkDeviceCreateInfo& createInfo,
            const bool spatialScalingConfigured) {
        uint32_t familyCount{};
        instance_info->funcs.GetPhysicalDeviceQueueFamilyProperties(
            physicalDevice, &familyCount, nullptr
        );
        std::vector<VkQueueFamilyProperties> families(familyCount);
        instance_info->funcs.GetPhysicalDeviceQueueFamilyProperties(
            physicalDevice, &familyCount, families.data()
        );
        const VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT |
            (spatialScalingConfigured ? VK_QUEUE_COMPUTE_BIT : 0);
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

    bool supportedGamescopeDisplayTiming(const VkPhysicalDevice physicalDevice,
            const VkDeviceCreateInfo& createInfo) {
        if (!rendererOwnsDisplayTiming(createInfo,
                layer_info->root.gamescopeWsiTimingProvisioned(), true))
            return false;
        uint32_t count{};
        if (instance_info->funcs.EnumerateDeviceExtensionProperties(
                physicalDevice, nullptr, &count, nullptr) != VK_SUCCESS)
            return false;
        std::vector<VkExtensionProperties> extensions(count);
        if (instance_info->funcs.EnumerateDeviceExtensionProperties(
                physicalDevice, nullptr, &count, extensions.data()) != VK_SUCCESS)
            return false;
        extensions.resize(count);
        return std::ranges::any_of(extensions, [](const auto& extension) {
            return std::string_view(extension.extensionName) ==
                VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME;
        });
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
            std::unique_ptr<GamescopeScalingSurface> scalingSurfaces;
            const auto environment = [](const char* name) -> std::string_view {
                const char* value = std::getenv(name);
                return value ? value : "";
            };
            const bool needsX11Adapter = requestsGamescopeScalingSurface(*info);
            const bool retirementRequested = presentationRetirementRequired(
                layer_info->root.frameGenerationInteropProvisioned(),
                layer_info->root.scalingEngineProvisioned(),
                layer_info->root.scalingSurfaceConnectionProvisioned());
            InstanceExtensionQuery extensionQuery;
            if (retirementRequested) {
                const auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
                    layer_info->GetInstanceProcAddr(VK_NULL_HANDLE,
                        "vkEnumerateInstanceExtensionProperties"));
                extensionQuery = queryPresentationInstanceExtensions(
                    enumerate, vk::enumerateLoaderInstanceExtensionProperties);
            }
            const auto& available = extensionQuery.properties;
            if (needsX11Adapter && !(instance_info && instance_info->scalingSurfaces) &&
                    needsGamescopeScalingSurface(
                    layer_info->root.scalingSurfaceConnectionProvisioned(),
                    true, spatialScalingLayer, splitLayerChainEnabled(),
                    environment("GAMESCOPE_WAYLAND_DISPLAY"),
                    environment("WAYLAND_DISPLAY"), environment("XDG_RUNTIME_DIR"),
                    std::getenv("WAYLAND_SOCKET") != nullptr)) {
                const bool waylandSupported = std::ranges::any_of(available,
                    [](const VkExtensionProperties& extension) {
                        return std::strcmp(extension.extensionName,
                            "VK_KHR_wayland_surface") == 0;
                    });
                if (canAttemptGamescopeScalingSurface(
                        extensionQuery.result, waylandSupported)) {
                    auto candidate = std::make_unique<GamescopeScalingSurface>(
                        layer_info->root.hdrExposureEnabled(),
                        [](uint32_t gamescopePid) {
                            return layer_info->root.queryGamescopeOutputHdr(gamescopePid);
                        });
                    if (candidate->connect())
                        scalingSurfaces = std::move(candidate);
                }
                if (!scalingSurfaces)
                    std::cerr << "framegen-vulkan: spatial scaling surface bridge unavailable; "
                                 "retaining application surface extent checks\n";
            }
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
            // Wine may keep several Vulkan instances alive. Every instance
            // using the shared surface connection needs the driver extension.
            if (needsX11Adapter && (scalingSurfaces ||
                    (instance_info && instance_info->scalingSurfaces))) {
                appendExtension("VK_KHR_wayland_surface");
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
                    .scalingSurfaces = std::move(scalingSurfaces),
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
                if (scalingSurfaces)
                    instance_info->scalingSurfaces = std::move(scalingSurfaces);
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
    bool bridgeHdrEnabled() {
        return frameGenerationLayer && instance_info && instance_info->scalingSurfaces &&
            instance_info->scalingSurfaces->hdrEnabled();
    }

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

    VkResult myvkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
            const char* layerName, uint32_t* count, VkExtensionProperties* properties) {
        if (!bridgeHdrEnabled() || (layerName && std::strcmp(layerName, "VK_LAYER_FGVK_render")))
            return instance_info->funcs.EnumerateDeviceExtensionProperties(
                physicalDevice, layerName, count, properties);
        try {
            auto extensions = layerName ? std::vector<VkExtensionProperties>{}
                : lowerDeviceExtensions(physicalDevice);
            if (std::ranges::none_of(extensions, [](const auto& extension) {
                    return std::strcmp(extension.extensionName, VK_EXT_HDR_METADATA_EXTENSION_NAME) == 0;
                }))
                extensions.push_back({VK_EXT_HDR_METADATA_EXTENSION_NAME, VK_EXT_HDR_METADATA_SPEC_VERSION});
            if (!properties) {
                *count = static_cast<uint32_t>(extensions.size());
                return VK_SUCCESS;
            }
            const auto written = std::min(*count, static_cast<uint32_t>(extensions.size()));
            std::copy_n(extensions.begin(), written, properties);
            *count = written;
            return written < extensions.size() ? VK_INCOMPLETE : VK_SUCCESS;
        } catch (const ls::vulkan_error& error) {
            return error.error();
        } catch (...) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    void myvkGetPhysicalDeviceFeatures2(
            VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* features) {
        // initVulkanInstanceFuncs resolves the core spelling with the KHR
        // alias as fallback. GPA below only exposes supported entrypoints.
        instance_info->funcs.GetPhysicalDeviceFeatures2(physicalDevice, features);
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
        const bool scalingEngineProvisioned = presentationDevice &&
            layer_info->root.scalingEngineProvisioned();
        const auto swapchainMaintenance1Extension =
            supportedSwapchainMaintenance1Extension(
                physdev, presentationDevice && presentationRetirementRequired(
                    frameGenerationInteropEnabled, scalingEngineProvisioned,
                    layer_info->root.scalingSurfaceConnectionProvisioned())
            );
        const auto layerQueueFamily = presentationDevice
            ? selectLayerQueueFamily(
                physdev, *info, scalingEngineProvisioned
            )
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
        const bool gamescopeDisplayTiming = presentationDevice &&
            supportedGamescopeDisplayTiming(physdev, *info);
        bool presentRetirementEnabled = false;
        bool shaderFloat16Enabled = false;
        bool hdrMetadataEnabled = false;
        bool lowerHdrMetadataEnabled = false;
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
            std::vector<const char*> precisionExtensions;
            bool enableScalingFp16 = false;
            if (scalingEngineProvisioned && layer_info->root.fp16RequestedAtStartup() &&
                    instance_info->funcs.GetPhysicalDeviceFeatures2) {
                uint32_t count{};
                auto result = instance_info->funcs.EnumerateDeviceExtensionProperties(
                    physdev, nullptr, &count, nullptr);
                std::vector<VkExtensionProperties> supported(count);
                if (result == VK_SUCCESS)
                    result = instance_info->funcs.EnumerateDeviceExtensionProperties(
                        physdev, nullptr, &count, supported.data());
                const bool extensionSupported = result == VK_SUCCESS &&
                    std::ranges::any_of(supported, [](const auto& extension) {
                        return std::strcmp(extension.extensionName,
                            VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME) == 0;
                    });
                if (extensionSupported) {
                    VkPhysicalDeviceShaderFloat16Int8Features half{
                        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,
                    };
                    VkPhysicalDeviceFeatures2 features{
                        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                        .pNext = &half,
                    };
                    instance_info->funcs.GetPhysicalDeviceFeatures2(physdev, &features);
                    enableScalingFp16 = half.shaderFloat16 == VK_TRUE;
                    if (enableScalingFp16) {
                        if (info->enabledExtensionCount)
                            precisionExtensions.assign(info->ppEnabledExtensionNames,
                                info->ppEnabledExtensionNames + info->enabledExtensionCount);
                        if (!std::ranges::any_of(precisionExtensions, [](const char* name) {
                                return std::strcmp(name, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME) == 0;
                            }))
                            precisionExtensions.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
                        newInfo.ppEnabledExtensionNames = precisionExtensions.data();
                        newInfo.enabledExtensionCount = static_cast<uint32_t>(precisionExtensions.size());
                    }
                }
            }
            std::vector<const char*> hdrExtensions;
            if (bridgeHdrEnabled()) {
                for (uint32_t i = 0; i < newInfo.enabledExtensionCount; ++i) {
                    const auto* name = newInfo.ppEnabledExtensionNames[i];
                    if (std::strcmp(name, VK_EXT_HDR_METADATA_EXTENSION_NAME) == 0)
                        hdrMetadataEnabled = true;
                    hdrExtensions.push_back(name);
                }
                if (hdrMetadataEnabled) {
                    const auto supported = lowerDeviceExtensions(physdev);
                    lowerHdrMetadataEnabled = std::ranges::any_of(supported, [](const auto& extension) {
                        return std::strcmp(extension.extensionName, VK_EXT_HDR_METADATA_EXTENSION_NAME) == 0;
                    });
                    if (!lowerHdrMetadataEnabled)
                        std::erase_if(hdrExtensions, [](const char* name) {
                            return std::strcmp(name, VK_EXT_HDR_METADATA_EXTENSION_NAME) == 0;
                        });
                    newInfo.ppEnabledExtensionNames = hdrExtensions.data();
                    newInfo.enabledExtensionCount = static_cast<uint32_t>(hdrExtensions.size());
                }
            }
            const vk::ShaderFloat16FeatureRequest precisionFeatures(newInfo, enableScalingFp16);
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
            // Copy-profile and GPU split diagnostics only (FGVK_PACING_PROFILE_COPY=1 or
            // FGVK_PACING_PROFILE_GPU_SPLIT=1). Negotiate a supported
            // calibration alias and known clock domains without adding any feature node.
            std::vector<const char*> copyCalibrationExtensions;
            if (frameGenerationLayer && frameGenerationInteropEnabled &&
                    (present_diagnostics::pacingCopyProfileEnabled() || present_diagnostics::pacingGpuSplitProfileEnabled())) {
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
                gamescopeDisplayTiming,
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

        if (presentRetirementEnabled)
            instance_info->presentRetirementDevices.insert(*device);
        if (gamescopeDisplayTiming)
            instance_info->gamescopeTimingDevices.insert(*device);
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
        if (hdrMetadataEnabled)
            instance_info->hdrMetadataDevices.insert(*device);
        if (lowerHdrMetadataEnabled)
            instance_info->lowerHdrMetadataDevices.insert(*device);

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
            instance_info->gamescopeTimingDevices.erase(*device);
            instance_info->presentTimingDevices.erase(*device);
            instance_info->presentMeteringDevices.erase(*device);
            instance_info->copyCalibrationDevices.erase(*device);
            instance_info->applicationTimingDevices.erase(*device);
            instance_info->presentFeedbackDevices.erase(*device);
            instance_info->hdrMetadataDevices.erase(*device);
            instance_info->lowerHdrMetadataDevices.erase(*device);
            instance_info->deferredPresentQueues.erase(*device);
        };
        try {
            if (!layerQueueFamily) {
                throw ls::error(scalingEngineProvisioned
                    ? "the application did not create an ordinary queue family "
                      "with graphics and compute support"
                    : "the application did not create an ordinary graphics queue family");
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
                // it, and the emulated acquire waits it on the acquire queue. On failure it stays
                // null and the acquire runs without the tail wait.
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
                // The second private queue exists only when the family had room for both. Like
                // the present queue, it is never registered in queueIdentities, so the application
                // cannot see it. F10: no emulated acquire submits on it any more, so
                // FGVK_INTERNAL_ACQUIRE_QUEUE only changes whether it is created (the acquire
                // path itself is the same either way).
                const char* acquireQueueSwitch = std::getenv("FGVK_INTERNAL_ACQUIRE_QUEUE");
                const bool acquireQueueAllowed = !(acquireQueueSwitch && std::strcmp(acquireQueueSwitch, "0") == 0);
                if (privatePresentQueue.acquireIndex && acquireQueueAllowed) {
                    wrapper->second.df().GetDeviceQueue(*device, *layerQueueFamily,
                        *privatePresentQueue.acquireIndex, &privateQueue->acquireQueue);
                    if (privateQueue->acquireQueue && wrapper->second.loaderdatafunc()) {
                        const auto result = (*wrapper->second.loaderdatafunc())(*device, privateQueue->acquireQueue);
                        if (result != VK_SUCCESS) privateQueue->acquireQueue = VK_NULL_HANDLE;
                    }
                }
                const bool acquireQueueActive = privateQueue->queue != VK_NULL_HANDLE &&
                    privateQueue->acquireQueue != VK_NULL_HANDLE;
                // F10: the host acquire signal needs the tail timeline and both counter functions.
                // It is also refused for the device's lifetime after a failed import.
                const bool hostAcquireAvailable = privateQueue->tail != VK_NULL_HANDLE &&
                    wrapper->second.df().GetSemaphoreCounterValueKHR != nullptr &&
                    wrapper->second.df().ImportSemaphoreFdKHR != nullptr;
                std::cerr << "framegen-vulkan: internal acquire queue: active="
                          << (acquireQueueActive ? 1 : 0) << " internal_prepare="
                          << (std::getenv("FGVK_INTERNAL_PREPARE") && std::strcmp(std::getenv("FGVK_INTERNAL_PREPARE"), "0") == 0 ? 0 : 1)
                          << " acquire_tail=" << (privateQueue->tail != VK_NULL_HANDLE ? 1 : 0)
                          << " host_acquire=" << (hostAcquireAvailable ? 1 : 0)
                          << '\n';
                // T4, GPU split profile only: the marker after each observed lower present.
                if (privateQueue->queue) {
                    privateQueue->marker = createPresentMarker(wrapper->second,
                        instance_info->copyCalibrationDevices.contains(*device));
                    if (privateQueue->marker) privateQueue->markAfterPresent = &presentMarkerAfterPresent;
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
            if (scalingEngineProvisioned) {
                rollbackCreatedDevice();
                std::cerr << "framegen-vulkan: device initialization failed for "
                             "a provisioned Scaling Engine profile; refusing "
                             "native fallback after fixed-surface capability "
                             "virtualization:\n"
                          << "- " << e.what() << '\n';
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
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
            return layer_info->root.getSwapchainContext(swapchain, mapping->second.get())
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
            const auto mapping = instance_info->swapchains.find(swapchain);
            if (instance_info->nativeSwapchains.contains(swapchain)) continue;
            try {
                static_cast<void>(layer_info->root.getSwapchainContext(swapchain, mapping->second.get())
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
        // the private queues' locks too. Lock order: mutex before acquireMutex.
        if (const auto owned = instance_info->deferredPresentQueues.find(device);
                owned != instance_info->deferredPresentQueues.end()) {
            const std::lock_guard queueLock(owned->second->mutex);
            const std::lock_guard acquireLock(owned->second->acquireMutex);
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
            if (owned->second->acquireQueue != VK_NULL_HANDLE) {
                const std::lock_guard acquireLock(owned->second->acquireMutex);
                const auto result = lower(owned->second->acquireQueue);
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
        return release->consumed ? VK_SUCCESS : VK_TIMEOUT;
    }
    constexpr char waitSemaphoresName[] = "vkWaitSemaphores";
    constexpr char waitSemaphoresKhrName[] = "vkWaitSemaphoresKHR";

    bool isOptionalReleaseCommand(const std::string_view name) {
        return name == queueSubmit2Name || name == queueSubmit2KhrName ||
            name == waitSemaphoresName || name == waitSemaphoresKhrName;
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

        collectRetiredSwapchains(
            device, 250'000'000, "device-destroy"
        );
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
        instance_info->gamescopeTimingDevices.erase(device);
        instance_info->presentTimingDevices.erase(device);
        instance_info->presentMeteringDevices.erase(device);
        instance_info->copyCalibrationDevices.erase(device);
        instance_info->applicationTimingDevices.erase(device);
        instance_info->presentFeedbackDevices.erase(device);
        instance_info->hdrMetadataDevices.erase(device);
        instance_info->lowerHdrMetadataDevices.erase(device);

        // destroy device
        auto vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(
            instance_info->funcs.GetDeviceProcAddr(device, "vkDestroyDevice"));
        if (!vkDestroyDevice) {
            std::cerr << "framegen-vulkan: failed to get next layer's vkDestroyDevice, "
                "the previous layer does not follow spec\n";
            return;
        }

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
        // vkDestroyInstance() wrapper returns. Root owns a Gamescope feedback
        // monitor thread, so leaving the process-global LayerInfo allocated
        // lets that worker resume inside unmapped layer code after dlclose().
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
                DeferredPresent::applicationWait(swapchain, presentId, timeout);
                return DeferredPresent::waitForPresent(device, swapchain, presentId, timeout,
                    [&](uint64_t remaining) { return next(device, swapchain, presentId, remaining); });
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
                DeferredPresent::applicationWait(swapchain, info->presentId, info->timeout);
                return DeferredPresent::waitForPresent(device, swapchain, info->presentId, info->timeout,
                    [&](uint64_t remaining) {
                        auto adjusted = *info;
                        adjusted.timeout = remaining;
                        return next(device, swapchain, &adjusted);
                    });
            });
    }
#endif

    // get optional function pointer override
    void VKAPI_CALL myvkSetHdrMetadataEXT(VkDevice device, uint32_t count,
            const VkSwapchainKHR* swapchains, const VkHdrMetadataEXT* metadata) {
        const auto lower = reinterpret_cast<PFN_vkSetHdrMetadataEXT>(
            instance_info->funcs.GetDeviceProcAddr(device, "vkSetHdrMetadataEXT"));
        if (!instance_info->hdrMetadataDevices.contains(device)) {
            if (lower)
                lower(device, count, swapchains, metadata);
            return;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const auto found = instance_info->swapchainInfos.find(swapchains[i]);
            if (found != instance_info->swapchainInfos.end() &&
                    instance_info->scalingSurfaces->setHdrMetadata(
                        found->second.surface, swapchains[i], metadata[i]))
                continue;
            if (lower && instance_info->lowerHdrMetadataDevices.contains(device))
                lower(device, 1, swapchains + i, metadata + i);
        }
    }

    PFN_vkVoidFunction getProcAddr(const std::string& name) {
        if (name == "vkSetHdrMetadataEXT" && !bridgeHdrEnabled())
            return nullptr;
        // The application role also observes actual serialized completion
        // pacing when diagnostics are off. Lower spatial roles forward waits
        // untouched and must not count or classify the same call twice.
        if (isPresentWaitCommand(name) && !frameGenerationLayer)
            return nullptr;
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

        if (std::string_view(name) ==
                "vkGetPhysicalDeviceSurfaceCapabilities2KHR" ||
                std::string_view(name) ==
                "vkGetPhysicalDeviceSurfaceFormats2KHR" ||
                std::string_view(name) == "vkGetPhysicalDeviceFeatures2" ||
                std::string_view(name) == "vkGetPhysicalDeviceFeatures2KHR" ||
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
        return layer_info->GetInstanceProcAddr(instance, name);
    }

    // get device-level function pointers
    // Reflex pass-through, pacing profile on only: each call is recorded, then forwarded with its
    // arguments unchanged. The extension is never advertised; these answer only an app that asks for it.
    VkResult VKAPI_CALL myvkSetLatencySleepModeNV(VkDevice device, VkSwapchainKHR swapchain,
            const VkLatencySleepModeInfoNV* info) {
        static thread_local CachedLower<PFN_vkSetLatencySleepModeNV, VkDevice> lower;
        const auto next = lower.get(device, [&] {
            return instance_info->funcs.GetDeviceProcAddr(device, "vkSetLatencySleepModeNV"); });
        if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
        if (info) present_diagnostics::recordReflexMode(info->lowLatencyMode, info->lowLatencyBoost,
            info->minimumIntervalUs);
        return next(device, swapchain, info);
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
        next(device, swapchain, info);
    }

    // The Reflex wrapper for a command the next layer exports, with the pacing profile on only.
    // Profile off, or a next layer without the command, returns nothing and the lookup stays unchanged.
    PFN_vkVoidFunction reflexPassThrough(VkDevice device, const char* name) {
        if (!present_diagnostics::pacingProfileEnabled()) return nullptr;
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

        if (std::string_view(name) == "vkSetHdrMetadataEXT" &&
                !instance_info->hdrMetadataDevices.contains(device))
            return instance_info->funcs.GetDeviceProcAddr(device, name);

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

        return instance_info->funcs.GetDeviceProcAddr(device, name);
    }
}

#if defined(FGVK_LAYER_ROLE_SPATIAL_SCALING)
// The split roles are separate DSOs, and Gamescope WSI may replace the surface
// handle between them. Export only a same-thread, one-shot capability relay
// from the lower spatial role. The upper role brackets one downstream query;
// current split layout 2 uses the immutable result to own the combined
// scaler/frame-generation context without moving the lower capability owner.
extern "C" __attribute__((visibility("default"))) VkBool32
fgvkSpatialScalingLookupFixedContract(
        const uint32_t rawOperation,
        const VkPhysicalDevice physicalDevice,
        VkSurfaceKHR* const lowerSurface,
        VkExtent2D* const source,
        VkExtent2D* const presentation,
        float* const factor,
        uint64_t* const policyRevision,
        uint64_t* const queryGeneration,
        VkBool32* const spatialSurfaceScalingSupported,
        VkBool32* const variableSurface,
        VkBool32* const memoryBudgetConstrained,
        uint32_t* const inactiveReason) noexcept {
    const auto operation = static_cast<FixedSurfaceCapabilityRelayOperation>(
        rawOperation
    );
    FixedSurfaceCapabilityRelaySlot* slot{};
    if (operation ==
            FixedSurfaceCapabilityRelayOperation::BeginCapability ||
            operation ==
            FixedSurfaceCapabilityRelayOperation::ConsumeCapability) {
        slot = &fixedSurfaceCapabilityRelay;
    } else if (operation ==
            FixedSurfaceCapabilityRelayOperation::BeginCreate ||
            operation == FixedSurfaceCapabilityRelayOperation::ConsumeCreate) {
        slot = &spatialCreateRelay;
    }
    if (!slot)
        return VK_FALSE;

    if (operation == FixedSurfaceCapabilityRelayOperation::BeginCapability ||
            operation == FixedSurfaceCapabilityRelayOperation::BeginCreate) {
        slot->begin();
        return VK_TRUE;
    }
    if (!lowerSurface || !source || !presentation || !factor ||
            !policyRevision || !queryGeneration ||
            !spatialSurfaceScalingSupported || !variableSurface ||
            !memoryBudgetConstrained || !inactiveReason) {
        slot->begin();
        return VK_FALSE;
    }

    const auto record = slot->consume(physicalDevice);
    if (!record)
        return VK_FALSE;

    *lowerSurface = record->lowerSurface;
    *source = record->contract.extents.source;
    *presentation = record->contract.extents.presentation;
    *factor = record->contract.factor;
    *policyRevision = record->contract.policyRevision;
    *queryGeneration = record->contract.queryGeneration;
    const auto metadata = fixedSurfaceScalingContractRelayMetadata(
        record->contract
    );
    *spatialSurfaceScalingSupported =
        metadata.spatialSurfaceScalingSupported;
    *variableSurface = metadata.variableSurface;
    *memoryBudgetConstrained = metadata.memoryBudgetConstrained;
    *inactiveReason = metadata.inactiveReason;
    return VK_TRUE;
}
#endif

namespace {
    using LowerFixedSurfaceContractLookup = VkBool32 (*) (
        uint32_t, VkPhysicalDevice, VkSurfaceKHR*, VkExtent2D*, VkExtent2D*,
        float*, uint64_t*, uint64_t*, VkBool32*, VkBool32*, VkBool32*,
        uint32_t*
    );

    constexpr std::string_view lowerFixedSurfaceContractSymbol =
        "fgvkSpatialScalingLookupFixedContract";
    constexpr std::string_view lowerSpatialLayerLibrary =
        "libfgvk-render-scaling.so";

    struct LowerFixedSurfaceContractLookupState {
        void* handle{nullptr};
        LowerFixedSurfaceContractLookup lookup{nullptr};
        VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
        FixedSurfaceCapabilityRelayOperation beginOperation{
            FixedSurfaceCapabilityRelayOperation::BeginCapability
        };
        FixedSurfaceCapabilityRelayOperation consumeOperation{
            FixedSurfaceCapabilityRelayOperation::ConsumeCapability
        };
        bool armed{false};

        LowerFixedSurfaceContractLookupState() = default;
        LowerFixedSurfaceContractLookupState(
                const FixedSurfaceCapabilityRelayOperation beginOperation,
                const FixedSurfaceCapabilityRelayOperation consumeOperation) :
            beginOperation(beginOperation), consumeOperation(consumeOperation) {}
        LowerFixedSurfaceContractLookupState(
            const LowerFixedSurfaceContractLookupState&
        ) = delete;
        LowerFixedSurfaceContractLookupState& operator=(
            const LowerFixedSurfaceContractLookupState&
        ) = delete;

        ~LowerFixedSurfaceContractLookupState() {
            reset();
            if (handle)
                dlclose(handle);
        }

        void reset() noexcept {
            if (armed && lookup) {
                static_cast<void>(lookup(
                    static_cast<uint32_t>(
                        this->beginOperation
                    ),
                    physicalDevice, nullptr, nullptr, nullptr, nullptr,
                    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
                ));
            }
            armed = false;
        }

        void begin(const VkPhysicalDevice requestedPhysicalDevice) noexcept {
            physicalDevice = requestedPhysicalDevice;
            if (!lookup)
                return;
            armed = lookup(
                static_cast<uint32_t>(
                    this->beginOperation
                ),
                physicalDevice, nullptr, nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr, nullptr
            ) == VK_TRUE;
        }

        [[nodiscard]] std::optional<FixedSurfaceCapabilityRelayRecord>
        consume() noexcept {
            if (!armed || !lookup)
                return std::nullopt;

            FixedSurfaceCapabilityRelayRecord record{
                .physicalDevice = physicalDevice,
            };
            FixedSurfaceScalingContractRelayMetadata metadata;
            const VkBool32 found = lookup(
                static_cast<uint32_t>(
                    this->consumeOperation
                ),
                physicalDevice, &record.lowerSurface,
                &record.contract.extents.source,
                &record.contract.extents.presentation,
                &record.contract.factor, &record.contract.policyRevision,
                &record.contract.queryGeneration,
                &metadata.spatialSurfaceScalingSupported,
                &metadata.variableSurface, &metadata.memoryBudgetConstrained,
                &metadata.inactiveReason
            );
            armed = false;
            if (found != VK_TRUE ||
                    !applyFixedSurfaceScalingContractRelayMetadata(
                        record.contract, metadata
                    )) {
                return std::nullopt;
            }
            return record;
        }
    };

    int findLowerFixedSurfaceContractLookup(
            struct dl_phdr_info* const info, const size_t,
            void* const rawState) {
        auto& state = *static_cast<LowerFixedSurfaceContractLookupState*>(
            rawState
        );
        const std::string_view libraryPath(
            info && info->dlpi_name ? info->dlpi_name : ""
        );
        if (!libraryPath.ends_with(lowerSpatialLayerLibrary))
            return 0;

        void* const handle = dlopen(
            libraryPath.data(), RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD
        );
        if (!handle)
            return 0;
        const auto lookup = reinterpret_cast<LowerFixedSurfaceContractLookup>(
            dlsym(handle, lowerFixedSurfaceContractSymbol.data())
        );
        if (!lookup) {
            dlclose(handle);
            return 0;
        }
        state.handle = handle;
        state.lookup = lookup;
        return 1;
    }

    void beginLowerFixedSurfaceCapabilityRelay(
            LowerFixedSurfaceContractLookupState& state,
            const VkPhysicalDevice physicalDevice) {
        if (!spatialScalingCapabilityRelayByLayer())
            return;

        dl_iterate_phdr(findLowerFixedSurfaceContractLookup, &state);
        if (!state.lookup)
            return;
        state.begin(physicalDevice);
    }

    struct SurfaceScalingEligibilityResult {
        bool supported{false};
        bool firstObservation{false};
        bool queueSupported{false};
        uint32_t advertisedFormatCount{0};
        uint32_t compatibleFormatCount{0};
    };

    SurfaceScalingEligibilityResult supportsSpatialScalingSurface(
            const VkPhysicalDevice physicalDevice,
            const VkSurfaceKHR surface,
            const VkSurfaceCapabilitiesKHR& capabilities) {
        if (!spatialScalingSurfaceCapabilitiesSupported(capabilities)) {
            return {};
        }
        {
            const std::lock_guard lock(
                instance_info->surfaceScalingMutex
            );
            const auto physical =
                instance_info->surfaceScalingEligibility.find(physicalDevice);
            if (physical !=
                    instance_info->surfaceScalingEligibility.end()) {
                const auto cached = physical->second.find(surface);
                if (cached != physical->second.end()) {
                    return {
                        .supported = cached->second.supported,
                        .firstObservation = false,
                        .queueSupported = cached->second.queueSupported,
                        .advertisedFormatCount =
                            cached->second.advertisedFormatCount,
                        .compatibleFormatCount =
                            cached->second.compatibleFormatCount,
                    };
                }
            }
        }

        bool supported = true;

        bool queueSupported = false;
        if (supported &&
                instance_info->funcs.GetPhysicalDeviceSurfaceSupportKHR) {
            uint32_t familyCount{};
            instance_info->funcs.GetPhysicalDeviceQueueFamilyProperties(
                physicalDevice, &familyCount, nullptr
            );
            std::vector<VkQueueFamilyProperties> families(familyCount);
            instance_info->funcs.GetPhysicalDeviceQueueFamilyProperties(
                physicalDevice, &familyCount, families.data()
            );
            constexpr VkQueueFlags required =
                VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            for (uint32_t family = 0; family < familyCount; ++family) {
                if ((families.at(family).queueFlags & required) != required)
                    continue;
                VkBool32 presentationSupported = VK_FALSE;
                const auto result =
                    instance_info->funcs.GetPhysicalDeviceSurfaceSupportKHR(
                        physicalDevice, family, surface,
                        &presentationSupported
                    );
                if (result == VK_SUCCESS &&
                        presentationSupported == VK_TRUE) {
                    queueSupported = true;
                    break;
                }
            }
        }
        supported = supported && queueSupported;

        constexpr VkFormatFeatureFlags scalerFeatures =
            VK_FORMAT_FEATURE_BLIT_SRC_BIT |
            VK_FORMAT_FEATURE_BLIT_DST_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        uint32_t formatCount{};
        std::vector<VkSurfaceFormatKHR> formats;
        if (supported) {
            bool complete = false;
            constexpr uint32_t maximumEnumerationAttempts = 3;
            for (uint32_t attempt = 0;
                    attempt < maximumEnumerationAttempts && !complete;
                    ++attempt) {
                auto result =
                    instance_info->funcs.GetPhysicalDeviceSurfaceFormatsKHR(
                        physicalDevice, surface, &formatCount, nullptr
                    );
                if (result != VK_SUCCESS || formatCount == 0)
                    return {};
                formats.resize(formatCount);
                uint32_t writtenCount = formatCount;
                result =
                    instance_info->funcs.GetPhysicalDeviceSurfaceFormatsKHR(
                        physicalDevice, surface, &writtenCount,
                        formats.data()
                    );
                if (result == VK_SUCCESS) {
                    if (writtenCount == 0)
                        return {};
                    formats.resize(writtenCount);
                    formatCount = writtenCount;
                    complete = true;
                } else if (result != VK_INCOMPLETE) {
                    return {};
                }
            }
            // A partial prefix cannot prove that every advertised format is
            // safe. Leave transient INCOMPLETE results uncached so a later
            // stable query can retry the eligibility decision.
            if (!complete)
                return {};
        }

        constexpr VkFormatFeatureFlags swapchainFeatures =
            VK_FORMAT_FEATURE_BLIT_SRC_BIT |
            VK_FORMAT_FEATURE_BLIT_DST_BIT;
        uint32_t compatibleFormatCount = 0;
        if (supported) {
            compatibleFormatCount = static_cast<uint32_t>(
                std::ranges::count_if(
                    formats.begin(), formats.begin() + formatCount,
                    [&](const VkSurfaceFormatKHR& format) {
                    if (format.format == VK_FORMAT_UNDEFINED)
                        return false;
                    const auto pipeline = classifySwapchainColor(
                        format.format, format.colorSpace, false
                    );
                    if (!spatialScalingColorSupported(pipeline)) {
                        return false;
                    }

                    VkFormatProperties surfaceProperties{};
                    instance_info->funcs.GetPhysicalDeviceFormatProperties(
                        physicalDevice, format.format, &surfaceProperties
                    );
                    if ((surfaceProperties.optimalTilingFeatures &
                            swapchainFeatures) != swapchainFeatures) {
                        return false;
                    }

                    VkFormatProperties workingProperties{};
                    instance_info->funcs.GetPhysicalDeviceFormatProperties(
                        physicalDevice, pipeline.exchangeFormat,
                        &workingProperties
                    );
                    return (workingProperties.optimalTilingFeatures &
                        scalerFeatures) == scalerFeatures;
                    }
                )
            );
        }
        supported = spatialScalingFixedSurfacePreflightSupported(
            queueSupported, formatCount, compatibleFormatCount
        );

        bool firstObservation = false;
        {
            const std::lock_guard lock(
                instance_info->surfaceScalingMutex
            );
            auto& surfaces =
                instance_info->surfaceScalingEligibility[physicalDevice];
            const auto [cached, inserted] = surfaces.emplace(
                surface,
                InstanceInfo::SurfaceScalingEligibility{
                    .supported = supported,
                    .queueSupported = queueSupported,
                    .advertisedFormatCount = formatCount,
                    .compatibleFormatCount = compatibleFormatCount,
                }
            );
            supported = cached->second.supported;
            queueSupported = cached->second.queueSupported;
            formatCount = cached->second.advertisedFormatCount;
            compatibleFormatCount = cached->second.compatibleFormatCount;
            firstObservation = inserted;
        }
        return {
            .supported = supported,
            .firstObservation = firstObservation,
            .queueSupported = queueSupported,
            .advertisedFormatCount = formatCount,
            .compatibleFormatCount = compatibleFormatCount,
        };
    }

    void clearFixedSurfaceScalingContract(
            const VkPhysicalDevice physicalDevice,
            const VkSurfaceKHR surface) {
        if (!instance_info)
            return;

        const std::lock_guard lock(instance_info->surfaceScalingMutex);
        const auto physicalContracts =
            instance_info->fixedSurfaceScalingContracts.find(physicalDevice);
        if (physicalContracts !=
                instance_info->fixedSurfaceScalingContracts.end()) {
            physicalContracts->second.erase(surface);
            if (physicalContracts->second.empty()) {
                instance_info->fixedSurfaceScalingContracts.erase(
                    physicalContracts
                );
            }
        }
    }

    void recordSurfaceOrigin(
            const VkSurfaceKHR surface,
            const SpatialSurfaceOrigin origin) {
        if (!instance_info || surface == VK_NULL_HANDLE)
            return;
        bool inserted{};
        {
            const std::lock_guard lock(instance_info->surfaceScalingMutex);
            inserted = instance_info->surfaceOrigins.emplace(
                surface, origin
            ).second;
        }
        if (inserted && present_diagnostics::enabled()) {
            std::cerr << "framegen-vulkan: present diagnostics: "
                         "operation=surface-provenance"
                      << " role=" << layerRoleName
                      << " surface=" << surface
                      << " origin=" << spatialSurfaceOriginName(origin)
                      << " gamescope_wsi_surface_proven="
                      << (origin == SpatialSurfaceOrigin::Wayland ? 1 : 0)
                      << '\n';
        }
    }

    [[nodiscard]] SpatialSurfaceOrigin surfaceOrigin(
            const VkSurfaceKHR surface) {
        if (!instance_info)
            return SpatialSurfaceOrigin::Unknown;
        const std::lock_guard lock(instance_info->surfaceScalingMutex);
        const auto origin = instance_info->surfaceOrigins.find(surface);
        return origin == instance_info->surfaceOrigins.end()
            ? SpatialSurfaceOrigin::Unknown : origin->second;
    }

    [[nodiscard]] bool spatialSurfaceScalingSupported(
            const VkSurfaceKHR surface) {
        return spatialSplitSurfaceScalingSupported(
            spatialScalingLayer, surfaceOrigin(surface)
        );
    }

    void logUnprovenSplitSurfaceOnce(const VkSurfaceKHR surface) {
        if (!instance_info || !spatialScalingLayer)
            return;
        bool firstObservation{};
        const auto origin = surfaceOrigin(surface);
        {
            const std::lock_guard lock(instance_info->surfaceScalingMutex);
            firstObservation = instance_info->unprovenSplitSurfacesLogged
                .insert(surface).second;
        }
        if (firstObservation) {
            std::cerr << "framegen-vulkan: spatial scaling split surface policy: "
                      << "role=" << layerRoleName
                      << "; surface=" << surface
                      << "; origin=" << spatialSurfaceOriginName(origin)
                      << "; gamescope_wsi_surface_proven=0"
                      << "; action=native"
                      << "; inactive_reason="
                      << spatialScalingInactiveReasonName(
                          SpatialScalingInactiveReason::
                              GamescopeWsiSurfaceUnproven
                      ) << '\n';
        }
    }

    VkResult maybeVirtualizeSurfaceCapabilities(
            const VkPhysicalDevice physicalDevice,
            const VkSurfaceKHR surface,
            VkSurfaceCapabilitiesKHR& capabilities,
            const std::optional<FixedSurfaceCapabilityRelayRecord>&
                lowerRelay) {
        if (!layer_info || !instance_info) {
            return VK_SUCCESS;
        }
        if (instance_info->scalingSurfaces) {
            const auto result = instance_info->scalingSurfaces->applicationCapabilities(
                surface, capabilities
            );
            if (result) {
                // The application sees its X11 window size, while create-time
                // policy queries the driver's real variable Wayland extent.
                // This is not a fixed-surface source/presentation contract.
                clearFixedSurfaceScalingContract(physicalDevice, surface);
                return *result;
            }
        }
        if (!spatialScalingCapabilityOwnedByLayer() &&
                !spatialScalingCapabilityRelayByLayer()) {
            clearFixedSurfaceScalingContract(physicalDevice, surface);
            return VK_SUCCESS;
        }
        if (!spatialSurfaceScalingSupported(surface)) {
            clearFixedSurfaceScalingContract(physicalDevice, surface);
            logUnprovenSplitSurfaceOnce(surface);
            return VK_SUCCESS;
        }
        try {
            auto candidate = capabilities;
            const auto relayInput = capabilities.currentExtent;
            const FixedSurfaceScalingContract* const lowerContract =
                lowerRelay ? &lowerRelay->contract : nullptr;
            if (spatialScalingCapabilityRelayByLayer() &&
                    (!lowerContract || !prepareFixedSurfaceCapabilityRelay(
                        candidate, *lowerContract
                    ))) {
                clearFixedSurfaceScalingContract(physicalDevice, surface);
                return VK_SUCCESS;
            }
            const auto selection =
                layer_info->root.modifySurfaceCapabilities(candidate);
            if (!selection) {
                clearFixedSurfaceScalingContract(physicalDevice, surface);
                return VK_SUCCESS;
            }
            if (lowerContract &&
                    (!sameExtent(
                        selection->extents.source,
                        lowerContract->extents.source
                    ) || !sameExtent(
                        selection->extents.presentation,
                        lowerContract->extents.presentation
                    ) || selection->factor != lowerContract->factor)) {
                clearFixedSurfaceScalingContract(physicalDevice, surface);
                std::cerr << "framegen-vulkan: spatial scaling capability relay "
                             "failed closed: reason=lower-contract-mismatch\n";
                return VK_SUCCESS;
            }
            const auto eligibility = supportsSpatialScalingSurface(
                physicalDevice, surface, capabilities
            );
            if (!eligibility.supported) {
                clearFixedSurfaceScalingContract(physicalDevice, surface);
                if (eligibility.firstObservation) {
                    std::cerr << "framegen-vulkan: spatial scaling capability "
                             "virtualization unavailable: reason="
                          << (eligibility.queueSupported
                                ? "no-compatible-sdr-format"
                                : "no-presentation-capable-graphics-compute-queue")
                          << "; queue_supported="
                          << eligibility.queueSupported
                          << "; advertised_formats="
                          << eligibility.advertisedFormatCount
                          << "; compatible_formats="
                          << eligibility.compatibleFormatCount
                          << '\n';
                }
                return VK_SUCCESS;
            }
            uint64_t queryGeneration{};
            FixedSurfaceScalingContract publishedContract;
            {
                const std::lock_guard lock(
                    instance_info->surfaceScalingMutex
                );
                queryGeneration =
                    instance_info->nextSurfaceScalingQueryGeneration++;
                publishedContract = FixedSurfaceScalingContract{
                    .extents = selection->extents,
                    .factor = selection->factor,
                    .policyRevision = selection->policyRevision,
                    .queryGeneration = queryGeneration,
                };
                instance_info->fixedSurfaceScalingContracts[physicalDevice]
                    .insert_or_assign(surface, publishedContract);
            }
#if defined(FGVK_LAYER_ROLE_SPATIAL_SCALING)
            fixedSurfaceCapabilityRelay.publish(
                physicalDevice, surface, publishedContract
            );
#endif
            if (eligibility.firstObservation) {
                std::cerr << "framegen-vulkan: spatial scaling surface virtualized: "
                          << "role=" << layerRoleName
                          << "; source=" << candidate.currentExtent.width << 'x'
                          << candidate.currentExtent.height
                          << "; presentation="
                          << selection->extents.presentation.width << 'x'
                          << selection->extents.presentation.height
                          << "; policy_revision="
                          << selection->policyRevision
                          << "; query_generation=" << queryGeneration
                          << "; queue_supported="
                          << eligibility.queueSupported
                          << "; advertised_formats="
                          << eligibility.advertisedFormatCount
                          << "; compatible_formats="
                          << eligibility.compatibleFormatCount;
                if (lowerContract) {
                    std::cerr << "; relay_input=" << relayInput.width << 'x'
                              << relayInput.height
                              << "; relay_mode="
                              << (sameExtent(
                                      relayInput,
                                      lowerContract->extents.source
                                  )
                                      ? "source-normalized"
                                      : "presentation-preserved")
                              << "; relay_surface_mode="
                              << (lowerRelay->lowerSurface == surface
                                      ? "shared"
                                      : "aliased");
                }
                std::cerr << '\n';
            }
            capabilities = candidate;
        } catch (const std::exception& error) {
            clearFixedSurfaceScalingContract(physicalDevice, surface);
            std::cerr << "framegen-vulkan: spatial scaling capability policy "
                         "failed closed: " << error.what() << '\n';
        }
        return VK_SUCCESS;
    }

    VkResult myvkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            const VkPhysicalDevice physicalDevice,
            const VkSurfaceKHR surface,
            VkSurfaceCapabilitiesKHR* capabilities) {
        if (!instance_info ||
                !instance_info->funcs.GetPhysicalDeviceSurfaceCapabilitiesKHR)
            return VK_ERROR_INITIALIZATION_FAILED;
        LowerFixedSurfaceContractLookupState lowerRelayState;
        beginLowerFixedSurfaceCapabilityRelay(
            lowerRelayState, physicalDevice
        );
        const auto result =
            instance_info->funcs.GetPhysicalDeviceSurfaceCapabilitiesKHR(
                physicalDevice, surface, capabilities
            );
        const auto lowerRelay = lowerRelayState.consume();
        if (result == VK_SUCCESS && capabilities) {
            return maybeVirtualizeSurfaceCapabilities(
                physicalDevice, surface, *capabilities, lowerRelay
            );
        } else {
            clearFixedSurfaceScalingContract(physicalDevice, surface);
        }
        return result;
    }

    template <typename Format>
    VkResult copyApplicationSurfaceFormats(
            const std::vector<VkSurfaceFormatKHR>& formats,
            uint32_t* count, Format* output) {
        if (!count)
            return VK_ERROR_INITIALIZATION_FAILED;
        if (!output) {
            *count = static_cast<uint32_t>(formats.size());
            return VK_SUCCESS;
        }
        const uint32_t written = std::min(*count,
            static_cast<uint32_t>(formats.size()));
        for (uint32_t i = 0; i < written; ++i) {
            if constexpr (std::is_same_v<Format, VkSurfaceFormatKHR>)
                output[i] = formats[i];
            else
                output[i].surfaceFormat = formats[i];
        }
        *count = written;
        return written < formats.size() ? VK_INCOMPLETE : VK_SUCCESS;
    }

    VkResult myvkGetPhysicalDeviceSurfaceFormatsKHR(
            const VkPhysicalDevice physicalDevice, const VkSurfaceKHR surface,
            uint32_t* count, VkSurfaceFormatKHR* formats) {
        if (!instance_info ||
                !instance_info->funcs.GetPhysicalDeviceSurfaceFormatsKHR)
            return VK_ERROR_INITIALIZATION_FAILED;
        try {
            if (instance_info->scalingSurfaces) {
                const auto applicationFormats =
                    instance_info->scalingSurfaces->applicationFormats(
                        physicalDevice, surface,
                        instance_info->funcs.GetPhysicalDeviceSurfaceFormatsKHR);
                if (applicationFormats)
                    return copyApplicationSurfaceFormats(*applicationFormats,
                        count, formats);
            }
        } catch (const std::bad_alloc&) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        return instance_info->funcs.GetPhysicalDeviceSurfaceFormatsKHR(
            physicalDevice, surface, count, formats);
    }

    VkResult myvkGetPhysicalDeviceSurfaceFormats2KHR(
            const VkPhysicalDevice physicalDevice,
            const VkPhysicalDeviceSurfaceInfo2KHR* surfaceInfo,
            uint32_t* count, VkSurfaceFormat2KHR* formats) {
        if (!layer_info || !instance_info || !surfaceInfo || !count)
            return VK_ERROR_INITIALIZATION_FAILED;
        const auto lower = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormats2KHR>(
            layer_info->GetInstanceProcAddr(instance_info->handles.front(),
                "vkGetPhysicalDeviceSurfaceFormats2KHR"));
        if (!lower)
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        bool plainFormats = !surfaceInfo->pNext;
        if (formats) {
            for (uint32_t i = 0; i < *count; ++i)
                plainFormats = plainFormats && !formats[i].pNext;
        }
        try {
            if (instance_info->scalingSurfaces) {
                // Keep ordinary queries on their existing allocation/query
                // path. Extended queries need their own lower constraints.
                if (plainFormats) {
                    const auto applicationFormats =
                        instance_info->scalingSurfaces->applicationFormats(
                            physicalDevice, surfaceInfo->surface,
                            instance_info->funcs.GetPhysicalDeviceSurfaceFormatsKHR);
                    if (applicationFormats)
                        return copyApplicationSurfaceFormats(*applicationFormats, count, formats);
                } else {
                    const auto result = instance_info->scalingSurfaces->applicationFormats2(
                        physicalDevice, *surfaceInfo, count, formats, lower);
                    if (result)
                        return *result;
                }
            }
        } catch (const std::bad_alloc&) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        return lower(physicalDevice, surfaceInfo, count, formats);
    }

    VkResult myvkGetPhysicalDeviceSurfaceCapabilities2KHR(
            const VkPhysicalDevice physicalDevice,
            const VkPhysicalDeviceSurfaceInfo2KHR* surfaceInfo,
            VkSurfaceCapabilities2KHR* capabilities) {
        if (!layer_info || !instance_info || !surfaceInfo || !capabilities)
            return VK_ERROR_INITIALIZATION_FAILED;
        const auto lower = reinterpret_cast<
            PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
                layer_info->GetInstanceProcAddr(
                    instance_info->handles.front(),
                    "vkGetPhysicalDeviceSurfaceCapabilities2KHR"
                )
            );
        if (!lower) {
            clearFixedSurfaceScalingContract(
                physicalDevice, surfaceInfo->surface
            );
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        LowerFixedSurfaceContractLookupState lowerRelayState;
        beginLowerFixedSurfaceCapabilityRelay(
            lowerRelayState, physicalDevice
        );
        const auto result = lower(physicalDevice, surfaceInfo, capabilities);
        const auto lowerRelay = lowerRelayState.consume();
        if (result == VK_SUCCESS) {
            return maybeVirtualizeSurfaceCapabilities(
                physicalDevice, surfaceInfo->surface,
                capabilities->surfaceCapabilities, lowerRelay
            );
        } else {
            clearFixedSurfaceScalingContract(
                physicalDevice, surfaceInfo->surface
            );
        }
        return result;
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
                const std::lock_guard lock(
                    instance_info->surfaceScalingMutex
                );
                for (auto& [physicalDevice, surfaces] :
                        instance_info->surfaceScalingEligibility) {
                    static_cast<void>(physicalDevice);
                    surfaces.erase(surface);
                }
                for (auto& [physicalDevice, contracts] :
                        instance_info->fixedSurfaceScalingContracts) {
                    static_cast<void>(physicalDevice);
                    contracts.erase(surface);
                }
                instance_info->variableSurfaceScalingExtents.erase(surface);
                instance_info->variableSurfaceRollbackExtents.erase(surface);
                instance_info->surfaceOrigins.erase(surface);
                if (const auto feedback = instance_info->waylandPresentation.find(surface);
                        feedback != instance_info->waylandPresentation.end()) {
                    feedback->second->stop();
                    instance_info->waylandPresentation.erase(feedback);
                }
                instance_info->unprovenSplitSurfacesLogged.erase(surface);
                instance_info->scalingAdmissionRetryStates.erase(surface);
            }
        }
        const auto lower = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
            layer_info->GetInstanceProcAddr(instance, "vkDestroySurfaceKHR")
        );
        if (lower)
            lower(instance, surface, alloc);
        if (instance_info && instance_info->scalingSurfaces)
            instance_info->scalingSurfaces->destroy(surface);
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
            const SpatialSurfaceOrigin origin) {
        const auto lower = reinterpret_cast<CreateSurface>(
            layer_info->GetInstanceProcAddr(instance, entrypoint)
        );
        if (!lower)
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        const auto result = lower(instance, createInfo, alloc, surface);
        present_diagnostics::traceVulkanSurface(layerRoleName, entrypoint, instance,
            result == VK_SUCCESS && surface ? *surface : VK_NULL_HANDLE, result);
        if (result == VK_SUCCESS && surface)
            recordSurfaceOrigin(*surface, origin);
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
            SpatialSurfaceOrigin::Wayland);
        // FGVK_PACING_PROFILE_DISPLAY=0 keeps the profile but leaves the display probe out (A/B run).
        const bool diagnostics = present_diagnostics::pacingWaylandProfileEnabled() ||
            present_diagnostics::pacingDisplayProbeEnabled();
        if (result == VK_SUCCESS && createInfo && surface && instance_info &&
                !spatialScalingLayer && diagnostics) {
            try {
                auto observer = WaylandPresentationFeedback::create(createInfo->display, createInfo->surface, diagnostics);
                if (observer) {
                    const std::lock_guard lock(instance_info->surfaceScalingMutex);
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
        if (instance_info && instance_info->scalingSurfaces && createInfo) {
            try {
                const auto result = instance_info->scalingSurfaces->create(
                    instance, layer_info->GetInstanceProcAddr, *createInfo, alloc, surface);
                if (result) {
                    if (*result == VK_SUCCESS)
                        recordSurfaceOrigin(*surface, SpatialSurfaceOrigin::Wayland);
                    return *result;
                }
            } catch (const std::bad_alloc&) {
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
        }
        return createSurfaceAndRecord<
            CreateXcbSurface
        >(instance, createInfo, alloc, surface, "vkCreateXcbSurfaceKHR",
            SpatialSurfaceOrigin::Xcb);
    }

    VkResult myvkCreateXlibSurfaceKHR(
            const VkInstance instance,
            const VkXlibSurfaceCreateInfoKHR* createInfo,
            const VkAllocationCallbacks* alloc,
            VkSurfaceKHR* surface) {
        if (instance_info && instance_info->scalingSurfaces && createInfo) {
            try {
                const auto result = instance_info->scalingSurfaces->create(
                    instance, layer_info->GetInstanceProcAddr, *createInfo, alloc, surface);
                if (result) {
                    if (*result == VK_SUCCESS)
                        recordSurfaceOrigin(*surface, SpatialSurfaceOrigin::Wayland);
                    return *result;
                }
            } catch (const std::bad_alloc&) {
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
        }
        return createSurfaceAndRecord<
            CreateXlibSurface
        >(instance, createInfo, alloc, surface, "vkCreateXlibSurfaceKHR",
            SpatialSurfaceOrigin::Xlib);
    }

    InternalSwapchainImages* internalImagesFor(const VkSwapchainKHR swapchain) {
        const auto found = instance_info->internalSwapchains.find(swapchain);
        return found == instance_info->internalSwapchains.end()
            ? nullptr : found->second.get();
    }

    // Diagnostic copy paths assume the lower image is the application's image.
    // FGVK_PACING_PROFILE_COPY=1 selects the native copy profile, and
    // FGVK_PACING_PROFILE_CONTENT=1 enables frame-content capture (see
    // FrameContentCapture::create). Either keeps internal images off.
    bool internalDiagnosticCopyPathConfigured() {
        const char* content = std::getenv("FGVK_PACING_PROFILE_CONTENT");
        return present_diagnostics::pacingCopyProfileEnabled() ||
            (content && std::string_view(content) == "1");
    }

    // F5 gate and creation for one context-backed swapchain. Refusal or any
    // allocation failure keeps the direct lower-image path. The decision is
    // logged once for each swapchain created with a Frame Generation context.
    void installInternalSwapchainImages(vk::Vulkan& vk,
            const VkSwapchainCreateInfoKHR& application,
            const VkSwapchainKHR swapchain, const SwapchainInfo& swapchainInfo) {
        const char* reason = nullptr;
        uint32_t imageCount = 0;
        std::shared_ptr<InternalSwapchainImages> images;
        try {
            const auto& context = layer_info->root.getSwapchainContext(swapchain, vk);
            const auto& profile = context.appliedProfile();
            const auto support = queryInternalImageSupport(
                vk, application.imageFormat, application.imageUsage);
            const InternalImageGateInput gate{
                .environmentEnabled = internalSwapchainEnvironmentEnabled(),
                .nativeSwapchain = false,
                .contextCreated = true,
                .frameGenerationEnabled = profile.frame_generation_enabled,
                .adaptive = profile.adaptive,
                .deferredPresentQueue = static_cast<bool>(swapchainInfo.deferredPresentQueue),
                .diagnosticCopyPath = internalDiagnosticCopyPathConfigured(),
                .gamescopeScalingSurface = swapchainInfo.gamescopeScalingSurface != nullptr,
                .gamescopeDetected = context.gamescopeDetectedAtCreation(),
                .spatialScalingActive = swapchainInfo.spatialScalingActive,
                .hdrDisplayResize = swapchainInfo.hdrDisplayResize,
                .variableSurface = swapchainInfo.variableSurface,
                .applicationExtent = swapchainInfo.applicationExtent,
                .lowerExtent = swapchainInfo.extent,
                .applicationFormat = application.imageFormat,
                .lowerFormat = swapchainInfo.format,
                .imageArrayLayers = application.imageArrayLayers,
                .applicationFlags = application.flags,
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
        } catch (const std::exception&) {
            images.reset();
            reason = "context-not-created";
        }
        if (images)
            instance_info->internalSwapchains.insert_or_assign(swapchain, images);
        std::cerr << "framegen-vulkan: internal swapchain images: active="
                  << (images ? 1 : 0)
                  << " reason=" << (reason ? reason : "eligible")
                  << " images=" << (images ? imageCount : 0)
                  << " lower_images=" << swapchainInfo.images.size()
                  << " surface=" << (swapchainInfo.variableSurface ? "variable" : "fixed")
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
            instance_info->internalSwapchains.erase(createdSwapchain);
            if (instance_info->scalingSurfaces) {
                instance_info->scalingSurfaces->destroySwapchain(
                    info->surface, createdSwapchain
                );
            }
            try {
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
            // Keep framegen-vulkan's old context and scaler resources alive until the
            // application explicitly destroys that handle.

            // A live factor change can make the upper role request recreation
            // immediately after it observes a new profile. Force the lower
            // role past the present-path polling interval here so both split
            // layers build the replacement from the same saved revision.
            layer_info->root.update(true);

            if (present_diagnostics::enabled()) {
                std::cerr << "framegen-vulkan: present diagnostics: "
                             "operation=swapchain-create-observed"
                          << " role=" << layerRoleName
                          << " surface=" << info->surface
                          << " requested_old_swapchain="
                          << info->oldSwapchain
                          << " requested_width=" << info->imageExtent.width
                          << " requested_height=" << info->imageExtent.height
                          << " requested_min_images=" << info->minImageCount
                          << " requested_present_mode="
                          << static_cast<int>(info->presentMode)
                          << '\n';
            }

            // create swapchain
            VkSwapchainCreateInfoKHR newInfo = *info;
            bool ownedPresentTiming = false;
            uint32_t ownedPresentTimingTargetStage{};
            std::optional<SpatialScalingExtents> previousVariableExtents;
            std::optional<SpatialScalingExtents>
                variableSurfaceRollbackExtents;
            std::optional<FixedSurfaceScalingContract> fixedSurfaceContract;
            {
                const std::lock_guard lock(
                    instance_info->surfaceScalingMutex
                );
                const auto previous =
                    instance_info->variableSurfaceScalingExtents.find(
                        info->surface
                    );
                if (previous !=
                        instance_info->variableSurfaceScalingExtents.end()) {
                    previousVariableExtents = previous->second;
                }
                const auto rollback =
                    instance_info->variableSurfaceRollbackExtents.find(
                        info->surface
                    );
                if (rollback !=
                        instance_info->variableSurfaceRollbackExtents.end()) {
                    variableSurfaceRollbackExtents = rollback->second;
                }
                const auto physicalContracts =
                    instance_info->fixedSurfaceScalingContracts.find(
                        it->second.physdev()
                    );
                if (physicalContracts !=
                        instance_info->fixedSurfaceScalingContracts.end()) {
                    const auto contract = physicalContracts->second.find(
                        info->surface
                    );
                    if (contract != physicalContracts->second.end())
                        fixedSurfaceContract = contract->second;
                }
            }
            // Fixed Gamescope surfaces do not participate in the variable-
            // surface cache, but live factor changes can still recreate a
            // swapchain before WSI changes the application's virtual window.
            // Supply the exact previous split from the replaced swapchain so
            // policy can retain it safely. For the null-old WSI spelling,
            // accept a unique live context on this surface only.
            const auto previousSwapchainExtents = [&]()
                    -> std::optional<SpatialScalingExtents> {
                const auto extentsFor = [&](const auto& swapchainInfo)
                        -> std::optional<SpatialScalingExtents> {
                    if (swapchainInfo.surface != info->surface || sameExtent(
                            swapchainInfo.applicationExtent,
                            swapchainInfo.extent
                        )) {
                        return std::nullopt;
                    }
                    return SpatialScalingExtents{
                        .source = swapchainInfo.applicationExtent,
                        .presentation = swapchainInfo.extent,
                    };
                };
                if (info->oldSwapchain != VK_NULL_HANDLE) {
                    const auto previous =
                        instance_info->swapchainInfos.find(info->oldSwapchain);
                    return previous != instance_info->swapchainInfos.end()
                        ? extentsFor(previous->second) : std::nullopt;
                }

                std::optional<SpatialScalingExtents> unique;
                for (const auto& [handle, swapchainInfo] :
                        instance_info->swapchainInfos) {
                    static_cast<void>(handle);
                    const auto candidate = extentsFor(swapchainInfo);
                    if (!candidate)
                        continue;
                    if (unique)
                        return std::nullopt;
                    unique = candidate;
                }
                return unique;
            }();
            if (previousSwapchainExtents)
                previousVariableExtents = previousSwapchainExtents;
            // The application already destroyed this null-old predecessor.
            // Complete its existing fence/grace-protected retirement before
            // querying live memory for the replacement. Otherwise admission
            // charges the old WSI/private buffers against their replacement
            // and can permanently discard a valid scaling envelope.
            // Keep the copied surface proof above; non-null oldSwapchain
            // contexts remain application-owned and are not retired here.
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
                        UINT64_MAX,
                        "replacement-create")) {
                    throw ls::vulkan_error(
                        VK_ERROR_INITIALIZATION_FAILED,
                        "retained swapchain retirement failed "
                        "before null-old replacement"
                    );
                }
                retiredNullOldReplacement = true;
                if (present_diagnostics::enabled()) {
                    std::cerr << "framegen-vulkan: present diagnostics: "
                                 "operation=swapchain-retirement-before-replacement"
                              << " role=" << layerRoleName
                              << " swapchain="
                              << *retainedOldSwapchain
                              << " surface=" << info->surface
                              << " reason=null-upper-old-swapchain"
                              << " lower_old_swapchain=0"
                              << " action=destroy-before-create"
                              << '\n';
                }
            }
            LowerFixedSurfaceContractLookupState lowerCreateRelayState(
                FixedSurfaceCapabilityRelayOperation::BeginCreate,
                FixedSurfaceCapabilityRelayOperation::ConsumeCreate
            );
            if (spatialScalingOwnedByLayer() &&
                    spatialScalingCapabilityRelayByLayer()) {
                beginLowerFixedSurfaceCapabilityRelay(
                    lowerCreateRelayState, it->second.physdev()
                );
            }
            bool spatialScalingActivationSupported =
                spatialSurfaceScalingSupported(info->surface);
            auto modification =
                layer_info->root.modifySwapchainCreateInfo(
                    it->second, newInfo, previousVariableExtents,
                    variableSurfaceRollbackExtents,
                    fixedSurfaceContract,
                    spatialScalingActivationSupported,
                    instance_info->scalingSurfaces &&
                        instance_info->scalingSurfaces->owns(info->surface),
                    [&](const FixedSurfaceScalingContract& contract) {
#if defined(FGVK_LAYER_ROLE_SPATIAL_SCALING)
                        if (spatialScalingCapabilityOwnedByLayer() &&
                                !spatialScalingOwnedByLayer()) {
                            spatialCreateRelay.publish(
                                it->second.physdev(), info->surface, contract
                            );
                        }
#else
                        static_cast<void>(contract);
#endif
                    },
                    [&, newInfo = &newInfo]() {
                        auto driverInfo = *newInfo;
                        if (instance_info->scalingSurfaces) {
                            const auto prepared = instance_info->scalingSurfaces->prepareSwapchain(
                                it->second.physdev(), driverInfo,
                                instance_info->funcs.GetPhysicalDeviceSurfaceFormatsKHR);
                            if (prepared != VK_SUCCESS)
                                throw ls::vulkan_error(prepared, "HDR surface format is unavailable");
                        }
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
                        // The bridge already owns protocol timing. Native and
                        // full-WSI surfaces negotiate their own Vulkan contract.
                        if (instance_info->presentTimingDevices.contains(device) &&
                                driverInfo.presentMode == VK_PRESENT_MODE_FIFO_KHR &&
                                !(instance_info->scalingSurfaces &&
                                  instance_info->scalingSurfaces->owns(info->surface))) {
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
                            const VkResult initialResult = res;
                            VkSwapchainCreateInfoKHR retryInfo = driverInfo;
                            retryInfo.minImageCount = info->minImageCount;
                            *swapchain = VK_NULL_HANDLE;
                            res = it->second.df().CreateSwapchainKHR(
                                device, &retryInfo, alloc, swapchain
                            );
                            present_diagnostics::traceVulkanSetup(layerRoleName, *info, retryInfo,
                                res, res == VK_SUCCESS ? *swapchain : VK_NULL_HANDLE, device);
                            if (present_diagnostics::enabled()) {
                                std::cerr << "framegen-vulkan: present diagnostics: "
                                             "operation=swapchain-create-compatibility-retry"
                                          << " role=" << layerRoleName
                                          << " initial_result="
                                          << static_cast<int>(initialResult)
                                          << " retry_result="
                                          << static_cast<int>(res)
                                          << " requested_min_images="
                                          << info->minImageCount
                                          << " provisioned_min_images="
                                          << provisionedMinImages
                                          << " retry_min_images="
                                          << retryInfo.minImageCount
                                          << " action="
                                          << (res == VK_SUCCESS
                                                ? "use-application-minimum"
                                                : "propagate-retry-failure")
                                          << '\n';
                            }
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
                        // Optional private ABI: no change to application or WSI
                        // formats. Supply the initial shader choice before images.
                        using SetHdrPrecision = VkBool32 (VKAPI_PTR *)(
                            VkDevice, VkSwapchainKHR, VkBool32);
                        const auto setHdrPrecision = reinterpret_cast<SetHdrPrecision>(
                            instance_info->funcs.GetDeviceProcAddr(device,
                                "makoSetSwapchainHdrPrecisionV1"));
                        if (setHdrPrecision)
                            setHdrPrecision(device, *swapchain,
                                layer_info->root.hdrCompactBuffersEnabled());
                        if (driverInfo.imageColorSpace != newInfo->imageColorSpace) {
                            // vkBasalt sees the driver's SRGB transport pair. Supply
                            // the original colour space before it exposes images or
                            // builds an effect graph; never infer HDR from bit depth.
                            using SetColorSpace = VkBool32 (VKAPI_PTR *)(
                                VkDevice, VkSwapchainKHR, VkColorSpaceKHR);
                            const auto setColorSpace = reinterpret_cast<SetColorSpace>(
                                instance_info->funcs.GetDeviceProcAddr(device,
                                    "makoSetSwapchainColorSpaceV1"));
                            if (setColorSpace) {
                                if (!setColorSpace(device, *swapchain, newInfo->imageColorSpace))
                                    throw ls::vulkan_error(VK_ERROR_FORMAT_NOT_SUPPORTED,
                                        "shader layer rejected the HDR colour-space handoff");
                            } else if (environmentFlagEnabled(std::getenv("ENABLE_VKBASALT"))) {
                                throw ls::vulkan_error(VK_ERROR_FORMAT_NOT_SUPPORTED,
                                    "HDR shaders require the colour-aware framegen-vulkan vkBasalt build");
                            }
                        }
                    }
                );
            const auto lowerCreateRelay = lowerCreateRelayState.consume();
            const auto lowerCreateDecision = lowerCreateRelay
                ? classifySpatialCreateRelay(
                    lowerCreateRelay->contract, info->imageExtent
                )
                : SpatialCreateRelayDecision::Unavailable;
            const bool lowerCreateSplitDecision = lowerCreateDecision ==
                SpatialCreateRelayDecision::Split;
            const bool lowerCreateNativeDecision = lowerCreateDecision ==
                SpatialCreateRelayDecision::Native;
            if (lowerCreateSplitDecision) {
                const bool relayAdjustedUpperPrediction =
                    !modification.spatialScalingActive ||
                    !sameExtent(
                        modification.applicationExtent,
                        lowerCreateRelay->contract.extents.source
                    ) ||
                    !sameExtent(
                        modification.presentationExtent,
                        lowerCreateRelay->contract.extents.presentation
                    );
                modification.applicationExtent =
                    lowerCreateRelay->contract.extents.source;
                modification.presentationExtent =
                    lowerCreateRelay->contract.extents.presentation;
                modification.spatialScalingActive = true;
                modification.variableSurface =
                    lowerCreateRelay->contract.variableSurface;
                modification.spatialScalingMemoryConstrained =
                    lowerCreateRelay->contract.memoryBudgetConstrained;
                modification.spatialScalingInactiveReason =
                    SpatialScalingInactiveReason::None;
                newInfo.imageExtent = modification.presentationExtent;
                std::cerr << "framegen-vulkan: spatial scaling create relay applied: "
                          << "role=" << layerRoleName
                          << "; source="
                          << modification.applicationExtent.width << 'x'
                          << modification.applicationExtent.height
                          << "; presentation="
                          << modification.presentationExtent.width << 'x'
                          << modification.presentationExtent.height
                          << "; relay_surface_mode="
                          << (lowerCreateRelay->lowerSurface == info->surface
                                ? "shared" : "aliased")
                          << "; ownership=upper-combined-role"
                          << "; surface_extent_mode="
                          << (modification.variableSurface
                                ? "variable" : "fixed")
                          << "; memory_budget_constrained="
                          << modification.spatialScalingMemoryConstrained
                          << "; source_presentation_split=1"
                          << "; active=1"
                          << "; relay_adjusted_upper_prediction="
                          << relayAdjustedUpperPrediction
                          << "; pipeline=combined-cost-aware\n";
            } else if (lowerCreateNativeDecision) {
                // The lower extent owner is authoritative even when the upper
                // role could independently derive a variable-surface split.
                // A native result means WSI did not provide a safe compositor
                // presentation surface, so discard the upper prediction and
                // keep frame generation on the application's exact extent.
                modification.applicationExtent = info->imageExtent;
                modification.presentationExtent = info->imageExtent;
                modification.spatialScalingActive = false;
                modification.variableSurface =
                    lowerCreateRelay->contract.variableSurface;
                modification.spatialScalingMemoryConstrained = false;
                modification.spatialScalingInactiveReason =
                    lowerCreateRelay->contract.inactiveReason;
                newInfo.imageExtent = info->imageExtent;
                spatialScalingActivationSupported = lowerCreateRelay->contract
                    .spatialSurfaceScalingSupported;
                std::cerr << "framegen-vulkan: spatial scaling create relay applied: "
                          << "role=" << layerRoleName
                          << "; source=" << info->imageExtent.width << 'x'
                          << info->imageExtent.height
                          << "; presentation=" << info->imageExtent.width << 'x'
                          << info->imageExtent.height
                          << "; relay_surface_mode="
                          << (lowerCreateRelay->lowerSurface == info->surface
                                ? "shared" : "aliased")
                          << "; ownership=upper-combined-role"
                          << "; source_presentation_split=0"
                          << "; active=0"
                          << "; action=native"
                          << "; inactive_reason="
                          << spatialScalingInactiveReasonName(
                                modification.spatialScalingInactiveReason
                             )
                          << '\n';
            } else if (shouldRejectUncontractedSpatialCreate(
                    layer_info->root.scalingEngineProvisioned(),
                    modification.spatialScalingActive,
                    lowerCreateSplitDecision || lowerCreateNativeDecision)) {
                std::cerr << "framegen-vulkan: spatial scaling create relay unavailable: "
                          << "role=" << layerRoleName
                          << "; requested=" << info->imageExtent.width << 'x'
                          << info->imageExtent.height
                          << "; action=reject-transient-native-context"
                          << "; inactive_reason=no-lower-create-contract\n";
                throw ls::vulkan_error(
                    VK_ERROR_INITIALIZATION_FAILED,
                    "the combined scaling/frame-generation owner did not "
                    "receive the lower WSI extent contract"
                );
            }
            const auto commitVariableSurfaceScaling = [&]() {
                const std::lock_guard lock(
                    instance_info->surfaceScalingMutex
                );
                // The current lower split role owns extent selection but not
                // reconstruction resources. Retain its successful variable
                // source/presentation pair even though spatialScalingActive
                // is intentionally false for that allocation-free context;
                // later lower creates need this proven presentation envelope
                // before the upper resource owner can consume the relay.
                const bool spatialScalingExtentSelected =
                    modification.spatialScalingActive ||
                    (spatialScalingCapabilityOwnedByLayer() &&
                        !sameExtent(
                            modification.applicationExtent,
                            modification.presentationExtent
                        ));
                const auto committedRollback =
                    committedVariableSurfaceRollbackExtents(
                        previousVariableExtents,
                        variableSurfaceRollbackExtents,
                        layer_info->root.active(),
                        modification.variableSurface,
                        spatialScalingExtentSelected,
                        modification.variableFeedbackSuppressed,
                        modification.retainVariableSurfaceProof,
                        modification.spatialScalingMemoryConstrained,
                        modification.applicationExtent,
                        modification.presentationExtent
                    );
                const auto committed =
                    modification.spatialScalingAdmissionRetryEligible &&
                        previousVariableExtents
                    ? previousVariableExtents
                    : committedVariableSurfaceScalingExtents(
                        previousVariableExtents,
                        layer_info->root.active(),
                        modification.variableSurface,
                        spatialScalingExtentSelected,
                        modification.variableFeedbackSuppressed,
                        modification.retainVariableSurfaceProof,
                        modification.applicationExtent,
                        modification.presentationExtent,
                        modification.retainVariableSurfacePresentationProof
                    );
                if (committed) {
                    instance_info->variableSurfaceScalingExtents.insert_or_assign(
                        info->surface, *committed
                    );
                } else {
                    instance_info->variableSurfaceScalingExtents.erase(
                        info->surface
                    );
                }
                if (committedRollback) {
                    instance_info->variableSurfaceRollbackExtents
                        .insert_or_assign(info->surface, *committedRollback);
                } else {
                    instance_info->variableSurfaceRollbackExtents.erase(
                        info->surface
                    );
                }
            };

            // The internal HDR resize must not shrink CAS/DLS from one source
            // pixel to one display pixel. Hand off admitted source geometry
            // before vkBasalt exposes images and builds its immutable graph.
            if (hdrBridgeSharpenSourceRequired(
                    modification.hdrDisplayResize,
                    modification.spatialScalingActive,
                    modification.applicationExtent,
                    modification.presentationExtent)) {
                using SetSharpenSource = VkBool32 (VKAPI_PTR *)(
                    VkDevice, VkSwapchainKHR, uint32_t, uint32_t);
                const auto setSharpenSource = reinterpret_cast<SetSharpenSource>(
                    instance_info->funcs.GetDeviceProcAddr(device,
                        "makoSetSwapchainSharpenSourceExtentV1"));
                if (setSharpenSource) {
                    const auto source = modification.applicationExtent;
                    const auto accepted = setSharpenSource(
                        device, *swapchain, source.width, source.height);
                    std::cerr << "framegen-vulkan: HDR shader source footprint: "
                              << "source=" << source.width << 'x' << source.height
                              << "; output=" << modification.presentationExtent.width
                              << 'x' << modification.presentationExtent.height
                              << "; accepted=" << accepted << '\n';
                }
            }

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

            if (instance_info->scalingSurfaces &&
                    !instance_info->scalingSurfaces->createSwapchain(
                        info->surface, *swapchain, newInfo, info->imageExtent, imageCount,
                        instance_info->engineName,
                        modification.gamescopeProtocolPresentMode,
                        modification.gamescopePresentRefreshHz)) {
                throw ls::vulkan_error(
                    VK_ERROR_SURFACE_LOST_KHR,
                    "Gamescope scaling surface swapchain feedback failed"
                );
            }
            if (present_diagnostics::enabled() &&
                    instance_info->scalingSurfaces &&
                    instance_info->scalingSurfaces->owns(info->surface)) {
                std::cerr << "framegen-vulkan: present diagnostics: "
                             "operation=gamescope-scaling-swapchain-feedback"
                          << " surface=" << info->surface
                          << " swapchain=" << *swapchain
                          << " image_count=" << imageCount
                          << " format=" << static_cast<uint32_t>(
                                newInfo.imageFormat)
                          << " color_space=" << static_cast<uint32_t>(
                                newInfo.imageColorSpace)
                          << " requested_old_swapchain="
                          << info->oldSwapchain
                          << " lower_present_mode="
                          << static_cast<uint32_t>(newInfo.presentMode)
                          << " compositor_present_mode="
                          << (modification.gamescopeProtocolPresentMode
                                ? static_cast<int64_t>(
                                    *modification.gamescopeProtocolPresentMode)
                                : -1)
                          << " action=fresh-protocol-lifetime"
                          << '\n';
            }

            std::shared_ptr<WaylandPresentationFeedback> waylandPresentation;
            {
                const std::lock_guard lock(instance_info->surfaceScalingMutex);
                const auto observer = instance_info->waylandPresentation.find(info->surface);
                if (observer != instance_info->waylandPresentation.end()) waylandPresentation = observer->second;
            }
            auto& swapchainInfo = instance_info->swapchainInfos.emplace(*swapchain, SwapchainInfo {
                .images = std::move(swapchainImages),
                .surface = info->surface,
                .surfaceOrigin = surfaceOrigin(info->surface),
                .waylandPresentation = std::move(waylandPresentation),
                .deferredPresentQueue = instance_info->deferredPresentQueues.contains(device)
                    ? instance_info->deferredPresentQueues.at(device) : nullptr,
                .gamescopeScalingSurface = instance_info->scalingSurfaces &&
                        instance_info->scalingSurfaces->owns(info->surface)
                    ? instance_info->scalingSurfaces.get() : nullptr,
                .gamescopeDisplayTiming =
                    instance_info->gamescopeTimingDevices.contains(device),
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
                .swapchainImageCountCompatibility =
                    modification.swapchainImageCountCompatibility,
                .applicationOldSwapchainProvided =
                    info->oldSwapchain != VK_NULL_HANDLE,
                .applicationExtent = modification.applicationExtent,
                .extent = newInfo.imageExtent,
                .gamescopePresentationTarget =
                    modification.gamescopePresentationTarget,
                .incomingPresentMode = info->presentMode,
                .presentMode = modification.gamescopeProtocolPresentMode
                    .value_or(newInfo.presentMode),
                .privateOrderedTransport =
                    modification.privateOrderedTransport,
                .spatialScalingActive =
                    modification.spatialScalingActive,
                .hdrDisplayResize = modification.hdrDisplayResize,
                .variableSurface = modification.variableSurface,
                .spatialScalingMemoryConstrained =
                    modification.spatialScalingMemoryConstrained,
                .spatialScalingAdmissionRetryEligible =
                    modification.spatialScalingAdmissionRetryEligible &&
                    swapchainCreateIsReplacement(
                        newInfo.oldSwapchain, retiredNullOldReplacement
                    ),
                .spatialScalingPolicyRevision =
                    modification.spatialScalingPolicyRevision,
                .spatialScalingInactiveReason =
                    modification.spatialScalingInactiveReason,
                .spatialScalingActivationSupported =
                    spatialScalingActivationSupported,
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
                commitVariableSurfaceScaling();
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
                // The lower image is native-sized when scaling is active,
                // while the application intentionally renders only the
                // virtual source rectangle. Falling back to direct native
                // presentation would expose an incomplete top-left frame, so
                // fail creation and let the normal rollback destroy the lower
                // swapchain instead.
                if (swapchainInfo.spatialScalingActive)
                    throw ls::error(
                        "spatial scaling context initialization failed", e
                    );
                instance_info->swapchains.emplace(
                    *swapchain, ls::R<vk::Vulkan>(it->second)
                );
                instance_info->nativeSwapchains.insert(*swapchain);
                commitVariableSurfaceScaling();
                lowerSwapchainCreated = false;
                std::cerr << "framegen-vulkan: frame-generation context initialization "
                             "failed; native presentation retained:\n"
                          << "- " << e.what() << '\n';
                return res;
            }

            installInternalSwapchainImages(it->second, *info, *swapchain,
                swapchainInfo);

            instance_info->swapchains.emplace(*swapchain,
                ls::R<vk::Vulkan>(it->second));

            commitVariableSurfaceScaling();
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

        if (instance_info->scalingSurfaces) {
            for (uint32_t i = 0; i < info->swapchainCount; ++i) {
                const auto metadata = instance_info->swapchainInfos.find(info->pSwapchains[i]);
                if (instance_info->nativeSwapchains.contains(info->pSwapchains[i]) &&
                        metadata != instance_info->swapchainInfos.end()) {
                    const auto prepared = instance_info->scalingSurfaces->preparePresent(
                        metadata->second.surface, info->pSwapchains[i]);
                    if (prepared != VK_SUCCESS) {
                        const auto mapping = instance_info->swapchains.find(info->pSwapchains[i]);
                        if (mapping == instance_info->swapchains.end())
                            return VK_ERROR_INITIALIZATION_FAILED;
                        return completeRejectedPresent(mapping->second.get().df().QueueSubmit,
                            queue, *info, prepared);
                    }
                }
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
            try {
                const auto metadata =
                    instance_info->swapchainInfos.find(swapchain);
                if (metadata != instance_info->swapchainInfos.end() &&
                        metadata->second.spatialScalingActive) {
                    const auto identity =
                        instance_info->queueIdentities.find(queue);
                    if (identity == instance_info->queueIdentities.end() ||
                            identity->second.familyIndex !=
                                it->second.get().queueFamilyIndex() ||
                            queue != it->second.get().queue()) {
                        throw ls::vulkan_error(
                            VK_ERROR_UNKNOWN,
                            "spatial scaling requires the application's "
                            "ordinary queue 0 from framegen-vulkan's graphics/compute "
                            "presentation family"
                        );
                    }
                }
                const auto waitSemaphores = info->waitSemaphoreCount > 0
                    ? std::span<const VkSemaphore>(
                        info->pWaitSemaphores, info->waitSemaphoreCount
                    )
                    : std::span<const VkSemaphore>{};

                auto& context = layer_info->root.getSwapchainContext(swapchain, it->second);
                presentingContextId = context.diagnosticsId();
                if (const auto found = instance_info->internalSwapchains.find(swapchain);
                        found != instance_info->internalSwapchains.end()) {
                    // A copy keeps the images alive for this whole present call.
                    const auto internal = found->second;
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
                const VkSurfaceKHR presentingSurface = metadata ==
                        instance_info->swapchainInfos.end()
                    ? VK_NULL_HANDLE
                    : metadata->second.surface;
                std::optional<ScalingAdmissionRetryKey>
                    scalingAdmissionRetryKey;
                if (metadata != instance_info->swapchainInfos.end() &&
                        metadata->second.
                            spatialScalingAdmissionRetryEligible) {
                    scalingAdmissionRetryKey = ScalingAdmissionRetryKey{
                        .sourceWidth =
                            metadata->second.applicationExtent.width,
                        .sourceHeight =
                            metadata->second.applicationExtent.height,
                        .policyRevision = metadata->second.
                            spatialScalingPolicyRevision,
                    };
                } else if (presentingSurface != VK_NULL_HANDLE &&
                        metadata != instance_info->swapchainInfos.end() &&
                        metadata->second.spatialScalingActive) {
                    instance_info->scalingAdmissionRetryStates.erase(
                        presentingSurface
                    );
                }
                bool scalingAdmissionRetrySurfaceAvailable = false;
                if (presentingSurface != VK_NULL_HANDLE &&
                        scalingAdmissionRetryKey) {
                    scalingAdmissionRetrySurfaceAvailable = instance_info
                        ->scalingAdmissionRetryStates[presentingSurface]
                        .available(*scalingAdmissionRetryKey);
                }
                const bool scalingAdmissionRetryRequested =
                    context.requestSpatialScalingAdmissionRetryAfterPresent(
                        result,
                        !liveProfileRecreationRequested &&
                            scalingAdmissionRetrySurfaceAvailable
                    );
                if (scalingAdmissionRetryRequested &&
                        scalingAdmissionRetryKey) {
                    instance_info->scalingAdmissionRetryStates[
                        presentingSurface
                    ].record(*scalingAdmissionRetryKey);
                    if (present_diagnostics::enabled()) {
                        std::cerr << "framegen-vulkan: present diagnostics: "
                                     "operation=spatial-scaling-admission-retry-budget"
                                  << " surface=" << presentingSurface
                                  << " source="
                                  << scalingAdmissionRetryKey->sourceWidth
                                  << 'x'
                                  << scalingAdmissionRetryKey->sourceHeight
                                  << " policy_revision="
                                  << scalingAdmissionRetryKey->policyRevision
                                  << " action=episode-consumed\n";
                    }
                }
                const bool orderedAcquireRecoveryRecreationRequested =
                    context.requestOrderedAcquireRecreationAfterPresent(
                        result,
                        !liveProfileRecreationRequested &&
                            !scalingAdmissionRetryRequested
                    );
                if (liveProfileRecreationRequested ||
                        scalingAdmissionRetryRequested ||
                        orderedAcquireRecoveryRecreationRequested) {
                    // This role owns the guarded request only after
                    // Swapchain::present() returned from a successful lower
                    // present. The application's acquired real image has
                    // therefore already reached the presentation engine; make
                    // that proof explicit before replacing the successful
                    // result with the one-shot recreation signal.
                    if (present_diagnostics::enabled()) {
                        std::cerr << "framegen-vulkan: present diagnostics: "
                                     "operation=original-present-recreation-propagate"
                                  << " context=" << context.diagnosticsId()
                                  << " result=" << VK_ERROR_OUT_OF_DATE_KHR
                                  << " action=application-image-submitted-before-recreation\n";
                    }
                    result = VK_ERROR_OUT_OF_DATE_KHR;
                }
                if (result == VK_ERROR_OUT_OF_DATE_KHR &&
                        present_diagnostics::enabled()) {
                    std::cerr << "framegen-vulkan: present diagnostics: "
                                 "operation=swapchain-recreation-observed"
                              << " context=" << context.diagnosticsId()
                              << " role=" << layerRoleName
                              << " swapchain=" << swapchain
                              << " source="
                              << (liveProfileRecreationRequested
                                    ? "guarded-live-profile-request"
                                    : scalingAdmissionRetryRequested
                                        ? "spatial-scaling-admission-retry"
                                    : orderedAcquireRecoveryRecreationRequested
                                        ? "transition-scoped-acquire-timeout"
                                    : "upstream-or-driver")
                              << '\n';
                }
            } catch (const ls::vulkan_error& e) {
                if (e.error() != VK_ERROR_OUT_OF_DATE_KHR) {
                    std::cerr << "framegen-vulkan: swapchain presentation failed:\n";
                    std::cerr << "- " << e.what() << '\n';
                } else if (presentingContextId &&
                        present_diagnostics::enabled()) {
                    // Some direct original-image paths report OUT_OF_DATE by
                    // exception after the application image has already
                    // traversed the lower role. Preserve the same upper-role
                    // recreation observation as the normal returned-result
                    // path so the game receives one coherent signal.
                    std::cerr << "framegen-vulkan: present diagnostics: "
                                 "operation=swapchain-recreation-observed"
                              << " context=" << *presentingContextId
                              << " role=" << layerRoleName
                              << " swapchain=" << swapchain
                              << " source=upstream-or-driver\n";
                }

                result = e.error();
            } catch (const std::exception& e) {
                std::cerr << "framegen-vulkan: swapchain presentation failed:\n";
                std::cerr << "- " << e.what() << '\n';
                result = VK_ERROR_UNKNOWN;
            }

            if (result != VK_SUCCESS && info->pResults)
                info->pResults[i] = result;

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

    VkResult bridgeAcquisitionResult(const VkSwapchainKHR swapchain) {
        if (!instance_info->scalingSurfaces)
            return VK_SUCCESS;
        const auto info = instance_info->swapchainInfos.find(swapchain);
        return info == instance_info->swapchainInfos.end() ? VK_SUCCESS
            : instance_info->scalingSurfaces->acquisitionResult(
                info->second.surface, swapchain);
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
        auto& context = layer_info->root.getSwapchainContext(swapchain, mapping->second.get());
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
        auto& context = layer_info->root.getSwapchainContext(swapchain, mapping->second.get());
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
        if (!images.isAcquired(applicationIndex))
            return VK_ERROR_VALIDATION_FAILED_EXT;
        if (const VkResult sticky = images.stickyError(); sticky != VK_SUCCESS) {
            static_cast<void>(images.consumeWithoutCopy(applicationIndex, applicationWaits));
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
            if (!descriptor.handedOff)
                images.releaseNotHandedOff(queue, applicationIndex);
            throw;
        }
        if (!descriptor.handedOff) {
            images.releaseNotHandedOff(queue, applicationIndex);
            return result < 0 ? result : VK_ERROR_OUT_OF_DATE_KHR;
        }
        return result;
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
        const auto extentResult = bridgeAcquisitionResult(swapchain);
        if (extentResult != VK_SUCCESS) return extentResult;
        return present_diagnostics::observeApplicationAcquire(
            device, swapchain, present_diagnostics::ApplicationFrameApi::AcquireKhr,
            timeout, [&] {
                if (auto* internal = internalImagesFor(swapchain)) {
                    return acquireInternalSwapchainImage(swapchain, *internal, timeout,
                        semaphore, fence, imageIndex);
                }
                return acquireAfterDeferredPresent(swapchain, timeout, [&](uint64_t remaining) {
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
        const auto extentResult = bridgeAcquisitionResult(info->swapchain);
        if (extentResult != VK_SUCCESS) return extentResult;
        return present_diagnostics::observeApplicationAcquire(
            device, info->swapchain, present_diagnostics::ApplicationFrameApi::AcquireKhr2,
            info->timeout, [&] {
                if (auto* internal = internalImagesFor(info->swapchain)) {
                    return acquireInternalSwapchainImage(info->swapchain, *internal,
                        info->timeout, info->semaphore, info->fence, imageIndex);
                }
                return acquireAfterDeferredPresent(info->swapchain, info->timeout, [&](uint64_t remaining) {
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
        if (instance_info->scalingSurfaces) {
            instance_info->scalingSurfaces->destroySwapchain(
                surface, swapchain
            );
        }
        if (present_diagnostics::enabled()) {
            std::cerr << "framegen-vulkan: present diagnostics: "
                         "operation=swapchain-destroy-observed"
                      << " role=" << layerRoleName
                      << " swapchain=" << swapchain
                      << " surface=" << surface
                      << " allocation_callbacks=" << (alloc ? 1 : 0)
                      << '\n';
        }
        auto context = layer_info->root.takeSwapchainContext(swapchain);
        instance_info->swapchainInfos.erase(swapchain);
        instance_info->swapchains.erase(swapchain);
        instance_info->nativeSwapchains.erase(swapchain);
        // Removed before the lower destroy. A retired context keeps the object
        // so a pending lower present can still wait its copied semaphore.
        std::shared_ptr<InternalSwapchainImages> internalImages;
        if (const auto found = instance_info->internalSwapchains.find(swapchain);
                found != instance_info->internalSwapchains.end()) {
            internalImages = std::move(found->second);
            instance_info->internalSwapchains.erase(found);
        }

        if (!native && context && context->presentRetirementEnabled()) {
            if (!alloc) {
                const auto now = std::chrono::steady_clock::now();
                bool inserted = false;
                size_t pending = 0;
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
                    pending = instance_info->retiredSwapchains.size();
                }
                if (!inserted) {
                    std::cerr << "framegen-vulkan: duplicate deferred swapchain "
                                 "retirement ignored\n";
                    return;
                }
                if (present_diagnostics::enabled()) {
                    std::cerr << "framegen-vulkan: present diagnostics: "
                                 "operation=swapchain-retirement-deferred"
                              << " role=" << layerRoleName
                              << " swapchain=" << swapchain
                              << " reason=await-later-present-and-fence"
                              << " grace_ms="
                              << swapchainRetirementGracePeriod.count()
                              << " pending="
                              << pending
                              << '\n';
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
                    { "vkEnumerateDeviceExtensionProperties", VKPTR(myvkEnumerateDeviceExtensionProperties) },
                    { "vkSetHdrMetadataEXT", VKPTR(myvkSetHdrMetadataEXT) },
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
                    { "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
                        VKPTR(myvkGetPhysicalDeviceSurfaceCapabilitiesKHR) },
                    { "vkGetPhysicalDeviceSurfaceCapabilities2KHR",
                        VKPTR(myvkGetPhysicalDeviceSurfaceCapabilities2KHR) },
                    { "vkGetPhysicalDeviceSurfaceFormatsKHR",
                        VKPTR(myvkGetPhysicalDeviceSurfaceFormatsKHR) },
                    { "vkGetPhysicalDeviceSurfaceFormats2KHR",
                        VKPTR(myvkGetPhysicalDeviceSurfaceFormats2KHR) },
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

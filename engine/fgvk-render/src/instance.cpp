/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "instance.hpp"

#include "device_selection.hpp"
#include "layer_role.hpp"
#include "pnext_chain.hpp"
#include "present_diagnostics.hpp"
#include "swapchain/create_policy.hpp"
#include "swapchain/output_margin.hpp"
#include "swapchain/swapchain.hpp"

#include "fgvk-common/configuration/detection.hpp"
#include "fgvk-common/helpers/errors.hpp"
#include "fgvk-common/helpers/paths.hpp"
#include "fgvk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <stdlib.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>

using namespace fgvk;
using namespace fgvk::layer;

#ifndef FGVK_BUILD_VERSION
#define FGVK_BUILD_VERSION "unknown"
#endif
#ifndef FGVK_BUILD_FINGERPRINT
#define FGVK_BUILD_FINGERPRINT FGVK_BUILD_VERSION
#endif

namespace {
    constexpr char fgvkBuildIdentity[] =
        "framegen-vulkan: render layer active; identity="
        "VK_LAYER_FGVK_render; build="
        FGVK_BUILD_VERSION
        "; fingerprint="
        FGVK_BUILD_FINGERPRINT
        "; role=frame-generation";

    std::string diagnosticToken(const std::string_view raw) {
        if (raw.empty())
            return "(unset)";

        const auto separator = raw.find_last_of("/\\");
        std::string token(raw.substr(
            separator == std::string_view::npos ? 0 : separator + 1
        ));
        for (char& character : token) {
            if (character == ' ' || character == '\t' ||
                    character == '\r' || character == '\n' ||
                    character == '=') {
                character = '_';
            }
        }
        return token.empty() ? "(unset)" : token;
    }

    std::string_view identificationToken(const ls::IdentType type) {
        switch (type) {
            case ls::IdentType::OVERRIDE:
                return "override";
            case ls::IdentType::EXECUTABLE:
                return "executable";
            case ls::IdentType::WINE_EXECUTABLE:
                return "wine-executable";
            case ls::IdentType::PROCESS_NAME:
                return "process-name";
            case ls::IdentType::FALLBACK:
                return "fallback";
        }
        return "unknown";
    }

    std::string toHexId(const uint32_t id) {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string value = "0x0000";
        value.at(2) = digits[(id >> 12U) & 0xFU];
        value.at(3) = digits[(id >> 8U) & 0xFU];
        value.at(4) = digits[(id >> 4U) & 0xFU];
        value.at(5) = digits[id & 0xFU];
        return value;
    }

    PhysicalDeviceIdentity identifyApplicationDevice(const vk::Vulkan& vk) {
        const auto& funcs = vk.fi();
        const auto physicalDevice = vk.physdev();

        uint32_t extensionCount{};
        funcs.EnumerateDeviceExtensionProperties(
            physicalDevice, nullptr, &extensionCount, VK_NULL_HANDLE
        );
        std::vector<VkExtensionProperties> extensions(extensionCount);
        funcs.EnumerateDeviceExtensionProperties(
            physicalDevice, nullptr, &extensionCount, extensions.data()
        );
        const bool hasPciInfo = std::ranges::find_if(
            extensions,
            [](const VkExtensionProperties& extension) {
                return std::string(extension.extensionName)
                    == VK_EXT_PCI_BUS_INFO_EXTENSION_NAME;
            }
        ) != extensions.end();

        VkPhysicalDevicePCIBusInfoPropertiesEXT pciInfo{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT
        };
        VkPhysicalDeviceProperties2 properties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = hasPciInfo && funcs.GetPhysicalDeviceProperties2
                ? &pciInfo : nullptr
        };
        if (funcs.GetPhysicalDeviceProperties2) {
            funcs.GetPhysicalDeviceProperties2(physicalDevice, &properties);
        } else {
            funcs.GetPhysicalDeviceProperties(
                physicalDevice, &properties.properties
            );
        }

        std::array<char, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE> deviceName{};
        std::copy_n(
            properties.properties.deviceName,
            deviceName.size() - 1,
            deviceName.begin()
        );

        return {
            .name = deviceName.data(),
            .vendorId = toHexId(properties.properties.vendorID),
            .deviceId = toHexId(properties.properties.deviceID),
            .pci = hasPciInfo && funcs.GetPhysicalDeviceProperties2
                ? std::optional<std::string>{
                    std::to_string(pciInfo.pciBus) + ":" +
                    std::to_string(pciInfo.pciDevice) + "." +
                    std::to_string(pciInfo.pciFunction)
                }
                : std::nullopt,
        };
    }

    class ScopedEnvironmentOverride {
    public:
        ScopedEnvironmentOverride(std::string name, const char* value) :
                name(std::move(name)) {
            if (const char* current = std::getenv(this->name.c_str()))
                this->previousValue = current;

            if (setenv(this->name.c_str(), value, 1) != 0)
                throw ls::error("unable to set environment override: " + this->name);
        }

        ~ScopedEnvironmentOverride() {
            if (this->previousValue)
                static_cast<void>(setenv(
                    this->name.c_str(), this->previousValue->c_str(), 1
                ));
            else
                static_cast<void>(unsetenv(this->name.c_str()));
        }

        ScopedEnvironmentOverride(const ScopedEnvironmentOverride&) = delete;
        ScopedEnvironmentOverride& operator=(const ScopedEnvironmentOverride&) = delete;
        ScopedEnvironmentOverride(ScopedEnvironmentOverride&&) = delete;
        ScopedEnvironmentOverride& operator=(ScopedEnvironmentOverride&&) = delete;

    private:
        std::string name;
        std::optional<std::string> previousValue;
    };

    /// The one Gamescope check: a process started inside a Gamescope session
    /// is outside framegen-vulkan's supported environments.
    bool gamescopeSessionDetected() {
        const char* gamescopeDisplay = std::getenv("GAMESCOPE_WAYLAND_DISPLAY");
        if (gamescopeDisplay && *gamescopeDisplay)
            return true;
        return environmentFlagEnabled(std::getenv("ENABLE_GAMESCOPE_WSI")) ||
            environmentFlagEnabled(std::getenv("STEAM_GAMESCOPE_HDR_SUPPORTED"));
    }

    /// helper function to add required extensions
    std::vector<const char*> add_extensions(const char* const* existingExtensions, size_t count,
            const std::vector<const char*>& requiredExtensions) {
        std::vector<const char*> extensions(count);
        std::copy_n(existingExtensions, count, extensions.data());

        for (const auto& requiredExtension : requiredExtensions) {
            auto it = std::ranges::find_if(extensions,
                [requiredExtension](const char* extension) {
                    return std::string(extension) == std::string(requiredExtension);
                });
            if (it == extensions.end())
                extensions.push_back(requiredExtension);
        }

        return extensions;
    }
}


Root::Root() {
    this->fp16AtStartup = this->config.get().global().allow_fp16;
    // S8b: restart-bound, before any device or worker reads realOffsetAuto().
    fgvk::layer::realOffsetAutoConfigured.store(this->config.get().global().pacing_correction,
        std::memory_order_relaxed);
    std::cerr << fgvkBuildIdentity << '\n';

    // H3: read for every process, before the gamescope and profile gates. A Wine
    // process has a mapped Windows executable; the native Wayland colour mapping reads this flag.
    const auto identification = ls::identify();
    this->wineProcessAtStartup = identification.wine_executable.has_value() &&
        !identification.wine_executable->empty();

    const bool gamescopeSession = gamescopeSessionDetected();
    this->gamescopeSessionAtStartup = gamescopeSession;
    if (gamescopeSessionBlocked(gamescopeSession, std::getenv("FGVK_GAMESCOPE"))) {
        std::cerr << "framegen-vulkan: Gamescope session: frame generation off (FGVK_GAMESCOPE=0)\n";
        return;
    }
    if (gamescopeSession) {
        std::cerr << "framegen-vulkan: gamescope session: active (experimental) gamescope_wsi="
                  << (environmentFlagEnabled(std::getenv("ENABLE_GAMESCOPE_WSI")) ? 1 : 0)
                  << '\n';
    }

    // find active profile
    const auto& profile = findProfile(this->config.get(), identification);
    if (present_diagnostics::pacingProfileEnabled()) {
        const auto overlayLibraries = present_diagnostics::steamOverlayLibraries();
        std::ostringstream identity;
        identity << "framegen-vulkan: present diagnostics: "
                     "operation=process-identity"
                  << " pid=" << ::getpid()
                  << " executable="
                  << diagnosticToken(identification.executable)
                  << " wine_executable="
                  << diagnosticToken(
                         identification.wine_executable.value_or("")
                     )
                  << " process_name="
                  << diagnosticToken(identification.process_name)
                  << " profile="
                  << diagnosticToken(
                         profile ? profile->second.name : "(none)"
                     )
                  << " identification="
                  << (profile
                        ? identificationToken(profile->first) : "none")
                  << " build=" << FGVK_BUILD_VERSION
                  << " fingerprint=" << FGVK_BUILD_FINGERPRINT
                  << " steam_overlay_hook_loaded=" << overlayLibraries.hookLoaded
                  << " steam_overlay_vulkan_loaded=" << overlayLibraries.vulkanLoaded
                  << '\n';
        present_diagnostics::writePacingProfileRecords(identity.str());
    }
    if (!profile.has_value()) {
        return;
    }

    this->active_profile = profile->second;
    // Frame Generation provisioning is a process-start choice because LSFG
    // needs application-device interop. The separate execution switch remains
    // live: 0x performs no generation work while 2x-5x can reuse already
    // provisioned backend and private resources in place.
    this->frameGenerationInteropProvisionedAtStartup =
        this->active_profile->frame_generation_provisioned;
    this->swapchainImageCountCompatibilityConfiguredAtStartup =
        this->active_profile->swapchain_image_count_compatibility;

    std::cerr << "framegen-vulkan: using profile with name '" << this->active_profile->name << "' ";
    switch (profile->first) {
        case ls::IdentType::OVERRIDE:
            std::cerr << "(identified via override)\n";
            break;
        case ls::IdentType::EXECUTABLE:
            std::cerr << "(identified via executable)\n";
            break;
        case ls::IdentType::WINE_EXECUTABLE:
            std::cerr << "(identified via wine executable)\n";
            break;
        case ls::IdentType::PROCESS_NAME:
            std::cerr << "(identified via process name)\n";
            break;
        case ls::IdentType::FALLBACK:
            std::cerr << "(identified via fallback)\n";
            break;
    }
    if (this->active_profile->ultra_performance) {
        std::cerr << "framegen-vulkan: Ultra Performance active: flow_scale="
                  << ls::effectiveFlowScale(*this->active_profile)
                  << "; lighter_model="
                  << ls::effectivePerformanceMode(*this->active_profile)
                  << "; resources=active-policy"
                  << "; compatible_profile_reload=live\n";
    }
    if (this->active_profile->swapchain_image_count_compatibility) {
        std::cerr << "framegen-vulkan: Game Swapchain Images compatibility "
                     "active: policy=application-minimum; lifetime=process; "
                     "generated-headroom=disabled\n";
    }
}

ConfigurationUpdateResult Root::update(const bool forceConfigurationPoll) {
    ConfigurationUpdateResult result;
    const auto now = std::chrono::steady_clock::now();

    // Configuration hot reload does not need a filesystem metadata query for
    // every presented frame. Keep the UI responsive while bounding the check
    // to four times per second, matching the feedback sampling cadence.
    constexpr auto configurationPollInterval = std::chrono::milliseconds(250);
    if (!forceConfigurationPoll && this->lastConfigurationPoll &&
            now - *this->lastConfigurationPoll < configurationPollInterval)
        return result;
    this->lastConfigurationPoll = now;

    if (!this->config.update())
        return result;

    result.reloaded = true;
    const auto& currentGlobal = this->config.get().global();
    result.globalChangeDeferred = backendGlobalChangePending(
        this->backendGlobal, currentGlobal
    ) || currentGlobal.allow_fp16 != this->fp16AtStartup;

    const auto previousProfileName = this->active_profile
        ? std::optional<std::string>{this->active_profile->name}
        : std::nullopt;
    const auto& profile = findProfile(this->config.get(), ls::identify());
    auto requestedProfile = profile
        ? std::optional<ls::GameConf>{profile->second}
        : std::nullopt;
    const bool activeUltraPerformance = this->active_profile &&
        this->active_profile->ultra_performance;
    const bool requestedUltraPerformance = requestedProfile &&
        requestedProfile->ultra_performance;

    auto runtimeProfile = requestedProfile;
    bool profileProcessRestartRequired = false;
    bool frameGenerationProvisioningPending = false;
    bool ultraPerformancePending =
        activeUltraPerformance != requestedUltraPerformance;
    bool swapchainImageCountCompatibilityPending = false;
    if (runtimeProfile && this->active_profile) {
        auto projection = projectProcessStaticProfileForLiveUpdate(
            *this->active_profile, *runtimeProfile,
            this->frameGenerationInteropProvisionedAtStartup,
            this->swapchainImageCountCompatibilityConfiguredAtStartup
        );
        *runtimeProfile = std::move(projection.runtimeProfile);
        frameGenerationProvisioningPending =
            projection.frameGenerationProvisioningPending;
        ultraPerformancePending = projection.ultraPerformancePending;
        swapchainImageCountCompatibilityPending =
            projection.swapchainImageCountCompatibilityPending;
        profileProcessRestartRequired = projection.restartRequired() ||
            currentGlobal.allow_fp16 != this->fp16AtStartup;
    } else if (runtimeProfile) {
        if (runtimeProfile->frame_generation_provisioned !=
                this->frameGenerationInteropProvisionedAtStartup) {
            runtimeProfile->frame_generation_provisioned =
                this->frameGenerationInteropProvisionedAtStartup;
            frameGenerationProvisioningPending = true;
            profileProcessRestartRequired = true;
        }
        if (runtimeProfile->swapchain_image_count_compatibility !=
                this->swapchainImageCountCompatibilityConfiguredAtStartup) {
            runtimeProfile->swapchain_image_count_compatibility =
                this->swapchainImageCountCompatibilityConfiguredAtStartup;
            swapchainImageCountCompatibilityPending = true;
            profileProcessRestartRequired = true;
        }
    }
    if (frameGenerationProvisioningPending) {
        std::cerr << "framegen-vulkan: Frame Generation provisioning toggle "
                     "deferred; restart the game to change LSFG device "
                     "interop and backend ownership; 0x and compatible "
                     "profile changes remain live\n";
    }
    if (ultraPerformancePending) {
        profileProcessRestartRequired = true;
        std::cerr << "framegen-vulkan: Ultra Performance toggle deferred; "
                     "restart the game to apply its model and "
                     "resource policy; compatible profile changes remain live\n";
    }
    if (swapchainImageCountCompatibilityPending) {
        std::cerr << "framegen-vulkan: Game Swapchain Images compatibility "
                     "toggle deferred; restart the game to change its WSI "
                     "image-count contract\n";
    }
    if (profileProcessRestartRequired) {
        result.processRestartDeferredContexts += this->swapchains.size();
        result.processProfileChangeDeferred = true;
    }

    this->runtimeStateRevision++;
    if (runtimeProfile)
        this->active_profile = std::move(*runtimeProfile);
    else
        this->active_profile = std::nullopt;

    const auto currentProfileName = this->active_profile
        ? std::optional<std::string>{this->active_profile->name}
        : std::nullopt;
    if (previousProfileName != currentProfileName) {
        std::cerr << "framegen-vulkan: live profile selection changed: previous='"
                  << previousProfileName.value_or("(none)")
                  << "' current='"
                  << currentProfileName.value_or("(none)") << "'";
        if (profile.has_value()) {
            switch (profile->first) {
                case ls::IdentType::OVERRIDE:
                    std::cerr << " source=override";
                    break;
                case ls::IdentType::EXECUTABLE:
                    std::cerr << " source=executable";
                    break;
                case ls::IdentType::WINE_EXECUTABLE:
                    std::cerr << " source=wine-executable";
                    break;
                case ls::IdentType::PROCESS_NAME:
                    std::cerr << " source=process-name";
                    break;
                case ls::IdentType::FALLBACK:
                    std::cerr << " source=fallback";
                    break;
            }
        }
        std::cerr << '\n';
    }

    if (this->active_profile) {
        const auto contextProfile = *this->active_profile;
        for (auto& [swapchain, context] : this->swapchains) {
            static_cast<void>(swapchain);
            const auto update = context.updateProfile(
                contextProfile, this->runtimeStateRevision
            );
            if (update.action == ProfileUpdateAction::ApplyLive)
                result.liveContextsUpdated++;
            if (update.swapchainRecreationDeferred)
                result.swapchainRecreationDeferredContexts++;
            if (update.processRestartDeferred)
                result.processRestartDeferredContexts++;
            if (update.swapchainRecreationRequested)
                result.recreationRequestedContexts++;
        }
        result.processProfileChangeDeferred =
            result.processProfileChangeDeferred ||
            (requestedProfile && backendProfileChangePending(
                this->backendProfile, *requestedProfile
            ));
        if (result.processProfileChangeDeferred) {
            result.processRestartDeferredContexts = std::max(
                result.processRestartDeferredContexts,
                this->swapchains.size()
            );
        }
    } else {
        // Losing the active profile must stop generation immediately, but it
        // does not require destroying any in-flight Vulkan resources.
        for (auto& [swapchain, context] : this->swapchains) {
            static_cast<void>(swapchain);
            context.disableFrameGeneration();
            result.liveContextsUpdated++;
        }
    }

    return result;
}

void Root::modifyInstanceCreateInfo(VkInstanceCreateInfo& createInfo,
        const std::function<void(void)>& finish) const {
    if (!this->frameGenerationInteropProvisioned()) {
        finish();
        return;
    }

    std::vector<const char*> requiredExtensions{"VK_KHR_get_physical_device_properties2"};
    if (this->frameGenerationInteropProvisioned()) {
        requiredExtensions.push_back("VK_KHR_external_memory_capabilities");
        requiredExtensions.push_back("VK_KHR_external_semaphore_capabilities");
    }
    auto extensions = add_extensions(
        createInfo.ppEnabledExtensionNames,
        createInfo.enabledExtensionCount,
        requiredExtensions
    );
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    finish();
}

void Root::modifyDeviceCreateInfo(VkDeviceCreateInfo& createInfo,
        const char* const swapchainMaintenance1Extension,
        const std::function<void(void)>& finish) const {
    const bool frameGenerationInterop = this->frameGenerationInteropProvisioned();
    if (!frameGenerationInterop && !swapchainMaintenance1Extension) {
        finish();
        return;
    }

    std::vector<const char*> requiredExtensions;
    if (frameGenerationInterop) {
        requiredExtensions = {
            "VK_KHR_external_memory",
            "VK_KHR_external_memory_fd",
            "VK_KHR_external_semaphore",
            "VK_KHR_external_semaphore_fd",
            "VK_KHR_timeline_semaphore"
        };
    }
    if (swapchainMaintenance1Extension)
        requiredExtensions.push_back(swapchainMaintenance1Extension);
    auto extensions = add_extensions(
        createInfo.ppEnabledExtensionNames,
        createInfo.enabledExtensionCount,
        requiredExtensions
    );
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    bool timelineFeatureEnabled = false;
    bool swapchainMaintenance1FeatureEnabled = false;
    auto* featureInfo = reinterpret_cast<VkBaseInStructure*>(const_cast<void*>(createInfo.pNext));
    while (featureInfo) {
        if (featureInfo->sType ==
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* features = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(featureInfo);
            if (frameGenerationInterop)
                features->timelineSemaphore = VK_TRUE;
            timelineFeatureEnabled = features->timelineSemaphore == VK_TRUE;
        } else if (featureInfo->sType ==
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            auto* features = reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(featureInfo);
            if (frameGenerationInterop)
                features->timelineSemaphore = VK_TRUE;
            timelineFeatureEnabled = features->timelineSemaphore == VK_TRUE;
        } else if (featureInfo->sType ==
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT) {
            auto* features = reinterpret_cast<
                VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT*>(
                    featureInfo
                );
            if (swapchainMaintenance1Extension)
                features->swapchainMaintenance1 = VK_TRUE;
            swapchainMaintenance1FeatureEnabled =
                features->swapchainMaintenance1 == VK_TRUE;
        }

        featureInfo = const_cast<VkBaseInStructure*>(featureInfo->pNext);
    }

    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
        .pNext = const_cast<void*>(createInfo.pNext),
        .timelineSemaphore = VK_TRUE
    };
    if (frameGenerationInterop && !timelineFeatureEnabled)
        createInfo.pNext = &timelineFeatures;

    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance1Features{
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT,
        .pNext = const_cast<void*>(createInfo.pNext),
        .swapchainMaintenance1 = VK_TRUE,
    };
    if (swapchainMaintenance1Extension &&
            !swapchainMaintenance1FeatureEnabled) {
        createInfo.pNext = &maintenance1Features;
    }

    finish();
}

SwapchainCreateModification Root::modifySwapchainCreateInfo(const vk::Vulkan& vk,
        VkSwapchainCreateInfoKHR& createInfo,
        const bool winePassThroughHdr,
        const std::function<void(void)>& finish) const {
    SwapchainCreateModification modification{
        .applicationExtent = createInfo.imageExtent,
        .presentationExtent = createInfo.imageExtent,
    };
    if (!this->active_profile.has_value()) {
        finish();
        return modification;
    }

    VkSurfaceCapabilitiesKHR caps{}; // NOLINT (enum value 0)
    auto res = vk.fi().GetPhysicalDeviceSurfaceCapabilitiesKHR(
        vk.physdev(), createInfo.surface, &caps);
    if (res != VK_SUCCESS)
        throw ls::vulkan_error(res, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR() failed");

    const auto contextProfile = *this->active_profile;
    modification.swapchainImageCountCompatibility =
        contextProfile.swapchain_image_count_compatibility;
    auto prospectiveCreateInfo = createInfo;
    static_cast<void>(context_ModifySwapchainCreateInfo(
        contextProfile,
        caps.maxImageCount,
        prospectiveCreateInfo,
        vk.frameGenerationInteropEnabled(),
        winePassThroughHdr,
        caps.minImageCount
    ));

    const uint32_t applicationMinImages = createInfo.minImageCount;
    const VkPresentModeKHR applicationPresentMode = createInfo.presentMode;
    modification.privateOrderedTransport = context_ModifySwapchainCreateInfo(
        contextProfile,
        caps.maxImageCount,
        createInfo,
        vk.frameGenerationInteropEnabled(),
        winePassThroughHdr,
        caps.minImageCount
    );

    // Some WSI layers forward both a MAILBOX base mode and a MAILBOX-only
    // maintenance1 compatibility list. The private SDR transport changes the
    // base mode to FIFO, so that list must not reach the lower driver: leaving
    // it attached makes the create description inconsistent and has surfaced
    // as a Wine vkCreateSwapchainKHR assertion. Remove only the outer node from
    // our lower-facing chain. Copy a known prefix so neither the immutable mode
    // array nor any caller-owned pNext link is ever rewritten.
    const FilteredSwapchainCreatePNextChain presentModes(
        createInfo.pNext,
        VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT,
        modification.privateOrderedTransport
    );
    if (!presentModes.valid()) {
        throw ls::error(
            "unsupported VkSwapchainCreateInfoKHR pNext prefix before "
            "present-mode filtering: " +
            std::to_string(presentModes.unsupportedStructureType())
        );
    }
    createInfo.pNext = presentModes.head();

    // Once per swapchain creation: what the layer asks the lower driver for.
    // present_mode_group lists the modes each present may switch between
    // (VK_EXT_swapchain_maintenance1); with a group, present_mode is only
    // the starting mode.
    std::string presentModeGroup = "-";
    for (auto* node = static_cast<const VkBaseInStructure*>(createInfo.pNext); node; node = node->pNext) {
        if (node->sType != VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT)
            continue;
        const auto* group = reinterpret_cast<const VkSwapchainPresentModesCreateInfoEXT*>(node);
        if (!group->pPresentModes || !group->presentModeCount)
            continue;
        presentModeGroup.clear();
        for (uint32_t m = 0; m < group->presentModeCount; ++m)
            presentModeGroup += (m ? "," : "") + std::to_string(static_cast<int64_t>(group->pPresentModes[m]));
    }
    std::cerr << "framegen-vulkan: lower swapchain request: role=" << layerRoleName
              << " present_mode=" << static_cast<int>(createInfo.presentMode)
              << " application_present_mode=" << static_cast<int>(applicationPresentMode)
              << " min_images=" << createInfo.minImageCount
              << " application_min_images=" << applicationMinImages
              << " surface_min_images=" << caps.minImageCount
              << " surface_max_images=" << caps.maxImageCount
              << " present_mode_group=" << presentModeGroup
              << " transport=" << (modification.privateOrderedTransport ? "ordered" : "direct")
              << '\n';

    finish();
    modification.presentationExtent = createInfo.imageExtent;
    return modification;
}

void Root::createSwapchainContext(const vk::Vulkan& vk,
        VkSwapchainKHR swapchain, const SwapchainInfo& info,
        const bool swapchainMaintenance1Enabled) {
    if (!this->active_profile.has_value())
        throw ls::error("attempted to create swapchain context while layer is inactive");
    auto profile = *this->active_profile;
    profile.frame_generation_provisioned =
        this->frameGenerationInteropProvisionedAtStartup;
    const auto& global = this->config.get().global();

    const bool frameGenerationRequested =
        profile.frame_generation_provisioned &&
        profile.frame_generation_enabled;
    const bool frameGenerationAvailableOnDevice =
        vk.frameGenerationInteropEnabled();
    if (frameGenerationAvailableOnDevice &&
            !this->backend.has_value()) { // emplace backend late, due to loader bug
        try {
            // The backend owns a separate Vulkan instance. Prevent framegen-vulkan
            // Renderer or a selected post-process layer from entering that
            // internal instance while preserving caller values for the rest of
            // the game process.
            const ScopedEnvironmentOverride disableFgvk(
                "DISABLE_FGVK", "1"
            );
            const ScopedEnvironmentOverride disableMangoHud(
                "MANGOHUD", "0"
            );
            const ScopedEnvironmentOverride disableVkBasalt(
                "ENABLE_VKBASALT", "0"
            );
            std::string dll{};
            if (global.dll.has_value())
                dll = *global.dll;
            else
                dll = ls::findShaderDll();

            const auto applicationDevice = identifyApplicationDevice(vk);
            if (profile.gpu) {
                std::cerr << "framegen-vulkan: backend GPU selection: configured="
                          << *profile.gpu << '\n';
            } else {
                std::cerr << "framegen-vulkan: backend GPU selection: following game device="
                          << applicationDevice.name << " ("
                          << applicationDevice.vendorId << ":"
                          << applicationDevice.deviceId << ")\n";
            }

            this->backend.emplace(
                [gpu = profile.gpu, applicationDevice](
                    const std::string& deviceName,
                    std::pair<const std::string&, const std::string&> ids,
                    const std::optional<std::string>& pci
                ) {
                    return matchesBackendDevice(
                        {
                            .name = deviceName,
                            .vendorId = ids.first,
                            .deviceId = ids.second,
                            .pci = pci,
                        },
                        gpu,
                        applicationDevice
                    );
                },
                dll, this->fp16AtStartup
            );
            this->backendGlobal = global;
            this->backendGlobal->allow_fp16 = this->fp16AtStartup;
            this->backendProfile = profile;
        } catch (const std::exception& e) {
            if (frameGenerationRequested)
                throw ls::error("failed to create backend instance", e);
        }
    }

    backend::Instance* frameGenerationBackend =
        frameGenerationAvailableOnDevice && this->backend.has_value()
        ? &this->backend.mut() : nullptr;
    if (frameGenerationRequested && !vk.frameGenerationInteropEnabled()) {
        std::cerr << "framegen-vulkan: frame generation was enabled after the "
                     "application device was created without external interop; "
                     "a game restart is required\n";
    }

    auto contextProfile = this->backendProfile
        ? profileForExistingBackend(profile, *this->backendProfile)
        : profile;
    if (!frameGenerationAvailableOnDevice)
        contextProfile.frame_generation_enabled = false;
    this->swapchains.emplace(swapchain,
        Swapchain(vk, frameGenerationBackend, std::move(contextProfile), info,
            this->fp16AtStartup,
            swapchainMaintenance1Enabled));
}

void Root::removeSwapchainContext(VkSwapchainKHR swapchain) {
    static_cast<void>(this->takeSwapchainContext(swapchain));
}

std::optional<Swapchain> Root::takeSwapchainContext(
        const VkSwapchainKHR swapchain) {
    auto context = this->swapchains.extract(swapchain);
    if (context.empty())
        return std::nullopt;
    return std::optional<Swapchain>{std::move(context.mapped())};
}

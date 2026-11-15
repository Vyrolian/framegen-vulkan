/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "fgvk-backend/fgvk.hpp"
#include "fgvk-common/configuration/config.hpp"
#include "fgvk-common/helpers/errors.hpp"
#include "fgvk-common/helpers/pointers.hpp"
#include "fgvk-common/vulkan/vulkan.hpp"
#include "runtime_transition.hpp"
#include "swapchain/swapchain.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

    struct ConfigurationUpdateResult {
        bool reloaded{false};
        size_t liveContextsUpdated{0};
        size_t swapchainRecreationDeferredContexts{0};
        size_t processRestartDeferredContexts{0};
        size_t recreationRequestedContexts{0};
        bool processProfileChangeDeferred{false};
        bool globalChangeDeferred{false};
    };

    struct SwapchainCreateModification {
        bool privateOrderedTransport{false};
        bool swapchainImageCountCompatibility{false};
        VkExtent2D applicationExtent{};
        VkExtent2D presentationExtent{};
    };

    /// Root context of the framegen-vulkan layer.
    class Root {
    public:
        /// create the fgvk root context
        /// @throws ls::error on failure
        Root();

        /// check if the layer is active
        /// @return true if active
        [[nodiscard]] bool active() const { return this->active_profile.has_value(); }

        /// Whether this matched framegen-vulkan process provisioned frame-generation
        /// interop at device creation. Provisioning is independent from the
        /// live 0x execution state so 0x can become active in place.
        [[nodiscard]] bool frameGenerationInteropProvisioned() const {
            return this->frameGenerationInteropProvisionedAtStartup;
        }

        [[nodiscard]] bool fp16RequestedAtStartup() const {
            return this->fp16AtStartup;
        }

        /// Whether the process was started inside a Gamescope session, detected once at layer init.
        [[nodiscard]] bool gamescopeSession() const {
            return this->gamescopeSessionAtStartup;
        }

        /// ensure the layer is up-to-date
        /// @param forceConfigurationPoll bypass the present-path polling
        /// interval at an application-owned swapchain creation boundary
        /// @return classification of any configuration update
        ConfigurationUpdateResult update(bool forceConfigurationPoll = false);

        /// modify instance create info
        /// @param createInfo original create info
        /// @param finish function to call after modification
        void modifyInstanceCreateInfo(VkInstanceCreateInfo& createInfo,
            const std::function<void(void)>& finish) const;
        /// modify device create info
        /// @param createInfo original create info
        /// @param swapchainMaintenance1Extension optional supported spelling
        /// @param finish function to call after modification
        void modifyDeviceCreateInfo(VkDeviceCreateInfo& createInfo,
            const char* swapchainMaintenance1Extension,
            const std::function<void(void)>& finish) const;

        /// modify swapchain create info
        /// @param vk vulkan instance
        /// @param createInfo original create info
        /// @param finish function to call after modification
        [[nodiscard]] SwapchainCreateModification modifySwapchainCreateInfo(
            const vk::Vulkan& vk, VkSwapchainCreateInfoKHR& createInfo,
            const std::function<void(void)>& finish) const;
        /// create swapchain context
        /// @param vk vulkan instance
        /// @param swapchain swapchain handle
        /// @param info swapchain info
        /// @throws ls::error on failure
        void createSwapchainContext(const vk::Vulkan& vk, VkSwapchainKHR swapchain,
            const SwapchainInfo& info,
            bool swapchainMaintenance1Enabled);
        /// get swapchain context
        /// @param swapchain swapchain handle
        /// @return swapchain context
        /// @throws ls::error if not found
        [[nodiscard]] Swapchain& getSwapchainContext(VkSwapchainKHR swapchain) {
            const auto& it = this->swapchains.find(swapchain);
            if (it == this->swapchains.end())
                throw ls::error("swapchain context not found");

            return it->second;
        }
        /// swapchain context, or nullptr for a native or dormant swapchain
        [[nodiscard]] Swapchain* findSwapchainContext(VkSwapchainKHR swapchain) {
            const auto it = this->swapchains.find(swapchain);
            return it == this->swapchains.end() ? nullptr : &it->second;
        }
        /// Remove a context from live configuration updates while preserving
        /// its GPU resources for bounded presentation-fence retirement.
        [[nodiscard]] std::optional<Swapchain> takeSwapchainContext(
            VkSwapchainKHR swapchain);
        /// remove swapchain context
        /// @param swapchain swapchain handle
        void removeSwapchainContext(VkSwapchainKHR swapchain);
    private:
        ls::WatchedConfig config;
        std::optional<ls::GameConf> active_profile;
        bool frameGenerationInteropProvisionedAtStartup{false};
        bool fp16AtStartup{true};
        bool gamescopeSessionAtStartup{false};
        bool swapchainImageCountCompatibilityConfiguredAtStartup{false};

        ls::lazy<backend::Instance> backend;
        std::optional<ls::GlobalConf> backendGlobal;
        std::optional<ls::GameConf> backendProfile;
        std::unordered_map<VkSwapchainKHR, Swapchain> swapchains;
        std::optional<std::chrono::steady_clock::time_point> lastConfigurationPoll;
        uint64_t runtimeStateRevision{1};
    };

}

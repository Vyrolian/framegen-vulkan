/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

// QH2: the generated copy runs on a dedicated transfer-family queue. FGVK_TRANSFER_GENERATED_COPY=1 turns it on, read once
// per process. Unset or any other value changes nothing: no queue, no sharing change and no different command recording.
[[nodiscard]] inline bool transferGeneratedCopyRequested() noexcept {
    static const bool requested = [] {
        const auto* value = std::getenv("FGVK_TRANSFER_GENERATED_COPY");
        return value && std::string_view(value) == "1";
    }();
    return requested;
}

// QH2, diagnostics: 1 once a device was created with its transfer queue (0 otherwise). The per-second summary
// reads it beside each context's own transfer_copy flag.
inline std::atomic<uint32_t> transferCopyApplied{0};

// The transfer family: the first family with TRANSFER and without GRAPHICS or COMPUTE. A family that reports
// timestamps is preferred, but not required. Pure, so the rule can be tested without a device.
[[nodiscard]] inline std::optional<uint32_t> selectTransferFamily(
        const std::span<const VkQueueFamilyProperties> families) noexcept {
    std::optional<uint32_t> fallback;
    for (uint32_t i = 0; i < families.size(); ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_TRANSFER_BIT) == 0 ||
                (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0 ||
                families[i].queueCount == 0)
            continue;
        if (families[i].timestampValidBits > 0)
            return std::optional<uint32_t>{i};
        if (!fallback) fallback = i;
    }
    return std::optional<uint32_t>{fallback};
}

// Owns the changed arrays through lower vkCreateDevice, like PrivatePresentQueueRequest. It requests one queue on the
// transfer family. When the application already requests that family, its entry grows by one: the copied priorities
// and the extra queue takes the last one, and the extra queue's index is the application's count. Otherwise one
// ordinary entry is appended at index 0. Every application entry keeps its indices, flags, priorities and pNext. A
// family that is full, or whose entry has flags, gets no request. Build from the create info that apply() will change.
class TransferQueueRequest {
public:
    TransferQueueRequest(const VkDeviceCreateInfo& info,
            const std::span<const VkQueueFamilyProperties> families) {
        family = selectTransferFamily(families);
        if (!family) return;
        const uint32_t transfer = *family;
        timestamps = families[transfer].timestampValidBits > 0;
        const std::span<const VkDeviceQueueCreateInfo> requests{info.pQueueCreateInfos, info.queueCreateInfoCount};

        uint32_t entries = 0;
        uint32_t entry = 0;
        for (uint32_t i = 0; i < requests.size(); ++i) {
            if (requests[i].queueFamilyIndex == transfer) {
                ++entries;
                entry = i;
            }
        }
        if (entries > 1) return;

        if (entries == 0) {
            queues.assign(requests.begin(), requests.end());
            priorities.assign(1, 1.0F);
            queues.push_back(VkDeviceQueueCreateInfo{
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .queueFamilyIndex = transfer,
                .queueCount = 1,
                .pQueuePriorities = nullptr,
            });
            queues.back().pQueuePriorities = priorities.data();
            index = 0;
            return;
        }

        const auto& requested = requests[entry];
        if (requested.flags != 0 || requested.queueCount == 0 || !requested.pQueuePriorities ||
                requested.queueCount >= families[transfer].queueCount)
            return;
        const std::span<const float> copied{requested.pQueuePriorities, requested.queueCount};
        queues.assign(requests.begin(), requests.end());
        priorities.assign(copied.begin(), copied.end());
        priorities.push_back(priorities.back());
        queues[entry].pQueuePriorities = priorities.data();
        queues[entry].queueCount += 1;
        index = requested.queueCount;
    }

    void apply(VkDeviceCreateInfo& info) const {
        if (!index) return;
        info.queueCreateInfoCount = static_cast<uint32_t>(queues.size());
        info.pQueueCreateInfos = queues.data();
    }

    std::optional<uint32_t> family;
    std::optional<uint32_t> index;
    bool timestamps{false};

private:
    std::vector<VkDeviceQueueCreateInfo> queues;
    std::vector<float> priorities;
};

// QH2: the transfer queue of one device, created beside its DeferredPresentQueue and destroyed with the device, its
// pool first. Every swapchain of the device shares it, so every submit holds `mutex`. The worker takes this mutex
// while it holds its DeferredPresentQueue mutex, and nothing takes them in the other order.
struct TransferCopyQueue {
    VkQueue queue{VK_NULL_HANDLE};
    uint32_t family{};
    uint32_t index{};
    // Copies are timed only when the family reports valid timestamp bits.
    bool timestamps{false};
    VkCommandPool pool{VK_NULL_HANDLE};
    std::mutex mutex;
};

// QH2: the queue families of a lower swapchain whose images are concurrent with the transfer family. The application's
// own list is kept only when it was CONCURRENT. The layer and transfer families follow, each listed once. The caller
// needs at least two families for VK_SHARING_MODE_CONCURRENT.
[[nodiscard]] inline std::vector<uint32_t> transferSharingFamilies(const VkSharingMode sharing,
        const std::span<const uint32_t> applicationFamilies,
        const uint32_t layerFamily, const uint32_t transferFamily) {
    std::vector<uint32_t> families;
    const auto add = [&families](const uint32_t family) {
        if (std::ranges::find(families, family) == families.end())
            families.push_back(family);
    };
    if (sharing == VK_SHARING_MODE_CONCURRENT)
        for (const uint32_t family : applicationFamilies) add(family);
    add(layerFamily);
    add(transferFamily);
    return families;
}

}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <optional>
#include <span>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace fgvk::layer {
// Own the changed arrays through lower vkCreateDevice. Preserve application
// indices, priorities, flags and per-family pNext chains byte-for-byte; append
// exactly one ordinary queue only when the selected family has room.
class PrivatePresentQueueRequest {
public:
    PrivatePresentQueueRequest(const VkDeviceCreateInfo& info,
            std::span<const VkQueueFamilyProperties> families,
            std::optional<uint32_t> family) {
        if (!family || *family >= families.size()) return;
        uint64_t requestedTotal{};
        for (uint32_t i = 0; i < info.queueCreateInfoCount; ++i)
            if (info.pQueueCreateInfos[i].queueFamilyIndex == *family)
                requestedTotal += info.pQueueCreateInfos[i].queueCount;
        if (requestedTotal >= families[*family].queueCount) return;
        for (uint32_t i = 0; i < info.queueCreateInfoCount; ++i) {
            const auto& requested = info.pQueueCreateInfos[i];
            if (requested.queueFamilyIndex != *family || requested.flags ||
                    requested.queueCount >= families[*family].queueCount ||
                    !requested.queueCount || !requested.pQueuePriorities) continue;
            queues.assign(info.pQueueCreateInfos, info.pQueueCreateInfos + info.queueCreateInfoCount);
            priorities.assign(requested.pQueuePriorities,
                requested.pQueuePriorities + requested.queueCount);
            priorities.push_back(priorities.back());
            queues[i].pQueuePriorities = priorities.data();
            queues[i].queueCount++;
            index = requested.queueCount;
            break;
        }
    }
    void apply(VkDeviceCreateInfo& info) const {
        if (index) info.pQueueCreateInfos = queues.data();
    }
    std::optional<uint32_t> index;
private:
    std::vector<VkDeviceQueueCreateInfo> queues;
    std::vector<float> priorities;
};
}

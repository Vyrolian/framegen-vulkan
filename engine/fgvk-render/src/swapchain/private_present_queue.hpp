/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <optional>
#include <span>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace fgvk::layer {
// Own the changed arrays through lower vkCreateDevice. Preserve application
// indices, priorities, flags and per-family pNext chains byte-for-byte. Append
// one ordinary queue for the private present queue when the selected family has
// room. Append a second one for the emulated acquire only when the family has
// room for both; the application never sees either queue.
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
        const uint32_t extra = requestedTotal + 2 <= families[*family].queueCount ? 2u : 1u;
        for (uint32_t i = 0; i < info.queueCreateInfoCount; ++i) {
            const auto& requested = info.pQueueCreateInfos[i];
            if (requested.queueFamilyIndex != *family || requested.flags ||
                    requested.queueCount >= families[*family].queueCount ||
                    !requested.queueCount || !requested.pQueuePriorities) continue;
            queues.assign(info.pQueueCreateInfos, info.pQueueCreateInfos + info.queueCreateInfoCount);
            priorities.assign(requested.pQueuePriorities,
                requested.pQueuePriorities + requested.queueCount);
            // Each extra queue takes the priority of the last requested queue.
            for (uint32_t added = 0; added < extra; ++added)
                priorities.push_back(priorities.back());
            queues[i].pQueuePriorities = priorities.data();
            queues[i].queueCount += extra;
            index = requested.queueCount;
            if (extra == 2) acquireIndex = *index + 1;
            break;
        }
    }
    void apply(VkDeviceCreateInfo& info) const {
        if (index) info.pQueueCreateInfos = queues.data();
    }
    std::optional<uint32_t> index;
    // Second extra queue for the emulated acquire. Empty when only one fits.
    std::optional<uint32_t> acquireIndex;
private:
    std::vector<VkDeviceQueueCreateInfo> queues;
    std::vector<float> priorities;
};
}

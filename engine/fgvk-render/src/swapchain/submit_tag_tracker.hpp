/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

// The game's submission frame tag (VK_NV_low_latency2 explicit submission attribution): the presentID of the
// VkLatencySubmissionPresentIdNV chained to a VkSubmitInfo2, or 0 when the chain has none. The walk is bounded
// like the present chain walk in present_diagnostics.cpp. Every read goes through memcpy: the chain's nodes are
// other structs, and a read through VkBaseInStructure miscompiled under GCC -O2 (type-based aliasing).
[[nodiscard]] inline uint64_t submissionPresentId(const void* chain) noexcept {
    constexpr unsigned maximumDepth = 32;
    for (unsigned depth = 0; chain && depth < maximumDepth; ++depth) {
        VkStructureType type{};
        std::memcpy(&type, chain, sizeof(type));
        if (type == VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV) {
            VkLatencySubmissionPresentIdNV node{};
            std::memcpy(&node, chain, sizeof(node));
            return node.presentID;
        }
        const void* next{};
        std::memcpy(&next, static_cast<const std::byte*>(chain) + offsetof(VkBaseInStructure, pNext), sizeof(next));
        chain = next;
    }
    return 0;
}

// One change of a queue's submission tag: the tag that ended and the counts under it.
struct SubmitTagTransition {
    uint64_t previousTag{};
    // Tagged submits with previousTag, counted from the call that set it.
    uint64_t submits{};
    // Untagged submits since the previous transition on this queue.
    uint64_t untagged{};
};

// Tracks the submission tag of each queue. Only a change of tag returns a transition, so a steady frame costs one
// lock and two counter increments. Queues are few and never removed.
class SubmitTagTracker {
public:
    [[nodiscard]] std::optional<SubmitTagTransition> observe(const VkQueue queue, const uint64_t tag) {
        const std::lock_guard lock(this->mutex);
        QueueTags& state = this->queues[queue];
        if (tag == 0) {
            ++state.untagged;
            return std::nullopt;
        }
        if (tag == state.tag) {
            ++state.submits;
            return std::nullopt;
        }
        const SubmitTagTransition transition{state.tag, state.submits, state.untagged};
        state = QueueTags{.tag = tag, .submits = 1, .untagged = 0};
        return transition;
    }

private:
    struct QueueTags {
        uint64_t tag{};
        uint64_t submits{};
        uint64_t untagged{};
    };
    std::mutex mutex;
    std::map<VkQueue, QueueTags> queues;
};

}

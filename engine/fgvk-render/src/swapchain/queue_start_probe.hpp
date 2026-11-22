/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

// GQ1 counters for the per-second recorder line: frames not measured (no free query) and frames dropped unread
// (pending overflow or results not available within a second).
inline std::atomic<uint64_t> queueFrameSkipped{0};
inline std::atomic<uint64_t> queueFrameDropped{0};

// GQ1 (pacing profile only): when the game's own work of each frame starts and ends on each of its queues. The
// game's submit calls carry two extra command buffers at most: a start timestamp at the head of the call that opens
// a frame on the queue and an end timestamp at the tail of every call. This header holds the pure parts: which
// queries a call writes (QueueFrameRing) and the rewritten submit infos (rewriteSubmitsWithTimestamps).

// One game frame on one queue: the calls tagged with its Reflex ID, and the untagged calls after them (explicit
// submission attribution: an untagged submit belongs to the most recent ID on the same queue).
struct QueueFrame {
    uint64_t presentId{};
    // Calls of this frame that carried its end timestamp.
    uint64_t submits{};
    // CLOCK_MONOTONIC just before the lower vkQueueSubmit2 of the opening call and of the latest call.
    int64_t submitNs{};
    int64_t lastSubmitNs{};
    uint32_t startQuery{};
    // The latest call's end query. Earlier calls' end queries are released when a later call takes a new one.
    uint32_t endQuery{};
};

// The timestamps one submit call carries.
struct QueueFramePlan {
    // Prepend the start timestamp (startQuery) to the first submit info: the call opens a frame.
    bool start{};
    uint32_t startQuery{};
    // Append the end timestamp (endQuery) to the last submit info.
    uint32_t endQuery{};
};

// The frame ring of one queue. Queries are a ring of indices; an index is reused only when no open or pending
// frame holds it. A call that finds its next index held carries nothing and its frame is not reported (skipped).
// Not thread-safe: the owner locks it. Allocates only in the constructor.
class QueueFrameRing {
public:
    static constexpr size_t maximumPending = 64;

    explicit QueueFrameRing(const uint32_t queries) : held(queries, false) {}

    // One submit call with frame tag `tag` (0 when untagged) at `nowNs`. Returns what the call carries, or nothing.
    [[nodiscard]] std::optional<QueueFramePlan> begin(const uint64_t tag, const int64_t nowNs) {
        const bool opens = !this->open || (tag != 0 && tag != this->current.presentId);
        if (opens) {
            this->closeCurrent();
            const auto start = this->take();
            const auto end = start ? this->take() : std::nullopt;
            if (!start || !end) {
                if (start) this->held[*start] = false;
                // The frame is not measured; later calls with the same tag carry nothing either.
                this->current = QueueFrame{.presentId = tag};
                this->open = true;
                this->valid = false;
                ++this->skippedFrames;
                return std::nullopt;
            }
            this->current = QueueFrame{.presentId = tag, .submits = 1, .submitNs = nowNs, .lastSubmitNs = nowNs,
                .startQuery = *start, .endQuery = *end};
            this->open = true;
            this->valid = true;
            return QueueFramePlan{.start = true, .startQuery = *start, .endQuery = *end};
        }
        if (!this->valid) return std::nullopt;
        const auto end = this->take();
        if (!end) {
            // The frame's end would be an earlier call's end: drop the frame.
            this->release(this->current);
            this->valid = false;
            ++this->skippedFrames;
            return std::nullopt;
        }
        this->held[this->current.endQuery] = false;
        this->current.endQuery = *end;
        this->current.lastSubmitNs = nowNs;
        ++this->current.submits;
        return QueueFramePlan{.start = false, .startQuery = this->current.startQuery, .endQuery = *end};
    }

    // The oldest closed frame not yet read or dropped.
    [[nodiscard]] const QueueFrame* oldest() const {
        return this->pendingCount ? &this->pending[this->pendingHead] : nullptr;
    }

    // Releases the oldest closed frame's queries after it was read or dropped.
    void retireOldest() {
        if (!this->pendingCount) return;
        this->release(this->pending[this->pendingHead]);
        this->pendingHead = (this->pendingHead + 1) % maximumPending;
        --this->pendingCount;
    }

    // Frames not measured since the previous call (no free query), and frames dropped unread (pending overflow).
    [[nodiscard]] uint64_t takeSkipped() { return std::exchange(this->skippedFrames, 0); }
    [[nodiscard]] uint64_t takeDropped() { return std::exchange(this->droppedFrames, 0); }
    [[nodiscard]] size_t pendingFrames() const { return this->pendingCount; }

private:
    std::optional<uint32_t> take() {
        if (this->held.empty()) return std::nullopt;
        const auto index = static_cast<uint32_t>(this->next % this->held.size());
        if (this->held[index]) return std::nullopt;
        this->held[index] = true;
        ++this->next;
        return index;
    }

    void release(const QueueFrame& frame) {
        this->held[frame.startQuery] = false;
        this->held[frame.endQuery] = false;
    }

    void closeCurrent() {
        if (!this->open) return;
        this->open = false;
        if (!this->valid) return;
        if (this->pendingCount == maximumPending) {
            // Nothing read them back: drop the oldest.
            this->retireOldest();
            ++this->droppedFrames;
        }
        this->pending[(this->pendingHead + this->pendingCount) % maximumPending] = this->current;
        ++this->pendingCount;
    }

    std::vector<bool> held;
    uint64_t next{};
    QueueFrame current{};
    bool open{};
    bool valid{};
    std::array<QueueFrame, maximumPending> pending{};
    size_t pendingHead{};
    size_t pendingCount{};
    uint64_t skippedFrames{};
    uint64_t droppedFrames{};
};

// The submit call's frame tag: the first nonzero VkLatencySubmissionPresentIdNV of its submit infos, else 0.
template<typename TagOf>
[[nodiscard]] uint64_t submitCallTag(const std::span<const VkSubmitInfo2> submits, TagOf&& tagOf) {
    for (const auto& submit : submits)
        if (const uint64_t tag = tagOf(submit.pNext)) return tag;
    return 0;
}

// Storage of one rewritten submit call. Reused per thread, so steady frames do not allocate.
struct TimestampedSubmits {
    std::vector<VkSubmitInfo2> infos;
    std::vector<VkCommandBufferSubmitInfo> buffers;
};

// Copies `submits` into `out` with `start` (when not null) prepended to the first submit info's command buffers and
// `end` appended to the last one's. pNext, flags, waits and signals stay as they are. The added command buffers use
// the device mask of the info's first command buffer, or 0. Returns false (and leaves `out` unspecified) for an empty
// call.
[[nodiscard]] inline bool rewriteSubmitsWithTimestamps(const std::span<const VkSubmitInfo2> submits,
        const VkCommandBuffer start, const VkCommandBuffer end, TimestampedSubmits& out) {
    if (submits.empty() || end == VK_NULL_HANDLE) return false;
    const size_t first = 0;
    const size_t last = submits.size() - 1;
    size_t total = submits[first].commandBufferInfoCount + 2;
    if (last != first) total += submits[last].commandBufferInfoCount;
    out.infos.assign(submits.begin(), submits.end());
    out.buffers.clear();
    out.buffers.reserve(total);  // no reallocation below: the infos point into it
    const auto mask = [](const VkSubmitInfo2& info) {
        return info.commandBufferInfoCount && info.pCommandBufferInfos ? info.pCommandBufferInfos[0].deviceMask : 0u;
    };
    const auto rebuild = [&](const size_t index, const bool prepend, const bool append) {
        const VkSubmitInfo2& original = submits[index];
        const size_t begin = out.buffers.size();
        const VkCommandBufferSubmitInfo added{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = VK_NULL_HANDLE, .deviceMask = mask(original)};
        if (prepend) {
            out.buffers.push_back(added);
            out.buffers.back().commandBuffer = start;
        }
        for (uint32_t i = 0; original.pCommandBufferInfos && i < original.commandBufferInfoCount; ++i)
            out.buffers.push_back(original.pCommandBufferInfos[i]);
        if (append) {
            out.buffers.push_back(added);
            out.buffers.back().commandBuffer = end;
        }
        out.infos[index].commandBufferInfoCount = static_cast<uint32_t>(out.buffers.size() - begin);
        out.infos[index].pCommandBufferInfos = out.buffers.data() + begin;
    };
    rebuild(first, start != VK_NULL_HANDLE, last == first);
    if (last != first) rebuild(last, false, true);
    return true;
}

}

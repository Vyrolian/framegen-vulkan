/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/present_marker.hpp"
#include "present_diagnostics.hpp"
#include "fgvk-common/vulkan/command_buffer.hpp"
#include "fgvk-common/vulkan/fence.hpp"
#include "fgvk-common/vulkan/gpu_clock.hpp"
#include "fgvk-common/vulkan/timestamp_query_pool.hpp"
#include "fgvk-common/vulkan/vulkan.hpp"

#include <array>
#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>

namespace fgvk::layer {
namespace {

constexpr size_t markerSlots = 32;

struct MarkerSlot {
    std::optional<vk::CommandBuffer> command;
    std::optional<vk::TimestampQueryPool> queries;
    std::optional<vk::Fence> fence;
    bool busy{};
    uint64_t context{}, source{}, feedback{};
    bool generated{};
};

// Every member is used under the private queue's mutex only.
struct PresentMarker {
    VkDevice device{};
    PFN_vkQueueSubmit submit{};
    PFN_vkGetFenceStatus fenceStatus{};
    PFN_vkResetFences resetFences{};
    std::array<MarkerSlot, markerSlots> slots;
    std::optional<vk::CalibratedClock> clock;
    present_diagnostics::Clock::time_point calibratedAt{};

    void emit(const MarkerSlot& slot, const bool available, const int64_t startNs, const int64_t endNs) noexcept {
        present_diagnostics::logPacingPresentMarkerGpu(slot.context, slot.source, slot.feedback,
            slot.generated, available, startNs, endNs);
    }

    void collect() noexcept {
        const auto now = present_diagnostics::Clock::now();
        if (clock && (!clock->calibrated() || now - calibratedAt >= std::chrono::seconds(1))) {
            calibratedAt = now;
            static_cast<void>(clock->recalibrate());
        }
        for (auto& slot : slots) {
            if (!slot.busy) continue;
            const VkFence fence = slot.fence->handle();
            if (fenceStatus(device, fence) != VK_SUCCESS) continue; // still running or lost
            bool available = false;
            int64_t startNs{}, endNs{};
            try {
                // The fence proved this submission's reset and writes done, so the values are its own.
                if (const auto ticks = slot.queries->tryTimestamps(2); ticks && clock && clock->calibrated()) {
                    startNs = clock->monotonicNanoseconds(ticks->at(0));
                    endNs = clock->monotonicNanoseconds(ticks->at(1));
                    available = startNs != 0 && endNs != 0;
                }
            } catch (const std::exception&) {
            }
            emit(slot, available, startNs, endNs);
            if (resetFences(device, 1, &fence) != VK_SUCCESS) {
                slot.fence.reset(); // never reused; the slot stays busy and is skipped
                continue;
            }
            slot.busy = false;
        }
    }
};

}

std::shared_ptr<void> createPresentMarker(const vk::Vulkan& vk, const bool calibration) noexcept {
    if (!present_diagnostics::pacingProfileEnabled() || !present_diagnostics::pacingGpuSplitProfileEnabled())
        return nullptr;
    try {
        auto marker = std::make_shared<PresentMarker>();
        marker->device = vk.dev();
        marker->submit = vk.df().QueueSubmit;
        marker->fenceStatus = vk.df().GetFenceStatus;
        marker->resetFences = vk.df().ResetFences;
        if (!marker->submit || !marker->fenceStatus || !marker->resetFences)
            throw std::runtime_error("missing fence or submit functions");
        for (auto& slot : marker->slots) {
            slot.queries.emplace(vk, 2);
            slot.fence.emplace(vk);
            slot.command.emplace(vk);
            slot.command->begin(vk, 0); // resubmitted as is, never re-recorded
            slot.queries->reset(slot.command->handle());
            slot.queries->write(slot.command->handle(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
            slot.queries->write(slot.command->handle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);
            slot.command->end(vk);
        }
        const auto get = [&](const char* name) { return vk.fi().GetDeviceProcAddr(vk.dev(), name); };
        auto calibrate = calibration
            ? reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(get("vkGetCalibratedTimestampsKHR")) : nullptr;
        if (calibration && !calibrate)
            calibrate = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(get("vkGetCalibratedTimestampsEXT"));
        if (calibrate)
            marker->clock.emplace(vk.dev(), calibrate, marker->slots[0].queries->timestampValidBits(),
                marker->slots[0].queries->timestampPeriodNanoseconds());
        return marker;
    } catch (const std::exception& error) {
        present_diagnostics::reportPacingGpuSplitUnavailable("present-marker", error.what());
    } catch (...) {
        present_diagnostics::reportPacingGpuSplitUnavailable("present-marker", "unknown error");
    }
    return nullptr;
}

void presentMarkerAfterPresent(void* const owner, const VkQueue queue, const uint64_t context,
        const uint64_t source, const uint64_t feedback, const bool generated) noexcept {
    auto& marker = *static_cast<PresentMarker*>(owner);
    marker.collect();
    for (auto& slot : marker.slots) {
        if (slot.busy || !slot.fence) continue;
        slot.context = context;
        slot.source = source;
        slot.feedback = feedback;
        slot.generated = generated;
        const VkCommandBuffer command = slot.command->handle();
        const VkSubmitInfo submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &command,
        };
        // Untailed on purpose: the emulated acquire does not wait this marker.
        if (marker.submit(queue, 1, &submit, slot.fence->handle()) == VK_SUCCESS)
            slot.busy = true;
        else
            marker.emit(slot, false, 0, 0);
        return;
    }
    // Every slot in flight: this present goes unmarked, counted as unavailable.
    MarkerSlot unmarked{};
    unmarked.context = context;
    unmarked.source = source;
    unmarked.feedback = feedback;
    unmarked.generated = generated;
    marker.emit(unmarked, false, 0, 0);
}

}

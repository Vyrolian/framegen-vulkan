/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <cstdint>
#include <span>

#include <vulkan/vulkan_core.h>

namespace vk { class Vulkan; }

namespace fgvk::layer::queue_start {

// What a queue's probe needs from the layer: the application device's wrapper, the queue's family and whether the
// device enabled calibrated timestamps. A null vk skips the queue for good.
struct QueueSource {
    VkDevice device{VK_NULL_HANDLE};
    const vk::Vulkan* vk{};
    uint32_t family{UINT32_MAX};
    bool calibration{};
};
using ResolveQueue = QueueSource (*)(VkQueue queue);

// GQ1, pacing profile only: called by the application's vkQueueSubmit2 wrapper right before the lower call. Returns
// the submit infos to pass down: `submits` with the frame's timestamp command buffers added, or `submits` unchanged.
// The returned storage is thread-local and stays valid until this thread's next call. Reads back earlier frames of
// the queue without waiting and records them. No lock is held when it returns. `resolve` runs once per queue.
[[nodiscard]] std::span<const VkSubmitInfo2> prepare(VkQueue queue, std::span<const VkSubmitInfo2> submits,
    ResolveQueue resolve) noexcept;

// Destroys the probes of a device's queues. Call before the lower vkDestroyDevice; the application has finished
// all work on the device by then.
void destroyDevice(VkDevice device) noexcept;

}

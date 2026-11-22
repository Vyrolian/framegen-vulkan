/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain/queue_start_probes.hpp"

#include "present_diagnostics.hpp"
#include "swapchain/queue_start_probe.hpp"
#include "swapchain/reflex_dynamic_cap.hpp"
#include "swapchain/submit_tag_tracker.hpp"

#include "fgvk-common/vulkan/gpu_clock.hpp"
#include "fgvk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace fgvk::layer::queue_start {
namespace {

using Clock = std::chrono::steady_clock;

// Queries per queue (each a start or an end). A frame holds two; a steady frame with ten submit calls wraps the ring
// in about 25 frames.
constexpr uint32_t ringQueries = 256;
// A GPU time may precede its CPU submit only by the calibration error. A query that reads earlier is a previous
// use's value whose reset has not executed yet.
constexpr int64_t calibrationSlackNs = 2'000'000;
// Results still unavailable this long after the frame's last submit are dropped.
constexpr int64_t unavailableLimitNs = 1'000'000'000;

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

void logOnce(const char* reason) {
    static std::once_flag once;
    std::call_once(once, [reason] {
        std::cerr << "framegen-vulkan: queue frame probe (GQ1) skips a queue: " << reason << '\n';
    });
}

// RDC1: set by the device creation that wants the dynamic cap's queue waits (entrypoint.cpp).
std::atomic<bool> dynamicProbesWanted{false};

class Probe {
public:
    static std::unique_ptr<Probe> create(const VkQueue queue, const QueueSource& source) noexcept {
        try {
            auto probe = std::unique_ptr<Probe>(new Probe(queue, source.family));
            if (!probe->initialize(*source.vk, source.calibration)) return nullptr;
            return probe;
        } catch (...) {
            logOnce("resource creation failed");
            return nullptr;
        }
    }

    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;

    ~Probe() {
        if (this->commandPool != VK_NULL_HANDLE && this->destroyCommandPool)
            this->destroyCommandPool(this->device, this->commandPool, nullptr);
        if (this->queryPool != VK_NULL_HANDLE && this->destroyQueryPool)
            this->destroyQueryPool(this->device, this->queryPool, nullptr);
    }

    // Under the probe's lock: read back closed frames, then plan this call. Returns the command buffers to add.
    struct Added {
        VkCommandBuffer start{VK_NULL_HANDLE};
        VkCommandBuffer end{VK_NULL_HANDLE};
    };
    [[nodiscard]] std::optional<Added> plan(const uint64_t tag) {
        const std::lock_guard lock(this->mutex);
        const auto now = nowNs();
        this->resolve(now);
        const auto plan = this->ring.begin(tag, now);
        if (const auto skipped = this->ring.takeSkipped())
            queueFrameSkipped.fetch_add(skipped, std::memory_order_relaxed);
        if (const auto dropped = this->ring.takeDropped())
            queueFrameDropped.fetch_add(dropped, std::memory_order_relaxed);
        if (!plan) return std::nullopt;
        return Added{.start = plan->start ? this->starts[plan->startQuery] : VK_NULL_HANDLE,
            .end = this->ends[plan->endQuery]};
    }

private:
    Probe(const VkQueue queue, const uint32_t family) : queue(queue), family(family), ring(ringQueries) {}

    bool initialize(const vk::Vulkan& vk, const bool calibration) {
        this->device = vk.dev();
        const auto get = [&](const char* name) { return vk.fi().GetDeviceProcAddr(this->device, name); };
        if (!calibration) {
            logOnce("the device did not enable calibrated timestamps");
            return false;
        }
        auto calibrate = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(get("vkGetCalibratedTimestampsKHR"));
        if (!calibrate)
            calibrate = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(get("vkGetCalibratedTimestampsEXT"));
        uint32_t familyCount{};
        vk.fi().GetPhysicalDeviceQueueFamilyProperties(vk.physdev(), &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vk.fi().GetPhysicalDeviceQueueFamilyProperties(vk.physdev(), &familyCount, families.data());
        if (this->family >= families.size()) {
            logOnce("unknown queue family");
            return false;
        }
        const auto& properties = families[this->family];
        // vkCmdResetQueryPool needs a graphics or compute queue.
        if (!(properties.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
            logOnce("transfer-only queue family");
            return false;
        }
        const uint32_t validBits = properties.timestampValidBits;
        VkPhysicalDeviceProperties device{};
        vk.fi().GetPhysicalDeviceProperties(vk.physdev(), &device);
        const float periodNs = device.limits.timestampPeriod;
        if (validBits == 0 || validBits > 64 || !std::isfinite(periodNs) || periodNs <= 0.0F || !calibrate) {
            logOnce("no timestamps or calibration on this queue family");
            return false;
        }
        this->clock.emplace(this->device, calibrate, validBits, periodNs);
        if (!this->clock->recalibrate()) {
            logOnce("vkGetCalibratedTimestamps failed");
            return false;
        }
        this->calibratedAt = Clock::now();

        const auto createQueryPool = reinterpret_cast<PFN_vkCreateQueryPool>(get("vkCreateQueryPool"));
        this->destroyQueryPool = reinterpret_cast<PFN_vkDestroyQueryPool>(get("vkDestroyQueryPool"));
        this->queryResults = reinterpret_cast<PFN_vkGetQueryPoolResults>(get("vkGetQueryPoolResults"));
        this->dynamic = ReflexDynamicCapRegistry::get(this->device);
        const auto resetQueries = reinterpret_cast<PFN_vkCmdResetQueryPool>(get("vkCmdResetQueryPool"));
        const auto writeTimestamp = reinterpret_cast<PFN_vkCmdWriteTimestamp>(get("vkCmdWriteTimestamp"));
        this->destroyCommandPool = vk.df().DestroyCommandPool;
        if (!createQueryPool || !this->destroyQueryPool || !this->queryResults || !resetQueries || !writeTimestamp ||
                !vk.df().CreateCommandPool || !this->destroyCommandPool || !vk.df().AllocateCommandBuffers ||
                !vk.df().BeginCommandBuffer || !vk.df().EndCommandBuffer) {
            logOnce("missing device functions");
            return false;
        }
        const VkQueryPoolCreateInfo queryInfo{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = ringQueries};
        if (createQueryPool(this->device, &queryInfo, nullptr, &this->queryPool) != VK_SUCCESS) {
            this->queryPool = VK_NULL_HANDLE;
            logOnce("vkCreateQueryPool failed");
            return false;
        }
        const VkCommandPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = this->family};
        if (vk.df().CreateCommandPool(this->device, &poolInfo, nullptr, &this->commandPool) != VK_SUCCESS) {
            this->commandPool = VK_NULL_HANDLE;
            logOnce("vkCreateCommandPool failed");
            return false;
        }
        // Recorded once; the pool frees them with itself.
        std::vector<VkCommandBuffer> buffers(size_t{ringQueries} * 2);
        const VkCommandBufferAllocateInfo allocateInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = this->commandPool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = static_cast<uint32_t>(buffers.size())};
        if (vk.df().AllocateCommandBuffers(this->device, &allocateInfo, buffers.data()) != VK_SUCCESS) {
            logOnce("vkAllocateCommandBuffers failed");
            return false;
        }
        const auto& setLoaderData = vk.loaderdatafunc();
        const VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT};
        for (size_t index = 0; index < buffers.size(); ++index) {
            const auto buffer = buffers[index];
            if (setLoaderData && (*setLoaderData)(this->device, buffer) != VK_SUCCESS) {
                logOnce("vkSetDeviceLoaderData failed");
                return false;
            }
            const auto query = static_cast<uint32_t>(index % ringQueries);
            const bool start = index < ringQueries;
            if (vk.df().BeginCommandBuffer(buffer, &beginInfo) != VK_SUCCESS) {
                logOnce("vkBeginCommandBuffer failed");
                return false;
            }
            resetQueries(buffer, this->queryPool, query, 1);
            writeTimestamp(buffer, start ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                this->queryPool, query);
            if (vk.df().EndCommandBuffer(buffer) != VK_SUCCESS) {
                logOnce("vkEndCommandBuffer failed");
                return false;
            }
        }
        this->starts.assign(buffers.begin(), buffers.begin() + ringQueries);
        this->ends.assign(buffers.begin() + ringQueries, buffers.end());
        return true;
    }

    // One query's value, if available. Never waits.
    std::optional<uint64_t> read(const uint32_t query) const {
        uint64_t values[2]{};
        const auto result = this->queryResults(this->device, this->queryPool, query, 1, sizeof(values), values,
            sizeof(values), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if ((result != VK_SUCCESS && result != VK_NOT_READY) || values[1] == 0) return std::nullopt;
        return values[0];
    }

    // Records closed frames whose queries are available, oldest first; stops at the first one still in flight.
    void resolve(const int64_t now) {
        if (Clock::now() - this->calibratedAt >= std::chrono::seconds(1)) {
            this->calibratedAt = Clock::now();
            static_cast<void>(this->clock->recalibrate());
        }
        while (const auto* frame = this->ring.oldest()) {
            const auto start = this->read(frame->startQuery);
            const auto end = this->read(frame->endQuery);
            if (start && end) {
                const int64_t startNs = this->clock->monotonicNanoseconds(*start);
                const int64_t endNs = this->clock->monotonicNanoseconds(*end);
                if (startNs != 0 && endNs >= startNs && startNs + calibrationSlackNs >= frame->submitNs &&
                        endNs + calibrationSlackNs >= frame->lastSubmitNs) {
                    present_diagnostics::recordQueueFrame(this->queue, this->family, frame->presentId,
                        frame->submits, frame->submitNs, frame->lastSubmitNs, startNs, endNs, true);
                    // RDC1: the controller takes the wait of every available frame; it picks the main queue itself.
                    if (this->dynamic) this->dynamic->feed(reinterpret_cast<uintptr_t>(this->queue), frame->submits,
                        startNs - frame->submitNs, frame->lastSubmitNs);
                    this->ring.retireOldest();
                    continue;
                }
            }
            if (now - frame->lastSubmitNs <= unavailableLimitNs) break;
            present_diagnostics::recordQueueFrame(this->queue, this->family, frame->presentId, frame->submits,
                frame->submitNs, frame->lastSubmitNs, 0, 0, false);
            queueFrameDropped.fetch_add(1, std::memory_order_relaxed);
            this->ring.retireOldest();
        }
    }

    std::mutex mutex;
    VkQueue queue{VK_NULL_HANDLE};
    uint32_t family{};
    VkDevice device{VK_NULL_HANDLE};
    // RDC1: the device's dynamic cap controller, fed by every available frame of every queue of the device.
    std::shared_ptr<ReflexDynamicCap> dynamic;
    QueueFrameRing ring;
    std::optional<vk::CalibratedClock> clock;
    Clock::time_point calibratedAt{};
    VkQueryPool queryPool{VK_NULL_HANDLE};
    VkCommandPool commandPool{VK_NULL_HANDLE};
    std::vector<VkCommandBuffer> starts;
    std::vector<VkCommandBuffer> ends;
    PFN_vkDestroyQueryPool destroyQueryPool{};
    PFN_vkDestroyCommandPool destroyCommandPool{};
    PFN_vkGetQueryPoolResults queryResults{};
};

struct Registry {
    std::mutex mutex;
    struct Entry {
        VkDevice device{VK_NULL_HANDLE};
        // Null when the queue is skipped for good.
        std::unique_ptr<Probe> probe;
    };
    std::unordered_map<VkQueue, Entry> probes;
};

Registry& registry() {
    static auto* value = new Registry();  // leaked: submits may race static destruction at exit
    return *value;
}

Probe* findOrCreate(const VkQueue queue, const ResolveQueue resolve) {
    auto& state = registry();
    const std::lock_guard lock(state.mutex);
    if (const auto found = state.probes.find(queue); found != state.probes.end()) return found->second.probe.get();
    const auto source = resolve(queue);
    auto probe = source.vk ? Probe::create(queue, source) : nullptr;
    if (!source.vk) logOnce("not an application queue of a framegen-vulkan device");
    auto* raw = probe.get();
    state.probes.emplace(queue, Registry::Entry{.device = source.device, .probe = std::move(probe)});
    return raw;
}

}

std::span<const VkSubmitInfo2> prepare(const VkQueue queue, const std::span<const VkSubmitInfo2> submits,
        const ResolveQueue resolve) noexcept {
    try {
        if (submits.empty()) return submits;
        for (const auto& submit : submits)
            if (submit.flags & VK_SUBMIT_PROTECTED_BIT) return submits;
        // The probe lives until destroyDevice, which the application cannot call while it submits on the device.
        auto* probe = findOrCreate(queue, resolve);
        if (!probe) return submits;
        const uint64_t tag = submitCallTag(submits, [](const void* chain) { return submissionPresentId(chain); });
        const auto added = probe->plan(tag);
        if (!added) return submits;
        static thread_local TimestampedSubmits storage;
        if (!rewriteSubmitsWithTimestamps(submits, added->start, added->end, storage)) return submits;
        return storage.infos;
    } catch (...) {
        return submits;
    }
}

void requestDynamicProbes() noexcept {
    dynamicProbesWanted.store(true, std::memory_order_relaxed);
}

bool dynamicProbesRequested() noexcept {
    return dynamicProbesWanted.load(std::memory_order_relaxed);
}

void destroyDevice(const VkDevice device) noexcept {
    try {
        auto& state = registry();
        const std::lock_guard lock(state.mutex);
        std::erase_if(state.probes, [device](const auto& entry) { return entry.second.device == device; });
    } catch (...) {}
}

}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "present_diagnostics.hpp"
#include "generated_frame_plan.hpp"
#include <array>
#include <cmath>
#include <memory>

namespace fgvk::layer::present_diagnostics {

// One source and at most four generated copies, in existing command buffers.
// The swapchain owner proves completion with its existing render fence before
// collecting/reusing this pool. No query uses WAIT_BIT and no policy reads it.
class NativeCopyProfile {
public:
    static constexpr uint32_t capacity = GeneratedFramePlan::capacity + 1;
    struct Functions {
        PFN_vkCreateQueryPool create{};
        PFN_vkDestroyQueryPool destroy{};
        PFN_vkCmdResetQueryPool reset{};
        PFN_vkCmdWriteTimestamp write{};
        PFN_vkGetQueryPoolResults results{};
        PFN_vkGetCalibratedTimestampsKHR calibrate{};
    };
    struct Calibration {
        VkResult result{VK_ERROR_EXTENSION_NOT_PRESENT};
        uint64_t device{}, host{}, deviation{};
        Clock::time_point started{}, finished{};
    };
    struct Sample {
        uint64_t context{}, source{}, generation{};
        VkSwapchainKHR swapchain{};
        VkDevice device{};
        VkQueue queue{};
        VkCommandBuffer command{};
        uint32_t slot{}, image{};
        bool generated{}, available{};
        uint64_t begin{}, end{};
        uint32_t validBits{};
        float period{};
        VkResult queryResult{VK_NOT_READY};
        Clock::time_point collected{}, readStarted{}, readFinished{};
        Calibration before{}, after{};
    };

    NativeCopyProfile(VkDevice device, Functions functions, uint32_t validBits, float period,
            uint64_t context) : device(device), functions(functions), validBits(validBits),
            period(period), context(context) {
        if (!functions.create || !functions.destroy || !functions.reset || !functions.write || !functions.results ||
                !validBits || validBits > 64 || !std::isfinite(period) || period <= 0) return;
        const VkQueryPoolCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = capacity * 2,
        };
        initialization = functions.create(device, &info, nullptr, &pool);
    }
    ~NativeCopyProfile() { if (pool) functions.destroy(device, pool, nullptr); }
    NativeCopyProfile(const NativeCopyProfile&) = delete;
    NativeCopyProfile& operator=(const NativeCopyProfile&) = delete;
    [[nodiscard]] bool ready() const { return pool && initialization == VK_SUCCESS; }
    [[nodiscard]] bool hasCalibration() const { return functions.calibrate != nullptr; }

    // False means the previous batch lacks its owner's completion proof. Do
    // not reset a query that could still be referenced by an in-flight copy.
    bool beginSource(uint64_t source, uint64_t generation, VkSwapchainKHR swapchain) noexcept {
        armed = false;
        if (!ready() || pending) return false;
        this->source = source;
        this->generation = generation;
        this->swapchain = swapchain;
        samples = {};
        ended = {};
        count = 0;
        before = calibration();
        armed = true;
        return true;
    }
    void beginCopy(VkCommandBuffer command, VkQueue queue, uint32_t image, bool generated, uint32_t pass = 0) noexcept {
        if (!armed || !ready() || (pending && !count) || pass >= capacity - 1) return;
        const uint32_t slot = generated ? pass + 1 : 0;
        // Only a source-copy command can initialize a batch. Its existing
        // timeline signals order all subsequent generated-copy submissions.
        if ((!generated && pending) || (generated && (!pending || !count)) || samples[slot].command) return;
        if (!generated) {
            functions.reset(command, pool, 0, capacity * 2);
            pending = true;
        }
        samples[slot] = {.context = context, .source = source, .generation = generation,
            .swapchain = swapchain, .device = device, .queue = queue, .command = command,
            .slot = slot, .image = image, .generated = generated,
            .validBits = validBits, .period = period, .before = before};
        count = std::max(count, slot + 1);
        functions.write(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, slot * 2);
    }
    void endCopy(VkCommandBuffer command, bool generated, uint32_t pass = 0) noexcept {
        if (!armed || !ready() || pass >= capacity - 1) return;
        const uint32_t slot = generated ? pass + 1 : 0;
        if (samples[slot].command != command || !pending || ended[slot]) return;
        functions.write(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, slot * 2 + 1);
        ended[slot] = true;
    }
    // Call only AFTER the existing batch completion fence is known signaled.
    void completed() noexcept {
        if (!pending) return;
        const auto observed = Clock::now();
        struct Query { uint64_t ticks{}, available{}; };
        std::array<Query, capacity * 2> values{};
        const auto readStarted = Clock::now();
        const auto result = functions.results(device, pool, 0, count * 2,
            count * 2 * sizeof(Query), values.data(), sizeof(Query),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        const auto readFinished = Clock::now();
        const auto after = calibration();
        for (uint32_t slot = 0; slot < count; ++slot) {
            auto& sample = samples[slot];
            if (!sample.command) continue;
            sample.queryResult = result;
            sample.available = (result == VK_SUCCESS || result == VK_NOT_READY) &&
                values[slot * 2].available && values[slot * 2 + 1].available;
            if (sample.available) {
                sample.begin = values[slot * 2].ticks;
                sample.end = values[slot * 2 + 1].ticks;
            }
            sample.collected = observed;
            sample.readStarted = readStarted;
            sample.readFinished = readFinished;
            sample.after = after;
            publish(sample);
        }
        pending = false;
        armed = false;
    }
    static void publish(const Sample& sample) noexcept;
private:
    Calibration calibration() const noexcept {
        Calibration result;
        if (!functions.calibrate) return result;
        const VkCalibratedTimestampInfoKHR clocks[]{
            {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, VK_TIME_DOMAIN_DEVICE_KHR},
            {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR},
        };
        std::array<uint64_t, 2> values{};
        result.started = Clock::now();
        result.result = functions.calibrate(device, 2, clocks, values.data(), &result.deviation);
        result.finished = Clock::now();
        if (result.result == VK_SUCCESS) { result.device = values[0]; result.host = values[1]; }
        return result;
    }
    VkDevice device{};
    Functions functions;
    uint32_t validBits{};
    float period{};
    uint64_t context{}, source{}, generation{};
    VkSwapchainKHR swapchain{};
    VkQueryPool pool{};
    VkResult initialization{VK_ERROR_FEATURE_NOT_PRESENT};
    bool pending{}, armed{};
    uint32_t count{};
    Calibration before{};
    std::array<Sample, capacity> samples{};
    std::array<bool, capacity> ended{};
};
}

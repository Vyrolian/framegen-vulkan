/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "mako-common/vulkan/buffer.hpp"
#include "mako-common/vulkan/image.hpp"
#include "mako-common/vulkan/vulkan.hpp"
#include <array>
#include <charconv>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace mako::layer {

// Diagnostic-only image readback. Records into existing copy commands and
// reads only after the owner's existing render fence proves completion.
class FrameContentCapture {
public:
    // Process-start diagnostic budget; default preserves the version-1 clip.
    static std::optional<uint64_t> sourceLimit(std::string_view value) noexcept {
        if (value.empty()) return 128;
        uint64_t count{};
        const auto parsed = std::from_chars(value.data(), value.data()+value.size(), count);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data()+value.size() || count < 2 || count > 4096)
            return std::nullopt;
        return count;
    }
    static std::shared_ptr<FrameContentCapture> create(uint64_t context) noexcept;
    ~FrameContentCapture();
    void beginSource(const vk::Vulkan& vk, VkFormat format, uint64_t source,
        uint64_t backendFrame, VkQueue callerQueue, VkQueue privatePresentQueue) noexcept;
    void record(const vk::Vulkan& vk, VkCommandBuffer command, VkImage image,
        VkExtent2D extent, uint32_t imageIndex, bool generated) noexcept;
    void completed(const vk::Vulkan& vk) noexcept;
private:
    explicit FrameContentCapture(uint64_t context);
    void announce(const std::string& fields) noexcept;
    void write(const void* data, size_t size);
    static constexpr uint32_t width = 192, height = 108;
    static constexpr size_t imageBytes = width * height * 4;
    uint64_t maxSources{128};
    uint64_t context{}, source{}, backendFrame{}, callerQueue{}, privateQueue{}, batches{};
    int fd{-1};
    bool disabled{}, active{};
    std::string path, trigger;
    std::chrono::steady_clock::time_point pollAfter{};
    std::optional<vk::Image> reduced;
    std::optional<vk::Buffer> staging;
    // Magic, version, context, source, backend frame, role, image index,
    // image handle, CPU record/completion time, copy/caller/private queues,
    // width, height, byte count, capture source ordinal. All little-endian u64.
    std::array<std::array<uint64_t, 17>, 2> records{};
    std::array<bool, 2> pending{};
};
}

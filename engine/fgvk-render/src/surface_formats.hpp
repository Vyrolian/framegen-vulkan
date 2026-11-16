/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

// Pure rules behind the surface format log (entrypoint.cpp). Nothing here calls Vulkan. The
// queries themselves are pass-throughs: the list, count and VkResult the game receives are the
// lower driver's, and this header only decides what is printed about them.

// One (format, colour space) pair, as the application receives it from a surface format query.
struct SurfaceFormatPair {
    uint32_t format{0};
    uint32_t colorSpace{0};
};

// At most this many distinct lists are logged per process.
inline constexpr size_t maxLoggedSurfaceFormatLists = 8;

// 64-bit FNV-1a key of one list, built in a streaming way so a query answer can be keyed straight
// from the driver's array without copying it. The count goes in first, so a list and its prefix
// never share a key, and the order of pairs matters.
class SurfaceFormatListKey {
public:
    explicit constexpr SurfaceFormatListKey(const uint32_t count) noexcept {
        mix(count);
    }

    constexpr void add(const SurfaceFormatPair pair) noexcept {
        mix(pair.format);
        mix(pair.colorSpace);
    }

    [[nodiscard]] constexpr uint64_t value() const noexcept {
        return hash;
    }

private:
    constexpr void mix(const uint32_t word) noexcept {
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            hash ^= (word >> shift) & 0xffU;
            hash *= 0x100000001b3ULL;
        }
    }

    uint64_t hash{0xcbf29ce484222325ULL};
};

[[nodiscard]] constexpr uint64_t surfaceFormatListHash(std::span<const SurfaceFormatPair> pairs) noexcept {
    SurfaceFormatListKey key(static_cast<uint32_t>(pairs.size()));
    for (const auto& pair : pairs)
        key.add(pair);
    return key.value();
}

// "count=N pairs=F:C,F:C" in decimal, the same numeric form as the layer's colour pipeline line.
[[nodiscard]] inline std::string surfaceFormatListLine(std::span<const SurfaceFormatPair> pairs) {
    std::string line = "count=" + std::to_string(pairs.size()) + " pairs=";
    for (size_t index = 0; index < pairs.size(); ++index) {
        if (index != 0)
            line += ',';
        line += std::to_string(pairs[index].format);
        line += ':';
        line += std::to_string(pairs[index].colorSpace);
    }
    return line;
}

// Only a complete answer is logged: VK_SUCCESS with an array. A count-only query has no formats,
// and VK_INCOMPLETE returns a truncated prefix that the game did not ask to see in full.
[[nodiscard]] constexpr bool logsSurfaceFormatQuery(const VkResult result, const bool arrayReturned) noexcept {
    return result == VK_SUCCESS && arrayReturned;
}

// Process-wide dedupe of logged lists. A list that was already logged, or a process at the cap, is
// answered without the mutex. The mutex covers only the insert of a new list, re-checked under the lock.
class SurfaceFormatListLog {
public:
    // True for the first call that sees this hash while fewer than the cap are logged.
    [[nodiscard]] bool claim(const uint64_t hash) {
        if (full() || contains(hash))
            return false;
        const std::lock_guard<std::mutex> lock(mutex);
        if (full() || contains(hash))
            return false;
        const size_t count = logged.load(std::memory_order_relaxed);
        hashes[count].store(hash, std::memory_order_relaxed);
        logged.store(count + 1, std::memory_order_release);
        return true;
    }

private:
    [[nodiscard]] bool full() const noexcept {
        return logged.load(std::memory_order_acquire) >= maxLoggedSurfaceFormatLists;
    }

    [[nodiscard]] bool contains(const uint64_t hash) const noexcept {
        const size_t count = logged.load(std::memory_order_acquire);
        for (size_t index = 0; index < count; ++index) {
            if (hashes[index].load(std::memory_order_relaxed) == hash)
                return true;
        }
        return false;
    }

    std::mutex mutex;
    std::atomic<size_t> logged{0};
    std::array<std::atomic<uint64_t>, maxLoggedSurfaceFormatLists> hashes{};
};

} // namespace fgvk::layer

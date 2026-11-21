/* SPDX-License-Identifier: GPL-3.0-or-later */

// GP2 partitioning: runs are in-order, maximal and split only by the independence helper.
// The helper is the only authority, so these tests use synthetic barrier lists and no Vulkan device.

#include "helpers/independent_dispatch.hpp"
#include "fgvk-common/vulkan/backend_overlap.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace {
    void require(const bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    // One barrier as a ManagedShader builds it: GENERAL to GENERAL, queue families ignored.
    VkImageMemoryBarrier access(uintptr_t image, VkAccessFlags role) {
        return {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = role,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = reinterpret_cast<VkImage>(image),
        };
    }
    VkImageMemoryBarrier reads(uintptr_t image) { return access(image, VK_ACCESS_SHADER_READ_BIT); }
    VkImageMemoryBarrier writes(uintptr_t image) { return access(image, VK_ACCESS_SHADER_WRITE_BIT); }

    // Exclusive run ends for a dispatch list, one barrier group per dispatch.
    std::vector<size_t> ends(const std::vector<std::vector<VkImageMemoryBarrier>>& dispatches) {
        std::vector<std::span<const VkImageMemoryBarrier>> spans;
        spans.reserve(dispatches.size());
        for (const auto& dispatch : dispatches) spans.emplace_back(dispatch);
        return fgvk::backend::independentRunEnds(spans);
    }

    void testEmptyAndSingle() {
        require(ends({}).empty(), "empty list has runs");
        require(ends({{writes(1)}}) == std::vector<size_t>{1}, "single dispatch is not one run");
    }

    void testAllIndependentIsOneRun() {
        // Shared immutable input, disjoint outputs: one barrier for the whole list.
        require(ends({{reads(1)}, {reads(1), writes(2)}, {writes(3)}, {reads(4)}})
                == std::vector<size_t>{4}, "independent list did not form one run");
    }

    void testDependentPairSplitsInOrder() {
        // [0,1] are independent; 2 reads what 1 writes, so it starts a run; 3 joins it.
        require(ends({{reads(9)}, {writes(2)}, {reads(2)}, {reads(3)}})
                == std::vector<size_t>{2, 4}, "dependent pair in the middle did not split in order");
        // Three runs: 0 | 1,2 | 3, where 3 reads what 2 writes.
        require(ends({{writes(1)}, {reads(1)}, {writes(2)}, {reads(2)}})
                == std::vector<size_t>{1, 3, 4}, "three-run partition is wrong");
    }

    void testChainIsSingletons() {
        require(ends({{writes(1)}, {reads(1), writes(2)}, {reads(2), writes(3)}})
                == std::vector<size_t>{1, 2, 3}, "dependent chain did not stay serial");
    }

    void testLaterDependenceSplitsOnlyWhereNeeded() {
        // 2 depends on 0, not on 1, but the run [0,1] already holds 0, so 2 starts a new run.
        require(ends({{writes(1)}, {writes(2)}, {reads(1)}})
                == std::vector<size_t>{2, 3}, "non-adjacent dependence was missed");
    }

    void testWriteConflicts() {
        require(ends({{writes(1)}, {writes(1)}}) == std::vector<size_t>{1, 2},
            "write-after-write was grouped");
        require(ends({{reads(1)}, {reads(1)}}) == std::vector<size_t>{2},
            "shared reads were split");
        require(ends({{reads(1)}, {writes(1)}}) == std::vector<size_t>{1, 2},
            "write-after-read was grouped");
    }

    void testInvalidDispatchNeverShares() {
        // A null image is not independent of anything, so it always stands alone.
        require(ends({{reads(1)}, {access(0, VK_ACCESS_SHADER_READ_BIT)}, {reads(2)}})
                == std::vector<size_t>{1, 2, 3}, "null-image dispatch shared a run");
    }

    void testRunsCoverEveryDispatchInOrder() {
        const std::vector<std::vector<VkImageMemoryBarrier>> dispatches{
            {reads(1)}, {writes(2)}, {reads(2)}, {writes(3)}, {reads(3)}, {reads(4)}};
        const auto runEnds = ends(dispatches);
        require(!runEnds.empty() && runEnds.back() == dispatches.size(),
            "runs do not cover the list");
        size_t begin = 0;
        for (const size_t end : runEnds) {
            require(end > begin, "a run is empty or runs are out of order");
            std::vector<std::span<const VkImageMemoryBarrier>> run;
            for (size_t i = begin; i < end; ++i) run.emplace_back(dispatches[i]);
            require(run.size() == 1 || fgvk::backend::independentComputeDispatches(run),
                "a run holds dependent dispatches");
            begin = end;
        }
    }

    void testOverlapSwitchParsesExactlyOne() {
        require(!vk::parseBackendOverlap(nullptr), "unset switch enabled overlap");
        require(vk::parseBackendOverlap("1"), "FGVK_BACKEND_OVERLAP=1 not enabled");
        require(!vk::parseBackendOverlap("0"), "FGVK_BACKEND_OVERLAP=0 enabled");
        require(!vk::parseBackendOverlap(""), "empty switch enabled overlap");
        require(!vk::parseBackendOverlap("true"), "non-numeric switch enabled overlap");
        require(!vk::parseBackendOverlap("1 "), "trailing text enabled overlap");
    }
}

int main() {
    try {
        testEmptyAndSingle();
        testAllIndependentIsOneRun();
        testDependentPairSplitsInOrder();
        testChainIsSingletons();
        testLaterDependenceSplitsOnlyWhereNeeded();
        testWriteConflicts();
        testInvalidDispatchNeverShares();
        testRunsCoverEveryDispatchInOrder();
        testOverlapSwitchParsesExactlyOne();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Dispatch group partitioning checks passed\n";
    return 0;
}

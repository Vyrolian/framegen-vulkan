/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "pnext_chain.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

using namespace fgvk::layer;

namespace {
    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }

    // The production walks copy each pNext link with memcpy. For a local node
    // reached only that way, GCC at -O2/-O3 can lose track of the pointer
    // and drop the stores that initialize the node. Publishing the node to an
    // opaque volatile sink makes it escape without changing its contents.
    const void* volatile publishedChainNode{};
    void publishChainNode(const void* node) {
        publishedChainNode = node;
    }

    void testGeneratedOutputMetadata() {
        constexpr uint64_t presentIds[]{1234};
        constexpr VkPresentModeKHR modes[]{VK_PRESENT_MODE_FIFO_KHR};
        const VkPresentRegionsKHR regions{
            .sType = VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR,
        };
        const VkSwapchainPresentFenceInfoEXT fence{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
            .pNext = &regions,
        };
        const VkSwapchainPresentModeInfoEXT mode{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT,
            .pNext = &fence, .swapchainCount = 1, .pPresentModes = modes,
        };
        const VkPresentTimesInfoGOOGLE timing{
            .sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE,
            .pNext = &mode,
        };
        const VkPresentIdKHR id{
            .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
            .pNext = &timing, .swapchainCount = 1, .pPresentIds = presentIds,
        };
        publishChainNode(&mode);
        const GeneratedPresentPNextChain generated(&id);
        const auto* copied = static_cast<const VkSwapchainPresentModeInfoEXT*>(generated.head());
        expect(copied != nullptr, "generated output lost its persistent mode override");
        expect(copied != &mode, "generated output borrowed the caller mode node");
        expect(copied->sType == mode.sType, "generated mode type changed");
        expect(copied->pNext == nullptr, "generated mode retained caller frame metadata");
        expect(copied->swapchainCount == 1, "generated mode count changed");
        expect(copied->pPresentModes == modes, "generated mode array changed");
        expect(id.pNext == &timing && timing.pNext == &mode && mode.pNext == &fence &&
                fence.pNext == &regions && id.pPresentIds[0] == 1234,
            "caller real-output metadata must remain immutable and complete");
        const VkPresentIdKHR idOnly{
            .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
            .pNext = &fence, .swapchainCount = 1, .pPresentIds = presentIds,
        };
        expect(!GeneratedPresentPNextChain(&idOnly).head() &&
                !GeneratedPresentPNextChain(nullptr).head(),
            "generated output must not acquire a caller completion ID or fence");
#if defined(VK_KHR_present_id2)
        const VkPresentId2KHR id2{
            .sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
            .pNext = &mode, .swapchainCount = 1, .pPresentIds = presentIds,
        };
        const GeneratedPresentPNextChain generated2(&id2);
        expect(generated2.head() && static_cast<const VkSwapchainPresentModeInfoEXT*>(
                generated2.head())->pNext == nullptr && id2.pNext == &mode,
            "ID2 metadata must also remain solely on the caller real image");
#endif
    }

#if defined(VK_KHR_present_id2)
    void testPresentId2Prefix() {
        constexpr uint64_t ids[]{0x123456789abcdef0ULL};
        constexpr VkPresentModeKHR modes[]{VK_PRESENT_MODE_FIFO_KHR};
        const VkSwapchainPresentFenceInfoEXT fence{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
        };
        const VkPresentRegionsKHR regions{
            .sType = VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR,
            .pNext = &fence,
        };
        const VkSwapchainPresentModeInfoEXT mode{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT,
            .pNext = &regions,
            .swapchainCount = 1,
            .pPresentModes = modes,
        };
        const VkPresentId2KHR id{
            .sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
            .pNext = &mode,
            .swapchainCount = 1,
            .pPresentIds = ids,
        };
        const FilteredPresentPNextChain passthrough(&id, false);
        expect(passthrough.valid() && passthrough.head() == &id,
            "present ID2 prefix without a mode removal was copied");
        const FilteredPresentPNextChain filtered(&id, true);
        expect(filtered.valid(), "present ID2 prefix was rejected");
        const auto* copied = static_cast<const VkPresentId2KHR*>(
            filtered.head());
        expect(copied != &id && copied->sType == id.sType &&
                copied->swapchainCount == 1 && copied->pPresentIds == ids,
            "present ID2 filtering lost its count or borrowed IDs");
        expect(copied->pNext == &regions && id.pNext == &mode &&
                mode.pNext == &regions && regions.pNext == &fence &&
                ids[0] == 0x123456789abcdef0ULL,
            "present ID2 filtering changed caller data or the retained suffix");
    }
#endif

#if defined(VK_KHR_present_id2) && defined(VK_KHR_present_wait2)
    void testPresentId2Wait2Enablement() {
        // Feature structs carry a mutable void* pNext, so these stay non-const.
        VkPhysicalDevicePresentWait2FeaturesKHR wait2{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR,
            .presentWait2 = VK_TRUE,
        };
        VkPhysicalDevicePresentId2FeaturesKHR id2{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR,
            .pNext = &wait2,
            .presentId2 = VK_TRUE,
        };
        publishChainNode(&wait2);
        publishChainNode(&id2);
        expect(enablesApplicationPresentId2Wait2(&id2),
            "enabled present ID2 and wait2 features were not recognized");

        wait2.presentWait2 = VK_FALSE;
        expect(!enablesApplicationPresentId2Wait2(&id2),
            "a disabled wait2 feature was accepted");
        wait2.presentWait2 = VK_TRUE;
        id2.presentId2 = VK_FALSE;
        expect(!enablesApplicationPresentId2Wait2(&id2),
            "a disabled present ID2 feature was accepted");
        id2.presentId2 = VK_TRUE;

        expect(!enablesApplicationPresentId2Wait2(&wait2),
            "wait2 alone was accepted");
        expect(!enablesApplicationPresentId2Wait2(nullptr),
            "an absent feature chain was accepted");
        id2.pNext = nullptr;
        expect(!enablesApplicationPresentId2Wait2(&id2),
            "present ID2 alone was accepted without wait2");

        VkPhysicalDeviceTimelineSemaphoreFeatures unrelated{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
            .pNext = &wait2,
            .timelineSemaphore = VK_TRUE,
        };
        publishChainNode(&unrelated);
        id2.pNext = &unrelated;
        expect(enablesApplicationPresentId2Wait2(&id2),
            "features behind an unrelated feature node were not found");
    }
#endif

    void testPresentTimingPrefix() {
        // Issue #48: vkd3d-proton prepends EXT timing ahead of the present ID,
        // maintenance1 mode override and fence. Keep borrowed payloads opaque:
        // filtering a link must not drop, rewrite or dereference their data.
        constexpr uint64_t timingPayload[]{0x1122334455667788ULL, 90'000'000};
        constexpr uint64_t presentIds[]{0x123456789abcdef0ULL};
        constexpr VkPresentModeKHR modes[]{VK_PRESENT_MODE_MAILBOX_KHR};
        const VkFence fences[]{VK_NULL_HANDLE};
        const VkSwapchainPresentFenceInfoEXT fence{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
            .pNext = nullptr,
            .swapchainCount = 1,
            .pFences = fences,
        };
        const VkSwapchainPresentModeInfoEXT mode{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT,
            .pNext = &fence,
            .swapchainCount = 1,
            .pPresentModes = modes,
        };
        const VkPresentIdKHR id{
            .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
            .pNext = &mode,
            .swapchainCount = 1,
            .pPresentIds = presentIds,
        };
        const detail::PresentTimingsInfoExtLayout timings{
            .sType = detail::presentTimingsInfoExtType,
            .pNext = &id,
            .swapchainCount = 1,
            .pTimingInfos = timingPayload,
        };
        const FilteredPresentPNextChain filtered(&timings, true);
        expect(filtered.valid(),
            "EXT present timing prefix rejected the first Proton present");
        const auto* copied = static_cast<
            const detail::PresentTimingsInfoExtLayout*>(filtered.head());
        expect(copied != &timings && copied->sType == timings.sType &&
                copied->swapchainCount == 1 &&
                copied->pTimingInfos == timingPayload,
            "copied timing node lost its type, count or borrowed payload");
        const auto* copiedId = static_cast<const VkPresentIdKHR*>(copied->pNext);
        expect(copiedId != &id && copiedId->sType == id.sType &&
                copiedId->swapchainCount == 1 &&
                copiedId->pPresentIds == presentIds &&
                copiedId->pNext == &fence,
            "timing filter lost the present ID or its unchanged fence suffix");
        expect(timings.pNext == &id && id.pNext == &mode &&
                mode.pNext == &fence && modes[0] == VK_PRESENT_MODE_MAILBOX_KHR &&
                timingPayload[0] == 0x1122334455667788ULL &&
                presentIds[0] == 0x123456789abcdef0ULL,
            "present filtering modified caller-owned links or payloads");

        const VkPresentRegionsKHR regions{
            .sType = VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR,
            .pNext = &fence,
            .swapchainCount = 1,
            .pRegions = nullptr,
        };
        // Null pTimingInfos is legal and must stay in an untouched suffix.
        const detail::PresentTimingsInfoExtLayout nullTimings{
            .sType = detail::presentTimingsInfoExtType,
            .pNext = &regions,
            .swapchainCount = 1,
            .pTimingInfos = nullptr,
        };
        auto modeHead = mode;
        modeHead.pNext = &nullTimings;
        const FilteredPresentPNextChain suffix(&modeHead, true);
        expect(suffix.valid() && suffix.head() == &nullTimings,
            "timing in an untouched suffix was unnecessarily copied");
        const FilteredPresentPNextChain disabled(&timings, false);
        expect(disabled.valid() && disabled.head() == &timings,
            "passthrough timing chain changed without a removal target");

        const VkBaseInStructure unknown{
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pNext = reinterpret_cast<const VkBaseInStructure*>(&mode),
        };
        auto unknownTimings = timings;
        unknownTimings.pNext = &unknown;
        const FilteredPresentPNextChain rejected(&unknownTimings, true);
        expect(!rejected.valid() && rejected.head() == &unknownTimings &&
                rejected.unsupportedStructureType() == unknown.sType &&
                unknownTimings.pNext == &unknown &&
                unknown.pNext == reinterpret_cast<const VkBaseInStructure*>(&mode),
            "timing support weakened unknown-prefix rejection or input ownership");
    }

    // The view format list of a mutable swapchain create chain: read at the head or
    // behind other structures, and absent, empty or without an array otherwise.
    void testImageFormatListOf() {
        constexpr VkFormat formats[]{VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB};
        const VkImageFormatListCreateInfo list{
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
            .pNext = nullptr,
            .viewFormatCount = 2,
            .pViewFormats = formats,
        };
        publishChainNode(&list);
        const auto direct = imageFormatListOf(&list);
        expect(direct.size() == 2 && direct.data() == formats &&
                direct[0] == VK_FORMAT_B8G8R8A8_UNORM && direct[1] == VK_FORMAT_B8G8R8A8_SRGB,
            "the format list at the head of a chain was not read");

        const VkDeviceGroupSwapchainCreateInfoKHR group{
            .sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_SWAPCHAIN_CREATE_INFO_KHR,
            .pNext = &list,
            .modes = 0,
        };
        publishChainNode(&group);
        const auto behindOther = imageFormatListOf(&group);
        expect(behindOther.size() == 2 && behindOther.data() == formats,
            "the format list behind another structure was not read");

        const VkDeviceGroupSwapchainCreateInfoKHR alone{
            .sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_SWAPCHAIN_CREATE_INFO_KHR,
            .pNext = nullptr,
            .modes = 0,
        };
        publishChainNode(&alone);
        expect(imageFormatListOf(nullptr).empty() && imageFormatListOf(&alone).empty(),
            "a chain without a format list reported formats");

        const VkImageFormatListCreateInfo emptyList{
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
            .pNext = nullptr,
            .viewFormatCount = 0,
            .pViewFormats = formats,
        };
        const VkImageFormatListCreateInfo noArray{
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
            .pNext = nullptr,
            .viewFormatCount = 2,
            .pViewFormats = nullptr,
        };
        publishChainNode(&emptyList);
        publishChainNode(&noArray);
        expect(imageFormatListOf(&emptyList).empty() && imageFormatListOf(&noArray).empty(),
            "an empty format list reported formats");
    }
}

int main() {
    testGeneratedOutputMetadata();
    const VkPresentTimesInfoGOOGLE googleTiming{
        .sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE,
    };
    const detail::PresentTimingsInfoExtLayout extTiming{
        .sType = detail::presentTimingsInfoExtType,
    };
    VkPresentIdKHR timingPrefix{
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .pNext = &googleTiming,
    };
    expect(hasPresentTiming(&timingPrefix),
        "upstream Google timing was hidden behind an ID prefix");
    timingPrefix.pNext = &extTiming;
    expect(hasPresentTiming(&timingPrefix),
        "upstream EXT timing was hidden behind an ID prefix");
    timingPrefix.pNext = nullptr;
    expect(!hasPresentTiming(&timingPrefix) && !hasPresentTiming(nullptr),
        "ordinary present metadata incorrectly claimed timing ownership");
#if defined(VK_KHR_present_id2)
    testPresentId2Prefix();
#endif
#if defined(VK_KHR_present_id2) && defined(VK_KHR_present_wait2)
    testPresentId2Wait2Enablement();
#endif
    testPresentTimingPrefix();
    testImageFormatListOf();
    const VkBaseInStructure tail{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
    };
    // Model a lower WSI layer's maintenance1 node: its mode list can live
    // in immutable storage and advertises MAILBOX only. Ordered SDR filters the
    // node; it must never const-cast and rewrite this array to FIFO.
    constexpr VkPresentModeKHR immutableModes[]{VK_PRESENT_MODE_MAILBOX_KHR};
    const VkSwapchainPresentModesCreateInfoEXT createModes{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT,
        .pNext = &tail,
        .presentModeCount = 1,
        .pPresentModes = immutableModes,
    };

    const FilteredSwapchainCreatePNextChain filteredHead(
        &createModes, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT
    );
    expect(filteredHead.valid() && filteredHead.head() == &tail,
        "head present-mode node was not filtered");
    expect(immutableModes[0] == VK_PRESENT_MODE_MAILBOX_KHR,
        "immutable lower-layer mode storage was modified");

    const FilteredSwapchainCreatePNextChain disabledCreateFilter(
        &createModes, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT,
        false
    );
    expect(disabledCreateFilter.valid() &&
            disabledCreateFilter.head() == &createModes,
        "disabled filtering changed a native passthrough chain");

    const VkDeviceGroupSwapchainCreateInfoKHR prefix{
        .sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = &createModes,
        .modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR,
    };
    const auto* const originalCreateLink = prefix.pNext;
    const FilteredSwapchainCreatePNextChain filteredNestedCreate(
        &prefix, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT
    );
    expect(filteredNestedCreate.valid(),
        "known immutable swapchain-create prefix could not be copied");
    expect(prefix.pNext == originalCreateLink,
        "create filter rewrote caller-owned immutable storage");
    const auto* copiedCreatePrefix = reinterpret_cast<
        const VkDeviceGroupSwapchainCreateInfoKHR*>(
            filteredNestedCreate.head()
        );
    expect(copiedCreatePrefix != &prefix &&
            copiedCreatePrefix->sType == prefix.sType &&
            copiedCreatePrefix->modes == prefix.modes &&
            copiedCreatePrefix->pNext == &tail,
        "nested create filtering did not preserve the known prefix payload");

    const VkBaseInStructure unknownCreatePrefix{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = reinterpret_cast<const VkBaseInStructure*>(&createModes),
    };
    const FilteredSwapchainCreatePNextChain unknownCreateFilter(
        &unknownCreatePrefix,
        VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT
    );
    expect(!unknownCreateFilter.valid() &&
            unknownCreateFilter.unsupportedStructureType() ==
                VK_STRUCTURE_TYPE_APPLICATION_INFO &&
            unknownCreatePrefix.pNext == reinterpret_cast<
                const VkBaseInStructure*>(&createModes),
        "unknown immutable create prefix was modified or truncated");

    VkBaseInStructure presentTail{
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .pNext = nullptr,
    };
    VkPresentRegionsKHR regions{
        .sType = VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR,
        .pNext = &presentTail,
        .swapchainCount = 0,
        .pRegions = nullptr,
    };
    const VkDeviceGroupPresentInfoKHR immutablePresentPrefix{
        .sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR,
        .pNext = &regions,
        .swapchainCount = 0,
        .pDeviceMasks = nullptr,
        .mode = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR,
    };
    const FilteredPresentPNextChain disabledPresentFilter(
        &immutablePresentPrefix, false
    );
    expect(disabledPresentFilter.valid() &&
            disabledPresentFilter.head() == &immutablePresentPrefix,
        "disabled present filtering copied or changed the input chain");

    std::cout << "pNext chain tests passed\n";
    return 0;
}

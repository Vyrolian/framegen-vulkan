/* SPDX-License-Identifier: GPL-3.0-or-later */
// QH2 and QH3 policy: the transfer family choice and its fallback order, the queue request that extends the device
// create info, and the queue families a lower swapchain shares with the transfer family. No device is needed.
#include "swapchain/transfer_queue.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

using namespace fgvk::layer;

namespace {
    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }

    constexpr VkQueueFlags graphicsCompute = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    constexpr VkQueueFlags transferOnly = VK_QUEUE_TRANSFER_BIT;

    VkQueueFamilyProperties family(const VkQueueFlags flags, const uint32_t count, const uint32_t timestampBits) {
        return VkQueueFamilyProperties{
            .queueFlags = flags,
            .queueCount = count,
            .timestampValidBits = timestampBits,
            .minImageTransferGranularity = {1, 1, 1},
        };
    }

    VkDeviceQueueCreateInfo request(const uint32_t familyIndex, const uint32_t count,
            const float* priorities, const VkDeviceQueueCreateFlags flags = 0) {
        return VkDeviceQueueCreateInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext = nullptr,
            .flags = flags,
            .queueFamilyIndex = familyIndex,
            .queueCount = count,
            .pQueuePriorities = priorities,
        };
    }

    // The entries and priorities of a C create info, read through spans so the indexing is bounds-tracked.
    std::span<const VkDeviceQueueCreateInfo> entriesOf(const VkDeviceCreateInfo& info) {
        return {info.pQueueCreateInfos, info.queueCreateInfoCount};
    }
    std::span<const float> prioritiesOf(const VkDeviceQueueCreateInfo& entry) {
        return {entry.pQueuePriorities, entry.queueCount};
    }

    VkDeviceCreateInfo createInfo(const std::span<const VkDeviceQueueCreateInfo> queues) {
        VkDeviceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.queueCreateInfoCount = static_cast<uint32_t>(queues.size());
        info.pQueueCreateInfos = queues.data();
        return info;
    }

    void testFamilySelection() {
        // A dedicated transfer family is chosen over the graphics family that also has transfer.
        const std::array dedicated{
            family(graphicsCompute, 16, 64),
            family(transferOnly, 2, 64),
        };
        expect(selectTransferFamily(dedicated) == std::optional<uint32_t>{1},
            "the dedicated transfer family was not chosen");

        // No family without graphics and compute: none is chosen.
        const std::array none{
            family(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT, 1, 64),
            family(VK_QUEUE_COMPUTE_BIT, 4, 64),
        };
        expect(!selectTransferFamily(none), "a graphics or compute family was chosen as the transfer family");

        // A transfer family that also has compute is refused.
        const std::array withCompute{
            family(VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 2, 64),
        };
        expect(!selectTransferFamily(withCompute), "a family with COMPUTE was chosen as the transfer family");

        // A family with no queues is refused.
        const std::array empty{family(transferOnly, 0, 64)};
        expect(!selectTransferFamily(empty), "an empty transfer family was chosen");

        // Timestamps are preferred, not required.
        const std::array noTimestamps{family(transferOnly, 1, 0), family(transferOnly, 1, 36)};
        expect(selectTransferFamily(noTimestamps) == std::optional<uint32_t>{1},
            "a family with timestamps was not preferred");
        const std::array onlyNoTimestamps{family(transferOnly, 1, 0)};
        expect(selectTransferFamily(onlyNoTimestamps) == std::optional<uint32_t>{0},
            "a transfer family without timestamps was refused");
    }

    void testAbsentFamilyAppendsOneQueue() {
        const std::array families{family(graphicsCompute, 16, 64), family(transferOnly, 2, 64)};
        const std::array<float, 2> applicationPriorities{1.0F, 0.5F};
        const std::array queues{request(0, 2, applicationPriorities.data())};
        const VkDeviceCreateInfo info = createInfo(queues);

        TransferQueueRequest transfer(info, families);
        expect(transfer.family == std::optional<uint32_t>{1}, "the absent transfer family was not chosen");
        expect(transfer.index == std::optional<uint32_t>{0}, "an appended transfer queue is not index zero");
        expect(transfer.timestamps, "the transfer family's timestamps were not recorded");

        VkDeviceCreateInfo applied = info;
        transfer.apply(applied);
        expect(applied.queueCreateInfoCount == 2, "the appended entry is missing from the create info");
        const auto& original = entriesOf(applied)[0];
        expect(original.queueFamilyIndex == 0 && original.queueCount == 2 &&
                original.pQueuePriorities == applicationPriorities.data(),
            "the application's entry changed when a transfer entry was appended");
        const auto& appended = entriesOf(applied)[1];
        expect(appended.sType == VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO && appended.pNext == nullptr &&
                appended.flags == 0 && appended.queueFamilyIndex == 1 && appended.queueCount == 1 &&
                prioritiesOf(appended).size() == 1 && prioritiesOf(appended)[0] == 1.0F,
            "the appended transfer entry is not one priority-1.0 queue on the transfer family");
    }

    void testRequestedFamilyGrowsByOne() {
        const std::array families{family(graphicsCompute, 16, 64), family(transferOnly, 4, 64)};
        const std::array<float, 2> graphicsPriorities{1.0F, 0.75F};
        const std::array<float, 2> transferPriorities{0.5F, 0.25F};
        const std::array queues{
            request(0, 2, graphicsPriorities.data()),
            request(1, 2, transferPriorities.data()),
        };
        const VkDeviceCreateInfo info = createInfo(queues);

        TransferQueueRequest transfer(info, families);
        expect(transfer.family == std::optional<uint32_t>{1}, "the requested transfer family was not chosen");
        expect(transfer.index == std::optional<uint32_t>{2},
            "the extra queue index is not the application's count");

        VkDeviceCreateInfo applied = info;
        transfer.apply(applied);
        expect(applied.queueCreateInfoCount == 2, "growing an entry changed the entry count");
        const auto& grown = entriesOf(applied)[1];
        expect(grown.queueFamilyIndex == 1 && grown.queueCount == 3, "the transfer entry did not grow by one");
        expect(prioritiesOf(grown).size() == 3 && prioritiesOf(grown)[0] == 0.5F &&
                prioritiesOf(grown)[1] == 0.25F,
            "the application's priorities were not copied");
        expect(prioritiesOf(grown)[2] == 0.25F, "the extra queue did not take the last priority");
        expect(transferPriorities[0] == 0.5F && transferPriorities[1] == 0.25F,
            "the application's priority array changed");

        const auto& untouched = entriesOf(applied)[0];
        expect(untouched.queueFamilyIndex == 0 && untouched.queueCount == 2 &&
                untouched.pQueuePriorities == graphicsPriorities.data(),
            "an entry for another family changed");
    }

    void testFullFamilyIsNotRequested() {
        const std::array families{family(graphicsCompute, 16, 64), family(transferOnly, 2, 64)};
        const std::array<float, 2> priorities{0.5F, 0.25F};
        const std::array queues{request(1, 2, priorities.data())};
        const VkDeviceCreateInfo info = createInfo(queues);
        TransferQueueRequest transfer(info, families);
        expect(transfer.family == std::optional<uint32_t>{1}, "the full transfer family was not chosen");
        expect(!transfer.index, "a full transfer family was requested anyway");
        VkDeviceCreateInfo applied = info;
        transfer.apply(applied);
        expect(applied.pQueueCreateInfos == queues.data() && applied.queueCreateInfoCount == 1,
            "an unrequested transfer changed the create info");
    }

    void testFlaggedEntryIsNotExtended() {
        const std::array families{family(graphicsCompute, 16, 64), family(transferOnly, 4, 64)};
        const std::array<float, 1> priorities{1.0F};
        const std::array queues{request(1, 1, priorities.data(), VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT)};
        const VkDeviceCreateInfo info = createInfo(queues);
        TransferQueueRequest transfer(info, families);
        expect(!transfer.index, "a protected entry on the transfer family was extended");
    }

    void testNoTransferFamilyNoRequest() {
        const std::array families{family(graphicsCompute, 16, 64)};
        const std::array<float, 1> priorities{1.0F};
        const std::array queues{request(0, 1, priorities.data())};
        const VkDeviceCreateInfo info = createInfo(queues);
        TransferQueueRequest transfer(info, families);
        expect(!transfer.family && !transfer.index, "a device without a transfer family got a transfer queue");
        VkDeviceCreateInfo applied = info;
        transfer.apply(applied);
        expect(applied.pQueueCreateInfos == queues.data() && applied.queueCreateInfoCount == 1,
            "a device without a transfer family changed its create info");
    }

    void testSharingFamilies() {
        // An exclusive application swapchain shares only with the two families.
        const std::vector<uint32_t> exclusive = transferSharingFamilies(VK_SHARING_MODE_EXCLUSIVE, {}, 0, 1);
        expect((exclusive == std::vector<uint32_t>{0, 1}), "an exclusive swapchain did not share with the transfer family");

        // A concurrent application keeps its own families, once each, before the layer and transfer families.
        const std::array applicationFamilies{uint32_t{2}, uint32_t{0}};
        const std::vector<uint32_t> concurrent = transferSharingFamilies(VK_SHARING_MODE_CONCURRENT,
            applicationFamilies, 0, 1);
        expect((concurrent == std::vector<uint32_t>{2, 0, 1}),
            "a concurrent swapchain's families were not kept and deduplicated");

        // An exclusive swapchain ignores a family list it does not use.
        const std::array ignored{uint32_t{5}};
        const std::vector<uint32_t> ignoredList = transferSharingFamilies(VK_SHARING_MODE_EXCLUSIVE, ignored, 0, 1);
        expect((ignoredList == std::vector<uint32_t>{0, 1}), "an exclusive swapchain kept a family it did not list");

        // The layer and transfer families being equal leaves one family, which the caller refuses.
        const std::vector<uint32_t> single = transferSharingFamilies(VK_SHARING_MODE_EXCLUSIVE, {}, 3, 3);
        expect(single.size() == 1, "duplicate layer and transfer families were listed twice");
    }

    // QH3: plain transfer families precede the video families, 64 timestamp bits precede fewer, and ties keep the lower
    // index. Graphics, compute, optical flow and empty families never appear.
    void testCandidateOrder() {
        const std::array families{
            family(transferOnly, 1, 0),                                                    // 0: plain, no timestamps
            family(VK_QUEUE_VIDEO_DECODE_BIT_KHR | VK_QUEUE_TRANSFER_BIT, 1, 64),          // 1: video, 64 bits
            family(transferOnly, 1, 36),                                                   // 2: plain, 36 bits
            family(transferOnly, 1, 64),                                                   // 3: plain, 64 bits
            family(VK_QUEUE_VIDEO_ENCODE_BIT_KHR | VK_QUEUE_TRANSFER_BIT, 1, 64),          // 4: video, 64 bits
        };
        expect((transferFamilyCandidates(families) == std::vector<uint32_t>{3, 2, 0, 1, 4}),
            "the transfer candidates are not in preference order");

        const std::array ties{family(transferOnly, 1, 64), family(transferOnly, 1, 64)};
        expect((transferFamilyCandidates(ties) == std::vector<uint32_t>{0, 1}), "a tie did not keep the lower index");

        const std::array excluded{
            family(graphicsCompute, 16, 64),
            family(VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 2, 64),
            family(VK_QUEUE_OPTICAL_FLOW_BIT_NV | VK_QUEUE_TRANSFER_BIT, 1, 64),
            family(transferOnly, 0, 64),
        };
        expect(transferFamilyCandidates(excluded).empty(), "an excluded family was a transfer candidate");
    }

    // QH3, the NVIDIA layout measured in QD1. Family 1 is the only plain transfer family, and VKD3D requests both
    // of its queues, so the copy falls back to a video family. Family 5 (optical flow) is never a candidate.
    std::array<VkQueueFamilyProperties, 6> nvidiaFamilies() {
        return {
            family(graphicsCompute, 16, 64),
            family(transferOnly, 2, 64),
            family(VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 8, 64),
            family(VK_QUEUE_VIDEO_DECODE_BIT_KHR | VK_QUEUE_TRANSFER_BIT, 1, 32),
            family(VK_QUEUE_VIDEO_ENCODE_BIT_KHR | VK_QUEUE_TRANSFER_BIT, 1, 32),
            family(VK_QUEUE_OPTICAL_FLOW_BIT_NV | VK_QUEUE_TRANSFER_BIT, 1, 64),
        };
    }

    void testNvidiaLayoutFallsBackToVideoFamily() {
        const auto families = nvidiaFamilies();
        const std::array<float, 2> both{1.0F, 0.5F};
        const std::array<float, 1> one{1.0F};

        // Family 1 requested at both of its queues: family 3 (video decode) is the first candidate that can take one.
        const std::array fullFirst{request(1, 2, both.data())};
        const VkDeviceCreateInfo full = createInfo(fullFirst);
        TransferQueueRequest fallback(full, families);
        expect(fallback.family == std::optional<uint32_t>{3}, "the video decode family was not the fallback");
        expect(fallback.index == std::optional<uint32_t>{0}, "the fallback queue is not index zero");
        expect(!fallback.timestamps, "a 32-bit video family was marked as timed");

        VkDeviceCreateInfo applied = full;
        fallback.apply(applied);
        expect(applied.queueCreateInfoCount == 2, "the fallback entry is missing from the create info");
        const auto& original = entriesOf(applied)[0];
        expect(original.queueFamilyIndex == 1 && original.queueCount == 2 && original.pQueuePriorities == both.data(),
            "the application's family 1 entry changed");
        const auto& appended = entriesOf(applied)[1];
        expect(appended.queueFamilyIndex == 3 && appended.queueCount == 1 && prioritiesOf(appended)[0] == 1.0F,
            "the fallback entry is not one priority-1.0 queue on family 3");

        // Family 1 free: the plain family is still chosen, and its 64-bit timestamps are used.
        const std::array freeFirst{request(0, 1, one.data())};
        TransferQueueRequest plain(createInfo(freeFirst), families);
        expect(plain.family == std::optional<uint32_t>{1} && plain.index == std::optional<uint32_t>{0} &&
                plain.timestamps, "a free plain transfer family was not chosen");

        // Families 1, 3 and 4 full: no request. Family 1 is still reported, and optical flow family 5 is not used.
        const std::array allFull{request(1, 2, both.data()), request(3, 1, one.data()), request(4, 1, one.data())};
        const VkDeviceCreateInfo none = createInfo(allFull);
        TransferQueueRequest refused(none, families);
        expect(refused.family == std::optional<uint32_t>{1}, "a refused request did not report the first candidate");
        expect(!refused.index, "a full set of transfer families was requested anyway");
        VkDeviceCreateInfo unchanged = none;
        refused.apply(unchanged);
        expect(unchanged.pQueueCreateInfos == allFull.data() && unchanged.queueCreateInfoCount == 3,
            "a refused transfer request changed the create info");
    }

    void testRefusedEntriesFallThrough() {
        const auto families = nvidiaFamilies();
        const std::array<float, 1> one{1.0F};

        // A protected entry on family 1 is refused, so the request moves on to family 3.
        const std::array flagged{request(1, 1, one.data(), VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT)};
        TransferQueueRequest protectedEntry(createInfo(flagged), families);
        expect(protectedEntry.family == std::optional<uint32_t>{3} &&
                protectedEntry.index == std::optional<uint32_t>{0},
            "a protected family 1 entry did not fall through to the video family");

        // Two entries for family 1 are refused too.
        const std::array twice{request(1, 1, one.data()), request(1, 1, one.data())};
        TransferQueueRequest duplicated(createInfo(twice), families);
        expect(duplicated.family == std::optional<uint32_t>{3} && duplicated.index == std::optional<uint32_t>{0},
            "two family 1 entries did not fall through to the video family");
    }

    void testOpticalFlowIsNeverTransfer() {
        const std::array families{family(VK_QUEUE_OPTICAL_FLOW_BIT_NV | VK_QUEUE_TRANSFER_BIT, 1, 64)};
        expect(!selectTransferFamily(families), "an optical flow family was chosen as the transfer family");
        const VkDeviceCreateInfo info = createInfo({});
        TransferQueueRequest transfer(info, families);
        expect(!transfer.family && !transfer.index, "an optical flow family got a transfer queue");
    }
}

int main() {
    testFamilySelection();
    testAbsentFamilyAppendsOneQueue();
    testRequestedFamilyGrowsByOne();
    testFullFamilyIsNotRequested();
    testFlaggedEntryIsNotExtended();
    testNoTransferFamilyNoRequest();
    testSharingFamilies();
    testCandidateOrder();
    testNvidiaLayoutFallsBackToVideoFamily();
    testRefusedEntriesFallThrough();
    testOpticalFlowIsNeverTransfer();
    std::cout << "framegen-vulkan transfer queue policy tests passed.\n";
    return 0;
}

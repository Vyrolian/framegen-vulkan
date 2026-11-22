/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain/present_layout.hpp"
#include "swapchain/internal_images.hpp"

#include "swapchain/deferred_present.hpp"
#include "swapchain/inline_generation.hpp"
#include "library_pin.hpp"
#include "pnext_chain.hpp"
#include "present_diagnostics.hpp"
#include "fgvk-common/helpers/errors.hpp"
#include "fgvk-common/vulkan/command_buffer.hpp"
#include "fgvk-common/vulkan/gpu_clock.hpp"
#include "fgvk-common/vulkan/timestamp_query_pool.hpp"
#include "fgvk-common/vulkan/semaphore.hpp"
#include "fgvk-common/vulkan/timeline_semaphore.hpp"
#include "fgvk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

    namespace {
        constexpr uint64_t destroyWaitTimeoutNs = 2'000'000'000ULL;
        // D7d: the application indices with their own stored acquire for the stall probe.
        constexpr size_t stallProbeIndexSlots = 8;

        VkImageMemoryBarrier barrier(const VkImage image,
                const VkAccessFlags srcAccess, const VkAccessFlags dstAccess,
                const VkImageLayout oldLayout, const VkImageLayout newLayout) {
            return VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = srcAccess,
                .dstAccessMask = dstAccess,
                .oldLayout = oldLayout,
                .newLayout = newLayout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            };
        }

        // F10: a timeline counter read for the host acquire path. It never waits, and a failed
        // read reads as zero, which refuses every nonzero wait value. The caller checks that
        // GetSemaphoreCounterValueKHR is loaded.
        uint64_t timelineCounter(const vk::VulkanDeviceFuncs& funcs, const VkDevice device,
                const VkSemaphore semaphore) {
            uint64_t value = 0;
            if (funcs.GetSemaphoreCounterValueKHR(device, semaphore, &value) != VK_SUCCESS)
                return 0;
            return value;
        }
    }

    struct InternalSwapchainImages::Impl {
        VkDevice device{VK_NULL_HANDLE};
        vk::VulkanDeviceFuncs funcs{};
        std::shared_ptr<DeferredPresentQueue> queue;
        VkSwapchainKHR lowerSwapchain{VK_NULL_HANDLE};
        VkExtent2D extent{};
        InternalImageRing ring;

        std::vector<VkImage> images;
        std::vector<VkDeviceMemory> memories;
        std::vector<VkImage> lowerImages;
        // Copy of the application's view formats, read by createImages. Owned here: the
        // caller's create chain is not kept.
        std::vector<VkFormat> viewFormats;

        // FI2b: one release timeline per queue. `release` is signalled only on the private queue, `callRelease`
        // only on the game queue at the present call. Each has one signalling queue, so no cross-queue wait is needed.
        std::optional<vk::TimelineSemaphore> release;
        std::optional<vk::TimelineSemaphore> callRelease;
        // Serialised by queue->mutex: the last value of each timeline and everything the copy and
        // release paths write. The emulated acquire never touches these. It reads only
        // the ring (its own mutex) and the release and tail counters, and it takes
        // queue->mutex only for the queued submit (F10).
        // Private values: queue->mutex only. lastCallReleaseValue is written by the game-queue submit and read by the
        // worker, so it is guarded by the leaf mutex below (FP2).
        uint64_t lastReleaseValue{0};
        uint64_t lastCallReleaseValue{0};
        // FP2: the release value of the last drain submit (queue->mutex). retireParked waits it on the release timeline.
        uint64_t lastDrainValue{0};
        // FP2 (leaf mutex): the direct generated outputs whose lower image is taken and not yet presented. Each one is counted
        // by the budget (see beginCallOutput).
        size_t callInFlight{0};
        // FP2: the leaf mutex. It is taken alone or nested inside queue->mutex, never around a present, a private-queue
        // submit or a blocking wait (a zero-timeout query is allowed). It guards every field a game-queue submit writes
        // or reads: parking (the pool, the policy and the busy flags), callInFlight, lastCallReleaseValue, copyClock,
        // copyClockAt, copyContext, copyPending, copyPendingGame, copyPendingValue and copyPendingSource. copyTimed is written only while
        // a copy is recorded, before any submit of that pair. lastReleaseValue, lastDrainValue and the ring's states are
        // not guarded by it.
        std::mutex leaf;
        LowerParkingPool parking;
        // FI2: the family of the command pool the copies are recorded from (vk.cmdpool(), created for the application's
        // queue family). A call-side copy needs the game queue to be of this family (rule 5). Set in initialize().
        uint32_t copyFamily{UINT32_MAX};
        std::vector<vk::Semaphore> copied;
        // R1: one binary semaphore per application image. releaseNotHandedOff signals it on
        // the application queue and the private release waits it, so the private release
        // timeline is only ever signalled on the private queue.
        std::vector<vk::Semaphore> handoff;
        std::vector<vk::Semaphore> lowerAcquire;
        std::vector<VkFence> lowerFences;
        uint32_t lowerHead{0};
        // Lower-acquire slot committed by the last worker acquire. Only the
        // worker sets it, under queue->mutex, and submit reads it under the same lock.
        uint32_t workerSlot{0};
        // Index applicationIndex * lowerImages.size() + lowerIndex. Recorded on
        // the game thread by ensureCopies and only read afterwards.
        std::vector<std::optional<vk::CommandBuffer>> copies;
        // T3b, diagnostics only: one timestamp pair per copy pair (same index as copies).
        // The pool pointer and context are set on the game thread under queue->mutex;
        // timed is written when a pair is recorded (game thread), pending/pendingSource
        // under queue->mutex. copySource is written lock-free by the game thread.
        std::unique_ptr<vk::TimestampQueryPool> copyTimes;
        std::optional<vk::CalibratedClock> copyClock;
        present_diagnostics::Clock::time_point copyClockAt{};
        uint64_t copyContext{};
        std::vector<char> copyTimed, copyPending;
        // FI2: 1 for a pending copy submitted on the game queue, 0 for the private queue (the copy's `queue=` field).
        std::vector<char> copyPendingGame;
        // FI2b rule 6: the release value a pending copy signals on its timeline (copyPendingGame picks it). Its
        // timestamps are read only once that value is reached.
        std::vector<uint64_t> copyPendingValue;
        std::vector<uint64_t> copyPendingSource;
        std::unique_ptr<std::atomic<uint64_t>[]> copySource;
        bool copyTimesTried{};

        std::atomic<VkResult> sticky{VK_SUCCESS};
        std::atomic<bool> lowerSuboptimal{false};
        // D7, pacing profile only: the release wait value, application index and tail wait
        // value of the latest emulated acquire submit. Stored before that submit.
        std::atomic<uint64_t> probeAcquireWait{0};
        std::atomic<uint64_t> probeAcquireIndex{0};
        std::atomic<uint64_t> probeAcquireTail{0};
        // D7d, pacing profile only: the release wait value and tail value of the latest emulated
        // acquire of each application index below stallProbeIndexSlots, and the steady-clock time
        // (ns, zero when never stored) of that store. Stored with the probeAcquire* values.
        std::array<std::atomic<uint64_t>, stallProbeIndexSlots> probeWaitOf{}, probeTailOf{};
        std::array<std::atomic<int64_t>, stallProbeIndexSlots> probeAtOf{};

        Impl(const vk::Vulkan& vk, const VkSwapchainKHR lower, const uint32_t imageCount,
                std::shared_ptr<DeferredPresentQueue> presentQueue)
            : device(vk.dev()), funcs(vk.df()),
              queue(std::move(presentQueue)), lowerSwapchain(lower),
              ring(imageCount) {}

        ~Impl();

        bool initialize(const vk::Vulkan& vk, const VkSwapchainCreateInfoKHR& application,
                VkExtent2D applicationExtent, std::span<const VkImage> lowerHandles,
                uint32_t imageCount);
        bool createImages(const vk::Vulkan& vk, const VkSwapchainCreateInfoKHR& application,
                uint32_t imageCount);
        VkCommandBuffer copyCommandFor(const vk::Vulkan& vk, uint32_t applicationIndex,
                uint32_t lowerIndex);
        // Caller holds queue->mutex. Submits one release-timeline signal on
        // submitQueue, after waits, and frees applicationIndex at that value. Callers
        // pass the private queue, which also signals the F9c tail. On failure the image
        // is freed at the last submitted value of that timeline and the error is sticky.
        VkResult releaseLocked(VkQueue submitQueue, uint32_t applicationIndex,
                std::span<const VkSemaphore> waits);
        // FI2b: the timeline semaphore of a value on the given timeline.
        [[nodiscard]] VkSemaphore timelineHandle(const ReleaseTimeline timeline) const {
            return timelineHandleOf(std::array{release->handle(), callRelease->handle()}, timeline);
        }
        // FI2b, caller holds queue->mutex: the last value submitted on a timeline.
        [[nodiscard]] uint64_t& lastValueOf(const ReleaseTimeline timeline) noexcept {
            return timeline == ReleaseTimeline::Private ? lastReleaseValue : lastCallReleaseValue;
        }
        // FI2b, caller holds queue->mutex: the release signal of a submit on submitQueue. The private queue
        // signals `release`, any other queue (the game queue at the call) signals `callRelease`.
        [[nodiscard]] ReleaseSignal releaseSignalOn(const VkQueue submitQueue) {
            if (submitQueue == queue->queue)
                return planReleaseSignal(true, lastReleaseValue, 0);
            // FP2: the game queue's last value is read under the leaf mutex, the lock its submits write it under.
            const std::lock_guard leafLock(leaf);
            return planReleaseSignal(false, 0, lastCallReleaseValue);
        }
        // D7, D7d (pacing profile only): stores one emulated acquire's release wait value, tail
        // value and time for the stall probe. The caller checks the profile first.
        void noteAcquireProbe(uint32_t applicationIndex, uint64_t waitValue, uint64_t tailValue);
        // X1: stores a failing result as the sticky error, then wakes an application acquire
        // blocked on the ring. The store comes first, so the woken acquire returns this error.
        void failSticky(VkResult error) noexcept {
            sticky.store(error);
            ring.abort();
        }
    };

    bool InternalSwapchainImages::Impl::initialize(const vk::Vulkan& vk,
            const VkSwapchainCreateInfoKHR& application,
            const VkExtent2D applicationExtent,
            const std::span<const VkImage> lowerHandles,
            const uint32_t imageCount) {
        extent = applicationExtent;
        copyFamily = vk.queueFamilyIndex();
        lowerImages.assign(lowerHandles.begin(), lowerHandles.end());
        const size_t lowerCount = lowerImages.size();

        if (!createImages(vk, application, imageCount))
            return false;

        release.emplace(vk, 0);
        callRelease.emplace(vk, 0);
        copied.reserve(lowerCount);
        for (size_t i = 0; i < lowerCount; ++i)
            copied.emplace_back(vk);
        handoff.reserve(images.size());
        for (size_t i = 0; i < images.size(); ++i)
            handoff.emplace_back(vk);

        // Slots beyond the lower count let one lower image be acquired while
        // earlier copies still hold their own slots.
        const size_t slotCount = lowerCount + 2;
        lowerAcquire.reserve(slotCount);
        lowerFences.reserve(slotCount);
        for (size_t i = 0; i < slotCount; ++i) {
            lowerAcquire.emplace_back(vk);
            // Created signalled: the first wait on each slot must return at once.
            const VkFenceCreateInfo fenceInfo{
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .flags = VK_FENCE_CREATE_SIGNALED_BIT,
            };
            VkFence fence{VK_NULL_HANDLE};
            if (funcs.CreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
                return false;
            lowerFences.push_back(fence);
        }
        parking.reserveSlots(slotCount);
        copies.resize(static_cast<size_t>(imageCount) * lowerCount);
        return true;
    }

    bool InternalSwapchainImages::Impl::createImages(const vk::Vulkan& vk,
            const VkSwapchainCreateInfoKHR& application, const uint32_t imageCount) {
        // A mutable application swapchain (Zink) lets the application create UNORM and
        // sRGB views on these images, so they take the bit and the same format list.
        const auto formats = imageFormatListOf(application.pNext);
        viewFormats.assign(formats.begin(), formats.end());
        const VkImageFormatListCreateInfo formatList{
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
            .pNext = nullptr,
            .viewFormatCount = static_cast<uint32_t>(viewFormats.size()),
            .pViewFormats = viewFormats.data(),
        };
        const VkImageCreateFlags flags = internalImageCreateFlags(application.flags);
        const bool chainFormatList = (flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0 &&
            !viewFormats.empty();
        images.reserve(imageCount);
        memories.reserve(imageCount);
        for (uint32_t i = 0; i < imageCount; ++i) {
            const VkImageCreateInfo imageInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .pNext = chainFormatList ? &formatList : nullptr,
                .flags = flags,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = application.imageFormat,
                .extent = {.width = extent.width, .height = extent.height, .depth = 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = application.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                .sharingMode = application.imageSharingMode,
                .queueFamilyIndexCount = application.queueFamilyIndexCount,
                .pQueueFamilyIndices = application.pQueueFamilyIndices,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };
            VkImage image{VK_NULL_HANDLE};
            if (funcs.CreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS)
                return false;
            images.push_back(image);

            VkMemoryRequirements requirements{};
            funcs.GetImageMemoryRequirements(device, image, &requirements);
            const auto memoryType = vk.findMemoryTypeIndex(requirements.memoryTypeBits, false);
            if (!memoryType)
                return false;
            const VkMemoryDedicatedAllocateInfo dedicated{
                .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                .image = image,
            };
            const VkMemoryAllocateInfo allocation{
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .pNext = &dedicated,
                .allocationSize = requirements.size,
                .memoryTypeIndex = *memoryType,
            };
            VkDeviceMemory memory{VK_NULL_HANDLE};
            if (funcs.AllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS ||
                    memory == VK_NULL_HANDLE)
                return false;
            memories.push_back(memory);
            if (funcs.BindImageMemory(device, image, memory, 0) != VK_SUCCESS)
                return false;
        }
        return true;
    }

    // Records the copy for one application/lower pair on first use. The
    // SIMULTANEOUS_USE flag allows a later submission of the same pair while an
    // earlier execution is still pending, because the copy does not depend on
    // the state of any earlier execution.
    VkCommandBuffer InternalSwapchainImages::Impl::copyCommandFor(const vk::Vulkan& vk,
            const uint32_t applicationIndex, const uint32_t lowerIndex) {
        auto& slot = copies.at(static_cast<size_t>(applicationIndex) * lowerImages.size() + lowerIndex);
        if (!slot) {
            const VkImage source = images.at(applicationIndex);
            const VkImage target = lowerImages.at(lowerIndex);
            try {
                slot.emplace(vk);
                slot->begin(vk, VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);
                // The app's present layout maps to GENERAL on its private image.
                // The lower image is discarded because the whole extent is written.
                const std::array preBarriers{
                    barrier(source, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        present_layout::backingLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                    barrier(target, VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
                };
                const std::array postBarriers{
                    barrier(source, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, present_layout::backingLayout),
                    barrier(target, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
                };
                const size_t pair = static_cast<size_t>(applicationIndex) * lowerImages.size() + lowerIndex;
                const auto first = static_cast<uint32_t>(pair * 2);
                if (copyTimes) {
                    // The submit's waits block the transfer stage, so the start stamp follows them.
                    copyTimes->reset(slot->handle(), first, 2);
                    copyTimes->write(slot->handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, first);
                }
                slot->blitImage(vk, preBarriers, {source, target}, extent, postBarriers);
                if (copyTimes) {
                    copyTimes->write(slot->handle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, first + 1);
                    copyTimed.at(pair) = 1;
                }
                slot->end(vk);
            } catch (...) {
                slot.reset();
                throw;
            }
        }
        return slot->handle();
    }

    VkResult InternalSwapchainImages::Impl::releaseLocked(const VkQueue submitQueue,
            const uint32_t applicationIndex, const std::span<const VkSemaphore> waits) {
        // FI2b: the signal goes on the timeline of submitQueue, and no other wait is added.
        const ReleaseSignal signal = releaseSignalOn(submitQueue);
        const uint64_t value = signal.value;
        const std::vector<VkPipelineStageFlags> stages(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        const std::vector<uint64_t> waitValues(waits.size(), 0);
        const VkSemaphore releaseHandle = timelineHandle(signal.timeline);
        const VkTimelineSemaphoreSubmitInfo timeline{
            .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .waitSemaphoreValueCount = static_cast<uint32_t>(waitValues.size()),
            .pWaitSemaphoreValues = waitValues.data(),
            .signalSemaphoreValueCount = 1,
            .pSignalSemaphoreValues = &value,
        };
        const VkSubmitInfo submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = &timeline,
            .waitSemaphoreCount = static_cast<uint32_t>(waits.size()),
            .pWaitSemaphores = waits.data(),
            .pWaitDstStageMask = stages.data(),
            .commandBufferCount = 0,
            .pCommandBuffers = nullptr,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &releaseHandle,
        };
        // The private queue also signals the F9c tail.
        const VkResult result = submitQueue == queue->queue
            ? submitPrivate(*queue, funcs.QueueSubmit, submit, VK_NULL_HANDLE,
                SubmitTag{.kind = submitKindRelease, .applicationIndex = applicationIndex})
            : funcs.QueueSubmit(submitQueue, 1, &submit, VK_NULL_HANDLE);
        if (result == VK_SUCCESS) {
            lastValueOf(signal.timeline) = value;
            static_cast<void>(ring.complete(applicationIndex, value, signal.timeline));
        } else {
            failSticky(result);
            static_cast<void>(ring.complete(applicationIndex, lastValueOf(signal.timeline), signal.timeline));
        }
        return result;
    }

    void InternalSwapchainImages::Impl::noteAcquireProbe(const uint32_t applicationIndex,
            const uint64_t waitValue, const uint64_t tailValue) {
        probeAcquireWait.store(waitValue, std::memory_order_relaxed);
        probeAcquireIndex.store(applicationIndex, std::memory_order_relaxed);
        probeAcquireTail.store(tailValue, std::memory_order_relaxed);
        // D7d: the same values for this application index. The time is stored last.
        if (applicationIndex < stallProbeIndexSlots) {
            probeWaitOf[applicationIndex].store(waitValue, std::memory_order_relaxed);
            probeTailOf[applicationIndex].store(tailValue, std::memory_order_relaxed);
            probeAtOf[applicationIndex].store(steadyNanoseconds(present_diagnostics::Clock::now()),
                std::memory_order_release);
        }
    }

    InternalSwapchainImages::Impl::~Impl() {
        // Wait for the last copy so no pending GPU work reads the images or
        // signals the semaphores that are destroyed below. FI2b: a copy may have
        // signalled either timeline, so both are waited, all of them at once.
        if (release && callRelease && (lastReleaseValue != 0 || lastCallReleaseValue != 0)) {
            const std::array handles{release->handle(), callRelease->handle()};
            const std::array values{lastReleaseValue, lastCallReleaseValue};
            const VkSemaphoreWaitInfo waitInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                .semaphoreCount = static_cast<uint32_t>(handles.size()),
                .pSemaphores = handles.data(),
                .pValues = values.data(),
            };
            if (funcs.WaitSemaphoresKHR(device, &waitInfo, destroyWaitTimeoutNs) == VK_TIMEOUT) {
                std::cerr << "framegen-vulkan: internal swapchain images: destroy wait timed out\n";
                static_cast<void>(funcs.DeviceWaitIdle(device));
            }
        }
        copies.clear();
        lowerAcquire.clear();
        copied.clear();
        handoff.clear();
        release.reset();
        callRelease.reset();
        for (const VkFence fence : lowerFences) {
            if (fence != VK_NULL_HANDLE)
                funcs.DestroyFence(device, fence, nullptr);
        }
        lowerFences.clear();
        for (const VkImage image : images) {
            if (image != VK_NULL_HANDLE)
                funcs.DestroyImage(device, image, nullptr);
        }
        images.clear();
        for (size_t i = 0; i < memories.size(); ++i) {
            if (memories[i] == VK_NULL_HANDLE)
                continue;
            funcs.FreeMemory(device, memories[i], nullptr);
        }
        memories.clear();
    }

    std::shared_ptr<InternalSwapchainImages> InternalSwapchainImages::create(
            const vk::Vulkan& vk, const VkSwapchainKHR lowerSwapchain,
            const VkSwapchainCreateInfoKHR& application,
            const VkExtent2D extent, const std::span<const VkImage> lowerImages,
            const uint32_t imageCount, std::shared_ptr<DeferredPresentQueue> queue) {
        if (imageCount < InternalImageRing::minCount || imageCount > InternalImageRing::maxCount ||
                lowerImages.size() < 2 || !queue || queue->queue == VK_NULL_HANDLE ||
                lowerSwapchain == VK_NULL_HANDLE) {
            return nullptr;
        }
        auto state = std::make_unique<Impl>(vk, lowerSwapchain, imageCount, std::move(queue));
        try {
            if (!state->initialize(vk, application, extent, lowerImages, imageCount))
                return nullptr;
        } catch (const std::exception& error) {
            std::cerr << "framegen-vulkan: internal swapchain images: setup failed: "
                      << error.what() << '\n';
            return nullptr;
        }
        auto images = std::shared_ptr<InternalSwapchainImages>(
            new InternalSwapchainImages(std::move(state)));
        if (present_diagnostics::pacingProfileEnabled())
            registerStallProbe(images.get());
        return images;
    }

    InternalSwapchainImages::InternalSwapchainImages(std::unique_ptr<Impl> state)
        : impl(std::move(state)) {}

    InternalSwapchainImages::~InternalSwapchainImages() {
        // Removed before impl is destroyed, so the stall watchdog never dumps a destroyed owner.
        if (present_diagnostics::pacingProfileEnabled())
            unregisterStallProbe(this);
        // FP2 rule 6: a parked image is drained before the timelines are waited, so its drain is one of the waited values.
        // The parking worker holds the owner alive, so no other thread reads the pool here.
        if (impl && impl->parking.size() > 0) {
            const std::lock_guard queueLock(impl->queue->mutex);
            static_cast<void>(drainParkedLocked());
        }
    }

    // D7 stall probe. The registry, its mutex and the watchdog flag are leaked on purpose:
    // the detached watchdog may still run while the process exits.
    namespace {
        // A stall is 2 s without an application present. The watchdog polls every 500 ms
        // and dumps at 2 s, 4 s and 6 s of the same stall.
        constexpr int64_t stallNanoseconds = 2'000'000'000;
        constexpr int stallDumpsPerStall = 3;
        constexpr auto stallPollInterval = std::chrono::milliseconds{500};
        // The queue mutex is try-locked up to 40 times, 5 ms apart: at most 200 ms.
        constexpr int queueLockAttempts = 40;
        constexpr auto queueLockRetryInterval = std::chrono::milliseconds{5};

        std::mutex& stallRegistryMutex() {
            static std::mutex& mutex = *new std::mutex;
            return mutex;
        }
        std::vector<const InternalSwapchainImages*>& stallRegistry() {
            static auto& registry = *new std::vector<const InternalSwapchainImages*>;
            return registry;
        }
        bool& stallWatchdogStarted() {
            static bool started = false;
            return started;
        }
        // D7d names for the lower operations in LowerRecord and the in-flight call. Any other value reads as unknown.
        const char* lowerOpName(const uint8_t op) {
            switch (op) {
            case lowerOpAcquireReal: return "acq_real";
            case lowerOpAcquireGenerated: return "acq_gen";
            case lowerOpAcquireSync: return "acq_sync";
            case lowerOpPresentReal: return "pres_real";
            case lowerOpPresentGenerated: return "pres_gen";
            case lowerOpPresentSync: return "pres_sync";
            case lowerOpAcquireCall: return "acq_call";
            case lowerOpAcquirePark: return "acq_park";
            default: return "unknown";
            }
        }
        // D7d: milliseconds with the given number of decimals, from nanoseconds.
        std::string millisecondsText(const int64_t nanoseconds, const int decimals) {
            char text[32];
            std::snprintf(text, sizeof(text), "%.*f", decimals, static_cast<double>(nanoseconds) / 1'000'000.0);
            return text;
        }
        // D7d: the milliseconds from `then` to `now`, one decimal. A time after the dump start reads as zero,
        // because a store or record can land after the dump's clock read.
        std::string agoText(const int64_t now, const int64_t then) {
            return millisecondsText(std::max<int64_t>(0, now - then), 1);
        }
        // D7d: the low 16 bits of a swapchain handle, in hex.
        std::string lowSwapchainHex(const uint64_t swapchain) {
            char text[8];
            std::snprintf(text, sizeof(text), "%x", static_cast<unsigned>(swapchain & 0xffffu));
            return text;
        }
        // D7b names for the submit kinds in SubmitRecord. Any other value reads as unknown.
        const char* submitKindName(const uint8_t kind) {
            switch (kind) {
            case submitKindCopy: return "copy";
            case submitKindRelease: return "release";
            case submitKindGeneratedCopy: return "gen_copy";
            case submitKindGeneratedSkip: return "gen_skip";
            default: return "unknown";
            }
        }
        // An index, or "-" when the submit has none.
        std::string indexText(const uint32_t index) {
            return index == UINT32_MAX ? std::string{"-"} : std::to_string(index);
        }
    }

    void InternalSwapchainImages::registerStallProbe(const InternalSwapchainImages* owner) {
        const std::lock_guard lock(stallRegistryMutex());
        try {
            stallRegistry().push_back(owner);
            // The watchdog runs inside this DSO. When the pin fails, it does not start and no stall
            // dumps are written.
            if (!stallWatchdogStarted() && pinLayerLibrary()) {
                std::thread(&InternalSwapchainImages::stallWatchdogLoop).detach();
                stallWatchdogStarted() = true;
            }
        } catch (const std::exception& error) {
            std::cerr << "framegen-vulkan: d7 stall probe unavailable: " << error.what() << '\n';
        }
    }

    void InternalSwapchainImages::unregisterStallProbe(const InternalSwapchainImages* owner) {
        const std::lock_guard lock(stallRegistryMutex());
        std::erase(stallRegistry(), owner);
    }

    void InternalSwapchainImages::stallWatchdogLoop() {
        const int64_t started = steadyNanoseconds(present_diagnostics::Clock::now());
        // Dumps made in the current stall. A new present sets a new baseline and re-arms them.
        int dumps = 0;
        int64_t stallBaseline = started;
        for (;;) {
            std::this_thread::sleep_for(stallPollInterval);
            // Before the first application present, the stall is measured from the watchdog's start.
            const int64_t lastPresent = present_diagnostics::lastApplicationPresentNanoseconds();
            const int64_t baseline = lastPresent != 0 ? lastPresent : started;
            if (baseline != stallBaseline) {
                // A present arrived: the next stall is reported as well.
                stallBaseline = baseline;
                dumps = 0;
            }
            const int64_t stalled = steadyNanoseconds(present_diagnostics::Clock::now()) - baseline;
            // Dump number dumps + 1 is due once the stall has lasted 2 s, then 4 s, then 6 s.
            if (dumps == stallDumpsPerStall || stalled < stallNanoseconds * (dumps + 1))
                continue;
            ++dumps;
            // Holding the registry mutex keeps every listed owner alive until its dump ends.
            const std::lock_guard lock(stallRegistryMutex());
            for (const InternalSwapchainImages* owner : stallRegistry())
                owner->writeStallProbe(stalled);
            // D7c: the FG timelines of the swapchains, after their per-owner lines.
            present_diagnostics::writeFgTimelineProbe(stalled);
        }
    }

    // Diagnostics only. It never blocks the normal path: the queue mutex is try-locked for at
    // most 200 ms, with no lock held while it sleeps (the registry mutex that keeps this owner
    // alive stays held, as before), and the ring is try-locked by its own describe call. The
    // device queries read counters and fence status; none of them waits on the GPU.
    void InternalSwapchainImages::writeStallProbe(const int64_t stalledNanoseconds) const {
        const VkDevice device = impl->device;
        const auto& funcs = impl->funcs;
        // D7d: the dump time, the reference for every age in this line.
        const int64_t now = steadyNanoseconds(present_diagnostics::Clock::now());
        // A counter read; "unavailable" when the device does not expose the query.
        const auto counter = [&funcs, device](const VkSemaphore semaphore) -> std::string {
            if (!funcs.GetSemaphoreCounterValueKHR)
                return "unavailable";
            uint64_t value{};
            const VkResult result = funcs.GetSemaphoreCounterValueKHR(device, semaphore, &value);
            return result == VK_SUCCESS ? std::to_string(value)
                : "error" + std::to_string(static_cast<int>(result));
        };
        std::ostringstream line;
        line << "framegen-vulkan: d7 stall stall_ms=" << stalledNanoseconds / 1'000'000
             << " release_gpu=" << (impl->release ? counter(impl->release->handle()) : std::string{"none"})
             << " call_release_gpu="
             << (impl->callRelease ? counter(impl->callRelease->handle()) : std::string{"none"});

        // D7b: up to 40 try-locks, 5 ms apart. Only when every attempt fails is the locked
        // group reported as busy.
        std::unique_lock queueLock(impl->queue->mutex, std::defer_lock);
        int lockTries = 0;
        bool locked = false;
        while (!locked && lockTries < queueLockAttempts) {
            if (lockTries > 0)
                std::this_thread::sleep_for(queueLockRetryInterval);
            ++lockTries;
            locked = queueLock.try_lock();
        }
        line << " lock_tries=" << lockTries;
        // Held only while try-locked: the worker's submits and fence resets need it, so the
        // fields read under it cannot change during the dump.
        line << " last_release=" << (locked ? std::to_string(impl->lastReleaseValue) : std::string{"busy"});
        std::string lastCallText{"busy"};
        if (locked) {
            const std::lock_guard leafLock(impl->leaf);
            lastCallText = std::to_string(impl->lastCallReleaseValue);
        }
        line << " last_call_release=" << lastCallText;
        line << " acq_wait=" << impl->probeAcquireWait.load(std::memory_order_relaxed)
             << " acq_index=" << impl->probeAcquireIndex.load(std::memory_order_relaxed)
             << " acq_tail=" << impl->probeAcquireTail.load(std::memory_order_relaxed);
        // Read without the queue lock: the tail handle is fixed after device creation, and the
        // counter query takes no externally synchronised handle.
        uint64_t tailGpu = 0;
        bool tailKnown = false;
        if (impl->queue->tail && funcs.GetSemaphoreCounterValueKHR)
            tailKnown = funcs.GetSemaphoreCounterValueKHR(device, impl->queue->tail, &tailGpu) == VK_SUCCESS;
        line << " tail_gpu=" << (!impl->queue->tail ? std::string{"none"}
            : tailKnown ? std::to_string(tailGpu) : counter(impl->queue->tail));
        line << " tail_submitted=" << impl->queue->tailSubmitted.load(std::memory_order_acquire);
        // D7d: the lower call in flight and the latest stored acquire of each application index.
        // Read without the queue lock: the atomics are the only fields involved.
        const uint8_t callOp = impl->queue->callOp.load(std::memory_order_acquire);
        line << " call=";
        if (callOp == lowerOpNone) {
            line << "none";
        } else {
            const uint32_t callLower = impl->queue->callLower.load(std::memory_order_relaxed);
            const int64_t callSince = impl->queue->callSinceNs.load(std::memory_order_relaxed);
            line << lowerOpName(callOp) << ":l" << indexText(callLower) << ':' << agoText(now, callSince);
        }
        line << " acq_by_index=";
        const size_t indexed = std::min(impl->images.size(), stallProbeIndexSlots);
        for (size_t index = 0; index < indexed; ++index) {
            line << (index ? " " : "") << index << ':';
            const int64_t storedAt = impl->probeAtOf[index].load(std::memory_order_acquire);
            if (storedAt == 0) {
                line << '-';
                continue;
            }
            line << impl->probeWaitOf[index].load(std::memory_order_relaxed) << '/'
                 << impl->probeTailOf[index].load(std::memory_order_relaxed) << "/-"
                 << agoText(now, storedAt);
        }

        line << " ring=";
        if (const auto states = impl->ring.tryDescribe()) {
            for (size_t index = 0; index < states->size(); ++index) {
                const auto& [letter, value] = (*states)[index];
                line << (index ? " " : "") << index << ':' << letter << ':' << value;
            }
        } else {
            line << "busy";
        }

        // VK_SUCCESS (0) is signalled, VK_NOT_READY (1) is pending, and a negative value is an error.
        // Under the queue lock only: commitLowerAcquire resets these fences under it, and
        // vkResetFences externally synchronises them.
        line << " fences=";
        if (locked) {
            for (size_t slot = 0; slot < impl->lowerFences.size(); ++slot)
                line << (slot ? "," : "") << static_cast<int>(funcs.GetFenceStatus(device, impl->lowerFences[slot]));
        } else {
            line << "busy";
        }
        line << " lower_head=" << (locked ? std::to_string(impl->lowerHead) : std::string{"busy"});
        line << " worker_slot=" << (locked ? std::to_string(impl->workerSlot) : std::string{"busy"});
        if (locked) {
            // D7b: oldest first. The entry at recentNext is the oldest, and the next to be
            // overwritten, so the walk starts there. A zero tail is an entry never written.
            // Completion comes from tail_gpu, not the record's fence: a record can outlive the
            // swapchain that owned its fence. d = done (tail_gpu reached it), p = pending, ? = unknown.
            const DeferredPresentQueue& queue = *impl->queue;
            line << " recent=";
            bool first = true;
            for (size_t step = 0; step < queue.recent.size(); ++step) {
                const SubmitRecord& record = queue.recent[(queue.recentNext + step) % queue.recent.size()];
                if (record.tail == 0)
                    continue;
                line << (first ? "" : " ") << record.tail << ':' << submitKindName(record.kind)
                     << ":a" << indexText(record.applicationIndex)
                     << ":l" << indexText(record.lowerIndex)
                     << ":w" << record.waits << ':'
                     << (!tailKnown ? '?' : record.tail <= tailGpu ? 'd' : 'p');
                // D7c: the batch ordinal, and for a generated skip the reason it presented no image.
                if (record.source != 0)
                    line << ":s" << record.source;
                if (record.kind == submitKindGeneratedSkip)
                    line << ":r" << DeferredPresent::skipReasonName(record.reason);
                first = false;
            }
            // D7d: the finished lower calls, oldest first. A zero start marks an entry never written.
            line << " lower=";
            bool firstLower = true;
            for (size_t step = 0; step < queue.lower.size(); ++step) {
                const LowerRecord& record = queue.lower[(queue.lowerNext + step) % queue.lower.size()];
                if (record.startNs == 0)
                    continue;
                line << (firstLower ? "" : " ") << record.tail << ':' << lowerOpName(record.op)
                     << ":l" << indexText(record.lowerIndex) << ":r" << record.result
                     << ":s" << record.source << ":x" << lowSwapchainHex(record.swapchain)
                     << ":-" << agoText(now, record.startNs)
                     << ':' << millisecondsText(record.endNs - record.startNs, 2);
                firstLower = false;
            }
            // The queue mutex is released before the write, so a slow stderr never holds it.
            queueLock.unlock();
        }
        std::cerr << line.str() << '\n';
    }

    std::span<const VkImage> InternalSwapchainImages::images() const noexcept {
        return impl->images;
    }

    uint32_t InternalSwapchainImages::lowerImageCount() const noexcept {
        return static_cast<uint32_t>(impl->lowerImages.size());
    }

    uint32_t InternalSwapchainImages::applicationImageCount() const noexcept {
        return static_cast<uint32_t>(impl->images.size());
    }

    bool InternalSwapchainImages::isAcquired(const uint32_t index) const {
        return impl->ring.isAcquired(index);
    }

    VkResult InternalSwapchainImages::acquireApplicationImage(const uint64_t timeout,
            const VkSemaphore semaphore, const VkFence fence,
            const bool deferredSuboptimal, uint32_t* const imageIndex) {
        if (const auto sticky = impl->sticky.load(); sticky != VK_SUCCESS)
            return sticky;
        // The split is observed only with the pacing profile. Clock reads never
        // change the waits, locks or submissions below.
        const bool measured = present_diagnostics::pacingProfileEnabled();
        const auto ringStarted = measured ? present_diagnostics::Clock::now() : present_diagnostics::Clock::time_point{};
        // Waits without any lock held when the timeout is finite or unlimited.
        // complete() wakes the wait when a presented image finishes its copy.
        const auto index = impl->ring.acquireWait(timeout);
        if (!index) {
            // X1: an error that woke this wait is returned, not a timeout.
            if (const auto error = impl->sticky.load(); error != VK_SUCCESS)
                return error;
            return timeout == 0 ? VK_NOT_READY : VK_TIMEOUT;
        }

        // The release timeline orders the acquired image after its last copy. FI2b: the stored
        // stamp names the timeline that copy signalled (private or game queue), and the wait uses that one.
        // Safety of the wait value: an index returns to Free only through complete(),
        // which stores a value whose release-timeline signal submit already returned
        // VK_SUCCESS (submitCopyLocked and releaseLocked; after a failed submit, the last
        // value that was submitted), or through release/releaseAll, which keep the value
        // already stored. So the release signal is submitted before this acquire. The
        // index is Acquired by this call, so complete() cannot change its value between
        // the read and the signal.
        const ReleaseStamp stamp = impl->ring.releaseValueOf(*index);
        const uint64_t waitValue = stamp.value;
        const VkSemaphore waitHandle = impl->timelineHandle(stamp.timeline);

        // F10, host path. Both waited values are checked on the host, and counter reads never
        // wait. Once the release value and the tail value are reached, the GPU would pass both
        // waits at once, so the application's semaphore can be signalled on the host. The signal
        // is a temporary import of an already-signalled SYNC_FD payload (fd -1, the sync-fd rule
        // of the Vulkan specification; Mesa's WSI signals an acquire this way). The application's
        // acquire semaphore has no pending operations (VUID-vkAcquireNextImageKHR-semaphore-01286),
        // and a SYNC_FD import has copy transference, so it takes the temporary flag
        // (VUID-VkImportSemaphoreFdInfoKHR-handleType-03263).
        // Ordering: a queued submit runs its signal after every private batch submitted before it.
        // tailSubmitted is the last tail value whose submit returned VK_SUCCESS, so the tail counter
        // reaching it means every such batch has completed. A batch still inside its submit under
        // queue->mutex is not covered. The queued path waits for that lock; this path does not, the
        // same gap as F9c.
        const bool hostDisabled = impl->queue->hostAcquireDisabled.load(std::memory_order_acquire);
        const bool tailPresent = impl->queue->tail != VK_NULL_HANDLE;
        const bool functionsLoaded = impl->funcs.GetSemaphoreCounterValueKHR != nullptr &&
            impl->funcs.ImportSemaphoreFdKHR != nullptr;
        // Counters are read only when every other condition holds, and only for a nonzero value.
        const bool hostCandidate = fence == VK_NULL_HANDLE && semaphore != VK_NULL_HANDLE &&
            tailPresent && functionsLoaded && !hostDisabled;
        const uint64_t tailValue = hostCandidate
            ? impl->queue->tailSubmitted.load(std::memory_order_acquire) : 0;
        const uint64_t releaseCounter = hostCandidate && waitValue != 0
            ? timelineCounter(impl->funcs, impl->device, waitHandle) : 0;
        const uint64_t tailCounter = hostCandidate && tailValue != 0
            ? timelineCounter(impl->funcs, impl->device, impl->queue->tail) : 0;
        // When this acquire read the tail counter, for the per-acquire record. No extra read.
        const auto tailSeen = measured && tailCounter != 0
            ? present_diagnostics::Clock::now() : present_diagnostics::Clock::time_point{};
        bool signalledOnHost = false;
        if (hostAcquireEligible(fence, semaphore, hostDisabled, tailPresent, functionsLoaded,
                waitValue, releaseCounter, tailValue, tailCounter)) {
            if (measured)
                impl->noteAcquireProbe(*index, waitValue, tailValue);
            const VkImportSemaphoreFdInfoKHR importInfo{
                .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
                .semaphore = semaphore,
                .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
                .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
                .fd = -1,
            };
            const VkResult imported = impl->funcs.ImportSemaphoreFdKHR(impl->device, &importInfo);
            if (imported == VK_SUCCESS) {
                signalledOnHost = true;
                if (measured) {
                    present_diagnostics::recordPacingAcquire(true, ringStarted,
                        present_diagnostics::Clock::now(), {}, tailValue, tailSeen);
                }
            } else if (!impl->queue->hostAcquireDisabled.exchange(true)) {
                // A failed import is not an acquire error: this acquire takes the queued path.
                std::cerr << "framegen-vulkan: host acquire signal disabled: result="
                          << static_cast<int>(imported) << '\n';
            }
        }

        if (!signalledOnHost) {
            // F10, queued path: the acquire runs on the private queue under queue->mutex, as F8
            // ran it. It waits only the release value, on its timeline. No tail wait is needed for a
            // private value: this submit is on the queue that holds every earlier private batch, so
            // queue order runs its signal after them. The release value's signal was submitted before
            // (see above), so the wait satisfies VUID-vkQueueSubmit-pWaitSemaphores-03238.
            const auto lockRequested = measured ? present_diagnostics::Clock::now() : present_diagnostics::Clock::time_point{};
            const std::lock_guard lock(impl->queue->mutex);
            const auto lockAcquired = measured ? present_diagnostics::Clock::now() : present_diagnostics::Clock::time_point{};
            const auto lockWait = measured ? lockAcquired - lockRequested : present_diagnostics::Clock::duration{};
            VkResult submitted = VK_SUCCESS;
            const bool waitRelease = waitValue != 0;
            const bool signalSemaphore = semaphore != VK_NULL_HANDLE;
            if (waitRelease || signalSemaphore || fence != VK_NULL_HANDLE) {
                const uint32_t waitCount = waitRelease ? 1u : 0u;
                const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                const uint64_t zero = 0;
                const VkTimelineSemaphoreSubmitInfo timeline{
                    .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                    .waitSemaphoreValueCount = waitCount,
                    .pWaitSemaphoreValues = &waitValue,
                    .signalSemaphoreValueCount = signalSemaphore ? 1u : 0u,
                    .pSignalSemaphoreValues = &zero,
                };
                const VkSubmitInfo submit{
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .pNext = &timeline,
                    .waitSemaphoreCount = waitCount,
                    .pWaitSemaphores = &waitHandle,
                    .pWaitDstStageMask = &stage,
                    .commandBufferCount = 0,
                    .pCommandBuffers = nullptr,
                    .signalSemaphoreCount = signalSemaphore ? 1u : 0u,
                    .pSignalSemaphores = &semaphore,
                };
                if (measured)
                    impl->noteAcquireProbe(*index, waitValue, 0);
                submitted = impl->funcs.QueueSubmit(impl->queue->queue, 1, &submit, fence);
                if (submitted != VK_SUCCESS) {
                    impl->failSticky(submitted);
                    static_cast<void>(impl->ring.release(*index));
                }
            }
            if (measured) {
                present_diagnostics::recordPacingAcquire(false, ringStarted,
                    present_diagnostics::Clock::now(), lockWait, 0, {});
            }
            if (submitted != VK_SUCCESS)
                return submitted;
        }

        *imageIndex = *index;
        const bool flagged = impl->lowerSuboptimal.exchange(false);
        return deferredSuboptimal || flagged ? VK_SUBOPTIMAL_KHR : VK_SUCCESS;
    }

    VkResult InternalSwapchainImages::releaseApplicationImages(
            const std::span<const uint32_t> indices) {
        return impl->ring.releaseAll(indices) ? VK_SUCCESS : VK_ERROR_VALIDATION_FAILED_EXT;
    }

    VkResult InternalSwapchainImages::waitLowerAcquireSlot(const uint64_t timeout) {
        // A zero timeout is a poll. The deferred-present owner calls it with
        // the DeferredPresentQueue mutex held, so it must never sleep there.
        const VkFence fence = impl->lowerFences.at(impl->lowerHead);
        if (timeout == 0)
            return impl->funcs.GetFenceStatus(impl->device, fence);
        return impl->funcs.WaitForFences(impl->device, 1, &fence, VK_TRUE, timeout);
    }

    VkSemaphore InternalSwapchainImages::lowerAcquireSemaphore() const {
        return impl->lowerAcquire.at(impl->lowerHead).handle();
    }

    uint32_t InternalSwapchainImages::commitLowerAcquire() {
        const uint32_t slot = impl->lowerHead;
        const VkFence fence = impl->lowerFences.at(slot);
        static_cast<void>(impl->funcs.ResetFences(impl->device, 1, &fence));
        impl->lowerHead = static_cast<uint32_t>((slot + 1) % impl->lowerFences.size());
        return slot;
    }

    void InternalSwapchainImages::markPending(const uint32_t applicationIndex) {
        static_cast<void>(impl->ring.present(applicationIndex));
    }

    VkResult InternalSwapchainImages::consumeWithoutCopy(const uint32_t applicationIndex,
            const std::span<const VkSemaphore> applicationWaits, const VkFence presentFence) {
        markPending(applicationIndex);
        const std::lock_guard lock(impl->queue->mutex);
        const VkResult released = impl->releaseLocked(impl->queue->queue, applicationIndex, applicationWaits);
        if (presentFence != VK_NULL_HANDLE)
            static_cast<void>(impl->funcs.QueueSubmit(impl->queue->queue, 0, nullptr, presentFence));
        return released;
    }

    void InternalSwapchainImages::releaseNotHandedOff(const VkQueue applicationQueue,
            const uint32_t applicationIndex) {
        // R1: the private release timeline is signalled only on the private queue. The
        // application's queue is the one this present call was made on, so the empty
        // submit here is ordered after any source copy already submitted there. It
        // signals the handoff semaphore, and the private release waits it. The
        // ordering argument is the same as before R1, with the wait moved to the
        // private queue.
        const VkSemaphore handoff = impl->handoff.at(applicationIndex).handle();
        const VkSubmitInfo handoffSignal{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &handoff,
        };
        const VkResult signalled = impl->funcs.QueueSubmit(applicationQueue, 1, &handoffSignal, VK_NULL_HANDLE);
        {
            const std::lock_guard lock(impl->queue->mutex);
            if (signalled == VK_SUCCESS) {
                const std::array waits{handoff};
                static_cast<void>(impl->releaseLocked(impl->queue->queue, applicationIndex, waits));
            } else {
                // Nothing was signalled, so nothing waits. The error is kept as sticky below.
                static_cast<void>(impl->releaseLocked(impl->queue->queue, applicationIndex, {}));
            }
        }
        if (signalled != VK_SUCCESS)
            impl->failSticky(signalled);
        // Only a clear sticky becomes OUT_OF_DATE, so an earlier error is kept. X1: the
        // ring is woken only when this exchange stored the error.
        VkResult expected = VK_SUCCESS;
        if (impl->sticky.compare_exchange_strong(expected, VK_ERROR_OUT_OF_DATE_KHR))
            impl->ring.abort();
    }

    VkResult InternalSwapchainImages::ensureCopies(const vk::Vulkan& vk,
            const uint32_t applicationIndex) {
        try {
            for (uint32_t lowerIndex = 0; lowerIndex < impl->lowerImages.size(); ++lowerIndex)
                static_cast<void>(impl->copyCommandFor(vk, applicationIndex, lowerIndex));
            return VK_SUCCESS;
        } catch (const ls::vulkan_error& error) {
            return error.error();
        } catch (const std::exception&) {
            return VK_ERROR_UNKNOWN;
        }
    }

    VkResult InternalSwapchainImages::submitCopyLocked(const VkQueue queue,
            const uint32_t applicationIndex, const uint32_t lowerIndex, const uint32_t slot,
            const std::span<const VkSemaphore> waits) {
        VkResult result = VK_ERROR_UNKNOWN;
        // FI2b: the copy signals the release timeline of the queue it is submitted on. FP2: a game-queue submit may run
        // without queue->mutex. The leaf mutex guards the state it reads and writes, and no leaf section waits or submits.
        const ReleaseSignal signal = impl->releaseSignalOn(queue);
        try {
            const auto& recorded = impl->copies.at(
                static_cast<size_t>(applicationIndex) * impl->lowerImages.size() + lowerIndex);
            if (recorded) {
                const VkCommandBuffer command = recorded->handle();
                std::vector<VkSemaphore> allWaits(waits.begin(), waits.end());
                allWaits.push_back(impl->lowerAcquire.at(slot).handle());
                const std::vector<uint64_t> waitValues(allWaits.size(), 0);
                const std::vector<VkPipelineStageFlags> stages(allWaits.size(), VK_PIPELINE_STAGE_TRANSFER_BIT);
                const std::array<VkSemaphore, 2> signals{
                    impl->copied.at(lowerIndex).handle(), impl->timelineHandle(signal.timeline),
                };
                const std::array<uint64_t, 2> signalValues{0, signal.value};
                const VkTimelineSemaphoreSubmitInfo timeline{
                    .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                    .waitSemaphoreValueCount = static_cast<uint32_t>(waitValues.size()),
                    .pWaitSemaphoreValues = waitValues.data(),
                    .signalSemaphoreValueCount = static_cast<uint32_t>(signalValues.size()),
                    .pSignalSemaphoreValues = signalValues.data(),
                };
                const VkSubmitInfo submit{
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .pNext = &timeline,
                    .waitSemaphoreCount = static_cast<uint32_t>(allWaits.size()),
                    .pWaitSemaphores = allWaits.data(),
                    .pWaitDstStageMask = stages.data(),
                    .commandBufferCount = 1,
                    .pCommandBuffers = &command,
                    .signalSemaphoreCount = static_cast<uint32_t>(signals.size()),
                    .pSignalSemaphores = signals.data(),
                };
                const size_t pair = static_cast<size_t>(applicationIndex) * impl->lowerImages.size() + lowerIndex;
                if (impl->copyTimes) {
                    const std::lock_guard leafLock(impl->leaf);
                    collectCopyTimestampsLocked(pair);
                }
                result = queue == impl->queue->queue
                    ? submitPrivate(*impl->queue, impl->funcs.QueueSubmit, submit, impl->lowerFences.at(slot),
                        SubmitTag{.kind = submitKindCopy, .applicationIndex = applicationIndex, .lowerIndex = lowerIndex})
                    : impl->funcs.QueueSubmit(queue, 1, &submit, impl->lowerFences.at(slot));
                if (result == VK_SUCCESS) {
                    {
                        const std::lock_guard leafLock(impl->leaf);
                        if (impl->copyTimes && impl->copyTimed.at(pair)) {
                            impl->copyPending.at(pair) = 1;
                            impl->copyPendingValue.at(pair) = signal.value;
                            impl->copyPendingSource.at(pair) = applicationIndex < impl->images.size()
                                ? impl->copySource[applicationIndex].load(std::memory_order_relaxed) : 0;
                            impl->copyPendingGame.at(pair) = queue != impl->queue->queue ? 1 : 0;
                        }
                        impl->lastValueOf(signal.timeline) = signal.value;
                        // The consumed slot is free for acquire again once its fence signals (the worker checks it).
                        impl->parking.release(slot);
                    }
                    static_cast<void>(impl->ring.complete(applicationIndex, signal.value, signal.timeline));
                    return VK_SUCCESS;
                }
            }
        } catch (const std::exception&) {
            result = VK_ERROR_UNKNOWN;
        }
        // Nothing was submitted. Free the image at the last value of this copy's timeline, and keep the failure.
        impl->failSticky(result);
        {
            const std::lock_guard leafLock(impl->leaf);
            impl->parking.release(slot);
            static_cast<void>(impl->ring.complete(applicationIndex, impl->lastValueOf(signal.timeline), signal.timeline));
        }
        return result;
    }

    void InternalSwapchainImages::enableCopyTimestamps(const vk::Vulkan& vk,
            const PFN_vkGetCalibratedTimestampsKHR calibrate, const uint64_t context) noexcept {
        if (impl->copyTimesTried)
            return;
        impl->copyTimesTried = true;
        try {
            const size_t pairs = impl->copies.size();
            auto pool = std::make_unique<vk::TimestampQueryPool>(vk, static_cast<uint32_t>(pairs * 2));
            auto sources = std::make_unique<std::atomic<uint64_t>[]>(impl->images.size());
            const std::lock_guard lock(impl->queue->mutex);
            const std::lock_guard leafLock(impl->leaf);
            impl->copyClock.emplace(impl->device, calibrate, pool->timestampValidBits(),
                pool->timestampPeriodNanoseconds());
            impl->copyContext = context;
            impl->copyTimed.assign(pairs, 0);
            impl->copyPending.assign(pairs, 0);
            impl->copyPendingGame.assign(pairs, 0);
            impl->copyPendingValue.assign(pairs, 0);
            impl->copyPendingSource.assign(pairs, 0);
            impl->copySource = std::move(sources);
            impl->copyTimes = std::move(pool);
        } catch (const std::exception& error) {
            present_diagnostics::reportPacingGpuSplitUnavailable("real-copy-queries", error.what());
        } catch (...) {
            present_diagnostics::reportPacingGpuSplitUnavailable("real-copy-queries", "unknown error");
        }
    }

    void InternalSwapchainImages::noteCopySource(const uint32_t applicationIndex, const uint64_t source) noexcept {
        if (impl->copySource && applicationIndex < impl->images.size())
            impl->copySource[applicationIndex].store(source, std::memory_order_relaxed);
    }

    void InternalSwapchainImages::collectCopyTimestampsLocked(const size_t resubmitted) noexcept {
        if (!impl->copyTimes || !impl->copyClock)
            return;
        const auto now = present_diagnostics::Clock::now();
        if (!impl->copyClock->calibrated() || now - impl->copyClockAt >= std::chrono::seconds(1)) {
            impl->copyClockAt = now;
            static_cast<void>(impl->copyClock->recalibrate());
        }
        for (size_t pair = 0; pair < impl->copyPending.size(); ++pair) {
            if (!impl->copyPending[pair])
                continue;
            // FI2b rule 6: a pair's queries keep the timestamps of its previous run until this copy runs
            // (the copy's reset runs inside it), and those read back as available. Reading them early gave
            // game-queue records 100+ ms before their call. So a pending copy is read only once its timeline
            // has reached its value. A zero-timeout wait only polls, and VK_SUCCESS means the value was reached.
            const VkSemaphore timelineSemaphore = impl->timelineHandle(impl->copyPendingGame[pair]
                ? ReleaseTimeline::Call : ReleaseTimeline::Private);
            const VkSemaphoreWaitInfo ranInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                .semaphoreCount = 1,
                .pSemaphores = &timelineSemaphore,
                .pValues = &impl->copyPendingValue[pair],
            };
            const bool ran = impl->funcs.WaitSemaphoresKHR != nullptr &&
                impl->funcs.WaitSemaphoresKHR(impl->device, &ranInfo, 0) == VK_SUCCESS;
            bool available = false;
            int64_t startNs{}, endNs{};
            if (!ran) {
                if (pair != resubmitted)
                    continue; // still queued; read on a later submit
            } else {
                try {
                    if (const auto ticks = impl->copyTimes->tryTimestampsAt(static_cast<uint32_t>(pair * 2), 2);
                            ticks && impl->copyClock->calibrated()) {
                        startNs = impl->copyClock->monotonicNanoseconds(ticks->at(0));
                        endNs = impl->copyClock->monotonicNanoseconds(ticks->at(1));
                        available = startNs != 0 && endNs != 0;
                    } else if (!ticks && pair != resubmitted) {
                        continue; // still running; read on a later submit
                    }
                } catch (const std::exception&) {
                }
            }
            impl->copyPending[pair] = 0;
            present_diagnostics::logPacingRealCopyGpu(impl->copyContext, impl->copyPendingSource[pair],
                available, startNs, endNs, impl->copyPendingGame[pair] != 0);
        }
    }

    VkResult InternalSwapchainImages::materialize(const vk::Vulkan& vk,
            const InternalPresentImage& image, const std::span<const VkSemaphore> waits,
            uint32_t* const lowerIndex, VkSemaphore* const copied) {
        if (!image.lowerAcquire)
            return VK_ERROR_UNKNOWN;
        const VkResult recorded = ensureCopies(vk, image.applicationIndex);
        if (recorded != VK_SUCCESS)
            return recorded;

        // Synchronous paths reach here only after every older deferred batch was
        // joined, so no worker callback is using the lower-acquire slots.
        // FP2 (fixed): a parked image is consumed before any fresh acquire, whatever the park target. The copy waits the
        // parked slot's acquire semaphore and signals its fence, and submitCopyLocked frees the slot. So no fresh acquire
        // runs while an image is parked, and none targets a busy slot. A sticky owner drains its parked images instead.
        {
            const std::lock_guard queueLock(impl->queue->mutex);
            const bool sticky = impl->sticky.load() != VK_SUCCESS;
            std::optional<ParkedLower> entry;
            {
                const std::lock_guard leafLock(impl->leaf);
                if (acquireTakesParkedFirst(sticky, impl->parking.size()))
                    entry = impl->parking.take();
            }
            if (entry) {
                const VkResult taken = submitCopyLocked(impl->queue->queue, image.applicationIndex,
                    entry->lowerIndex, entry->slot, waits);
                if (taken == VK_SUCCESS) {
                    *lowerIndex = entry->lowerIndex;
                    if (copied)
                        *copied = impl->copied.at(entry->lowerIndex).handle();
                }
                return taken;
            }
            if (sticky)
                static_cast<void>(drainParkedLocked());
        }
        const uint32_t slot = impl->lowerHead;
        uint32_t index = 0;
        // D7d: this acquire runs without queue->mutex, so its clock reads are the only addition,
        // and only when history keeps the record.
        const bool noted = impl->queue->history;
        const int64_t acquireStartNs = noted ? steadyNanoseconds(present_diagnostics::Clock::now()) : 0;
        const VkResult acquired = image.lowerAcquire(UINT64_MAX,
            impl->lowerAcquire.at(slot).handle(), &index);
        const int64_t acquireEndNs = noted ? steadyNanoseconds(present_diagnostics::Clock::now()) : 0;
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
            noteLowerError(acquired);
            if (noted) {
                const std::lock_guard lock(impl->queue->mutex);
                impl->queue->noteLower(lowerOpAcquireSync, impl->lowerSwapchain, UINT32_MAX, acquired,
                    0, acquireStartNs, acquireEndNs);
            }
            return acquired < 0 ? acquired : VK_ERROR_UNKNOWN;
        }
        if (acquired == VK_SUBOPTIMAL_KHR)
            markLowerSuboptimal();

        VkResult result = VK_SUCCESS;
        {
            const std::lock_guard lock(impl->queue->mutex);
            impl->queue->noteLower(lowerOpAcquireSync, impl->lowerSwapchain, index, acquired,
                0, acquireStartNs, acquireEndNs);
            result = submitCopyLocked(impl->queue->queue, image.applicationIndex, index, slot, waits);
        }
        if (result == VK_SUCCESS) {
            *lowerIndex = index;
            if (copied)
                *copied = impl->copied.at(index).handle();
        }
        return result;
    }

    VkResult InternalSwapchainImages::workerTryAcquire(uint32_t* const lowerIndex, LowerPoll* const poll) {
        // FP2 rule 5: a parked image is taken before the worker acquires, so parking never starves the worker. Its slot
        // stays busy until the worker's submit releases it, and the worker's own slot is the parked one.
        {
            const std::lock_guard leafLock(impl->leaf);
            // Taken whatever the park target is: a target of zero keeps the parked images for this acquire (FP2 rule 6).
            if (acquireTakesParkedFirst(impl->sticky.load() != VK_SUCCESS, impl->parking.size())) {
                if (const auto entry = impl->parking.take()) {
                    *lowerIndex = entry->lowerIndex;
                    impl->workerSlot = entry->slot;
                    if (poll) *poll = LowerPoll::Acquired;
                    return VK_SUCCESS;
                }
            }
        }
        LowerPoll reason{};
        return tryAcquireLowerLocked(lowerOpAcquireReal, lowerIndex, poll ? poll : &reason);
    }

    VkResult InternalSwapchainImages::tryAcquireLowerLocked(const uint8_t op, uint32_t* const lowerIndex,
            LowerPoll* const poll) {
        *poll = LowerPoll::Error;
        if (const auto sticky = impl->sticky.load(); sticky < 0) {
            // FP2 rule 6: a sticky owner parks nothing, so its parked images are drained here, under the queue lock.
            static_cast<void>(drainParkedLocked());
            return sticky;
        }
        // FP2: the next slot is busy while its image is parked or a call owns it. Its fence is not read then.
        bool slotBusy{};
        {
            const std::lock_guard leafLock(impl->leaf);
            slotBusy = impl->parking.isBusy(impl->lowerHead);
        }
        if (slotBusy) {
            *poll = LowerPoll::SlotBusy;
            return VK_NOT_READY;
        }
        // Poll only: this runs under the queue lock and must never sleep.
        const VkFence fence = impl->lowerFences.at(impl->lowerHead);
        const VkResult ready = impl->funcs.GetFenceStatus(impl->device, fence);
        if (ready != VK_SUCCESS) {
            *poll = ready < 0 ? LowerPoll::Error : LowerPoll::Fence;
            return ready < 0 ? ready : VK_NOT_READY;
        }
        uint32_t index = 0;
        // D7d: timed and noted only with history. A poll that is not ready or timed out is not noted.
        const bool noted = impl->queue->history;
        const int64_t started = noted ? steadyNanoseconds(present_diagnostics::Clock::now()) : 0;
        impl->queue->beginCall(op, UINT32_MAX);
        const VkResult lower = impl->funcs.AcquireNextImageKHR(impl->device, impl->lowerSwapchain,
            0, impl->lowerAcquire.at(impl->lowerHead).handle(), VK_NULL_HANDLE, &index);
        impl->queue->endCall();
        if (noted && lower != VK_NOT_READY && lower != VK_TIMEOUT) {
            const bool gotImage = lower == VK_SUCCESS || lower == VK_SUBOPTIMAL_KHR;
            impl->queue->noteLower(op, impl->lowerSwapchain, gotImage ? index : UINT32_MAX,
                lower, 0, started, steadyNanoseconds(present_diagnostics::Clock::now()));
        }
        if (lower != VK_SUCCESS && lower != VK_SUBOPTIMAL_KHR) {
            noteLowerError(lower);
            *poll = lower == VK_NOT_READY || lower == VK_TIMEOUT ? LowerPoll::Acquire : LowerPoll::Error;
            return lower;
        }
        *lowerIndex = index;
        impl->workerSlot = commitLowerAcquire();
        if (lower == VK_SUBOPTIMAL_KHR)
            markLowerSuboptimal();
        *poll = LowerPoll::Acquired;
        return VK_SUCCESS;
    }

    VkResult InternalSwapchainImages::callPrepare(const VkQueue callQueue, const uint32_t callFamily,
            const uint32_t applicationIndex, const VkSemaphore continuation,
            uint32_t* const lowerIndex, VkSemaphore* const presentWait, bool* const consumed) {
        *consumed = false;
        // The copy must be recorded for the game queue's family (rule 5). The check runs before the lower
        // acquire, so a refusal takes no lower image. FP1: a refusal is an error, which counts as other, not not-ready.
        if (impl->copyFamily != callFamily)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        uint32_t index = 0;
        LowerPoll poll{};
        const VkResult acquired = tryAcquireLowerLocked(lowerOpAcquireCall, &index, &poll);
        if (acquired != VK_SUCCESS)
            return acquired;
        *lowerIndex = index;
        // From here the application image is taken: submitCopyLocked frees it on success and on failure.
        *consumed = true;
        const std::array waits{continuation};
        const VkResult submitted = submitCopyLocked(callQueue, applicationIndex, index, impl->workerSlot, waits);
        if (submitted == VK_SUCCESS)
            *presentWait = impl->copied.at(index).handle();
        return submitted;
    }

    VkResult InternalSwapchainImages::callPrepareParked(const VkQueue callQueue, const uint32_t callFamily,
            const uint32_t applicationIndex, const VkSemaphore continuation,
            uint32_t* const lowerIndex, VkSemaphore* const presentWait, bool* const consumed) {
        *consumed = false;
        if (impl->copyFamily != callFamily)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        std::optional<ParkedLower> entry;
        {
            // The slot stays busy after the take. submitCopyLocked releases it once the copy is submitted or has failed.
            const std::lock_guard leafLock(impl->leaf);
            if (const VkResult sticky = impl->sticky.load(); sticky != VK_SUCCESS)
                return sticky;
            entry = impl->parking.take();
        }
        if (!entry)
            return VK_NOT_READY;
        *lowerIndex = entry->lowerIndex;
        // From here the application image is taken: submitCopyLocked frees it on success and on failure.
        *consumed = true;
        const std::array waits{continuation};
        const VkResult submitted = submitCopyLocked(callQueue, applicationIndex, entry->lowerIndex, entry->slot, waits);
        if (submitted == VK_SUCCESS)
            *presentWait = impl->copied.at(entry->lowerIndex).handle();
        return submitted;
    }

    VkSwapchainKHR InternalSwapchainImages::lowerSwapchainHandle() const noexcept {
        return impl->lowerSwapchain;
    }

    void InternalSwapchainImages::setParkPolicy(const uint32_t target, const uint32_t budget,
            const uint32_t callFamily) {
        const std::lock_guard leafLock(impl->leaf);
        impl->parking.setPolicy(target, budget, impl->copyFamily == callFamily);
    }

    size_t InternalSwapchainImages::parkedCount() const {
        const std::lock_guard leafLock(impl->leaf);
        return impl->parking.size();
    }

    size_t InternalSwapchainImages::outstandingCallOutputs() const {
        const std::lock_guard leafLock(impl->leaf);
        return impl->parking.size() + impl->callInFlight;
    }

    void InternalSwapchainImages::beginCallOutput() {
        const std::lock_guard leafLock(impl->leaf);
        ++impl->callInFlight;
    }

    void InternalSwapchainImages::endCallOutput() noexcept {
        const std::lock_guard leafLock(impl->leaf);
        if (impl->callInFlight > 0)
            --impl->callInFlight;
    }

    std::optional<ParkedLowerTake> InternalSwapchainImages::takeParked() {
        const std::lock_guard leafLock(impl->leaf);
        if (impl->sticky.load() != VK_SUCCESS)
            return std::nullopt;
        const auto entry = impl->parking.take();
        if (!entry)
            return std::nullopt;
        ++impl->callInFlight;
        return ParkedLowerTake{
            .lowerIndex = entry->lowerIndex,
            .slot = entry->slot,
            .semaphore = impl->lowerAcquire.at(entry->slot).handle(),
        };
    }

    VkResult InternalSwapchainImages::finishParkedCopy(const VkQueue gameQueue, const uint32_t slot) {
        // The copy that waited this slot was submitted on gameQueue just before, so this empty submit runs after it.
        // The fence signals when that copy has completed, which is when the slot may be acquired again.
        const VkResult signalled = impl->funcs.QueueSubmit(gameQueue, 0, nullptr, impl->lowerFences.at(slot));
        if (signalled != VK_SUCCESS) {
            impl->failSticky(signalled);
            return signalled;
        }
        const std::lock_guard leafLock(impl->leaf);
        impl->parking.release(slot);
        return VK_SUCCESS;
    }

    void InternalSwapchainImages::failParkedTake(const uint32_t slot) noexcept {
        // The slot stays busy: its fence is never submitted, so nothing may acquire into it.
        static_cast<void>(slot);
        impl->failSticky(VK_ERROR_UNKNOWN);
    }

    VkResult InternalSwapchainImages::drainParkedLocked() {
        // FP2 rule 6, caller holds queue->mutex. Each parked image gets one submit on the private queue: it waits the
        // image's acquire semaphore, signals the slot fence, and signals the release timeline, so the destructor and
        // retireParked wait it. The slot is freed after the submit returns.
        // Callers are retireParked, the destructor, and the sticky error paths (tryAcquireLowerLocked, parkAfterPresent,
        // materialize) only. Never on a park target change: a drained image stays acquired until the lower swapchain is
        // destroyed, so draining on a target change would leave the application one lower image short for the rest of the
        // swapchain's life. Repeated park, target-zero and acquire cycles would then starve the acquire.
        VkResult result = VK_SUCCESS;
        for (;;) {
            std::optional<ParkedLower> entry;
            {
                const std::lock_guard leafLock(impl->leaf);
                entry = impl->parking.take();
            }
            if (!entry)
                break;
            const VkSemaphore wait = impl->lowerAcquire.at(entry->slot).handle();
            const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            const uint64_t value = impl->lastReleaseValue + 1;
            const VkSemaphore releaseHandle = impl->release->handle();
            const VkTimelineSemaphoreSubmitInfo timeline{
                .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                .signalSemaphoreValueCount = 1,
                .pSignalSemaphoreValues = &value,
            };
            const VkSubmitInfo submit{
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext = &timeline,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &wait,
                .pWaitDstStageMask = &stage,
                .commandBufferCount = 0,
                .pCommandBuffers = nullptr,
                .signalSemaphoreCount = 1,
                .pSignalSemaphores = &releaseHandle,
            };
            const VkResult submitted = submitPrivate(*impl->queue, impl->funcs.QueueSubmit, submit,
                impl->lowerFences.at(entry->slot), SubmitTag{.kind = submitKindRelease});
            if (submitted == VK_SUCCESS) {
                impl->lastReleaseValue = value;
                impl->lastDrainValue = value;
            } else {
                impl->failSticky(submitted);
                result = submitted;
            }
            const std::lock_guard leafLock(impl->leaf);
            impl->parking.release(entry->slot);
        }
        return result;
    }

    ParkOutcome InternalSwapchainImages::parkAfterPresent(const size_t callHeld) {
        // FP2 rule 2, caller holds queue->mutex right after a lower present of the worker. The decision is
        // parkAfterPresentAction: a sticky owner drains its parked images (the error path). A park target of zero drains
        // nothing and parks nothing, so the parked images stay in the pool for the next acquire (FP2 rule 6, fixed).
        const bool sticky = impl->sticky.load() != VK_SUCCESS;
        ParkOutcome outcome = ParkOutcome::Drained;
        {
            const std::lock_guard leafLock(impl->leaf);
            // The in-flight direct outputs hold lower images too, so the budget counts them (FP2 rule 2).
            outcome = parkAfterPresentAction(sticky, impl->parking.size(), callHeld + impl->callInFlight,
                impl->parking.target(), impl->parking.budgetOutputs(), impl->parking.isBusy(impl->lowerHead));
        }
        if (outcome == ParkOutcome::Drained) {
            static_cast<void>(drainParkedLocked());
            return outcome;
        }
        if (outcome != ParkOutcome::Parked)
            return outcome;
        uint32_t index{};
        LowerPoll poll{};
        // The acquire commits workerSlot. It is zero-timeout, so the worker never sleeps here. FP3: a failed acquire
        // reports why (busy slot, unsignalled fence, NOT_READY or TIMEOUT, or error) for the park counters.
        if (tryAcquireLowerLocked(lowerOpAcquirePark, &index, &poll) != VK_SUCCESS)
            return parkOutcomeOfPoll(poll);
        const std::lock_guard leafLock(impl->leaf);
        const bool parked = impl->parking.push(ParkedLower{.lowerIndex = index, .slot = impl->workerSlot});
        assert(parked && "FP2: parking outside the budget");
        static_cast<void>(parked);
        return ParkOutcome::Parked;
    }

    VkResult InternalSwapchainImages::retireParked(const uint64_t timeoutNs) {
        uint64_t drained = 0;
        {
            const std::lock_guard queueLock(impl->queue->mutex);
            if (impl->parking.size() > 0)
                static_cast<void>(drainParkedLocked());
            drained = impl->lastDrainValue;
        }
        if (drained == 0)
            return VK_SUCCESS;
        // Waits the last drain's value only. The drains run in queue order, so this covers each of them.
        const VkSemaphore handle = impl->release->handle();
        const VkSemaphoreWaitInfo waitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &handle,
            .pValues = &drained,
        };
        return impl->funcs.WaitSemaphoresKHR(impl->device, &waitInfo, timeoutNs);
    }

    VkResult InternalSwapchainImages::workerSubmit(const VkQueue queue,
            const uint32_t applicationIndex, const uint32_t lowerIndex,
            const VkSemaphore continuation, VkSemaphore* const presentWait) {
        const std::array waits{continuation};
        const VkResult result = submitCopyLocked(queue, applicationIndex, lowerIndex,
            impl->workerSlot, waits);
        if (result == VK_SUCCESS && presentWait)
            *presentWait = impl->copied.at(lowerIndex).handle();
        return result;
    }

    VkResult InternalSwapchainImages::workerAbandon(const VkQueue queue,
            const uint32_t applicationIndex, const VkSemaphore continuation) {
        const std::array waits{continuation};
        return impl->releaseLocked(queue, applicationIndex, waits);
    }

    VkResult internalRealTryAcquire(void* const owner, uint32_t* const lowerIndex, LowerPoll* const poll) {
        return static_cast<InternalSwapchainImages*>(owner)->workerTryAcquire(lowerIndex, poll);
    }

    VkResult internalRealSubmit(void* const owner, const VkQueue queue,
            const uint32_t applicationIndex, const uint32_t lowerIndex,
            const VkSemaphore continuation, VkSemaphore* const presentWait) {
        return static_cast<InternalSwapchainImages*>(owner)->workerSubmit(
            queue, applicationIndex, lowerIndex, continuation, presentWait);
    }

    VkResult internalRealAbandon(void* const owner, const VkQueue queue,
            const uint32_t applicationIndex, const VkSemaphore continuation) {
        return static_cast<InternalSwapchainImages*>(owner)->workerAbandon(
            queue, applicationIndex, continuation);
    }

    VkResult internalRealCallPrepareParked(void* const owner, const VkQueue callQueue, const uint32_t callFamily,
            const uint32_t applicationIndex, const VkSemaphore continuation, uint32_t* const lowerIndex,
            VkSemaphore* const presentWait, bool* const consumed) {
        return static_cast<InternalSwapchainImages*>(owner)->callPrepareParked(callQueue, callFamily,
            applicationIndex, continuation, lowerIndex, presentWait, consumed);
    }

    size_t internalRealParkedCount(void* const owner) {
        return static_cast<const InternalSwapchainImages*>(owner)->outstandingCallOutputs();
    }

    ParkOutcome internalRealParkAfterPresent(void* const owner, const size_t callHeld) {
        return static_cast<InternalSwapchainImages*>(owner)->parkAfterPresent(callHeld);
    }

    VkResult internalRealCallPrepare(void* const owner, const VkQueue callQueue, const uint32_t callFamily,
            const uint32_t applicationIndex, const VkSemaphore continuation, uint32_t* const lowerIndex,
            VkSemaphore* const presentWait, bool* const consumed) {
        return static_cast<InternalSwapchainImages*>(owner)->callPrepare(callQueue, callFamily,
            applicationIndex, continuation, lowerIndex, presentWait, consumed);
    }

    VkResult InternalSwapchainImages::stickyError() const noexcept {
        return impl->sticky.load();
    }

    void InternalSwapchainImages::noteLowerError(const VkResult error) noexcept {
        if (error == VK_ERROR_OUT_OF_DATE_KHR || error == VK_ERROR_SURFACE_LOST_KHR ||
                error == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
            impl->failSticky(error);
        }
    }

    void InternalSwapchainImages::markLowerSuboptimal() noexcept {
        impl->lowerSuboptimal.store(true);
    }

    InternalImageDeviceSupport queryInternalImageSupport(const vk::Vulkan& vk,
            const VkFormat format, const VkImageUsageFlags usage,
            const VkImageCreateFlags flags, const std::span<const VkFormat> viewFormats) {
        InternalImageDeviceSupport support{};
        support.formatBlitCapable = vk.supportsOptimalTilingFormatFeatures(
            format, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT);
        const auto query = vk.fi().GetPhysicalDeviceImageFormatProperties2;
        if (!query)
            return support;
        // The list is queried only with the mutable bit, the same condition createImages uses.
        const VkImageFormatListCreateInfo formatList{
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
            .pNext = nullptr,
            .viewFormatCount = static_cast<uint32_t>(viewFormats.size()),
            .pViewFormats = viewFormats.data(),
        };
        const bool chainFormatList = (flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0 &&
            !viewFormats.empty();
        const VkPhysicalDeviceImageFormatInfo2 formatInfo{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .pNext = chainFormatList ? &formatList : nullptr,
            .format = format,
            .type = VK_IMAGE_TYPE_2D,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .flags = flags,
        };
        VkImageFormatProperties2 properties{
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        };
        support.imageFormatSupported =
            query(vk.physdev(), &formatInfo, &properties) == VK_SUCCESS;
        return support;
    }
}

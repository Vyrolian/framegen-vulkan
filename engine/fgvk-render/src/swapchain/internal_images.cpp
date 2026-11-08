/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain/internal_images.hpp"

#include "swapchain/deferred_present.hpp"
#include "present_diagnostics.hpp"
#include "fgvk-common/helpers/errors.hpp"
#include "fgvk-common/vulkan/command_buffer.hpp"
#include "fgvk-common/vulkan/device_memory_accounting.hpp"
#include "fgvk-common/vulkan/semaphore.hpp"
#include "fgvk-common/vulkan/timeline_semaphore.hpp"
#include "fgvk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
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
    }

    struct InternalSwapchainImages::Impl {
        VkDevice device{VK_NULL_HANDLE};
        vk::VulkanDeviceFuncs funcs{};
        std::shared_ptr<vk::DeviceMemoryAccounting> accounting;
        std::shared_ptr<DeferredPresentQueue> queue;
        VkSwapchainKHR lowerSwapchain{VK_NULL_HANDLE};
        VkExtent2D extent{};
        InternalImageRing ring;

        std::vector<VkImage> images;
        std::vector<VkDeviceMemory> memories;
        std::vector<VkDeviceSize> memorySizes;
        std::vector<VkImage> lowerImages;

        std::optional<vk::TimelineSemaphore> release;
        // Serialised by queue->mutex: the release value and everything the copy and
        // release paths write. The emulated acquire never touches these. It reads only
        // the ring (its own mutex) and the release handle, fixed at creation, and it
        // submits under acquireTarget()'s mutex, never queue->mutex.
        uint64_t lastReleaseValue{0};
        std::vector<vk::Semaphore> copied;
        // R1: one binary semaphore per application image. releaseNotHandedOff signals it on
        // the application queue and the private release waits it, so the release timeline is
        // only ever signalled on the private queue.
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
              accounting(vk.deviceMemoryAccounting()),
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
        // submitQueue, after waits, and frees applicationIndex at that value. Every
        // caller passes the private queue, which also signals the F9c tail. On
        // failure the image is freed at the last submitted value and the error is sticky.
        VkResult releaseLocked(VkQueue submitQueue, uint32_t applicationIndex,
                std::span<const VkSemaphore> waits);
    };

    bool InternalSwapchainImages::Impl::initialize(const vk::Vulkan& vk,
            const VkSwapchainCreateInfoKHR& application,
            const VkExtent2D applicationExtent,
            const std::span<const VkImage> lowerHandles,
            const uint32_t imageCount) {
        extent = applicationExtent;
        lowerImages.assign(lowerHandles.begin(), lowerHandles.end());
        const size_t lowerCount = lowerImages.size();

        if (!createImages(vk, application, imageCount))
            return false;

        release.emplace(vk, 0);
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
        copies.resize(static_cast<size_t>(imageCount) * lowerCount);
        return true;
    }

    bool InternalSwapchainImages::Impl::createImages(const vk::Vulkan& vk,
            const VkSwapchainCreateInfoKHR& application, const uint32_t imageCount) {
        images.reserve(imageCount);
        memories.reserve(imageCount);
        memorySizes.reserve(imageCount);
        for (uint32_t i = 0; i < imageCount; ++i) {
            const VkImageCreateInfo imageInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
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
            memorySizes.push_back(requirements.size);
            accounting->recordAllocation(vk::DeviceMemoryKind::Internal, requirements.size);
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
                // The application leaves its image in PRESENT_SRC_KHR. The
                // lower image is discarded because the whole extent is written.
                const std::array preBarriers{
                    barrier(source, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                    barrier(target, VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
                };
                const std::array postBarriers{
                    barrier(source, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
                    barrier(target, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
                };
                slot->blitImage(vk, preBarriers, {source, target}, extent, postBarriers);
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
        const uint64_t value = lastReleaseValue + 1;
        const std::vector<VkPipelineStageFlags> stages(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        const std::vector<uint64_t> waitValues(waits.size(), 0);
        const VkSemaphore releaseHandle = release->handle();
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
        // Every release now targets the private queue, so it also signals the F9c tail.
        const VkResult result = submitQueue == queue->queue
            ? submitPrivate(*queue, funcs.QueueSubmit, submit, VK_NULL_HANDLE,
                SubmitTag{.kind = submitKindRelease, .applicationIndex = applicationIndex})
            : funcs.QueueSubmit(submitQueue, 1, &submit, VK_NULL_HANDLE);
        if (result == VK_SUCCESS) {
            lastReleaseValue = value;
            static_cast<void>(ring.complete(applicationIndex, value));
        } else {
            sticky.store(result);
            static_cast<void>(ring.complete(applicationIndex, lastReleaseValue));
        }
        return result;
    }

    InternalSwapchainImages::Impl::~Impl() {
        // Wait for the last copy so no pending GPU work reads the images or
        // signals the semaphores that are destroyed below.
        if (release && lastReleaseValue != 0) {
            const VkSemaphore handle = release->handle();
            const VkSemaphoreWaitInfo waitInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                .semaphoreCount = 1,
                .pSemaphores = &handle,
                .pValues = &lastReleaseValue,
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
            accounting->recordFree(vk::DeviceMemoryKind::Internal, memorySizes[i]);
        }
        memories.clear();
        memorySizes.clear();
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
            if (!stallWatchdogStarted()) {
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
             << " release_gpu=" << (impl->release ? counter(impl->release->handle()) : std::string{"none"});

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
        if (!index)
            return timeout == 0 ? VK_NOT_READY : VK_TIMEOUT;
        const auto lockStarted = measured ? present_diagnostics::Clock::now() : present_diagnostics::Clock::time_point{};
        const auto ringWait = measured ? lockStarted - ringStarted : present_diagnostics::Clock::duration{};

        // The release timeline orders the acquired image after its last copy.
        // Safety of the wait value: an index returns to Free only through complete(),
        // which stores a value whose release-timeline signal submit already returned
        // VK_SUCCESS (submitCopyLocked and releaseLocked; after a failed submit, the last
        // value that was submitted), or through release/releaseAll, which keep the value
        // already stored. So the release signal is submitted before this submit. The
        // application's binary semaphore, signalled here, depends on that wait, and
        // VUID-vkQueueSubmit-pWaitSemaphores-03238 requires every signal it depends on
        // to be submitted before a binary wait on it. The timeline wait may cross queues.
        // The index is Acquired by this call, so complete() cannot change its value
        // between the read and the submit. The application's semaphore and fence are
        // signalled on the acquire queue, which no other submit uses. Without a second
        // queue the submit runs on the present queue under its mutex, as before F9.
        // F9c: with a second queue the submit also waits the private tail at
        // tailSubmitted, the last value whose signal submit returned VK_SUCCESS, so the
        // same VUID holds for that wait. A same-queue acquire needs no tail wait: queue
        // order already runs it after every earlier private batch, and so does the
        // tail wait on the acquire queue, which restores that ordering.
        // D5's internal_queue_lock_* fields keep their names. They now measure the wait
        // for submitMutex, the acquire queue's lock (or the present queue's lock).
        {
            const auto [submitQueue, submitMutex] = impl->queue->acquireTarget();
            const std::lock_guard lock(submitMutex);
            const auto lockAcquired = measured ? present_diagnostics::Clock::now() : present_diagnostics::Clock::time_point{};
            const auto lockWait = measured ? lockAcquired - lockStarted : present_diagnostics::Clock::duration{};
            const uint64_t waitValue = impl->ring.releaseValueOf(*index);
            const bool waitRelease = waitValue != 0;
            const bool signalSemaphore = semaphore != VK_NULL_HANDLE;
            const bool tailWait = impl->queue->acquireQueue != VK_NULL_HANDLE &&
                impl->queue->tail != VK_NULL_HANDLE;
            const uint64_t tailValue = tailWait
                ? impl->queue->tailSubmitted.load(std::memory_order_acquire) : 0;
            present_diagnostics::Clock::duration submitWait{};
            VkResult submitted = VK_SUCCESS;
            if (waitRelease || signalSemaphore || fence != VK_NULL_HANDLE) {
                const VkSemaphore releaseHandle = impl->release->handle();
                std::array<VkSemaphore, 2> waitSemaphores{};
                std::array<uint64_t, 2> waitValues{};
                uint32_t waitCount = 0;
                if (waitRelease) {
                    waitSemaphores[waitCount] = releaseHandle;
                    waitValues[waitCount++] = waitValue;
                }
                if (tailValue != 0) {
                    waitSemaphores[waitCount] = impl->queue->tail;
                    waitValues[waitCount++] = tailValue;
                }
                const std::array<VkPipelineStageFlags, 2> stages{
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
                const uint64_t zero = 0;
                const VkTimelineSemaphoreSubmitInfo timeline{
                    .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                    .waitSemaphoreValueCount = waitCount,
                    .pWaitSemaphoreValues = waitValues.data(),
                    .signalSemaphoreValueCount = signalSemaphore ? 1u : 0u,
                    .pSignalSemaphoreValues = &zero,
                };
                const VkSubmitInfo submit{
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .pNext = &timeline,
                    .waitSemaphoreCount = waitCount,
                    .pWaitSemaphores = waitSemaphores.data(),
                    .pWaitDstStageMask = stages.data(),
                    .commandBufferCount = 0,
                    .pCommandBuffers = nullptr,
                    .signalSemaphoreCount = signalSemaphore ? 1u : 0u,
                    .pSignalSemaphores = &semaphore,
                };
                if (measured) {
                    impl->probeAcquireWait.store(waitValue, std::memory_order_relaxed);
                    impl->probeAcquireIndex.store(*index, std::memory_order_relaxed);
                    impl->probeAcquireTail.store(tailValue, std::memory_order_relaxed);
                    // D7d: the same values for this application index. The time is stored last.
                    if (*index < stallProbeIndexSlots) {
                        impl->probeWaitOf[*index].store(waitValue, std::memory_order_relaxed);
                        impl->probeTailOf[*index].store(tailValue, std::memory_order_relaxed);
                        impl->probeAtOf[*index].store(steadyNanoseconds(present_diagnostics::Clock::now()),
                            std::memory_order_release);
                    }
                }
                const auto submitStarted = measured ? present_diagnostics::Clock::now() : present_diagnostics::Clock::time_point{};
                submitted = impl->funcs.QueueSubmit(submitQueue, 1, &submit, fence);
                if (measured)
                    submitWait = present_diagnostics::Clock::now() - submitStarted;
                if (submitted != VK_SUCCESS) {
                    impl->sticky.store(submitted);
                    static_cast<void>(impl->ring.release(*index));
                }
            }
            if (measured)
                present_diagnostics::recordInternalAcquire(ringWait, lockWait, submitWait);
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
            const std::span<const VkSemaphore> applicationWaits) {
        markPending(applicationIndex);
        const std::lock_guard lock(impl->queue->mutex);
        return impl->releaseLocked(impl->queue->queue, applicationIndex, applicationWaits);
    }

    void InternalSwapchainImages::releaseNotHandedOff(const VkQueue applicationQueue,
            const uint32_t applicationIndex) {
        // R1: the release timeline is signalled only on the private queue. The
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
            impl->sticky.store(signalled);
        VkResult expected = VK_SUCCESS;
        impl->sticky.compare_exchange_strong(expected, VK_ERROR_OUT_OF_DATE_KHR);
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
        try {
            const auto& recorded = impl->copies.at(
                static_cast<size_t>(applicationIndex) * impl->lowerImages.size() + lowerIndex);
            if (recorded) {
                const VkCommandBuffer command = recorded->handle();
                std::vector<VkSemaphore> allWaits(waits.begin(), waits.end());
                allWaits.push_back(impl->lowerAcquire.at(slot).handle());
                const std::vector<VkPipelineStageFlags> stages(allWaits.size(), VK_PIPELINE_STAGE_TRANSFER_BIT);
                const std::vector<uint64_t> waitValues(allWaits.size(), 0);
                const uint64_t value = impl->lastReleaseValue + 1;
                const std::array<VkSemaphore, 2> signals{
                    impl->copied.at(lowerIndex).handle(), impl->release->handle(),
                };
                const std::array<uint64_t, 2> signalValues{0, value};
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
                result = queue == impl->queue->queue
                    ? submitPrivate(*impl->queue, impl->funcs.QueueSubmit, submit, impl->lowerFences.at(slot),
                        SubmitTag{.kind = submitKindCopy, .applicationIndex = applicationIndex, .lowerIndex = lowerIndex})
                    : impl->funcs.QueueSubmit(queue, 1, &submit, impl->lowerFences.at(slot));
                if (result == VK_SUCCESS) {
                    impl->lastReleaseValue = value;
                    static_cast<void>(impl->ring.complete(applicationIndex, value));
                    return VK_SUCCESS;
                }
            }
        } catch (const std::exception&) {
            result = VK_ERROR_UNKNOWN;
        }
        // Nothing was submitted. Free the image at the last value that was, and keep the failure.
        impl->sticky.store(result);
        static_cast<void>(impl->ring.complete(applicationIndex, impl->lastReleaseValue));
        return result;
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

    VkResult InternalSwapchainImages::workerTryAcquire(uint32_t* const lowerIndex) {
        if (const auto sticky = impl->sticky.load(); sticky < 0)
            return sticky;
        // Poll only: this runs under the queue lock and must never sleep.
        const VkFence fence = impl->lowerFences.at(impl->lowerHead);
        const VkResult ready = impl->funcs.GetFenceStatus(impl->device, fence);
        if (ready != VK_SUCCESS)
            return ready < 0 ? ready : VK_NOT_READY;
        uint32_t index = 0;
        // D7d: timed and noted only with history. A poll that is not ready or timed out is not noted.
        const bool noted = impl->queue->history;
        const int64_t started = noted ? steadyNanoseconds(present_diagnostics::Clock::now()) : 0;
        impl->queue->beginCall(lowerOpAcquireReal, UINT32_MAX);
        const VkResult lower = impl->funcs.AcquireNextImageKHR(impl->device, impl->lowerSwapchain,
            0, impl->lowerAcquire.at(impl->lowerHead).handle(), VK_NULL_HANDLE, &index);
        impl->queue->endCall();
        if (noted && lower != VK_NOT_READY && lower != VK_TIMEOUT) {
            const bool gotImage = lower == VK_SUCCESS || lower == VK_SUBOPTIMAL_KHR;
            impl->queue->noteLower(lowerOpAcquireReal, impl->lowerSwapchain, gotImage ? index : UINT32_MAX,
                lower, 0, started, steadyNanoseconds(present_diagnostics::Clock::now()));
        }
        if (lower != VK_SUCCESS && lower != VK_SUBOPTIMAL_KHR) {
            noteLowerError(lower);
            return lower;
        }
        *lowerIndex = index;
        impl->workerSlot = commitLowerAcquire();
        if (lower == VK_SUBOPTIMAL_KHR)
            markLowerSuboptimal();
        return VK_SUCCESS;
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

    VkResult internalRealTryAcquire(void* const owner, uint32_t* const lowerIndex) {
        return static_cast<InternalSwapchainImages*>(owner)->workerTryAcquire(lowerIndex);
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

    VkResult InternalSwapchainImages::stickyError() const noexcept {
        return impl->sticky.load();
    }

    void InternalSwapchainImages::noteLowerError(const VkResult error) noexcept {
        if (error == VK_ERROR_OUT_OF_DATE_KHR || error == VK_ERROR_SURFACE_LOST_KHR ||
                error == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
            impl->sticky.store(error);
        }
    }

    void InternalSwapchainImages::markLowerSuboptimal() noexcept {
        impl->lowerSuboptimal.store(true);
    }

    InternalImageDeviceSupport queryInternalImageSupport(const vk::Vulkan& vk,
            const VkFormat format, const VkImageUsageFlags usage) {
        InternalImageDeviceSupport support{};
        support.formatBlitCapable = vk.supportsOptimalTilingFormatFeatures(
            format, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT);
        const auto query = vk.fi().GetPhysicalDeviceImageFormatProperties2;
        if (!query)
            return support;
        const VkPhysicalDeviceImageFormatInfo2 formatInfo{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .format = format,
            .type = VK_IMAGE_TYPE_2D,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .flags = 0,
        };
        VkImageFormatProperties2 properties{
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        };
        support.imageFormatSupported =
            query(vk.physdev(), &formatInfo, &properties) == VK_SUCCESS;
        return support;
    }

    bool internalSwapchainEnvironmentEnabled() noexcept {
        static const bool enabled = internalSwapchainEnvironmentAllows(
            std::getenv("FGVK_INTERNAL_SWAPCHAIN"));
        return enabled;
    }
}

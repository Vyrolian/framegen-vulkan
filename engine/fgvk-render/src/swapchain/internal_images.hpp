/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "swapchain/inline_generation.hpp"

#include <vulkan/vulkan_core.h>

namespace vk {
    class Vulkan;
}

namespace fgvk::layer {

    struct DeferredPresentQueue;
    class InternalSwapchainImages;
    inline constexpr uint32_t maxInternalImageCount = 8;

    /// Facts the F5 internal-image gate reads from create-time state. Every
    /// field is plain data so the gate can be unit-tested without Vulkan.
    struct InternalImageGateInput {
        bool frameGenerationEnabled{false};
        bool deferredPresentQueue{false};
        VkExtent2D applicationExtent{};
        VkExtent2D lowerExtent{};
        VkFormat applicationFormat{VK_FORMAT_UNDEFINED};
        VkFormat lowerFormat{VK_FORMAT_UNDEFINED};
        uint32_t imageArrayLayers{1};
        VkSwapchainCreateFlagsKHR applicationFlags{0};
        // Formats in the application's VkImageFormatListCreateInfo (zero when absent).
        uint32_t applicationViewFormatCount{0};
        uint32_t applicationMinImageCount{0};
        uint32_t lowerImageCount{0};
        // Optimal-tiling BLIT_SRC and BLIT_DST for the application format.
        bool formatBlitCapable{false};
        // Two-dimensional optimal image with the application usage plus TRANSFER_SRC.
        bool imageFormatSupported{false};
    };

    /// Create-time flags the application may set while internal images are used.
    [[nodiscard]] constexpr VkSwapchainCreateFlagsKHR internalImageAllowedCreateFlags() noexcept {
        VkSwapchainCreateFlagsKHR allowed = VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
#if defined(VK_KHR_present_id2)
        allowed |= VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
#endif
#if defined(VK_KHR_present_wait2)
        allowed |= VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
#endif
#if defined(VK_EXT_present_timing)
        allowed |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
#endif
        return allowed;
    }

    /// VkImageCreateFlags of the private images and of their support query. The
    /// mutable swapchain bit implies both image flags, so compatible views can
    /// supply usages unsupported by the base format.
    [[nodiscard]] constexpr VkImageCreateFlags internalImageCreateFlags(
            const VkSwapchainCreateFlagsKHR flags) noexcept {
        return (flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) != 0
            ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT : 0;
    }

    /// Returns the first failing gate reason as a stable kebab-case string, or
    /// nullptr when internal application images are eligible.
    [[nodiscard]] inline const char* internalImagesIneligibleReason(
            const InternalImageGateInput& input) noexcept {
        if (!input.frameGenerationEnabled) return "frame-generation-disabled";
        if (!input.deferredPresentQueue) return "no-deferred-present-queue";
        // A variable surface (Wayland: no fixed currentExtent) is not refused.
        // The lower swapchain has the application's extent, which the next
        // check requires.
        if (input.applicationExtent.width != input.lowerExtent.width ||
                input.applicationExtent.height != input.lowerExtent.height) {
            return "extent-mismatch";
        }
        if (input.applicationFormat != input.lowerFormat) return "format-mismatch";
        if (input.imageArrayLayers != 1) return "array-layers";
        if (input.applicationMinImageCount > maxInternalImageCount)
            return "application-min-image-count";
        if (input.lowerImageCount < 2) return "lower-image-count";
        if ((input.applicationFlags & ~internalImageAllowedCreateFlags()) != 0) return "create-flags";
        // Mutable private images need the application's format list.
        if ((input.applicationFlags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) != 0 &&
                input.applicationViewFormatCount == 0) return "create-flags";
        if (!input.formatBlitCapable) return "format-blit";
        if (!input.imageFormatSupported) return "image-format";
        return nullptr;
    }

    [[nodiscard]] inline bool internalImagesEligible(const InternalImageGateInput& input) noexcept {
        return internalImagesIneligibleReason(input) == nullptr;
    }

    /// Private image count: min(8, max(minImageCount, 3) + 2). The extra two
    /// let the worker hold up to three pending real images in its depth-3 batch
    /// ring while the application keeps acquiring. Returns zero when the ring
    /// cannot satisfy the application's minimum or the lower count is below two.
    [[nodiscard]] constexpr uint32_t internalImageCount(
            const uint32_t minImageCount, const uint32_t lowerImageCount) noexcept {
        if (lowerImageCount < 2 || minImageCount > maxInternalImageCount) return 0;
        return std::min(maxInternalImageCount, std::max(minImageCount, 3u) + 2u);
    }

    /// FI2b: the release timeline a value belongs to. The private queue signals Private and the game
    /// queue (at the present call) signals Call. Each timeline has one signalling queue, so its values
    /// are signalled in submit order and no submit needs a cross-queue release wait.
    enum class ReleaseTimeline : uint8_t { Private, Call };
    inline constexpr size_t releaseTimelineCount = 2;

    /// FI2b: a timeline value and the timeline it was signalled on.
    struct ReleaseStamp {
        uint64_t value{0};
        ReleaseTimeline timeline{ReleaseTimeline::Private};
        bool operator==(const ReleaseStamp&) const = default;
    };

    /// FI2b: the release signal of one submit. Its value is one past the last value submitted on the
    /// same timeline. A pending value on the other timeline adds no wait and refuses nothing, so the
    /// plan has no wait field.
    struct ReleaseSignal {
        ReleaseTimeline timeline{ReleaseTimeline::Private};
        uint64_t value{0};
    };

    [[nodiscard]] constexpr ReleaseSignal planReleaseSignal(const bool privateQueue,
            const uint64_t lastPrivate, const uint64_t lastCall) noexcept {
        if (privateQueue)
            return ReleaseSignal{.timeline = ReleaseTimeline::Private, .value = lastPrivate + 1};
        return ReleaseSignal{.timeline = ReleaseTimeline::Call, .value = lastCall + 1};
    }

    /// FI2b: the timeline semaphore that a value of the given timeline is waited or signalled on.
    /// Pure, so the mapping is unit-tested with plain handles.
    template<class Handle>
    [[nodiscard]] constexpr Handle timelineHandleOf(const std::array<Handle, releaseTimelineCount>& timelines,
            const ReleaseTimeline timeline) noexcept {
        return timelines[static_cast<size_t>(timeline)];
    }

    /// F10: whether an emulated acquire may complete on the host, with no queue submit. Every
    /// condition must hold: the application semaphore is set and no fence is (a host signal cannot
    /// signal a fence), the tail timeline exists, both counter functions are loaded and the host path
    /// is not disabled. A zero value needs no counter. A counter below its value refuses. The caller
    /// reads the counters (they never wait) and passes zero for a failed read.
    [[nodiscard]] inline bool hostAcquireEligible(const VkFence fence, const VkSemaphore semaphore,
            const bool disabled, const bool tailPresent, const bool functionsLoaded,
            const uint64_t waitValue, const uint64_t releaseCounter,
            const uint64_t tailValue, const uint64_t tailCounter) noexcept {
        if (fence != VK_NULL_HANDLE || semaphore == VK_NULL_HANDLE || disabled || !tailPresent ||
                !functionsLoaded)
            return false;
        if (waitValue != 0 && releaseCounter < waitValue)
            return false;
        return tailValue == 0 || tailCounter >= tailValue;
    }

    /// Free-image ring for the application-visible images. Each index moves
    /// through three states:
    ///   Free     -> Acquired  by acquire / acquireWait
    ///   Acquired -> Pending   by present (the application's copy is queued)
    ///   Pending  -> Free      by complete, which records the release value
    /// Acquired -> Free also covers release (vkReleaseSwapchainImages). Free
    /// images are handed out in the order they were freed. GPU reuse safety is
    /// ordered by the release timeline value stored by complete, not here.
    class InternalImageRing {
    public:
        static constexpr uint32_t minCount = 2;
        static constexpr uint32_t maxCount = maxInternalImageCount;

        /// @throws std::invalid_argument when count is outside [2, 8].
        explicit InternalImageRing(const uint32_t count)
            : states(checkedCount(count), State::Free), values(count) {
            for (uint32_t index = 0; index < count; ++index)
                freeOrder.push_back(index);
        }

        /// Takes the least recently released free index without waiting.
        [[nodiscard]] std::optional<uint32_t> acquire() {
            const std::lock_guard lock(mutex);
            return takeFreeLocked();
        }

        /// Takes the least recently released free index. A zero timeout never
        /// waits; UINT64_MAX waits until an index is freed or abort() is called.
        /// Only the ring's own mutex is held while waiting.
        [[nodiscard]] std::optional<uint32_t> acquireWait(const uint64_t timeoutNs) {
            std::unique_lock lock(mutex);
            if (freeOrder.empty() && timeoutNs != 0) {
                const auto ready = [this] { return !freeOrder.empty() || aborted; };
                if (timeoutNs == UINT64_MAX) {
                    changed.wait(lock, ready);
                } else {
                    const auto budget = std::chrono::nanoseconds(std::min<uint64_t>(timeoutNs, INT64_MAX));
                    static_cast<void>(changed.wait_for(lock, budget, ready));
                }
            }
            return takeFreeLocked();
        }

        /// Wakes every acquireWait blocked on an empty ring, and makes later waits on an
        /// empty ring return at once. The owner calls it after the sticky error is stored:
        /// no image may be freed any more, so a blocked application acquire must return
        /// that error. A free index is still handed out after abort.
        void abort() {
            const std::lock_guard lock(mutex);
            aborted = true;
            changed.notify_all();
        }

        /// Marks an acquired index as presented. The index stays unavailable until complete.
        [[nodiscard]] bool present(const uint32_t index) {
            const std::lock_guard lock(mutex);
            if (index >= states.size() || states[index] != State::Acquired)
                return false;
            states[index] = State::Pending;
            return true;
        }

        /// Frees a presented index once its copy has been submitted. The release value
        /// and its timeline are what the next reuse must wait for (FI2b).
        [[nodiscard]] bool complete(const uint32_t index, const uint64_t releaseValue,
                const ReleaseTimeline timeline) {
            const std::lock_guard lock(mutex);
            if (index >= states.size() || states[index] != State::Pending)
                return false;
            states[index] = State::Free;
            values[index] = ReleaseStamp{.value = releaseValue, .timeline = timeline};
            freeOrder.push_back(index);
            changed.notify_all();
            return true;
        }

        /// Returns an acquired index that was never presented.
        [[nodiscard]] bool release(const uint32_t index) {
            const std::lock_guard lock(mutex);
            if (index >= states.size() || states[index] != State::Acquired)
                return false;
            states[index] = State::Free;
            freeOrder.push_back(index);
            changed.notify_all();
            return true;
        }

        /// Returns every listed index only if all of them are distinct and acquired.
        [[nodiscard]] bool releaseAll(std::span<const uint32_t> indices) {
            const std::lock_guard lock(mutex);
            for (size_t i = 0; i < indices.size(); ++i) {
                const uint32_t index = indices[i];
                if (index >= states.size() || states[index] != State::Acquired)
                    return false;
                for (size_t j = 0; j < i; ++j) {
                    if (indices[j] == index)
                        return false;
                }
            }
            for (const uint32_t index : indices) {
                states[index] = State::Free;
                freeOrder.push_back(index);
            }
            changed.notify_all();
            return true;
        }

        /// Timeline value and timeline of the last copy that read this index. Zero
        /// means never used. The owner serialises access with the queue that signals it.
        /// @throws std::out_of_range for an index outside the ring.
        [[nodiscard]] ReleaseStamp& releaseValue(const uint32_t index) {
            return values.at(index);
        }

        /// Timeline value and timeline of the last copy that read this index, read under
        /// the ring mutex. On the production path only complete() writes it, and only
        /// for a Pending index, so a caller that holds the index Acquired reads a
        /// stable value.
        /// @throws std::out_of_range for an index outside the ring.
        [[nodiscard]] ReleaseStamp releaseValueOf(const uint32_t index) const {
            const std::lock_guard lock(mutex);
            return values.at(index);
        }

        /// Indices the application holds (Acquired, not yet presented).
        [[nodiscard]] size_t acquiredCount() const {
            return countState(State::Acquired);
        }

        /// Indices presented whose copy has not completed (Pending).
        [[nodiscard]] size_t pendingCount() const {
            return countState(State::Pending);
        }

        [[nodiscard]] bool isAcquired(const uint32_t index) const {
            const std::lock_guard lock(mutex);
            return index < states.size() && states[index] == State::Acquired;
        }

        /// Diagnostics only (D7). Each index's state letter (F free, A acquired,
        /// P pending) with its stored release value. Reads only when the ring mutex
        /// is free, so it never waits. Empty when the mutex is busy.
        [[nodiscard]] std::optional<std::vector<std::pair<char, uint64_t>>> tryDescribe() const {
            std::unique_lock lock(mutex, std::try_to_lock);
            if (!lock.owns_lock())
                return std::nullopt;
            std::vector<std::pair<char, uint64_t>> described;
            described.reserve(states.size());
            for (size_t index = 0; index < states.size(); ++index) {
                const char letter = states[index] == State::Free ? 'F'
                    : states[index] == State::Acquired ? 'A' : 'P';
                described.emplace_back(letter, values[index].value);
            }
            return described;
        }

    private:
        enum class State : uint8_t { Free, Acquired, Pending };

        static size_t checkedCount(const uint32_t count) {
            if (count < minCount || count > maxCount)
                throw std::invalid_argument("internal image ring size must be between 2 and 8");
            return count;
        }

        std::optional<uint32_t> takeFreeLocked() {
            if (freeOrder.empty())
                return std::nullopt;
            const uint32_t index = freeOrder.front();
            freeOrder.pop_front();
            states[index] = State::Acquired;
            return index;
        }

        size_t countState(const State wanted) const {
            const std::lock_guard lock(mutex);
            return static_cast<size_t>(std::count(states.begin(), states.end(), wanted));
        }

        mutable std::mutex mutex;
        std::condition_variable changed;
        std::vector<State> states;
        std::vector<ReleaseStamp> values;
        std::deque<uint32_t> freeOrder;
        // Set by abort() under mutex and never cleared. False for a ring never aborted.
        bool aborted{false};
    };

    /// FP2: a lower image the worker acquired and parked for a later present call. slot is the lowerAcquire and
    /// lowerFences ring slot that the acquire committed (InternalSwapchainImages::commitLowerAcquire).
    struct ParkedLower {
        uint32_t lowerIndex{};
        uint32_t slot{};
    };

    /// FP2: a parked image taken for one copy, with the acquire semaphore that the copy waits.
    struct ParkedLowerTake {
        uint32_t lowerIndex{};
        uint32_t slot{};
        VkSemaphore semaphore{VK_NULL_HANDLE};
    };

    /// FP2: the parked lower images of one swapchain, the parking policy, and the busy flag of every lower-acquire slot.
    /// Plain data with no lock of its own: the owner (InternalSwapchainImages) guards every call with its leaf mutex.
    /// A slot is busy from its park until the submit that consumes it has returned (a copy or a drain). The worker never
    /// acquires into a busy slot, so the slot's fence is not read or reset while the call owns it.
    class LowerParkingPool {
    public:
        static constexpr size_t capacity = 2;

        /// Sizes the busy flags for the owner's lower-acquire slots. Clears the pool.
        void reserveSlots(const size_t slotCount) {
            busy.assign(slotCount, 0);
            count = 0;
        }
        /// Sets the target and budget. A target applies only when the copies were recorded for the call's family.
        void setPolicy(const uint32_t target, const uint32_t budgetOutputs, const bool sameFamily) noexcept {
            targetCount = sameFamily ? std::min<uint32_t>(target, static_cast<uint32_t>(capacity)) : 0;
            budget = budgetOutputs;
        }
        [[nodiscard]] uint32_t target() const noexcept { return targetCount; }
        [[nodiscard]] uint32_t budgetOutputs() const noexcept { return budget; }
        [[nodiscard]] size_t size() const noexcept { return count; }
        /// True when the worker may park one more image while callHeld outputs are held at the call.
        [[nodiscard]] bool canPark(const size_t callHeld) const noexcept {
            return inlineParkFits(count, callHeld, targetCount, budget);
        }
        /// Adds a parked image and marks its slot busy. False when full or the slot is out of range.
        [[nodiscard]] bool push(const ParkedLower image) {
            if (count >= capacity || image.slot >= busy.size())
                return false;
            entries[count++] = image;
            busy[image.slot] = 1;
            return true;
        }
        /// Removes the oldest parked image (FIFO). Its slot stays busy until release.
        [[nodiscard]] std::optional<ParkedLower> take() {
            if (count == 0)
                return std::nullopt;
            const ParkedLower oldest = entries[0];
            for (size_t i = 1; i < count; ++i)
                entries[i - 1] = entries[i];
            --count;
            return oldest;
        }
        /// Clears a slot's busy flag after the submit that consumed it has returned.
        void release(const uint32_t slot) noexcept {
            if (slot < busy.size())
                busy[slot] = 0;
        }
        /// True when a slot may not be acquired into or read. An out-of-range slot reads as busy.
        [[nodiscard]] bool isBusy(const uint32_t slot) const noexcept {
            return slot >= busy.size() || busy[slot] != 0;
        }

    private:
        std::array<ParkedLower, capacity> entries{};
        size_t count{};
        std::vector<char> busy;
        uint32_t targetCount{};
        uint32_t budget{};
    };

    /// Device support for the private image format. Filled by the Vulkan
    /// side and copied into InternalImageGateInput.
    struct InternalImageDeviceSupport {
        bool formatBlitCapable{false};
        bool imageFormatSupported{false};
    };

    /// Per-present view of one application-visible image, built by the entrypoint
    /// and passed to Swapchain::present for the duration of that call.
    struct InternalPresentImage {
        uint32_t applicationIndex{};
        VkImage image{VK_NULL_HANDLE};
        // Presentation-wait and retirement-fence slot of the real output:
        // lowerImageCount + applicationIndex. Lower indices keep their own slots,
        // so a generated copy into lower image k never shares a slot with the real output.
        uint32_t realSlot{};
        InternalSwapchainImages* owner{};
        // Keeps the owner alive while a worker job still references it.
        std::shared_ptr<InternalSwapchainImages> keepAlive;
        // Synchronous lower acquire. The entrypoint wraps its deferred join
        // around the driver call and commits the lower-acquire slot.
        std::function<VkResult(uint64_t timeout, VkSemaphore semaphore, uint32_t* index)> lowerAcquire;
        // True once the copy that reads this image has been submitted or queued
        // to the worker. A present that ends without it must be recovered.
        bool handedOff{};
    };

    /// Layer-owned application images for one eligible swapchain. The
    /// application renders into these images. A real output is copied into a
    /// lower swapchain image either on the DeferredPresent worker at its deadline
    /// or synchronously at the moment it presents.
    class InternalSwapchainImages : public std::enable_shared_from_this<InternalSwapchainImages> {
    public:
        /// Creates the private images and their synchronization objects.
        /// Returns nullptr on any failure. Partial resources are destroyed.
        [[nodiscard]] static std::shared_ptr<InternalSwapchainImages> create(
            const vk::Vulkan& vk, VkSwapchainKHR lowerSwapchain,
            const VkSwapchainCreateInfoKHR& application,
            VkExtent2D extent, std::span<const VkImage> lowerImages,
            uint32_t imageCount, std::shared_ptr<DeferredPresentQueue> queue);

        /// Waits the release timeline (2 s, then vkDeviceWaitIdle) and destroys
        /// command buffers, semaphores, fences, images and memory.
        ~InternalSwapchainImages();
        InternalSwapchainImages(const InternalSwapchainImages&) = delete;
        InternalSwapchainImages& operator=(const InternalSwapchainImages&) = delete;

        [[nodiscard]] std::span<const VkImage> images() const noexcept;
        [[nodiscard]] uint32_t lowerImageCount() const noexcept;
        [[nodiscard]] uint32_t applicationImageCount() const noexcept;
        [[nodiscard]] bool isAcquired(uint32_t index) const;

        /// Emulated vkAcquireNextImage*. The caller has already joined the
        /// deferred present. deferredSuboptimal is the caller's suboptimal state.
        /// A finite timeout waits for a presented image to complete; zero polls.
        [[nodiscard]] VkResult acquireApplicationImage(uint64_t timeout,
            VkSemaphore semaphore, VkFence fence, bool deferredSuboptimal,
            uint32_t* imageIndex);

        /// vkReleaseSwapchainImages: no lower call and no GPU work.
        [[nodiscard]] VkResult releaseApplicationImages(std::span<const uint32_t> indices);

        /// Waits until the next lower-acquire slot can be reused. A zero
        /// timeout polls without sleeping, for use under the queue lock.
        [[nodiscard]] VkResult waitLowerAcquireSlot(uint64_t timeout);
        [[nodiscard]] VkSemaphore lowerAcquireSemaphore() const;
        /// Call after a successful lower acquire. Resets the slot fence and
        /// returns the slot that the copy will wait on.
        [[nodiscard]] uint32_t commitLowerAcquire();

        /// Acquired -> Pending. Called when the application presents the image.
        void markPending(uint32_t applicationIndex);
        /// Consumes the application waits without copying, then frees the image.
        /// Used only on the sticky-error path, which never reaches a lower acquire.
        /// X3: a non-null presentFence (the application's present fence) is signalled on the
        /// private queue right after that release, so it follows the consumed waits.
        [[nodiscard]] VkResult consumeWithoutCopy(uint32_t applicationIndex,
            std::span<const VkSemaphore> applicationWaits, VkFence presentFence = VK_NULL_HANDLE);
        /// Image not handed off: an empty submit on the application queue signals a
        /// handoff semaphore after any source copy already submitted there. The private
        /// queue then waits it and signals the release timeline, freeing the image. The
        /// swapchain becomes sticky OUT_OF_DATE. The private release timeline is never
        /// signalled on the application queue.
        void releaseNotHandedOff(VkQueue applicationQueue, uint32_t applicationIndex);

        /// Records the copies from one application image to every lower image.
        /// Game thread only; the worker resubmits recorded copies and never records.
        [[nodiscard]] VkResult ensureCopies(const vk::Vulkan& vk, uint32_t applicationIndex);
        /// Synchronous path. Acquires a lower image through image.lowerAcquire,
        /// copies the application image into it after waits, and frees the image.
        /// On success image.handedOff is set by the caller.
        [[nodiscard]] VkResult materialize(const vk::Vulkan& vk, const InternalPresentImage& image,
            std::span<const VkSemaphore> waits, uint32_t* lowerIndex, VkSemaphore* copied);

        /// Worker callbacks, reached through the free functions below. The caller
        /// holds DeferredPresentQueue::mutex and never sleeps there.
        [[nodiscard]] VkResult workerTryAcquire(uint32_t* lowerIndex);
        [[nodiscard]] VkResult workerSubmit(VkQueue queue, uint32_t applicationIndex,
            uint32_t lowerIndex, VkSemaphore continuation, VkSemaphore* presentWait);
        [[nodiscard]] VkResult workerAbandon(VkQueue queue, uint32_t applicationIndex,
            VkSemaphore continuation);

        /// FI2: the copy of a real made at its present call on the game queue (callQueue, of family callFamily).
        /// Caller holds queue->mutex. It refuses, leaving the image for the worker, when the copy was recorded for
        /// another family or when no lower image is free. Otherwise it takes the lower image and submits the copy,
        /// waiting continuation, exactly as the worker does. The copy signals callRelease (FI2b). *consumed is set
        /// once the application image is taken. That is at the submit, which frees it on success and on failure.
        [[nodiscard]] VkResult callPrepare(VkQueue callQueue, uint32_t callFamily,
            uint32_t applicationIndex, VkSemaphore continuation, uint32_t* lowerIndex,
            VkSemaphore* presentWait, bool* consumed);

        /// FP2 rule 3: the call copy of a real from a parked image. Takes no queue lock: it takes the oldest parked image
        /// under the leaf mutex and submits the copy on the game queue, waiting the image's slot semaphore and signalling
        /// its slot fence. Returns VK_NOT_READY with consumed false when no image is parked. Otherwise consumed is set
        /// and the result is the submit's, as callPrepare.
        [[nodiscard]] VkResult callPrepareParked(VkQueue callQueue, uint32_t callFamily,
            uint32_t applicationIndex, VkSemaphore continuation, uint32_t* lowerIndex,
            VkSemaphore* presentWait, bool* consumed);

        /// FP2: the lower swapchain this owner acquires from. A direct generated output takes a parked image only when its
        /// acquire is on this same handle.
        [[nodiscard]] VkSwapchainKHR lowerSwapchainHandle() const noexcept;
        /// FP2 rule 2: after a lower present of the worker, parks at most one lower image (see parkAfterPresentAction: a
        /// target of zero drains nothing). Caller holds queue->mutex and the deferred worker's mutex, so callHeld (the outputs
        /// call-held after that present) is read consistently with the admissions, which hold the worker's mutex too.
        /// FP3: returns why the park did or did not park (see ParkOutcome).
        [[nodiscard]] ParkOutcome parkAfterPresent(size_t callHeld);
        /// FP2: sets the parking target (0 turns parking off) and the call budget for the attached batch being presented.
        /// callFamily is the family of the call's queue: a target applies only when it matches the copies' family.
        void setParkPolicy(uint32_t target, uint32_t budget, uint32_t callFamily);
        /// FP2: the parked images now (leaf mutex).
        [[nodiscard]] size_t parkedCount() const;
        /// FP2: the lower images held outside the worker's job list: the parked images and the direct generated outputs
        /// between their take or acquire and their present (see beginCallOutput). The call budget counts them.
        [[nodiscard]] size_t outstandingCallOutputs() const;
        /// FP2: a direct generated output took a lower image by a fresh acquire (called under queue->mutex, right after the
        /// acquire succeeds). Its count ends with endCallOutput, after its present call has returned.
        void beginCallOutput();
        void endCallOutput() noexcept;
        /// FP2 rule 4: takes the oldest parked image for a direct generated output. Empty when none is parked or the owner is
        /// sticky. The slot stays busy until finishParkedCopy or failParkedTake. The output counts as in flight from here
        /// until endCallOutput.
        [[nodiscard]] std::optional<ParkedLowerTake> takeParked();
        /// FP2 rule 4: after the game-queue copy that waited a taken slot, submits an empty submit on gameQueue that signals
        /// the slot fence, then frees the slot. The caller holds no lock: the game thread owns the slot until this returns.
        [[nodiscard]] VkResult finishParkedCopy(VkQueue gameQueue, uint32_t slot);
        /// FP2: the copy of a taken slot could not be recorded or submitted. The slot's fence is never signalled, so the slot
        /// stays busy, and the owner turns sticky.
        void failParkedTake(uint32_t slot) noexcept;
        /// DG1 rule 6: gives a taken image back before its backend schedule started, so no submit has waited its acquire. The
        /// image is parked again and its output count ends. A full pool turns the owner sticky, as the image cannot be kept.
        void returnParked(ParkedLowerTake take) noexcept;
        /// FP2 rule 6: drains every parked image and waits for the last drain to finish, up to timeoutNs. Called before the
        /// lower swapchain is destroyed, so no parked acquire is still pending. VK_TIMEOUT means the drain is still running:
        /// call again.
        [[nodiscard]] VkResult retireParked(uint64_t timeoutNs);

        /// T3b, diagnostics only (pacing profile with GPU split): time each copy on the GPU.
        /// Game thread, before ensureCopies; idempotent. Copies recorded earlier stay untimed.
        void enableCopyTimestamps(const vk::Vulkan& vk, PFN_vkGetCalibratedTimestampsKHR calibrate,
            uint64_t context) noexcept;
        /// T3b: the pacing source of the application image about to be copied. Game thread,
        /// before the copy of that image is enqueued or materialized; lock-free.
        void noteCopySource(uint32_t applicationIndex, uint64_t source) noexcept;

        /// Sticky failure. OUT_OF_DATE, SURFACE_LOST and full-screen-exclusive
        /// loss from the lower path are kept. Submit failures are also kept.
        [[nodiscard]] VkResult stickyError() const noexcept;
        void noteLowerError(VkResult error) noexcept;
        void markLowerSuboptimal() noexcept;

    private:
        struct Impl;
        explicit InternalSwapchainImages(std::unique_ptr<Impl> state);
        // FI2: the lower acquire of workerTryAcquire, noted under `op` (lowerOpAcquireReal, or lowerOpAcquireCall for the
        // call path). Caller holds queue->mutex. FP3: *poll is set on every path. It says why a result other than VK_SUCCESS
        // returned no image: a busy slot, an unsignalled slot fence, a NOT_READY or TIMEOUT acquire, or an error. Each
        // caller keeps VK_NOT_READY semantics and only the park reads *poll.
        [[nodiscard]] VkResult tryAcquireLowerLocked(uint8_t op, uint32_t* lowerIndex, LowerPoll* poll);
        // Copies application image p into lower image lower after the waits and the lower-acquire slot. On success it
        // signals copied[lower] and the release timeline of queue and frees p. On failure it records the sticky error
        // and frees p at the last submitted release value. queue is the private queue (the caller holds queue->mutex) or
        // the game queue (FP2: the caller holds no lock; the leaf mutex guards the state). Either way the slot is released.
        [[nodiscard]] VkResult submitCopyLocked(VkQueue queue, uint32_t applicationIndex,
            uint32_t lowerIndex, uint32_t slot, std::span<const VkSemaphore> waits);
        // FP2 rule 6: submits one drain per parked image on the private queue. Caller holds queue->mutex. Called only on
        // the destroy paths and the sticky error, never on a park target change (see its definition).
        VkResult drainParkedLocked();
        // T3b: caller holds the leaf mutex (FP2). Emits every timed copy whose timestamps are
        // available; a pending copy of pair `resubmitted` is emitted unavailable.
        void collectCopyTimestampsLocked(size_t resubmitted) noexcept;
        // D7 stall probe, pacing profile only. create registers each owner and the
        // destructor removes it, under one registry mutex. A single detached watchdog
        // dumps the registered owners after an application stall.
        static void registerStallProbe(const InternalSwapchainImages* owner);
        static void unregisterStallProbe(const InternalSwapchainImages* owner);
        static void stallWatchdogLoop();
        // D7b: stalledNanoseconds is the time since the last application present.
        void writeStallProbe(int64_t stalledNanoseconds) const;
        std::unique_ptr<Impl> impl;
    };

    /// Worker callbacks for DeferredPresent's internal real output. They cast owner
    /// back to InternalSwapchainImages and forward to the members above.
    [[nodiscard]] VkResult internalRealTryAcquire(void* owner, uint32_t* lowerIndex);
    [[nodiscard]] VkResult internalRealCallPrepareParked(void* owner, VkQueue callQueue, uint32_t callFamily,
        uint32_t applicationIndex, VkSemaphore continuation, uint32_t* lowerIndex,
        VkSemaphore* presentWait, bool* consumed);
    [[nodiscard]] size_t internalRealParkedCount(void* owner);
    /// FP2: parks at most one lower image after a lower present of the worker, called with queue->mutex held.
    /// callHeld is the outputs call-held after that present. FP3: returns the outcome (see ParkOutcome).
    [[nodiscard]] ParkOutcome internalRealParkAfterPresent(void* owner, size_t callHeld);
    [[nodiscard]] VkResult internalRealSubmit(void* owner, VkQueue queue,
        uint32_t applicationIndex, uint32_t lowerIndex, VkSemaphore continuation,
        VkSemaphore* presentWait);
    [[nodiscard]] VkResult internalRealAbandon(void* owner, VkQueue queue,
        uint32_t applicationIndex, VkSemaphore continuation);
    [[nodiscard]] VkResult internalRealCallPrepare(void* owner, VkQueue callQueue, uint32_t callFamily,
        uint32_t applicationIndex, VkSemaphore continuation, uint32_t* lowerIndex,
        VkSemaphore* presentWait, bool* consumed);

    /// Device support queries for the private image format, usage, create flags and
    /// view formats. The flags and list must match what the private images are created with.
    [[nodiscard]] InternalImageDeviceSupport queryInternalImageSupport(
        const vk::Vulkan& vk, VkFormat format, VkImageUsageFlags usage,
        VkImageCreateFlags flags = 0, std::span<const VkFormat> viewFormats = {});
}

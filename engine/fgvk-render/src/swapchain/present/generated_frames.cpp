/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain/swapchain.hpp"
#include "swapchain/inline_generation.hpp"
#include "swapchain/internal_images.hpp"
#include "swapchain/present/internal.hpp"
#include "swapchain/retirement.hpp"
#include "fgvk-common/helpers/errors.hpp"
#include "fgvk-common/vulkan/command_buffer.hpp"
#include "fgvk-common/vulkan/image.hpp"
#include "fgvk-common/vulkan/semaphore.hpp"
#include "fgvk-common/vulkan/vulkan.hpp"
#include "present_diagnostics.hpp"
#include "pnext_chain.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

using namespace fgvk;
using namespace fgvk::layer;

using namespace fgvk::layer::present_detail;

VkResult Swapchain::presentGeneratedFrames(
        const PresentInvocation& invocation,
        const PresentationFramePlan& plan) {
    auto totalAcquireDuration = plan.preacquireDuration;
    auto generatedSubmitDuration = DiagnosticsClock::duration::zero();
    auto generatedPresentDuration = DiagnosticsClock::duration::zero();
    auto originalPresentDuration = DiagnosticsClock::duration::zero();
    bool acquireDeadlineExceeded = false;
    std::optional<VkResult> deferredAcquireError;
    const GeneratedPresentPNextChain generatedChain(
        invocation.nextChain);
    const int64_t generatedMode = [&]() -> int64_t {
        const auto found = present_diagnostics::applicationPresentMode(
            VK_PRESENT_MODE_MAX_ENUM_KHR, generatedChain.head());
        return found.dynamic ? found.mode : -1;
    }();
    const bool deferredFirst = invocation.deferredOutputEligible && invocation.pacedOutputs;
    bool deferredTail = invocation.deferredOutputEligible && invocation.pacedOutputs &&
        (deferredFirst || plan.scheduledGeneratedFrames.size() > 1) &&
        this->beginDeferredPresentBatch(invocation.vk,
            plan.scheduledGeneratedFrames.size() + (deferredFirst ? 1 : 0),
            this->frameState.backendTimelineIndex + plan.scheduledGeneratedFrames.size() - 1,
            invocation.readinessDeadline,
            invocation.releasedOutputLimit, invocation.releasedJoinTolerance,
            invocation.releasedOverlap, invocation.justInTime,
            invocation.pacedOutputs ? invocation.pacedOutputs->interval : DiagnosticsClock::duration{},
            invocation.limiterArrival);
    struct CloseDeferredBatch {
        DeferredPresent* worker;
        ~CloseDeferredBatch() { if (worker) worker->closeBatch(); }
    } closeDeferredBatch{deferredTail ? this->deferredPresent.get() : nullptr};
    const auto reportOrderedAcquire = [&](const bool timedOut) {
        if (!this->privateOrderedTransport)
            return;

        const auto observedAt = DiagnosticsClock::now();
        const auto observation =
            this->recoveryState.orderedAcquireRecovery.observe(
                observedAt, timedOut, acquireDeadlineExceeded,
                plan.boundedOrderedAcquireProbe
            );
        if (observation.quarantined)
            this->fixedRefreshBudget.reset();
    };

    // A released native batch lets the worker acquire, copy and present every generated
    // output at its deadline. Every output's copies are recorded before the first is
    // submitted; a failure keeps the whole batch on the path below.
    bool workerCopies = deferredTail && invocation.releasedOverlap &&
        !plan.generatedImagesPreacquired && this->deferredPresent;
    for (size_t i = 0; workerCopies && i < plan.scheduledGeneratedFrames.size(); ++i)
        workerCopies = this->recordGeneratedCopies(invocation.vk, this->currentRenderSlot(), i);

    static const bool sameQueueCopies = [] {
        const auto* value = std::getenv("FGVK_SAME_QUEUE_GENERATED_COPY");
        return value && std::string_view(value) == "1";
    }();
    static const bool directCopies = [] {
        const auto* value = std::getenv("FGVK_DIRECT_GENERATED_COPY");
        return value && std::string_view(value) == "1";
    }();
    // QH2: any pass of this batch whose copies were recorded on the transfer family makes the batch transfer-recorded.
    // Their submits never advance the private queue's tail, so the same-queue staging and the direct copy stay off.
    // Every scheduled pass is checked, since a failed recording can leave a mix of passes.
    const bool transferCopies = workerCopies && [&] {
        const auto& batchPasses = this->currentRenderSlot().passes;
        for (size_t i = 0; i < plan.scheduledGeneratedFrames.size() && i < batchPasses.size(); ++i)
            if (batchPasses.at(i).generatedOnTransfer) return true;
        return false;
    }();
    // FI4: the opt-in non-inline experiments keep their gate: a worker batch of one output on native Wayland IMMEDIATE.
    const bool nativeImmediate2x = nativeImmediateTwoXBatch(workerCopies, this->internalPresent != nullptr,
        plan.scheduledGeneratedFrames.size(), this->info.surfaceOrigin, this->currentPresentMode);
    // FI2 rule 1 / FI4: an attached backend takes the call path on every worker batch, whatever the surface, present mode
    // and multiplier (inlineCallBatchEligible). Its call-held budget replaces canPreacquireGenerated(), and is checked per
    // output below. The budget cap is the swapchain's call images (info.inlineCall), zero when it has none.
    // FI5: the budget reserves the worker's one held lower image (workerHeldLowerImages), not the create floor's two.
    const bool inlineCalls = this->attachedGeneration();
    const uint32_t callImages = this->info.inlineCall.count;
    const uint32_t callBudget = inlineCalls
        ? inlineCallLowerBudget(this->info.images.size(), this->info.surfaceMinImageCount, workerHeldLowerImages,
            callImages)
        : 0;
    const bool inlineEligible = inlineCallBatchEligible(inlineCalls, workerCopies, this->internalPresent != nullptr,
        transferCopies, this->info.images.size(), this->info.surfaceMinImageCount, workerHeldLowerImages, callImages);
    const bool tryDirect = inlineCalls
        ? inlineEligible
        : directCopies && !transferCopies && nativeImmediate2x && this->info.surfaceMinImageCount &&
            this->info.images.size() >= static_cast<size_t>(this->info.surfaceMinImageCount) + layerHeldLowerImages &&
            this->deferredPresent->canPreacquireGenerated();
    // FI2: an eligible attached batch. Each of its outputs is counted as copied at the call or as a fallback, and its real
    // is eligible for the call only when every output went direct (rule 5).
    const bool inlineBatch = tryDirect && inlineCalls;
    bool inlineAllDirect = inlineBatch;
    // FI4: the outputs of this batch, its generated outputs and its real. An eligible batch parks one lower image per output.
    const uint32_t batchOutputs = inlineBatchOutputs(plan.scheduledGeneratedFrames.size());
    if (this->pacingProfile && inlineBatch) this->pacingProfile->inlineCallBatchOutputs(batchOutputs);
    // FP2: the owner of this present's internal images parks lower images for an attached batch: target O (see
    // inlineBatchOutputs) for an eligible batch, 0 otherwise. A direct generated output takes a parked image only when its
    // acquire is on the owner's lower swapchain (FP2.4: invocation.swapchain is the handle the lower swapchain was created
    // under, so this holds for every present of the swapchain).
    InternalSwapchainImages* const parkOwner = inlineCalls && this->internalPresent ? this->internalPresent->owner : nullptr;
    const bool parkSameSwapchain = parkOwner && parkOwner->lowerSwapchainHandle() == invocation.swapchain;
    if (parkOwner) {
        parkOwner->setParkPolicy(inlineBatch ? batchOutputs : 0, callBudget, invocation.vk.queueFamilyIndex());
        if (this->deferredPresent)
            this->deferredPresent->setParkHook(this->internalPresent->keepAlive, &internalRealParkAfterPresent);
    }
    // The parked images of the owner count toward the budget. Zero when no owner parks.
    const auto parkedNow = [parkOwner]() -> size_t { return parkOwner ? parkOwner->parkedCount() : 0; };
    const bool privateStaging = sameQueueCopies && !transferCopies && nativeImmediate2x &&
        this->info.deferredPresentQueue->tail != VK_NULL_HANDLE &&
        // Earlier outputs still need the private queue ahead of this backend wait.
        !this->deferredPresent->olderBatchPending();
    // FI4: the previous output of this batch was a worker copy or skip, so its signal is its copied semaphore. Any other
    // kind of output signals the post-copy chain (see chainSignalBefore). Only the worker branch and direct outputs read it;
    // a present-thread output always waits the post-copy chain, since a worker refusal converts that chain first.
    bool previousWorker = false;
    // The semaphore the direct copy of output i waits for its predecessor (i > 0).
    const auto directPredecessor = [this](const size_t index, const bool afterWorker) -> VkSemaphore {
        return chainSignalBefore(index == 0, afterWorker) == ChainSignal::Copied
            ? this->currentRenderSlot().passes.at(index - 1).copied->handle()
            : this->postCopySemaphores.at((this->frameState.sequenceIndex - 1) % this->postCopySemaphores.size()).second.handle();
    };

    for (size_t i = 0; i < plan.scheduledGeneratedFrames.size(); ++i) {
        auto& postCopy = this->postCopySemaphores.at(
            this->frameState.sequenceIndex % this->postCopySemaphores.size()
        );
        auto& destinationImage = this->destinationImages.at(i);
        auto& pass = this->currentRenderSlot().passes.at(i);
        uint32_t acquiredImageIndex{};
        VkResult result{};
        // FP1: where this output's call copy ended (counted for an attached batch only, below).
        InlineCallOutcome callOutcome = InlineCallOutcome::None;
        // FP2 rule 4: a parked image taken for this direct generated output. Its slot's semaphore is waited by the copy.
        std::optional<ParkedLowerTake> parked;
        bool direct = false;
        if (tryDirect) {
            if (inlineCalls && parkSameSwapchain) {
                parked = parkOwner->takeParked();
                // FP4: no parked image yet. The worker's present may park one within inlineCallLowerWait. The wait holds no
                // fgvk lock (see DeferredPresent::awaitCallLower), and the take after it is the same as the first. FI4: once
                // a wait of this batch timed out, its later outputs skip the wait (inlineCallWaitAllowed).
                if (!parked && inlineCallWaitAllowed(this->callWaitTimedOutThisPresent)) {
                    const auto waited = this->deferredPresent->awaitCallLower(parkedNow);
                    this->callWaitTimedOutThisPresent = inlineCallWaitBatchTimedOut(this->callWaitTimedOutThisPresent, waited);
                    parked = parkOwner->takeParked();
                }
            }
            if (parked) {
                // FP2 rule 4: the parked image is counted by the budget already, so taking it needs no budget check and no
                // queue lock. The total of call-held and parked outputs does not change.
                direct = true;
                acquiredImageIndex = parked->lowerIndex;
                result = VK_SUCCESS;
                callOutcome = InlineCallOutcome::Parked;
            } else {
                bool budgetFits = !inlineCalls || inlineCallHoldFits(
                    inlineCallOutputsHeld(this->deferredPresent->callHeldOutputs(), parkedNow()), callBudget);
                bool lockMissed = false;
                bool acquired = false;
                if (budgetFits) {
                    result = this->deferredPresent->pollAcquire([&] {
                        // FP2 rule 2: the decisive budget check runs under queue->mutex, where the worker parks.
                        if (inlineCalls && !inlineCallHoldFits(
                                inlineCallOutputsHeld(this->deferredPresent->callHeldOutputs(), parkedNow()), callBudget)) {
                            budgetFits = false;
                            return VK_NOT_READY;
                        }
                        const VkResult acquired = invocation.vk.df().AcquireNextImageKHR(
                            invocation.vk.dev(), invocation.swapchain, 0,
                            pass.acquireSemaphore.handle(), VK_NULL_HANDLE, &acquiredImageIndex);
                        // FP2: the acquired image is in flight from this acquire, counted under queue->mutex so that the
                        // worker's park check cannot miss it. The count ends when this output's iteration ends.
                        if (parkOwner && (acquired == VK_SUCCESS || acquired == VK_SUBOPTIMAL_KHR))
                            parkOwner->beginCallOutput();
                        return acquired;
                    }, &lockMissed);
                    acquired = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
                }
                direct = acquired;
                callOutcome = classifyGeneratedAttempt(budgetFits, lockMissed, acquired,
                    result == VK_NOT_READY || result == VK_TIMEOUT);
            }
        }
        // FP2: a served output is in flight from its take or acquire until this iteration ends, on every exit (the copy's
        // enqueue or synchronous present, a failure, or OUT_OF_DATE). The worker counts it in callHeldLocked from its
        // admission, so the overlap is brief and only over-counts, never under-counts, the held outputs.
        struct CallOutputScope {
            InternalSwapchainImages* owner;
            ~CallOutputScope() {
                if (owner) owner->endCallOutput();
            }
        } const callOutputScope{direct ? parkOwner : nullptr};
        if (inlineBatch) {
            if (!inlineCallServed(callOutcome)) inlineAllDirect = false;
            if (this->pacingProfile) {
                if (inlineCallServed(callOutcome)) {
                    this->pacingProfile->inlineCallGeneratedActive();
                    if (callOutcome == InlineCallOutcome::Parked) this->pacingProfile->inlineCallParkedActive();
                } else {
                    this->pacingProfile->inlineCallFallbackActive(inlineCallFallbackReason(callOutcome));
                }
            }
        }

        // Any refusal keeps the path below.
        if (workerCopies && deferredTail && !direct) {
            const bool last = i + 1 == plan.scheduledGeneratedFrames.size();
            auto& slot = this->currentRenderSlot();
            if (privateStaging) {
                auto& queue = *this->info.deferredPresentQueue;
                const VkSemaphore backend = this->syncSemaphore->handle();
                const VkCommandBuffer command = pass.stagingCopy->handle();
                const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                const uint64_t backendValue = this->frameState.backendTimelineIndex;
                const VkTimelineSemaphoreSubmitInfo timeline{
                    .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                    .waitSemaphoreValueCount = 1,
                    .pWaitSemaphoreValues = &backendValue,
                };
                const VkSubmitInfo submit{
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &timeline,
                    .waitSemaphoreCount = 1, .pWaitSemaphores = &backend, .pWaitDstStageMask = &stage,
                    .commandBufferCount = 1, .pCommandBuffers = &command,
                    .signalSemaphoreCount = 1, .pSignalSemaphores = &pass.staged->handle(),
                };
                {
                    const std::lock_guard lock(queue.mutex);
                    const auto result = submitPrivate(queue, invocation.vk.df().QueueSubmit, submit, VK_NULL_HANDLE);
                    if (result != VK_SUCCESS)
                        throw ls::vulkan_error(result, "vkQueueSubmit() failed");
                    this->frameState.privateStagingValue = queue.tailNext;
                }
            } else {
                // Game-queue order protects this read from the next backend batch.
                pass.stagingCopy->submit(invocation.vk, {}, this->syncSemaphore->handle(),
                    this->frameState.backendTimelineIndex, std::array{pass.staged->handle()},
                    VK_NULL_HANDLE, 0);
            }
            if (present_diagnostics::pacingProfileEnabled()) {
                // D7c: the generated value this staging copy waits, before its increment below.
                present_diagnostics::noteFgStagingWait(this->diagnosticsState.contextId,
                    this->frameState.backendTimelineIndex);
            }
            DeferredPresent::GeneratedCopy copy{
                .device = invocation.vk.dev(),
                .acquire = invocation.vk.df().AcquireNextImageKHR,
                .submit = invocation.vk.df().QueueSubmit,
                .acquireSemaphore = pass.acquireSemaphore.handle(),
                .staged = pass.staged->handle(),
                // FI4: the previous output's signal, by its kind (see directPredecessor).
                .preceding = i ? directPredecessor(i, previousWorker) : VK_NULL_HANDLE,
                .continuation = last ? this->postCopySemaphores.at(
                    this->presentationSlotFor(invocation.imageIndex)).first.handle()
                    : pass.copied->handle(),
                .fence = last ? slot.fence.handle() : VK_NULL_HANDLE,
                .acquireProbe = pass.acquireFence ? &pass.acquireFence->probe : nullptr,
                .imageCount = static_cast<uint32_t>(this->info.images.size()),
                .giveUp = invocation.pacedOutputs->interval / 2,
                .internalImages = this->internalPresent != nullptr,
                // QH2: a transfer-recorded pass submits on the device's transfer queue.
                .transfer = pass.generatedOnTransfer ? this->info.transferCopy.get() : nullptr,
            };
            for (uint32_t k = 0; k < copy.imageCount; ++k) {
                copy.copies.at(k) = pass.generatedCopies.at(k).handle();
                copy.presentWaits.at(k) = this->postCopySemaphores.at(k).first.handle();
            }
            if (this->deferredPresent->enqueueGeneratedCopy(invocation.swapchain, generatedChain.head(),
                    invocation.pacedOutputs->at(i), copy,
                    DeferredPresent::OutputIdentity{this->frameState.backendFrameIndex - 1, plan.scheduledGeneratedFrames[i]},
                    this->reflexPresentId)) {
                if (privateStaging && present_diagnostics::pacingGpuSplitProfileEnabled() && this->pacingProfile) {
                    std::ostringstream record;
                    record << "framegen-vulkan: same-queue generated copy context=" << this->diagnosticsState.contextId
                           << " source=" << this->pacingProfile->sourceOrdinal() << '\n';
                    std::cerr << record.str();
                }
                this->deferredOutputQueuedThisFrame = true;
                if (copy.transfer && this->pacingProfile) this->pacingProfile->transferCopiesActive();
                if (this->pacingProfile) this->pacingProfile->generatedPresentMode(generatedMode);
                if (last) slot.inFlight = true;
                if (i == 0 && slot.outputQueries && this->pacingProfile) {
                    // The worker's pacing-profile-output record carries this batch's source, which
                    // beginDeferredPresentBatch set from the same ordinal in this present.
                    slot.outputSource = this->pacingProfile->sourceOrdinal();
                    slot.outputPending = true;
                    slot.outputDirect = false;
                }
                // Worker copies are counted on completion, when a pressure
                // skip can be distinguished from a submitted generated image.
                this->frameState.sequenceIndex++;
                this->frameState.backendTimelineIndex++;
                previousWorker = true;
                continue;
            }
            // Refused: no worker copy or skip will wait the staged signal, so
            // consume it here before the present-thread path. For i > 0 after a
            // worker copy, that pass's copied signal is consumed too, and the
            // inter-copy signal that the present-thread path waits for i > 0 is
            // signalled. After a direct copy the post-copy signal is already there,
            // so only the staged signal is consumed (FI4).
            workerCopies = false;
            const VkSemaphore stagedWait = pass.staged->handle();
            const bool chainedFromWorker = i > 0 && previousWorker;
            const std::array<VkSemaphore, 2> releaseWaits{
                stagedWait, chainedFromWorker ? slot.passes.at(i - 1).copied->handle() : VK_NULL_HANDLE};
            const std::array<VkPipelineStageFlags, 2> releaseStages{
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
            const VkSemaphore interCopy = chainedFromWorker ? this->postCopySemaphores.at(
                (this->frameState.sequenceIndex - 1) % this->postCopySemaphores.size()).second.handle()
                : VK_NULL_HANDLE;
            const VkSubmitInfo release{
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .waitSemaphoreCount = chainedFromWorker ? 2u : 1u, .pWaitSemaphores = releaseWaits.data(),
                .pWaitDstStageMask = releaseStages.data(),
                .signalSemaphoreCount = chainedFromWorker ? 1u : 0u, .pSignalSemaphores = chainedFromWorker ? &interCopy : nullptr,
            };
            const auto released = invocation.vk.df().QueueSubmit(
                invocation.vk.queue(), 1, &release, VK_NULL_HANDLE);
            if (released != VK_SUCCESS)
                throw ls::vulkan_error(released, "vkQueueSubmit() failed");
        }

        bool acquireBudgetExhausted = false;
        if (!direct && plan.generatedImagesPreacquired) {
            acquiredImageIndex = plan.preacquiredGeneratedImages.at(i);
            result = VK_SUCCESS;
        } else if (!direct) {
            // Recovery classification is active even when diagnostics are
            // disabled, so this one clock sample cannot use the opt-in timer.
            const auto acquireStarted = DiagnosticsClock::now();
            const auto consumedAcquireNanoseconds =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    totalAcquireDuration
                ).count();
            const auto remainingAcquireBudget =
                remainingGeneratedImageAcquireBudget(
                    plan.configuredAcquireTimeout,
                    consumedAcquireNanoseconds > 0
                        ? static_cast<uint64_t>(consumedAcquireNanoseconds)
                        : 0
                );
            acquireBudgetExhausted = remainingAcquireBudget &&
                *remainingAcquireBudget == 0;
            // A stalled safety wait leaves later acquires nonblocking until
            // one generated image is available again.
            const uint64_t acquireTimeout = this->generatedAcquireStalled ? 0 :
                orderedGeneratedImageAcquireTimeout(
                    std::nullopt, remainingAcquireBudget
                );
            if (acquireBudgetExhausted) {
                result = VK_TIMEOUT;
                acquireDeadlineExceeded = true;
            } else {
                // Never block acquisition while the worker may need to return
                // another image. Poll under its host-access lock, then seal and
                // drain the existing tail before ordinary bounded acquisition.
                bool polled = false;
                if (deferredTail) {
                    polled = true;
                    const auto pollSpareImage = [&] {
                        return this->deferredPresent->pollAcquire([&] {
                            return invocation.vk.df().AcquireNextImageKHR(
                                invocation.vk.dev(), invocation.swapchain, 0,
                                pass.acquireSemaphore.handle(), VK_NULL_HANDLE, &acquiredImageIndex);
                        });
                    };
                    result = pollSpareImage();
                    // While our batch is open, only the older overlapped batches can
                    // return images. Wait for all of them, then poll once more.
                    if ((result == VK_NOT_READY || result == VK_TIMEOUT) &&
                            this->deferredPresent->olderBatchPending() && this->deferredPresent->newestOpen()) {
                        static_cast<void>(this->completeDeferredPresent(UINT64_MAX, false, true));
                        result = pollSpareImage();
                    }
                    if (result == VK_NOT_READY || result == VK_TIMEOUT) {
                        this->deferredPresent->closeBatch();
                        const auto drained = this->completeDeferredPresent(UINT64_MAX);
                        deferredTail = false;
                        if (drained < 0) {
                            // Drain the scheduled backend and return the real
                            // application image through the existing partial-
                            // delivery path before propagating this error.
                            deferredAcquireError = drained;
                            result = VK_TIMEOUT;
                        }
                    }
                }
                if (!polled) {
                    result = invocation.vk.df().AcquireNextImageKHR(
                        invocation.vk.dev(), invocation.swapchain, acquireTimeout,
                        pass.acquireSemaphore.handle(), VK_NULL_HANDLE, &acquiredImageIndex);
                } else if (!deferredAcquireError && !deferredTail &&
                        (result == VK_NOT_READY || result == VK_TIMEOUT)) {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        DiagnosticsClock::now() - acquireStarted).count();
                    const auto remaining = remainingGeneratedImageAcquireBudget(
                        remainingAcquireBudget, static_cast<uint64_t>(std::max(int64_t{0}, elapsed)));
                    result = remaining && *remaining == 0 ? VK_TIMEOUT :
                        invocation.vk.df().AcquireNextImageKHR(
                            invocation.vk.dev(), invocation.swapchain,
                            this->generatedAcquireStalled ? 0 :
                                orderedGeneratedImageAcquireTimeout(std::nullopt, remaining),
                            pass.acquireSemaphore.handle(), VK_NULL_HANDLE, &acquiredImageIndex);
                }
            }
            const auto acquireDuration =
                DiagnosticsClock::now() - acquireStarted;
            totalAcquireDuration += acquireDuration;
            if (plan.configuredAcquireTimeout) {
                const auto acquireBudget =
                    std::chrono::duration_cast<DiagnosticsClock::duration>(
                        std::chrono::nanoseconds(
                            *plan.configuredAcquireTimeout
                        )
                    );
                if (totalAcquireDuration >= acquireBudget)
                    acquireDeadlineExceeded = true;
            }
        }

        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            this->generatedAcquireStalled = false;
        } else if (result == VK_TIMEOUT && !plan.configuredAcquireTimeout &&
                !deferredAcquireError) {
            // The unconfigured safety bound fired, not an explicit budget.
            this->generatedAcquireStalled = true;
        }

        if (result == VK_TIMEOUT || result == VK_NOT_READY) {
            reportOrderedAcquire(!acquireBudgetExhausted);
            // Backend work is already scheduled on this ordered path, so drain
            // its final timeline value. Fixed and explicit recovery probes
            // retain their direct transport contracts.
            const size_t skippedFrames =
                plan.scheduledGeneratedFrames.size() - i;
            const uint64_t finalGeneratedTimelineValue =
                this->frameState.backendTimelineIndex + skippedFrames - 1;
            auto& fallbackSemaphore = this->postCopySemaphores.at(
                this->presentationSlotFor(invocation.imageIndex)).first;
            std::array<VkSemaphore, 1> precedingCopy{};
            if (i) precedingCopy[0] = this->postCopySemaphores.at(
                (this->frameState.sequenceIndex - 1) % this->postCopySemaphores.size()).second.handle();

            const auto fallbackSubmitStarted = startPhaseTimer();
            auto& fallbackCommandBuffer = pass.commandBuffer;
            fallbackCommandBuffer.begin(invocation.vk);
            fallbackCommandBuffer.end(invocation.vk);
            fallbackCommandBuffer.submit(invocation.vk,
                std::span{precedingCopy}.first(i ? 1 : 0), this->syncSemaphore->handle(),
                finalGeneratedTimelineValue,
                std::array{fallbackSemaphore.handle()}, VK_NULL_HANDLE, 0,
                this->currentRenderSlot().fence.handle()
            );
            generatedSubmitDuration += finishPhaseTimer(
                fallbackSubmitStarted
            );
            this->currentRenderSlot().inFlight = true;

            this->frameState.sequenceIndex += skippedFrames;
            this->frameState.backendTimelineIndex += skippedFrames;

            result = this->presentOriginalImage(
                invocation, fallbackSemaphore.handle(),
                invocation.nextChain,
                &originalPresentDuration
            );
            if (deferredAcquireError) {
                this->deferredPresent->closeBatch();
                const auto drained = this->completeDeferredPresent(UINT64_MAX);
                if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
                    result = drained < 0 ? drained : *deferredAcquireError;
            }
            if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
                throw ls::vulkan_error(result, "vkQueuePresentKHR() failed");

            const auto presentWorkDuration = finishPhaseTimer(
                invocation.started
            );
            recordPresentPhases(
                this->diagnosticsState.contextId, presentWorkDuration,
                {
                    .renderFence = plan.renderFenceWaitDuration,
                    .schedule = plan.scheduleFramesDuration,
                    .sourceCopy = plan.submitSourceCopyDuration,
                    .acquire = totalAcquireDuration,
                    .generatedSubmit = generatedSubmitDuration,
                    .generatedPresent = generatedPresentDuration,
                    .originalPresent = originalPresentDuration,
                }
            );
            this->frameState.realFrameIndex++;
            return result;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            throw ls::vulkan_error(
                result, "vkAcquireNextImageKHR() failed"
            );
        }

        const auto acquiredSwapchainImage = this->info.images.at(
            acquiredImageIndex
        );
        auto& commandBuffer = pass.commandBuffer;
        auto& slot = this->currentRenderSlot();
        auto* const outputTimer = direct ? slot.outputQueries.get() : nullptr;
        commandBuffer.begin(invocation.vk);
        if (outputTimer) {
            outputTimer->reset(commandBuffer.handle());
            outputTimer->write(commandBuffer.handle(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
        }
        vk::acquireExternalImage(invocation.vk, commandBuffer.handle(), destinationImage.handle(),
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            vk::sharedImageOwnership(this->attachedGeneration(), invocation.vk.queueFamilyIndex()));
        const std::array preBarriers{
            barrierHelper(destinationImage.handle(),
                VK_ACCESS_NONE,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
            ),
            barrierHelper(acquiredSwapchainImage,
                VK_ACCESS_NONE,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
            ),
        };
        const std::array postBarriers{
            barrierHelper(acquiredSwapchainImage,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_MEMORY_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
            ),
        };
        commandBuffer.blitImage(invocation.vk,
            preBarriers,
            {destinationImage.handle(), acquiredSwapchainImage},
            destinationImage.getExtent(),
            postBarriers
        );
        if (outputTimer)
            outputTimer->write(commandBuffer.handle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);

        // FP2 rule 4: a parked image's copy waits the slot's semaphore, which its acquire signalled, not this pass's.
        std::array<VkSemaphore, 2> waitSemaphores{
            parked ? parked->semaphore : pass.acquireSemaphore.handle(), VK_NULL_HANDLE
        };
        size_t waitSemaphoreCount = 1;
        if (i) {
            // FI4: a direct copy after a worker copy waits that copy's signal. Every other copy waits the post-copy chain,
            // which a direct copy or a refusal (above) signals.
            const VkSemaphore previous = direct && previousWorker
                ? slot.passes.at(i - 1).copied->handle()
                : this->postCopySemaphores.at(
                    (this->frameState.sequenceIndex - 1) % this->postCopySemaphores.size()).second.handle();
            waitSemaphores.at(waitSemaphoreCount++) = previous;
        }
        // Presentation waits belong to acquired images, not a frame ring.
        // Reacquisition proves the previous presentation wait was consumed;
        // advancing sequenceIndex alone provides no such proof.
        const auto& presentSemaphore = this->postCopySemaphores.at(acquiredImageIndex).first;
        const VkSemaphore continuationSemaphore = i + 1 == plan.scheduledGeneratedFrames.size()
            ? this->postCopySemaphores.at(this->presentationSlotFor(invocation.imageIndex)).first.handle()
            : postCopy.second.handle();
        const std::array signalSemaphores{presentSemaphore.handle(), continuationSemaphore};

        commandBuffer.end(invocation.vk);
        const auto generatedSubmitStarted = startPhaseTimer();
        const auto copyFence = i == plan.scheduledGeneratedFrames.size() - 1
            ? this->currentRenderSlot().fence.handle() : VK_NULL_HANDLE;
        try {
            present_diagnostics::observeCopySubmission({
                .context = this->diagnosticsState.contextId,
                .source = this->pacingProfile ? this->pacingProfile->sourceOrdinal() : 0,
                .swapchain = invocation.swapchain, .generated = true,
                .imageIndex = acquiredImageIndex, .pass = static_cast<uint32_t>(i),
                .device = invocation.vk.dev(), .queue = invocation.vk.queue(), .command = commandBuffer.handle(),
                .waits = std::span{waitSemaphores}.first(waitSemaphoreCount), .signals = signalSemaphores,
                .timelineWait = this->syncSemaphore->handle(), .waitValue = this->frameState.backendTimelineIndex,
                .fence = copyFence,
            }, [&] {
                commandBuffer.submit(invocation.vk,
                    std::span{waitSemaphores}.first(waitSemaphoreCount),
                    this->syncSemaphore->handle(), this->frameState.backendTimelineIndex,
                    signalSemaphores, VK_NULL_HANDLE, 0, copyFence
                );
            });
            if (parked) {
                // FP2 rule 4: an empty submit after the copy signals the slot's fence, so the slot may be acquired again
                // once the copy has completed. The game thread owns the slot until this returns.
                const VkResult signalled = parkOwner->finishParkedCopy(invocation.vk.queue(), parked->slot);
                if (signalled != VK_SUCCESS)
                    throw ls::vulkan_error(signalled, "vkQueueSubmit() failed");
            }
        } catch (...) {
            if (parked) parkOwner->failParkedTake(parked->slot);
            throw;
        }
        if (i == plan.scheduledGeneratedFrames.size() - 1)
            slot.inFlight = true;
        if (outputTimer && this->pacingProfile) {
            slot.outputSource = this->pacingProfile->sourceOrdinal();
            slot.outputPending = true;
            slot.outputDirect = true;
        }
        generatedSubmitDuration += finishPhaseTimer(
            generatedSubmitStarted
        );

        if (this->pacingProfile) this->pacingProfile->generatedPresentMode(generatedMode);
        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = generatedChain.head(),
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &presentSemaphore.handle(),
            .swapchainCount = 1,
            .pSwapchains = &invocation.swapchain,
            .pImageIndices = &acquiredImageIndex,
        };
        if (deferredTail && (deferredFirst || i > 0))
            this->deferredPresentDeadline = invocation.pacedOutputs->at(i);
        else if (invocation.pacedOutputs)
            invocation.waitForOutput(invocation.pacedOutputs->at(i));
        else if (invocation.originalPresentDeadline)
            invocation.waitForOutput(*invocation.originalPresentDeadline);
        const auto generatedPresentStarted = startPhaseTimer();
        result = this->queuePresentWithRetirementFence(
            invocation.vk, invocation.queue, presentInfo, {
                .scheduled = invocation.pacedOutputs ? invocation.pacedOutputs->at(i)
                    : invocation.originalPresentDeadline.value_or(DiagnosticsClock::time_point{}),
                .generated = true, .copyExpected = true,
                .directGenerated = direct,
                .backendFrame = this->frameState.backendFrameIndex - 1,
                .phase = plan.scheduledGeneratedFrames[i],
            }
        );
        const auto oneGeneratedPresentDuration = finishPhaseTimer(
            generatedPresentStarted
        );
        generatedPresentDuration += oneGeneratedPresentDuration;
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            // The generated image belongs to framegen-vulkan, but the application's real
            // image is still acquired and its copy submission has already
            // consumed the application's wait semaphores. Return that real
            // image through the normal lower present path before propagating
            // OUT_OF_DATE, otherwise a game can block while tearing down an
            // acquired image that never reached the presentation engine.
            // FI2: the real goes through the worker copy here, not the call, so an eligible batch counts it as a fallback.
            if (inlineBatch && this->pacingProfile) this->pacingProfile->inlineCallFallbackActive(InlineCallOutcome::Other);
            const auto originalResult = this->presentOriginalImage(
                invocation, continuationSemaphore,
                invocation.nextChain,
                &originalPresentDuration
            );
            if (originalResult != VK_SUCCESS &&
                    originalResult != VK_SUBOPTIMAL_KHR &&
                    originalResult != VK_ERROR_OUT_OF_DATE_KHR) {
                throw ls::vulkan_error(
                    originalResult, "vkQueuePresentKHR() failed"
                );
            }
            this->frameState.sequenceIndex++;
            this->frameState.backendTimelineIndex++;
            this->frameState.realFrameIndex++;
            return VK_ERROR_OUT_OF_DATE_KHR;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            throw ls::vulkan_error(result, "vkQueuePresentKHR() failed");

        this->frameState.sequenceIndex++;
        this->frameState.backendTimelineIndex++;
        previousWorker = false;
    }

    // FI2 rule 5: the real may be copied at this call only when the batch is eligible and every output went direct. The
    // flag is read by enqueueInternalRealPresent and cleared here, so no other present sees it.
    // FP1: a real of a batch with a fallback is a cascade, and is not attempted at the call.
    this->inlineRealCall = {
        .eligible = inlineBatch && inlineAllDirect,
        .batch = inlineBatch,
        .outcome = inlineBatch && !inlineAllDirect ? InlineCallOutcome::Cascade : InlineCallOutcome::None,
    };
    const auto result = this->presentOriginalImage(
        invocation, this->postCopySemaphores.at(this->presentationSlotFor(invocation.imageIndex)).first.handle(),
        invocation.nextChain,
        &originalPresentDuration
    );
    const InlineCallOutcome realOutcome = this->inlineRealCall.outcome;
    this->inlineRealCall = {};
    if (inlineBatch && this->pacingProfile) {
        if (inlineCallServed(realOutcome)) {
            this->pacingProfile->inlineCallRealActive();
            if (realOutcome == InlineCallOutcome::Parked) this->pacingProfile->inlineCallParkedActive();
        } else {
            this->pacingProfile->inlineCallFallbackActive(inlineCallFallbackReason(realOutcome));
        }
    }
    const bool originalPresentRequestedRecreation =
        result == VK_ERROR_OUT_OF_DATE_KHR;
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR &&
            !originalPresentRequestedRecreation)
        throw ls::vulkan_error(result, "vkQueuePresentKHR() failed");

    const auto presentWorkDuration = finishPhaseTimer(
        invocation.started
    );
    reportOrderedAcquire(false);
    recordPresentPhases(
        this->diagnosticsState.contextId, presentWorkDuration,
        {
            .renderFence = plan.renderFenceWaitDuration,
            .schedule = plan.scheduleFramesDuration,
            .sourceCopy = plan.submitSourceCopyDuration,
            .acquire = totalAcquireDuration,
            .generatedSubmit = generatedSubmitDuration,
            .generatedPresent = generatedPresentDuration,
            .originalPresent = originalPresentDuration,
        }
    );
    this->frameState.realFrameIndex++;
    return result;
}

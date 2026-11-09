/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain/swapchain.hpp"
#include "frame_content_capture.hpp"
#include "swapchain/present/internal.hpp"
#include "swapchain/retirement.hpp"
#include "adaptive_scheduler.hpp"
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
#include <exception>
#include <iostream>
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
        const PresentationFramePlan& plan,
        const bool gamescopeHdrTransport) {
    const bool adaptiveVariableRefreshBounded =
        plan.adaptiveOrderedDeliveryPolicy ==
            AdaptiveOrderedDeliveryPolicy::VariableRefreshBounded;
    auto maximumAcquireDuration = plan.preacquireDuration;
    auto totalAcquireDuration = plan.preacquireDuration;
    auto generatedSubmitDuration = DiagnosticsClock::duration::zero();
    auto generatedPresentDuration = DiagnosticsClock::duration::zero();
    auto originalPresentDuration = DiagnosticsClock::duration::zero();
    bool acquireDeadlineExceeded = false;
    std::optional<VkResult> deferredAcquireError;
    const GeneratedPresentPNextChain generatedChain(
        this->gamescopeDetected ? nullptr : invocation.nextChain);
    const bool deferredFirst = invocation.deferredOutputEligible && invocation.pacedOutputs;
    bool deferredTail = invocation.deferredOutputEligible && invocation.pacedOutputs &&
        (deferredFirst || plan.scheduledGeneratedFrames.size() > 1) &&
        this->beginDeferredPresentBatch(invocation.vk,
            plan.scheduledGeneratedFrames.size() + (deferredFirst ? 1 : 0),
            this->frameState.backendTimelineIndex + plan.scheduledGeneratedFrames.size() - 1,
            invocation.readinessDeadline, invocation.capReadyOutputPacing,
            invocation.releasedOutputLimit, invocation.releasedJoinTolerance,
            invocation.releasedOverlap,
            invocation.pacedOutputs ? invocation.pacedOutputs->interval : DiagnosticsClock::duration{},
            invocation.limiterArrival);
    struct CloseDeferredBatch {
        DeferredPresent* worker;
        ~CloseDeferredBatch() { if (worker) worker->closeBatch(); }
    } closeDeferredBatch{deferredTail ? this->deferredPresent.get() : nullptr};
    uint64_t lastAcquireTimeout = 0;
    const auto reportOrderedAcquire = [&](const bool timedOut,
            const bool budgetExhausted,
            const size_t presentedGeneratedFrames) {
        if (!this->privateOrderedTransport)
            return;

        const auto observedAt = DiagnosticsClock::now();
        const bool transitionRecoveryActive = this->recoveryState
            .orderedAcquireRecovery.transitionRecoveryActive();
        const auto observation =
            this->recoveryState.orderedAcquireRecovery.observe(
                observedAt, timedOut, acquireDeadlineExceeded,
                plan.boundedOrderedAcquireProbe
            );
        if (transitionRecoveryActive && (timedOut || budgetExhausted) &&
                this->adaptiveScheduler) {
            static_cast<void>(this->adaptiveScheduler
                ->rejectActiveRampForTransportMiss(observedAt));
        }
        if (observation.recovered && transitionRecoveryActive &&
                this->adaptiveScheduler) {
            this->adaptiveScheduler->beginTransportRecovery(
                observedAt, true
            );
        }
        if (observation.quarantined) {
            this->fixedRefreshBudget.reset();
            if (presentDiagnosticsEnabled()) {
                std::cerr << "framegen-vulkan: present diagnostics: "
                             "operation=ordered-acquire-quarantine"
                          << " context=" << this->diagnosticsState.contextId
                          << " phase=native-drain"
                          << " reason="
                          << (budgetExhausted
                                ? "budget-exhausted"
                                : "generated-image-timeout")
                          << " acquire_total_ms="
                          << std::chrono::duration<double, std::milli>(
                                 totalAcquireDuration
                             ).count()
                          << " acquire_max_ms="
                          << std::chrono::duration<double, std::milli>(
                                 maximumAcquireDuration
                             ).count()
                          << " acquire_budget_ms="
                          << (plan.configuredAcquireTimeout
                                ? static_cast<double>(
                                      *plan.configuredAcquireTimeout
                                  ) / 1'000'000.0
                                : 0.0)
                          << " acquire_attempt_timeout_ms="
                          << static_cast<double>(lastAcquireTimeout) /
                                1'000'000.0
                          << " consecutive_failures="
                          << observation.consecutiveFailures
                          << " retry_ms="
                          << std::chrono::duration<double, std::milli>(
                                 observation.retryDelay
                             ).count()
                          << " bypassed_frames="
                          << observation.bypassedFrames
                          << " recovery_ms="
                          << std::chrono::duration<double, std::milli>(
                                 observation.recoveryDuration
                             ).count()
                          << " requested_generated="
                          << plan.requestedGeneratedFrames.size()
                          << " admitted_generated="
                          << plan.admittedGeneratedFrameCount
                          << " presented_generated="
                          << presentedGeneratedFrames
                          << " transition_authorized="
                          << transitionRecoveryActive
                          << " frame=" << this->frameState.realFrameIndex
                          << " sequence=" << this->frameState.sequenceIndex
                          << " action=native-drain\n";
            }
        } else if (observation.recovered && presentDiagnosticsEnabled()) {
            std::cerr << "framegen-vulkan: present diagnostics: "
                         "operation=ordered-acquire-recovered"
                      << " context=" << this->diagnosticsState.contextId
                      << " consecutive_failures="
                      << observation.consecutiveFailures
                      << " requested_generated="
                      << plan.requestedGeneratedFrames.size()
                      << " admitted_generated="
                      << plan.admittedGeneratedFrameCount
                      << " presented_generated="
                      << presentedGeneratedFrames
                      << " action=generated-resume\n";
        }
    };

    for (size_t i = 0; i < plan.scheduledGeneratedFrames.size(); ++i) {
        auto& postCopy = this->postCopySemaphores.at(
            this->frameState.sequenceIndex % this->postCopySemaphores.size()
        );
        auto& destinationImage = this->destinationImages.at(i);
        auto& pass = this->currentRenderSlot().passes.at(i);

        // A released native batch with one generated image lets the worker acquire,
        // copy and present it at its deadline. Any refusal keeps the path below.
        if (deferredTail && invocation.releasedOverlap && i == 0 &&
                plan.scheduledGeneratedFrames.size() == 1 &&
                !plan.generatedImagesPreacquired && !this->spatialScaler &&
                !this->frameContentCapture && !this->nativeCopyProfile &&
                !this->gamescopeDetected && this->deferredPresent &&
                this->recordGeneratedCopies(invocation.vk, this->currentRenderSlot())) {
            auto& slot = this->currentRenderSlot();
            // The staging copy stays on the game queue, ahead of the source copy
            // that releases the next backend batch. Its signal is consumed below.
            slot.stagingCopy->submit(invocation.vk, {}, this->syncSemaphore->handle(),
                this->frameState.backendTimelineIndex, std::array{slot.staged->handle()},
                VK_NULL_HANDLE, 0);
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
                .staged = slot.staged->handle(),
                .continuation = this->postCopySemaphores.at(
                    this->presentationSlotFor(invocation.imageIndex)).first.handle(),
                .fence = slot.fence.handle(),
                .imageCount = static_cast<uint32_t>(this->info.images.size()),
                .giveUp = invocation.pacedOutputs->interval / 2,
                .internalImages = this->internalPresent != nullptr,
            };
            for (uint32_t k = 0; k < copy.imageCount; ++k) {
                copy.copies.at(k) = slot.generatedCopies.at(k).handle();
                copy.presentWaits.at(k) = this->postCopySemaphores.at(k).first.handle();
            }
            if (this->deferredPresent->enqueueGeneratedCopy(invocation.swapchain, generatedChain.head(),
                    invocation.pacedOutputs->at(i), copy,
                    DeferredPresent::OutputIdentity{this->frameState.backendFrameIndex - 1, plan.scheduledGeneratedFrames[i]})) {
                this->deferredOutputQueuedThisFrame = true;
                slot.inFlight = true;
                if (slot.outputQueries && this->pacingProfile) {
                    // The worker's pacing-profile-output record carries this batch's source, which
                    // beginDeferredPresentBatch set from the same ordinal in this present.
                    slot.outputSource = this->pacingProfile->sourceOrdinal();
                    slot.outputPending = true;
                }
                // Worker copies are counted on completion, when a pressure
                // skip can be distinguished from a submitted generated image.
                this->frameState.sequenceIndex++;
                this->frameState.backendTimelineIndex++;
                continue;
            }
            // Refused: no worker copy or skip will wait the staged signal, so
            // consume it here before the present-thread path.
            const VkSemaphore stagedWait = slot.staged->handle();
            const VkPipelineStageFlags stagedStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            const VkSubmitInfo release{
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .waitSemaphoreCount = 1, .pWaitSemaphores = &stagedWait,
                .pWaitDstStageMask = &stagedStage,
            };
            const auto released = invocation.vk.df().QueueSubmit(
                invocation.vk.queue(), 1, &release, VK_NULL_HANDLE);
            if (released != VK_SUCCESS)
                throw ls::vulkan_error(released, "vkQueueSubmit() failed");
        }

        uint32_t acquiredImageIndex{};
        VkResult result{};
        bool acquireBudgetExhausted = false;
        if (plan.generatedImagesPreacquired) {
            acquiredImageIndex = plan.preacquiredGeneratedImages.at(i);
            result = VK_SUCCESS;
        } else {
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
                    this->gamescopeRefreshHz, remainingAcquireBudget
                );
            lastAcquireTimeout = acquireTimeout;
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
                                orderedGeneratedImageAcquireTimeout(this->gamescopeRefreshHz, remaining),
                            pass.acquireSemaphore.handle(), VK_NULL_HANDLE, &acquiredImageIndex);
                }
            }
            const auto acquireDuration =
                DiagnosticsClock::now() - acquireStarted;
            maximumAcquireDuration = std::max(
                maximumAcquireDuration, acquireDuration
            );
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
            if (acquireBudgetExhausted && presentDiagnosticsEnabled()) {
                std::cerr << "framegen-vulkan: present diagnostics: "
                             "operation=ordered-acquire-budget-exhausted"
                          << " context=" << this->diagnosticsState.contextId
                          << " phase=acquire"
                          << " acquire_total_ms="
                          << std::chrono::duration<double, std::milli>(
                                 totalAcquireDuration
                             ).count()
                          << " acquire_max_ms="
                          << std::chrono::duration<double, std::milli>(
                                 maximumAcquireDuration
                             ).count()
                          << " budget_ms="
                          << static_cast<double>(
                                 *plan.configuredAcquireTimeout
                             ) / 1'000'000.0
                          << " requested_generated="
                          << plan.requestedGeneratedFrames.size()
                          << " admitted_generated="
                          << plan.admittedGeneratedFrameCount
                          << " presented_generated=" << i
                          << " frame=" << this->frameState.realFrameIndex
                          << " sequence=" << this->frameState.sequenceIndex
                          << " action=stop-acquiring\n";
            }
            logSlowPresentOperation(
                "acquire-generated-image", this->frameState.realFrameIndex,
                this->frameState.sequenceIndex,
                acquireStarted, result, i, acquiredImageIndex
            );
        }

        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            this->generatedAcquireStalled = false;
        } else if (result == VK_TIMEOUT && !plan.configuredAcquireTimeout &&
                !deferredAcquireError) {
            // The unconfigured safety bound fired, not an explicit budget.
            this->generatedAcquireStalled = true;
        }

        if (result == VK_TIMEOUT || result == VK_NOT_READY) {
            // Record this normal batch's delivery loss before changing its
            // transport state. Otherwise intermittent timeouts separated by
            // healthy batches disappear from Adaptive's multiplier evaluation
            // even though generated outputs were lost.
            this->reportAdaptiveDelivery(plan, i);
            if (adaptiveVariableRefreshBounded) {
                const bool logPressure = this->recoveryState
                    .generatedImageAdmission.reportUnavailable();
                this->handleGeneratedImageAdmissionPressure(
                    plan, i, logPressure, false,
                    false,
                    lastAcquireTimeout,
                    "adaptive-fallback"
                );
            } else {
                reportOrderedAcquire(
                    !acquireBudgetExhausted, acquireBudgetExhausted, i
                );
            }
            // Backend work is already scheduled on this ordered path, so drain
            // its final timeline value. Fixed-refresh Adaptive retains the 3.3
            // ordered recovery contract. A bounded VRR timeout instead arms
            // zero-wait admission for later frames without changing the
            // scheduler's validated load. Fixed and explicit recovery probes
            // retain their direct transport contracts.
            const size_t skippedFrames =
                plan.scheduledGeneratedFrames.size() - i;
            if (!this->adaptiveScheduler)
                this->diagnosticsState.fixedSkippedFrames += skippedFrames;
            const uint64_t finalGeneratedTimelineValue =
                this->frameState.backendTimelineIndex + skippedFrames - 1;
            auto& fallbackSemaphore = this->postCopySemaphores.at(
                this->presentationSlotFor(invocation.imageIndex)).first;
            std::array<VkSemaphore, 1> precedingCopy{};
            if (i) precedingCopy[0] = this->postCopySemaphores.at(
                (this->frameState.sequenceIndex - 1) % this->postCopySemaphores.size()).second.handle();

            const auto fallbackSubmitStarted = startPresentDiagnostic();
            auto& fallbackCommandBuffer = pass.commandBuffer;
            fallbackCommandBuffer.begin(invocation.vk);
            fallbackCommandBuffer.end(invocation.vk);
            fallbackCommandBuffer.submit(invocation.vk,
                std::span{precedingCopy}.first(i ? 1 : 0), this->syncSemaphore->handle(),
                finalGeneratedTimelineValue,
                std::array{fallbackSemaphore.handle()}, VK_NULL_HANDLE, 0,
                this->currentRenderSlot().fence.handle()
            );
            generatedSubmitDuration += finishPresentDiagnostic(
                fallbackSubmitStarted
            );
            this->currentRenderSlot().inFlight = true;

            logPresentFallback(
                this->frameState.realFrameIndex, this->frameState.sequenceIndex, i, skippedFrames,
                finalGeneratedTimelineValue, "initial-timeout", "scheduled"
            );
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

            const auto presentWorkDuration = finishPresentDiagnostic(
                invocation.started
            );
            logSlowPresentOperation(
                "present-total", this->frameState.realFrameIndex, this->frameState.sequenceIndex,
                invocation.started, result
            );
            logSlowPresentBreakdown(
                this->diagnosticsState.contextId,
                this->frameState.realFrameIndex,
                this->frameState.sequenceIndex, presentWorkDuration,
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
            if (presentDiagnosticsEnabled()) {
                std::cerr << "framegen-vulkan: present diagnostics: "
                             "operation=generated-delivery-miss"
                          << " context=" << this->diagnosticsState.contextId
                          << " reason="
                          << (acquireBudgetExhausted
                                ? "acquire-budget-exhausted"
                                : "acquire-timeout")
                          << " planned="
                          << plan.requestedGeneratedFrames.size()
                          << " on_time=" << i
                          << " deadline_ms="
                          << static_cast<double>(
                                acquireBudgetExhausted
                                    ? *plan.configuredAcquireTimeout
                                    : lastAcquireTimeout
                             ) / 1'000'000.0
                          << " deadline_scope="
                          << (acquireBudgetExhausted
                                ? "application-present"
                                : "generated-image")
                          << " application_present_budget_ms="
                          << static_cast<double>(
                                plan.configuredAcquireTimeout.value_or(0)
                             ) / 1'000'000.0
                          << '\n';
            }
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
        commandBuffer.begin(invocation.vk);
        if (this->nativeCopyProfile)
            this->nativeCopyProfile->beginCopy(commandBuffer.handle(), invocation.vk.queue(), acquiredImageIndex, true, static_cast<uint32_t>(i));
        vk::acquireExternalImage(invocation.vk, commandBuffer.handle(), destinationImage.handle(),
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        if (this->spatialScaler &&
                this->spatialFramePipelinePlacement ==
                    SpatialFramePipelinePlacement::PostFrameGeneration) {
            this->spatialScaler->recordSourceToPresentation(
                invocation.vk, commandBuffer,
                destinationImage.handle(), acquiredSwapchainImage
            );
        } else {
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
        }

        std::array<VkSemaphore, 2> waitSemaphores{
            pass.acquireSemaphore.handle(), VK_NULL_HANDLE
        };
        size_t waitSemaphoreCount = 1;
        if (i) {
            const auto& previousPostCopy = this->postCopySemaphores.at(
                (this->frameState.sequenceIndex - 1) % this->postCopySemaphores.size()
            );
            waitSemaphores.at(waitSemaphoreCount++) = previousPostCopy.second.handle();
        }
        // Presentation waits belong to acquired images, not a frame ring.
        // Reacquisition proves the previous presentation wait was consumed;
        // advancing sequenceIndex alone provides no such proof.
        const auto& presentSemaphore = this->postCopySemaphores.at(acquiredImageIndex).first;
        const VkSemaphore continuationSemaphore = i + 1 == plan.scheduledGeneratedFrames.size()
            ? this->postCopySemaphores.at(this->presentationSlotFor(invocation.imageIndex)).first.handle()
            : postCopy.second.handle();
        const std::array signalSemaphores{presentSemaphore.handle(), continuationSemaphore};

        if (i == 0 && this->frameContentCapture)
            this->frameContentCapture->record(invocation.vk, commandBuffer.handle(),
                acquiredSwapchainImage, this->info.extent, acquiredImageIndex, true);
        if (this->nativeCopyProfile)
            this->nativeCopyProfile->endCopy(commandBuffer.handle(), true, static_cast<uint32_t>(i));
        commandBuffer.end(invocation.vk);
        const auto generatedSubmitStarted = startPresentDiagnostic();
        const auto copyFence = i == plan.scheduledGeneratedFrames.size() - 1
            ? this->currentRenderSlot().fence.handle() : VK_NULL_HANDLE;
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
        if (i == plan.scheduledGeneratedFrames.size() - 1)
            this->currentRenderSlot().inFlight = true;
        generatedSubmitDuration += finishPresentDiagnostic(
            generatedSubmitStarted
        );
        logSlowPresentOperation(
            "submit-generated-copy", this->frameState.realFrameIndex,
            this->frameState.sequenceIndex,
            generatedSubmitStarted, std::nullopt, i
        );

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
        const auto generatedPresentStarted = startPresentDiagnostic();
        result = this->queuePresentWithRetirementFence(
            invocation.vk, invocation.queue, presentInfo, {
                .scheduled = invocation.pacedOutputs ? invocation.pacedOutputs->at(i)
                    : invocation.originalPresentDeadline.value_or(DiagnosticsClock::time_point{}),
                .generated = true, .copyExpected = true,
                .backendFrame = this->frameState.backendFrameIndex - 1,
                .phase = plan.scheduledGeneratedFrames[i],
            }
        );
        const auto oneGeneratedPresentDuration = finishPresentDiagnostic(
            generatedPresentStarted
        );
        generatedPresentDuration += oneGeneratedPresentDuration;
        logSlowPresentOperation(
            "present-generated-image", this->frameState.realFrameIndex,
            this->frameState.sequenceIndex,
            generatedPresentStarted, result, i, acquiredImageIndex
        );
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            // The generated image belongs to framegen-vulkan, but the application's real
            // image is still acquired and its copy submission has already
            // consumed the application's wait semaphores. Return that real
            // image through the normal lower present path before propagating
            // OUT_OF_DATE, otherwise a game can block while tearing down an
            // acquired image that never reached the presentation engine.
            const auto originalResult = this->presentOriginalImage(
                invocation, continuationSemaphore,
                invocation.nextChain,
                &originalPresentDuration
            );
            if (presentDiagnosticsEnabled()) {
                std::cerr << "framegen-vulkan: present diagnostics: "
                             "operation=generated-present-recreation-drain"
                          << " context=" << this->diagnosticsState.contextId
                          << " frame=" << this->frameState.realFrameIndex
                          << " sequence=" << this->frameState.sequenceIndex
                          << " pass=" << i
                          << " generated_result=" << result
                          << " original_result=" << originalResult
                          << " action=return-application-image-before-recreation\n";
            }
            if (originalResult != VK_SUCCESS &&
                    originalResult != VK_SUBOPTIMAL_KHR &&
                    originalResult != VK_ERROR_OUT_OF_DATE_KHR) {
                throw ls::vulkan_error(
                    originalResult, "vkQueuePresentKHR() failed"
                );
            }
            if (this->adaptiveScheduler)
                this->reportAdaptiveDelivery(plan, i);
            else
                this->diagnosticsState.fixedSkippedFrames +=
                    plan.scheduledGeneratedFrames.size() - i;
            this->frameState.sequenceIndex++;
            this->frameState.backendTimelineIndex++;
            this->frameState.realFrameIndex++;
            return VK_ERROR_OUT_OF_DATE_KHR;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            throw ls::vulkan_error(result, "vkQueuePresentKHR() failed");
        if (!this->adaptiveScheduler)
            this->diagnosticsState.fixedGeneratedFrames++;

        this->frameState.sequenceIndex++;
        this->frameState.backendTimelineIndex++;
    }

    const auto result = this->presentOriginalImage(
        invocation, this->postCopySemaphores.at(this->presentationSlotFor(invocation.imageIndex)).first.handle(),
        invocation.nextChain,
        &originalPresentDuration
    );
    const bool originalPresentRequestedRecreation =
        result == VK_ERROR_OUT_OF_DATE_KHR;
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR &&
            !originalPresentRequestedRecreation)
        throw ls::vulkan_error(result, "vkQueuePresentKHR() failed");

    const auto presentWorkDuration = finishPresentDiagnostic(
        invocation.started
    );
    logSlowPresentOperation(
        "present-total", this->frameState.realFrameIndex,
        this->frameState.sequenceIndex, invocation.started, result
    );
    if (adaptiveVariableRefreshBounded) {
        if (plan.scheduledGeneratedFrames.size() ==
                plan.requestedGeneratedFrames.size()) {
            const size_t requiredStableBatches =
                adaptiveVariableRefreshBounded
                    ? generatedImageAdmissionRecoveryBatches(
                        this->info.images.size(),
                        plan.requestedGeneratedFrames.size()
                    )
                    : 1;
            this->reportGeneratedImageAdmissionAvailable(
                requiredStableBatches
            );
        }
    } else {
        reportOrderedAcquire(
            false, false, plan.scheduledGeneratedFrames.size()
        );
    }
    this->reportAdaptiveDelivery(
        plan, plan.scheduledGeneratedFrames.size()
    );
    logSlowPresentBreakdown(
        this->diagnosticsState.contextId, this->frameState.realFrameIndex,
        this->frameState.sequenceIndex, presentWorkDuration,
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

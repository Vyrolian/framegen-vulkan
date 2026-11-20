/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain/present_layout.hpp"
#include "swapchain/swapchain.hpp"
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

VkResult Swapchain::presentNativeFrame(const PresentInvocation& invocation) {
    return this->presentDirectApplicationFrame(invocation);
}

VkResult Swapchain::presentDirectApplicationFrame(
        const PresentInvocation& invocation,
        const std::string_view healthSource) {
    if (invocation.originalPresentDeadline)
        invocation.waitForOutput(*invocation.originalPresentDeadline);
    const VkPresentInfoKHR presentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = invocation.nextChain,
        .waitSemaphoreCount = static_cast<uint32_t>(
            invocation.waitSemaphores.size()
        ),
        .pWaitSemaphores = invocation.waitSemaphores.data(),
        .swapchainCount = 1,
        .pSwapchains = &invocation.swapchain,
        .pImageIndices = &invocation.imageIndex,
    };
    const auto originalPresentStarted = startPhaseTimer();
    const auto result = this->queuePresentWithRetirementFence(
        invocation.vk, invocation.queue, presentInfo, {
            .scheduled = invocation.originalPresentDeadline.value_or(DiagnosticsClock::time_point{}),
            .applicationImage = true,
        }
    );
    const PresentPhaseDurations phases{
        .originalPresent = finishPhaseTimer(originalPresentStarted),
    };
    const auto presentWorkDuration = finishPhaseTimer(
        invocation.started
    );
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        throw ls::vulkan_error(result, "vkQueuePresentKHR() failed");
    static_cast<void>(healthSource);

    recordPresentPhases(this->diagnosticsState.contextId, presentWorkDuration, phases);
    this->frameState.realFrameIndex++;
    return result;
}

VkResult Swapchain::presentOriginalImage(
        const PresentInvocation& invocation,
        const VkSemaphore waitSemaphore, const void* nextChain,
        DiagnosticsClock::duration* const duration) {
    VkSemaphore presentWaitSemaphore = waitSemaphore;
    const VkPresentInfoKHR presentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = nextChain,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &presentWaitSemaphore,
        .swapchainCount = 1,
        .pSwapchains = &invocation.swapchain,
        .pImageIndices = &invocation.imageIndex,
    };
    // The retained real output can be paced independently while the
    // game produces its next frame. The binary wait was already submitted by
    // the generated copy. All other transports retain synchronous ownership.
    if ((invocation.readyOutputPacing || invocation.capReadyOutputPacing) &&
            invocation.deferredOutputEligible && invocation.pacedOutputs) {
        this->deferredPresentDeadline = invocation.previousOutput
            ? std::max(*invocation.originalPresentDeadline,
                *invocation.previousOutput + invocation.pacedOutputs->interval)
            : *invocation.originalPresentDeadline;
    } else if (invocation.originalPresentDeadline) {
        invocation.waitForOutput(*invocation.originalPresentDeadline);
    }
    const auto originalPresentStarted = startPhaseTimer();
    const auto result = this->queuePresentWithRetirementFence(
        invocation.vk, invocation.queue, presentInfo, {
            .scheduled = invocation.originalPresentDeadline.value_or(DiagnosticsClock::time_point{}),
            .copyExpected = true,
            .applicationImage = true,
        }
    );
    if (duration)
        *duration = finishPhaseTimer(originalPresentStarted);
    return result;
}

void Swapchain::submitSourceCopy(const PresentInvocation& invocation,
        const VkImage swapchainImage, const vk::Image& sourceImage) {
    const auto& commandBuffer = this->currentRenderSlot().sourceCopy;
    const auto sourceLayout = this->internalPresent
        ? present_layout::backingLayout : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    const std::array preBarriers{
        barrierHelper(swapchainImage,
            VK_ACCESS_NONE,
            VK_ACCESS_TRANSFER_READ_BIT,
            sourceLayout,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
        ),
        barrierHelper(sourceImage.handle(),
            VK_ACCESS_NONE,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
        ),
    };
    const std::array postBarriers{
        vk::externalImageRelease(sourceImage.handle(), invocation.vk.queueFamilyIndex(),
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        barrierHelper(swapchainImage,
            VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_MEMORY_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            sourceLayout
        ),
    };
    commandBuffer.begin(invocation.vk);
    // Diagnostic split boundaries: two queries in the existing copy, no added barrier or wait.
    const bool splitRecorded = this->recordRenderSplitBegin(
        invocation.vk, commandBuffer.handle(), this->currentRenderSlot());
    commandBuffer.blitImage(invocation.vk,
        preBarriers,
        {swapchainImage, sourceImage.handle()},
        sourceImage.getExtent(),
        postBarriers
    );
    if (splitRecorded)
        this->recordRenderSplitEnd(commandBuffer.handle(), this->currentRenderSlot());
    commandBuffer.end(invocation.vk);

    const uint64_t sourceTimelineValue =
        this->frameState.backendTimelineIndex++;
    this->frameState.sequenceIndex++;
    // The copy overwrites the source image that the previous backend batch
    // read. That batch's S + G orders the overwrite. The next backend batch
    // also waits for the graphics queue's reads of the generated images.
    const uint64_t reuseWait = this->frameState.sourceReleaseTimelineValue;
    const uint64_t privateStaging = this->frameState.privateStagingValue;
    present_diagnostics::observeCopySubmission({
        .context = this->diagnosticsState.contextId,
        .source = this->pacingProfile ? this->pacingProfile->sourceOrdinal() : 0,
        .swapchain = invocation.swapchain, .imageIndex = invocation.imageIndex,
        .device = invocation.vk.dev(), .queue = invocation.vk.queue(), .command = commandBuffer.handle(),
        .waits = invocation.waitSemaphores,
        .timelineWait = reuseWait ? this->syncSemaphore->handle() : VK_NULL_HANDLE,
        .timelineSignal = this->syncSemaphore->handle(),
        .waitValue = reuseWait,
        .signalValue = sourceTimelineValue,
    }, [&] {
        if (privateStaging) {
            // Staging was submitted to the private queue. Wait for its tail
            // value before releasing the next backend batch, without waiting
            // for the subsequent final copy or present.
            const auto backend = this->syncSemaphore->handle();
            const auto command = commandBuffer.handle();
            std::vector<VkSemaphore> waits(invocation.waitSemaphores.begin(), invocation.waitSemaphores.end());
            std::vector<uint64_t> waitValues(waits.size(), 0);
            if (reuseWait) {
                waits.push_back(backend);
                waitValues.push_back(reuseWait);
            }
            waits.push_back(this->info.deferredPresentQueue->tail);
            waitValues.push_back(privateStaging);
            const std::vector<VkPipelineStageFlags> stages(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            const auto count = static_cast<uint32_t>(waits.size());
            const VkTimelineSemaphoreSubmitInfo timeline{
                .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                .waitSemaphoreValueCount = count, .pWaitSemaphoreValues = waitValues.data(),
                .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &sourceTimelineValue,
            };
            const VkSubmitInfo submit{
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &timeline,
                .waitSemaphoreCount = count, .pWaitSemaphores = waits.data(), .pWaitDstStageMask = stages.data(),
                .commandBufferCount = 1, .pCommandBuffers = &command,
                .signalSemaphoreCount = 1, .pSignalSemaphores = &backend,
            };
            const auto result = invocation.vk.df().QueueSubmit(invocation.vk.queue(), 1, &submit, VK_NULL_HANDLE);
            if (result != VK_SUCCESS)
                throw ls::vulkan_error(result, "vkQueueSubmit() failed");
            this->frameState.privateStagingValue = 0;
            return;
        }
        commandBuffer.submit(invocation.vk,
            invocation.waitSemaphores,
            reuseWait ? this->syncSemaphore->handle() : VK_NULL_HANDLE, reuseWait,
            {}, this->syncSemaphore->handle(), sourceTimelineValue
        );
    });
    if (present_diagnostics::pacingProfileEnabled()) {
        // D7c: the stall probe reads this copy's signal value on the FG timeline.
        present_diagnostics::noteFgSourceCopy(this->diagnosticsState.contextId,
            invocation.vk.dev(), this->syncSemaphore->handle(),
            invocation.vk.df().GetSemaphoreCounterValueKHR,
            this->pacingProfile ? this->pacingProfile->sourceOrdinal() : 0, sourceTimelineValue);
        this->fgTimelineLease.hold(this->diagnosticsState.contextId);
    }
    if (splitRecorded) {
        // Set only after the submit returns. The slot's existing fence later proves completion.
        auto& slot = this->currentRenderSlot();
        slot.splitSource = this->pacingProfile ? this->pacingProfile->sourceOrdinal() : 0;
        slot.splitPending = true;
    }
    this->frameState.sourceReleaseTimelineValue =
        this->frameState.nextSourceReleaseTimelineValue;
    this->frameState.nextSourceReleaseTimelineValue = 0;
}

bool Swapchain::recordRenderSplitBegin(const vk::Vulkan& vk,
        const VkCommandBuffer command, RenderSlot& slot) {
    if (!present_diagnostics::pacingGpuSplitProfileEnabled() || this->splitRenderUnavailable ||
            !this->instance || this->destinationImages.empty() ||
            !this->instance->splitProfileEnabled(this->ctx.get()))
        return false;
    try {
        if (!slot.splitQueries)
            slot.splitQueries = std::make_unique<vk::TimestampQueryPool>(vk, 2);
        this->ensureSplitRenderClock(vk, slot.splitQueries->timestampValidBits(),
            slot.splitQueries->timestampPeriodNanoseconds());
    } catch (const std::exception& error) {
        // Disable only the render half for this swapchain; the backend half is unaffected.
        this->splitRenderUnavailable = true;
        present_diagnostics::reportPacingGpuSplitUnavailable("queries", error.what());
        return false;
    }
    // Reset is recorded first so the pool holds only this copy's two boundaries.
    slot.splitQueries->reset(command);
    slot.splitQueries->write(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
    return true;
}

void Swapchain::recordRenderSplitEnd(const VkCommandBuffer command, RenderSlot& slot) {
    if (slot.splitQueries)
        slot.splitQueries->write(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);
}

present_diagnostics::SplitRenderHalf Swapchain::findRenderSplitHalf(const uint64_t source) const {
    if (source == 0)
        return {};
    for (const auto& half : this->splitRenderHalves) {
        if (half.source == source)
            return half;
    }
    return {};
}

VkResult Swapchain::presentHistoryOnly(
        const PresentInvocation& invocation,
        const PresentationFramePlan& plan) {
    PresentPhaseDurations phases{
        .renderFence = plan.renderFenceWaitDuration,
        .sourceCopy = plan.submitSourceCopyDuration,
    };
    const uint64_t sourceTimelineValue =
        this->frameState.backendTimelineIndex - 1;
    auto& fallbackPass = this->currentRenderSlot().passes.front();
    auto& fallbackSemaphore = this->postCopySemaphores.at(
        this->presentationSlotFor(invocation.imageIndex)).first;

    auto& fallbackCommandBuffer = fallbackPass.commandBuffer;
    const auto fallbackSubmitStarted = startPhaseTimer();
    fallbackCommandBuffer.begin(invocation.vk);
    fallbackCommandBuffer.end(invocation.vk);
    fallbackCommandBuffer.submit(invocation.vk,
        {}, this->syncSemaphore->handle(), sourceTimelineValue,
        std::array{fallbackSemaphore.handle()}, VK_NULL_HANDLE, 0,
        this->currentRenderSlot().fence.handle()
    );
    phases.generatedSubmit = finishPhaseTimer(
        fallbackSubmitStarted
    );
    this->currentRenderSlot().inFlight = true;

    const auto historyScheduleStarted = startPhaseTimer();
    try {
        this->instance->scheduleFrameHistory(this->ctx.get());
    } catch (const std::exception& error) {
        phases.schedule = finishPhaseTimer(historyScheduleStarted);
        // The fallback copy is already queued and signals fallbackSemaphore.
        // Present the real image through it and quarantine generation instead
        // of returning an error to the application.
        std::cerr << "framegen-vulkan: temporarily bypassing frame generation after "
                     "history scheduling failure; native presentation retained: "
                  << error.what() << '\n';
        this->recoveryState.backendPending = true;
        this->recoveryState.historyWarmupRemaining = 0;

        const auto fallbackResult = this->presentOriginalImage(
            invocation, fallbackSemaphore.handle(), invocation.nextChain,
            &phases.originalPresent
        );
        if (fallbackResult != VK_SUCCESS &&
                fallbackResult != VK_SUBOPTIMAL_KHR) {
            throw ls::vulkan_error(
                fallbackResult, "vkQueuePresentKHR() failed"
            );
        }
        const auto presentWorkDuration = finishPhaseTimer(
            invocation.started
        );
        recordPresentPhases(this->diagnosticsState.contextId, presentWorkDuration, phases);
        this->frameState.realFrameIndex++;
        return fallbackResult;
    }
    phases.schedule = finishPhaseTimer(historyScheduleStarted);

    if (this->pacingProfile) this->pacingProfile->backendScheduled(this->frameState.backendFrameIndex);
    this->frameState.backendFrameIndex++;
    if (this->recoveryState.historyWarmupRemaining > 0)
        this->recoveryState.historyWarmupRemaining--;

    const auto result = this->presentOriginalImage(
        invocation, fallbackSemaphore.handle(), invocation.nextChain,
        &phases.originalPresent
    );
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        throw ls::vulkan_error(result, "vkQueuePresentKHR() failed");

    const auto presentWorkDuration = finishPhaseTimer(
        invocation.started
    );
    recordPresentPhases(this->diagnosticsState.contextId, presentWorkDuration, phases);
    this->frameState.realFrameIndex++;
    return result;
}

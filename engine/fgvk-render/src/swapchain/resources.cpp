/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain/swapchain.hpp"
#include "present_diagnostics.hpp"
#include "swapchain/present/internal.hpp"
#include "fgvk-common/helpers/errors.hpp"
#include "fgvk-common/helpers/file_descriptors.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

using namespace fgvk;
using namespace fgvk::layer;

namespace {
    using DiagnosticsClock = present_diagnostics::Clock;
    constexpr auto privateResourceDrainBudget =
        std::chrono::milliseconds(50);

    void logPrivateFrameGenerationMemory(const vk::Vulkan& vk,
            const uint64_t contextId, const uint64_t stateRevision,
            const std::string_view operation) {
        const auto memory = vk.deviceMemorySnapshot();
        std::clog << "framegen-vulkan: renderer-memory operation=" << operation
                  << " context=" << contextId
                  << " state_revision=" << stateRevision
                  << " live_internal_bytes=" << memory.internal.bytes
                  << " live_internal_allocations="
                  << memory.internal.allocations
                  << " live_exported_bytes=" << memory.exported.bytes
                  << " live_exported_allocations="
                  << memory.exported.allocations
                  << " peak_internal_bytes=" << memory.peakInternal.bytes
                  << " peak_internal_allocations="
                  << memory.peakInternal.allocations
                  << " peak_exported_bytes=" << memory.peakExported.bytes
                  << " peak_exported_allocations="
                  << memory.peakExported.allocations << '\n';
    }
}

void Swapchain::ensureFrameGenerationExecutionResources(
        const vk::Vulkan& vk) {
    // These objects are WSI-facing and intentionally remain stable while the
    // full-resolution images and backend context are replaced. Preallocating
    // render slots avoids retiring binary semaphores that may still be
    // referenced by a previously queued presentation.
    while (this->renderSlots.size() < renderSlotDepth) {
        auto& slot = this->renderSlots.emplace_back(RenderSlot{
            .sourceCopy = vk::CommandBuffer(vk),
            .fence = vk::Fence(vk),
        });
        while (slot.passes.size() < GeneratedFramePlan::capacity) {
            slot.passes.emplace_back(RenderPass{
                .commandBuffer = vk::CommandBuffer(vk),
                .acquireSemaphore = vk::Semaphore(vk),
            });
        }
    }
    const size_t semaphoreFrames = std::max(
        this->info.images.size(), GeneratedFramePlan::capacity + 2
    );
    while (this->postCopySemaphores.size() < semaphoreFrames) {
        this->postCopySemaphores.emplace_back(
            vk::Semaphore(vk), vk::Semaphore(vk)
        );
    }
}

// The worker resubmits these copies, so they are recorded once per destination
// and swapchain image set. The slot's staging copy runs on the game queue at
// present time, before the source copy that releases the next backend batch.
// The worker therefore copies only staging into each swapchain image.
bool Swapchain::recordGeneratedCopies(const vk::Vulkan& vk, RenderSlot& slot, size_t pass) {
    if (pass >= this->destinationImages.size()) return false;
    RenderPass& renderPass = slot.passes.at(pass);
    if (renderPass.staging && renderPass.generatedCopies.size() == this->info.images.size()) return true;
    if (this->destinationImages.empty() ||
            this->info.images.size() > DeferredPresent::maxCopyImages)
        return false;
    // Diagnostic output split: two timestamps around each worker copy. Created once per slot.
    // A failure leaves the copies untimed, as they are without the split gate.
    if (pass == 0 && present_diagnostics::pacingGpuSplitProfileEnabled() && this->pacingProfile && !slot.outputQueries) {
        try {
            slot.outputQueries = std::make_unique<vk::TimestampQueryPool>(vk, 2);
        } catch (const std::exception& error) {
            present_diagnostics::reportPacingGpuSplitUnavailable("output-queries", error.what());
        } catch (...) {
            present_diagnostics::reportPacingGpuSplitUnavailable("output-queries", "unknown error");
        }
    }
    vk::TimestampQueryPool* const outputTimer = pass == 0 ? slot.outputQueries.get() : nullptr;
    try {
        const auto& destinationImage = this->destinationImages.at(pass);
        const VkExtent2D extent = destinationImage.getExtent();
        renderPass.staging.emplace(vk, extent, this->info.format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        const VkImage stagingImage = renderPass.staging->handle();
        renderPass.staged.emplace(vk);
        renderPass.copied.emplace(vk);
        renderPass.stagingCopy.emplace(vk);
        {
            const auto& command = *renderPass.stagingCopy;
            command.begin(vk, 0);
            vk::acquireExternalImage(vk, command.handle(), destinationImage.handle(),
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            // The staged signal orders this reset before the worker copies' timestamps.
            if (outputTimer) outputTimer->reset(command.handle());
            const std::array preBarriers{
                present_detail::barrierHelper(destinationImage.handle(),
                    VK_ACCESS_NONE,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                ),
                present_detail::barrierHelper(stagingImage,
                    VK_ACCESS_NONE,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                ),
            };
            // Leave staging readable by the worker's copies after the staged signal.
            const std::array postBarriers{
                present_detail::barrierHelper(stagingImage,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                ),
            };
            command.blitImage(vk, preBarriers,
                {destinationImage.handle(), stagingImage},
                extent, postBarriers);
            command.end(vk);
        }

        std::vector<vk::CommandBuffer> copies;
        copies.reserve(this->info.images.size());
        for (const VkImage swapchainImage : this->info.images) {
            copies.emplace_back(vk);
            const auto& command = copies.back();
            command.begin(vk, 0);
            const std::array preBarriers{
                present_detail::barrierHelper(swapchainImage,
                    VK_ACCESS_NONE,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                ),
            };
            const std::array postBarriers{
                present_detail::barrierHelper(swapchainImage,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_MEMORY_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                ),
            };
            if (outputTimer) outputTimer->write(command.handle(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
            command.blitImage(vk, preBarriers,
                {stagingImage, swapchainImage},
                extent, postBarriers);
            if (outputTimer) outputTimer->write(command.handle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);
            command.end(vk);
        }
        renderPass.generatedCopies = std::move(copies);
        return true;
    } catch (...) {
        renderPass.generatedCopies.clear();
        renderPass.stagingCopy.reset();
        renderPass.staged.reset();
        renderPass.copied.reset();
        renderPass.staging.reset();
        return false;
    }
}

void Swapchain::initializeGpuPacingProfile(backend::Instance& backend,
        backend::Context& context, const bool lowLatency) noexcept {
    // Split boundaries are independent of the drained GPU profile: the backend
    // enables only its non-draining state and the context's own fence reads. They
    // run for the diagnostics log, and for the GPU release of low_latency (L5) with
    // or without it. Retaining every completed sample is the log's alone.
    const bool diagnostics = this->pacingProfile && present_diagnostics::pacingGpuSplitProfileEnabled();
    if (diagnostics || lowLatency) {
        try {
            backend.enableSplitProfile(context, diagnostics);
        } catch (const std::exception& error) {
            present_diagnostics::reportPacingGpuSplitUnavailable("queries", error.what());
        }
    }
    if (!this->pacingProfile || !present_diagnostics::pacingGpuProfileEnabled()) return;
    try {
        backend.enableFrameProfiling(context, true);
    } catch (const std::exception& error) {
        std::cerr << "framegen-vulkan: GPU pacing profile unavailable; reason=query-initialization; "
                  << error.what() << '\n';
    }
}

Swapchain::FrameGenerationResources
Swapchain::buildFrameGenerationResources(const vk::Vulkan& vk,
        const SwapchainColorPipeline& pipeline,
        const ls::GameConf& resourceProfile) {
    if (!this->instance)
        throw ls::error("the frame-generation backend is unavailable");
    if (!pipeline.generationSupported)
        throw ls::error("the colour pipeline does not support frame generation");

    auto& backendInstance = *this->instance;
    const VkExtent2D extent = this->info.extent;
    std::vector<int> sourceFds(2, -1);
    ls::FileDescriptorScope sourceScope{sourceFds};
    std::vector<int> destinationFds(generatedFrameCapacityForProfile(resourceProfile), -1);
    ls::FileDescriptorScope destinationScope{destinationFds};
    FrameGenerationResources resources;
    resources.sourceImages.reserve(sourceFds.size());
    resources.destinationImages.reserve(destinationFds.size());
    const VkImageUsageFlags sourceImageUsage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    for (int& fd : sourceFds) {
        resources.sourceImages.emplace_back(vk,
            extent, pipeline.exchangeFormat,
            sourceImageUsage,
            std::nullopt, &fd);
    }
    for (int& fd : destinationFds) {
        resources.destinationImages.emplace_back(vk,
            extent, pipeline.exchangeFormat,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            std::nullopt, &fd);
    }

    int syncFd{-1};
    ls::FileDescriptorScope syncScope{{&syncFd, 1}};
    resources.syncSemaphore.emplace(vk, 0, std::nullopt, &syncFd);
    resources.context = ls::owned_ptr<ls::R<backend::Context>>(
        new ls::R<backend::Context>(backendInstance.openContext(
            (sourceScope.release(), std::pair{sourceFds[0], sourceFds[1]}),
            (destinationScope.release(), destinationFds),
            (syncScope.release(), syncFd),
            extent.width, extent.height, pipeline.encoding,
            1.0F / ls::effectiveFlowScale(resourceProfile),
            ls::effectivePerformanceMode(resourceProfile)
        )),
        [backend = &backendInstance](ls::R<backend::Context>& context) {
            backend->closeContext(context);
        }
    );
    this->initializeGpuPacingProfile(backendInstance, resources.context.get(),
        resourceProfile.low_latency);
    backend::makeLeaking();
    return resources;
}

void Swapchain::commitFrameGenerationResources(
        FrameGenerationResources resources,
        const ls::GameConf& resourceProfile) {
    // Old backend work and application-device copies are proven idle by the
    // caller. The context is declared last in this aggregate and therefore
    // closes before its imported images and shared timeline are destroyed.
    // An overlapped deferred batch may still wait on the retiring timeline.
    // No batch is open here (replacement precedes this frame's admission).
    static_cast<void>(this->completeDeferredPresent(UINT64_MAX));
    FrameGenerationResources retiring;
    retiring.sourceImages = std::move(this->sourceImages);
    retiring.destinationImages = std::move(this->destinationImages);
    // D7c: the retiring timeline is destroyed with `retiring`; the stall probe must not read it.
    present_diagnostics::forgetFgTimeline(this->diagnosticsState.contextId);
    retiring.syncSemaphore = std::move(this->syncSemaphore);
    retiring.context = std::move(this->ctx);

    this->sourceImages = std::move(resources.sourceImages);
    this->destinationImages = std::move(resources.destinationImages);
    this->syncSemaphore = std::move(resources.syncSemaphore);
    this->ctx = std::move(resources.context);
    applyFrameGenerationResourceProfile(this->profile, resourceProfile);

    this->frameState.backendTimelineIndex = 1;
    this->frameState.backendFrameIndex = 0;
    // The predictor's batch indices and the previous batch end belong to the replaced context.
    this->resetGpuRelease();
    this->gpuPreviousArrival.reset();
    if (this->pacingProfile) this->pacingProfile->resetBackend();
    for (auto& slot : this->renderSlots) {
        slot.inFlight = false;
        // A pending split read belongs to the retired backend context; drop it rather than read unproven work.
        slot.splitPending = false;
        // Recorded copies name the replaced destination and swapchain images.
        // Release them before the staging image they read.
        for (auto& pass : slot.passes) {
            pass.generatedCopies.clear();
            pass.stagingCopy.reset();
            pass.staged.reset();
            pass.copied.reset();
            pass.staging.reset();
        }
        // The output timestamps belong to the copies released above; a pending read is dropped.
        slot.outputPending = false;
        slot.outputQueries.reset();
    }
    this->frameState.sourceReleaseTimelineValue = 0;
    this->frameState.nextSourceReleaseTimelineValue = 0;
    this->recoveryState.backendPending = false;
    this->recoveryState.generatedImageAdmission.reset();
    this->recoveryState.orderedAcquireRecovery.reset();
    this->fixedRefreshBudget.reset();
    this->realFramePacer.reset();
    this->configuredFixedGeneratedFrames = fixedGeneratedFrameCount(
        this->profile.multiplier, this->destinationImages.size()
    );
    this->diagnosticsState.fixedWindowStarted.reset();
    this->diagnosticsState.fixedRealFrames = 0;
    this->diagnosticsState.fixedGeneratedFrames = 0;
    this->diagnosticsState.fixedSkippedFrames = 0;

    this->recoveryState.historyWarmupRemaining = historyWarmupFrameCount;
    this->ensureHistoryWarmup();
}

bool Swapchain::applyPendingFrameGenerationResources(
        const vk::Vulkan& vk) {
    if (!this->frameGenerationTransition.pendingRequest())
        return true;

    const auto now = DiagnosticsClock::now();
    if (this->frameGenerationTransition.beginPreparation(now)) {
        try {
            this->ensureFrameGenerationExecutionResources(vk);
            this->preparedFrameGenerationResources =
                this->buildFrameGenerationResources(
                    vk, this->colorPipeline,
                    this->frameGenerationTransition.value().profile
                );
            this->frameGenerationTransition.prepared();
            logPrivateFrameGenerationMemory(
                vk, this->diagnosticsState.contextId,
                this->frameGenerationTransition.stateRevision(),
                "private-context-prepared"
            );
            if (present_diagnostics::enabled()) {
                const auto& requested =
                    this->frameGenerationTransition.value().profile;
                std::cerr << "framegen-vulkan: present diagnostics: "
                             "operation=runtime-transition-prepared"
                          << " context=" << this->diagnosticsState.contextId
                          << " state_revision="
                          << this->frameGenerationTransition.stateRevision()
                          << " reason=frame-generation-resources"
                          << " requested_generated_capacity="
                          << generatedFrameCapacityForProfile(requested)
                          << " requested_flow_scale="
                          << ls::effectiveFlowScale(requested)
                          << " requested_lighter_model="
                          << ls::effectivePerformanceMode(requested)
                          << " action=drain-private-work\n";
            }
        } catch (const std::exception& error) {
            this->preparedFrameGenerationResources.reset();
            this->frameGenerationTransition.failed(
                std::chrono::seconds(5), now
            );
            std::cerr << "framegen-vulkan: private frame-generation resource "
                         "transition failed; the previous resources remain "
                         "active and retry is scheduled: "
                      << error.what() << '\n';
            if (present_diagnostics::enabled()) {
                std::cerr << "framegen-vulkan: present diagnostics: "
                             "operation=runtime-transition-failed"
                          << " context=" << this->diagnosticsState.contextId
                          << " state_revision="
                          << this->frameGenerationTransition.stateRevision()
                          << " reason=frame-generation-resources"
                          << " retry_ms=5000"
                          << " active_generated_capacity="
                          << this->destinationImages.size()
                          << " action=retain-active-resources\n";
            }
            return true;
        }
    }

    if (!this->frameGenerationTransition.draining())
        return true;
    if (!this->preparedFrameGenerationResources)
        return true;

    try {
        if (!this->instance || !this->instance->contextReady(this->ctx.get()))
            return false;
        if (!this->pollRenderSlotsIdle(vk))
            return false;
    } catch (const std::exception& error) {
        std::cerr << "framegen-vulkan: private frame-generation drain poll failed; "
                     "native presentation retained: "
                  << error.what() << '\n';
        return false;
    }

    const auto committedProfile =
        this->frameGenerationTransition.value().profile;
    auto replacement = std::move(*this->preparedFrameGenerationResources);
    this->preparedFrameGenerationResources.reset();
    this->commitFrameGenerationResources(
        std::move(replacement), committedProfile
    );
    const auto committedRevision =
        this->frameGenerationTransition.committed();
    logPrivateFrameGenerationMemory(
        vk, this->diagnosticsState.contextId,
        committedRevision.value_or(0), "private-context-applied"
    );

    std::cerr << "framegen-vulkan: frame-generation resources changed live: "
              << "generated_capacity=" << this->destinationImages.size()
              << "; flow_scale=" << ls::effectiveFlowScale(this->profile)
              << "; lighter_model="
              << ls::effectivePerformanceMode(this->profile)
              << "; transition=private-context\n";
    if (present_diagnostics::enabled()) {
        std::cerr << "framegen-vulkan: present diagnostics: "
                     "operation=runtime-transition-applied"
                  << " context=" << this->diagnosticsState.contextId
                  << " state_revision=" << committedRevision.value_or(0)
                  << " reason=frame-generation-resources"
                  << " transition=private-context"
                  << " effective_flow_scale="
                  << ls::effectiveFlowScale(this->profile)
                  << " lighter_model="
                  << ls::effectivePerformanceMode(this->profile)
                  << " generated_frame_capacity="
                  << this->destinationImages.size()
                  << " history_warmup_frames="
                  << historyWarmupFrameCount
                  << '\n';
    }
    return true;
}

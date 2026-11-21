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
#include <cstdlib>
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

Swapchain::GeneratedAcquireFence::GeneratedAcquireFence(const vk::Vulkan& vk)
    : fence(vk), probe{fence.handle(), vk.df().GetFenceStatus, vk.df().ResetFences},
      device(vk.dev()), wait(vk.df().WaitForFences) {}

Swapchain::GeneratedAcquireFence::~GeneratedAcquireFence() {
    // The worker has joined. The acquire fence is ordered after the semaphore
    // used by the copy, so a completed render slot alone does not prove it idle.
    if (probe.used && probe.status(device, probe.fence) == VK_NOT_READY)
        static_cast<void>(wait(device, 1, &probe.fence, VK_TRUE, UINT64_MAX));
}

// The worker resubmits these copies, so they are recorded once per destination
// and swapchain image set. The slot's staging copy runs before the source
// copy that releases the next backend batch.
// The worker therefore copies only staging into each swapchain image.
bool Swapchain::recordGeneratedCopies(const vk::Vulkan& vk, RenderSlot& slot, size_t pass) {
    if (pass >= this->destinationImages.size()) return false;
    RenderPass& renderPass = slot.passes.at(pass);
    if (renderPass.staging && renderPass.generatedCopies.size() == this->info.images.size()) return true;
    if (this->destinationImages.empty() ||
            this->info.images.size() > DeferredPresent::maxCopyImages)
        return false;
    // Diagnostic output split: timestamp pairs around staging and the first worker copy. Created once per slot.
    // A failure leaves the copies untimed, as they are without the split gate.
    if (pass == 0 && present_diagnostics::pacingGpuSplitProfileEnabled() && this->pacingProfile && !slot.outputQueries) {
        try {
            slot.outputQueries = std::make_unique<vk::TimestampQueryPool>(vk, 4);
        } catch (const std::exception& error) {
            present_diagnostics::reportPacingGpuSplitUnavailable("output-queries", error.what());
        } catch (...) {
            present_diagnostics::reportPacingGpuSplitUnavailable("output-queries", "unknown error");
        }
    }
    vk::TimestampQueryPool* const outputTimer = pass == 0 ? slot.outputQueries.get() : nullptr;
    // QH2: the lower images share the device's transfer family, so the staging image does too.
    const TransferCopyQueue* const transfer = this->info.transferConcurrent ? this->info.transferCopy.get() : nullptr;
    try {
        const auto& destinationImage = this->destinationImages.at(pass);
        const VkExtent2D extent = destinationImage.getExtent();
        const std::array<uint32_t, 2> sharedFamilies{vk.queueFamilyIndex(), transfer ? transfer->family : 0U};
        renderPass.staging.emplace(vk, extent, this->info.format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            std::nullopt, std::nullopt, nullptr,
            transfer ? std::span<const uint32_t>{sharedFamilies} : std::span<const uint32_t>{});
        const VkImage stagingImage = renderPass.staging->handle();
        // QH2: the final copies move to the transfer family only when the application never touches lower images
        // (this present uses internal images), and the copy is exact: same format, same extent as the swapchain.
        const bool onTransfer = transfer && this->internalPresent &&
            extent.width == this->info.extent.width && extent.height == this->info.extent.height;
        renderPass.staged.emplace(vk);
        renderPass.copied.emplace(vk);
        renderPass.stagingCopy.emplace(vk);
        {
            const auto& command = *renderPass.stagingCopy;
            command.begin(vk, 0);
            vk::acquireExternalImage(vk, command.handle(), destinationImage.handle(),
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                vk::sharedImageOwnership(this->attachedGeneration(), vk.queueFamilyIndex()));
            // The staged signal orders this reset before the worker copies' timestamps.
            if (outputTimer) {
                outputTimer->reset(command.handle());
                outputTimer->write(command.handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, 2);
            }
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
            if (outputTimer) outputTimer->write(command.handle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 3);
            command.end(vk);
        }

        std::vector<vk::CommandBuffer> copies;
        copies.reserve(this->info.images.size());
        for (const VkImage swapchainImage : this->info.images) {
            if (onTransfer) copies.emplace_back(vk, transfer->pool);
            else copies.emplace_back(vk);
            const auto& command = copies.back();
            command.begin(vk, 0);
            // QH2: a transfer family without timestamp bits leaves its copies untimed; they read as unavailable.
            vk::TimestampQueryPool* const copyTimer = onTransfer && !transfer->timestamps ? nullptr : outputTimer;
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
            if (copyTimer) copyTimer->write(command.handle(),
                onTransfer ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
            if (onTransfer) {
                command.copyImage(vk, preBarriers,
                    {stagingImage, swapchainImage},
                    extent, postBarriers);
            } else {
                command.blitImage(vk, preBarriers,
                    {stagingImage, swapchainImage},
                    extent, postBarriers);
            }
            if (copyTimer) copyTimer->write(command.handle(),
                onTransfer ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);
            command.end(vk);
        }
        renderPass.generatedCopies = std::move(copies);
        renderPass.generatedOnTransfer = onTransfer;
        static const bool acquireProbe = [] {
            const auto* value = std::getenv("FGVK_PACING_PROFILE_ACQUIRE_FENCE");
            return value && std::string_view(value) == "1";
        }();
        if (acquireProbe && outputTimer && this->info.surfaceOrigin == SurfaceOrigin::Wayland &&
                !renderPass.acquireFence) {
            try {
                renderPass.acquireFence = std::make_unique<GeneratedAcquireFence>(vk);
            } catch (const std::exception& error) {
                present_diagnostics::reportPacingGpuSplitUnavailable("generated-acquire-fence", error.what());
            }
        }
        return true;
    } catch (...) {
        renderPass.generatedCopies.clear();
        renderPass.generatedOnTransfer = false;
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
    // GP1 per-step timestamps: diagnostics only, and only on top of the split profile enabled above.
    if (diagnostics && present_diagnostics::pacingGpuPassProfileEnabled()) {
        try {
            backend.enablePassStepProfile(context);
        } catch (const std::exception& error) {
            std::cerr << "framegen-vulkan: GPU pass-step profile unavailable; reason=query-initialization; "
                      << error.what() << '\n';
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
    if (backendInstance.attached()) {
        // FI1: images and timeline on the application's device, no export. The attached context needs no makeLeaking.
        resources.sourceImages.reserve(2);
        for (size_t i = 0; i < 2; ++i) {
            resources.sourceImages.emplace_back(vk,
                extent, pipeline.exchangeFormat,
                backend::sourceExchangeImageUsage);
        }
        const size_t destinationCount = generatedFrameCapacityForProfile(resourceProfile);
        resources.destinationImages.reserve(destinationCount);
        for (size_t i = 0; i < destinationCount; ++i) {
            resources.destinationImages.emplace_back(vk,
                extent, pipeline.exchangeFormat,
                backend::destinationExchangeImageUsage);
        }
        resources.syncSemaphore.emplace(vk, 0);
        std::vector<VkImage> destinations;
        destinations.reserve(destinationCount);
        for (const auto& image : resources.destinationImages)
            destinations.push_back(image.handle());
        resources.context = ls::owned_ptr<ls::R<backend::Context>>(
            new ls::R<backend::Context>(backendInstance.openContext(
                std::pair{resources.sourceImages[0].handle(), resources.sourceImages[1].handle()},
                destinations, resources.syncSemaphore->handle(),
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
        return resources;
    }
    resources.sourceImages.reserve(sourceFds.size());
    resources.destinationImages.reserve(destinationFds.size());
    for (int& fd : sourceFds) {
        resources.sourceImages.emplace_back(vk,
            extent, pipeline.exchangeFormat,
            backend::sourceExchangeImageUsage,
            std::nullopt, &fd);
    }
    for (int& fd : destinationFds) {
        resources.destinationImages.emplace_back(vk,
            extent, pipeline.exchangeFormat,
            backend::destinationExchangeImageUsage,
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
    this->splitAwaitingRender.clear();
    if (this->pacingProfile) this->pacingProfile->resetBackend();
    for (auto& slot : this->renderSlots) {
        slot.inFlight = false;
        // A pending split read belongs to the retired backend context; drop it rather than read unproven work.
        slot.splitPending = false;
        // Recorded copies name the replaced destination and swapchain images.
        // Release them before the staging image they read.
        for (auto& pass : slot.passes) {
            pass.generatedCopies.clear();
            pass.generatedOnTransfer = false;
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
    this->frameState.privateStagingValue = 0;
    this->recoveryState.backendPending = false;
    this->recoveryState.generatedImageAdmission.reset();
    this->recoveryState.orderedAcquireRecovery.reset();
    this->fixedRefreshBudget.reset();
    this->realFramePacer.reset();
    this->configuredFixedGeneratedFrames = fixedGeneratedFrameCount(
        this->profile.multiplier, this->destinationImages.size()
    );

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
        } catch (const std::exception& error) {
            this->preparedFrameGenerationResources.reset();
            this->frameGenerationTransition.failed(
                std::chrono::seconds(5), now
            );
            std::cerr << "framegen-vulkan: private frame-generation resource "
                         "transition failed; the previous resources remain "
                         "active and retry is scheduled: "
                      << error.what() << '\n';
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
    // Commits the transition: it cancels the pending request, so the call stays.
    static_cast<void>(this->frameGenerationTransition.committed());

    std::cerr << "framegen-vulkan: frame-generation resources changed live: "
              << "generated_capacity=" << this->destinationImages.size()
              << "; flow_scale=" << ls::effectiveFlowScale(this->profile)
              << "; lighter_model="
              << ls::effectivePerformanceMode(this->profile)
              << "; transition=private-context\n";
    return true;
}

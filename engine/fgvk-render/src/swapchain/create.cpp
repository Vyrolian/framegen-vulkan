/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain/swapchain.hpp"
#include "swapchain/create_policy.hpp"
#include "swapchain/retirement.hpp"
#include "layer_role.hpp"
#include "present_diagnostics.hpp"
#include "wayland_presentation_feedback.hpp"
#include "x11_refresh.hpp"
#include "fgvk-common/helpers/errors.hpp"
#include "fgvk-common/helpers/file_descriptors.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

using namespace fgvk;
using namespace fgvk::layer;

namespace {
    using DiagnosticsClock = present_diagnostics::Clock;
    using DiagnosticsContextScope = present_diagnostics::ContextScope;
    uint64_t allocateDiagnosticsContextId() {
        return present_diagnostics::allocateContextId();
    }

    std::optional<uint64_t> configuredGeneratedImageAcquireTimeoutNs() {
        static const std::optional<uint64_t> timeout = []() -> std::optional<uint64_t> {
            const char* value = std::getenv("FGVK_PRESENT_ACQUIRE_TIMEOUT_MS");
            if (!value)
                return std::nullopt;

            char* end{};
            const double parsed = std::strtod(value, &end);
            if (end == value || *end != '\0' || parsed <= 0.0)
                return std::nullopt;

            constexpr uint64_t nanosecondsPerMillisecond = 1'000'000;
            const double maximumMilliseconds = static_cast<double>(
                (std::numeric_limits<uint64_t>::max() - 1) / nanosecondsPerMillisecond
            );
            const double clampedMilliseconds = std::min(parsed, maximumMilliseconds);
            return static_cast<uint64_t>(
                clampedMilliseconds * static_cast<double>(nanosecondsPerMillisecond)
            );
        }();
        return timeout;
    }
}

std::optional<uint64_t> Swapchain::generatedImageAcquireTimeoutNs() {
    return configuredGeneratedImageAcquireTimeoutNs();
}

bool layer::context_ModifySwapchainCreateInfo(const ls::GameConf& profile,
        uint32_t maxImages,
        VkSwapchainCreateInfoKHR& createInfo,
        const bool frameGenerationInteropEnabled,
        const bool winePassThroughHdr,
        const uint32_t surfaceMinImages) {
    const auto colorPipeline = classifySwapchainColor(
        createInfo.imageFormat, createInfo.imageColorSpace, winePassThroughHdr
    );
    const bool frameGenerationProvisioned =
        frameGenerationInteropEnabled && colorPipeline.generationSupported;
    // P1: the application's per-present mode reaches the lower driver. A create
    // chain whose present-mode group can switch to an unthrottled mode (VKD3D-Proton
    // creates FIFO with {FIFO, IMMEDIATE} and sends IMMEDIATE per present with VSync
    // off) keeps its group and per-present nodes instead of the ordered FIFO transport.
    const auto group = presentModeGroup(createInfo.pNext);
    const bool orderedSdrCandidate =
        frameGenerationProvisioned &&
        !directUnthrottledPresentation(createInfo.presentMode);
    const bool orderedFrameGenerationTransport = orderedSdrCandidate && !group.unthrottled;

    return applySwapchainCreateProvisioning(
        profile, maxImages, createInfo,
        frameGenerationProvisioned,
        orderedFrameGenerationTransport,
        profile.swapchain_image_count_compatibility,
        surfaceMinImages,
        orderedSdrCandidate && group.unthrottled && group.fifo
    );
}

Swapchain::Swapchain(const vk::Vulkan& vk, backend::Instance* backend,
            ls::GameConf profile, SwapchainInfo info,
            const bool fp16Requested,
            const bool swapchainMaintenance1Enabled) :
        instance(backend),
        privateOrderedTransport(info.privateOrderedTransport),
        currentPresentMode(info.presentMode),
        colorPipeline(classifySwapchainColor(info.format, info.colorSpace, info.winePassThroughHdr)),
        fp16Requested(fp16Requested),
        profile(std::move(profile)), info(std::move(info)) {
    this->presentFeedback.configure(this->info.applicationPresentFeedback);
    this->diagnosticsState.contextId = allocateDiagnosticsContextId();
    if (present_diagnostics::pacingProfileEnabled()) {
        present_diagnostics::initializePacingProfileLog();
        try {
            this->pacingProfile = std::make_unique<present_diagnostics::PacingProfile>();
        } catch (...) {} // Optional observations cannot prevent swapchain creation.
    }
    const DiagnosticsContextScope diagnosticsContext(
        this->diagnosticsState.contextId
    );
    const VkExtent2D extent = this->info.extent;

    if (swapchainMaintenance1Enabled && presentationRetirementRequired(
            vk.frameGenerationInteropEnabled())) {
        try {
            this->presentRetirementFences.reserve(this->info.images.size());
            for (size_t index = 0; index < this->info.images.size(); ++index) {
                static_cast<void>(index);
                this->presentRetirementFences.emplace_back(
                    PresentRetirementFence{.fence = vk::Fence(vk)}
                );
            }
            std::cerr << "framegen-vulkan: swapchain presentation retirement: "
                         "mode=maintenance1-per-image-fence; images="
                      << this->presentRetirementFences.size() << '\n';
        } catch (const std::exception& error) {
            this->presentRetirementFences.clear();
            std::cerr << "framegen-vulkan: swapchain presentation retirement "
                         "unavailable; live resource recreation will wait for "
                         "a natural swapchain boundary: "
                      << error.what() << '\n';
        }
    }

    // X5: an X11 surface has no present timing of its own on most drivers, so its monitor's refresh
    // is queried once per swapchain on a detached thread. The creating thread never waits for it, and
    // the present path reads the result when it arrives.
    if (this->info.surfaceOrigin == SurfaceOrigin::Xcb ||
            this->info.surfaceOrigin == SurfaceOrigin::Xlib) {
        this->x11Refresh = startX11WindowRefreshQuery(this->info.x11Window);
    }

    const bool initialFrameGenerationEnabled = effectiveFrameGenerationEnabled(
        this->profile
    );

    if (!backend && this->colorPipeline.generationSupported) {
        this->colorPipeline.generationSupported = false;
        this->colorPipeline.reason =
            "the frame-generation backend is unavailable";
    }

    std::cerr << "framegen-vulkan: swapchain colour pipeline: format="
              << static_cast<int>(this->info.format)
              << "; color-space=" << static_cast<int>(this->info.colorSpace)
              << "; mode=" << this->colorPipeline.name
              << "; transport="
              << (transportBytesPerPixel(this->colorPipeline.encoding) == 8
                    ? "rgba16f-64-bit" : "rgba8-32-bit")
              << "; frame-generation="
              << (this->colorPipeline.generationSupported ? "supported" : "passthrough")
              << '\n';
    if (!this->colorPipeline.generationSupported) {
        std::cerr << "framegen-vulkan: frame generation disabled for this swapchain: "
                  << this->colorPipeline.reason << '\n';
        return;
    }
    try {
        const VkExtent2D generationExtent = this->info.extent;
        std::vector<int> sourceFds(2, -1);
        ls::FileDescriptorScope sourceScope{sourceFds};
        std::vector<int> destinationFds(generatedFrameCapacityForProfile(this->profile), -1);
        ls::FileDescriptorScope destinationScope{destinationFds};
        this->sourceImages.reserve(sourceFds.size());
        for (int& fd : sourceFds)
            this->sourceImages.emplace_back(vk,
                generationExtent, this->colorPipeline.exchangeFormat,
                backend::sourceExchangeImageUsage,
                std::nullopt, &fd);

        this->destinationImages.reserve(destinationFds.size());
        for (int& fd : destinationFds)
            this->destinationImages.emplace_back(vk,
                generationExtent, this->colorPipeline.exchangeFormat,
                backend::destinationExchangeImageUsage,
                std::nullopt, &fd);

        int syncFd{-1};
        ls::FileDescriptorScope syncScope{{&syncFd, 1}};
        this->syncSemaphore.emplace(vk, 0, std::nullopt, &syncFd);

        try {
            this->ctx = ls::owned_ptr<ls::R<backend::Context>>(
                new ls::R<backend::Context>(backend->openContext(
                    (sourceScope.release(), std::pair{sourceFds[0], sourceFds[1]}),
                    (destinationScope.release(), destinationFds),
                    (syncScope.release(), syncFd),
                    generationExtent.width, generationExtent.height,
                    this->colorPipeline.encoding,
                    1.0F / ls::effectiveFlowScale(this->profile),
                    ls::effectivePerformanceMode(this->profile)
                )),
                [backend](ls::R<backend::Context>& ctx) {
                    backend->closeContext(ctx);
                }
            );

            this->initializeGpuPacingProfile(*backend, this->ctx.get(), this->profile.low_latency);

            // The backend's private VkDevice must outlive this layer on loader
            // combinations where destroying it during layer unload is unsafe.
            // This is process-lifetime retention, not active GPU work; see the
            // backend API contract before changing the teardown policy.
            backend::makeLeaking();
        } catch (const std::exception& e) {
            throw ls::error("failed to create swapchain context", e);
        }

        this->ensureFrameGenerationExecutionResources(vk);

        this->configuredFixedGeneratedFrames = fixedGeneratedFrameCount(
            this->profile.multiplier, this->destinationImages.size()
        );

        if (!initialFrameGenerationEnabled)
            std::cerr << "framegen-vulkan: frame generation is off; retained private "
                         "resources permit a live enable\n";

        const auto configuredAcquireTimeout =
            generatedImageAcquireTimeoutNs();
        if (configuredAcquireTimeout) {
            std::cerr << "framegen-vulkan: generated-image acquire timeout requested at "
                      << static_cast<double>(*configuredAcquireTimeout) /
                            1'000'000.0
                      << " ms; timeout results and successful wall-time "
                         "deadline overruns enter transport recovery\n";
        }
        const bool frameGenerationActive = effectiveFrameGenerationEnabled(
            this->profile
        ) && this->colorPipeline.generationSupported;
        this->recoveryState.replacementBackendStabilization.begin(
            this->info.replacement, frameGenerationActive,
            DiagnosticsClock::now()
        );
    } catch (const std::exception& e) {
        // Swapchain creation belongs to the game. A failure in framegen-vulkan's optional
        // interpolation resources must not turn a valid game swapchain into a
        // startup failure, especially when a driver exposes an HDR format that
        // cannot be initialized on this device. Keep the native swapchain and
        // present its real frames until the game recreates it.
        this->ctx = {};
        this->sourceImages.clear();
        this->destinationImages.clear();
        this->renderSlots.clear();
        this->renderSlotIndex = 0;
        this->postCopySemaphores.clear();
        this->colorPipeline.generationSupported = false;
        this->colorPipeline.name = "initialization-fallback";
        this->colorPipeline.reason =
            "frame-generation initialization failed; native presentation retained";
        std::cerr << "framegen-vulkan: " << this->colorPipeline.reason
                  << ": " << e.what() << '\n';
    }
}

#if defined(VK_KHR_present_wait2) && defined(VK_KHR_present_id2)
// GP1: the on-screen profile of this lower swapchain. The watcher waits each lower ID through the next
// layer's vkWaitForPresent2KHR and emits its completion through the pacing recorder.
void Swapchain::startOnScreenProfile(const VkDevice device, const VkSwapchainKHR swapchain,
        const PFN_vkWaitForPresent2KHR waitForPresent2) {
    const auto wait = [waitForPresent2, device, swapchain](const uint64_t lowerId, const uint64_t timeoutNs) {
        const VkPresentWait2InfoKHR info{
            .sType = VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR,
            .presentId = lowerId, .timeout = timeoutNs,
        };
        return waitForPresent2(device, swapchain, &info);
    };
    const auto emit = [](const OnScreenRecord& record) {
        // Diagnostics only: without the pacing profile the watcher still feeds the
        // correction, and no record is formatted.
        if (!present_diagnostics::pacingProfileEnabled()) return;
        present_diagnostics::writePacingProfileRecords(
            formatOnScreenRecord(record, static_cast<long>(getpid())));
    };
    // OS1: each completed wait is a screen sample for the ring that the worker reads.
    const auto observe = [ring = this->screenRing()](const ScreenSample& sample) {
        ring->push(sample);
    };
    this->onScreen = std::make_unique<OnScreenProfile>(handleBits(device), handleBits(swapchain),
        this->diagnosticsState.contextId, wait, emit, observe);
}

void Swapchain::stopOnScreenProfile() noexcept {
    if (this->onScreen) this->onScreen->stop();
}

void Swapchain::attachDeferredOnScreen() {
    if (this->onScreen && this->deferredPresent)
        this->deferredPresent->setOnScreen(&this->onScreen->lowerIds(), &this->onScreen->watch());
}
#endif

void Swapchain::configureScreenCorrection(const bool watcherActive, const char* gateReason,
        const bool gamescopeSession) {
    const auto plan = screenCorrectionSource(realOffsetMayEnable(), onScreenCorrectionEnvironment(),
        watcherActive, gateReason, static_cast<bool>(this->info.waylandPresentation));
    this->screenSource = plan.source;
    std::clog << "framegen-vulkan: on-screen correction: source=" << screenSourceName(plan.source)
              << " reason=" << plan.reason << " enabled=" << realOffsetAuto() << '\n';
    // GT1: the tick lock needs the watcher's samples on a gamescope surface and is logged for gamescope only.
    const auto tick = tickLockPlan(gamescopeTickLockEnvironment(), gamescopeSession,
        plan.source == ScreenSource::Watcher, realOffsetMayEnable());
    this->screenTickLock = tick.enabled;
    if (gamescopeSession) {
        std::clog << "framegen-vulkan: gamescope tick lock: state=" << (tick.enabled && realOffsetAuto() ? "on" : "off");
        if (!tick.enabled) std::clog << " reason=" << tick.reason;
        else if (!realOffsetAuto()) std::clog << " reason=correction-off";
        std::clog << '\n';
    }
}

std::shared_ptr<ScreenSampleRing> Swapchain::screenRing() {
    if (!this->screenSamples) this->screenSamples = std::make_shared<ScreenSampleRing>();
    return this->screenSamples;
}

void Swapchain::attachScreenSamples() {
    if (this->screenSource == ScreenSource::None || !this->deferredPresent) return;
    const auto ring = this->screenRing();
    this->deferredPresent->setScreenSamples(ring);
    this->deferredPresent->setTickLock(this->screenTickLock);
    if (this->screenSource == ScreenSource::Wayland && this->info.waylandPresentation)
        this->info.waylandPresentation->setScreenSink(ring);
}

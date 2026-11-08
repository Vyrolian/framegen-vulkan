/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "fgvk-backend/fgvk.hpp"
#include "fgvk-common/configuration/config.hpp"
#include "fgvk-common/helpers/pointers.hpp"
#include "fgvk-common/vulkan/command_buffer.hpp"
#include "fgvk-common/vulkan/fence.hpp"
#include "fgvk-common/vulkan/gpu_clock.hpp"
#include "fgvk-common/vulkan/image.hpp"
#include "fgvk-common/vulkan/semaphore.hpp"
#include "fgvk-common/vulkan/timeline_semaphore.hpp"
#include "fgvk-common/vulkan/timestamp_query_pool.hpp"
#include "fgvk-common/vulkan/vulkan.hpp"
#include "adaptive_scheduler.hpp"
#include "color_pipeline.hpp"
#include "generated_frame_delivery.hpp"
#include "profile_update.hpp"
#include "presentation_policy.hpp"
#include "present_diagnostics.hpp"
#include "native_copy_profile.hpp"
#include "runtime_status.hpp"
#include "runtime_health.hpp"
#include "runtime_transition.hpp"
#include "spatial_scaler.hpp"
#include "spatial_scaling_policy.hpp"
#include "spatial_command_recording.hpp"
#include "present_timing.hpp"
#include "swapchain/deferred_present.hpp"
#include "swapchain/internal_images.hpp"
#include "swapchain/present_feedback_probe.hpp"
#include "swapchain/release_gate.hpp"
#include "swapchain/uncapped_auto_cap.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

    class GamescopeScalingSurface;
    class WaylandPresentationFeedback;
    class FrameContentCapture;
    struct InternalPresentImage;

    /// swapchain info struct
    struct SwapchainInfo {
        std::vector<VkImage> images;
        VkSurfaceKHR surface;
        SpatialSurfaceOrigin surfaceOrigin{SpatialSurfaceOrigin::Unknown};
        std::shared_ptr<WaylandPresentationFeedback> waylandPresentation;
        std::shared_ptr<DeferredPresentQueue> deferredPresentQueue;
        // Borrowed from the instance owner, whose surface and connection
        // outlive Vulkan swapchain retirement. Null for ordinary/WSI surfaces.
        GamescopeScalingSurface* gamescopeScalingSurface{nullptr};
        // Enabled only when the lower chain advertises display timing and the
        // application has not selected its own timing-feedback namespace.
        bool gamescopeDisplayTiming{false};
        bool ownedPresentTiming{false};
        bool presentMetering{false};
        // Observational device capability state, not a CPU-spacing exclusion.
        // Actual timing nodes own output deadlines; IDs/waits remain on real.
        bool applicationPresentTiming{false};
        // Application timing plus the create-time timing bit, so the driver
        // answers vkGetSwapchainTimingPropertiesEXT for this swapchain.
        bool applicationSwapchainTiming{false};
        // Application enabled presentId2+presentWait2 and created this swapchain with both bits.
        bool applicationPresentFeedback{false};
        bool copyTimestampCalibration{false};
        uint32_t ownedPresentTimingTargetStage{};
        VkFormat format;
        VkColorSpaceKHR colorSpace;
        // Preserve the application's requested minimum separately from the
        // bounded minimum forwarded by framegen-vulkan and the actual returned count in
        // `images`. These values expose the selected launch policy and support
        // generated-capacity admission without changing presentation behavior.
        uint32_t requestedMinImageCount{0};
        uint32_t provisionedMinImageCount{0};
        bool swapchainImageCountCompatibility{false};
        // Preserve whether the application supplied the Vulkan replacement
        // lineage explicitly so retirement and replacement stabilization keep
        // the game's ownership boundary.
        bool applicationOldSwapchainProvided{false};
        // Extent the application requested after framegen-vulkan virtualized fixed WSI
        // capabilities. The actual swapchain images retain `extent`.
        VkExtent2D applicationExtent;
        VkExtent2D extent;
        // Proven Gamescope output geometry used to cap ordinary scaling and
        // to expose the live non-supersampling slider ceiling.
        std::optional<VkExtent2D> gamescopePresentationTarget;
        // Preserve the incoming application's or upper layer's request before
        // framegen-vulkan selects its immutable lower/compositor transport contract.
        VkPresentModeKHR incomingPresentMode{VK_PRESENT_MODE_FIFO_KHR};
        VkPresentModeKHR presentMode;
        // Persist the exact create-time transport decision. HDR feedback can
        // change later, but Vulkan present mode/pNext compatibility cannot be
        // reinterpreted without replacing the game-owned swapchain.
        bool privateOrderedTransport{false};
        bool spatialScalingActive{false};
        // Requested internal native output resize, including a memory-rejected
        // candidate eligible for retry; the saved Scaling toggle stays off.
        bool hdrDisplayResize{false};
        // Variable WSI surfaces retain the source extent and select a larger
        // lower presentation envelope. Factor edits which remain above an
        // already active ceiling can therefore be applied without replacing
        // the game-owned swapchain.
        bool variableSurface{false};
        // True when scaling remains active at a lower effective factor because
        // the requested presentation extent exceeded the create-time memory
        // admission envelope.
        bool spatialScalingMemoryConstrained{false};
        // A replacement resolution change was rejected only by live memory
        // admission while remaining inside the previous scaled envelope.
        // One delayed, surface-budgeted retry may re-sample the driver.
        bool spatialScalingAdmissionRetryEligible{false};
        uint64_t spatialScalingPolicyRevision{0};
        // Exact create-time reason for retaining native geometry. Keep this
        // independent from activation support: memory and output-headroom
        // guards are actionable on an otherwise supported surface.
        SpatialScalingInactiveReason spatialScalingInactiveReason{
            SpatialScalingInactiveReason::None
        };
        // False only when the lower split role proved that Gamescope WSI did
        // not own this surface. No live factor can make that surface safe, so
        // scaler choices remain dormant until a natural surface boundary.
        bool spatialScalingActivationSupported{true};
        // The upper WSI either supplied a live oldSwapchain handle or framegen-vulkan
        // completed retirement of the exact retained lower handle before a
        // null-old destroy-before-create replacement. This is a known
        // owner-driven replacement rather than a cold process start, so
        // Adaptive may use its bounded recovery guard.
        bool replacement{false};
    };

    /// modify the swapchain create info based on the profile pre-swapchain creation
    /// @param profile active game profile
    /// @param maxImages maximum number of images supported by the surface
    /// @param createInfo swapchain create info to modify
    [[nodiscard]] bool context_ModifySwapchainCreateInfo(
        const ls::GameConf& profile, uint32_t maxImages,
        VkSwapchainCreateInfoKHR& createInfo, bool gamescopeHdrActive,
        bool gamescopeDetected,
        const PresentationEnvironmentPolicy& presentationEnvironment,
        bool frameGenerationInteropEnabled,
        bool spatialScalingActive);

    /// swapchain context for a layer instance
    class Swapchain {
    public:
        /// create a new swapchain context
        /// @param vk vulkan instance
        /// @param backend optional framegen-vulkan frame-generation backend; scaling is
        /// independent and remains available when this is null
        /// @param profile active game profile
        /// @param info swapchain info
        Swapchain(const vk::Vulkan& vk, backend::Instance* backend,
            ls::GameConf profile, SwapchainInfo info,
            std::optional<std::filesystem::path> scalingShaderDll,
            bool fp16Requested,
            std::optional<bool> gamescopeHdrActive,
            bool gamescopeDetected, bool hdrExposureDisabled,
            std::optional<uint32_t> gamescopeRefreshHz,
            GamescopePresentationFeedback gamescopePresentationFeedback,
            uint64_t runtimeStateRevision,
            bool swapchainMaintenance1Enabled);

        [[nodiscard]] bool needsGpuLoadMonitor() const {
            return this->profile.gpu_headroom_percent > 0 &&
                this->profile.frame_generation_enabled && this->instance &&
                !this->gpuLoadMonitorInitialized;
        }
        void setGpuLoadMonitor(std::shared_ptr<present_diagnostics::GpuLoadMonitor> monitor) {
            this->gpuLoadMonitor = std::move(monitor);
            // An unavailable device or failed setup must not retry every frame.
            this->gpuLoadMonitorInitialized = true;
        }

        /// present a frame
        /// @param vk vulkan instance
        /// @param queue presentation queue
        /// @param next_chain next chain pointer for the present info (WARN: shared!)
        /// @param imageIdx swapchain image index to present to
        /// @param semaphores semaphores to wait on before presenting
        /// @param internal application image descriptor when imageIdx is an F5
        ///        private image; nullptr keeps the lower-image contract.
        /// @throws ls::vulkan_error on vulkan errors
        VkResult present(const vk::Vulkan& vk,
            VkQueue queue, VkSwapchainKHR swapchain,
            void* next_chain, uint32_t imageIdx,
            std::span<const VkSemaphore> semaphores,
            InternalPresentImage* internal = nullptr);
        // A spare image can be acquired before the deferred deadline. Join
        // before blocking acquisition or reusing presentation resources.
        // retainNewest waits only for an older overlapped batch, not the newest one.
        VkResult completeDeferredPresent(uint64_t timeoutNs, bool acquisition = false,
            bool retainNewest = false);
        // Join retained output, but keep one sealed overlapped batch spacing.
        VkResult completeDeferredPresentRetainingOverlap() {
            return this->completeDeferredPresent(UINT64_MAX, false,
                this->deferredPresent && this->deferredPresent->retainable());
        }
        void observeDeferredApplicationAcquire() {
            if (deferredPresent) deferredPresent->applicationAcquire();
        }
        [[nodiscard]] bool beginDeferredPresentBatch(const vk::Vulkan& vk, size_t limit,
            uint64_t readyValue, std::chrono::steady_clock::time_point readyDeadline, bool capOwned,
            std::optional<std::chrono::steady_clock::time_point> releasedOutputLimit = std::nullopt,
            std::chrono::steady_clock::duration releasedJoinTolerance = {},
            bool overlap = false, std::chrono::steady_clock::duration outputInterval = {},
            std::chrono::steady_clock::time_point arrival = {});
        [[nodiscard]] bool hasDeferredPresent() const { return static_cast<bool>(this->deferredPresent); }
        // True while an admitted batch is still outputting or waiting to be joined.
        [[nodiscard]] bool deferredPresentPending() const {
            return this->deferredPresent && this->deferredPresent->pending();
        }
        template<typename Acquire>
        VkResult pollDeferredAcquire(Acquire&& acquire) {
            return this->deferredPresent->pollAcquire(std::forward<Acquire>(acquire));
        }
        [[nodiscard]] bool deferredOverlapPending() const {
            return this->deferredPresent && this->deferredPresent->retainable();
        }
        template<typename Acquire>
        VkResult waitDeferredAcquire(uint64_t timeout, Acquire&& acquire) {
            const auto started = DeferredPresent::Clock::now();
            const auto result = this->deferredPresent->waitAcquire(timeout, std::forward<Acquire>(acquire));
            this->realFramePacer.recordAcquireWait(DeferredPresent::Clock::now() - started);
            return result;
        }
        /// stable identifier used to correlate this context's diagnostics
        [[nodiscard]] uint64_t diagnosticsId() const {
            return this->diagnosticsState.contextId;
        }
        [[nodiscard]] bool spatialScalingActive() const {
            return this->spatialScaler.has_value();
        }
        [[nodiscard]] VkExtent2D frameGenerationResourceExtent() const {
            return frameGenerationExtent(
                this->spatialFramePipelinePlacement,
                this->info.applicationExtent, this->info.extent
            );
        }
        [[nodiscard]] SpatialFramePipelinePlacement
        framePipelinePlacement() const {
            return this->spatialFramePipelinePlacement;
        }
        /// Profile currently applied to this context, read by the F5 internal-image gate.
        [[nodiscard]] const ls::GameConf& appliedProfile() const {
            return this->profile;
        }
        /// Gamescope detection captured when this context was created.
        [[nodiscard]] bool gamescopeDetectedAtCreation() const {
            return this->gamescopeDetected;
        }

        /// Apply configuration that is safe for an already-created context.
        /// Spatial model and sharpness changes rebuild framegen-vulkan's private scaler
        /// at the next presentation boundary. The lower managed spatial role
        /// may request one game-owned recreation for an extent change after a
        /// retirement-fenced present; unsupported resource-shape changes wait
        /// for a natural recreation. Process-wide backend changes wait for
        /// restart. The decision reports every applied and pending boundary.
        [[nodiscard]] ProfileUpdateDecision updateProfile(
            const ls::GameConf& profile, uint64_t runtimeStateRevision,
            const ls::GameConf* requestedProfile = nullptr,
            bool processRestartPending = false);

        /// After the lower WSI has consumed this present's wait semaphores,
        /// convert one successful result into VK_ERROR_OUT_OF_DATE_KHR for a
        /// pending live profile-resource change. The application remains the
        /// owner of destruction and recreation.
        [[nodiscard]] bool requestLiveProfileResourceRecreationAfterPresent(
            VkResult lowerPresentResult);

        /// Re-sample a transient live-memory rejection once after a bounded
        /// settling period. The entrypoint owns the per-surface episode budget.
        [[nodiscard]] bool requestSpatialScalingAdmissionRetryAfterPresent(
            VkResult lowerPresentResult, bool surfaceRequestAvailable);

        /// Escalate persistent generated-image starvation after a confirmed
        /// menu return. This is shared by Fixed and Adaptive policies.
        [[nodiscard]] bool requestOrderedAcquireRecreationAfterPresent(
            VkResult lowerPresentResult, bool surfaceRequestAvailable);

        /// Wait for every layer-owned maintenance1 present fence associated
        /// with this swapchain. A zero timeout is a nonblocking retirement
        /// poll that also requires idle backend and application-device work
        /// before context destruction; finite waits are used only at
        /// application destruction.
        [[nodiscard]] bool waitForPresentRetirement(
            const vk::Vulkan& vk, uint64_t timeoutNs);
        [[nodiscard]] bool presentRetirementEnabled() const {
            return !this->presentRetirementFences.empty();
        }
        /// Record a confirmed Gamescope application-HDR state change. Existing
        /// contexts retain their safe encoding until the game naturally
        /// recreates them.
        [[nodiscard]] bool updateGamescopeHdrState(
            bool active, uint64_t runtimeStateRevision);

        /// Update the compositor scanout budget without rebuilding resources.
        void updateGamescopeRefreshRate(std::optional<uint32_t> refreshHz);
        void updateGamescopePresentationFeedback(
            const GamescopePresentationFeedback& feedback);
        void updateGamescopeFocus(const GamescopeFocusFeedback& focus) {
            if (!this->gamescopeFocusInitialized) {
                this->lastFocusReturnSequence = focus.returnSequence;
                this->gamescopeFocusInitialized = true;
            }
            this->gamescopeFocus = focus;
        }

        /// Stop generation in place when the active profile disappears.
        void disableFrameGeneration();
    private:
        void applyGamescopeFocus(std::chrono::steady_clock::time_point now);
        GamescopeFocusFeedback gamescopeFocus;
        GenerationPolicyResetGate generationPolicyResetGate;
        bool steamMenuSuspended{false};
        bool gamescopeFocusInitialized{false};
        uint64_t lastFocusReturnSequence{0};
        std::optional<bool> lastReportedGameFocus;
        bool focusReported{false};
        // Shared construction/replacement support; no independent resource owner.
        [[nodiscard]] static bool directSpatialFrameGenerationOutputSupported(
            const vk::Vulkan& vk, VkFormat format,
            SpatialFramePipelinePlacement placement, bool spatialScalingActive);
        [[nodiscard]] static SwapchainColorPipeline initialColorPipeline(
            VkFormat format, VkColorSpaceKHR colorSpace,
            std::optional<bool> gamescopeHdrActive, bool gamescopeDetected,
            bool hdrExposureDisabled);
        static void selectPackedHdr10Transport(const vk::Vulkan& vk,
            backend::Instance& backendInstance, SwapchainColorPipeline& pipeline,
            bool& applicationSupported, bool& backendSupported);

        struct PresentInvocation {
            const vk::Vulkan& vk;
            VkQueue queue;
            VkSwapchainKHR swapchain;
            const void* nextChain;
            uint32_t imageIndex;
            std::span<const VkSemaphore> waitSemaphores;
            std::chrono::steady_clock::time_point cadenceStarted;
            mutable std::chrono::steady_clock::time_point started;
            // The limiter's arrival for this source. Diagnostics only: a worker batch
            // records it on its first output when the pacing profile is enabled.
            std::chrono::steady_clock::time_point limiterArrival{};
            // Fractional may spend the existing cap wait preparing generated
            // outputs. Every real-frame/fallback path retains this deadline.
            std::optional<std::chrono::steady_clock::time_point>
                originalPresentDeadline;
            std::optional<RealFramePacer::OutputBatch> pacedOutputs;
            bool readyOutputPacing{false};
            bool capReadyOutputPacing{false};
            bool deferredOutputEligible{false};
            std::chrono::steady_clock::time_point readinessDeadline{};
            // Release-gated worker batches: latest output around the next
            // expected source present, and how long its join may keep spacing.
            std::optional<std::chrono::steady_clock::time_point> releasedOutputLimit;
            std::chrono::steady_clock::duration releasedJoinTolerance{};
            // The released batch may keep spacing while one newer source batch is admitted.
            bool releasedOverlap{false};
            mutable std::chrono::steady_clock::duration outputWait{};
            std::chrono::steady_clock::duration readinessWait{};
            present_diagnostics::PacingProfile* profiler{};
            mutable std::optional<std::chrono::steady_clock::time_point> previousOutput;

            void waitForOutput(std::chrono::steady_clock::time_point deadline) const;
        };

        struct PresentationFramePlan {
            // Policy request before WSI admission. Fully admitted timestamps
            // remain authoritative through backend submission.
            GeneratedFramePlan requestedGeneratedFrames;
            // Executable plan after transport-specific admission. Gamescope HDR
            // and headroom-tight ordered SDR may contain fewer, deliberately
            // re-spaced timestamps.
            GeneratedFramePlan scheduledGeneratedFrames;
            size_t admittedGeneratedFrameCount{0};
            bool historyWarmupActive{false};
            bool generatedImagesPreacquired{false};
            // Fixed refresh retains zero-wait sequential acquire. Requested
            // VRR uses a finite application-present ceiling, then zero-wait
            // pressure preflight. One enum prevents contradictory modes.
            AdaptiveOrderedDeliveryPolicy adaptiveOrderedDeliveryPolicy{
                AdaptiveOrderedDeliveryPolicy::NotApplicable
            };
            // Recovery probes are transport-owned synthetic delivery. Keep
            // them out of Adaptive ramp and Smooth Cadence qualification even
            // when an isolated guard clears before delivery is reported.
            bool orderedAcquireRecoveryProbe{false};
            bool boundedOrderedAcquireProbe{false};
            std::optional<uint64_t> configuredAcquireTimeout;
            // Opt-in diagnostic phase accounting stays inline with the frame
            // plan so a slow total can expose multiple sub-threshold waits
            // without allocating or emitting one record per phase.
            std::chrono::steady_clock::duration renderFenceWaitDuration{};
            std::chrono::steady_clock::duration preacquireDuration{};
            std::chrono::steady_clock::duration scheduleFramesDuration{};
            std::chrono::steady_clock::duration submitSourceCopyDuration{};
            std::array<uint32_t, GeneratedFramePlan::capacity>
                preacquiredGeneratedImages{};
        };

        struct FrameState {
            size_t sequenceIndex{1};
            // The backend timeline is reset only when framegen-vulkan commits a new
            // private context. Diagnostics and binary-semaphore ring indices
            // remain monotonic across that transition.
            size_t backendTimelineIndex{1};
            size_t realFrameIndex{0};
            size_t backendFrameIndex{0};
            // Timeline value whose signal proves the backend batch of the
            // previous source no longer reads either source image; 0 = unknown.
            uint64_t sourceReleaseTimelineValue{0};
            // Value of the batch being scheduled in the current present; the
            // source copy moves it into sourceReleaseTimelineValue.
            uint64_t nextSourceReleaseTimelineValue{0};
        };

        struct RecoveryState {
            GeneratedImageAdmission generatedImageAdmission;
            OrderedAcquireRecovery orderedAcquireRecovery;
            PipelineBusyRecovery pipelineBusyRecovery;
            ReplacementBackendStabilization replacementBackendStabilization;
            bool backendPending{false};
            size_t historyWarmupRemaining{0};
        };

        struct DiagnosticsState {
            std::optional<std::chrono::steady_clock::time_point>
                fixedWindowStarted;
            size_t fixedRealFrames{0};
            size_t fixedGeneratedFrames{0};
            size_t fixedSkippedFrames{0};
            uint64_t contextId{0};
            bool orderedAcquireClassificationSplitLogged{false};
            bool orderedGeneratedAdmissionPolicyLogged{false};
            std::optional<present_diagnostics::ApplicationPresentMode>
                applicationPresentMode;
        };

        struct ColorTransitionState {
            std::optional<bool> pendingGamescopeHdrActive;
            uint64_t pendingHdrStateRevision{0};
            std::optional<std::chrono::steady_clock::time_point> retryAt;
        };

        struct SpatialResourceRequest {
            ls::ScalingMethod method{ls::ScalingMethod::Native};
            float sharpness{0.5F};
            ls::HdrQuality hdrQuality{ls::HdrQuality::Quality};

            friend bool operator==(
                    const SpatialResourceRequest& left,
                    const SpatialResourceRequest& right) {
                return left.method == right.method &&
                    left.sharpness == right.sharpness &&
                    ls::hdrCompactBuffers(left.hdrQuality) == ls::hdrCompactBuffers(right.hdrQuality);
            }
        };

        struct FrameGenerationResourceRequest {
            ls::GameConf profile;

            friend bool operator==(
                    const FrameGenerationResourceRequest& left,
                    const FrameGenerationResourceRequest& right) {
                return ls::effectiveFlowScale(left.profile) ==
                        ls::effectiveFlowScale(right.profile) &&
                    ls::hdrApproximateGeneration(left.profile.hdr_quality) ==
                        ls::hdrApproximateGeneration(right.profile.hdr_quality) &&
                    ls::effectivePerformanceMode(left.profile) ==
                        ls::effectivePerformanceMode(right.profile) &&
                    generatedFrameCapacityForProfile(left.profile) ==
                        generatedFrameCapacityForProfile(right.profile);
            }
        };

        struct FrameGenerationResources {
            bool hdrReducedPrecision{false};
            // Context must be destroyed before the exported Vulkan resources
            // it imports. Members are destroyed in reverse declaration order.
            std::vector<vk::Image> sourceImages;
            std::vector<vk::Image> destinationImages;
            ls::lazy<vk::TimelineSemaphore> syncSemaphore;
            ls::owned_ptr<ls::R<backend::Context>> context;
        };

        struct RuntimeStatusState {
            ls::GameConf requestedProfile;
            uint64_t stateRevision{0};
            bool swapchainRecreationPending{false};
            bool processRestartPending{false};
            std::optional<std::string> error;
        };

        bool frameGenerationHdrReducedPrecision{false};
        std::vector<vk::Image> sourceImages;
        std::vector<vk::Image> destinationImages;
        ls::lazy<vk::TimelineSemaphore> syncSemaphore;
        // D7c: forgets this swapchain's FG timeline entry before syncSemaphore is destroyed.
        // Members are destroyed in reverse order, so it must stay declared after syncSemaphore.
        present_diagnostics::FgTimelineLease fgTimelineLease;

        struct RenderPass {
            vk::CommandBuffer commandBuffer;
            vk::Semaphore acquireSemaphore;
        };
        // One slot per frame-generation batch. A slot is reused only after the
        // fence of the batch three presents earlier signals; source-image
        // reuse is ordered on the GPU by FrameState::sourceReleaseTimelineValue.
        static constexpr size_t renderSlotDepth = 3;
        struct RenderSlot {
            vk::CommandBuffer sourceCopy;
            vk::Fence fence;
            std::vector<RenderPass> passes;
            bool inFlight{false};
            // Staging, its binary signal and its recorded game-queue copy are
            // created with generatedCopies and released with them when the
            // destination images are replaced.
            std::optional<vk::Image> staging;
            std::optional<vk::Semaphore> staged;
            std::optional<vk::CommandBuffer> stagingCopy;
            // One pre-recorded copy per swapchain image, from staging to that
            // image, recorded lazily and cleared whenever the destination images
            // are replaced.
            std::vector<vk::CommandBuffer> generatedCopies;
            // Diagnostic split timestamps of this slot's last source copy. Read only
            // after this slot's fence is observed signalled; the copy itself is unchanged.
            std::unique_ptr<vk::TimestampQueryPool> splitQueries;
            uint64_t splitSource{};
            bool splitPending{};
        };
        std::vector<RenderSlot> renderSlots;
        size_t renderSlotIndex{0};
        RenderSlot& currentRenderSlot() {
            return this->renderSlots.at(this->renderSlotIndex);
        }
        // first: presentation wait indexed by acquired WSI image; second:
        // inter-copy dependency indexed by sequence ring, never presented.
        std::vector<std::pair<vk::Semaphore, vk::Semaphore>> postCopySemaphores;

        struct SpatialScalingPass {
            vk::CommandBuffer commandBuffer;
            vk::Semaphore readySemaphore;
            vk::Fence completionFence;
            bool completionInFlight{false};
            SpatialCommandRecording recording;
        };
        std::optional<SpatialScaler> spatialScaler;
        std::optional<bool> shaderHdrReducedPrecision;
        std::vector<SpatialScalingPass> spatialScalingPasses;
        SpatialFramePipelinePlacement spatialFramePipelinePlacement{
            SpatialFramePipelinePlacement::PreFrameGeneration
        };

        struct PresentRetirementFence {
            vk::Fence fence;
            bool associated{false};
            bool used{false};
        };
        std::vector<PresentRetirementFence> presentRetirementFences;
        bool lastLowerPresentRetirementProtected{false};
        bool presentRetirementBusyLogged{false};
        bool externalPresentFenceLogged{false};

        backend::Instance* instance{nullptr};
        ls::owned_ptr<ls::R<backend::Context>> ctx;
        FrameState frameState;
        RecoveryState recoveryState;
        DiagnosticsState diagnosticsState;
        std::unique_ptr<present_diagnostics::PacingProfile> pacingProfile;
        std::unique_ptr<present_diagnostics::NativeCopyProfile> nativeCopyProfile;
        bool nativeCopyProfileInitialized{};
        std::shared_ptr<FrameContentCapture> frameContentCapture;
        // Diagnostic GPU split timestamps; created only while the current backend
        // context reports them enabled. Recent halves wait here for their backend sample.
        std::optional<vk::CalibratedClock> splitRenderClock;
        present_diagnostics::Clock::time_point splitRenderCalibratedAt{};
        bool splitRenderUnavailable{};
        std::array<present_diagnostics::SplitRenderHalf, 8> splitRenderHalves{};
        size_t splitRenderHalfNext{};
        ColorTransitionState colorTransitionState;
        PrivateResourceTransition<SpatialResourceRequest> spatialTransition;
        std::optional<SpatialScaler> preparedSpatialScaler;
        PrivateResourceTransition<FrameGenerationResourceRequest>
            frameGenerationTransition;
        std::optional<FrameGenerationResources>
            preparedFrameGenerationResources;
        RuntimeStatusPublisher runtimeStatusPublisher;
        RuntimeStatusState runtimeStatusState;
        LiveProfileResourceRecreation liveProfileResourceRecreation;
        std::optional<std::chrono::steady_clock::time_point>
            spatialScalingAdmissionRetryAfter;
        bool spatialScalingAdmissionRetrySignaled{false};
        std::optional<AdaptiveScheduler> adaptiveScheduler;
        // A partial native-first acquisition proves that this exact ordered
        // WSI context cannot reliably deliver its larger automatic batch.
        // Retain the observed lower ceiling until game-owned recreation.
        std::optional<size_t> adaptiveOrderedWsiGeneratedCapacityLimit;
        size_t configuredFixedGeneratedFrames{0};
        // Set after a safety timeout; later generated acquires poll with
        // timeout 0 until one succeeds, so a hidden window costs at most one
        // safety wait.
        bool generatedAcquireStalled{false};

        bool gamescopeDetected{false};
        // Immutable for this context; copied from SwapchainInfo rather than
        // inferred again from the current SDR/HDR colour pipeline.
        bool privateOrderedTransport{false};
        VkPresentModeKHR currentPresentMode{VK_PRESENT_MODE_FIFO_KHR};
        std::optional<uint32_t> gamescopeRefreshHz;
        GamescopePresentationFeedback gamescopePresentationFeedback;
        FixedRefreshBudget fixedRefreshBudget;
        RealFramePacer realFramePacer;
        // Source-cap deadline moved to the application's own release wait;
        // see release_gate.hpp. Learns per context; holds presents until then.
        ReleaseGateOwner releaseGate;
        std::shared_ptr<present_diagnostics::GpuLoadMonitor> gpuLoadMonitor;
        bool gpuLoadMonitorInitialized{false};
        GpuHeadroomController gpuHeadroom;
        GpuHeadroomController::Result gpuHeadroomResult;
        std::optional<std::chrono::steady_clock::time_point> lastGpuHeadroomDiagnostic;
        // Actual scheduled batch, including the real image; never the user's
        // unproven maximum multiplier. Reset before native/history fallbacks.
        size_t bridgeOutputBatchSize{1};
        std::optional<std::chrono::steady_clock::time_point> outputSourceDeadline;
        // Last deadline scheduled by an owned output timeline. Recovery may
        // sample native cadence only after this earlier queue has cleared.
        std::optional<std::chrono::steady_clock::time_point> orderedPresentationDeadline;
        OrderedPresentTimeline wsiPresentTimeline;
        OwnedPresentTiming presentTiming;
        // Last automatic cap value printed to the log; zero when no automatic cap applies.
        double uncappedAutoCapLogged{0.0};
        // True when the previous application present used an automatic cap value as baseFpsCap.
        bool uncappedAutoCapApplied{false};
        double applicationRefreshHz{0.0};
        uint64_t refreshQuerySource{0};
        int32_t refreshQueryLastResult{INT32_MIN};
        uint64_t refreshQueryLastDuration{UINT64_MAX - 1};
        uint64_t refreshQueryLastInterval{UINT64_MAX - 1};
        bool refreshQueryPrinted{false};
        PresentFeedbackProbe presentFeedback;
#if defined(VK_KHR_present_wait2)
        PFN_vkWaitForPresent2KHR presentWait2{};
#endif
        uint64_t presentFeedbackTag{};
        bool refreshQueryConfirmed{false};
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
        PFN_vkGetSwapchainTimingPropertiesEXT swapchainTimingProperties{};
#endif
        bool presentTimingInitialized{false};
        PFN_vkGetPastPresentationTimingGOOGLE wsiPresentTimingQuery{nullptr};
        uint32_t wsiTimingPresentsSinceDrain{0};
        bool wsiPresentTimingLogged{false};
        VkResult deferredPresentResult{VK_SUCCESS};
        bool deferredOutputQueuedThisFrame{false};
        std::optional<std::chrono::steady_clock::time_point> deferredPresentDeadline;
        // Set only for the duration of a present() that was given an internal image.
        InternalPresentImage* internalPresent{nullptr};
        bool internalScalingWaitLogged{false};
        SmoothCadenceBaseCap smoothCadenceBaseCap;
        SmoothCadencePacerHandoff smoothCadencePacerHandoff;
        // A null-old replacement loses the driver's explicit WSI lineage.
        // Consume this exactly once before any private spatial submission.
        bool replacementWsiPrimePending{false};

        SwapchainColorPipeline colorPipeline;
        bool hdrExposureDisabled{true};
        std::optional<std::filesystem::path> scalingShaderDll;
        bool fp16Requested{true};
        ls::GameConf profile;
        SwapchainInfo info;

        [[nodiscard]] bool applyPendingColorPipeline(const vk::Vulkan& vk);
        [[nodiscard]] bool applyPendingFrameGenerationResources(
            const vk::Vulkan& vk);
        void applyPendingSpatialScaler(const vk::Vulkan& vk);
        void prepareSpatialScalingPass(const vk::Vulkan& vk,
            SpatialScalingPass& pass);
        void recordSpatialScalingPass(const vk::Vulkan& vk,
            SpatialScalingPass& pass, VkImage applicationImage,
            VkImage frameGenerationSource = VK_NULL_HANDLE);
        void invalidateSpatialScalingRecordings() noexcept;
        [[nodiscard]] bool spatialScalingPassesReady(
            const vk::Vulkan& vk);
        void configureDirectSpatialFrameGenerationOutputs(
            const vk::Vulkan& vk, SpatialScaler& scaler,
            std::span<const vk::Image> outputs,
            std::string_view reason) noexcept;
        void clearDirectSpatialFrameGenerationOutputs() noexcept;
        void publishRuntimeStatus(std::string_view reason) noexcept;
        [[nodiscard]] static std::optional<uint64_t>
            generatedImageAcquireTimeoutNs();
        void ensureFrameGenerationExecutionResources(const vk::Vulkan& vk);
        void initializeGpuPacingProfile(backend::Instance& backend, backend::Context& context) noexcept;
        [[nodiscard]] FrameGenerationResources buildFrameGenerationResources(
            const vk::Vulkan& vk, const SwapchainColorPipeline& pipeline,
            const ls::GameConf& resourceProfile);
        void commitFrameGenerationResources(
            FrameGenerationResources resources,
            const ls::GameConf& resourceProfile,
            std::string_view reason);
        void rebuildPrivateResources(const vk::Vulkan& vk,
            SwapchainColorPipeline pipeline,
            const ls::GameConf& resourceProfile);
        [[nodiscard]] bool resetGenerationScheduler(
            std::chrono::steady_clock::time_point now,
            std::string_view reason);

        // Real-frame presentation and source history.
        void recordPresentCadence(
            std::chrono::steady_clock::time_point presentNow);
        [[nodiscard]] VkResult presentNativeFrame(
            const PresentInvocation& invocation);
        [[nodiscard]] VkResult presentDirectApplicationFrame(
            const PresentInvocation& invocation,
            std::string_view healthSource = "native");
        [[nodiscard]] VkResult presentSpatiallyScaledFrame(
            const PresentInvocation& invocation);
        [[nodiscard]] VkResult presentOriginalImage(
            const PresentInvocation& invocation, VkSemaphore waitSemaphore,
            const void* nextChain,
            std::chrono::steady_clock::duration* duration = nullptr);
        void submitSourceCopy(const PresentInvocation& invocation,
            VkImage swapchainImage, const vk::Image& sourceImage);
        [[nodiscard]] VkResult presentHistoryOnly(
            const PresentInvocation& invocation,
            const PresentationFramePlan& plan);

        // Frame planning, resource readiness, and generated-image admission.
        [[nodiscard]] bool recoverBackendIfReady(const vk::Vulkan& vk);
        void ensureHistoryWarmup(bool restart = false);
        [[nodiscard]] PresentationFramePlan prepareFramePlan(
            std::chrono::steady_clock::time_point presentNow,
            bool orderedAcquireRecoveryProbe);
        void reportAdaptiveDelivery(const PresentationFramePlan& plan,
            size_t acceptedForPresentation);
        [[nodiscard]] bool generationPipelineReady(
            const vk::Vulkan& vk, bool gamescopeHdrTransport,
            const PresentationFramePlan& plan,
            std::chrono::steady_clock::time_point presentNow);
        [[nodiscard]] bool prepareRenderFence(const vk::Vulkan& vk);
        // Diagnostic split timestamps. Recording only adds two queries to the existing copy.
        [[nodiscard]] bool recordRenderSplitBegin(const vk::Vulkan& vk, VkCommandBuffer command, RenderSlot& slot);
        void recordRenderSplitEnd(VkCommandBuffer command, RenderSlot& slot);
        void collectRenderSplitTimestamps();
        [[nodiscard]] present_diagnostics::SplitRenderHalf findRenderSplitHalf(uint64_t source) const;
        [[nodiscard]] bool recordGeneratedCopies(const vk::Vulkan& vk, RenderSlot& slot);
        [[nodiscard]] bool renderSlotsInFlight() const;
        [[nodiscard]] bool pollRenderSlotsIdle(const vk::Vulkan& vk);
        void handleRenderFenceBudgetMiss(
            const PresentationFramePlan& plan);
        void preacquireGeneratedImages(
            const PresentInvocation& invocation,
            PresentationFramePlan& plan,
            bool trackNonblockingAdmission,
            bool classifyAdaptiveLoadFailure,
            uint64_t acquireTimeout,
            bool reportAvailableOnFullAdmission = true);
        void handleGeneratedImageAdmissionPressure(
            const PresentationFramePlan& plan,
            size_t admittedGeneratedFrames,
            bool logPressure,
            bool retainPartialAdmissionCapacity,
            bool classifyAdaptiveLoadFailure,
            uint64_t acquireTimeoutNanoseconds,
            const char* action);
        void reportGeneratedImageAdmissionAvailable(
            size_t requiredStableBatches = 1);

        // Generated delivery and lower-swapchain retirement.
        [[nodiscard]] VkResult presentGeneratedFrames(
            const PresentInvocation& invocation,
            const PresentationFramePlan& plan,
            bool gamescopeHdrTransport);
        [[nodiscard]] VkResult queuePresentWithRetirementFence(
            const vk::Vulkan& vk, VkQueue queue,
            const VkPresentInfoKHR& presentInfo,
            const present_diagnostics::PacingProfile::OutputObservation& observation = {});
        // F5 internal images. The application image is read from the private
        // image, and its presentation wait and retirement fence use realSlot.
        [[nodiscard]] VkImage applicationImageFor(uint32_t applicationIndex) const;
        [[nodiscard]] size_t presentationSlotFor(uint32_t applicationIndex) const;
        void ensureInternalPresentSlots(const vk::Vulkan& vk);
        [[nodiscard]] bool enqueueInternalRealPresent(const vk::Vulkan& vk,
            const VkPresentInfoKHR& presentInfo, DeferredPresent::Clock::time_point deadline);
        [[nodiscard]] VkResult materializeInternalPresent(const vk::Vulkan& vk,
            const VkPresentInfoKHR& presentInfo, uint32_t* lowerIndex, VkSemaphore* copied);
        VkResult retireAcquiredImagesAndPresent(const vk::Vulkan& vk,
            VkQueue queue, VkSwapchainKHR swapchain, const void* nextChain,
            uint32_t originalImageIndex,
            std::span<const VkSemaphore> applicationWaitSemaphores,
            std::span<const uint32_t> acquiredImageIndices,
            VkImage originalImage);
        // Declared last: join before any semaphore, fence, image or backend
        // context can be destroyed, including moved retirement contexts.
        std::unique_ptr<DeferredPresent> deferredPresent;
    };

}

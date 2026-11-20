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
#include "color_pipeline.hpp"
#include "completion_spiral.hpp"
#include "generated_frame_delivery.hpp"
#include "profile_update.hpp"
#include "presentation_policy.hpp"
#include "present_diagnostics.hpp"
#include "runtime_transition.hpp"
#include "present_timing.hpp"
#include "swapchain/deferred_present.hpp"
#include "swapchain/internal_images.hpp"
#include "swapchain/low_latency.hpp"
#include "swapchain/onscreen_watch.hpp"
#include "swapchain/present_feedback_probe.hpp"
#include "swapchain/reflex_frame_limit.hpp"
#include "swapchain/release_gate.hpp"
#include "swapchain/screen_samples.hpp"
#include "swapchain/surface_origin.hpp"
#include "swapchain/uncapped_auto_cap.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

    class WaylandPresentationFeedback;
    struct InternalPresentImage;
    struct X11RefreshCell;

    /// swapchain info struct
    struct SwapchainInfo {
        std::vector<VkImage> images;
        VkSurfaceKHR surface;
        SurfaceOrigin surfaceOrigin{SurfaceOrigin::Unknown};
        // X11 window XID of an Xlib or XCB surface (0 otherwise), read for the X5 refresh query.
        uint64_t x11Window{0};
        // H3: a native Wayland surface in a Wine process, where PASS_THROUGH carries Wine's HDR pairs.
        bool winePassThroughHdr{false};
        std::shared_ptr<WaylandPresentationFeedback> waylandPresentation;
        std::shared_ptr<DeferredPresentQueue> deferredPresentQueue;
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
        uint32_t surfaceMinImageCount{};
        bool swapchainImageCountCompatibility{false};
        // Preserve whether the application supplied the Vulkan replacement
        // lineage explicitly so retirement and replacement stabilization keep
        // the game's ownership boundary.
        bool applicationOldSwapchainProvided{false};
        // Extent the application requested. The actual swapchain images retain `extent`.
        VkExtent2D applicationExtent;
        VkExtent2D extent;
        // Preserve the incoming application's or upper layer's request before
        // framegen-vulkan selects its immutable lower/compositor transport contract.
        VkPresentModeKHR incomingPresentMode{VK_PRESENT_MODE_FIFO_KHR};
        VkPresentModeKHR presentMode;
        // Persist the create-time transport decision; present-mode compatibility
        // cannot change without replacing the game-owned swapchain.
        bool privateOrderedTransport{false};
        // The upper WSI either supplied a live oldSwapchain handle or framegen-vulkan
        // completed retirement of the exact retained lower handle before a
        // null-old destroy-before-create replacement. This is a known
        // owner-driven replacement rather than a cold process start.
        bool replacement{false};
    };

    /// modify the swapchain create info based on the profile pre-swapchain creation
    /// @param profile active game profile
    /// @param maxImages maximum number of images supported by the surface
    /// @param createInfo swapchain create info to modify
    /// @param winePassThroughHdr native Wayland surface in a Wine process (H3 PASS_THROUGH HDR)
    /// @param surfaceMinImages surface minImageCount for the layer-needs floor (0 = none)
    [[nodiscard]] bool context_ModifySwapchainCreateInfo(
        const ls::GameConf& profile, uint32_t maxImages,
        VkSwapchainCreateInfoKHR& createInfo,
        bool frameGenerationInteropEnabled,
        bool winePassThroughHdr,
        uint32_t surfaceMinImages = 0);

    /// swapchain context for a layer instance
    class Swapchain {
    public:
        /// create a new swapchain context
        /// @param vk vulkan instance
        /// @param backend optional framegen-vulkan frame-generation backend
        /// @param profile active game profile
        /// @param info swapchain info
        Swapchain(const vk::Vulkan& vk, backend::Instance* backend,
            ls::GameConf profile, SwapchainInfo info,
            bool fp16Requested,
            bool swapchainMaintenance1Enabled);

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
            uint64_t readyValue, std::chrono::steady_clock::time_point readyDeadline,
            std::optional<std::chrono::steady_clock::time_point> releasedOutputLimit = std::nullopt,
            std::chrono::steady_clock::duration releasedJoinTolerance = {},
            bool overlap = false, bool justInTime = false,
            std::chrono::steady_clock::duration outputInterval = {},
            std::chrono::steady_clock::time_point arrival = {});
        [[nodiscard]] bool hasDeferredPresent() const { return static_cast<bool>(this->deferredPresent); }
        // X3: whether the application chain of the latest present reached a lower present or the
        // deferred worker. False after a present that threw or returned before that point: the
        // caller then signals the application's present fence itself.
        [[nodiscard]] bool applicationChainWasHandedOff() const { return this->applicationChainHandedOff; }
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
        /// Native acquire fallback (low_latency): the release point the next acquire waits for,
        /// or nothing while the application releases through tokens (release_gate.hpp). `now` is
        /// the acquire's start, the origin of a stepping GPU hold (L5).
        [[nodiscard]] std::optional<ReleaseGate::Clock::time_point> acquireRelease(
                ReleaseGate::Clock::time_point now) const {
            return this->releaseGate.acquireRelease(now);
        }
        /// The acquire fallback held an acquire; the application is recorded as a native waiter.
        void noteAcquireHold() const {
            this->releaseGate.noteAcquireHold();
        }
        /// The acquire fallback returned to the application at `now` (L5 observation).
        void noteAcquireReleased(ReleaseGate::Clock::time_point now) const {
            this->releaseGate.noteAcquireReleased(now);
        }
        [[nodiscard]] VkExtent2D frameGenerationResourceExtent() const {
            return this->info.extent;
        }
        /// Reflex sleep mode of this swapchain: the application's request and the frame limit
        /// it is forwarded with (reflex_frame_limit.hpp).
        [[nodiscard]] ReflexFrameLimit& reflexFrameLimit(VkSwapchainKHR swapchain) {
            if (!this->reflexLimit) this->reflexLimit = ReflexFrameLimitRegistry::get(swapchain);
            return *this->reflexLimit;
        }
        /// Profile currently applied to this context, read by the F5 internal-image gate.
        [[nodiscard]] const ls::GameConf& appliedProfile() const {
            return this->profile;
        }
        /// The swapchain has no internal application images (reason from the F5 gate,
        /// a string literal). It presents real frames only from now on.
        void setFrameGenerationUnavailable(const char* reason);
        /// Apply configuration that is safe for an already-created context.
        /// Flow Scale and model changes rebuild framegen-vulkan's private
        /// frame-generation context at the next presentation boundary.
        /// Unsupported resource-shape changes wait for a natural recreation.
        /// Process-wide backend changes wait for restart. The decision reports
        /// every applied and pending boundary.
        [[nodiscard]] ProfileUpdateDecision updateProfile(
            const ls::GameConf& profile, uint64_t runtimeStateRevision);

        /// After the lower WSI has consumed this present's wait semaphores,
        /// convert one successful result into VK_ERROR_OUT_OF_DATE_KHR for a
        /// pending live profile-resource change. The application remains the
        /// owner of destruction and recreation.
        [[nodiscard]] bool requestLiveProfileResourceRecreationAfterPresent(
            VkResult lowerPresentResult);

        /// Escalate persistent generated-image starvation after a confirmed
        /// menu return.
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
        /// Stop generation in place when the active profile disappears.
        void disableFrameGeneration();
#if defined(VK_KHR_present_wait2) && defined(VK_KHR_present_id2)
        /// GP1: starts this lower swapchain's on-screen profile (the gate was active at creation). `waitForPresent2`
        /// is the next layer's vkWaitForPresent2KHR. Nothing starts without the gate.
        void startOnScreenProfile(VkDevice device, VkSwapchainKHR swapchain,
            PFN_vkWaitForPresent2KHR waitForPresent2);
        /// GP1: unregisters the on-screen owner and joins its watcher. Call before the lower swapchain is destroyed.
        void stopOnScreenProfile() noexcept;
#endif
        /// OS1: decides which producer feeds this lower swapchain's automatic output correction and logs the
        /// decision, once per lower swapchain. `watcherActive` and `gateReason` are the GP1 gate's result.
        /// GT1: `gamescopeSession` decides the gamescope tick lock with the same inputs (see tick_lock.hpp).
        void configureScreenCorrection(bool watcherActive, const char* gateReason, bool gamescopeSession);
    private:
#if defined(VK_KHR_present_wait2) && defined(VK_KHR_present_id2)
        /// GP1: gives the deferred worker the on-screen owner and watcher, when the gate is active.
        void attachDeferredOnScreen();
#endif
        /// OS1: the screen samples of this swapchain, created on first use and shared with the watcher's observer,
        /// the deferred worker and the native Wayland sink.
        std::shared_ptr<ScreenSampleRing> screenRing();
        /// OS1: hands the screen ring to the deferred worker, and to the Wayland observer as its sink when that is
        /// the source. Called wherever the deferred worker is created.
        void attachScreenSamples();
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
            // Generated-output preparation may spend the existing cap wait.
            // Every real-frame/fallback path retains this deadline.
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
            // low_latency with a released batch: the worker starts the batch at its own readiness.
            bool justInTime{false};
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
            // Executable plan after transport-specific admission. Headroom-tight
            // ordered SDR may contain fewer, deliberately re-spaced timestamps.
            GeneratedFramePlan scheduledGeneratedFrames;
            size_t admittedGeneratedFrameCount{0};
            bool historyWarmupActive{false};
            bool generatedImagesPreacquired{false};
            // Recovery probes are transport-owned synthetic delivery.
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
            // Private queue tail value that protects the preceding staging read.
            uint64_t privateStagingValue{0};
        };

        struct RecoveryState {
            GeneratedImageAdmission generatedImageAdmission;
            OrderedAcquireRecovery orderedAcquireRecovery;
            ReplacementBackendStabilization replacementBackendStabilization;
            bool backendPending{false};
            size_t historyWarmupRemaining{0};
        };

        struct DiagnosticsState {
            uint64_t contextId{0};
        };

        struct FrameGenerationResourceRequest {
            ls::GameConf profile;

            friend bool operator==(
                    const FrameGenerationResourceRequest& left,
                    const FrameGenerationResourceRequest& right) {
                return ls::effectiveFlowScale(left.profile) ==
                        ls::effectiveFlowScale(right.profile) &&
                    ls::effectivePerformanceMode(left.profile) ==
                        ls::effectivePerformanceMode(right.profile) &&
                    generatedFrameCapacityForProfile(left.profile) ==
                        generatedFrameCapacityForProfile(right.profile);
            }
        };

        struct FrameGenerationResources {
            // Context must be destroyed before the exported Vulkan resources
            // it imports. Members are destroyed in reverse declaration order.
            std::vector<vk::Image> sourceImages;
            std::vector<vk::Image> destinationImages;
            ls::lazy<vk::TimelineSemaphore> syncSemaphore;
            ls::owned_ptr<ls::R<backend::Context>> context;
        };

        std::vector<vk::Image> sourceImages;
        std::vector<vk::Image> destinationImages;
        ls::lazy<vk::TimelineSemaphore> syncSemaphore;
        // D7c: forgets this swapchain's FG timeline entry before syncSemaphore is destroyed.
        // Members are destroyed in reverse order, so it must stay declared after syncSemaphore.
        present_diagnostics::FgTimelineLease fgTimelineLease;

        struct GeneratedAcquireFence {
            explicit GeneratedAcquireFence(const vk::Vulkan& vk);
            ~GeneratedAcquireFence();
            vk::Fence fence;
            DeferredPresent::AcquireProbe probe;
            VkDevice device;
            PFN_vkWaitForFences wait;
        };
        struct RenderPass {
            vk::CommandBuffer commandBuffer;
            vk::Semaphore acquireSemaphore;
            std::unique_ptr<GeneratedAcquireFence> acquireFence;
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
            // Binary signal of this pass's worker copy or skip, waited once by the next
            // generated pass of the same batch. Created and released with the staged resources.
            std::optional<vk::Semaphore> copied;
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
            // Diagnostic split timestamps of this slot's last source copy. Read only
            // after this slot's fence is observed signalled; the copy itself is unchanged.
            std::unique_ptr<vk::TimestampQueryPool> splitQueries;
            uint64_t splitSource{};
            bool splitPending{};
            // Diagnostic output split: staging and worker copies of the first generated
            // output. Created with the recorded copies; read only after the slot fence.
            std::unique_ptr<vk::TimestampQueryPool> outputQueries;
            uint64_t outputSource{};
            bool outputPending{};
            bool outputDirect{};
        };
        std::vector<RenderSlot> renderSlots;
        size_t renderSlotIndex{0};
        RenderSlot& currentRenderSlot() {
            return this->renderSlots.at(this->renderSlotIndex);
        }
        // first: presentation wait indexed by acquired WSI image; second:
        // inter-copy dependency indexed by sequence ring, never presented.
        std::vector<std::pair<vk::Semaphore, vk::Semaphore>> postCopySemaphores;

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
        // OS1: the source of each application present. It advances where PacingProfile::beginSource advances
        // its batch, with or without the profile, so the output correction never depends on the profile. While
        // the profile is on, it equals the profile's sourceOrdinal() at every use.
        uint64_t sourceSerial{};
        // Diagnostic GPU split timestamps, render and output halves. Created on first use
        // with the split gate on; recent halves wait here for their backend sample.
        std::optional<vk::CalibratedClock> splitRenderClock;
        present_diagnostics::Clock::time_point splitRenderCalibratedAt{};
        bool splitRenderUnavailable{};
        std::array<present_diagnostics::SplitRenderHalf, 8> splitRenderHalves{};
        // Backend split samples waiting for their render half (diagnostics only), with the presents waited.
        static constexpr uint8_t splitRenderWaitPresents = 6;
        std::vector<std::pair<backend::SplitFrameProfile, uint8_t>> splitAwaitingRender;
        size_t splitRenderHalfNext{};
        PrivateResourceTransition<FrameGenerationResourceRequest>
            frameGenerationTransition;
        std::optional<FrameGenerationResources>
            preparedFrameGenerationResources;
        LiveProfileResourceRecreation liveProfileResourceRecreation;
        const char* frameGenerationUnavailableReason{nullptr};
        LiveEnableRecreation liveEnableRecreation;
        bool liveEnableRecreationWaitLogged{false};
        size_t configuredFixedGeneratedFrames{0};
        // Set after a safety timeout; later generated acquires poll with
        // timeout 0 until one succeeds, so a hidden window costs at most one
        // safety wait.
        bool generatedAcquireStalled{false};

        // Immutable for this context; copied from SwapchainInfo rather than
        // inferred again from the current colour pipeline.
        bool privateOrderedTransport{false};
        VkPresentModeKHR currentPresentMode{VK_PRESENT_MODE_FIFO_KHR};
        // Last per-present modes printed to stderr (-2 = nothing printed yet).
        int64_t loggedApplicationPresentMode{-2}, loggedRealPresentMode{-2};
        FixedRefreshBudget fixedRefreshBudget;
        RealFramePacer realFramePacer;
        // Log-only completion spiral check. It reads pacing values and never changes them.
        CompletionSpiralDetector completionSpiral;
        // Source-cap deadline moved to the application's own release wait;
        // see release_gate.hpp. Learns per context; holds presents until then.
        ReleaseGateOwner releaseGate;
        // Delay of every release point of this context (low_latency; low_latency.hpp).
        LowLatencyRelease lowLatencyRelease;
        // GPU release of this context (L5, low_latency.hpp). The predictor exists while the release
        // delay runs with split timestamps; the stepping hold is the fallback without them. The armed
        // release is the predicted release of the next frame; the previous arrival bounds the release
        // time that the next token return may take (present.cpp stepGpuRelease).
        std::optional<GpuReleasePredictor> gpuPredictor;
        GpuSteppingHold gpuStepping;
        std::optional<ReleaseGate::Clock::time_point> gpuArmed;
        std::optional<ReleaseGate::Clock::time_point> gpuPreviousArrival;
        // Reflex request of this swapchain, shared with the Reflex wrapper through
        // ReflexFrameLimitRegistry; fetched at the first present.
        std::shared_ptr<ReflexFrameLimit> reflexLimit;
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
        // GT1: the application timing query reported variable refresh (an all-ones refresh interval).
        bool applicationRefreshVariable{false};
        // X5: refresh of the monitor under an X11 surface. A detached query started at creation fills
        // the cell (x11_refresh.hpp). The present path reads it only when owned and application timing
        // gave no rate. Null for other surfaces.
        std::shared_ptr<X11RefreshCell> x11Refresh;
        uint64_t refreshQuerySource{0};
        int32_t refreshQueryLastResult{INT32_MIN};
        uint64_t refreshQueryLastDuration{UINT64_MAX - 1};
        uint64_t refreshQueryLastInterval{UINT64_MAX - 1};
        bool refreshQueryPrinted{false};
        PresentFeedbackProbe presentFeedback;
#if defined(VK_KHR_present_wait)
        PFN_vkWaitForPresentKHR presentWait1{};
#endif
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
        // X3: set when the present's application image reaches a lower present or the deferred worker.
        // present() clears it on entry.
        bool applicationChainHandedOff{false};

        SwapchainColorPipeline colorPipeline;
        bool fp16Requested{true};
        ls::GameConf profile;
        SwapchainInfo info;

        [[nodiscard]] bool applyPendingFrameGenerationResources(
            const vk::Vulkan& vk);
        [[nodiscard]] static std::optional<uint64_t>
            generatedImageAcquireTimeoutNs();
        void ensureFrameGenerationExecutionResources(const vk::Vulkan& vk);
        void initializeGpuPacingProfile(backend::Instance& backend, backend::Context& context,
            bool lowLatency) noexcept;
        // L5 GPU release of one capped present (present.cpp): the hold its release token carries and
        // the mode it runs in. `active` is the release delay of this present (low_latency.hpp).
        struct GpuRelease {
            GpuReleaseMode mode{GpuReleaseMode::Off};
            GpuHold hold{};
        };
        [[nodiscard]] GpuRelease stepGpuRelease(bool active,
            std::chrono::steady_clock::time_point arrival,
            std::chrono::steady_clock::duration capInterval);
        // The backend's previous batch still runs on the GPU, from one non-blocking fence query.
        [[nodiscard]] std::optional<bool> previousBatchBusy() const;
        void resetGpuRelease() noexcept;
        [[nodiscard]] FrameGenerationResources buildFrameGenerationResources(
            const vk::Vulkan& vk, const SwapchainColorPipeline& pipeline,
            const ls::GameConf& resourceProfile);
        void commitFrameGenerationResources(
            FrameGenerationResources resources,
            const ls::GameConf& resourceProfile);

        // Real-frame presentation and source history.
        [[nodiscard]] VkResult presentNativeFrame(
            const PresentInvocation& invocation);
        [[nodiscard]] VkResult presentDirectApplicationFrame(
            const PresentInvocation& invocation,
            std::string_view healthSource = "native");
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
        [[nodiscard]] bool prepareRenderFence(const vk::Vulkan& vk);
        // Diagnostic split timestamps. Recording only adds two queries to the existing copy.
        [[nodiscard]] bool recordRenderSplitBegin(const vk::Vulkan& vk, VkCommandBuffer command, RenderSlot& slot);
        void recordRenderSplitEnd(VkCommandBuffer command, RenderSlot& slot);
        void ensureSplitRenderClock(const vk::Vulkan& vk, uint32_t validBits, float periodNs);
        void refreshSplitRenderCalibration();
        void recordOutputGpuSplit(const vk::Vulkan& vk, const RenderSlot& slot);
        void collectRenderSplitTimestamps(const vk::Vulkan& vk);
        [[nodiscard]] present_diagnostics::SplitRenderHalf findRenderSplitHalf(uint64_t source) const;
        [[nodiscard]] bool recordGeneratedCopies(const vk::Vulkan& vk, RenderSlot& slot, size_t pass);
        [[nodiscard]] bool renderSlotsInFlight() const;
        [[nodiscard]] bool pollRenderSlotsIdle(const vk::Vulkan& vk);
        void handleRenderFenceBudgetMiss();
        void preacquireGeneratedImages(
            const PresentInvocation& invocation,
            PresentationFramePlan& plan,
            bool trackNonblockingAdmission,
            uint64_t acquireTimeout,
            bool reportAvailableOnFullAdmission = true);
        void reportGeneratedImageAdmissionAvailable(
            size_t requiredStableBatches = 1);

        // Generated delivery and lower-swapchain retirement.
        [[nodiscard]] VkResult presentGeneratedFrames(
            const PresentInvocation& invocation,
            const PresentationFramePlan& plan);
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
        void noteInternalCopyProfile(const vk::Vulkan& vk, InternalPresentImage& image);
        [[nodiscard]] VkResult materializeInternalPresent(const vk::Vulkan& vk,
            const VkPresentInfoKHR& presentInfo, uint32_t* lowerIndex, VkSemaphore* copied);
        VkResult retireAcquiredImagesAndPresent(const vk::Vulkan& vk,
            VkQueue queue, VkSwapchainKHR swapchain, const void* nextChain,
            uint32_t originalImageIndex,
            std::span<const VkSemaphore> applicationWaitSemaphores,
            std::span<const uint32_t> acquiredImageIndices,
            VkImage originalImage);
        // OS1: the screen ring (see screenRing) and the source chosen at creation (see configureScreenCorrection).
        // Declared before onScreen and deferredPresent, so both producers and the worker are gone before it.
        std::shared_ptr<ScreenSampleRing> screenSamples;
        ScreenSource screenSource{ScreenSource::None};
        // GT1: the gamescope tick lock of this lower swapchain, decided with screenSource (see configureScreenCorrection).
        bool screenTickLock{};
        // GP1: the on-screen profile of this lower swapchain, when its gate is active. Declared before
        // deferredPresent, so the worker is joined before the owner and watcher are destroyed.
        std::unique_ptr<OnScreenProfile> onScreen;
        // Declared last: join before any semaphore, fence, image or backend
        // context can be destroyed, including moved retirement contexts.
        std::unique_ptr<DeferredPresent> deferredPresent;
    };

}

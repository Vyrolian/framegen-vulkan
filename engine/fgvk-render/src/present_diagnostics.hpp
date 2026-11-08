/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "adaptive_scheduler.hpp"
#include "bridge_present_timing.hpp"

#include <chrono>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <optional>
#include <span>
#include <utility>
#include <string_view>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer::present_diagnostics {

    using Clock = std::chrono::steady_clock;

    [[nodiscard]] uint64_t allocateContextId();
    /// Return the process-start diagnostic policy shared by all layer paths.
    [[nodiscard]] bool enabled();
    [[nodiscard]] bool pacingProfileEnabled();
    [[nodiscard]] bool pacingGpuProfileEnabled();
    /// Non-draining split GPU boundaries; active only with FGVK_PACING_PROFILE=1 and FGVK_PACING_PROFILE_GPU_SPLIT=1.
    [[nodiscard]] bool pacingGpuSplitProfileEnabled();
    [[nodiscard]] bool pacingFrameTraceEnabled();
    /// Native copy timestamps; generated copies move to the present thread. Implies the frame trace.
    [[nodiscard]] bool pacingCopyProfileEnabled();
    [[nodiscard]] bool pacingWaylandProfileEnabled();
    /// Publish already-buffered, structured observer records to the same bounded sink.
    void writePacingProfileRecords(std::string_view records) noexcept;
    /// Prepare the private capture outside presentation; never capture game stdout.
    void initializePacingProfileLog() noexcept;
    /// Full-trace setup at this layer boundary, before/after framegen-vulkan negotiation.
    /// These are enabled requests, never enumerated capabilities or proof of use.
    void traceVulkanSetup(std::string_view role, const VkInstanceCreateInfo& incoming,
        const VkInstanceCreateInfo& forwarded, VkResult result, VkInstance instance) noexcept;
    void traceVulkanSetup(std::string_view role, const VkDeviceCreateInfo& incoming,
        const VkDeviceCreateInfo& forwarded, VkResult result, VkDevice device,
        VkPhysicalDevice physicalDevice) noexcept;
    void traceVulkanSetup(std::string_view role, const VkSwapchainCreateInfoKHR& incoming,
        const VkSwapchainCreateInfoKHR& forwarded, VkResult result, VkSwapchainKHR swapchain,
        VkDevice device) noexcept;
    void traceVulkanSurface(std::string_view role, std::string_view api,
        VkInstance instance, VkSurfaceKHR surface, VkResult result) noexcept;
    /// Return the process-start slow-operation threshold in milliseconds.
    [[nodiscard]] double thresholdMilliseconds();
    [[nodiscard]] Clock::time_point start();

    /// CPU observations and optional backend GPU spans. No samples feed policy.
    /// Storage and per-frame work remain constant regardless of session length.
    class PacingProfile {
    public:
        enum Metric : size_t {
            SourceInterval, LayerPresent, OutsideLayer, OutputInterval,
            WithinBatch, BetweenBatches, LowerPresent, SourceCapWait,
            ReadinessWait, OutputWait, ProfilerPublish, PlannedOutputInterval,
            OutputWakeLateness, SourceCapWakeLateness, SourceDeadlineLateness,
            GpuPrepass, GpuGeneration, GpuOutput, GpuSpan, GpuReadback,
            PresentTargetInterval, MetricCount
        };
        enum OutputTiming : size_t { DriverTiming, ApplicationTiming, ExtTiming,
            GoogleTiming, BridgeTiming, OutputTimingCount };
        // Worker-deferred generated outputs that presented no image, by cause.
        enum GeneratedSkip : size_t { SkipReadiness, SkipExpedite, SkipGiveUp, SkipAcquire,
            SkipOther, GeneratedSkipCount };
        struct OutputObservation {
            Clock::time_point scheduled{};
            uint32_t imageIndex{UINT32_MAX};
            bool generated{}, deferred{}, copyExpected{}, copyChecked{};
            VkResult copyStatus{VK_NOT_READY};
            Clock::time_point copySampledAt{};
            uint64_t source{};
            Clock::time_point enqueued{}, queueLockStarted{}, queueLockAcquired{};
            bool readinessChecked{};
            Clock::time_point readinessStarted{}, readinessFinished{}, readinessDeadline{};
            VkResult readinessResult{VK_NOT_READY};
            VkQueue queue{};
            VkSemaphore presentWait{};
            // The real output of the application's own image, as opposed to a generated frame.
            bool applicationImage{};
            // Limiter arrival of a worker batch, on its first output only; zero otherwise.
            Clock::time_point arrival{};
        };
        struct OutputSample {
            size_t ordinal{}, source{};
            Clock::time_point started{}, finished{};
            VkResult result{};
            OutputObservation observation;
        };
        static constexpr size_t outputTraceCapacity = 512;
        struct SourceSample {
            size_t source{}, backendEpoch{}, backendFrame{};
            bool backendScheduled{};
            Clock::time_point started{}, finished{};
            std::array<Clock::duration, 3> waits{};
            std::array<Clock::duration, 9> phases{};
            bool smooth{}, frameGeneration{}, adaptive{};
            size_t multiplier{};
            double cap{}, configuredCap{};
        };
        [[nodiscard]] SourceSample sourceSample(Clock::time_point finished) const {
            return {batch, backendEpoch, backendFrame, hasBackendFrame,
                sourceStarted.value_or(Clock::time_point{}), finished, waits, lastPhases,
                window.smoothCadence, window.frameGenerationEnabled, window.adaptive,
                window.configuredMultiplier, window.effectiveCapFps, window.configuredCapFps};
        }
        // Diagnostic identities are bounded, context-local and never feed policy.
        void resetBackend() { ++backendEpoch; backendSources = {}; }
        void backendScheduled(size_t frame) {
            backendFrame = frame;
            hasBackendFrame = true;
            backendSources[frame % backendSources.size()] = {frame, batch};
        }
        [[nodiscard]] size_t sourceForBackend(size_t frame) const {
            const auto& entry = backendSources[frame % backendSources.size()];
            return entry.first == frame ? entry.second : 0;
        }
        [[nodiscard]] size_t backendGeneration() const { return backendEpoch; }
        [[nodiscard]] uint64_t sourceOrdinal() const { return batch; }
        struct Samples : BridgePresentTiming::Samples {
            double minimum{};
            size_t over33ms{};
            void add(Clock::duration duration) {
                const double value = std::chrono::duration<double, std::milli>(duration).count();
                addMilliseconds(value);
            }
            void addMilliseconds(double value) {
                if (!std::isfinite(value) || value < 0) return;
                minimum = count ? std::min(minimum, value) : value;
                over33ms += value > 1000.0 / 30.0;
                BridgePresentTiming::Samples::add(value);
            }
        };
        struct Window {
            struct SourceHitch {
                size_t previousFrame{};
                Clock::duration interval{}, layer{}, outside{}, capWait{}, readinessWait{}, outputWait{}, profilerPublish{};
                std::array<Clock::duration, 9> phases{};
            } worstSource;
            std::array<Samples, MetricCount> metrics;
            std::array<Samples, 9> phases;
            std::array<size_t, OutputTimingCount> timingAttempts{};
            size_t targetDiscontinuities{};
            size_t sources{}, outputs{}, errors{}, readyBatches{}, capBatches{};
            size_t workerBatches{}, workerReadinessMisses{};
            std::array<size_t, GeneratedSkipCount> generatedSkips{};
            size_t lateReadinessBatches{}, generatedCompressed{};
            bool internalImages{};
            size_t fifoCallerTimingSources{};
            std::array<OutputSample, outputTraceCapacity> outputTrace{};
            size_t outputTraceCount{}, outputTraceDropped{};
            Clock::duration elapsed{};
            bool smoothCadence{};
            double effectiveCapFps{};
            bool frameGenerationEnabled{}, adaptive{};
            size_t configuredMultiplier{};
            double configuredCapFps{};
        };
        void beginSource(Clock::time_point now) {
            if (!windowStarted) windowStarted = now;
            if (sourceStarted) window.metrics[SourceInterval].add(now - *sourceStarted);
            if (sourceFinished) window.metrics[OutsideLayer].add(now - *sourceFinished);
            if (sourceStarted && sourceFinished && now - *sourceStarted > window.worstSource.interval) {
                window.worstSource = {
                    .previousFrame = batch,
                    .interval = now - *sourceStarted,
                    .layer = *sourceFinished - *sourceStarted,
                    .outside = now - *sourceFinished,
                    .capWait = waits[0], .readinessWait = waits[1], .outputWait = waits[2],
                    .profilerPublish = lastPublish,
                    .phases = lastPhases,
                };
            }
            waits = {};
            lastPhases = {};
            hasBackendFrame = false;
            sourceStarted = now;
            ++batch;
            ++window.sources;
        }
        OutputSample output(Clock::time_point started, Clock::time_point finished, VkResult result,
                const OutputObservation& observation) {
            if (previousOutput) {
                const auto interval = started - *previousOutput;
                window.metrics[OutputInterval].add(interval);
                window.metrics[previousBatch == batch ? WithinBatch : BetweenBatches].add(interval);
            }
            previousOutput = started;
            previousBatch = batch;
            window.metrics[LowerPresent].add(finished - started);
            ++window.outputs;
            window.errors += result < 0;
            const OutputSample sample{++outputOrdinal, observation.source ? observation.source : batch, started, finished, result, observation};
            if (window.outputTraceCount < window.outputTrace.size())
                window.outputTrace[window.outputTraceCount++] = sample;
            else
                ++window.outputTraceDropped;
            return sample;
        }
        OutputSample output(Clock::time_point started, Clock::time_point finished, VkResult result) {
            return output(started, finished, result, OutputObservation{});
        }
        // Observe the timing actually forwarded to the lower present, rather
        // than inferring an owner from mode, extension support or CPU spacing.
        // Targets are requests in the host clock, never display observations.
        void outputTiming(OutputTiming owner, std::optional<Clock::time_point> target = {}) {
            ++window.timingAttempts[owner];
            if (target && previousTarget && previousTimingOwner == owner) {
                if (*target > *previousTarget)
                    window.metrics[PresentTargetInterval].add(*target - *previousTarget);
                else
                    ++window.targetDiscontinuities;
            }
            previousTimingOwner = owner;
            previousTarget = target;
        }
        void wait(Metric metric, Clock::duration duration) {
            window.metrics[metric].add(duration);
            if (metric >= SourceCapWait && metric <= OutputWait)
                waits[metric - SourceCapWait] += duration;
        }
        void publicationCost(Clock::duration duration) {
            lastPublish = duration;
            window.metrics[ProfilerPublish].add(duration);
        }
        void gpuMicroseconds(Metric metric, double value) {
            window.metrics[metric].addMilliseconds(value / 1000.0);
        }
        void policy(bool smooth, double cap, bool fifoCallerTiming = false) {
            window.smoothCadence = smooth;
            window.effectiveCapFps = cap;
            window.fifoCallerTimingSources += fifoCallerTiming;
        }
        void settings(bool frameGeneration, bool adaptive, size_t multiplier, double configuredCap) {
            window.frameGenerationEnabled = frameGeneration;
            window.adaptive = adaptive;
            window.configuredMultiplier = multiplier;
            window.configuredCapFps = configuredCap;
        }
        void phaseTiming(const std::array<Clock::duration, 9>& durations) {
            lastPhases = durations;
            for (size_t i = 0; i < durations.size(); ++i)
                window.phases[i].add(durations[i]);
        }
        void pacedBatch(bool ready, bool capped) {
            window.readyBatches += ready;
            window.capBatches += capped;
        }
        void completedWorkerBatch(bool readinessMiss) {
            ++window.workerBatches;
            window.workerReadinessMisses += readinessMiss;
        }
        void generatedSkipped(GeneratedSkip kind) { ++window.generatedSkips[kind]; }
        void lateReadinessBatch() { ++window.lateReadinessBatches; }
        void generatedCompressedPairs(size_t pairs) { window.generatedCompressed += pairs; }
        // Persistent for this swapchain: set once a present uses F5 internal images.
        void internalImagesActive() { internalImages = true; }
        std::optional<Window> finishSource(Clock::time_point now) {
            if (!sourceStarted || !windowStarted) return {};
            window.metrics[LayerPresent].add(now - *sourceStarted);
            sourceFinished = now;
            if (now - *windowStarted < std::chrono::seconds(1)) return {};
            window.elapsed = now - *windowStarted;
            window.internalImages = internalImages;
            const auto completed = window;
            window = {};
            windowStarted = now;
            return completed;
        }
    private:
        Window window;
        std::array<std::pair<size_t, size_t>, 8> backendSources{};
        size_t backendEpoch{1}, backendFrame{};
        bool hasBackendFrame{};
        std::array<Clock::duration, 3> waits{};
        std::array<Clock::duration, 9> lastPhases{};
        Clock::duration lastPublish{};
        size_t batch{}, previousBatch{}, outputOrdinal{};
        bool internalImages{};
        std::optional<Clock::time_point> windowStarted, sourceStarted, sourceFinished, previousOutput;
        std::optional<Clock::time_point> previousTarget;
        OutputTiming previousTimingOwner{DriverTiming};
    };

    void logPacingProfile(uint64_t context, VkPresentModeKHR mode, Clock::time_point finished,
        const PacingProfile::Window& window, VkSwapchainKHR swapchain = VK_NULL_HANDLE,
        std::string_view wsi = "unknown", bool waylandObserver = false) noexcept;
    void recordPacingProfilePhases(uint64_t context, const std::array<Clock::duration, 9>& phases);
    void logPacingSource(uint64_t context, VkSwapchainKHR swapchain,
        const PacingProfile::SourceSample& sample) noexcept;
    void bindApplicationPresentSource(uint64_t context, uint64_t source) noexcept;
    void logPacingOutput(uint64_t context, VkSwapchainKHR swapchain, const PacingProfile::OutputSample& sample) noexcept;
    /// One record per worker-deferred generated output that presented no image. Reason is a stable name.
    void logPacingSkip(uint64_t context, uint64_t source, std::string_view reason,
        Clock::time_point scheduled, Clock::time_point now) noexcept;
    void logPacingGpuFrame(uint64_t context, uint64_t source, uint64_t epoch,
        uint64_t frame, double previousWaitUs, double prepassSubmitUs, double generationSubmitUs,
        double prepassUs, double spanUs, double readbackUs, std::span<const double> generatedUs) noexcept;

    /// Render-side split boundaries of one source's copy, in CLOCK_MONOTONIC
    /// nanoseconds. Zero means unknown, including an uncalibrated application device.
    struct SplitRenderHalf {
        uint64_t source{};
        int64_t gameDoneNs{};
        int64_t sourceCopyEndNs{};
    };
    void logPacingGpuSplitFrame(uint64_t context, uint64_t source, uint64_t backendFrame,
        int64_t gameDoneNs, int64_t sourceCopyEndNs, int64_t fgStartNs, int64_t prepassEndNs,
        int64_t generatedEndNs, int64_t batchEndNs) noexcept;
    /// Diagnostic: one line per process for the first split step that cannot start.
    void reportPacingGpuSplitUnavailable(std::string_view reason, std::string_view detail) noexcept;

    struct SteamOverlayLibraries {
        bool hookLoaded{};
        bool vulkanLoaded{};
    };
    /// Inspect loaded ELF names once at diagnostic process identification.
    /// This neither loads libraries nor reads /proc or the environment.
    [[nodiscard]] SteamOverlayLibraries steamOverlayLibraries();

    /// Fixed-size private-present CPU windows, including healthy sub-threshold
    /// work. Index order is total, fence, schedule, copy, acquire, generated
    /// submit, generated present, original present, unattributed.
    class PresentPhaseTiming {
    public:
        struct Window {
            size_t calls{};
            std::array<BridgePresentTiming::Samples, 9> phases;
        };
        std::optional<Window> observe(uint64_t context, Clock::time_point finished,
                const std::array<double, 9>& milliseconds) {
            if (!started || context != owner) {
                owner = context;
                started = finished;
                window = {};
            }
            ++window.calls;
            for (size_t index = 0; index < milliseconds.size(); ++index)
                window.phases[index].add(milliseconds[index]);
            if (finished - *started < std::chrono::seconds(1))
                return {};
            const auto completed = window;
            window = {};
            started = finished;
            return completed;
        }
    private:
        uint64_t owner{};
        std::optional<Clock::time_point> started;
        Window window;
    };
    void recordPresentPhaseTiming(uint64_t context, size_t frame, size_t sequence,
        Clock::time_point finished, const std::array<double, 9>& milliseconds) noexcept;

    void logBridgeTiming(uint64_t bridgeId, VkSwapchainKHR swapchain,
        const BridgePresentTiming::Window& window,
        size_t outstanding, uint64_t refreshCycleNs,
        uint32_t timingVersion = 1, uint64_t refreshIntervalNs = 0);

    enum class PresentWaitApi { Khr, Khr2 };

    enum class ApplicationFrameApi { AcquireKhr, AcquireKhr2, Present };

    /// One F5 emulated acquire: the wait for a free private image and the wait
    /// to take the worker's queue lock. Measured only with the pacing profile.
    struct InternalAcquireTiming {
        Clock::duration ringWait{};
        Clock::duration queueLock{};
    };

    /// Bounded CPU observations at the application's layer entrypoints. These
    /// include intentional pacing, unlike the private-work present timer.
    /// A stream owns one caller/swapchain pair; switching drops its old window.
    class ApplicationFrameTiming {
    public:
        struct Window {
            size_t calls{};
            size_t successful{};
            size_t timeouts{};
            size_t notReady{};
            size_t errors{};
            size_t polls{};
            BridgePresentTiming::Samples entryInterval;
            BridgePresentTiming::Samples duration;
            BridgePresentTiming::Samples configurationUpdate;
            BridgePresentTiming::Samples deferredJoin;
            BridgePresentTiming::Samples internalRingWait;
            BridgePresentTiming::Samples internalQueueLock;
        };

        [[nodiscard]] std::optional<Window> observe(uintptr_t caller,
                VkSwapchainKHR swapchain, Clock::time_point started,
                Clock::time_point finished, std::optional<VkResult> result,
                std::optional<uint64_t> timeout,
                std::optional<Clock::duration> configurationUpdate = {},
                std::optional<Clock::duration> deferredJoin = {},
                std::optional<InternalAcquireTiming> internal = {}) {
            if (!windowStarted || caller != lastCaller || swapchain != lastSwapchain) {
                windowStarted = started;
                previousEntry.reset();
                lastCaller = caller;
                lastSwapchain = swapchain;
                window = {};
            }
            if (previousEntry)
                window.entryInterval.add(milliseconds(started - *previousEntry));
            previousEntry = started;
            ++window.calls;
            if (result) {
                window.successful += *result == VK_SUCCESS || *result == VK_SUBOPTIMAL_KHR;
                window.timeouts += *result == VK_TIMEOUT;
                window.notReady += *result == VK_NOT_READY;
                window.errors += *result < 0;
            }
            window.polls += timeout && *timeout == 0;
            window.duration.add(milliseconds(finished - started));
            if (configurationUpdate)
                window.configurationUpdate.add(milliseconds(*configurationUpdate));
            if (deferredJoin)
                window.deferredJoin.add(milliseconds(*deferredJoin));
            if (internal) {
                window.internalRingWait.add(milliseconds(internal->ringWait));
                window.internalQueueLock.add(milliseconds(internal->queueLock));
            }
            if (finished - *windowStarted < std::chrono::seconds(1))
                return std::nullopt;
            windowStarted = finished;
            const auto completed = window;
            window = {};
            return completed;
        }
    private:
        static double milliseconds(Clock::duration value) {
            return std::chrono::duration<double, std::milli>(value).count();
        }
        std::optional<Clock::time_point> windowStarted;
        std::optional<Clock::time_point> previousEntry;
        uintptr_t lastCaller{};
        VkSwapchainKHR lastSwapchain{};
        Window window;
    };

    struct ApplicationAcquireDetails {
        uint64_t callId{};
        uint64_t context{};
        size_t lowerCalls{};
        Clock::duration lowerDuration{};
        std::optional<InternalAcquireTiming> internal;
    };

    // Only the application acquire wrapper installs this scope. framegen-vulkan's own
    // generated-image acquisitions must never become application evidence.
    class ApplicationAcquireScope {
    public:
        ApplicationAcquireScope();
        ~ApplicationAcquireScope();
        ApplicationAcquireScope(const ApplicationAcquireScope&) = delete;
        ApplicationAcquireScope& operator=(const ApplicationAcquireScope&) = delete;
        [[nodiscard]] const ApplicationAcquireDetails& details() const { return observation; }
    private:
        friend void bindApplicationAcquireContext(uint64_t) noexcept;
        friend void recordApplicationAcquireDriver(uint64_t, VkResult, Clock::time_point,
            Clock::time_point, std::optional<uint32_t>) noexcept;
        friend void recordInternalAcquire(Clock::duration, Clock::duration) noexcept;
        ApplicationAcquireScope* previous{};
        bool installed{};
        ApplicationAcquireDetails observation;
    };

    void bindApplicationAcquireContext(uint64_t context) noexcept;

    /// Attach an F5 emulated acquire's split to the application acquire in progress
    /// on this thread. Does nothing outside an application acquire scope.
    void recordInternalAcquire(Clock::duration ringWait, Clock::duration queueLock) noexcept;

    void recordApplicationAcquireDriver(uint64_t timeout, VkResult result,
        Clock::time_point started, Clock::time_point finished,
        std::optional<uint32_t> imageIndex) noexcept;

    template<typename Acquire>
    VkResult observeApplicationAcquireDriver(uint64_t timeout, Acquire&& acquire,
            const uint32_t* imageIndex) {
        if (!pacingFrameTraceEnabled()) return acquire();
        const auto started = Clock::now();
        const auto result = acquire();
        const auto finished = Clock::now();
        recordApplicationAcquireDriver(timeout, result, started, finished,
            imageIndex && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
                ? std::optional<uint32_t>{*imageIndex} : std::nullopt);
        return result;
    }

    void recordApplicationFrameTiming(ApplicationFrameApi api, uintptr_t caller,
        VkSwapchainKHR swapchain, Clock::time_point started, Clock::time_point finished,
        std::optional<VkResult> result = {}, std::optional<uint64_t> timeout = {},
        std::optional<Clock::duration> configurationUpdate = {},
        std::optional<Clock::duration> deferredJoin = {},
        std::optional<uint32_t> imageIndex = {}, uint64_t context = 0, uint64_t source = 0,
        const ApplicationAcquireDetails* acquisition = nullptr);

    template<typename Acquire>
    VkResult observeApplicationAcquire(VkDevice device, VkSwapchainKHR swapchain,
            ApplicationFrameApi api, uint64_t timeout, Acquire&& acquire, const uint32_t* imageIndex = nullptr) {
        if (!enabled() && !pacingProfileEnabled())
            return acquire();
        ApplicationAcquireScope scope;
        const auto started = Clock::now();
        const auto result = acquire();
        const auto finished = Clock::now();
        try {
            recordApplicationFrameTiming(api, reinterpret_cast<uintptr_t>(device),
                swapchain, started, finished, result, timeout, {}, {},
                imageIndex && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
                    ? std::optional<uint32_t>{*imageIndex} : std::nullopt, scope.details().context, 0, &scope.details());
        } catch (...) {
            // Observing a completed acquire cannot change its returned result.
        }
        return result;
    }

    class ApplicationPresentScope {
    public:
        ApplicationPresentScope(VkQueue queue, VkSwapchainKHR swapchain, std::optional<uint32_t> imageIndex = {});
        ~ApplicationPresentScope();
        ApplicationPresentScope(const ApplicationPresentScope&) = delete;
        ApplicationPresentScope& operator=(const ApplicationPresentScope&) = delete;
        void configurationUpdateStarted();
        void configurationUpdateFinished();
        void deferredJoinStarted();
        void deferredJoinFinished();
        VkResult returned(VkResult value) noexcept {
            result = value;
            if (active) finished = Clock::now();
            return value;
        }
    private:
        friend void bindApplicationPresentSource(uint64_t, uint64_t) noexcept;
        ApplicationPresentScope* previous{};
        uint64_t context{}, source{};
        std::optional<uint32_t> imageIndex;
        bool active;
        uintptr_t caller;
        VkSwapchainKHR swapchain;
        Clock::time_point started{};
        Clock::time_point finished{};
        std::optional<VkResult> result;
        Clock::time_point updateStarted{};
        std::optional<Clock::duration> updateDuration;
        Clock::time_point joinStarted{};
        std::optional<Clock::duration> joinDuration;
    };

    // Native FG copy submission metadata, not GPU execution/completion.
    // Spans remain producer-local and are serialized before this call returns.
    struct CopySubmission {
        uint64_t context{}, source{};
        VkSwapchainKHR swapchain{};
        bool generated{};
        uint32_t imageIndex{}, pass{};
        VkDevice device{};
        VkQueue queue{};
        VkCommandBuffer command{};
        std::span<const VkSemaphore> waits{}, signals{};
        VkSemaphore timelineWait{}, timelineSignal{};
        uint64_t waitValue{}, signalValue{};
        VkFence fence{};
    };
    void recordCopySubmission(const CopySubmission& info, Clock::time_point started,
        Clock::time_point finished, bool submitted) noexcept;

    template<typename Submit>
    void observeCopySubmission(const CopySubmission& info, Submit&& submit) {
        if (!pacingFrameTraceEnabled()) { submit(); return; }
        const auto started = Clock::now();
        try { submit(); }
        catch (...) {
            recordCopySubmission(info, started, Clock::now(), false);
            throw;
        }
        recordCopySubmission(info, started, Clock::now(), true);
    }

    struct ApplicationPresentMode {
        int64_t mode{-1};
        bool dynamic{};
        bool operator==(const ApplicationPresentMode&) const = default;
    };

    [[nodiscard]] inline ApplicationPresentMode applicationPresentMode(
            VkPresentModeKHR createdMode, const void* chain) {
        for (auto* node = static_cast<const VkBaseInStructure*>(chain);
                node; node = node->pNext) {
            if (node->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT) {
                const auto* modes = reinterpret_cast<const VkSwapchainPresentModeInfoEXT*>(node);
                // A multi-swapchain call has no per-context index here. Leave
                // it explicitly unknown rather than attributing index zero.
                return {modes->swapchainCount == 1 && modes->pPresentModes
                    ? static_cast<int64_t>(*modes->pPresentModes) : -1, true};
            }
        }
        return {static_cast<int64_t>(createdMode), false};
    }

    /// One bounded stream per calling thread. Switching device, swapchain or
    /// API drops an incomplete window instead of joining unrelated lifetimes.
    /// These observations never authorize completion or change a timeout.
    class ApplicationPresentWait {
    public:
        struct Failure {
            VkResult result;
            uint64_t presentId;
            uint64_t timeout;
            double durationMs;
        };
        struct Window {
            size_t calls{};
            size_t successful{};
            size_t timeouts{};
            size_t errors{};
            size_t polls{};
            uint64_t firstPresentId{};
            uint64_t lastPresentId{};
            std::optional<Failure> firstFailure;
            BridgePresentTiming::Samples duration;
        };

        [[nodiscard]] std::optional<Window> observe(VkDevice device,
                VkSwapchainKHR swapchain, PresentWaitApi api,
                uint64_t presentId, uint64_t timeout, VkResult result,
                Clock::time_point started, Clock::time_point finished) {
            if (!windowStarted || device != lastDevice ||
                    swapchain != lastSwapchain || api != lastApi) {
                windowStarted = started;
                lastDevice = device;
                lastSwapchain = swapchain;
                lastApi = api;
                window = {};
            }
            if (!window.calls)
                window.firstPresentId = presentId;
            ++window.calls;
            window.successful += result == VK_SUCCESS;
            window.timeouts += result == VK_TIMEOUT;
            window.errors += result < 0;
            window.polls += timeout == 0;
            window.lastPresentId = presentId;
            window.duration.add(std::chrono::duration<double, std::milli>(
                finished - started).count());
            if ((result == VK_TIMEOUT || result < 0) && !window.firstFailure)
                window.firstFailure = Failure{result, presentId, timeout,
                    std::chrono::duration<double, std::milli>(finished - started).count()};
            if (finished - *windowStarted < std::chrono::seconds(1))
                return std::nullopt;
            windowStarted = finished;
            const auto completed = window;
            window = {};
            return completed;
        }
    private:
        std::optional<Clock::time_point> windowStarted;
        VkDevice lastDevice{};
        VkSwapchainKHR lastSwapchain{};
        PresentWaitApi lastApi{};
        Window window;
    };

    void recordApplicationPresentWait(VkDevice device, VkSwapchainKHR swapchain,
        PresentWaitApi api, uint64_t presentId, uint64_t timeout, VkResult result,
        Clock::time_point started, Clock::time_point finished);

    template<typename Wait>
    VkResult observeApplicationPresentWait(VkDevice device, VkSwapchainKHR swapchain,
            PresentWaitApi api, uint64_t presentId, uint64_t timeout, Wait&& wait) {
        if (!enabled() && !pacingProfileEnabled())
            return wait();
        const auto started = Clock::now();
        const auto result = wait();
        const auto finished = Clock::now();
        recordApplicationPresentWait(device, swapchain, api, presentId, timeout,
            result, started, finished);
        return result;
    }

    class ContextScope {
    public:
        explicit ContextScope(uint64_t contextId, PacingProfile* profile = nullptr);
        ~ContextScope();

        ContextScope(const ContextScope&) = delete;
        ContextScope& operator=(const ContextScope&) = delete;
    private:
        uint64_t previousContextId;
        PacingProfile* previousPacingProfile;
    };

    void logSlowOperation(std::string_view operation,
        size_t frameIndex, size_t sequenceIndex,
        Clock::time_point started,
        std::optional<VkResult> result = std::nullopt,
        std::optional<size_t> passIndex = std::nullopt,
        std::optional<uint32_t> imageIndex = std::nullopt);

    void logPresentFallback(size_t frameIndex, size_t sequenceIndex,
        size_t passIndex, size_t skippedFrames, uint64_t timelineValue,
        std::string_view acquireMode, std::string_view backendWork);

    void logHistoryWarmup(size_t frameIndex, size_t sequenceIndex,
        size_t remainingFrames, bool recovery,
        std::optional<uint32_t> acquiredImage);

    [[nodiscard]] AdaptiveSchedulerDiagnostics& adaptiveScheduler();

}

/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "bridge_present_timing.hpp"
#include "swapchain/low_latency.hpp"

#include <chrono>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <string_view>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer::present_diagnostics {

    using Clock = std::chrono::steady_clock;

    [[nodiscard]] uint64_t allocateContextId();
    [[nodiscard]] bool pacingProfileEnabled();
    [[nodiscard]] bool pacingGpuProfileEnabled();
    /// Non-draining split GPU boundaries; active only with FGVK_PACING_PROFILE=1 and FGVK_PACING_PROFILE_GPU_SPLIT=1.
    [[nodiscard]] bool pacingGpuSplitProfileEnabled();
    [[nodiscard]] bool pacingFrameTraceEnabled();
    [[nodiscard]] bool pacingWaylandProfileEnabled();
    /// Publish already-buffered, structured observer records to the same bounded sink.
    void writePacingProfileRecords(std::string_view records) noexcept;
    /// Prepare the private capture outside presentation; never capture game stdout.
    void initializePacingProfileLog() noexcept;
    /// Test hook: writes every queued pacing record before returning. Production never calls it.
    void flushPacingProfileForTests() noexcept;
    /// Records one Wayland feedback sample or lost-sample gap, keyed by the observer's token.
    /// Only the pacing profile calls these.
    enum class WaylandEvent : uint8_t { Request, Call, Presented, Discarded, Unavailable, Abandoned };
    struct WaylandSample {
        WaylandEvent event{};
        uint64_t id{}, context{}, source{};
        uint32_t image{};
        bool generated{};
        uint64_t requested{}, received{}, finished{}, presentationNow{}, calibrationSpan{};
        uint64_t presented{}, sequence{};
        uint32_t refresh{}, flags{}, output{}, clock{UINT32_MAX}, reason{};
        VkResult result{};
    };
    /// False when the sample was not queued (the record queue was full). Each returns false without the profile.
    [[nodiscard]] bool recordWaylandSample(uint64_t token, const WaylandSample& sample) noexcept;
    [[nodiscard]] bool recordWaylandGap(uint64_t token, uint64_t lost) noexcept;
    /// Cost of one display-feedback probe (its before/after/dispatch work), in nanoseconds.
    void recordDisplayProbeCost(uint64_t nanoseconds) noexcept;
    /// The display truth probe runs with the pacing profile unless FGVK_PACING_PROFILE_DISPLAY=0 (an A/B switch).
    [[nodiscard]] bool pacingDisplayProbeEnabled();
    /// The deferred worker calls this once, from its own thread, so the writer can sample its scheduling.
    void registerDeferredWorkerThread() noexcept;
    /// The lower present ID carried by a present's pNext chain (VkPresentIdKHR or VkPresentId2KHR), or 0.
    [[nodiscard]] uint64_t lowerPresentId(const void* chain) noexcept;
    /// D7: records the steady-clock time of an application present. Only with the pacing profile.
    void noteApplicationPresent();
    /// D7: nanoseconds of the last noted present on the steady clock, or zero before the first.
    [[nodiscard]] int64_t lastApplicationPresentNanoseconds();

    /// D7c: one source copy of a frame-generation timeline. The stagingWait is the generated value
    /// the staging copy waited on the timeline (zero until that copy is noted). A zero sourceValue
    /// marks a record that was never written.
    struct FgRecord {
        uint64_t source{};
        uint64_t sourceValue{};
        uint64_t stagingWait{};
    };
    /// D7c: the registry behind the FG stall probe, keyed by swapchain diagnostics context. It is
    /// a plain object so tests can drive it directly; the free functions below wrap one instance
    /// and return at once when the pacing profile is off.
    class FgTimelineLedger {
    public:
        static constexpr uint32_t recentCount = 8;
        struct Entry {
            VkDevice device{};
            VkSemaphore sync{};
            PFN_vkGetSemaphoreCounterValueKHR counter{};
            std::array<FgRecord, recentCount> recent{};
            uint32_t next{};
        };
        void noteSourceCopy(uint64_t context, VkDevice device, VkSemaphore sync,
            PFN_vkGetSemaphoreCounterValueKHR counter, uint64_t source, uint64_t sourceValue);
        void noteStagingWait(uint64_t context, uint64_t value);
        void forget(uint64_t context);
        /// The probe lines, one per entry in context order, oldest record first. The counter is
        /// read under the registry lock; nothing else is waited on or locked.
        [[nodiscard]] std::string describe(int64_t stalledNanoseconds) const;
        [[nodiscard]] size_t size() const;
    private:
        mutable std::mutex mutex;
        std::map<uint64_t, Entry> entries;
    };
    /// D7c: records a swapchain's source copy on its FG timeline. Only with the pacing profile.
    void noteFgSourceCopy(uint64_t context, VkDevice device, VkSemaphore sync,
        PFN_vkGetSemaphoreCounterValueKHR counter, uint64_t source, uint64_t sourceValue);
    /// D7c: records the generated value that the newest source record's staging copy waited.
    void noteFgStagingWait(uint64_t context, uint64_t value);
    /// D7c: drops a context's entry before its timeline semaphore is destroyed or moved away.
    void forgetFgTimeline(uint64_t context);
    /// D7c: prints one line per registered context. Called by the D7 watchdog after its dump.
    void writeFgTimelineProbe(int64_t stalledNanoseconds);
    /// D7c: holds one context's registry entry until the owning swapchain is destroyed. Declare it
    /// after the timeline semaphore, so it forgets the entry before the semaphore is destroyed.
    /// A moved-from lease holds nothing.
    class FgTimelineLease {
    public:
        FgTimelineLease() = default;
        FgTimelineLease(const FgTimelineLease&) = delete;
        FgTimelineLease& operator=(const FgTimelineLease&) = delete;
        FgTimelineLease(FgTimelineLease&& other) noexcept : context(std::exchange(other.context, 0)) {}
        FgTimelineLease& operator=(FgTimelineLease&& other) noexcept {
            if (this != &other) {
                if (context) forgetFgTimeline(context);
                context = std::exchange(other.context, 0);
            }
            return *this;
        }
        ~FgTimelineLease() { if (context) forgetFgTimeline(context); }
        /// The lease owns the entry of `contextId` until it is destroyed or replaced.
        void hold(uint64_t contextId) { context = contextId; }
    private:
        uint64_t context{};
    };
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
        static constexpr size_t PresentModeSlotCount = 7;
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
            // Display identity: the Wayland feedback id of the lower present (0 if none), the backend
            // frame the output belongs to, its interpolation position (0 for a real), and the lower present ID.
            uint64_t feedback{};
            uint64_t backendFrame{};
            double phase{};
            uint64_t presentId{};
            // S1: first output only; slack is the signed deadline-minus-readiness
            // before the shift.
            Clock::time_point readyAt{};
            Clock::duration slack{};
            // W2: the effective pacing correction of a generated output; zero otherwise.
            Clock::duration offset{};
            // GP1: the application's own present ID for a real output (0 otherwise). present_id above
            // is the lower ID the output was presented with.
            uint64_t appPresentId{};
            // GT1: the tick snap (snapped deadline minus the deadline before the snap) and the target tick
            // (zero when the output was not snapped).
            Clock::duration tickSnap{};
            Clock::time_point targetTick{};
        };
        struct OutputSample {
            size_t ordinal{};
            uint64_t source{};
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
            bool frameGeneration{}, adaptive{};
            size_t multiplier{};
            double cap{}, configuredCap{};
        };
        [[nodiscard]] SourceSample sourceSample(Clock::time_point finished) const {
            return {batch, backendEpoch, backendFrame, hasBackendFrame,
                sourceStarted.value_or(Clock::time_point{}), finished, waits, lastPhases,
                window.frameGenerationEnabled, window.adaptive,
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
            // D6: the worker's longest call under the queue lock, per output, by kind.
            Samples workerLockAcquire, workerLockSubmit, workerLockPresent;
            // F9b: presented internal reals, by whether a lower image was prepared before the deadline.
            size_t internalRealPrepared{}, internalRealAtDeadline{};
            size_t generatedPrepared{}, generatedAtDeadline{};
            bool internalImages{};
            size_t fifoCallerTimingSources{};
            std::array<OutputSample, outputTraceCapacity> outputTrace{};
            size_t outputTraceCount{}, outputTraceDropped{};
            Clock::duration elapsed{};
            double effectiveCapFps{};
            bool frameGenerationEnabled{}, adaptive{};
            size_t configuredMultiplier{};
            double configuredCapFps{};
            // Per-present VkSwapchainPresentModeInfoEXT mode, by present
            // kind (slots in presentModeSlotNames), and the lower swapchain's
            // create-time mode, which applies to presents without one.
            std::array<size_t, PresentModeSlotCount> applicationModes{}, realModes{}, generatedModes{};
            int64_t swapchainPresentMode{-1};
            // low_latency: whether the setting was on, the release delay of each capped source,
            // and the release-token kinds the application has used (ReleaseGate::kindBit, the
            // acquire bit, and bit 4 for a window where Reflex stepped the release delay aside).
            bool lowLatency{};
            Samples lowLatencyExtra;
            uint32_t lowLatencyKinds{};
            // Released batches start at their own readiness (the setting, as for low_latency).
            bool justInTime{};
            // batch_hold: the profile's batch_hold setting at the window's latest present (1: the 2 ms hold).
            bool batchHold{true};
            // L3, BH2: each held batch's start minus its readiness.
            Samples batchPhaseHold;
            // L5: the GPU release mode of the window's latest capped present, the hold its release
            // token carried when a predicted release was armed (or the stepping hold), and the sum
            // and count of resolved previous-batch-end leads (previous end minus arrival, ms).
            GpuReleaseMode gpuReleaseMode{GpuReleaseMode::Off};
            Samples gpuReleaseHold;
            double gpuPrevEndSumMs{};
            size_t gpuPrevEndCount{};
        };
        static constexpr std::array<std::string_view, PresentModeSlotCount> presentModeSlotNames{
            "none", "immediate", "mailbox", "fifo", "fifo_relaxed", "fifo_latest_ready", "other"};
        // -1 means the present carried no per-present mode.
        [[nodiscard]] static constexpr size_t presentModeSlot(int64_t mode) {
            switch (mode) {
            case -1: return 0;
            case VK_PRESENT_MODE_IMMEDIATE_KHR: return 1;
            case VK_PRESENT_MODE_MAILBOX_KHR: return 2;
            case VK_PRESENT_MODE_FIFO_KHR: return 3;
            case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return 4;
            case 1000361000: return 5; // VK_PRESENT_MODE_FIFO_LATEST_READY_KHR
            default: return 6;
            }
        }
        // One call per application present: the mode in the caller's chain
        // and in the chain framegen-vulkan forwards for the real image.
        void presentModes(int64_t application, int64_t real, VkPresentModeKHR swapchainMode) {
            ++window.applicationModes[presentModeSlot(application)];
            ++window.realModes[presentModeSlot(real)];
            window.swapchainPresentMode = static_cast<int64_t>(swapchainMode);
        }
        // One call per generated output handed to a lower present.
        void generatedPresentMode(int64_t mode) { ++window.generatedModes[presentModeSlot(mode)]; }
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
        void policy(double cap, bool fifoCallerTiming = false) {
            window.effectiveCapFps = cap;
            window.fifoCallerTimingSources += fifoCallerTiming;
        }
        void settings(bool frameGeneration, bool adaptive, size_t multiplier, double configuredCap) {
            window.frameGenerationEnabled = frameGeneration;
            window.adaptive = adaptive;
            window.configuredMultiplier = multiplier;
            window.configuredCapFps = configuredCap;
        }
        // One call per present; delay samples only the capped sources the setting governs.
        void lowLatency(bool enabled, Clock::duration delay, uint32_t kinds, bool justInTime, bool batchHold) {
            window.lowLatency = enabled;
            if (enabled) window.lowLatencyExtra.add(delay);
            window.lowLatencyKinds = kinds;
            window.justInTime = justInTime;
            window.batchHold = batchHold;
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
        // L5, one call per present: its GPU release mode, and the hold of its token when one was armed.
        void gpuRelease(GpuReleaseMode mode, std::optional<Clock::duration> hold) {
            window.gpuReleaseMode = mode;
            if (hold) window.gpuReleaseHold.add(*hold);
        }
        // L5: one resolved previous-batch-end lead (the batch end minus the frame's arrival).
        void gpuPrevEnd(Clock::duration lead) {
            window.gpuPrevEndSumMs += std::chrono::duration<double, std::milli>(lead).count();
            ++window.gpuPrevEndCount;
        }
        // L3, BH2: one held batch that became ready, and how far its phase-locked start came after that.
        void justInTimeBatch(Clock::duration hold) { window.batchPhaseHold.add(hold); }
        void generatedSkipped(GeneratedSkip kind) { ++window.generatedSkips[kind]; }
        void lateReadinessBatch() { ++window.lateReadinessBatches; }
        void generatedCompressedPairs(size_t pairs) { window.generatedCompressed += pairs; }
        // D6: one output's longest worker calls under the queue lock, in nanoseconds. A bucket
        // the output did not use (zero) is not added, so each mean is over the calls that used it.
        void workerLockSplit(uint64_t acquireNs, uint64_t submitNs, uint64_t presentNs) {
            if (acquireNs) window.workerLockAcquire.addMilliseconds(static_cast<double>(acquireNs) / 1e6);
            if (submitNs) window.workerLockSubmit.addMilliseconds(static_cast<double>(submitNs) / 1e6);
            if (presentNs) window.workerLockPresent.addMilliseconds(static_cast<double>(presentNs) / 1e6);
        }
        void internalRealPresented(bool prepared) {
            if (prepared) ++window.internalRealPrepared;
            else ++window.internalRealAtDeadline;
        }
        void generatedPresented(bool prepared) {
            if (prepared) ++window.generatedPrepared;
            else ++window.generatedAtDeadline;
        }
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
    /// Copy-side split of one generated output's worker copy, in CLOCK_MONOTONIC nanoseconds.
    /// available is false when no copy timestamps were read (a skipped output, or uncalibrated);
    /// both times are then zero.
    void logPacingOutputGpu(uint64_t context, uint64_t source, bool available,
        int64_t copyStartNs, int64_t copyEndNs) noexcept;
    /// T3b: the same split for the private -> lower copy of a real output (internal images),
    /// FG on or off. With FG off its start follows the game's present waits.
    void logPacingRealCopyGpu(uint64_t context, uint64_t source, bool available,
        int64_t copyStartNs, int64_t copyEndNs) noexcept;
    /// T4: the private queue's GPU marker submitted right after a lower present, joined by its
    /// feedback id. Unavailable markers keep no times.
    void logPacingPresentMarkerGpu(uint64_t context, uint64_t source, uint64_t feedback, bool generated,
        bool available, int64_t markerStartNs, int64_t markerEndNs) noexcept;
    /// Diagnostic: one line per process for the first split step that cannot start.
    void reportPacingGpuSplitUnavailable(std::string_view reason, std::string_view detail) noexcept;

    struct SteamOverlayLibraries {
        bool hookLoaded{};
        bool vulkanLoaded{};
    };
    /// Inspect loaded ELF names once at diagnostic process identification.
    /// This neither loads libraries nor reads /proc or the environment.
    [[nodiscard]] SteamOverlayLibraries steamOverlayLibraries();

    enum class PresentWaitApi { Khr, Khr2 };

    enum class ApplicationFrameApi { AcquireKhr, AcquireKhr2, Present };

    /// Layer entrypoints timed with the pacing profile. The order is fixed; the names are the record text.
    enum class LayerCall : uint8_t {
        QueuePresent, AcquireNextImage, AcquireNextImage2, QueueSubmit2, WaitSemaphores,
        QueueWaitIdle, DeviceWaitIdle, WaitForPresent, WaitForPresent2, ReleaseSwapchainImages,
        GetSwapchainImages, LatencySleep, Count
    };
    /// One layer call on the calling thread. With the pacing profile off it reads no clock and
    /// touches no thread-local state. Nested layer calls count once, in the outermost span.
    /// A present span also emits the application-frame record that splits the gap before it.
    class LayerCallSpan {
    public:
        explicit LayerCallSpan(LayerCall call) noexcept;
        ~LayerCallSpan();
        LayerCallSpan(const LayerCallSpan&) = delete;
        LayerCallSpan& operator=(const LayerCallSpan&) = delete;
        VkResult returned(VkResult value) noexcept { result = value; return value; }
        void presentWait(uint64_t presentId, uint64_t timeoutNs) noexcept {
            if (active) { this->presentId = presentId; this->timeoutNs = timeoutNs; }
        }
        void semaphoreWait(uint64_t count, uint64_t timeoutNs) noexcept {
            if (active) { waitCount = count; this->timeoutNs = timeoutNs; }
        }
    private:
        LayerCall call;
        bool active{};
        Clock::time_point enter{};
        VkResult result{VK_SUCCESS};
        uint64_t presentId{}, timeoutNs{}, waitCount{};
        bool outermost{};
        // Present spans: the inside-layer sums since the previous present exit, read at entry.
        std::array<int64_t, 4> inside{};
        Clock::time_point previousExit{};
    };
    /// Reflex markers and mode calls, recorded only with the pacing profile.
    void recordReflexMarker(uint64_t presentId, VkLatencyMarkerNV marker, Clock::time_point at) noexcept;
    void recordReflexSleep(Clock::time_point entered, Clock::time_point exited, VkResult result) noexcept;
    void recordReflexMode(VkBool32 lowLatency, VkBool32 boost, uint32_t minimumIntervalUs, VkResult result) noexcept;

    /// Bounded CPU observations at the application's present entrypoints. These
    /// include intentional pacing, unlike the private-work present timer.
    /// A stream owns one caller/swapchain pair; switching drops its old window.
    class ApplicationFrameTiming {
    public:
        struct Window {
            size_t calls{};
            BridgePresentTiming::Samples entryInterval;
            BridgePresentTiming::Samples duration;
            BridgePresentTiming::Samples configurationUpdate;
            BridgePresentTiming::Samples deferredJoin;
        };

        [[nodiscard]] std::optional<Window> observe(uintptr_t caller,
                VkSwapchainKHR swapchain, Clock::time_point started,
                Clock::time_point finished,
                std::optional<Clock::duration> configurationUpdate = {},
                std::optional<Clock::duration> deferredJoin = {}) {
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
            window.duration.add(milliseconds(finished - started));
            if (configurationUpdate)
                window.configurationUpdate.add(milliseconds(*configurationUpdate));
            if (deferredJoin)
                window.deferredJoin.add(milliseconds(*deferredJoin));
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
        ApplicationAcquireScope* previous{};
        bool installed{};
        ApplicationAcquireDetails observation;
    };

    void bindApplicationAcquireContext(uint64_t context) noexcept;

    /// F10 per-acquire record, pacing profile only: one line per application acquire of the internal
    /// images. Times are steady-clock; zero means not measured. tailSeen is when this acquire read the
    /// tail counter; tailWaitValue is the tail value the host path waited on (0 on the queued path).
    void recordPacingAcquire(bool host, Clock::time_point enter, Clock::time_point exit,
        Clock::duration lockWait, uint64_t tailWaitValue, Clock::time_point tailSeen) noexcept;

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
        // Only the frame trace records an application acquire now; the pacing profile keeps no
        // acquire window, so an untraced acquire reads no clock and installs no scope.
        if (!pacingFrameTraceEnabled())
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
        if (!pacingProfileEnabled())
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

}

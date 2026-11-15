/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "generated_frame_plan.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace fgvk::layer {

    /// Frames of temporal history that must be rebuilt before generated
    /// output resumes after a warm-up request.
    inline constexpr size_t historyWarmupFrameCount = 3;

    /// Ordinary readiness checks preserve progress. A new interruption must
    /// discard partial pre-interruption history and start a complete warm-up.
    [[nodiscard]] constexpr size_t historyWarmupFramesAfterRequest(
            const size_t remaining, const size_t required,
            const bool restart) noexcept {
        return restart || remaining == 0 ? required : remaining;
    }

    /// framegen-vulkan's current presentation transport owns one swapchain per lower
    /// submit/present sequence. A Vulkan present batch supplies its binary
    /// waits and per-swapchain pNext arrays once for the whole batch, so it
    /// cannot be decomposed safely until an explicit fan-out transport owns
    /// those semantics.
    [[nodiscard]] constexpr bool shouldRejectManagedMultiSwapchainPresent(
            const uint32_t swapchainCount,
            const bool containsManagedFgvkContext) noexcept {
        return swapchainCount > 1 && containsManagedFgvkContext;
    }

    [[nodiscard]] inline bool environmentFlagEnabled(const char* value) {
        if (!value)
            return false;
        const std::string_view flag(value);
        return flag == "1" || flag == "true" ||
            flag == "yes" || flag == "on";
    }

    /// The real frame already belongs to the application's minimum. Ordered
    /// delivery acquires and presents generated images one at a time, so a
    /// complete generated batch does not require another unused relief image.
    /// Only an undersized pool needs opportunistic admission before backend
    /// work. A pool that just fits uses the bounded acquire budget below.
    [[nodiscard]] inline bool orderedGeneratedBatchNeedsNonblockingAdmission(
            const uint32_t requestedMinImages,
            const size_t returnedImages,
            const size_t requestedGeneratedFrames) noexcept {
        if (requestedGeneratedFrames == 0)
            return false;
        if (returnedImages < requestedMinImages)
            return true;
        return returnedImages - requestedMinImages <
            requestedGeneratedFrames;
    }

    /// Moving preparation into source-cap slack must not move a zero-wait
    /// admission attempt ahead of the image-release opportunity it previously
    /// had. Check the active policy's full capacity because planning can
    /// qualify a higher generated count after this decision.
    [[nodiscard]] inline bool orderedGeneratedBatchCanPrepareBeforeCap(
            const uint32_t requestedMinImages,
            const size_t returnedImages,
            const size_t maximumGeneratedFrames,
            const bool admissionUnderPressure) noexcept {
        return maximumGeneratedFrames > 0 && !admissionUnderPressure &&
            !orderedGeneratedBatchNeedsNonblockingAdmission(
                requestedMinImages, returnedImages, maximumGeneratedFrames);
    }

    /// Without a relief image, FIFO may need to release an earlier present
    /// before the next output can be acquired. Allow that progress, but never
    /// introduce an unbounded wait on this path, even for standalone launches.
    /// Preserve smaller user ceilings and share the budget across the batch;
    /// the existing per-image deadline and recovery still apply.
    [[nodiscard]] inline std::optional<uint64_t>
    orderedGeneratedBatchAcquireBudget(
            const uint32_t requestedMinImages,
            const size_t returnedImages,
            const size_t requestedGeneratedFrames,
            const std::optional<uint64_t> configuredBudget) noexcept {
        if (requestedGeneratedFrames == 0 ||
                returnedImages < requestedMinImages ||
                returnedImages - requestedMinImages != requestedGeneratedFrames)
            return configuredBudget;
        constexpr uint64_t maximumBatchBudget = 50'000'000;
        return std::min(configuredBudget.value_or(maximumBatchBudget),
            maximumBatchBudget);
    }


    /// Ordered SDR owns one configured acquire-wait budget per application
    /// present, not one full wait for every generated image. Returning zero
    /// means the caller must stop acquiring and retain the real frame; it must
    /// not issue another zero-timeout acquire on the ordered path.
    [[nodiscard]] inline std::optional<uint64_t>
    remainingGeneratedImageAcquireBudget(
            const std::optional<uint64_t> configuredBudget,
            const uint64_t consumedNanoseconds) {
        if (!configuredBudget)
            return std::nullopt;
        if (consumedNanoseconds >= *configuredBudget)
            return 0;
        return *configuredBudget - consumedNanoseconds;
    }

    /// A recovery probe owns one image. An explicit configured acquire budget
    /// remains authoritative; otherwise one confirmed display period bounds
    /// the retry. Without either contract the retry stays nonblocking instead
    /// of guessing a device-independent timeout.
    [[nodiscard]] inline uint64_t orderedRecoveryAcquireTimeout(
            const std::optional<uint32_t> refreshHz,
            const std::optional<uint64_t> configuredTimeout) {
        if (configuredTimeout)
            return *configuredTimeout;
        if (!refreshHz || *refreshHz == 0)
            return 0;

        constexpr uint64_t nanosecondsPerSecond = 1'000'000'000;
        return (nanosecondsPerSecond + *refreshHz - 1) / *refreshHz;
    }

    /// Recovery can probe only when the current policy requests generated work.
    /// A display-budget real-only frame is not a failed acquisition
    /// and must leave the pending probe available for the next eligible frame.
    [[nodiscard]] constexpr bool orderedAcquireProbeEligible(
            const bool probeRequested, const bool historyWarmupActive,
            const size_t requestedGeneratedFrames) noexcept {
        return probeRequested && !historyWarmupActive &&
            requestedGeneratedFrames > 0;
    }

    /// Once a lower-swapchain image has been acquired, transport ownership is
    /// independent of HDR classification. A caught backend failure must retire
    /// every owned image before the application's original image can be
    /// presented natively.
    [[nodiscard]] inline bool preacquiredImagesRequireRetirement(
            const bool generatedImagesPreacquired,
            const size_t admittedGeneratedFrameCount) {
        return generatedImagesPreacquired && admittedGeneratedFrameCount > 0;
    }

    struct GeneratedImageAdmissionRecovery {
        bool resumed{false};
        size_t missedAttempts{0};
        size_t bypassedFrames{0};
        size_t stableBatches{0};
        size_t requiredStableBatches{0};
    };

    /// A successful generated batch presents every generated image plus the
    /// application's real image. Require enough consecutive full batches to
    /// account for one complete lower-swapchain turnover before re-arming a
    /// bounded acquire. This is derived from owned WSI capacity rather than a
    /// refresh-rate, source-rate, or device-specific timer.
    [[nodiscard]] constexpr size_t
    generatedImageAdmissionRecoveryBatches(
            const size_t swapchainImages,
            const size_t requestedGeneratedFrames) noexcept {
        if (swapchainImages == 0 || requestedGeneratedFrames == 0)
            return 1;
        const size_t imagesPresentedPerBatch = requestedGeneratedFrames + 1;
        return std::max<size_t>(
            1,
            (swapchainImages + imagesPresentedPerBatch - 1) /
                imagesPresentedPerBatch
        );
    }

    /// Tracks temporary generated-swapchain pressure without turning it into
    /// an engine or temporal-history failure. Headroom-tight
    /// ordered SDR admission are nonblocking; this state exists only to
    /// aggregate diagnostics and report the eventual recovery.
    class GeneratedImageAdmission {
    public:
        [[nodiscard]] bool underPressure() const {
            return this->pressure;
        }

        /// Record an unavailable image. Returns true for the first miss and
        /// then at power-of-two intervals, allowing diagnostics to remain
        /// useful without synchronously logging every real frame.
        [[nodiscard]] bool reportUnavailable() {
            if (!this->pressure) {
                this->pressure = true;
                this->missedAttempts = 1;
                this->bypassedFrames = 0;
                this->stableBatches = 0;
                return true;
            }

            this->missedAttempts++;
            this->stableBatches = 0;
            return (this->missedAttempts & (this->missedAttempts - 1)) == 0;
        }

        void reportBypassedFrame() {
            if (this->pressure)
                this->bypassedFrames++;
        }

        [[nodiscard]] GeneratedImageAdmissionRecovery reportAvailable(
                const size_t requiredStableBatches = 1) {
            if (!this->pressure)
                return {};
            this->stableBatches++;
            const size_t required = std::max<size_t>(
                1, requiredStableBatches
            );
            const GeneratedImageAdmissionRecovery recovery{
                .resumed = this->stableBatches >= required,
                .missedAttempts = this->missedAttempts,
                .bypassedFrames = this->bypassedFrames,
                .stableBatches = this->stableBatches,
                .requiredStableBatches = required,
            };
            if (recovery.resumed)
                this->reset();
            return recovery;
        }

        void reset() {
            this->pressure = false;
            this->missedAttempts = 0;
            this->bypassedFrames = 0;
            this->stableBatches = 0;
        }

    private:
        bool pressure{false};
        size_t missedAttempts{0};
        size_t bypassedFrames{0};
        size_t stableBatches{0};
    };

    /// Recovers the ordered SDR transport only from explicit Vulkan acquire
    /// failure. Successful calls are never reclassified from their duration,
    /// FPS, device, resolution, or workload. An explicit menu return or live
    /// generation-policy transition may authorize one recreation if the next
    /// generated-image transport actually fails.
    class OrderedAcquireRecovery {
    public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;
        using Duration = Clock::duration;

        struct PresentDecision {
            bool bypassGeneration{false};
            bool beginHistoryWarmup{false};
            // The first generated present after the drain is deliberately a
            // single-image transport probe, never the normal 3x/4x/5x plan.
            bool limitGeneratedFrames{false};
            // A post-failure recovery probe receives one small display-
            // relative timeout instead of a whole multi-image budget.
            bool preacquireGeneratedFrame{false};
            bool boundedAcquireProbe{false};
            size_t bypassedFrames{0};
            size_t consecutiveFailures{0};
            Duration drainDuration{};
        };

        struct Observation {
            bool quarantined{false};
            bool recovered{false};
            bool timedOut{false};
            bool deadlineExceeded{false};
            size_t consecutiveFailures{0};
            size_t bypassedFrames{0};
            Duration retryDelay{};
            Duration recoveryDuration{};
        };

        struct NonblockingMissObservation {
            bool diagnostic{false};
            bool quarantined{false};
            bool boundedProbeFailed{false};
            size_t consecutiveFailures{0};
            size_t bypassedFrames{0};
            Duration retryDelay{};
            Duration recoveryDuration{};
        };

        [[nodiscard]] static constexpr auto maximumRetryDelay() {
            return std::chrono::milliseconds{30000};
        }

        [[nodiscard]] PresentDecision beforePresent(const TimePoint now) {
            if (!this->retryAt) {
                if (this->probePending) {
                    return {
                        .limitGeneratedFrames = true,
                        .preacquireGeneratedFrame = true,
                        .boundedAcquireProbe = true,
                        .consecutiveFailures = this->consecutiveFailures,
                    };
                }
                return {};
            }

            if (now < *this->retryAt) {
                this->bypassedFrames++;
                return {
                    .bypassGeneration = true,
                    .bypassedFrames = this->bypassedFrames,
                    .consecutiveFailures = this->consecutiveFailures,
                    .drainDuration = this->recoveryStartedAt
                        ? now - *this->recoveryStartedAt
                        : Duration{},
                };
            }

            this->retryAt.reset();
            this->probePending = true;
            return {
                .beginHistoryWarmup = true,
                .limitGeneratedFrames = true,
                .preacquireGeneratedFrame = true,
                .boundedAcquireProbe = true,
                .bypassedFrames = this->bypassedFrames,
                .consecutiveFailures = this->consecutiveFailures,
                .drainDuration = this->recoveryStartedAt
                    ? now - *this->recoveryStartedAt
                    : Duration{},
            };
        }

        [[nodiscard]] Observation observe(const TimePoint now,
                const bool timedOut,
                const bool deadlineExceeded = false,
                const bool boundedRecoveryProbe = false) {
            if (timedOut || deadlineExceeded) {
                if (this->transitionRecreationArmed)
                    this->transitionTransportFailed = true;
                return this->beginNativeDrain(
                    now, timedOut, deadlineExceeded
                );
            }

            const bool drainProbeRecovered = this->probePending;
            if (drainProbeRecovered) {
                this->probePending = false;
                this->retryAt.reset();
                this->recoveryStartedAt.reset();
                this->consecutiveFailures = 0;
                this->bypassedFrames = 0;
            }
            if (this->transitionRecreationArmed &&
                    !this->transitionTransportFailed) {
                this->transitionRecreationArmed = false;
            }

            return {
                .recovered = drainProbeRecovered,
                .consecutiveFailures = this->consecutiveFailures,
                .bypassedFrames = this->bypassedFrames,
            };
        }

        [[nodiscard]] bool active() const {
            return this->retryAt.has_value() || this->probePending;
        }

        void armTransitionRecreation() {
            this->transitionRecreationArmed = true;
            this->transitionTransportFailed = false;
        }

        [[nodiscard]] bool transitionRecoveryActive() const {
            return this->transitionRecreationArmed;
        }

        [[nodiscard]] bool signalRecreation() {
            if (!this->transitionRecreationArmed ||
                    !this->transitionTransportFailed ||
                    this->recreationSignaled) {
                return false;
            }
            this->transitionRecreationArmed = false;
            this->transitionTransportFailed = false;
            this->recreationSignaled = true;
            return true;
        }

        /// A bounded post-failure probe miss returns to native backoff;
        /// probePending can therefore never become a permanent real-only
        /// state.
        [[nodiscard]] NonblockingMissObservation
        reportNonblockingProbeUnavailable(
                const TimePoint now) {
            if (this->probePending) {
                this->bypassedFrames++;
                if (this->transitionRecreationArmed)
                    this->transitionTransportFailed = true;
                const auto observation = this->beginNativeDrain(
                    now, true, false
                );
                return {
                    .diagnostic = true,
                    .quarantined = true,
                    .boundedProbeFailed = true,
                    .consecutiveFailures =
                        observation.consecutiveFailures,
                    .bypassedFrames = observation.bypassedFrames,
                    .retryDelay = observation.retryDelay,
                    .recoveryDuration = observation.recoveryDuration,
                };
            }

            // Normal presentation does not call this without a guard or probe.
            // Keep accidental calls observable without creating recovery state.
            this->bypassedFrames++;
            return {
                .diagnostic = true,
                .consecutiveFailures = this->consecutiveFailures,
                .bypassedFrames = this->bypassedFrames,
                .recoveryDuration = this->recoveryStartedAt
                    ? now - *this->recoveryStartedAt
                    : Duration{},
            };
        }

        void pauseForExternalInterruption() {
            // Steam-owned presentation is a discontinuity, not a transport
            // failure. Discard any incomplete retry before the return event
            // arms a fresh one-shot permission.
            this->reset();
        }

        void reset() {
            this->retryAt.reset();
            this->recoveryStartedAt.reset();
            this->probePending = false;
            this->consecutiveFailures = 0;
            this->bypassedFrames = 0;
            this->transitionRecreationArmed = false;
            this->transitionTransportFailed = false;
        }

    private:
        [[nodiscard]] Observation beginNativeDrain(const TimePoint now,
                const bool timedOut, const bool deadlineExceeded) {
            if (!this->recoveryStartedAt)
                this->recoveryStartedAt = now;
            this->consecutiveFailures++;
            const auto delay = retryDelayForFailure(
                this->consecutiveFailures
            );
            this->retryAt = now + delay;
            this->probePending = false;
            return {
                .quarantined = true,
                .timedOut = timedOut,
                .deadlineExceeded = deadlineExceeded,
                .consecutiveFailures = this->consecutiveFailures,
                .bypassedFrames = this->bypassedFrames,
                .retryDelay = delay,
                .recoveryDuration = now - *this->recoveryStartedAt,
            };
        }

        [[nodiscard]] static Duration retryDelayForFailure(
                const size_t failures) {
            constexpr std::array delays{
                std::chrono::milliseconds{250},
                std::chrono::milliseconds{500},
                std::chrono::milliseconds{1000},
                std::chrono::milliseconds{2000},
                std::chrono::milliseconds{5000},
                std::chrono::milliseconds{15000},
                maximumRetryDelay(),
            };
            return delays.at(std::min(failures, delays.size()) - 1);
        }

        std::optional<TimePoint> retryAt;
        std::optional<TimePoint> recoveryStartedAt;
        bool probePending{false};
        size_t consecutiveFailures{0};
        size_t bypassedFrames{0};
        bool transitionRecreationArmed{false};
        bool transitionTransportFailed{false};
        bool recreationSignaled{false};
    };

    /// A generated image is optional, so framegen-vulkan never waits without bound for
    /// one. A hidden or destroyed window can stop FIFO image release
    /// indefinitely, and an unbounded wait on the application's present thread
    /// hangs the game (VKD3D-Proton presents on its D3D12 queue thread).
    inline constexpr uint64_t generatedImageAcquireSafetyTimeoutNs = 100'000'000;

    /// The configured application-present budget is the transport contract.
    /// Do not derive a second per-image failure threshold from refresh rate or
    /// measured duration; the remaining cumulative budget is authoritative.
    [[nodiscard]] inline uint64_t orderedGeneratedImageAcquireTimeout(
            const std::optional<uint32_t>,
            const std::optional<uint64_t> remainingBudget) {
        return remainingBudget.value_or(generatedImageAcquireSafetyTimeoutNs);
    }

    /// Spaces ordered compositor commits without a lower Wayland FIFO wait.
    /// The isolated bridge learns readiness from its own compositor feedback,
    /// never from source-frame intervals (which include application waits).
    /// Legacy WSI, without queue-stage observations, retains its fixed batch lead.
    /// Deadlines remain ordered across changes and submission stays bounded.
    class OrderedPresentTimeline {
    public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;
        struct Slot {
            TimePoint submitAt;
            TimePoint presentAt;
        };

        /// Clock changes invalidate learned work without moving output targets.
        void invalidateReadiness() { this->resetReadiness(); }

        /// A lost feedback transport returns to the fixed legacy lead. Clearing
        /// samples alone would retain observed-mode startup lead indefinitely.
        void useUnobservedTiming() {
            this->resetReadiness();
            this->lastBatchSerial.reset();
            this->lastBatchAt.reset();
        }

        [[nodiscard]] static bool validRate(const double fps) noexcept {
            return std::isfinite(fps) && fps >= 1.0 && fps <= 1000.0;
        }

        /// Identify one generated batch. A pause retires old feedback and
        /// primes recovery once; native history warm-up must not prime a
        /// second startup delay when the known generated batch resumes.
        void observeSourceBatch(const TimePoint now, const uint64_t serial) {
            if (this->lastBatchSerial == serial)
                return;
            if (this->lastBatchAt) {
                if (now <= *this->lastBatchAt)
                    this->resetReadiness();
                else if (now - *this->lastBatchAt > std::chrono::seconds(1)) {
                    this->clearPending();
                    if (!this->primeRecovery) {
                        this->recoveryReadiness = this->readinessCount
                            ? std::optional{*std::max_element(this->readiness.begin(),
                                this->readiness.begin() + this->readinessCount)}
                            : std::nullopt;
                    }
                    this->primeRecovery = true;
                }
            }
            this->lastBatchSerial = serial;
            this->lastBatchAt = now;
            this->batchIndex = (this->batchIndex + 1) % this->pending.size();
            // V2 reports individual drops, possibly before earlier outputs.
            // Retire forgotten entries individually when the bounded ring wraps.
            const auto& expired = this->pending[this->batchIndex];
            if (expired.staged)
                this->completedOutputs += expired.count - expired.acknowledged;
            this->pending[this->batchIndex] = {};
            this->queuedOutputs = this->submittedOutputs - this->completedOutputs;
        }

        /// Record only this owner's protocol IDs, after submission backpressure.
        /// Fixed storage also bounds sessions with absent/dropped feedback.
        void submitted(const uint32_t id, const TimePoint desired,
                const TimePoint submittedAt) {
            this->recordSubmission(id, desired, submittedAt, false);
        }

        /// Timing-v2 serials identify this swapchain's outputs without a
        /// wrapping 32-bit namespace or legacy broadcast feedback.
        void submittedTiming(const uint64_t serial, const TimePoint desired,
                const TimePoint submittedAt) {
            this->recordSubmission(serial, desired, submittedAt, true);
        }

        /// Queue completion is observed before the compositor's requested-time
        /// hold. Learn only that stage, not latch/feedback latency or a nominal
        /// FIFO duration. Zero queueEnd includes explicit drops and unavailable
        /// timestamps: acknowledge the output without learning fabricated work.
        void feedbackTiming(const uint64_t serial, const TimePoint queueEnd,
                const TimePoint receivedAt) {
            for (auto& batch : this->pending) {
                const auto index = serial - batch.firstId;
                if (!batch.staged || index >= batch.count || batch.received[index])
                    continue;
                batch.received[index] = true;
                ++batch.acknowledged;
                ++this->completedOutputs;
                const auto submittedAt = batch.outputSubmittedAt[index];
                if (batch.observed || queueEnd <= TimePoint{} ||
                        queueEnd < submittedAt || queueEnd > receivedAt ||
                        queueEnd - submittedAt > maximumReadiness)
                    return;
                batch.observed = true;
                this->recordReadiness(queueEnd - submittedAt);
                return;
            }
        }

    private:
        void recordSubmission(const uint64_t id, const TimePoint desired,
                const TimePoint submittedAt, const bool staged) {
            if (!this->lastBatchSerial || !this->generationEnabled)
                return;
            auto& batch = this->pending[this->batchIndex];
            if (batch.count == batch.desired.size())
                return;
            if (batch.count == 0) {
                batch.firstId = id;
                batch.staged = staged;
                batch.firstSubmission = this->submittedOutputs + 1;
                batch.lead = this->scheduledLead;
                batch.refreshPeriod = this->scheduledRefreshPeriod;
                // FIFO eligibility can include slots belonging to earlier
                // batches. Those slots already constrain the next deadline;
                // they are not additional GPU work for this batch.
                batch.queuedDuration = this->scheduledInterval * static_cast<int64_t>(
                    std::min<uint64_t>(this->submittedOutputs - this->completedOutputs,
                        GeneratedFramePlan::capacity + 1));
                batch.submittedAt = submittedAt;
            } else if (staged != batch.staged || id != (staged
                    ? batch.firstId + batch.count
                    : static_cast<uint32_t>(batch.firstId + batch.count))) {
                return;
            }
            batch.outputSubmittedAt[batch.count] = submittedAt;
            batch.desired[batch.count++] = desired;
            ++this->submittedOutputs;
        }

        void recordReadiness(const Clock::duration sample) {
            this->readiness[this->readinessIndex] = sample;
            this->readinessIndex = (this->readinessIndex + 1) % this->readiness.size();
            this->readinessCount = std::min(this->readinessCount + 1,
                this->readiness.size());
        }

    public:

        /// The compositor reports when a ready output was first eligible to latch,
        /// separately from the requested hold. Correct the lead
        /// by that difference: using submission-to-feedback elapsed time would
        /// count our already queued output spacing as new GPU work.
        /// Use the first feedback received for each batch. If an earlier output
        /// was dropped, a later output still supplies readiness evidence for
        /// the batch's first deadline; later outputs cannot shrink its budget.
        void feedback(const uint32_t id, const TimePoint desired,
                const TimePoint earliest, const Clock::duration margin = {}) {
            for (auto& batch : this->pending) {
                const auto index = static_cast<uint32_t>(id - batch.firstId);
                if (batch.staged || index >= batch.count || batch.desired[index] != desired)
                    continue;
                // Completion is acknowledged at the compositor latch, which
                // can precede the predicted display timestamp. Every matched
                // output advances completion, even after this batch supplied
                // its readiness sample or if optional readiness is absent.
                // An acknowledged ordered output retires its prefix, including
                // dropped outputs. Use owned ordinals, not wrapping protocol IDs
                // or the future display timestamp, to measure queue depth.
                this->completedOutputs = std::max(this->completedOutputs,
                    batch.firstSubmission + index);
                if (margin < Clock::duration::zero() || earliest <= TimePoint{} ||
                        margin >= earliest.time_since_epoch())
                    return;
                const auto readyAt = earliest - margin;
                if (batch.observed || readyAt < batch.submittedAt ||
                        readyAt - batch.submittedAt > maximumReadiness)
                    return;
                batch.observed = true;
                // Preserve half the latch-to-refresh room for dispatch jitter.
                // The full margin can ratchet deadlines into another refresh.
                // Queued GPU work has its separate bounded reserve below.
                const auto allowance = std::min(margin, batch.refreshPeriod) / 2;
                const auto reservedSpacing = std::max(Clock::duration::zero(),
                    batch.desired[0] - batch.submittedAt - batch.lead);
                const auto queuedHold = std::max(Clock::duration::zero(),
                    batch.queuedDuration - reservedSpacing);
                const auto sample = std::max(Clock::duration::zero(),
                    batch.lead + (readyAt - batch.desired[0]) + allowance - queuedHold);
                if (sample > maximumReadiness)
                    return;
                this->recordReadiness(sample);
                return;
            }
        }

        [[nodiscard]] std::optional<Slot> schedule(const TimePoint now,
                const double outputFps, const double refreshFps,
                const size_t outputBatchSize = 1,
                const bool generationEnabled = true,
                const std::optional<TimePoint> sourceDeadline = std::nullopt) {
            if (!validRate(outputFps) || !validRate(refreshFps) ||
                    outputBatchSize == 0 ||
                    outputBatchSize > GeneratedFramePlan::capacity + 1)
                return std::nullopt;
            // Warm-up can learn native-only readiness before generation starts.
            // Prime once when a larger admitted batch first appears. Retain the
            // high-water mark across Fractional small/large alternation so it
            // cannot repeatedly impose startup latency.
            if (this->lastBatchSerial && generationEnabled &&
                    outputBatchSize > this->readinessBatchCapacity) {
                this->resetReadiness();
                this->readinessBatchCapacity = outputBatchSize;
            }
            const auto period = [](const double fps) {
                return std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(1.0 / fps));
            };
            const auto refreshPeriod = period(refreshFps);
            const auto interval = period(std::min(outputFps, refreshFps));
            const auto batchDuration = interval * static_cast<int64_t>(outputBatchSize);
            // Apply the retained work estimate when generated work resumes.
            // Native history/relief can last longer than the sample window;
            // it must neither consume this allowance nor inherit extra lead.
            if (this->primeRecovery && outputBatchSize > 1 && generationEnabled) {
                this->readiness[0] = std::max(
                    this->recoveryReadiness.value_or(Clock::duration::zero()),
                    std::min((this->recoveryReadiness ? 2 : 4) * refreshPeriod,
                        Clock::duration(maximumReadiness)));
                this->readinessCount = 1;
                this->readinessIndex = 1;
                this->primeRecovery = false;
                this->recoveryReadiness.reset();
            }
            // Four refreshes prime unknown asynchronous work. Observed ready
            // batches can release that allowance; CPU pacing cannot enlarge it.
            const auto measuredReadiness = this->readinessCount
                ? *std::max_element(this->readiness.begin(),
                    this->readiness.begin() + this->readinessCount)
                : std::min(4 * refreshPeriod, Clock::duration(maximumReadiness));
            // A threaded presentation waiter releases source credit while
            // another batch is pending. Its deadlines already reserve those
            // slots; a second refresh of lead consumes the next frame's work
            // budget. Keep one refresh of dispatch room for shallow queues,
            // measured readiness when larger, and three for deeper queues.
            const auto reserve = (this->queuedOutputs <= outputBatchSize ? 1 : 3)
                * refreshPeriod;
            const auto bridgeReadiness = std::max(measuredReadiness,
                std::min(reserve, Clock::duration(maximumReadiness)));
            const auto lead = generationEnabled
                ? (this->lastBatchSerial ? bridgeReadiness :
                    std::max(2 * refreshPeriod, batchDuration))
                : Clock::duration::zero();
            const auto next = this->lastPresentAt
                ? *this->lastPresentAt + interval : now;
            auto presentAt = std::max(now + lead, next);
            // Completion-paced clients can return just after the previous
            // deadline. Reapplying a whole refresh of dispatch reserve then
            // adds that return delay on every batch. Keep the existing slot
            // when learned work fits and at most one batch is pending. Cold
            // start, recovery allowance, deeper queues and genuinely late
            // work retain their full lead and rebase instead of catching up.
            if (generationEnabled && this->lastBatchSerial &&
                    this->readinessCount && this->lastPresentAt &&
                    this->queuedOutputs <= outputBatchSize &&
                    next > now && next >= now + measuredReadiness)
                presentAt = next;
            this->lastPresentAt = presentAt;
            // Readiness feedback must use the allowance actually granted;
            // otherwise a retained slot would relearn its saved reserve as
            // additional GPU work and undo the phase correction.
            this->scheduledLead = std::min(lead, presentAt - now);
            this->scheduledRefreshPeriod = refreshPeriod;
            this->scheduledInterval = interval;
            if (!generationEnabled && this->generationEnabled) {
                this->resetReadiness();
                this->lastBatchAt.reset();
            }
            this->generationEnabled = generationEnabled;
            // A capped producer has already reserved its return deadline.
            // Stage this bounded batch by then so submission backpressure
            // does not consume the next source interval. Display targets and
            // readiness lead remain unchanged; late work never submits early.
            const auto submitAt = presentAt - lead - refreshPeriod;
            return Slot{
                .submitAt = std::max(now, sourceDeadline
                    ? std::min(submitAt, *sourceDeadline) : submitAt),
                .presentAt = presentAt,
            };
        }

    private:
        void resetReadiness() {
            this->clearPending();
            this->readinessCount = 0;
            this->readinessIndex = 0;
            this->readinessBatchCapacity = 0;
            this->primeRecovery = false;
            this->recoveryReadiness.reset();
        }

        void clearPending() {
            this->pending = {};
            this->submittedOutputs = 0;
            this->completedOutputs = 0;
            this->queuedOutputs = 0;
        }

        static constexpr auto maximumReadiness = std::chrono::milliseconds(100);
        struct PendingBatch {
            uint64_t firstId{};
            uint64_t firstSubmission{};
            size_t count{};
            std::array<TimePoint, GeneratedFramePlan::capacity + 1> desired{};
            std::array<TimePoint, GeneratedFramePlan::capacity + 1> outputSubmittedAt{};
            std::array<bool, GeneratedFramePlan::capacity + 1> received{};
            size_t acknowledged{};
            bool staged{};
            TimePoint submittedAt{};
            Clock::duration lead{};
            Clock::duration refreshPeriod{};
            Clock::duration queuedDuration{};
            bool observed{};
        };
        std::optional<TimePoint> lastPresentAt;
        std::optional<uint64_t> lastBatchSerial;
        std::optional<TimePoint> lastBatchAt;
        std::array<PendingBatch, 16> pending{};
        size_t batchIndex{};
        std::array<Clock::duration, 4> readiness{};
        size_t readinessIndex{};
        size_t readinessCount{};
        size_t readinessBatchCapacity{};
        Clock::duration scheduledLead{};
        Clock::duration scheduledRefreshPeriod{};
        Clock::duration scheduledInterval{};
        uint64_t submittedOutputs{};
        uint64_t completedOutputs{};
        uint64_t queuedOutputs{};
        bool generationEnabled{};
        bool primeRecovery{};
        std::optional<Clock::duration> recoveryReadiness;
    };

    /// Limits application presents before frame-generation policy observes
    /// them. Deadlines stay on an absolute cadence while the application is
    /// early, but any late frame rebases immediately so a loading stall cannot
    /// create a burst of catch-up presents.
    class RealFramePacer {
    public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

        struct OutputBatch {
            TimePoint first;
            Clock::duration interval;
            size_t count;

            [[nodiscard]] TimePoint at(const size_t index) const {
                return first + interval * static_cast<int64_t>(index);
            }
        };

        /// Observe the application boundary, not generated submissions. Learn
        /// with eight unpaced samples; once pacing is active, exclude our own
        /// measured waits when detecting sustained slower application work.
        /// This prevents a relative game limiter from feeding our waits back
        /// into an ever-increasing output interval.
        void observeSource(const TimePoint now, const bool directFixed, const size_t outputCount = 2,
                const bool releasedSource = false) {
            if (!directFixed) {
                if (this->readyOutputCount) this->resetOutputs();
                this->resetSource();
                return;
            }
            if (this->sourceReleased != releasedSource) {
                this->resetSource();
                this->sourceReleased = releasedSource;
            }
            const auto previous = this->sourceAt;
            this->sourceAt = now;
            const auto wait = std::exchange(this->sourceOutputWait, Clock::duration{});
            const auto acquireWait = std::exchange(this->sourceAcquireWait, Clock::duration{});
            // A separate wait thread can gate the producer through its frame
            // latency event. A completed real-ID wait with no overlapping
            // acquire makes that source interval completion-paced, even
            // though the output itself was delivered by our worker.
            const bool completionPaced = std::exchange(this->sourceCompletionPaced, false);
            const bool detachedOutput = this->sourceDetachedOutput && !completionPaced;
            this->sourceCapBatchRun = std::exchange(this->sourceCapBatchUsed, false)
                ? std::min(this->sourceCapBatchRun + 1, this->sourceSamples.size()) : 0;
            this->sourceInterrupted = false;
            if (!previous)
                return;
            const auto elapsed = now - *previous;
            const auto work = std::max(Clock::duration{}, elapsed - wait);
            const bool completionBlocksSource = completionPaced && this->sourcePeriod > Clock::duration{} &&
                elapsed <= std::chrono::milliseconds{250} && elapsed <= 2 * this->sourcePeriod &&
                elapsed > this->sourcePeriod + this->sourcePeriod /
                    static_cast<int64_t>(2 * std::clamp(outputCount, size_t{2}, GeneratedFramePlan::capacity + 1));
            this->completionBlockedSamples = completionBlocksSource
                ? std::min(size_t{2}, this->completionBlockedSamples + 1) : 0;
            if (elapsed <= Clock::duration::zero() || elapsed > std::chrono::milliseconds{250}) {
                this->sourceInterrupted = true;
                this->sourcePacedPeriod = this->sourceWorkPeriod = Clock::duration{};
                this->sourceCapBatchRun = 0;
                this->sourceSampleCount = this->slowerSourceSamples = this->fasterSourceSamples = 0;
                this->sourceDetachedSamples = 0;
                if (releasedSource) {
                    this->sourcePeriod = {};
                    this->previousSlowerMean = {};
                    this->sourceHasQualifiedCadence = false;
                }
                return;
            }
            if (releasedSource) {
                // A queued producer can submit several sources together, then
                // pause for their GPU work. Individual arrivals and their block
                // mean measure that burst, not throughput. Keep the entire window.
                this->sourceSamples[this->sourceSampleCount++] = elapsed;
                if (this->sourceSampleCount == this->sourceSamples.size()) {
                    Clock::duration total{};
                    for (const auto sample : this->sourceSamples) total += sample;
                    const auto mean = total / static_cast<int64_t>(this->sourceSamples.size());
                    // One slower block can be a transient. A slower period is
                    // adopted only when the previous block was also slower, at
                    // the smaller of the two means. Every slower block is kept
                    // as the reference for the next block's check.
                    const bool slowerBlock = this->sourceHasQualifiedCadence &&
                        this->sourcePeriod > Clock::duration::zero() &&
                        mean > this->sourcePeriod + this->sourcePeriod / 16;
                    if (!slowerBlock) {
                        this->sourcePeriod = mean;
                        this->previousSlowerMean = Clock::duration::zero();
                    } else {
                        if (this->previousSlowerMean > Clock::duration::zero())
                            this->sourcePeriod = std::min(mean, this->previousSlowerMean);
                        this->previousSlowerMean = mean;
                    }
                    this->sourceHasQualifiedCadence = true;
                    this->sourceSampleCount = 0;
                }
                return;
            }
            if (this->sourcePeriod > Clock::duration::zero()) {
                this->sourceInterrupted = elapsed > 2 * this->sourcePeriod;
                if (this->sourceInterrupted) {
                    this->sourceSampleCount = 0;
                    this->sourceDetachedSamples = 0;
                    this->sourcePacedPeriod = this->sourceWorkPeriod = Clock::duration{};
                    this->sourceCapBatchRun = 0;
                    this->fasterSourceSamples = 0;
                    // One hitch skips pacing without losing the estimate.
                    // Sustained >2-period arrivals are a changed workload,
                    // not eight independent hitches that can never qualify.
                    if (++this->slowerSourceSamples >= this->sourceSamples.size()) {
                        this->sourcePeriod = Clock::duration{};
                        this->sourceCapOwnsOutputs = false;
                        this->slowerSourceSamples = this->fasterSourceSamples = 0;
                    }
                    return;
                }
                // A higher game cap can be hidden by our old output sleep.
                // Two arrivals at least an eighth output slot faster release that sleep and
                // qualify the new source directly, without pacing each step
                // of an artificial ramp. Ordinary alternating jitter cannot
                // accumulate evidence across slower frames.
                // Scale the margin with the batch: at 5x our own waits can
                // occupy four fifths of the old period even after work recovers.
                const auto fasterMargin = this->sourcePeriod / static_cast<int64_t>(
                    8 * std::clamp(outputCount, size_t{2}, GeneratedFramePlan::capacity + 1));
                // A first-output phase hold can move into the next acquire.
                // Unlike readiness trading with an absolute game limiter,
                // this is measured blocking on our own retained outputs.
                // Release only the phase when faster wait-excluded work is
                // hidden by that join; keep the qualified batch interval.
                const bool phaseBlockedRecovery = this->readyOutputCount &&
                    this->readyOutputHold > fasterMargin && acquireWait > fasterMargin &&
                    this->sourceWorkPeriod > fasterMargin &&
                    elapsed <= this->sourcePacedPeriod + fasterMargin &&
                    work <= this->sourceWorkPeriod - fasterMargin;
                // A relative game limiter can hide even a modest speedup
                // behind our output span. Compare wait-excluded work only
                // with a complete previously paced window, never with the
                // initial unpaced period or the full application interval.
                // Detached delivery still has bounded joins when WSI has no
                // spare image. Preserve the acceleration test for these waits;
                // a quarter-period threshold can hide real recovery behind
                // readiness and acquisition. Detached acceleration retunes
                // the interval in place: longer jitter runs can pass any
                // consecutive-sample threshold, and must not discard eight
                // paced batches. Keep the complete source window so its
                // slower samples can correct an early-arrival estimate.
                if (elapsed <= this->sourcePeriod - fasterMargin ||
                        (wait > Clock::duration{} && this->sourceWorkPeriod > fasterMargin &&
                            elapsed <= this->sourcePacedPeriod - fasterMargin &&
                            work <= this->sourceWorkPeriod - fasterMargin) || phaseBlockedRecovery) {
                    const size_t requiredFasterSamples = !phaseBlockedRecovery && detachedOutput &&
                        elapsed > this->sourcePeriod - this->sourcePeriod / 4 ? 4 : 2;
                    if (++this->fasterSourceSamples >= requiredFasterSamples) {
                        if (detachedOutput) {
                            if (phaseBlockedRecovery)
                                this->readyOutputRequalification = 2 * this->sourceSamples.size();
                            this->sourcePeriod = std::min(this->sourcePeriod, elapsed);
                            this->fasterSourceSamples = 0;
                        } else {
                            this->sourcePeriod = Clock::duration::zero();
                            this->sourcePacedPeriod = this->sourceWorkPeriod = Clock::duration{};
                            this->sourceSampleCount = this->slowerSourceSamples = this->fasterSourceSamples = 0;
                            this->sourceDetachedSamples = 0;
                            return;
                        }
                    }
                } else {
                    this->fasterSourceSamples = 0;
                }
                // A learned game period may already be slower than a
                // nonbinding configured cap. Compare against that cap when
                // releasing its ownership, rather than the slower estimate.
                const auto slowerReference = this->sourceCapOwnsOutputs && this->activeFramesPerSecond > 0
                    ? std::min(this->sourcePeriod, std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double>(1.0 / this->activeFramesPerSecond)))
                    : this->sourcePeriod;
                if (detachedOutput && this->sourceCapOwnsOutputs && this->sourceCapBatchRun == 0 &&
                        elapsed > slowerReference + fasterMargin) {
                    // Detached cap delivery observes the game's full cadence.
                    // A slower game limiter must be able to replace the cap
                    // owner even though no synchronous output wait grew.
                    if (++this->slowerSourceSamples == this->sourceSamples.size()) {
                        this->sourceCapOwnsOutputs = false;
                        this->slowerSourceSamples = 0;
                    }
                } else if (!detachedOutput && (work > slowerReference ||
                        (wait > Clock::duration{} && this->sourceWorkPeriod > Clock::duration{} &&
                            elapsed >= this->sourcePacedPeriod + fasterMargin &&
                            work >= this->sourceWorkPeriod + fasterMargin))) {
                    if (++this->slowerSourceSamples == this->sourceSamples.size()) {
                        this->sourcePeriod = Clock::duration::zero();
                        this->sourcePacedPeriod = this->sourceWorkPeriod = Clock::duration{};
                        this->sourceCapOwnsOutputs = false;
                        this->sourceSampleCount = this->slowerSourceSamples = this->fasterSourceSamples = 0;
                        this->sourceDetachedSamples = 0;
                        return;
                    }
                } else {
                    this->slowerSourceSamples = 0;
                }
            }
            this->sourceWorkSamples[this->sourceSampleCount] = wait > Clock::duration{}
                ? work : Clock::duration{};
            this->sourceSamples[this->sourceSampleCount++] = elapsed;
            this->sourceDetachedSamples += detachedOutput;
            if (this->sourceSampleCount == this->sourceSamples.size()) {
                auto sorted = this->sourceSamples;
                std::sort(sorted.begin(), sorted.end());
                const auto measuredPeriod = sorted[sorted.size() / 2 - 1] +
                    (sorted[sorted.size() / 2] - sorted[sorted.size() / 2 - 1]) / 2;
                // Sample complete windows, including their slower arrivals.
                // Synchronous windows retain their existing one-way bound.
                // Complete detached-output windows retain pacing while their
                // full-source median moves in either direction. A brief host
                // acquisition wait must not switch back to one-way sampling
                // or create an eight-frame hole for ordinary timing jitter.
                this->sourcePeriod = this->sourceDetachedSamples == this->sourceSamples.size()
                    ? measuredPeriod : this->sourcePeriod > Clock::duration::zero()
                    ? std::min(this->sourcePeriod, measuredPeriod) : measuredPeriod;
                this->sourceHasQualifiedCadence = true;
                auto sortedWork = this->sourceWorkSamples;
                std::sort(sortedWork.begin(), sortedWork.end());
                if (this->fasterSourceSamples == 0 && this->slowerSourceSamples == 0) {
                    this->sourceWorkPeriod = sortedWork.front() > Clock::duration{}
                        ? sortedWork[sortedWork.size() / 2 - 1] +
                            (sortedWork[sortedWork.size() / 2] - sortedWork[sortedWork.size() / 2 - 1]) / 2
                        : Clock::duration{};
                    // Readiness can trade time with an absolute game limiter.
                    // Require the full interval to change in the same direction
                    // before treating a wait-excluded change as a new cadence.
                    this->sourcePacedPeriod = this->sourceWorkPeriod > Clock::duration{}
                        ? measuredPeriod : Clock::duration{};
                }
                this->sourceSampleCount = 0;
                this->sourceDetachedSamples = 0;
            }
        }

        void recordOutputWait(const Clock::duration wait, const bool detachedOutput = false,
                const bool pacedOutput = true) {
            this->sourceOutputWait = wait;
            if (this->readyOutputCount && (!detachedOutput || !pacedOutput))
                this->resetOutputs();
            // A readiness miss delivers no paced batch. It must not erase
            // the cadence's actual detached-delivery history: a shortened
            // estimate can cause that miss, and the following slower samples
            // must still be able to correct it. A synchronous paced batch or
            // an ordinary policy reset ends the detached episode.
            if (pacedOutput)
                this->sourceDetachedOutput = detachedOutput;
        }
        void recordAcquireWait(const Clock::duration wait) {
            // Keep evidence of how the output was submitted. A host join is
            // real wait cost, but it does not turn a detached slot into the
            // synchronous delivery path used by other transports.
            this->sourceOutputWait += wait;
            this->sourceAcquireWait += wait;
        }

        void recordCompletionPacingWait(const Clock::duration wait) {
            this->sourceOutputWait += wait;
            this->sourceCompletionPaced = wait > Clock::duration::zero();
        }
        [[nodiscard]] bool completionPacingBlocksSource() const { return this->completionBlockedSamples >= 2; }
        /// X4: an application present-mode change starts a new output clock. Completion samples taken
        /// in the old mode no longer establish completion pacing; the new mode must observe them again.
        void resetCompletionBlockedSamples() { this->completionBlockedSamples = 0; }

        /// No fallback delay on a missed source interval or when preparation
        /// has already consumed a whole interval. GPU readiness shares this
        /// same bound and never starts an independent unbounded wait.
        [[nodiscard]] Clock::duration sourceReadinessBudget(const TimePoint now, const size_t count = 1,
                const bool allowDetachedOutput = false) const {
            // Once the configured cap has spaced a complete batch, a brief
            // miss must not replace that owner with readiness + output waits.
            // Those waits can consume the next cap interval and perpetuate
            // the miss. Sustained slower work requalifies in observeSource;
            // a game-owned limiter with no proven cap keeps its fallback.
            if (!this->sourceAt || this->sourceInterrupted ||
                    (this->sourceCapOwnsOutputs && !(allowDetachedOutput && this->sourceDetachedOutput)) ||
                    this->sourcePeriod <= Clock::duration::zero() || now < *this->sourceAt ||
                    count == 0 || count > GeneratedFramePlan::capacity + 1)
                return {};
            // A bounded detached batch consumes no synchronous output span.
            // Its first output follows readiness on the retained phase; the
            // worker never retains a second source batch.
            const auto span = allowDetachedOutput ? Clock::duration{} :
                (this->sourcePeriod / static_cast<int64_t>(count)) * static_cast<int64_t>(count - 1);
            return std::max(Clock::duration::zero(), std::min(
                this->sourcePeriod - span - (now - *this->sourceAt),
                std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds{50})));
        }

        /// Game-owned caps have no reserved framegen-vulkan cap slack. Detached batches
        /// share one output phase, including the gap after the prior real.
        /// Bound first-output holds to one slot. On exhaustion, observe a
        /// complete source window without those holds so faster work cannot
        /// remain hidden behind the old phase. Within-batch spacing stays on.
        [[nodiscard]] std::optional<OutputBatch> scheduleReadyOutputs(
                const TimePoint now, const size_t count, const bool allowDetachedOutput = false) {
            if (count < 2 || count > GeneratedFramePlan::capacity + 1 ||
                    this->sourceReadinessBudget(now, count, allowDetachedOutput) <= Clock::duration::zero()) {
                this->resetOutputs();
                return std::nullopt;
            }
            const auto interval = this->sourcePeriod / static_cast<int64_t>(count);
            if (!OrderedPresentTimeline::validRate(1.0 / std::chrono::duration<double>(interval).count())) {
                this->resetOutputs();
                return std::nullopt;
            }
            if (!allowDetachedOutput) {
                this->resetOutputs();
                return OutputBatch{now, interval, count};
            }
            return this->chainDetachedOutputs(now, interval, count, false);
        }

        /// A source-cap deadline owned by the application's release wait
        /// (see swapchain/release_gate.hpp) no longer holds the present call,
        /// so there is no return deadline to plan against. Space the batch
        /// from GPU readiness, as the worker does for an uncapped source; the
        /// release wait enforces the cap itself. The cap interval is the
        /// shortest batch period. A GPU-bound source slower than a non-binding
        /// cap is spaced on its measured period (the block mean observeSource
        /// learns; a slower period needs two consecutive slower blocks), so each
        /// generated frame sits midway between reals instead of on the cap's
        /// half slot.
        ///
        /// Without a cap (sourceInterval <= 0) no interval bounds the batch.
        /// Space it only on a qualified, uninterrupted measured period. Until
        /// that cadence is proven, return nullopt so the caller's ready-output
        /// path carries the warm-up.
        ///
        /// justInTime (low_latency): the batch's nominal first output is `now`,
        /// and the worker starts it at the batch's own readiness instead of
        /// after the previous batch's planned output phase.
        [[nodiscard]] std::optional<OutputBatch> scheduleReleasedOutputs(
                const TimePoint now, const Clock::duration sourceInterval, const size_t count,
                const bool justInTime = false) {
            if (count < 2 || count > GeneratedFramePlan::capacity + 1) {
                this->resetOutputs();
                return std::nullopt;
            }
            auto batchPeriod = sourceInterval;
            if (sourceInterval <= Clock::duration::zero()) {
                if (!this->sourceHasQualifiedCadence || this->sourceInterrupted ||
                        this->sourcePeriod <= Clock::duration::zero()) {
                    this->resetOutputs();
                    return std::nullopt;
                }
                batchPeriod = this->sourcePeriod;
            } else if (this->sourceHasQualifiedCadence && !this->sourceInterrupted &&
                    this->sourcePeriod > sourceInterval && this->sourcePeriod <= 4 * sourceInterval)
                batchPeriod = this->sourcePeriod;
            const auto interval = batchPeriod / static_cast<int64_t>(count);
            if (!OrderedPresentTimeline::validRate(1.0 / std::chrono::duration<double>(interval).count())) {
                this->resetOutputs();
                return std::nullopt;
            }
            return this->chainDetachedOutputs(now, interval, count, justInTime);
        }

        /// Rephase a substantially late retained tail. Normal sleep overshoot
        /// stays on its absolute clock instead of extending every period and
        /// teaching the source estimator an ever-slower cadence.
        void completeOutputBatch(const TimePoint lastOutput,
                const Clock::duration readinessShift = {}) {
            if (this->lastOutputAt)
                *this->lastOutputAt += readinessShift;
            if (this->readyOutputCount && this->lastOutputAt &&
                    lastOutput > *this->lastOutputAt +
                        this->sourcePeriod / static_cast<int64_t>(2 * this->readyOutputCount))
                this->lastOutputAt = lastOutput;
        }

        /// Admission/acquisition fallback can finish a ready batch on the
        /// caller. Read the clock only for that active output episode.
        void synchronousOutput() {
            if (this->readyOutputCount && this->lastOutputAt)
                this->completeOutputBatch(Clock::now());
        }

        /// Plan against the existing source-cap interval. Keep a target-output
        /// clock across changing batch counts: spacing each batch separately
        /// would produce 12.5/25 ms gaps for a 40 -> 60 FPS source/target pair.
        /// Detached Fixed delivery may extend the final slot past the source
        /// deadline; the caller retains that application-return deadline.
        [[nodiscard]] std::optional<OutputBatch> scheduleOutputs(
                const TimePoint readyAt, const TimePoint realFrameAt, const double outputFps,
                const size_t count, const bool targetClockActive,
                const bool rephaseLateFixed = false) {
            if (this->readyOutputCount) this->resetOutputs();
            if (!OrderedPresentTimeline::validRate(outputFps) ||
                    this->activeFramesPerSecond <= 0.0 || count == 0 ||
                    count > GeneratedFramePlan::capacity + 1 ||
                    (count == 1 && !targetClockActive)) {
                // A zero-generation turn belongs on the output clock only
                // when it is part of a running Fractional ledger. Startup,
                // native preference and native probes keep the source cap.
                this->resetOutputs();
                return std::nullopt;
            }
            const auto period = [](const double fps) {
                return std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(1.0 / fps));
            };
            const auto interval = period(outputFps);
            const auto sourceInterval = period(this->activeFramesPerSecond);
            const auto span = interval * static_cast<int64_t>(count - 1);
            if (span >= sourceInterval) {
                this->resetOutputs();
                return std::nullopt;
            }
            const auto latestFirst = realFrameAt - span;
            auto first = latestFirst;
            if (this->outputFramesPerSecond == outputFps && this->lastOutputAt) {
                const auto next = *this->lastOutputAt + interval;
                if (next > realFrameAt - sourceInterval)
                    first = rephaseLateFixed && !targetClockActive
                        ? std::max(next, latestFirst) : std::min(next, latestFirst);
            }
            // A GPU-ready native Fixed batch with detached final delivery may
            // miss its first slot. Retain the complete interval and the prior
            // output's phase while the source cap keeps its original clock.
            // Never retain a whole missed source interval of output debt.
            if (rephaseLateFixed && !targetClockActive && count > 1) {
                first = std::max(first, readyAt);
                // Readiness can shift the preceding batch just across the
                // source-return boundary. Keep that phase within one output
                // slot of the boundary rather than dropping the next pair.
                // Only one source batch is retained, so this cannot queue a
                // second source interval of output debt.
                if (first >= realFrameAt + interval) {
                    this->resetOutputs();
                    return std::nullopt;
                }
            }
            // Never compress an overdue output batch into catch-up presents.
            if (first < readyAt || (this->lastOutputAt && first <= *this->lastOutputAt)) {
                this->resetOutputs();
                return std::nullopt;
            }
            this->outputFramesPerSecond = outputFps;
            this->lastOutputAt = first + span;
            if (this->sourceAt && !targetClockActive) {
                this->sourceCapBatchUsed = true;
                // Cold cap startup keeps its immediate owner. Once a game
                // cadence is learned, one burst or partial-batch return must
                // not permanently suppress its ready-output fallback.
                if (!this->sourceHasQualifiedCadence || this->sourceCapOwnsOutputs ||
                        this->sourceCapBatchRun + 1 >= this->sourceSamples.size())
                    this->sourceCapOwnsOutputs = true;
            }
            return OutputBatch{first, interval, count};
        }

        /// Continue the previous detached batch's output phase. The worker
        /// retains one source batch, so its final output must leave before
        /// the next source normally arrives: otherwise that present, or the
        /// acquire before it, joins the worker on the application's producing
        /// thread. VKD3D-Proton runs both on its D3D12 queue-submission
        /// thread, where any join stalls later game submissions. Bound the
        /// phase hold to three quarters of a slot and shorten only that gap.
        ///
        /// justInTime batches (released, low_latency) take no such hold and count
        /// no requalification: their nominal first output is `now`, and the
        /// worker moves it to readiness. lastOutputAt, readyOutputCount and
        /// readyOutputHold are still written, so a later batch that is not
        /// just-in-time chains from the same bookkeeping.
        [[nodiscard]] OutputBatch chainDetachedOutputs(
                const TimePoint now, const Clock::duration interval, const size_t count,
                const bool justInTime) {
            auto first = now;
            if (this->readyOutputCount != count) {
                this->resetOutputs();
            } else if (!justInTime && this->readyOutputRequalification) {
                --this->readyOutputRequalification;
            } else if (!justInTime && this->lastOutputAt) {
                const auto next = *this->lastOutputAt + interval;
                if (next > now) {
                    if (next - now < interval)
                        first = std::min(next, now + maximumDetachedHold(interval));
                    else
                        this->readyOutputRequalification = 2 * this->sourceSamples.size();
                }
            }
            this->readyOutputCount = count;
            this->readyOutputHold = first - now;
            this->lastOutputAt = first + interval * static_cast<int64_t>(count - 1);
            return OutputBatch{first, interval, count};
        }
        [[nodiscard]] static constexpr Clock::duration maximumDetachedHold(const Clock::duration interval) {
            return interval - interval / 4;
        }

        void resetOutputs() {
            this->lastOutputAt.reset();
            this->outputFramesPerSecond = 0.0;
            this->readyOutputCount = this->readyOutputRequalification = 0;
            this->readyOutputHold = Clock::duration{};
        }

        [[nodiscard]] TimePoint schedule(
                const TimePoint now, const double framesPerSecond,
                const bool preserveRatePhase = false) {
            if (!std::isfinite(framesPerSecond) || framesPerSecond <= 0.0) {
                if (this->activeFramesPerSecond > 0.0 || this->nextFrameAt)
                    this->reset();
                else if (!this->readyOutputCount)
                    this->resetOutputs();
                return now;
            }

            if (this->activeFramesPerSecond != framesPerSecond) {
                this->sourceCapOwnsOutputs = false;
                this->sourceCapBatchRun = 0;
                this->sourceCapBatchUsed = false;
                if (preserveRatePhase && this->activeFramesPerSecond > 0.0 && this->nextFrameAt) {
                    // A governor's small rate adjustment belongs to the same
                    // pacing episode. Move its next boundary without granting
                    // a free frame or discarding the Adaptive output clock.
                    *this->nextFrameAt += std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double>(1.0 / framesPerSecond - 1.0 / this->activeFramesPerSecond));
                } else {
                    this->nextFrameAt.reset();
                    this->resetOutputs();
                }
                this->activeFramesPerSecond = framesPerSecond;
            }

            const auto interval = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(
                    1.0 / framesPerSecond
                )
            );
            if (!this->nextFrameAt) {
                this->nextFrameAt = now + interval;
                return now;
            }

            if (now >= *this->nextFrameAt) {
                this->nextFrameAt = now + interval;
                return now;
            }

            const auto deadline = *this->nextFrameAt;
            *this->nextFrameAt += interval;
            return deadline;
        }

        void reset() {
            this->nextFrameAt.reset();
            this->activeFramesPerSecond = 0.0;
            this->resetOutputs();
            this->resetSource();
        }

    private:
        void resetSource() {
            this->sourceAt.reset();
            this->sourcePeriod = this->sourceOutputWait = this->sourceAcquireWait = Clock::duration::zero();
            this->previousSlowerMean = Clock::duration::zero();
            this->sourcePacedPeriod = this->sourceWorkPeriod = Clock::duration{};
            this->sourceCapOwnsOutputs = false;
            this->sourceHasQualifiedCadence = this->sourceCapBatchUsed = false;
            this->sourceCapBatchRun = 0;
            this->sourceSampleCount = this->slowerSourceSamples = this->fasterSourceSamples = 0;
            this->sourceDetachedSamples = 0;
            this->sourceDetachedOutput = false;
            this->sourceCompletionPaced = false;
            this->completionBlockedSamples = 0;
            this->sourceInterrupted = false;
            this->sourceReleased = false;
        }
        std::optional<TimePoint> sourceAt;
        std::array<Clock::duration, 8> sourceSamples{};
        std::array<Clock::duration, 8> sourceWorkSamples{};
        Clock::duration sourceWorkPeriod{}, sourcePacedPeriod{};
        Clock::duration sourcePeriod{}, sourceOutputWait{};
        // Mean of the latest slower released block; zero when none is pending.
        Clock::duration previousSlowerMean{};
        Clock::duration sourceAcquireWait{}, readyOutputHold{};
        size_t sourceSampleCount{}, slowerSourceSamples{}, fasterSourceSamples{};
        size_t sourceDetachedSamples{};
        bool sourceInterrupted{};
        bool sourceReleased{};
        bool sourceDetachedOutput{};
        bool sourceCompletionPaced{};
        size_t completionBlockedSamples{};
        bool sourceCapOwnsOutputs{};
        bool sourceHasQualifiedCadence{}, sourceCapBatchUsed{};
        size_t sourceCapBatchRun{};
        double activeFramesPerSecond{0.0};
        std::optional<TimePoint> nextFrameAt;
        double outputFramesPerSecond{0.0};
        std::optional<TimePoint> lastOutputAt;
        size_t readyOutputCount{}, readyOutputRequalification{};
    };

    /// Normally suppress synthetic frames that exceed confirmed refresh.
    /// Fixed Smooth Cadence can instead request the full multiplier when
    /// ordered FIFO owns real-frame pacing, including under VRR.
    class FixedRefreshBudget {
    public:
        using TimePoint = std::chrono::steady_clock::time_point;

        [[nodiscard]] size_t plan(const TimePoint now,
                const std::optional<uint32_t> refreshHz,
                const size_t maximumGeneratedFrames,
                const bool fullMultiplierCadence = false) {
            if (!refreshHz || *refreshHz == 0 || maximumGeneratedFrames == 0) {
                this->lastRealFrame = now;
                return maximumGeneratedFrames;
            }
            if (!this->lastRealFrame) {
                this->lastRealFrame = now;
                return 0;
            }

            const double rawInterval = std::chrono::duration<double>(
                now - *this->lastRealFrame
            ).count();
            this->lastRealFrame = now;
            if (rawInterval <= 0.0 || rawInterval > 0.25) {
                this->smoothedIntervalSeconds = 0.0;
                this->outputCredit = 0.0;
                return 0;
            }
            if (this->smoothedIntervalSeconds == 0.0)
                this->smoothedIntervalSeconds = rawInterval;
            else
                this->smoothedIntervalSeconds =
                    this->smoothedIntervalSeconds * 0.75 + rawInterval * 0.25;

            // Retain both sides of timestamp jitter. Clipping late intervals
            // at the multiplier while charging every early interval slowly
            // drains the budget even when the average source rate fits exactly.
            const double desiredOutputs = std::max(
                this->smoothedIntervalSeconds * static_cast<double>(*refreshHz),
                1.0
            );
            this->outputCredit += desiredOutputs;
            const size_t requestedOutputs = std::max<size_t>(
                1, static_cast<size_t>(std::floor(this->outputCredit + 1e-9))
            );
            const size_t generated = std::min(
                requestedOutputs - 1, maximumGeneratedFrames
            );
            this->outputCredit -= static_cast<double>(generated + 1);
            if (this->outputCredit < 0.0)
                this->outputCredit = 0.0;
            // Carry at most one output of timing credit, never a backlog from
            // a slow source. It can absorb jitter without authorizing an
            // extended overshoot when the source accelerates.
            this->outputCredit = std::min(this->outputCredit, 1.0);
            // Keep the display budget warm for a later policy change, but
            // retain full Fixed output under ordered FIFO pacing.
            return fullMultiplierCadence ? maximumGeneratedFrames : generated;
        }

        void reset() {
            this->lastRealFrame.reset();
            this->smoothedIntervalSeconds = 0.0;
            this->outputCredit = 0.0;
        }

    private:
        std::optional<TimePoint> lastRealFrame;
        double smoothedIntervalSeconds{0.0};
        double outputCredit{0.0};
    };

}

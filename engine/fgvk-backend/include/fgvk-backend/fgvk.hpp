/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace fgvk::backend {

    /// Encoding of the images exchanged with the frame-generation backend.
    enum class FrameEncoding : uint8_t {
        Sdr8,
        SdrHighPrecision,
        ScRgbLinear,
        Hdr10Pq,
    };

    class [[gnu::visibility("default")]] ContextImpl;
    class [[gnu::visibility("default")]] InstanceImpl;

    using Context = ContextImpl;

    /// Diagnostic timings for the most recent completed generated-frame batch.
    /// GPU spans include commands/barriers; CPU submit times can overlap GPU work.
    struct FrameProfile {
        uint64_t backendFrameIndex{}; // Zero-based scheduled batch, including history-only steps.
        double cpuPreviousWaitUs{};
        double cpuPrepassSubmitUs{}; // Separate prepass; zero for a combined single-output submission.
        double cpuGenerationSubmitUs{}; // Includes preprocessing for a combined single-output submission.
        double gpuPrepassUs{};
        double gpuInputConversionUs{};
        double gpuMotionEstimationUs{};
        std::vector<double> gpuGeneratedUs;
        std::vector<double> gpuSynthesisUs;
        std::vector<double> gpuOutputConversionUs;
        double gpuSpanUs{};
        double cpuReadbackUs{}; // Automatic completed-batch collection only.
        uint32_t timestampValidBits{};
        float timestampPeriodNs{};
    };

    /// Non-draining GPU boundaries for one generated batch, converted to
    /// CLOCK_MONOTONIC nanoseconds. Zero means unknown.
    struct SplitFrameProfile {
        uint64_t backendFrameIndex{}; // Same zero-based batch index as FrameProfile.
        int64_t fgStartNs{};          // Prepass start, after the batch-order barrier.
        int64_t prepassEndNs{};       // Prepass end.
        int64_t generatedEndNs{};     // Source-release start: every generated output of this batch has completed.
        int64_t batchEndNs{};         // Source-release end.
    };

    /// The newest completed batch of a split-profiled context, read when its fence signalled.
    struct CompletedBatchEnd {
        uint64_t backendFrameIndex{}; // Same zero-based batch index as SplitFrameProfile.
        int64_t batchEndNs{};         // Source-release end, CLOCK_MONOTONIC nanoseconds.
    };

    ///
    /// Primitive exception class that deliveres a detailed error message
    ///
    class [[gnu::visibility("default")]] error : public std::runtime_error {
    public:
        ///
        /// Construct an error
        ///
        /// @param msg Error message.
        /// @param inner Inner exception.
        ///
        explicit error(const std::string &msg, const std::exception &inner);

        ///
        /// Construct an error
        ///
        /// @param msg Error message.
        ///
        explicit error(const std::string &msg);

        error(const error &) = default;
        error &operator=(const error &) = default;
        error(error &&) = default;
        error &operator=(error &&) = default;
        ~error() override;
    };

    /// Function type for picking a device based on its name and IDs
    using DevicePicker = std::function<bool(
        const std::string& deviceName,
        std::pair<const std::string&, const std::string&> ids, // (vendor ID, device ID) 0xXXXX format
        const std::optional<std::string>& pci // (bus:slot.func) if available, no padded zeros
    )>;

    ///
    /// Main entry point of the library
    ///
    class [[gnu::visibility("default")]] Instance {
    public:
        ///
        /// Create a fgvk instance
        ///
        /// @param devicePicker Function that picks a physical device based on some identifiers.
        /// @param shaderDllPath Path to the Lossless.dll file to load shaders from.
        /// @param allowLowPrecision Request supported FP16 model shaders; false requires FP32.
        ///
        /// @throws backend::error on failure
        ///
        Instance(
            const DevicePicker& devicePicker,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision
        );

        ///
        /// Open a frame generation context.
        ///
        /// The VkFormat of the exchanged images is inferred from encoding:
        /// - Sdr8: VK_FORMAT_R8G8B8A8_UNORM
        /// - SdrHighPrecision, ScRgbLinear, Hdr10Pq: VK_FORMAT_R16G16B16A16_SFLOAT
        ///
        /// Hdr10Pq input is PQ BT.2020 and its output is written back as PQ;
        /// the model runs on linear scRGB BT.709 (80 nits per unit) in between.
        /// ScRgbLinear input and output are already linear scRGB.
        ///
        /// The application and library must keep track of the frame index. When the next frame
        /// is ready, signal the syncFd with one increment (with the first trigger being 1).
        /// Each generated frame will increment the semaphore by one:
        /// - Application signals 1 -> Start generating with (curr, next) source images
        /// - Library signals 2 -> First frame between (curr, next) is ready
        /// - Library signals N+1 -> N-th frame between (curr, next) is ready
        /// - Application signals N+2 -> Start generating with (next, curr) source images
        ///
        /// Context creation initializes image layouts; upload sources afterwards.
        /// Before signalling the input timeline, release uploaded sources to
        /// VK_QUEUE_FAMILY_EXTERNAL in GENERAL. The backend acquires both sources
        /// and releases them in GENERAL at batch completion (also for history).
        /// Acquire each generated output from EXTERNAL in GENERAL after its
        /// timeline wait, then perform any caller-owned layout transition.
        /// Source uploads and generated outputs fully overwrite their images:
        /// UNDEFINED transitions discard prior contents/ownership on reuse.
        /// Complete all previous reads before permitting that overwrite.
        ///
        /// Takes ownership of every supplied descriptor on entry, including when
        /// construction fails before an import. Pass distinct owned descriptors;
        /// the caller must neither close nor reuse them after this call.
        ///
        /// @param sourceFds Pair of file descriptors for the source images alternated between.
        /// @param destFds Vector with file descriptors to import output images from.
        /// @param syncFd File descriptor for the timeline semaphore used for synchronization.
        /// @param width Width of the images.
        /// @param height Height of the images.
        /// @param encoding Colour encoding of the exchanged images.
        /// @param flow Inverse motion Flow Scale after profile presets (1..4).
        /// @param perf Whether to enable performance mode.
        /// @throws backend::error on failure
        ///
        Context& openContext(
            std::pair<int, int> sourceFds,
            const std::vector<int>& destFds,
            int syncFd,
            uint32_t width, uint32_t height,
            FrameEncoding encoding, float flow, bool perf
        );

        ///
        /// Schedule a new set of generated frames.
        ///
        /// @param context Context to use.
        /// @throws backend::error on failure
        ///
        void scheduleFrames(Context& context);

        /// Enable diagnostic CPU clocks and GPU timestamps on a fresh context.
        /// Must precede its first schedule; ordinary contexts allocate no queries.
        /// collectCompleted reuses the next normal previous-work fence wait
        /// to collect each completed batch. It adds no wait or polling loop.
        void enableFrameProfiling(Context& context, bool collectCompleted = false);

        /// Consume a collected batch once; history/unsupported/unavailable
        /// observations return no sample. This performs no Vulkan operation.
        [[nodiscard]] std::optional<FrameProfile> takeCompletedFrameProfile(Context& context);

        /// Read the latest batch after the caller establishes GPU completion with
        /// its normal output wait. No extra fence wait or blocking query-result
        /// flag is added; unavailable queries fail. Readback is measurement overhead.
        [[nodiscard]] FrameProfile readFrameProfile(const Context& context) const;

        /// Enable non-draining split GPU boundaries on a fresh context. Must precede
        /// its first schedule. It adds no wait, drain, or schedule change: each
        /// batch is read only after its existing fence has signalled. Calibration
        /// or query failure logs one reason and leaves the context unchanged.
        /// retainCompleted keeps each completed generated batch for
        /// takeCompletedSplitProfiles (the diagnostics log); off keeps only the
        /// latest batch end for latestBatchEnd.
        void enableSplitProfile(Context& context, bool retainCompleted);

        /// Consume completed split samples oldest first. The queue is bounded to
        /// eight entries and drops its oldest sample. Performs no Vulkan operation.
        [[nodiscard]] std::vector<SplitFrameProfile> takeCompletedSplitProfiles(Context& context);

        /// Whether split boundaries are active on this context.
        [[nodiscard]] bool splitProfileEnabled(const Context& context) const;

        /// The newest batch whose fence was observed signalled, or nothing without
        /// split boundaries. Read from the same observation as the split samples;
        /// performs no Vulkan operation.
        [[nodiscard]] std::optional<CompletedBatchEnd> latestBatchEnd(const Context& context) const;

        /// Whether the most recently scheduled batch still runs on the GPU, from one
        /// non-blocking fence status query. Nothing before the context's first batch.
        /// Observes no completion, so it changes no state.
        [[nodiscard]] std::optional<bool> previousBatchPending(const Context& context) const;

        /// Read the split boundaries of every in-flight batch that has already completed,
        /// from one non-blocking fence status query per scheduled slot (prepareWork itself
        /// observes only the slot it reuses, three batches back). Waits for nothing.
        /// @throws ls::vulkan_error when a status query fails
        void pollSplitProfiles(Context& context);

        ///
        /// Schedule generated frames at explicit interpolation timestamps.
        ///
        /// Timestamps must be strictly increasing values between 0 and 1. The
        /// number of timestamps may not exceed the destination-image capacity
        /// supplied when the context was opened.
        ///
        /// @param context Context to use.
        /// @param timestamps Normalized positions between the previous and current real frame.
        /// @throws backend::error on failure
        ///
        void scheduleFrames(Context& context, std::span<const float> timestamps);

        ///
        /// Update temporal model history without generating output frames.
        ///
        /// The caller must submit the source frame and signal its shared timeline
        /// value first. The shared pre-pass is then run so temporal feature maps,
        /// source-image parity, and synchronization remain aligned while the
        /// expensive per-output generation passes are skipped.
        ///
        /// @param context Context to advance.
        ///
        void scheduleFrameHistory(Context& context);

        /// Check whether the context's previously submitted work has completed
        /// without blocking.
        [[nodiscard]] bool contextReady(const Context& context) const;

        ///
        /// Close a frame generation context
        ///
        /// @param context Context to close.
        ///
        void closeContext(const Context& context);

        // Non-copyable and non-movable
        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;
        Instance(Instance&&) = delete;
        Instance& operator=(Instance&&) = delete;
        virtual ~Instance();
    private:
        void collectRetiredContexts();

        std::unique_ptr<InstanceImpl> m_impl;

        std::vector<std::unique_ptr<Context>> m_contexts;
        std::vector<std::unique_ptr<Context>> m_retiredContexts;
    };

    ///
    /// Retain private Vulkan objects for the remaining process lifetime.
    /// This works around Vulkan loader configurations where destroying a
    /// nested instance and device during layer unload is unsafe.
    /// The retained device submits no new work, but its driver resources remain
    /// allocated until the host process exits. Do not enable this before a
    /// backend context has successfully been attached to an application layer.
    ///
    void makeLeaking();

}

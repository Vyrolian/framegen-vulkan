/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

// Internal to the pacing recorder. Producers on game threads enqueue these fixed structs; the
// writer thread in present_records.cpp formats them into the text the analysers read. Nothing
// here formats, allocates or locks on the producer side, except the rare one-off text path.

#include "native_copy_profile.hpp"
#include "present_diagnostics.hpp"
#include "trace_recorder.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>

namespace fgvk::layer::present_records {

using trace_recorder::RecordKind;
using trace_recorder::SlotPool;
using trace_recorder::TraceRecorder;
using Clock = present_diagnostics::Clock;
using LayerCall = present_diagnostics::LayerCall;

struct TextPayload {
    std::string* text{};
    size_t bytes{};
};

struct PacingWindowPayload {
    uint64_t context{};
    VkSwapchainKHR swapchain{};
    VkPresentModeKHR mode{};
    Clock::time_point finished{};
    size_t slot{};
    std::array<char, 16> wsi{};
    bool waylandObserver{};
};

struct PacingOutputPayload {
    uint64_t context{};
    VkSwapchainKHR swapchain{};
    present_diagnostics::PacingProfile::OutputSample sample{};
};

struct PacingSkipPayload {
    uint64_t context{};
    uint64_t source{};
    std::array<char, 16> reason{};
    Clock::time_point scheduled{};
    Clock::time_point now{};
};

struct PacingSourcePayload {
    uint64_t context{};
    VkSwapchainKHR swapchain{};
    uint32_t thread{};
    present_diagnostics::PacingProfile::SourceSample sample{};
};

struct PacingGpuPayload {
    uint64_t context{};
    uint64_t source{};
    uint64_t epoch{};
    uint64_t frame{};
    double previousWaitUs{};
    double prepassSubmitUs{};
    double generationSubmitUs{};
    double prepassUs{};
    double spanUs{};
    double readbackUs{};
    uint32_t outputs{};
    // Output counts above this are counted but not listed. framegen-vulkan caps generated frames at four.
    std::array<double, 16> generatedUs{};
};

struct PacingGpuSplitPayload {
    uint64_t context{};
    uint64_t source{};
    uint64_t backendFrame{};
    int64_t gameDoneNs{};
    int64_t sourceCopyEndNs{};
    int64_t fgStartNs{};
    int64_t prepassEndNs{};
    int64_t generatedEndNs{};
    int64_t batchEndNs{};
};

struct PacingOutputGpuPayload {
    uint64_t context{};
    uint64_t source{};
    bool available{};
    int64_t copyStartNs{};
    int64_t copyEndNs{};
};

struct PacingPresentMarkerPayload {
    uint64_t context{};
    uint64_t source{};
    uint64_t feedback{};
    bool generated{};
    bool available{};
    int64_t markerStartNs{};
    int64_t markerEndNs{};
};

struct PacingAcquireDriverPayload {
    uint32_t thread{};
    uint64_t callId{};
    uint64_t attempt{};
    uint64_t timeout{};
    Clock::time_point started{};
    Clock::time_point finished{};
    VkResult result{};
    uint32_t imageIndex{};
};

struct PacingApplicationPayload {
    uint32_t thread{};
    uint64_t stream{};
    uint64_t ordinal{};
    uint64_t context{};
    uint64_t source{};
    VkSwapchainKHR swapchain{};
    present_diagnostics::ApplicationFrameApi api{};
    uint32_t imageIndex{};
    Clock::time_point started{};
    Clock::time_point finished{};
    std::optional<VkResult> result;
    std::optional<uint64_t> timeout;
    std::optional<Clock::duration> configurationUpdate;
    std::optional<Clock::duration> deferredJoin;
    uintptr_t caller{};
    uint64_t callId{};
    uint64_t lowerCalls{};
    Clock::duration lowerDuration{};
};

struct PacingWaitPayload {
    uint32_t thread{};
    VkSwapchainKHR swapchain{};
    present_diagnostics::PresentWaitApi api{};
    uint64_t presentId{};
    uint64_t timeout{};
    VkResult result{};
    Clock::time_point started{};
    Clock::time_point finished{};
};

// A copy submission with its semaphore lists. The lists exceed one record, so the payload is a slot.
struct CopySlot {
    uint64_t context{};
    uint64_t source{};
    VkSwapchainKHR swapchain{};
    bool generated{};
    uint32_t imageIndex{};
    uint32_t pass{};
    VkDevice device{};
    VkQueue queue{};
    VkCommandBuffer command{};
    Clock::time_point started{};
    Clock::time_point finished{};
    bool submitted{};
    VkSemaphore timelineWait{};
    uint64_t waitValue{};
    VkSemaphore timelineSignal{};
    uint64_t signalValue{};
    VkFence fence{};
    uint32_t thread{};
    size_t waitCount{};
    size_t signalCount{};
    std::array<VkSemaphore, 16> waits{};
    std::array<VkSemaphore, 16> signals{};
};

struct PacingCopyPayload {
    size_t slot{};
};

struct ApplicationWindowPayload {
    uint32_t thread{};
    uint64_t stream{};
    uintptr_t caller{};
    VkSwapchainKHR swapchain{};
    present_diagnostics::ApplicationFrameApi api{};
    Clock::time_point finished{};
    size_t slot{};
};

struct PresentWaitWindowPayload {
    uint32_t thread{};
    uint64_t waiter{};
    VkDevice device{};
    VkSwapchainKHR swapchain{};
    present_diagnostics::PresentWaitApi api{};
    Clock::time_point finished{};
    size_t slot{};
};

struct PacingCallPayload {
    uint32_t thread{};
    LayerCall call{};
    Clock::time_point enter{};
    Clock::time_point exit{};
    VkResult result{};
    uint64_t presentId{};
    uint64_t timeoutNs{};
    uint64_t waitCount{};
};

struct PacingCallWindowPayload {
    uint32_t thread{};
    LayerCall call{};
    uint64_t count{};
    int64_t sumNs{};
    int64_t maxNs{};
};

struct PacingAppFramePayload {
    uint32_t thread{};
    uint64_t source{};
    Clock::time_point enter{};
    Clock::time_point exit{};
    Clock::time_point previousExit{};
    int64_t insideLayer{};
    int64_t insideSubmit{};
    int64_t insideWait{};
    int64_t insideAcquire{};
};

struct PacingAcquirePayload {
    uint32_t thread{};
    Clock::time_point enter{};
    Clock::time_point exit{};
    bool host{};
    int64_t lockWaitNs{};
    uint64_t tailWaitValue{};
    int64_t tailSeenNs{};
};

struct ReflexMarkerPayload {
    uint32_t thread{};
    uint64_t presentId{};
    VkLatencyMarkerNV marker{};
    int64_t ns{};
};

struct ReflexSleepPayload {
    uint32_t thread{};
    Clock::time_point enter{};
    Clock::time_point exit{};
    VkResult result{};
};

struct ReflexModePayload {
    VkBool32 lowLatency{};
    VkBool32 boost{};
    uint32_t minimumIntervalUs{};
};

struct WaylandSamplePayload {
    uint64_t token{};
    present_diagnostics::WaylandSample sample{};
};

struct WaylandGapPayload {
    uint64_t token{};
    uint64_t lost{};
};

// The slots and the recorder that every producer shares. Created once by initialize().
struct Pools {
    SlotPool<present_diagnostics::PacingProfile::Window, 4> pacingWindows;
    SlotPool<CopySlot, 16> copies;
    SlotPool<present_diagnostics::ApplicationFrameTiming::Window, 8> applicationWindows;
    SlotPool<present_diagnostics::ApplicationPresentWait::Window, 8> presentWaitWindows;
};

/// The recorder, or null before initialize(). Producers never create it.
[[nodiscard]] TraceRecorder* activeRecorder() noexcept;
/// The slot pools, or null before initialize().
[[nodiscard]] Pools* activePools() noexcept;

/// Enqueues one payload, or returns false when the recorder is not initialised or the queue is full.
template<class Payload>
bool enqueue(const RecordKind kind, const Payload& payload) noexcept {
    if (auto* recorder = activeRecorder()) return recorder->record(kind, payload);
    return false;
}

/// Formats one window into the text the analysers read. Shared by the writer and by the
/// profile-off fallback, which keeps the diagnostics path's stderr output unchanged.
void formatPacingWindow(const PacingWindowPayload& payload,
    const present_diagnostics::PacingProfile::Window& window, std::ostream& line);
void formatApplicationWindow(const ApplicationWindowPayload& payload,
    const present_diagnostics::ApplicationFrameTiming::Window& window, std::ostream& line);
void formatPresentWaitWindow(const PresentWaitWindowPayload& payload,
    const present_diagnostics::ApplicationPresentWait::Window& window, std::ostream& line);

/// Creates the recorder, slot pools, sink and writer thread once. Only the pacing profile calls it.
void initialize() noexcept;
/// Writes every queued record now, on the calling thread. Test hook and process exit.
void flushRecords() noexcept;
/// Rare one-off text: queued as an owned string the writer frees after writing.
void writeText(std::string text) noexcept;
/// Remembers a thread the writer should sample from /proc once per second as a deferred worker.
void registerWorkerThread(uint32_t thread) noexcept;
/// Adds one display-probe cost sample to the recorder record's per-second maximum and sum.
void noteDisplayProbeCost(uint64_t nanoseconds) noexcept;

}

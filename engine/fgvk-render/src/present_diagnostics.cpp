/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "present_diagnostics.hpp"
#include "present_records.hpp"
#include "trace_recorder.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <string_view>

#include <unistd.h>
#include <link.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifndef FGVK_BUILD_VERSION
#define FGVK_BUILD_VERSION "unknown"
#endif
#ifndef FGVK_BUILD_FINGERPRINT
#define FGVK_BUILD_FINGERPRINT FGVK_BUILD_VERSION
#endif

namespace fgvk::layer::present_diagnostics {
namespace {
// Per-thread state of the layer-call spans. Only the owning thread touches it.
struct LayerCallThreadState {
    uint32_t thread{};
    uint32_t depth{};
    // Layer, submit, wait and acquire time since this thread's previous present exit.
    std::array<int64_t, 4> inside{};
    Clock::time_point previousExit{};
    struct Aggregate {
        uint64_t count{};
        int64_t sumNs{};
        int64_t maxNs{};
    };
    std::array<Aggregate, static_cast<size_t>(LayerCall::Count)> aggregates{};
    Clock::time_point windowStart{};
    uint64_t boundContext{};
    uint64_t boundSource{};
};

LayerCallThreadState& layerThreadState() {
    thread_local LayerCallThreadState state;
    if (!state.thread) state.thread = trace_recorder::currentThreadId();
    return state;
}

constexpr int64_t layerCallRecordNs = 250'000;

size_t insideBucket(const LayerCall call) {
    switch (call) {
    case LayerCall::QueueSubmit2:
        return 1;
    case LayerCall::WaitSemaphores:
    case LayerCall::QueueWaitIdle:
    case LayerCall::DeviceWaitIdle:
    case LayerCall::WaitForPresent:
    case LayerCall::WaitForPresent2:
    case LayerCall::LatencySleep:
        return 2;
    case LayerCall::AcquireNextImage:
    case LayerCall::AcquireNextImage2:
    case LayerCall::ReleaseSwapchainImages:
    case LayerCall::GetSwapchainImages:
        return 3;
    default:
        return 0;
    }
}
}

void initializePacingProfileLog() noexcept {
    if (!pacingProfileEnabled()) return;
    present_records::initialize();
}

void flushPacingProfileForTests() noexcept {
    present_records::flushRecords();
}

namespace {
// D7: the steady-clock time of the latest application present. Only the pacing profile writes it.
std::atomic<int64_t>& lastApplicationPresent() {
    static std::atomic<int64_t> value{0};
    return value;
}
}

void noteApplicationPresent() {
    if (!pacingProfileEnabled()) return;
    lastApplicationPresent().store(std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
}

int64_t lastApplicationPresentNanoseconds() {
    return lastApplicationPresent().load(std::memory_order_relaxed);
}

namespace {
// D7c: the registry is read by the detached stall watchdog, which can still run while the
// process exits. It is therefore never destroyed.
FgTimelineLedger& fgTimelineLedger() {
    static auto* ledger = new FgTimelineLedger;
    return *ledger;
}

// A counter value, "errorN" when the query fails, or "unavailable" when the device has no
// counter query. The query does not wait for the GPU.
std::string fgCounterText(const FgTimelineLedger::Entry& entry) {
    if (!entry.counter)
        return "unavailable";
    uint64_t value{};
    const VkResult result = entry.counter(entry.device, entry.sync, &value);
    return result == VK_SUCCESS ? std::to_string(value)
        : "error" + std::to_string(static_cast<int>(result));
}
}

void FgTimelineLedger::noteSourceCopy(const uint64_t context, const VkDevice device,
        const VkSemaphore sync, const PFN_vkGetSemaphoreCounterValueKHR counter,
        const uint64_t source, const uint64_t sourceValue) {
    try {
        const std::lock_guard lock(mutex);
        Entry& entry = entries[context];
        entry.device = device;
        entry.sync = sync;
        entry.counter = counter;
        entry.recent[entry.next++ % recentCount] = {source, sourceValue, 0};
    } catch (...) {}
}

void FgTimelineLedger::noteStagingWait(const uint64_t context, const uint64_t value) {
    try {
        const std::lock_guard lock(mutex);
        const auto found = entries.find(context);
        if (found == entries.end() || found->second.next == 0)
            return;
        found->second.recent[(found->second.next - 1) % recentCount].stagingWait = value;
    } catch (...) {}
}

void FgTimelineLedger::forget(const uint64_t context) {
    try {
        const std::lock_guard lock(mutex);
        entries.erase(context);
    } catch (...) {}
}

std::string FgTimelineLedger::describe(const int64_t stalledNanoseconds) const {
    std::string text;
    try {
        const std::lock_guard lock(mutex);
        for (const auto& [context, entry] : entries) {
            std::ostringstream line;
            line << "framegen-vulkan: d7 fg stall_ms=" << stalledNanoseconds / 1'000'000
                 << " context=" << context << " sync_gpu=" << fgCounterText(entry) << " recent=";
            // The oldest record is at next (the slot overwritten next); empty slots are skipped.
            bool first = true;
            for (uint32_t step = 0; step < recentCount; ++step) {
                const FgRecord& record = entry.recent[(entry.next + step) % recentCount];
                if (record.sourceValue == 0)
                    continue;
                line << (first ? "" : " ") << record.source << ":S" << record.sourceValue << ":B";
                if (record.stagingWait != 0) line << record.stagingWait;
                else line << '-';
                first = false;
            }
            line << '\n';
            text += line.str();
        }
    } catch (...) {}
    return text;
}

size_t FgTimelineLedger::size() const {
    const std::lock_guard lock(mutex);
    return entries.size();
}

void noteFgSourceCopy(const uint64_t context, const VkDevice device, const VkSemaphore sync,
        const PFN_vkGetSemaphoreCounterValueKHR counter, const uint64_t source,
        const uint64_t sourceValue) {
    if (!pacingProfileEnabled())
        return;
    try {
        fgTimelineLedger().noteSourceCopy(context, device, sync, counter, source, sourceValue);
    } catch (...) {}
}

void noteFgStagingWait(const uint64_t context, const uint64_t value) {
    if (!pacingProfileEnabled())
        return;
    try {
        fgTimelineLedger().noteStagingWait(context, value);
    } catch (...) {}
}

void forgetFgTimeline(const uint64_t context) {
    if (!pacingProfileEnabled())
        return;
    try {
        fgTimelineLedger().forget(context);
    } catch (...) {}
}

void writeFgTimelineProbe(const int64_t stalledNanoseconds) {
    if (!pacingProfileEnabled())
        return;
    try {
        // The text is built under the registry lock and written after it is released, so a
        // slow stderr never holds the lock that the present thread needs.
        const std::string text = fgTimelineLedger().describe(stalledNanoseconds);
        std::cerr << text;
    } catch (...) {}
}

bool pacingFrameTraceEnabled() {
    static const bool value = [] {
        const char* environment = std::getenv("FGVK_PACING_PROFILE_TRACE");
        return (environment && std::string_view(environment) == "1");
    }();
    return value;
}

bool pacingGpuProfileEnabled() {
    static const bool value = [] {
        const char* environment = std::getenv("FGVK_PACING_PROFILE_GPU");
        // Not implied by the trace: backend GPU timing drains every in-flight
        // batch per source, which changes pacing.
        return (environment && std::string_view(environment) == "1");
    }();
    return value;
}

bool pacingGpuSplitProfileEnabled() {
    static const bool value = [] {
        // Read once per process with the CPU capture it correlates with. It implies neither
        // the trace nor the drained GPU profile, so it adds no wait to the presentation path.
        const char* profile = std::getenv("FGVK_PACING_PROFILE");
        const char* split = std::getenv("FGVK_PACING_PROFILE_GPU_SPLIT");
        return profile && std::string_view(profile) == "1" &&
            split && std::string_view(split) == "1";
    }();
    return value;
}

bool pacingGpuPassProfileEnabled() {
    static const bool value = [] {
        const char* passes = std::getenv("FGVK_PACING_PROFILE_GPU_PASSES");
        return pacingGpuSplitProfileEnabled() && passes && std::string_view(passes) == "1";
    }();
    return value;
}

bool pacingWaylandProfileEnabled() {
    static const bool value = [] {
        const char* environment = std::getenv("FGVK_PACING_PROFILE_WAYLAND");
        return (environment && std::string_view(environment) == "1") || pacingFrameTraceEnabled();
    }();
    return value;
}

void writePacingProfileRecords(const std::string_view records) noexcept {
    if (!pacingProfileEnabled() || records.empty()) return;
    try { present_records::writeText(std::string(records)); } catch (...) {}
}

namespace {
// Setup is observed only at creation, with bounded lists and no Vulkan calls.
// Unknown feature payloads deliberately remain unavailable; a type is not a value.
void setupChain(std::ostream& out, const void* next) {
    size_t count{};
    out << " pnext=";
    auto* node = static_cast<const VkBaseInStructure*>(next);
    for (; node && count < 256; node = node->pNext, ++count) {
        if (count) out << ',';
        out << static_cast<int>(node->sType);
    }
    if (!count) out << '-';
    out << " pnext_truncated=" << (node != nullptr);
    node = static_cast<const VkBaseInStructure*>(next);
    for (size_t i = 0; node && i < 256; node = node->pNext, ++i) {
#define FGVK_SETUP_FEATURE(stype, type, member) \
        case stype: out << " " #member "=" << reinterpret_cast<const type*>(node)->member; break
        switch (node->sType) {
            FGVK_SETUP_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, VkPhysicalDevicePresentIdFeaturesKHR, presentId);
            FGVK_SETUP_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, VkPhysicalDevicePresentWaitFeaturesKHR, presentWait);
            FGVK_SETUP_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, VkPhysicalDevicePresentId2FeaturesKHR, presentId2);
            FGVK_SETUP_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR, VkPhysicalDevicePresentWait2FeaturesKHR, presentWait2);
            FGVK_SETUP_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR, VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR, swapchainMaintenance1);
#ifdef VK_EXT_descriptor_heap
            FGVK_SETUP_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT, VkPhysicalDeviceDescriptorHeapFeaturesEXT, descriptorHeap);
#endif
#ifdef VK_NV_present_metering
            FGVK_SETUP_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_METERING_FEATURES_NV, VkPhysicalDevicePresentMeteringFeaturesNV, presentMetering);
#endif
            default: break;
        }
#undef FGVK_SETUP_FEATURE
    }
}

void setupExtensions(std::ostream& out, uint32_t count, const char* const* names) {
    out << " extension_count=" << count << " extensions=";
    const uint32_t retained = names ? std::min(count, 256u) : 0;
    for (uint32_t i = 0; i < retained; ++i) {
        if (i) out << ',';
        // Extension names have a Vulkan-defined maximum and token alphabet.
        const std::string_view name(names[i], strnlen(names[i], VK_MAX_EXTENSION_NAME_SIZE));
        for (const char c : name)
            out << ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' ? c : '_');
    }
    if (!retained) out << '-';
    out << " extensions_truncated=" << (retained != count);
}

std::ostringstream setupLine(std::string_view role, uint64_t call, std::string_view kind,
        std::string_view boundary, VkResult result) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "framegen-vulkan: present diagnostics: operation=pacing-profile-vulkan-setup version=1"
        << " pid=" << getpid() << " role=" << role << " call=" << call
        << " kind=" << kind << " boundary=" << boundary << " result=" << result;
    return out;
}
}

void traceVulkanSetup(std::string_view role, const VkInstanceCreateInfo& incoming,
        const VkInstanceCreateInfo& forwarded, VkResult result, VkInstance instance) noexcept {
    if (!pacingFrameTraceEnabled()) return;
    try {
        const auto call = allocateContextId();
        for (const auto boundary : {"incoming", "forwarded"}) {
            const auto& info = boundary == std::string_view("incoming") ? incoming : forwarded;
            auto out = setupLine(role, call, "instance", boundary, result);
            out << " handle=" << instance << " flags=" << info.flags
                << " api_version=" << (info.pApplicationInfo ? info.pApplicationInfo->apiVersion : 0);
            setupExtensions(out, info.enabledExtensionCount, info.ppEnabledExtensionNames);
            setupChain(out, info.pNext);
            out << '\n'; writePacingProfileRecords(out.str());
        }
    } catch (...) {}
}

void traceVulkanSetup(std::string_view role, const VkDeviceCreateInfo& incoming,
        const VkDeviceCreateInfo& forwarded, VkResult result, VkDevice device,
        VkPhysicalDevice physicalDevice) noexcept {
    if (!pacingFrameTraceEnabled()) return;
    try {
        const auto call = allocateContextId();
        for (const auto boundary : {"incoming", "forwarded"}) {
            const auto& info = boundary == std::string_view("incoming") ? incoming : forwarded;
            auto out = setupLine(role, call, "device", boundary, result);
            out << " handle=" << device << " physical_device=" << physicalDevice << " flags=" << info.flags;
            setupExtensions(out, info.enabledExtensionCount, info.ppEnabledExtensionNames);
            setupChain(out, info.pNext);
            out << " queue_count=" << info.queueCreateInfoCount << " queues=";
            const auto retained = info.pQueueCreateInfos ? std::min(info.queueCreateInfoCount, 64u) : 0;
            for (uint32_t i = 0; i < retained; ++i) {
                const auto& queue = info.pQueueCreateInfos[i];
                if (i) out << ',';
                out << queue.queueFamilyIndex << ':' << queue.queueCount << ':' << queue.flags;
            }
            if (!retained) out << '-';
            out << " queues_truncated=" << (retained != info.queueCreateInfoCount) << '\n';
            writePacingProfileRecords(out.str());
        }
    } catch (...) {}
}

void traceVulkanSetup(std::string_view role, const VkSwapchainCreateInfoKHR& incoming,
        const VkSwapchainCreateInfoKHR& forwarded, VkResult result, VkSwapchainKHR swapchain,
        VkDevice device) noexcept {
    if (!pacingFrameTraceEnabled()) return;
    try {
        const auto call = allocateContextId();
        for (const auto boundary : {"incoming", "forwarded"}) {
            const auto& info = boundary == std::string_view("incoming") ? incoming : forwarded;
            auto out = setupLine(role, call, "swapchain", boundary, result);
            out << " handle=" << swapchain << " device=" << device << " surface=" << info.surface
                << " old_swapchain=" << info.oldSwapchain << " flags=" << info.flags
                << " min_images=" << info.minImageCount << " format=" << info.imageFormat
                << " color_space=" << info.imageColorSpace << " width=" << info.imageExtent.width
                << " height=" << info.imageExtent.height << " layers=" << info.imageArrayLayers
                << " usage=" << info.imageUsage << " sharing=" << info.imageSharingMode
                << " transform=" << info.preTransform << " alpha=" << info.compositeAlpha
                << " present_mode=" << info.presentMode << " clipped=" << info.clipped;
            setupChain(out, info.pNext);
            out << '\n'; writePacingProfileRecords(out.str());
        }
    } catch (...) {}
}

void traceVulkanSurface(std::string_view role, std::string_view api,
        VkInstance instance, VkSurfaceKHR surface, VkResult result) noexcept {
    if (!pacingFrameTraceEnabled()) return;
    try {
        auto out = setupLine(role, allocateContextId(), "surface", "forwarded", result);
        out << " handle=" << surface << " instance=" << instance << " api=" << api << '\n';
        writePacingProfileRecords(out.str());
    } catch (...) {}
}

bool pacingProfileEnabled() {
    static const bool value = [] {
        const char* environment = std::getenv("FGVK_PACING_PROFILE");
        return (environment && std::string_view(environment) == "1") || pacingGpuProfileEnabled() || pacingWaylandProfileEnabled();
    }();
    return value;
}

void logPacingOutput(const uint64_t context, const VkSwapchainKHR swapchain,
        const PacingProfile::OutputSample& sample) noexcept {
    if (!pacingFrameTraceEnabled()) return;
    present_records::enqueue(trace_recorder::RecordKind::PacingOutput,
        present_records::PacingOutputPayload{context, swapchain, sample});
}

void logPacingSkip(const uint64_t context, const uint64_t source, const std::string_view reason,
        const Clock::time_point scheduled, const Clock::time_point now) noexcept {
    if (!pacingProfileEnabled()) return;
    present_records::PacingSkipPayload payload{.context = context, .source = source,
        .scheduled = scheduled, .now = now};
    const auto length = std::min(reason.size(), payload.reason.size() - 1);
    std::memcpy(payload.reason.data(), reason.data(), length);
    present_records::enqueue(trace_recorder::RecordKind::PacingSkip, payload);
}

void logPacingProfile(const uint64_t context, const VkPresentModeKHR mode,
        const Clock::time_point finished, const PacingProfile::Window& window,
        const VkSwapchainKHR swapchain, const std::string_view wsi, const bool waylandObserver) noexcept {
    try {
        present_records::PacingWindowPayload payload{.context = context, .swapchain = swapchain,
            .mode = mode, .finished = finished, .waylandObserver = waylandObserver};
        const auto length = std::min(wsi.size(), payload.wsi.size() - 1);
        std::memcpy(payload.wsi.data(), wsi.data(), length);
        auto* recorder = present_records::activeRecorder();
        auto* pools = present_records::activePools();
        if (!recorder || !pools) return;
        // The window is copied into a fixed slot; a full pool is a counted drop, never a wait.
        const size_t slot = pools->pacingWindows.claim(window);
        if (slot == decltype(pools->pacingWindows)::none) {
            recorder->noteSlotDrop();
            return;
        }
        payload.slot = slot;
        if (!present_records::enqueue(trace_recorder::RecordKind::PacingWindow, payload))
            pools->pacingWindows.release(slot);
    } catch (...) {}
}

void logPacingSource(const uint64_t context, const VkSwapchainKHR swapchain,
        const PacingProfile::SourceSample& sample) noexcept {
    if (!pacingProfileEnabled()) return;
    present_records::enqueue(trace_recorder::RecordKind::PacingSource,
        present_records::PacingSourcePayload{context, swapchain, trace_recorder::currentThreadId(), sample});
}

void logPacingGpuFrame(const uint64_t context, const uint64_t source, const uint64_t epoch,
        const uint64_t frame, const double previousWaitUs, const double prepassSubmitUs,
        const double generationSubmitUs, const double prepassUs, const double spanUs,
        const double readbackUs, const std::span<const double> generatedUs) noexcept {
    if (!pacingProfileEnabled()) return;
    present_records::PacingGpuPayload payload{.context = context, .source = source, .epoch = epoch,
        .frame = frame, .previousWaitUs = previousWaitUs, .prepassSubmitUs = prepassSubmitUs,
        .generationSubmitUs = generationSubmitUs, .prepassUs = prepassUs, .spanUs = spanUs,
        .readbackUs = readbackUs, .outputs = static_cast<uint32_t>(generatedUs.size())};
    const auto listed = std::min(generatedUs.size(), payload.generatedUs.size());
    std::copy_n(generatedUs.begin(), listed, payload.generatedUs.begin());
    present_records::enqueue(trace_recorder::RecordKind::PacingGpu, payload);
}

void reportPacingGpuSplitUnavailable(const std::string_view reason, const std::string_view detail) noexcept {
    static std::atomic<bool> reported{false};
    if (reported.exchange(true))
        return;
    try {
        std::cerr << "framegen-vulkan: GPU split profile unavailable; reason=" << reason
                  << "; " << detail << '\n';
    } catch (...) {}
}

void logPacingGpuSplitFrame(const uint64_t context, const uint64_t source, const uint64_t backendFrame,
        const int64_t gameDoneNs, const int64_t sourceCopyEndNs, const int64_t fgStartNs,
        const int64_t prepassEndNs, const int64_t generatedEndNs, const int64_t batchEndNs) noexcept {
    if (!pacingProfileEnabled()) return;
    present_records::enqueue(trace_recorder::RecordKind::PacingGpuSplit,
        present_records::PacingGpuSplitPayload{context, source, backendFrame, gameDoneNs,
            sourceCopyEndNs, fgStartNs, prepassEndNs, generatedEndNs, batchEndNs});
}

void logPacingGpuPass(const uint64_t context, const uint32_t step, const std::string_view name,
        const bool overlap, const uint64_t batches, const int64_t p50Ns, const int64_t p95Ns) noexcept {
    if (!pacingProfileEnabled()) return;
    try {
        present_records::PacingGpuPassPayload payload{.context = context, .batches = batches,
            .p50Ns = p50Ns, .p95Ns = p95Ns, .step = step, .overlap = overlap};
        const auto length = std::min(name.size(), payload.name.size() - 1);
        std::memcpy(payload.name.data(), name.data(), length);
        present_records::enqueue(trace_recorder::RecordKind::PacingGpuPass, payload);
    } catch (...) {}
}

void logPacingOutputGpu(const uint64_t context, const uint64_t source, const bool available,
        const int64_t copyStartNs, const int64_t copyEndNs, const int64_t stagingStartNs, const int64_t stagingEndNs) noexcept {
    if (!pacingProfileEnabled()) return;
    present_records::enqueue(trace_recorder::RecordKind::PacingOutputGpu,
        present_records::PacingOutputGpuPayload{context, source, available,
            available ? copyStartNs : 0, available ? copyEndNs : 0,
            available ? stagingStartNs : 0, available ? stagingEndNs : 0});
}

void logPacingRealCopyGpu(const uint64_t context, const uint64_t source, const bool available,
        const int64_t copyStartNs, const int64_t copyEndNs, const bool gameQueue) noexcept {
    if (!pacingProfileEnabled()) return;
    present_records::enqueue(trace_recorder::RecordKind::PacingRealCopyGpu,
        present_records::PacingOutputGpuPayload{context, source, available,
            available ? copyStartNs : 0, available ? copyEndNs : 0, 0, 0, gameQueue});
}

void logPacingPresentMarkerGpu(const uint64_t context, const uint64_t source, const uint64_t feedback,
        const bool generated, const bool available, const int64_t markerStartNs, const int64_t markerEndNs) noexcept {
    if (!pacingProfileEnabled()) return;
    present_records::enqueue(trace_recorder::RecordKind::PacingPresentMarkerGpu,
        present_records::PacingPresentMarkerPayload{context, source, feedback, generated, available,
            available ? markerStartNs : 0, available ? markerEndNs : 0});
}

SteamOverlayLibraries steamOverlayLibraries() {
    SteamOverlayLibraries libraries;
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* state) {
        auto& loaded = *static_cast<SteamOverlayLibraries*>(state);
        std::string_view name{info->dlpi_name ? info->dlpi_name : ""};
        const auto slash = name.find_last_of('/');
        if (slash != std::string_view::npos) name.remove_prefix(slash + 1);
        loaded.hookLoaded |= name == "gameoverlayrenderer.so";
        loaded.vulkanLoaded |= name == "steamoverlayvulkanlayer.so";
        return 0;
    }, &libraries);
    return libraries;
}

namespace { thread_local ApplicationAcquireScope* applicationAcquireScope{}; }

ApplicationAcquireScope::ApplicationAcquireScope() {
    // The F5 acquire split is a pacing-profile observation, so the scope exists for
    // either switch. Only the frame trace needs a call identity.
    if (!pacingFrameTraceEnabled() && !pacingProfileEnabled()) return;
    installed = true;
    previous = applicationAcquireScope;
    if (pacingFrameTraceEnabled()) observation.callId = allocateContextId();
    applicationAcquireScope = this;
}

ApplicationAcquireScope::~ApplicationAcquireScope() {
    if (installed) applicationAcquireScope = previous;
}

void recordPacingAcquire(const bool host, const Clock::time_point enter, const Clock::time_point exit,
        const Clock::duration lockWait, const uint64_t tailWaitValue, const Clock::time_point tailSeen) noexcept {
    if (!pacingProfileEnabled()) return;
    present_records::enqueue(trace_recorder::RecordKind::PacingAcquire, present_records::PacingAcquirePayload{
        .thread = trace_recorder::currentThreadId(), .enter = enter, .exit = exit, .host = host,
        .lockWaitNs = std::chrono::duration_cast<std::chrono::nanoseconds>(lockWait).count(),
        .tailWaitValue = tailWaitValue, .tailSeenNs = trace_recorder::nanoseconds(tailSeen)});
}

void bindApplicationAcquireContext(const uint64_t context) noexcept {
    if (applicationAcquireScope) applicationAcquireScope->observation.context = context;
}

void recordApplicationAcquireDriver(const uint64_t timeout, const VkResult result,
        const Clock::time_point started, const Clock::time_point finished,
        const std::optional<uint32_t> imageIndex) noexcept {
    if (!applicationAcquireScope) return;
    auto& observation = applicationAcquireScope->observation;
    ++observation.lowerCalls;
    observation.lowerDuration += finished - started;
    if (!pacingProfileEnabled()) return;
    present_records::PacingAcquireDriverPayload payload{.thread = trace_recorder::currentThreadId(),
        .callId = observation.callId, .attempt = observation.lowerCalls, .timeout = timeout,
        .started = started, .finished = finished, .result = result,
        .imageIndex = imageIndex.value_or(UINT32_MAX)};
    present_records::enqueue(trace_recorder::RecordKind::PacingAcquireDriver, payload);
}

void recordCopySubmission(const CopySubmission& info, const Clock::time_point started,
        const Clock::time_point finished, const bool submitted) noexcept {
    if (!pacingProfileEnabled()) return;
    auto* recorder = present_records::activeRecorder();
    auto* pools = present_records::activePools();
    if (!recorder || !pools) return;
    present_records::CopySlot slot{};
    slot.context = info.context;
    slot.source = info.source;
    slot.swapchain = info.swapchain;
    slot.generated = info.generated;
    slot.imageIndex = info.imageIndex;
    slot.pass = info.pass;
    slot.device = info.device;
    slot.queue = info.queue;
    slot.command = info.command;
    slot.started = started;
    slot.finished = finished;
    slot.submitted = submitted;
    slot.timelineWait = info.timelineWait;
    slot.waitValue = info.waitValue;
    slot.timelineSignal = info.timelineSignal;
    slot.signalValue = info.signalValue;
    slot.fence = info.fence;
    slot.thread = trace_recorder::currentThreadId();
    slot.waitCount = info.waits.size();
    slot.signalCount = info.signals.size();
    std::copy_n(info.waits.begin(), std::min(info.waits.size(), size_t{16}), slot.waits.begin());
    std::copy_n(info.signals.begin(), std::min(info.signals.size(), size_t{16}), slot.signals.begin());
    const size_t claimed = pools->copies.claim(slot);
    if (claimed == decltype(pools->copies)::none) {
        recorder->noteSlotDrop();
        return;
    }
    if (!present_records::enqueue(trace_recorder::RecordKind::PacingCopy,
            present_records::PacingCopyPayload{claimed}))
        pools->copies.release(claimed);
}

void recordApplicationFrameTiming(const ApplicationFrameApi api, const uintptr_t caller,
        const VkSwapchainKHR swapchain, const Clock::time_point started,
        const Clock::time_point finished, const std::optional<VkResult> result,
        const std::optional<uint64_t> timeout,
        const std::optional<Clock::duration> configurationUpdate,
        const std::optional<Clock::duration> deferredJoin,
        const std::optional<uint32_t> imageIndex, const uint64_t context, const uint64_t source,
        const ApplicationAcquireDetails* acquisition) {
    thread_local ApplicationFrameTiming presentObservations;
    thread_local uint64_t streamId = allocateContextId();
    thread_local const auto threadId = trace_recorder::currentThreadId();
    if (pacingFrameTraceEnabled()) {
        thread_local uint64_t ordinal{};
        present_records::PacingApplicationPayload payload{.thread = threadId, .stream = streamId,
            .ordinal = ++ordinal, .context = context, .source = source, .swapchain = swapchain,
            .api = api, .imageIndex = imageIndex.value_or(UINT32_MAX), .started = started,
            .finished = finished, .result = result, .timeout = timeout,
            .configurationUpdate = configurationUpdate, .deferredJoin = deferredJoin,
            .caller = caller, .callId = acquisition ? acquisition->callId : 0,
            .lowerCalls = acquisition ? acquisition->lowerCalls : 0,
            .lowerDuration = acquisition ? acquisition->lowerDuration : Clock::duration{}};
        present_records::enqueue(trace_recorder::RecordKind::PacingApplication, payload);
    }
    // Only presents have a window record. Acquires are recorded per call above, under the trace.
    if (api != ApplicationFrameApi::Present || !pacingProfileEnabled())
        return;
    const auto window = presentObservations.observe(
        caller, swapchain, started, finished, configurationUpdate, deferredJoin);
    if (!window)
        return;
    present_records::ApplicationWindowPayload payload{.thread = threadId, .stream = streamId,
        .caller = caller, .swapchain = swapchain, .finished = finished};
    auto* recorder = present_records::activeRecorder();
    auto* pools = present_records::activePools();
    if (!recorder || !pools) return;
    const size_t slot = pools->applicationWindows.claim(*window);
    if (slot == decltype(pools->applicationWindows)::none) {
        recorder->noteSlotDrop();
        return;
    }
    payload.slot = slot;
    if (!present_records::enqueue(trace_recorder::RecordKind::ApplicationPresentWindow, payload))
        pools->applicationWindows.release(slot);
}

namespace { thread_local ApplicationPresentScope* applicationPresentScope{}; }

void bindApplicationPresentSource(const uint64_t context, const uint64_t source) noexcept {
    if (applicationPresentScope) {
        applicationPresentScope->context = context;
        applicationPresentScope->source = source;
    }
    if (pacingProfileEnabled()) {
        auto& state = layerThreadState();
        state.boundContext = context;
        state.boundSource = source;
    }
}

ApplicationPresentScope::ApplicationPresentScope(const VkQueue queue,
        const VkSwapchainKHR swapchain, const std::optional<uint32_t> imageIndex) :
        previous(applicationPresentScope), imageIndex(imageIndex), active(pacingProfileEnabled()),
        caller(reinterpret_cast<uintptr_t>(queue)), swapchain(swapchain) {
    if (this->active)
        this->started = Clock::now();
    applicationPresentScope = this;
}

ApplicationPresentScope::~ApplicationPresentScope() {
    applicationPresentScope = this->previous;
    if (!this->active)
        return;
    // Diagnostics must not turn a completed application present into an error
    // while unwinding a presentation failure or a terminal allocation failure.
    try {
        recordApplicationFrameTiming(ApplicationFrameApi::Present, this->caller,
            this->swapchain, this->started, this->result ? this->finished : Clock::now(), this->result, {}, this->updateDuration, this->joinDuration, this->imageIndex, this->context, this->source);
    } catch (...) {
    }
}

void ApplicationPresentScope::configurationUpdateStarted() {
    if (this->active)
        this->updateStarted = Clock::now();
}

void ApplicationPresentScope::configurationUpdateFinished() {
    if (this->active)
        this->updateDuration = Clock::now() - this->updateStarted;
}

void ApplicationPresentScope::deferredJoinStarted() {
    if (this->active)
        this->joinStarted = Clock::now();
}

void ApplicationPresentScope::deferredJoinFinished() {
    if (this->active)
        this->joinDuration = Clock::now() - this->joinStarted;
}

void recordApplicationPresentWait(const VkDevice device, const VkSwapchainKHR swapchain,
        const PresentWaitApi api, const uint64_t presentId, const uint64_t timeout,
        const VkResult result, const Clock::time_point started,
        const Clock::time_point finished) {
    thread_local ApplicationPresentWait observations;
    thread_local uint64_t waiterId = allocateContextId();
    thread_local const auto threadId = trace_recorder::currentThreadId();
    if (pacingFrameTraceEnabled()) {
        present_records::PacingWaitPayload payload{.thread = threadId, .swapchain = swapchain,
            .api = api, .presentId = presentId, .timeout = timeout, .result = result,
            .started = started, .finished = finished};
        present_records::enqueue(trace_recorder::RecordKind::PacingWait, payload);
    }
    const auto window = observations.observe(device, swapchain, api,
        presentId, timeout, result, started, finished);
    if (!window)
        return;
    present_records::PresentWaitWindowPayload payload{.thread = threadId, .waiter = waiterId,
        .device = device, .swapchain = swapchain, .api = api, .finished = finished};
    if (!pacingProfileEnabled()) return;
    auto* recorder = present_records::activeRecorder();
    auto* pools = present_records::activePools();
    if (!recorder || !pools) return;
    const size_t slot = pools->presentWaitWindows.claim(*window);
    if (slot == decltype(pools->presentWaitWindows)::none) {
        recorder->noteSlotDrop();
        return;
    }
    payload.slot = slot;
    if (!present_records::enqueue(trace_recorder::RecordKind::ApplicationPresentWaitWindow, payload))
        pools->presentWaitWindows.release(slot);
}

void recordReflexMarker(const uint64_t presentId, const VkLatencyMarkerNV marker,
        const Clock::time_point at, const bool layer) noexcept {
    if (!pacingProfileEnabled()) return;
    present_records::enqueue(trace_recorder::RecordKind::ReflexMarker, present_records::ReflexMarkerPayload{
        trace_recorder::currentThreadId(), presentId, marker, trace_recorder::nanoseconds(at), layer});
}

void recordLayerReflexMarker(const uint64_t presentId, const VkLatencyMarkerNV marker) noexcept {
    recordReflexMarker(presentId, marker, Clock::now(), true);
}

void recordReflexSleep(const Clock::time_point entered, const Clock::time_point exited,
        const VkResult result) noexcept {
    if (!pacingProfileEnabled()) return;
    present_records::enqueue(trace_recorder::RecordKind::ReflexSleep, present_records::ReflexSleepPayload{
        trace_recorder::currentThreadId(), entered, exited, result});
}

void recordReflexTiming(const VkLatencyTimingsFrameReportNV& report, const int64_t queryNs) noexcept {
    if (!pacingProfileEnabled()) return;
    present_records::enqueue(trace_recorder::RecordKind::ReflexTiming, present_records::ReflexTimingPayload{
        .thread = trace_recorder::currentThreadId(), .queryNs = queryNs,
        .nowNs = trace_recorder::nanoseconds(Clock::now()), .presentId = report.presentID,
        .inputUs = report.inputSampleTimeUs, .simStartUs = report.simStartTimeUs, .simEndUs = report.simEndTimeUs,
        .submitStartUs = report.renderSubmitStartTimeUs, .submitEndUs = report.renderSubmitEndTimeUs,
        .presentStartUs = report.presentStartTimeUs, .presentEndUs = report.presentEndTimeUs,
        .driverStartUs = report.driverStartTimeUs, .driverEndUs = report.driverEndTimeUs,
        .osQueueStartUs = report.osRenderQueueStartTimeUs, .osQueueEndUs = report.osRenderQueueEndTimeUs,
        .gpuStartUs = report.gpuRenderStartTimeUs, .gpuEndUs = report.gpuRenderEndTimeUs});
}

void recordSubmitTag(const VkQueue queue, const uint64_t presentId, const SubmitTagTransition& transition) noexcept {
    if (!pacingProfileEnabled()) return;
    present_records::enqueue(trace_recorder::RecordKind::SubmitTag, present_records::SubmitTagPayload{
        .thread = trace_recorder::currentThreadId(), .nowNs = trace_recorder::nanoseconds(Clock::now()),
        .queue = queue, .presentId = presentId, .previousId = transition.previousTag,
        .submits = transition.submits, .untagged = transition.untagged});
}

void recordReflexMode(const VkBool32 lowLatency, const VkBool32 boost,
        const uint32_t minimumIntervalUs, const VkResult result) noexcept {
    if (!pacingProfileEnabled()) return;
    present_records::enqueue(trace_recorder::RecordKind::ReflexMode, present_records::ReflexModePayload{
        lowLatency, boost, minimumIntervalUs, result});
}

bool recordWaylandSample(const uint64_t token, const WaylandSample& sample) noexcept {
    if (!pacingProfileEnabled()) return false;
    return present_records::enqueue(trace_recorder::RecordKind::WaylandSample,
        present_records::WaylandSamplePayload{token, sample});
}

bool recordWaylandGap(const uint64_t token, const uint64_t lost) noexcept {
    if (!pacingProfileEnabled()) return false;
    return present_records::enqueue(trace_recorder::RecordKind::WaylandGap,
        present_records::WaylandGapPayload{token, lost});
}

void recordDisplayProbeCost(const uint64_t nanoseconds) noexcept {
    if (!pacingProfileEnabled()) return;
    present_records::noteDisplayProbeCost(nanoseconds);
}

bool pacingDisplayProbeEnabled() {
    static const bool value = [] {
        const char* display = std::getenv("FGVK_PACING_PROFILE_DISPLAY");
        return pacingProfileEnabled() && !(display && std::string_view(display) == "0");
    }();
    return value;
}

void registerDeferredWorkerThread() noexcept {
    if (!pacingProfileEnabled()) return;
    present_records::registerWorkerThread(trace_recorder::currentThreadId());
}

uint64_t lowerPresentId(const void* chain) noexcept {
    // Bounded like the worker's own chain walk: the present's pNext holds at most a few nodes.
    for (unsigned depth = 0; chain && depth < 8; ++depth) {
        VkStructureType type{};
        std::memcpy(&type, chain, sizeof(type));
        if (type == VK_STRUCTURE_TYPE_PRESENT_ID_KHR) {
            VkPresentIdKHR node{};
            std::memcpy(&node, chain, sizeof(node));
            return node.swapchainCount >= 1 && node.pPresentIds ? node.pPresentIds[0] : 0;
        }
#if defined(VK_KHR_present_id2)
        if (type == VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR) {
            VkPresentId2KHR node{};
            std::memcpy(&node, chain, sizeof(node));
            return node.swapchainCount >= 1 && node.pPresentIds ? node.pPresentIds[0] : 0;
        }
#endif
        const void* next{};
        std::memcpy(&next, static_cast<const std::byte*>(chain) + offsetof(VkBaseInStructure, pNext), sizeof(next));
        chain = next;
    }
    return 0;
}

LayerCallSpan::LayerCallSpan(const LayerCall callKind) noexcept : call(callKind) {
    if (!pacingProfileEnabled()) return;
    active = true;
    auto& state = layerThreadState();
    outermost = state.depth == 0;
    ++state.depth;
    if (call == LayerCall::QueuePresent) {
        inside = state.inside;
        previousExit = state.previousExit;
    }
    enter = Clock::now();
}

LayerCallSpan::~LayerCallSpan() {
    if (!active) return;
    try {
        const auto exit = Clock::now();
        auto& state = layerThreadState();
        --state.depth;
        const int64_t duration = trace_recorder::nanoseconds(exit) - trace_recorder::nanoseconds(enter);
        auto& aggregate = state.aggregates[static_cast<size_t>(call)];
        ++aggregate.count;
        aggregate.sumNs += duration;
        aggregate.maxNs = std::max(aggregate.maxNs, duration);
        if (call == LayerCall::QueuePresent) {
            present_records::PacingAppFramePayload frame{.thread = state.thread,
                .context = state.boundContext, .source = state.boundSource,
                .enter = enter, .exit = exit, .previousExit = previousExit, .insideLayer = inside[0],
                .insideSubmit = inside[1], .insideWait = inside[2], .insideAcquire = inside[3]};
            present_records::enqueue(trace_recorder::RecordKind::PacingAppFrame, frame);
            state.inside = {};
            state.previousExit = exit;
            state.boundContext = 0;
            state.boundSource = 0;
        } else if (outermost) {
            state.inside[0] += duration;
            if (const auto bucket = insideBucket(call)) state.inside[bucket] += duration;
        }
        if (duration >= layerCallRecordNs) {
            present_records::PacingCallPayload record{.thread = state.thread, .call = call,
                .enter = enter, .exit = exit, .result = result, .presentId = presentId,
                .timeoutNs = timeoutNs, .waitCount = waitCount};
            present_records::enqueue(trace_recorder::RecordKind::PacingCall, record);
        }
        if (!state.windowStart.time_since_epoch().count()) {
            state.windowStart = exit;
        } else if (exit - state.windowStart >= std::chrono::seconds(1)) {
            for (size_t index = 0; index < state.aggregates.size(); ++index) {
                auto& window = state.aggregates[index];
                if (!window.count) continue;
                present_records::PacingCallWindowPayload record{.thread = state.thread,
                    .call = static_cast<LayerCall>(index), .count = window.count,
                    .sumNs = window.sumNs, .maxNs = window.maxNs};
                present_records::enqueue(trace_recorder::RecordKind::PacingCallWindow, record);
                window = {};
            }
            state.windowStart = exit;
        }
    } catch (...) {}
}

namespace {
    std::atomic<uint32_t> nextContextSequence{1};
    thread_local uint64_t activeContextId{0};
    thread_local PacingProfile* activePacingProfile{};
}

uint64_t allocateContextId() {
    const auto processId = static_cast<uint64_t>(
        static_cast<uint32_t>(::getpid())
    );
    const auto sequence = static_cast<uint64_t>(
        nextContextSequence.fetch_add(1, std::memory_order_relaxed)
    );
    return (processId << 32) | sequence;
}

ContextScope::ContextScope(const uint64_t contextId, PacingProfile* profile) :
        previousContextId(activeContextId), previousPacingProfile(activePacingProfile) {
    activeContextId = contextId;
    activePacingProfile = profile;
}

ContextScope::~ContextScope() {
    activeContextId = this->previousContextId;
    activePacingProfile = this->previousPacingProfile;
}

void recordPacingProfilePhases(const uint64_t context, const std::array<Clock::duration, 9>& phases) {
    if (activePacingProfile && context == activeContextId)
        activePacingProfile->phaseTiming(phases);
}

}

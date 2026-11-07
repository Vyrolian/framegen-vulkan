/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "present_diagnostics.hpp"
#include "native_copy_profile.hpp"

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

#ifndef MAKO_BUILD_VERSION
#define MAKO_BUILD_VERSION "unknown"
#endif
#ifndef MAKO_BUILD_FINGERPRINT
#define MAKO_BUILD_FINGERPRINT MAKO_BUILD_VERSION
#endif

namespace mako::layer::present_diagnostics {
void NativeCopyProfile::publish(const Sample& sample) noexcept {
    try {
        const auto ns = [](auto time) { return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count(); };
        std::ostringstream line;
        line.imbue(std::locale::classic());
        line << std::setprecision(std::numeric_limits<float>::max_digits10)
             << "MAKO Renderer: present diagnostics: operation=pacing-profile-copy-gpu version=1"
             << " pid=" << getpid() << " context=" << sample.context << " source=" << sample.source
             << " generation=" << sample.generation << " swapchain=" << sample.swapchain
             << " device=" << sample.device << " queue=" << sample.queue << " command=" << sample.command
             << " slot=" << sample.slot << " generated=" << sample.generated << " image_index=" << sample.image
             << " query_result=" << sample.queryResult << " available=" << sample.available
             << " begin_ticks=" << sample.begin << " end_ticks=" << sample.end
             << " valid_bits=" << sample.validBits << " period_ns=" << sample.period
             << " collected_ns=" << ns(sample.collected) << " read_started_ns=" << ns(sample.readStarted)
             << " read_finished_ns=" << ns(sample.readFinished);
        const auto calibration = [&](const char* name, const Calibration& value) {
            line << ' ' << name << "_result=" << value.result << ' ' << name << "_device_ticks=" << value.device
                 << ' ' << name << "_host_ns=" << value.host << ' ' << name << "_deviation_ns=" << value.deviation
                 << ' ' << name << "_started_ns=" << ns(value.started) << ' ' << name << "_finished_ns=" << ns(value.finished);
        };
        calibration("before", sample.before);
        calibration("after", sample.after);
        line << '\n';
        writePacingProfileRecords(line.str());
    } catch (...) {}
}
namespace {
// Read the mapped ELF note of this very DSO, not a cached CMake fingerprint
// or a pathname whose file may already have been replaced during development.
std::string loadedRendererBuildId() {
    struct State { uintptr_t address; std::string id; } state{
        reinterpret_cast<uintptr_t>(&pacingFrameTraceEnabled), "unavailable"};
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) {
        auto& state = *static_cast<State*>(data);
        bool contains{};
        for (size_t i = 0; i < info->dlpi_phnum; ++i) {
            const auto& segment = info->dlpi_phdr[i];
            const auto begin = info->dlpi_addr + segment.p_vaddr;
            if (segment.p_type == PT_LOAD && state.address >= begin && state.address - begin < segment.p_memsz)
                contains = true;
        }
        if (!contains) return 0;
        for (size_t i = 0; i < info->dlpi_phnum; ++i) {
            const auto& segment = info->dlpi_phdr[i];
            if (segment.p_type != PT_NOTE || segment.p_memsz > 65536) continue;
            const auto* bytes = reinterpret_cast<const unsigned char*>(info->dlpi_addr + segment.p_vaddr);
            size_t offset{};
            while (offset <= segment.p_memsz && segment.p_memsz - offset >= sizeof(ElfW(Nhdr))) {
                ElfW(Nhdr) note{};
                std::memcpy(&note, bytes + offset, sizeof(note));
                offset += sizeof(note);
                const size_t nameBytes = (static_cast<size_t>(note.n_namesz) + 3) & ~size_t{3};
                const size_t valueBytes = (static_cast<size_t>(note.n_descsz) + 3) & ~size_t{3};
                if (nameBytes > segment.p_memsz - offset || valueBytes > segment.p_memsz - offset - nameBytes) break;
                if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 && note.n_descsz && note.n_descsz <= 64 &&
                        std::memcmp(bytes + offset, "GNU", 4) == 0) {
                    constexpr char hex[] = "0123456789abcdef";
                    state.id.clear();
                    for (size_t n = 0; n < note.n_descsz; ++n) {
                        const auto value = bytes[offset + nameBytes + n];
                        state.id += hex[value >> 4]; state.id += hex[value & 15];
                    }
                    return 1;
                }
                offset += nameBytes + valueBytes;
            }
        }
        return 1;
    }, &state);
    return state.id;
}

class PacingLog {
public:
    PacingLog() noexcept {
        try {
            retainBeginning = pacingWaylandProfileEnabled();
            byteLimit = retainBeginning ? 64 * 1024 * 1024 : 16 * 1024 * 1024;
            const char* requested = std::getenv("MAKO_PACING_PROFILE_LOG");
            if (requested && std::string_view(requested) == "stderr") {
                descriptor = STDERR_FILENO;
                return;
            }
            std::filesystem::path path;
            if (requested && *requested) {
                path = requested;
                if (!path.is_absolute()) return;
            } else {
                const char* temporary = std::getenv("TMPDIR");
                std::filesystem::path directory = temporary && *temporary ? temporary : "/tmp";
                if (!directory.is_absolute()) directory = "/tmp";
                path = directory / ("mako-pacing-" + std::to_string(geteuid()) + "-" +
                    std::to_string(getpid()) + "-" + std::to_string(Clock::now().time_since_epoch().count()) + ".log");
            }
            // Exclusive creation refuses old files, symlinks, FIFOs and devices.
            // This log contains only MAKO timing, never launcher/game output.
            descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
            if (descriptor < 0) {
                std::cerr << "MAKO Renderer: pacing profile capture unavailable; choose a new absolute MAKO_PACING_PROFILE_LOG path\n";
                return;
            }
            owned = true;
            std::cerr << "MAKO Renderer: pacing profile capture: path=" << path
                      << "; observations=cpu-submission; gpu_requested=" << pacingGpuProfileEnabled()
                      << "; limit_bytes=" << byteLimit << "; retention=" << (retainBeginning ? "stop" : "wrap") << '\n';
            std::ostringstream identity;
            identity << "MAKO Renderer: present diagnostics: operation=pacing-profile-session version=2 pid="
                     << getpid() << " build=" << MAKO_BUILD_VERSION << " fingerprint=" << MAKO_BUILD_FINGERPRINT
                     << " renderer_build_id=" << loadedRendererBuildId()
                     << " capture_limit_bytes=" << byteLimit << " retention=" << (retainBeginning ? "stop" : "wrap")
                     << " wayland_requested=" << pacingWaylandProfileEnabled()
                     << " frame_trace=" << pacingFrameTraceEnabled() << '\n';
            write(identity.str());
        } catch (...) {
            if (owned) ::close(descriptor);
            descriptor = -1;
            owned = false;
        }
    }
    ~PacingLog() { if (owned) ::close(descriptor); }
    void write(const std::string_view line) {
        const std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock()) { dropped.fetch_add(1, std::memory_order_relaxed); return; }
        if (descriptor < 0) return;
        if (owned && bytes + line.size() + 256 > byteLimit) {
            if (retainBeginning) {
                std::array<char, 256> marker{};
                const auto size = std::snprintf(marker.data(), marker.size(),
                    "MAKO Renderer: present diagnostics: operation=pacing-profile-capture-limit version=1 pid=%ld limit_bytes=%zu captured_bytes=%zu rejected_bytes=%zu\n",
                    static_cast<long>(getpid()), byteLimit, bytes, line.size());
                static_cast<void>(::write(descriptor, marker.data(), static_cast<size_t>(size)));
                ::close(descriptor);
                descriptor = -1;
                owned = false;
                return; // Preserve the session header and every earlier phase.
            }
            if (::ftruncate(descriptor, 0) != 0 || ::lseek(descriptor, 0, SEEK_SET) < 0) {
                ::close(descriptor); descriptor = -1; owned = false;
                return;
            }
            bytes = 0;
            constexpr std::string_view marker = "MAKO Renderer: present diagnostics: operation=pacing-profile-log-wrap version=2\n";
            const auto count = ::write(descriptor, marker.data(), marker.size());
            if (count > 0) bytes += static_cast<size_t>(count);
        }
        // No flush-per-field or retry loop. Publication time is measured by
        // the caller, so a slow output sink remains visible in the next window.
        const auto count = ::write(descriptor, line.data(), line.size());
        if (count > 0) bytes += static_cast<size_t>(count);
        if (count != static_cast<ssize_t>(line.size())) dropped.fetch_add(1, std::memory_order_relaxed);
        const auto lost = dropped.exchange(0, std::memory_order_relaxed);
        if (lost) {
            std::array<char, 160> marker{};
            const auto size = std::snprintf(marker.data(), marker.size(),
                "\nMAKO Renderer: present diagnostics: operation=pacing-profile-capture-gap lost_records=%zu\n", lost);
            const auto recorded = ::write(descriptor, marker.data(), static_cast<size_t>(size));
            if (recorded > 0) bytes += static_cast<size_t>(recorded);
        }
    }
private:
    size_t byteLimit{16 * 1024 * 1024};
    int descriptor{-1};
    size_t bytes{};
    bool owned{};
    bool retainBeginning{};
    std::mutex mutex;
    std::atomic<size_t> dropped{};
};

PacingLog& pacingLog() {
    static PacingLog output;
    return output;
}
}

void initializePacingProfileLog() noexcept {
    if (!pacingProfileEnabled()) return;
    try { (void)pacingLog(); } catch (...) {}
}

bool pacingFrameTraceEnabled() {
    static const bool value = [] {
        const char* environment = std::getenv("MAKO_PACING_PROFILE_TRACE");
        return environment && std::string_view(environment) == "1";
    }();
    return value;
}

bool pacingGpuProfileEnabled() {
    static const bool value = [] {
        const char* environment = std::getenv("MAKO_PACING_PROFILE_GPU");
        return (environment && std::string_view(environment) == "1") || pacingFrameTraceEnabled();
    }();
    return value;
}

bool pacingWaylandProfileEnabled() {
    static const bool value = [] {
        const char* environment = std::getenv("MAKO_PACING_PROFILE_WAYLAND");
        return (environment && std::string_view(environment) == "1") || pacingFrameTraceEnabled();
    }();
    return value;
}

void writePacingProfileRecords(const std::string_view records) noexcept {
    if (!pacingProfileEnabled() || records.empty()) return;
    try { pacingLog().write(records); } catch (...) {}
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
#define MAKO_SETUP_FEATURE(stype, type, member) \
        case stype: out << " " #member "=" << reinterpret_cast<const type*>(node)->member; break
        switch (node->sType) {
            MAKO_SETUP_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, VkPhysicalDevicePresentIdFeaturesKHR, presentId);
            MAKO_SETUP_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, VkPhysicalDevicePresentWaitFeaturesKHR, presentWait);
            MAKO_SETUP_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, VkPhysicalDevicePresentId2FeaturesKHR, presentId2);
            MAKO_SETUP_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR, VkPhysicalDevicePresentWait2FeaturesKHR, presentWait2);
            MAKO_SETUP_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR, VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR, swapchainMaintenance1);
#ifdef VK_EXT_descriptor_heap
            MAKO_SETUP_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT, VkPhysicalDeviceDescriptorHeapFeaturesEXT, descriptorHeap);
#endif
#ifdef VK_NV_present_metering
            MAKO_SETUP_FEATURE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_METERING_FEATURES_NV, VkPhysicalDevicePresentMeteringFeaturesNV, presentMetering);
#endif
            default: break;
        }
#undef MAKO_SETUP_FEATURE
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
    out << "MAKO Renderer: present diagnostics: operation=pacing-profile-vulkan-setup version=1"
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
        const char* environment = std::getenv("MAKO_PACING_PROFILE");
        const char* content = std::getenv("MAKO_PACING_PROFILE_CONTENT");
        return (environment && std::string_view(environment) == "1") || pacingGpuProfileEnabled() || pacingWaylandProfileEnabled() ||
            (content && std::string_view(content) == "1");
    }();
    return value;
}

namespace {
void appendPacingOutput(std::ostream& line, const uint64_t context, const VkSwapchainKHR swapchain,
        const PacingProfile::OutputSample& sample) {
    const auto ns = [](auto time) { return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count(); };
    const auto& observed = sample.observation;
    line << "MAKO Renderer: present diagnostics: operation=pacing-profile-output"
         << " version=" << (pacingFrameTraceEnabled() ? 2 : 1) << " context=" << context << " pid=" << getpid() << " swapchain=" << swapchain
         << " ordinal=" << sample.ordinal << " source=" << sample.source
         << " generated=" << observed.generated << " deferred=" << observed.deferred
         << " image_index=" << observed.imageIndex
         << " scheduled_ns=" << ns(observed.scheduled)
         << " started_ns=" << ns(sample.started) << " finished_ns=" << ns(sample.finished)
         << " result=" << sample.result << " copy_checked=" << observed.copyChecked
         << " copy_status=" << observed.copyStatus
         << " copy_sampled_ns=" << ns(observed.copySampledAt);
    if (pacingFrameTraceEnabled())
        line << " enqueued_ns=" << ns(observed.enqueued)
             << " queue_lock_started_ns=" << ns(observed.queueLockStarted)
             << " queue_lock_acquired_ns=" << ns(observed.queueLockAcquired)
             << " ready_checked=" << observed.readinessChecked
             << " ready_started_ns=" << ns(observed.readinessStarted)
             << " ready_finished_ns=" << ns(observed.readinessFinished)
             << " ready_deadline_ns=" << ns(observed.readinessDeadline)
             << " ready_result=" << observed.readinessResult
             << " queue=" << observed.queue << " present_wait=" << observed.presentWait;
    line << '\n';
}
}

void logPacingOutput(const uint64_t context, const VkSwapchainKHR swapchain,
        const PacingProfile::OutputSample& sample) noexcept {
    if (!pacingFrameTraceEnabled()) return;
    try {
        std::ostringstream line;
        line.imbue(std::locale::classic());
        appendPacingOutput(line, context, swapchain, sample);
        writePacingProfileRecords(line.str());
    } catch (...) {}
}

void logPacingProfile(const uint64_t context, const VkPresentModeKHR mode,
        const Clock::time_point finished, const PacingProfile::Window& window,
        const VkSwapchainKHR swapchain, const std::string_view wsi, const bool waylandObserver) noexcept {
    try {
        constexpr std::array<std::string_view, PacingProfile::MetricCount> names{
            "source_interval", "layer_present", "outside_layer", "output_interval",
            "within_batch", "between_batches", "lower_present", "source_cap_wait",
            "readiness_wait", "output_wait", "profiler_publish", "planned_output_interval",
            "output_wake_lateness", "source_cap_wake_lateness", "source_deadline_lateness",
            "gpu_prepass", "gpu_generation", "gpu_output", "gpu_span", "gpu_readback",
            "present_target_interval"};
        std::ostringstream line;
        line.imbue(std::locale::classic());
        line << "MAKO Renderer: present diagnostics: operation=pacing-profile"
             << " version=2 context=" << context << " pid=" << getpid()
             << " sample_end_monotonic_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(finished.time_since_epoch()).count()
             << " window_ms=" << std::chrono::duration<double, std::milli>(window.elapsed).count()
             << " present_mode=" << mode << " sources=" << window.sources
             << " swapchain=" << swapchain
             << " wsi=" << wsi << " wayland_requested=" << pacingWaylandProfileEnabled()
             << " wayland_observer=" << waylandObserver
             << " output_attempts=" << window.outputs << " output_errors=" << window.errors
             << " ready_batches=" << window.readyBatches << " cap_batches=" << window.capBatches
             << " worker_batches=" << window.workerBatches
             << " worker_readiness_misses=" << window.workerReadinessMisses
             << " output_trace_version=" << (pacingFrameTraceEnabled() ? 2 : 1) << " output_trace_samples=" << (pacingFrameTraceEnabled() ? window.outputs : window.outputTraceCount)
             << " output_trace_dropped=" << (pacingFrameTraceEnabled() ? 0 : window.outputTraceDropped)
             << " wait_capture=1"
             << " fifo_caller_timing_sources=" << window.fifoCallerTimingSources
             << " smooth_cadence=" << window.smoothCadence
             << " effective_base_cap_fps=" << window.effectiveCapFps
             << " frame_generation_enabled=" << window.frameGenerationEnabled
             << " adaptive=" << window.adaptive
             << " configured_multiplier=" << window.configuredMultiplier
             << " configured_base_cap_fps=" << window.configuredCapFps;
        line << " gpu_requested=" << pacingGpuProfileEnabled();
        constexpr std::array<std::string_view, PacingProfile::OutputTimingCount> timingNames{
            "driver", "application", "ext", "google", "bridge"};
        for (size_t i = 0; i < timingNames.size(); ++i)
            line << " timing_" << timingNames[i] << "_attempts=" << window.timingAttempts[i];
        line << " present_target_discontinuities=" << window.targetDiscontinuities;
        const auto ms = [](Clock::duration duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        };
        const auto& hitch = window.worstSource;
        line << " worst_source_previous_frame=" << hitch.previousFrame
             << " worst_source_interval_ms=" << ms(hitch.interval)
             << " worst_source_layer_ms=" << ms(hitch.layer)
             << " worst_source_outside_ms=" << ms(hitch.outside)
             << " worst_source_cap_wait_ms=" << ms(hitch.capWait)
             << " worst_source_readiness_wait_ms=" << ms(hitch.readinessWait)
             << " worst_source_output_wait_ms=" << ms(hitch.outputWait)
             << " worst_source_profiler_publish_ms=" << ms(hitch.profilerPublish);
        constexpr std::array<std::string_view, 9> phaseNames{
            "private_total", "render_fence", "schedule", "source_copy", "acquire",
            "generated_submit", "generated_present", "original_present", "unattributed"};
        for (size_t index = 0; index < phaseNames.size(); ++index) {
            line << " worst_source_" << phaseNames[index] << "_ms=" << ms(hitch.phases[index])
                 << ' ' << phaseNames[index] << "_mean_ms=" << window.phases[index].mean()
                 << ' ' << phaseNames[index] << "_max_ms=" << window.phases[index].maximum;
        }
        for (size_t index = 0; index < names.size(); ++index) {
            const auto& sample = window.metrics[index];
            line << ' ' << names[index] << "_samples=" << sample.count
                 << ' ' << names[index] << "_min_ms=" << sample.minimum
                 << ' ' << names[index] << "_mean_ms=" << sample.mean()
                 << ' ' << names[index] << "_max_ms=" << sample.maximum
                 << ' ' << names[index] << "_over_33ms=" << sample.over33ms;
        }
        line << '\n';
        const auto ns = [](Clock::time_point time) {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
        };
        for (size_t i = 0; i < window.outputTraceCount; ++i) {
            const auto& sample = window.outputTrace[i];
            if (!pacingFrameTraceEnabled()) appendPacingOutput(line, context, swapchain, sample);
        }
        if (pacingProfileEnabled()) pacingLog().write(line.str());
        else std::cerr << line.str();
    } catch (...) {}
}

void logPacingSource(const uint64_t context, const VkSwapchainKHR swapchain,
        const PacingProfile::SourceSample& sample) noexcept {
    try {
        const auto ns = [](auto value) { return std::chrono::duration_cast<std::chrono::nanoseconds>(value).count(); };
        std::ostringstream line;
        line.imbue(std::locale::classic());
        line << "MAKO Renderer: present diagnostics: operation=pacing-profile-source version=1"
             << " pid=" << getpid() << " tid=" << gettid() << " context=" << context << " swapchain=" << swapchain
             << " source=" << sample.source << " started_ns=" << ns(sample.started.time_since_epoch())
             << " finished_ns=" << ns(sample.finished.time_since_epoch())
             << " backend_scheduled=" << sample.backendScheduled << " backend_epoch=" << sample.backendEpoch
             << " backend_frame=" << sample.backendFrame
             << " frame_generation_enabled=" << sample.frameGeneration << " adaptive=" << sample.adaptive
             << " multiplier=" << sample.multiplier << " smooth_cadence=" << sample.smooth
             << " effective_cap_fps=" << sample.cap << " configured_cap_fps=" << sample.configuredCap;
        constexpr std::array<std::string_view, 3> waitNames{"cap", "readiness", "output"};
        for (size_t i = 0; i < waitNames.size(); ++i)
            line << ' ' << waitNames[i] << "_wait_ns=" << ns(sample.waits[i]);
        constexpr std::array<std::string_view, 9> names{
            "private_total", "render_fence", "schedule", "source_copy", "acquire",
            "generated_submit", "generated_present", "original_present", "unattributed"};
        for (size_t i = 0; i < names.size(); ++i) line << ' ' << names[i] << "_ns=" << ns(sample.phases[i]);
        line << '\n';
        writePacingProfileRecords(line.str());
    } catch (...) {}
}

void logPacingGpuFrame(const uint64_t context, const uint64_t source, const uint64_t epoch,
        const uint64_t frame, const double previousWaitUs, const double prepassSubmitUs,
        const double generationSubmitUs, const double prepassUs, const double spanUs,
        const double readbackUs, const std::span<const double> generatedUs) noexcept {
    try {
        std::ostringstream line;
        line.imbue(std::locale::classic());
        line << "MAKO Renderer: present diagnostics: operation=pacing-profile-gpu version=1"
             << " pid=" << getpid() << " context=" << context << " source=" << source
             << " backend_epoch=" << epoch << " backend_frame=" << frame
             << " previous_wait_us=" << previousWaitUs << " prepass_submit_us=" << prepassSubmitUs
             << " generation_submit_us=" << generationSubmitUs << " prepass_us=" << prepassUs
             << " span_us=" << spanUs << " readback_us=" << readbackUs << " outputs=" << generatedUs.size();
        for (size_t i = 0; i < generatedUs.size(); ++i) line << " output_" << i << "_us=" << generatedUs[i];
        line << '\n';
        writePacingProfileRecords(line.str());
    } catch (...) {}
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

void recordPresentPhaseTiming(const uint64_t context, const size_t frame,
        const size_t sequence, const Clock::time_point finished,
        const std::array<double, 9>& milliseconds) noexcept {
    try {
        thread_local PresentPhaseTiming observation;
        thread_local const auto threadId = gettid();
        const auto window = observation.observe(context, finished, milliseconds);
        if (!window) return;
        constexpr std::array<std::string_view, 9> names{
            "total", "render_fence", "schedule", "source_copy", "acquire",
            "generated_submit", "generated_present", "original_present", "unattributed"};
        std::ostringstream line;
        line << "MAKO Renderer: present diagnostics: operation=present-phase-summary"
             << " context=" << context << " pid=" << getpid() << " tid=" << threadId
             << " sample_end_monotonic_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(finished.time_since_epoch()).count()
             << " calls=" << window->calls << " frame=" << frame << " sequence=" << sequence;
        for (size_t index = 0; index < names.size(); ++index)
            line << ' ' << names[index] << "_mean_ms=" << window->phases[index].mean()
                 << ' ' << names[index] << "_max_ms=" << window->phases[index].maximum;
        line << '\n';
        std::cerr << line.str();
    } catch (...) {}
}

namespace { thread_local ApplicationAcquireScope* applicationAcquireScope{}; }

ApplicationAcquireScope::ApplicationAcquireScope() {
    if (!pacingFrameTraceEnabled()) return;
    previous = applicationAcquireScope;
    observation.callId = allocateContextId();
    applicationAcquireScope = this;
}

ApplicationAcquireScope::~ApplicationAcquireScope() {
    if (observation.callId) applicationAcquireScope = previous;
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
    try {
        const auto ns = [](auto time) { return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count(); };
        std::ostringstream line;
        line.imbue(std::locale::classic());
        line << "MAKO Renderer: present diagnostics: operation=pacing-profile-acquire-driver version=1"
             << " pid=" << getpid() << " tid=" << gettid() << " call_id=" << observation.callId
             << " attempt=" << observation.lowerCalls << " timeout_ns=" << timeout
             << " started_ns=" << ns(started) << " finished_ns=" << ns(finished)
             << " result=" << result << " image_index=" << imageIndex.value_or(UINT32_MAX) << '\n';
        writePacingProfileRecords(line.str());
    } catch (...) {}
}

void recordCopySubmission(const CopySubmission& info, const Clock::time_point started,
        const Clock::time_point finished, const bool submitted) noexcept {
    try {
        const auto ns = [](auto time) { return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count(); };
        std::ostringstream line;
        line.imbue(std::locale::classic());
        line << "MAKO Renderer: present diagnostics: operation=pacing-profile-copy version=1"
             << " pid=" << getpid() << " tid=" << gettid()
             << " context=" << info.context << " source=" << info.source << " swapchain=" << info.swapchain
             << " generated=" << info.generated << " image_index=" << info.imageIndex << " pass=" << info.pass
             << " device=" << info.device << " queue=" << info.queue << " command=" << info.command
             << " started_ns=" << ns(started) << " finished_ns=" << ns(finished) << " submitted=" << submitted
             << " timeline_wait=" << info.timelineWait << " wait_value=" << info.waitValue
             << " timeline_signal=" << info.timelineSignal << " signal_value=" << info.signalValue
             << " fence=" << info.fence;
        const auto semaphores = [&](std::string_view name, std::span<const VkSemaphore> values) {
            const size_t recorded = std::min(values.size(), size_t{16});
            line << ' ' << name << "_count=" << values.size() << ' ' << name << "_recorded=" << recorded;
            for (size_t i = 0; i < recorded; ++i) line << ' ' << name << '_' << i << '=' << values[i];
        };
        semaphores("wait", info.waits);
        semaphores("signal", info.signals);
        line << '\n';
        writePacingProfileRecords(line.str());
    } catch (...) {}
}

void recordApplicationFrameTiming(const ApplicationFrameApi api, const uintptr_t caller,
        const VkSwapchainKHR swapchain, const Clock::time_point started,
        const Clock::time_point finished, const std::optional<VkResult> result,
        const std::optional<uint64_t> timeout,
        const std::optional<Clock::duration> configurationUpdate,
        const std::optional<Clock::duration> deferredJoin,
        const std::optional<uint32_t> imageIndex, const uint64_t context, const uint64_t source,
        const ApplicationAcquireDetails* acquisition) {
    thread_local std::array<ApplicationFrameTiming, 3> observations;
    thread_local uint64_t streamId = allocateContextId();
    thread_local const auto threadId = gettid();
    if (pacingFrameTraceEnabled()) {
        thread_local uint64_t ordinal{};
        const auto ns = [](auto time) { return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count(); };
        std::ostringstream trace;
        trace.imbue(std::locale::classic());
        trace << "MAKO Renderer: present diagnostics: operation=pacing-profile-application version=1"
              << " pid=" << getpid() << " tid=" << threadId << " stream=" << streamId << " ordinal=" << ++ordinal
              << " context=" << context << " source=" << source << " swapchain=" << swapchain
              << " api=" << (api == ApplicationFrameApi::Present ? "present" : api == ApplicationFrameApi::AcquireKhr ? "acquire" : "acquire2")
              << " image_index=" << imageIndex.value_or(UINT32_MAX)
              << " started_ns=" << ns(started) << " finished_ns=" << ns(finished)
              << " result_known=" << result.has_value() << " result=" << result.value_or(VK_SUCCESS)
              << " timeout_known=" << timeout.has_value() << " timeout_ns=" << timeout.value_or(0)
              << " join_known=" << deferredJoin.has_value() << " join_ns=" << (deferredJoin ? std::chrono::duration_cast<std::chrono::nanoseconds>(*deferredJoin).count() : 0)
              << " configuration_known=" << configurationUpdate.has_value() << " configuration_ns=" << (configurationUpdate ? std::chrono::duration_cast<std::chrono::nanoseconds>(*configurationUpdate).count() : 0)
              << " caller=" << reinterpret_cast<void*>(caller);
        if (acquisition && acquisition->callId)
            trace << " call_id=" << acquisition->callId << " lower_calls=" << acquisition->lowerCalls
                  << " lower_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(acquisition->lowerDuration).count();
        trace << '\n';
        writePacingProfileRecords(trace.str());
    }
    const auto window = observations.at(static_cast<size_t>(api)).observe(
        caller, swapchain, started, finished, result, timeout, configurationUpdate, deferredJoin);
    if (!window)
        return;
    const bool present = api == ApplicationFrameApi::Present;
    std::ostringstream line;
    line.imbue(std::locale::classic());
    line << "MAKO Renderer: present diagnostics: operation="
         << (present ? "application-queue-present" : "application-acquire")
         << " stream=" << streamId << " pid=" << getpid()
         << " tid=" << threadId
         << " sample_end_monotonic_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(finished.time_since_epoch()).count()
         << (present ? " queue=" : " device=") << reinterpret_cast<void*>(caller)
         << " swapchain=" << swapchain
         << " api=" << (present ? "KHR" : api == ApplicationFrameApi::AcquireKhr ? "KHR" : "KHR2")
         << " calls=" << window->calls
         << " entry_interval_samples=" << window->entryInterval.count
         << " entry_interval_mean_ms=" << window->entryInterval.mean()
         << " entry_interval_max_ms=" << window->entryInterval.maximum
         << " duration_mean_ms=" << window->duration.mean()
         << " duration_max_ms=" << window->duration.maximum;
    if (present) {
        line << " includes_intentional_pacing=1"
             << " configuration_update_samples=" << window->configurationUpdate.count
             << " configuration_update_mean_ms=" << window->configurationUpdate.mean()
             << " configuration_update_max_ms=" << window->configurationUpdate.maximum
             << " deferred_join_samples=" << window->deferredJoin.count
             << " deferred_join_mean_ms=" << window->deferredJoin.mean()
             << " deferred_join_max_ms=" << window->deferredJoin.maximum;
    } else {
        line << " successful=" << window->successful << " timeouts=" << window->timeouts
             << " not_ready=" << window->notReady << " errors=" << window->errors
             << " polls=" << window->polls;
    }
    line << '\n';
    if (pacingProfileEnabled()) pacingLog().write(line.str());
    else std::cerr << line.str();
}

namespace { thread_local ApplicationPresentScope* applicationPresentScope{}; }

void bindApplicationPresentSource(const uint64_t context, const uint64_t source) noexcept {
    if (applicationPresentScope) {
        applicationPresentScope->context = context;
        applicationPresentScope->source = source;
    }
}

ApplicationPresentScope::ApplicationPresentScope(const VkQueue queue,
        const VkSwapchainKHR swapchain, const std::optional<uint32_t> imageIndex) :
        previous(applicationPresentScope), imageIndex(imageIndex), active(enabled() || pacingProfileEnabled()),
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

void logBridgeTiming(const uint64_t bridgeId, const VkSwapchainKHR swapchain,
        const BridgePresentTiming::Window& window,
        const size_t outstanding, const uint64_t refreshCycleNs,
        const uint32_t timingVersion, const uint64_t refreshIntervalNs) {
    std::ostringstream line;
    line << "MAKO Renderer: present diagnostics: operation=gamescope-bridge-timing"
         << " bridge=" << bridgeId << " pid=" << getpid()
         << " swapchain=" << swapchain
         << " timestamp_source=compositor-reported"
         << " timing_protocol=" << timingVersion << " timestamp_kind=prediction"
         << " requests=" << window.requests << " feedbacks=" << window.feedbacks
         << " outstanding=" << outstanding << " unmatched=" << window.unmatched
         << " overwritten=" << window.overwritten << " discontinuities=" << window.discontinuities
         << " nonconsecutive_ids=" << window.nonconsecutiveIds
         << " repeated_timestamps=" << window.repeatedTimestamps
         << " backwards_timestamps=" << window.backwardsTimestamps
         << " late_latch_feedbacks=" << window.lateLatchFeedbacks
         << " dropped=" << window.dropped
         << " invalid_stages=" << window.invalidStages
         << " unavailable_stages=" << window.unavailableStages
         << " queue_ready_samples=" << window.queueReady.count
         << " queue_ready_mean_ms=" << window.queueReady.mean()
         << " queue_ready_max_ms=" << window.queueReady.maximum
         << " compositor_hold_samples=" << window.compositorHold.count
         << " compositor_hold_mean_ms=" << window.compositorHold.mean()
         << " compositor_hold_max_ms=" << window.compositorHold.maximum
         << " requested_interval_samples=" << window.requestedInterval.count
         << " requested_interval_mean_ms=" << window.requestedInterval.mean()
         << " requested_interval_stddev_ms=" << window.requestedInterval.deviation()
         << " requested_interval_max_ms=" << window.requestedInterval.maximum
         << " reported_interval_samples=" << window.reportedInterval.count
         << " reported_interval_mean_ms=" << window.reportedInterval.mean()
         << " reported_interval_stddev_ms=" << window.reportedInterval.deviation()
         << " reported_interval_max_ms=" << window.reportedInterval.maximum
         << " lateness_mean_ms=" << window.lateness.mean()
         << " lateness_max_ms=" << window.lateness.maximum
         << " submit_lateness_mean_ms=" << window.submitLateness.mean()
         << " submit_lateness_max_ms=" << window.submitLateness.maximum
         << " refresh_cycle_ns=" << refreshCycleNs
         << " refresh_interval_ns=" << refreshIntervalNs << '\n';
    std::cerr << line.str();
}

void recordApplicationPresentWait(const VkDevice device, const VkSwapchainKHR swapchain,
        const PresentWaitApi api, const uint64_t presentId, const uint64_t timeout,
        const VkResult result, const Clock::time_point started,
        const Clock::time_point finished) {
    thread_local ApplicationPresentWait observations;
    thread_local uint64_t waiterId = allocateContextId();
    thread_local const auto threadId = gettid();
    if (pacingFrameTraceEnabled()) {
        std::ostringstream trace;
        trace << "MAKO Renderer: present diagnostics: operation=pacing-profile-wait version=1"
              << " pid=" << getpid() << " tid=" << threadId << " swapchain=" << swapchain
              << " api=" << (api == PresentWaitApi::Khr ? "KHR" : "KHR2")
              << " present_id=" << presentId << " timeout_ns=" << timeout << " result=" << result
              << " started_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(started.time_since_epoch()).count()
              << " finished_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(finished.time_since_epoch()).count() << '\n';
        writePacingProfileRecords(trace.str());
    }
    const auto window = observations.observe(device, swapchain, api,
        presentId, timeout, result, started, finished);
    if (!window)
        return;
    std::ostringstream line;
    line << "MAKO Renderer: present diagnostics: operation=application-present-wait"
         << " waiter=" << waiterId << " pid=" << getpid()
         << " tid=" << threadId
         << " sample_end_monotonic_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(finished.time_since_epoch()).count()
         << " device=" << device << " swapchain=" << swapchain
         << " api=" << (api == PresentWaitApi::Khr ? "KHR" : "KHR2")
         << " calls=" << window->calls << " successful=" << window->successful
         << " timeouts=" << window->timeouts << " errors=" << window->errors
         << " polls=" << window->polls
         << " first_present_id=" << window->firstPresentId
         << " last_present_id=" << window->lastPresentId
         << " duration_mean_ms=" << window->duration.mean()
         << " duration_max_ms=" << window->duration.maximum;
    if (window->firstFailure) {
        const auto& failure = *window->firstFailure;
        line << " first_failure_result=" << failure.result
             << " first_failure_present_id=" << failure.presentId
             << " first_failure_timeout_ns=" << failure.timeout
             << " first_failure_duration_ms=" << failure.durationMs;
    }
    line << '\n';
    if (pacingProfileEnabled()) pacingLog().write(line.str());
    if (enabled()) std::cerr << line.str();
}
namespace {
    std::atomic<uint32_t> nextContextSequence{1};
    thread_local uint64_t activeContextId{0};
    thread_local PacingProfile* activePacingProfile{};

    double elapsedMilliseconds(const Clock::time_point started) {
        return std::chrono::duration<double, std::milli>(
            Clock::now() - started
        ).count();
    }

    void logAdaptiveStabilization(const std::string_view reason,
            const Clock::duration duration) {
        if (!enabled())
            return;

        std::cerr << "MAKO Renderer: present diagnostics: operation=adaptive-stabilization"
                  << " context=" << activeContextId
                  << " reason=" << reason
                  << " duration_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         duration
                     ).count()
                  << '\n';
    }

    void logAdaptiveRamp(const size_t previousLimit, const size_t newLimit,
            const double baseFps) {
        if (!enabled())
            return;

        std::cerr << "MAKO Renderer: present diagnostics: operation=adaptive-ramp"
                  << " context=" << activeContextId
                  << " previous_generated_limit=" << previousLimit
                  << " generated_limit=" << newLimit
                  << " base_fps=" << baseFps << '\n';
    }

    void logAdaptiveRampResult(const bool accepted,
            const size_t previousLimit, const size_t testedLimit,
            const double previousBaseFps, const double currentBaseFps,
            const double previousOutputFps, const double currentOutputFps,
            const std::string_view reason) {
        if (!enabled())
            return;

        std::cerr << "MAKO Renderer: present diagnostics: operation="
                  << (accepted ? "adaptive-ramp-accepted" : "adaptive-load-shed")
                  << " context=" << activeContextId
                  << " previous_generated_limit=" << previousLimit
                  << " tested_generated_limit=" << testedLimit
                  << " previous_base_fps=" << previousBaseFps
                  << " current_base_fps=" << currentBaseFps
                  << " previous_output_fps=" << previousOutputFps
                  << " current_output_fps=" << currentOutputFps;
        if (!reason.empty())
            std::cerr << " reason=" << reason;
        std::cerr << '\n';
    }

    void logAdaptiveProbeAborted(const std::string_view reason,
            const size_t testedLimit) {
        if (!enabled())
            return;

        std::cerr << "MAKO Renderer: present diagnostics: operation=adaptive-probe-aborted"
                  << " context=" << activeContextId
                  << " reason=" << reason
                  << " tested_generated_limit=" << testedLimit << '\n';
    }

    void logAdaptiveRearm(const std::string_view operation,
            const std::string_view reason, const size_t failures,
            const size_t fallbackLimit, const Clock::duration cooldown,
            const double baselineBaseFps, const double currentBaseFps,
            const std::string_view decision) {
        if (!enabled())
            return;

        std::cerr << "MAKO Renderer: present diagnostics: operation=" << operation
                  << " context=" << activeContextId
                  << " reason=" << reason
                  << " consecutive_failures=" << failures;
        if (operation == "adaptive-rearm-scheduled") {
            std::cerr << " cooldown_ms="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             cooldown
                         ).count()
                      << " stable_required_ms="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             AdaptiveScheduler::stableRearmDuration()
                         ).count()
                      << " fallback_generated_limit=" << fallbackLimit
                      << " baseline_base_fps=" << baselineBaseFps;
        } else {
            std::cerr << " fallback_generated_limit=" << fallbackLimit
                      << " baseline_base_fps=" << baselineBaseFps
                      << " current_base_fps=" << currentBaseFps;
            if (!decision.empty())
                std::cerr << " decision=" << decision;
        }
        std::cerr << '\n';
    }

    void logAdaptiveFastCadenceBurst(const double baselineBaseFps,
            const double instantaneousBaseFps, const double thresholdFps,
            const size_t ignoredFrames, const size_t totalIgnoredFrames,
            const Clock::duration duration) {
        if (!enabled())
            return;

        std::cerr << "MAKO Renderer: present diagnostics: operation="
                  << "adaptive-fast-cadence-burst"
                  << " context=" << activeContextId
                  << " baseline_base_fps=" << baselineBaseFps
                  << " instantaneous_base_fps=" << instantaneousBaseFps
                  << " threshold_fps=" << thresholdFps
                  << " ignored_frames=" << ignoredFrames
                  << " total_ignored_frames=" << totalIgnoredFrames
                  << " duration_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         duration
                     ).count()
                  << '\n';
    }

    void logAdaptiveFastCadenceBurstComplete(
            const size_t totalIgnoredFrames, const Clock::duration duration) {
        if (!enabled())
            return;

        std::cerr << "MAKO Renderer: present diagnostics: operation="
                  << "adaptive-fast-cadence-burst-complete"
                  << " context=" << activeContextId
                  << " total_ignored_frames=" << totalIgnoredFrames
                  << " duration_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         duration
                     ).count()
                  << '\n';
    }

    void logAdaptiveRampBackoff(const size_t testedLimit,
            const size_t failures, const double baselineBaseFps,
            const Clock::duration delay) {
        if (!enabled())
            return;

        std::cerr << "MAKO Renderer: present diagnostics: operation=adaptive-ramp-backoff"
                  << " context=" << activeContextId
                  << " tested_generated_limit=" << testedLimit
                  << " consecutive_failures=" << failures
                  << " baseline_base_fps=" << baselineBaseFps
                  << " retry_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         delay
                     ).count()
                  << '\n';
    }

    void logAdaptiveRecoveryResume(const size_t generationLimit,
            const Clock::duration higherProbeDelay,
            const std::string_view reason) {
        if (!enabled())
            return;

        std::cerr << "MAKO Renderer: present diagnostics: operation="
                  << "adaptive-recovery-resume-scheduled"
                  << " context=" << activeContextId
                  << " generated_limit=" << generationLimit
                  << " higher_probe_delay_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         higherProbeDelay
                     ).count()
                  << " reason=" << reason << '\n';
    }

    void logAdaptiveStableCadence(const std::string_view operation,
            const size_t generatedLimit, const double baselineBaseFps,
            const double currentBaseFps, const std::string_view reason) {
        if (!enabled())
            return;

        std::cerr << "MAKO Renderer: present diagnostics: operation=" << operation
                  << " context=" << activeContextId
                  << " generated_limit=" << generatedLimit
                  << " baseline_base_fps=" << baselineBaseFps
                  << " current_base_fps=" << currentBaseFps
                  << " projected_output_fps="
                  << currentBaseFps * static_cast<double>(generatedLimit + 1);
        if (!reason.empty())
            std::cerr << " reason=" << reason;
        std::cerr << '\n';
    }

    void logAdaptiveRescueStart(const size_t generatedLimit,
            const double baselineBaseFps, const double currentBaseFps,
            const double projectedOutputFps, const std::string_view reason) {
        if (!enabled())
            return;

        std::cerr << "MAKO Renderer: present diagnostics: operation=adaptive-rescue-start"
                  << " context=" << activeContextId
                  << " generated_limit=" << generatedLimit
                  << " baseline_base_fps=" << baselineBaseFps
                  << " current_base_fps=" << currentBaseFps
                  << " projected_output_fps=" << projectedOutputFps
                  << " reason=" << reason
                  << " measurement_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         AdaptiveScheduler::rescueMeasurementDuration()
                     ).count()
                  << '\n';
    }

    void logAdaptiveRescueComplete(const size_t previousLimit,
            const size_t resumedLimit, const size_t requestedLimit,
            const size_t configuredLimit, const double baselineBaseFps,
            const double measuredBaseFps, const std::string_view decision) {
        if (!enabled())
            return;

        std::cerr << "MAKO Renderer: present diagnostics: operation=adaptive-rescue-complete"
                  << " context=" << activeContextId
                  << " previous_generated_limit=" << previousLimit
                  << " resumed_generated_limit=" << resumedLimit
                  << " requested_generated_limit=" << requestedLimit
                  << " configured_generated_limit=" << configuredLimit
                  << " baseline_base_fps=" << baselineBaseFps
                  << " measured_base_fps=" << measuredBaseFps
                  << " decision=" << decision
                  << " cooldown_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         AdaptiveScheduler::rescueCooldown()
                     ).count()
                  << '\n';
    }

    void logAdaptiveAutomaticBaseCapSuppressed(
            const size_t generationLimit, const double baselineBaseFps,
            const double currentBaseFps, const std::string_view reason) {
        if (!enabled())
            return;

        std::cerr << "MAKO Renderer: present diagnostics: operation="
                     "adaptive-auto-base-cap-suppressed"
                  << " context=" << activeContextId
                  << " generated_limit=" << generationLimit
                  << " baseline_base_fps=" << baselineBaseFps
                  << " current_base_fps=" << currentBaseFps
                  << " reason=" << reason
                  << " action=release-half-target-pacer-for-bounded-rescue"
                  << '\n';
    }

    void logAdaptiveDiscontinuityRecoveryStart(
            const size_t generationLimit, const double baselineBaseFps,
            const std::string_view reason,
            const Clock::duration maximumDuration) {
        if (!enabled())
            return;

        std::cerr << "MAKO Renderer: present diagnostics: operation="
                  << "adaptive-discontinuity-recovery-start"
                  << " context=" << activeContextId
                  << " generated_limit=" << generationLimit
                  << " baseline_base_fps=" << baselineBaseFps
                  << " reason=" << reason
                  << " maximum_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         maximumDuration
                     ).count()
                  << '\n';
    }

    void logAdaptiveDiscontinuityRecoveryComplete(
            const size_t generationLimit, const double baselineBaseFps,
            const double measuredBaseFps, const std::string_view decision) {
        if (!enabled())
            return;

        std::cerr << "MAKO Renderer: present diagnostics: operation="
                  << "adaptive-discontinuity-recovery-complete"
                  << " context=" << activeContextId
                  << " generated_limit=" << generationLimit
                  << " baseline_base_fps=" << baselineBaseFps
                  << " measured_base_fps=" << measuredBaseFps
                  << " decision=" << decision << '\n';
    }

    void logAdaptiveCadenceRefresh(const std::string_view reason,
            const size_t retainedGenerationLimit, const size_t historyFrames) {
        if (!enabled())
            return;

        std::cerr << "MAKO Renderer: present diagnostics: operation="
                  << "adaptive-cadence-refresh"
                  << " context=" << activeContextId
                  << " reason=" << reason
                  << " retained_generated_limit=" << retainedGenerationLimit
                  << " history_warmup_frames=" << historyFrames
                  << " policy=sdr-fast-resume\n";
    }

    void logDynamicCadenceRecovery(const std::string_view operation,
            const size_t generationLimit, const double baselineBaseFps,
            const double measuredBaseFps,
            const size_t confirmedSamples) {
        if (!enabled())
            return;

        std::cerr << "MAKO Renderer: present diagnostics: operation="
                  << operation
                  << " context=" << activeContextId
                  << " generated_limit=" << generationLimit
                  << " baseline_base_fps=" << baselineBaseFps
                  << " measured_base_fps=" << measuredBaseFps
                  << " confirmed_samples=" << confirmedSamples
                  << '\n';
    }

    void logAdaptiveNearTargetNativePreference(
            const std::string_view operation, const uint32_t targetFps,
            const double baseFps, const double nativeIntervalRmsMilliseconds,
            const double fractionalIntervalRmsMilliseconds,
            const Clock::duration holdDuration,
            const std::string_view reason) {
        if (!enabled())
            return;

        std::cerr << "MAKO Renderer: present diagnostics: operation="
                  << operation
                  << " context=" << activeContextId
                  << " target_fps=" << targetFps
                  << " base_fps=" << baseFps
                  << " predicted_native_interval_rms_ms="
                  << nativeIntervalRmsMilliseconds
                  << " predicted_fractional_interval_rms_ms="
                  << fractionalIntervalRmsMilliseconds
                  << " hold_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         holdDuration
                     ).count()
                  << " reason=" << reason
                  << '\n';
    }

    class SchedulerDiagnostics final : public AdaptiveSchedulerDiagnostics {
    public:
        [[nodiscard]] bool enabled() const override {
            return present_diagnostics::enabled();
        }

        void plan(const AdaptivePlanDiagnostic& plan) override {
            if (!present_diagnostics::enabled())
                return;

            // std::cerr is unbuffered, so emitting every field directly adds
            // repeated flush points to the presentation thread. Assemble the
            // opt-in aggregate first and publish it as one complete record.
            std::ostringstream message;
            message << "MAKO Renderer: present diagnostics: operation=adaptive-plan"
                    << " context=" << activeContextId
                    << " base_fps=" << plan.baseFps
                    << " target_fps=" << plan.targetFps
                    << " generated=" << plan.generatedFrames
                    << " max_generated=" << plan.maximumGeneratedFrames
                    << " stable_cadence=" << (plan.stableCadence ? 1 : 0);
            if (plan.nearTargetNativePreference) {
                message << " near_target_native=1"
                        << " predicted_native_interval_rms_ms="
                        << plan.predictedNativeIntervalRmsMilliseconds
                        << " predicted_fractional_interval_rms_ms="
                        << plan.predictedFractionalIntervalRmsMilliseconds;
            }
            if (plan.configuredMaximumGeneratedFrames) {
                message << " configured_max_generated="
                        << plan.configuredMaximumGeneratedFrames;
            }
            if (!plan.phase.empty())
                message << " phase=" << plan.phase;
            if (plan.recoveryGenerationLimit) {
                message << " recovery_generated_limit="
                        << plan.recoveryGenerationLimit;
            }
            if (!plan.rearmReason.empty()) {
                message << " rearm_reason=" << plan.rearmReason
                        << " cooldown_remaining_ms="
                        << std::chrono::duration_cast<std::chrono::milliseconds>(
                               plan.rearmCooldownRemaining
                           ).count()
                        << " rearm_baseline_base_fps="
                        << plan.rearmBaselineBaseFps;
            }
            if (plan.pacingSourceSamples) {
                message << " target_clock="
                        << (plan.targetOutputClockActive ? 1 : 0)
                        << " target_budget_credit_outputs="
                        << plan.targetOutputBudgetCreditOutputs
                        << " target_deferred_budget_output="
                        << (plan.targetOutputDeferredBudgetOutput ? 1 : 0)
                        << " target_phase_error_ms="
                        << plan.targetOutputPhaseErrorMilliseconds
                        << " source_interval_samples="
                        << plan.pacingSourceSamples
                        << " source_interval_mean_ms="
                        << plan.sourceIntervalMeanMilliseconds
                        << " source_interval_stddev_ms="
                        << plan.sourceIntervalStdDevMilliseconds
                        << " source_interval_p95_ms="
                        << plan.sourceIntervalP95Milliseconds
                        << " source_interval_p99_ms="
                        << plan.sourceIntervalP99Milliseconds
                        << " generated_count_changes="
                        << plan.generatedCountChanges
                        << " requested_interval_samples="
                        << plan.requestedIntervalSamples
                        << " requested_interval_mean_ms="
                        << plan.requestedIntervalMeanMilliseconds
                        << " requested_interval_stddev_ms="
                        << plan.requestedIntervalStdDevMilliseconds
                        << " requested_interval_p95_ms="
                        << plan.requestedIntervalP95Milliseconds
                        << " requested_interval_p99_ms="
                        << plan.requestedIntervalP99Milliseconds
                        << " target_phase_error_samples="
                        << plan.targetPhaseErrorSamples
                        << " target_phase_error_rms_ms="
                        << plan.targetPhaseErrorRmsMilliseconds
                        << " target_phase_error_max_ms="
                        << plan.targetPhaseErrorMaximumMilliseconds;
            }
            message << '\n';
            std::cerr << message.str();
        }

        void stabilization(const std::string_view reason,
                const Clock::duration duration) override {
            logAdaptiveStabilization(reason, duration);
        }

        void ramp(const size_t previousLimit, const size_t newLimit,
                const double baseFps) override {
            logAdaptiveRamp(previousLimit, newLimit, baseFps);
        }

        void rampResult(const bool accepted, const size_t previousLimit,
                const size_t testedLimit, const double previousBaseFps,
                const double currentBaseFps, const double previousOutputFps,
                const double currentOutputFps,
                const std::string_view reason) override {
            logAdaptiveRampResult(
                accepted, previousLimit, testedLimit, previousBaseFps,
                currentBaseFps, previousOutputFps, currentOutputFps, reason
            );
        }

        void probeAborted(const std::string_view reason,
                const size_t testedLimit) override {
            logAdaptiveProbeAborted(reason, testedLimit);
        }

        void rearm(const std::string_view operation,
                const std::string_view reason, const size_t failures,
                const size_t fallbackLimit, const Clock::duration cooldown,
                const double baselineBaseFps, const double currentBaseFps,
                const std::string_view decision) override {
            logAdaptiveRearm(
                operation, reason, failures, fallbackLimit, cooldown,
                baselineBaseFps, currentBaseFps, decision
            );
        }

        void fastCadenceBurst(const double baselineBaseFps,
                const double instantaneousBaseFps, const double thresholdFps,
                const size_t ignoredFrames, const size_t totalIgnoredFrames,
                const Clock::duration duration) override {
            logAdaptiveFastCadenceBurst(
                baselineBaseFps, instantaneousBaseFps, thresholdFps,
                ignoredFrames, totalIgnoredFrames, duration
            );
        }

        void fastCadenceBurstComplete(const size_t totalIgnoredFrames,
                const Clock::duration duration) override {
            logAdaptiveFastCadenceBurstComplete(totalIgnoredFrames, duration);
        }

        void rampBackoff(const size_t testedLimit, const size_t failures,
                const double baselineBaseFps,
                const Clock::duration delay) override {
            logAdaptiveRampBackoff(
                testedLimit, failures, baselineBaseFps, delay
            );
        }

        void targetConstrained(const uint32_t targetFps,
                const size_t maximumMultiplier, const double baseFps,
                const double projectedOutputFps) override {
            if (!present_diagnostics::enabled())
                return;
            std::cerr
                << "MAKO Renderer: present diagnostics: "
                << "operation=adaptive-target-constrained"
                << " context=" << activeContextId
                << " target_fps=" << targetFps
                << " maximum_multiplier=" << maximumMultiplier
                << " base_fps=" << baseFps
                << " projected_output_fps=" << projectedOutputFps
                << " reason=multiplier-ceiling"
                << '\n';
        }

        void recoveryResume(const size_t generationLimit,
                const Clock::duration higherProbeDelay,
                const std::string_view reason) override {
            logAdaptiveRecoveryResume(
                generationLimit, higherProbeDelay, reason
            );
        }

        void stableCadence(const std::string_view operation,
                const size_t generatedLimit, const double baselineBaseFps,
                const double currentBaseFps,
                const std::string_view reason) override {
            logAdaptiveStableCadence(
                operation, generatedLimit, baselineBaseFps,
                currentBaseFps, reason
            );
        }

        void rescueStart(const size_t generatedLimit,
                const double baselineBaseFps, const double currentBaseFps,
                const double projectedOutputFps,
                const std::string_view reason) override {
            logAdaptiveRescueStart(
                generatedLimit, baselineBaseFps, currentBaseFps,
                projectedOutputFps, reason
            );
        }

        void rescueComplete(const size_t previousLimit,
                const size_t resumedLimit, const size_t requestedLimit,
                const size_t configuredLimit, const double baselineBaseFps,
                const double measuredBaseFps,
                const std::string_view decision) override {
            logAdaptiveRescueComplete(
                previousLimit, resumedLimit, requestedLimit, configuredLimit,
                baselineBaseFps, measuredBaseFps, decision
            );
        }

        void automaticBaseCapSuppressed(const size_t generationLimit,
                const double baselineBaseFps,
                const double currentBaseFps,
                const std::string_view reason) override {
            logAdaptiveAutomaticBaseCapSuppressed(
                generationLimit, baselineBaseFps, currentBaseFps, reason
            );
        }

        void automaticBaseCapRestored(const size_t generationLimit,
                const double baselineBaseFps, const double measuredBaseFps,
                const std::string_view reason) override {
            if (!enabled())
                return;
            std::cerr << "MAKO Renderer: present diagnostics: operation="
                         "adaptive-auto-base-cap-restored"
                      << " context=" << activeContextId
                      << " generated_limit=" << generationLimit
                      << " baseline_base_fps=" << baselineBaseFps
                      << " measured_base_fps=" << measuredBaseFps
                      << " reason=" << reason
                      << " action=restore-half-target-pacer\n";
        }

        void discontinuityRecoveryStart(const size_t generationLimit,
                const double baselineBaseFps, const std::string_view reason,
                const Clock::duration maximumDuration) override {
            logAdaptiveDiscontinuityRecoveryStart(
                generationLimit, baselineBaseFps, reason, maximumDuration
            );
        }

        void discontinuityRecoveryComplete(const size_t generationLimit,
                const double baselineBaseFps, const double measuredBaseFps,
                const std::string_view decision) override {
            logAdaptiveDiscontinuityRecoveryComplete(
                generationLimit, baselineBaseFps, measuredBaseFps, decision
            );
        }

        void cadenceRefresh(const std::string_view reason,
                const size_t retainedGenerationLimit,
                const size_t historyFrames) override {
            logAdaptiveCadenceRefresh(
                reason, retainedGenerationLimit, historyFrames
            );
        }

        void nativeCadenceProbe(const std::string_view operation,
                const size_t generationLimit,
                const double baselineBaseFps,
                const double measuredBaseFps,
                const size_t confirmedSamples) override {
            logDynamicCadenceRecovery(
                operation, generationLimit, baselineBaseFps,
                measuredBaseFps, confirmedSamples
            );
        }

        void nearTargetNativePreference(const std::string_view operation,
                const uint32_t targetFps, const double baseFps,
                const double nativeIntervalRmsMilliseconds,
                const double fractionalIntervalRmsMilliseconds,
                const Clock::duration holdDuration,
                const std::string_view reason) override {
            logAdaptiveNearTargetNativePreference(
                operation, targetFps, baseFps,
                nativeIntervalRmsMilliseconds,
                fractionalIntervalRmsMilliseconds, holdDuration, reason
            );
        }
    };
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

bool enabled() {
    static const bool value = [] {
        const char* environment = std::getenv("MAKO_PRESENT_DIAGNOSTICS");
        return environment && std::string_view(environment) != "0";
    }();
    return value;
}

double thresholdMilliseconds() {
    static const double threshold = [] {
        constexpr double defaultThresholdMs = 20.0;
        const char* value = std::getenv(
            "MAKO_PRESENT_DIAGNOSTICS_THRESHOLD_MS"
        );
        if (!value)
            return defaultThresholdMs;

        char* end{};
        const double parsed = std::strtod(value, &end);
        if (end == value || *end != '\0' || parsed < 0.0)
            return defaultThresholdMs;
        return parsed;
    }();
    return threshold;
}

Clock::time_point start() {
    if (!enabled())
        return {};
    return Clock::now();
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

void logSlowOperation(const std::string_view operation,
        const size_t frameIndex, const size_t sequenceIndex,
        const Clock::time_point started,
        const std::optional<VkResult> result,
        const std::optional<size_t> passIndex,
        const std::optional<uint32_t> imageIndex) {
    if (!enabled())
        return;

    const double durationMs = elapsedMilliseconds(started);
    const bool resultFailed = result &&
        *result != VK_SUCCESS && *result != VK_SUBOPTIMAL_KHR;
    if (durationMs < thresholdMilliseconds() && !resultFailed)
        return;

    std::ostringstream message;
    message << "MAKO Renderer: present diagnostics: operation=" << operation
            << " context=" << activeContextId
            << " duration_ms=" << durationMs
            << " frame=" << frameIndex
            << " sequence=" << sequenceIndex;
    if (result)
        message << " result=" << static_cast<int>(*result);
    if (passIndex)
        message << " pass=" << *passIndex;
    if (imageIndex)
        message << " image=" << *imageIndex;
    std::cerr << message.str() << '\n';
}

void logPresentFallback(const size_t frameIndex,
        const size_t sequenceIndex, const size_t passIndex,
        const size_t skippedFrames, const uint64_t timelineValue,
        const std::string_view acquireMode,
        const std::string_view backendWork) {
    if (!enabled())
        return;

    std::cerr << "MAKO Renderer: present diagnostics: operation=skip-generated-frames"
              << " context=" << activeContextId
              << " frame=" << frameIndex
              << " sequence=" << sequenceIndex
              << " pass=" << passIndex
              << " skipped=" << skippedFrames
              << " wait_timeline=" << timelineValue
              << " acquire_mode=" << acquireMode
              << " backend_work=" << backendWork
              << '\n';
}

void logHistoryWarmup(const size_t frameIndex,
        const size_t sequenceIndex, const size_t remainingFrames,
        const bool recovery,
        const std::optional<uint32_t> acquiredImage) {
    if (!enabled())
        return;

    std::cerr << "MAKO Renderer: present diagnostics: operation=history-warmup"
              << " context=" << activeContextId
              << " frame=" << frameIndex
              << " sequence=" << sequenceIndex
              << " reason=" << (recovery ? "recovery" : "startup")
              << " remaining_frames=" << remainingFrames;
    if (acquiredImage)
        std::cerr << " released_image=" << *acquiredImage;
    std::cerr << '\n';
}

AdaptiveSchedulerDiagnostics& adaptiveScheduler() {
    static SchedulerDiagnostics diagnostics;
    return diagnostics;
}

}

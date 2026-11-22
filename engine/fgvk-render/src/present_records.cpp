/* SPDX-License-Identifier: GPL-3.0-or-later */

// The pacing recorder's writer. Producers enqueue fixed records (present_records.hpp); this
// thread drains them every 50 ms, formats them with the text formatters below, and is the only
// owner of the log file. Producers never take a lock here, and nothing here runs on game threads.

#include "present_records.hpp"

#include "library_pin.hpp"
#include "fgvk-common/vulkan/backend_overlap.hpp"
#include "fgvk-common/vulkan/queue_priority.hpp"
#include "swapchain/output_margin.hpp"
#include "swapchain/queue_start_probe.hpp"
#include "swapchain/release_gate.hpp"
#include "swapchain/real_present_lead.hpp"
#include "swapchain/reflex_out_of_band.hpp"
#include "swapchain/tick_lock.hpp"
#include "swapchain/transfer_queue.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <mutex>
#include <sched.h>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <memory>

#include <fcntl.h>
#include <link.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef FGVK_BUILD_VERSION
#define FGVK_BUILD_VERSION "unknown"
#endif
#ifndef FGVK_BUILD_FINGERPRINT
#define FGVK_BUILD_FINGERPRINT FGVK_BUILD_VERSION
#endif

namespace fgvk::layer::present_records {
namespace {

using present_diagnostics::PacingProfile;
using present_diagnostics::ApplicationFrameApi;
using present_diagnostics::ApplicationFrameTiming;
using present_diagnostics::ApplicationPresentWait;
using present_diagnostics::PresentWaitApi;
using present_diagnostics::WaylandEvent;
using present_diagnostics::WaylandSample;
using present_diagnostics::pacingFrameTraceEnabled;
using present_diagnostics::pacingGpuProfileEnabled;
using present_diagnostics::pacingWaylandProfileEnabled;

constexpr size_t textBudgetBytes = 8ull * 1024 * 1024;
constexpr auto writerPeriod = std::chrono::milliseconds{50};
constexpr size_t pendingFlushBytes = 64ull * 1024;

std::ostringstream classicStream() {
    std::ostringstream line;
    line.imbue(std::locale::classic());
    return line;
}

int64_t nanosecondsOf(const Clock::duration value) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value).count();
}

int64_t epochNanoseconds(const Clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
}

// Read the mapped ELF note of this very DSO, not a cached CMake fingerprint
// or a pathname whose file may already have been replaced during development.
std::string loadedRendererBuildId() {
    struct State { uintptr_t address; std::string id; } state{
        reinterpret_cast<uintptr_t>(&present_diagnostics::pacingFrameTraceEnabled), "unavailable"};
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

// FGVK_PACING_PROFILE_LIMIT_MB overrides the capture limit. The wrap default is 1 GiB; the
// stop default (the Wayland capture) stays at 512 MiB.
size_t captureLimitBytes(const bool retainBeginning) {
    if (const char* requested = std::getenv("FGVK_PACING_PROFILE_LIMIT_MB")) {
        char* end{};
        const unsigned long long megabytes = std::strtoull(requested, &end, 10);
        if (end != requested && *end == '\0' && megabytes >= 1 && megabytes <= (1ull << 20))
            return static_cast<size_t>(megabytes * 1024 * 1024);
    }
    return retainBeginning ? 512ull * 1024 * 1024 : 1024ull * 1024 * 1024;
}

// The one owner of the capture file. Lines are buffered and written by the writer thread.
// The wrap and stop rules are the ones the synchronous sink had.
class PacingSink {
public:
    PacingSink() {
        try {
            retainBeginning = pacingWaylandProfileEnabled();
            byteLimit = captureLimitBytes(retainBeginning);
            const char* requested = std::getenv("FGVK_PACING_PROFILE_LOG");
            if (requested && std::string_view(requested) == "stderr") {
                descriptor = STDERR_FILENO;
                return;
            }
            std::filesystem::path path;
            if (requested && *requested) {
                path = requested;
                if (!path.is_absolute()) return;
                descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
            } else {
                const std::string name = "fgvk-pacing-" + std::to_string(geteuid()) + "-" +
                    std::to_string(getpid()) + "-" + std::to_string(Clock::now().time_since_epoch().count()) + ".log";
                constexpr int flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
                if (const char* home = std::getenv("HOME"); home && std::filesystem::path(home).is_absolute()) {
                    path = std::filesystem::path(home) / "fgvk-logs" / name;
                    const auto directory = path.parent_path();
                    if (::mkdir(directory.c_str(), 0700) == 0 || errno == EEXIST) {
                        const int folder = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                        if (folder >= 0) {
                            struct stat info{};
                            if (::fstat(folder, &info) == 0 && info.st_uid == geteuid() && (info.st_mode & 0077) == 0)
                                descriptor = ::openat(folder, name.c_str(), flags, 0600);
                            ::close(folder);
                        }
                    }
                }
                const char* temporary = std::getenv("TMPDIR");
                if (descriptor < 0) {
                    std::filesystem::path directory = temporary && *temporary ? temporary : "/tmp";
                    if (!directory.is_absolute()) directory = "/tmp";
                    path = directory / name;
                    descriptor = ::open(path.c_str(), flags, 0600);
                }
            }
            // Exclusive creation refuses old files, symlinks, FIFOs and devices.
            // This log contains only framegen-vulkan timing, never launcher/game output.
            if (descriptor < 0) {
                std::cerr << "framegen-vulkan: pacing profile capture unavailable; choose a new absolute FGVK_PACING_PROFILE_LOG path\n";
                return;
            }
            owned = true;
            std::cerr << "framegen-vulkan: pacing profile capture: path=" << path
                      << "; observations=cpu-submission; gpu_requested=" << present_diagnostics::pacingGpuProfileEnabled()
                      << "; limit_bytes=" << byteLimit << "; retention=" << (retainBeginning ? "stop" : "wrap") << '\n';
            std::ostringstream identity;
            identity << "framegen-vulkan: present diagnostics: operation=pacing-profile-session version=2 pid="
                     << getpid() << " build=" << FGVK_BUILD_VERSION << " fingerprint=" << FGVK_BUILD_FINGERPRINT
                     << " renderer_build_id=" << loadedRendererBuildId()
                     << " capture_limit_bytes=" << byteLimit << " retention=" << (retainBeginning ? "stop" : "wrap")
                     << " wayland_requested=" << pacingWaylandProfileEnabled()
                     << " frame_trace=" << pacingFrameTraceEnabled() << '\n';
            append(identity.str());
            flush();
        } catch (...) {
            if (owned) ::close(descriptor);
            descriptor = -1;
            owned = false;
        }
    }
    PacingSink(const PacingSink&) = delete;
    PacingSink& operator=(const PacingSink&) = delete;

    void append(const std::string_view line) {
        if (descriptor < 0) return;
        if (owned && bytes + line.size() + 256 > byteLimit) {
            flush();
            if (retainBeginning) {
                // Preserve the session header and every earlier phase.
                std::array<char, 256> marker{};
                const auto size = std::snprintf(marker.data(), marker.size(),
                    "framegen-vulkan: present diagnostics: operation=pacing-profile-capture-limit version=1 pid=%ld limit_bytes=%zu captured_bytes=%zu rejected_bytes=%zu\n",
                    static_cast<long>(getpid()), byteLimit, bytes, line.size());
                static_cast<void>(::write(descriptor, marker.data(), static_cast<size_t>(size)));
                ::close(descriptor);
                descriptor = -1;
                owned = false;
                return;
            }
            if (::ftruncate(descriptor, 0) != 0 || ::lseek(descriptor, 0, SEEK_SET) < 0) {
                ::close(descriptor); descriptor = -1; owned = false;
                return;
            }
            bytes = 0;
            constexpr std::string_view marker = "framegen-vulkan: present diagnostics: operation=pacing-profile-log-wrap version=2\n";
            pending.append(marker);
            bytes += marker.size();
        }
        pending.append(line);
        bytes += line.size();
        if (pending.size() >= pendingFlushBytes) flush();
    }

    // One write per cycle. A short or failed write is a lost record, reported as a gap.
    void flush() {
        if (pending.empty()) return;
        if (descriptor >= 0) {
            const auto count = ::write(descriptor, pending.data(), pending.size());
            if (count != static_cast<ssize_t>(pending.size())) failedWrites += 1;
        }
        pending.clear();
    }

    void reportGaps(const uint64_t lostRecords) {
        if (!lostRecords) return;
        std::array<char, 160> marker{};
        const auto size = std::snprintf(marker.data(), marker.size(),
            "\nframegen-vulkan: present diagnostics: operation=pacing-profile-capture-gap lost_records=%llu\n",
            static_cast<unsigned long long>(lostRecords));
        append(std::string_view(marker.data(), static_cast<size_t>(size)));
    }
    // Lines lost to a failed write; reported at the next cycle as a gap.
    [[nodiscard]] uint64_t takeFailedWrites() { return std::exchange(failedWrites, 0); }
    [[nodiscard]] size_t captured() const { return bytes; }
    ~PacingSink() = default;

private:
    size_t byteLimit{16 * 1024 * 1024};
    int descriptor{-1};
    size_t bytes{};
    bool owned{};
    bool retainBeginning{};
    uint64_t failedWrites{};
    std::string pending;
};

struct SecondState {
    Clock::time_point start{};
    uint64_t enqueued{};
    uint64_t dropped{};
    uint64_t slotDrops{};
    int64_t enqueueSumNs{};
    int64_t enqueueMaxNs{};
    int64_t lagMaxNs{};
    int64_t cycleMaxNs{};
};

struct TrackedThread {
    uint32_t thread{};
    bool worker{};
    bool hasPrevious{};
    uint64_t runNs{};
    uint64_t waitNs{};
    uint64_t voluntary{};
    uint64_t involuntary{};
};

struct Recorder {
    TraceRecorder recorder;
    Pools pools;
    PacingSink sink;
    std::mutex writeMutex;
    SecondState second;
    std::array<std::atomic<uint32_t>, 8> workers{};
    std::array<TrackedThread, 64> tracked{};
    size_t trackedCount{};
    std::atomic<uint64_t> probeMaxNs{};
    std::atomic<uint64_t> probeSumNs{};
    std::atomic<size_t> pendingText{};
};

std::atomic<Recorder*> activeState{};
std::mutex initializeMutex;

Recorder* state() noexcept {
    return activeState.load(std::memory_order_acquire);
}

// ---- Formatters: the text of every record. Moved from the synchronous sink without change. ----

void appendPacingOutput(std::ostream& line, const uint64_t context, const VkSwapchainKHR swapchain,
        const PacingProfile::OutputSample& sample) {
    const auto ns = [](auto time) { return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count(); };
    const auto& observed = sample.observation;
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-output"
         << " version=" << (pacingFrameTraceEnabled() ? 2 : 1) << " context=" << context << " pid=" << getpid() << " swapchain=" << swapchain
         << " ordinal=" << sample.ordinal << " source=" << sample.source
         << " generated=" << observed.generated << " deferred=" << observed.deferred
         << " image_index=" << observed.imageIndex
         << " scheduled_ns=" << ns(observed.scheduled)
         << " started_ns=" << ns(sample.started) << " finished_ns=" << ns(sample.finished)
         << " result=" << sample.result << " copy_checked=" << observed.copyChecked
         << " copy_status=" << observed.copyStatus
         << " copy_sampled_ns=" << ns(observed.copySampledAt)
         << " arrival_ns=" << ns(observed.arrival);
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
    // Appended last, so the fields above keep their positions for existing parsers.
    line << " feedback=" << observed.feedback
         << " backend_frame=" << observed.backendFrame
         << " phase=" << observed.phase
         << " present_id=" << observed.presentId
         << " ready_ns=" << ns(observed.readyAt)
         << " slack_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(observed.slack).count()
         << " offset_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(observed.offset).count();
    // GP1: the application's own ID. GT1: the tick snap and target tick, zero without the tick lock.
    line << " app_present_id=" << observed.appPresentId
         << " tick_snap_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(observed.tickSnap).count()
         << " target_tick_ns=" << ns(observed.targetTick);
    if (observed.copySubmitStarted != Clock::time_point{})
        line << " copy_submit_ns=" << ns(observed.copySubmitStarted)
             << " copy_acquire_return_ns=" << ns(observed.copyAcquireReturned)
             << " copy_submit_return_ns=" << ns(observed.copySubmitReturned);
    if (observed.copyAcquirePendingNs != 0)
        line << " acquire_fence_pending_ns="
             << ns(observed.copyAcquireReturned + std::chrono::nanoseconds{observed.copyAcquirePendingNs});
    if (observed.copyAcquireReadyNs != 0)
        line << " acquire_fence_ready_ns="
             << ns(observed.copyAcquireReturned + std::chrono::nanoseconds{observed.copyAcquireReadyNs});
    // PT4: the timing this output forwarded, last of all. The kind is printed as a number, not a character.
    line << " timing_target_ns=" << observed.timingTarget
         << " timing_flags=" << observed.timingFlags
         << " timing_stage=" << observed.timingStage
         << " timing_domain_id=" << observed.timingDomainId
         << " timing_kind=" << static_cast<unsigned>(observed.timingKind);
    line << '\n';
}

void formatPacingSkip(const PacingSkipPayload& payload, std::ostream& line) {
    const auto ns = [](Clock::time_point time) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    };
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-skip version=1"
         << " pid=" << getpid() << " context=" << payload.context << " source=" << payload.source
         << " reason=" << std::string_view(payload.reason.data()) << " scheduled_ns=" << ns(payload.scheduled)
         << " now_ns=" << ns(payload.now) << '\n';
}

void formatPacingSource(const PacingSourcePayload& payload, std::ostream& line) {
    const auto& sample = payload.sample;
    const auto ns = [](auto value) { return std::chrono::duration_cast<std::chrono::nanoseconds>(value).count(); };
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-source version=1"
         << " pid=" << getpid() << " tid=" << payload.thread << " context=" << payload.context << " swapchain=" << payload.swapchain
         << " source=" << sample.source << " started_ns=" << ns(sample.started.time_since_epoch())
         << " finished_ns=" << ns(sample.finished.time_since_epoch())
         << " backend_scheduled=" << sample.backendScheduled << " backend_epoch=" << sample.backendEpoch
         << " backend_frame=" << sample.backendFrame
         << " frame_generation_enabled=" << sample.frameGeneration << " adaptive=" << sample.adaptive
         << " multiplier=" << sample.multiplier
         << " effective_cap_fps=" << sample.cap << " configured_cap_fps=" << sample.configuredCap;
    constexpr std::array<std::string_view, 3> waitNames{"cap", "readiness", "output"};
    for (size_t i = 0; i < waitNames.size(); ++i)
        line << ' ' << waitNames[i] << "_wait_ns=" << ns(sample.waits[i]);
    constexpr std::array<std::string_view, 9> names{
        "private_total", "render_fence", "schedule", "source_copy", "acquire",
        "generated_submit", "generated_present", "original_present", "unattributed"};
    for (size_t i = 0; i < names.size(); ++i) line << ' ' << names[i] << "_ns=" << ns(sample.phases[i]);
    line << '\n';
}

void formatPacingGpu(const PacingGpuPayload& payload, std::ostream& line) {
    const auto count = std::min<size_t>(payload.outputs, payload.generatedUs.size());
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-gpu version=1"
         << " pid=" << getpid() << " context=" << payload.context << " source=" << payload.source
         << " backend_epoch=" << payload.epoch << " backend_frame=" << payload.frame
         << " previous_wait_us=" << payload.previousWaitUs << " prepass_submit_us=" << payload.prepassSubmitUs
         << " generation_submit_us=" << payload.generationSubmitUs << " prepass_us=" << payload.prepassUs
         << " span_us=" << payload.spanUs << " readback_us=" << payload.readbackUs << " outputs=" << payload.outputs;
    for (size_t i = 0; i < count; ++i) line << " output_" << i << "_us=" << payload.generatedUs[i];
    line << '\n';
}

void formatPacingGpuSplit(const PacingGpuSplitPayload& payload, std::ostream& line) {
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-gpu-split version=1"
         << " pid=" << getpid() << " context=" << payload.context << " source=" << payload.source
         << " backend_frame=" << payload.backendFrame << " game_done_ns=" << payload.gameDoneNs
         << " source_copy_end_ns=" << payload.sourceCopyEndNs << " fg_start_ns=" << payload.fgStartNs
         << " prepass_end_ns=" << payload.prepassEndNs << " generated_end_ns=" << payload.generatedEndNs
         << " batch_end_ns=" << payload.batchEndNs << '\n';
}

void formatPacingGpuPass(const PacingGpuPassPayload& payload, std::ostream& line) {
    const std::string_view name(payload.name.data());
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-gpu-pass version=1"
         << " pid=" << getpid() << " context=" << payload.context << " overlap=" << (payload.overlap ? 1 : 0)
         << " step=" << payload.step << " name=" << name << " batches=" << payload.batches
         << " p50_ns=" << payload.p50Ns << " p95_ns=" << payload.p95Ns << '\n';
}

static void formatTimedCopy(const char* operation, const PacingOutputGpuPayload& payload, std::ostream& line) {
    line << "framegen-vulkan: present diagnostics: operation=" << operation << " version=1"
         << " pid=" << getpid() << " context=" << payload.context << " source=" << payload.source
         << " available=" << payload.available << " copy_start_ns=" << payload.copyStartNs
         << " copy_end_ns=" << payload.copyEndNs;
}

void formatPacingOutputGpu(const PacingOutputGpuPayload& payload, std::ostream& line) {
    formatTimedCopy("pacing-profile-output-gpu", payload, line);
    line << " staging_start_ns=" << payload.stagingStartNs << " staging_end_ns=" << payload.stagingEndNs << '\n';
}

void formatPacingRealCopyGpu(const PacingOutputGpuPayload& payload, std::ostream& line) {
    formatTimedCopy("pacing-profile-real-copy-gpu", payload, line);
    // FI2: game when the copy was submitted on the application's queue at the present call, private on the worker.
    line << " queue=" << (payload.gameQueue ? "game" : "private") << '\n';
}

void formatPacingPresentMarkerGpu(const PacingPresentMarkerPayload& payload, std::ostream& line) {
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-present-marker-gpu version=1"
         << " pid=" << getpid() << " context=" << payload.context << " source=" << payload.source
         << " feedback=" << payload.feedback << " generated=" << payload.generated
         << " available=" << payload.available << " marker_start_ns=" << payload.markerStartNs
         << " marker_end_ns=" << payload.markerEndNs << '\n';
}

void formatAcquireDriver(const PacingAcquireDriverPayload& payload, std::ostream& line) {
    const auto ns = [](auto time) { return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count(); };
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-acquire-driver version=1"
         << " pid=" << getpid() << " tid=" << payload.thread << " call_id=" << payload.callId
         << " attempt=" << payload.attempt << " timeout_ns=" << payload.timeout
         << " started_ns=" << ns(payload.started) << " finished_ns=" << ns(payload.finished)
         << " result=" << payload.result << " image_index=" << payload.imageIndex << '\n';
}

void formatApplication(const PacingApplicationPayload& payload, std::ostream& trace) {
    const auto ns = [](auto time) { return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count(); };
    trace << "framegen-vulkan: present diagnostics: operation=pacing-profile-application version=1"
          << " pid=" << getpid() << " tid=" << payload.thread << " stream=" << payload.stream << " ordinal=" << payload.ordinal
          << " context=" << payload.context << " source=" << payload.source << " swapchain=" << payload.swapchain
          << " api=" << (payload.api == ApplicationFrameApi::Present ? "present" : payload.api == ApplicationFrameApi::AcquireKhr ? "acquire" : "acquire2")
          << " image_index=" << payload.imageIndex
          << " started_ns=" << ns(payload.started) << " finished_ns=" << ns(payload.finished)
          << " result_known=" << payload.result.has_value() << " result=" << payload.result.value_or(VK_SUCCESS)
          << " timeout_known=" << payload.timeout.has_value() << " timeout_ns=" << payload.timeout.value_or(0)
          << " join_known=" << payload.deferredJoin.has_value() << " join_ns=" << (payload.deferredJoin ? std::chrono::duration_cast<std::chrono::nanoseconds>(*payload.deferredJoin).count() : 0)
          << " configuration_known=" << payload.configurationUpdate.has_value() << " configuration_ns=" << (payload.configurationUpdate ? std::chrono::duration_cast<std::chrono::nanoseconds>(*payload.configurationUpdate).count() : 0)
          << " caller=" << reinterpret_cast<void*>(payload.caller);
    if (payload.callId)
        trace << " call_id=" << payload.callId << " lower_calls=" << payload.lowerCalls
              << " lower_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(payload.lowerDuration).count();
    trace << '\n';
}

void formatWait(const PacingWaitPayload& payload, std::ostream& trace) {
    trace << "framegen-vulkan: present diagnostics: operation=pacing-profile-wait version=1"
          << " pid=" << getpid() << " tid=" << payload.thread << " swapchain=" << payload.swapchain
          << " api=" << (payload.api == PresentWaitApi::Khr ? "KHR" : "KHR2")
          << " present_id=" << payload.presentId << " timeout_ns=" << payload.timeout << " result=" << payload.result
          << " started_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(payload.started.time_since_epoch()).count()
          << " finished_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(payload.finished.time_since_epoch()).count() << '\n';
}

void formatCopy(const CopySlot& slot, std::ostream& line) {
    const auto ns = [](auto time) { return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count(); };
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-copy version=1"
         << " pid=" << getpid() << " tid=" << slot.thread
         << " context=" << slot.context << " source=" << slot.source << " swapchain=" << slot.swapchain
         << " generated=" << slot.generated << " image_index=" << slot.imageIndex << " pass=" << slot.pass
         << " device=" << slot.device << " queue=" << slot.queue << " command=" << slot.command
         << " started_ns=" << ns(slot.started) << " finished_ns=" << ns(slot.finished) << " submitted=" << slot.submitted
         << " timeline_wait=" << slot.timelineWait << " wait_value=" << slot.waitValue
         << " timeline_signal=" << slot.timelineSignal << " signal_value=" << slot.signalValue
         << " fence=" << slot.fence;
    const auto semaphores = [&](std::string_view name, size_t count, const std::array<VkSemaphore, 16>& values) {
        const size_t recorded = std::min(count, size_t{16});
        line << ' ' << name << "_count=" << count << ' ' << name << "_recorded=" << recorded;
        for (size_t i = 0; i < recorded; ++i) line << ' ' << name << '_' << i << '=' << values[i];
    };
    semaphores("wait", slot.waitCount, slot.waits);
    semaphores("signal", slot.signalCount, slot.signals);
    line << '\n';
}

void formatApplicationWindowText(const ApplicationWindowPayload& payload,
        const ApplicationFrameTiming::Window& window, std::ostream& line) {
    line << "framegen-vulkan: present diagnostics: operation=application-queue-present"
         << " stream=" << payload.stream << " pid=" << getpid()
         << " tid=" << payload.thread
         << " sample_end_monotonic_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(payload.finished.time_since_epoch()).count()
         << " queue=" << reinterpret_cast<void*>(payload.caller)
         << " swapchain=" << payload.swapchain
         << " api=KHR"
         << " calls=" << window.calls
         << " entry_interval_samples=" << window.entryInterval.count
         << " entry_interval_mean_ms=" << window.entryInterval.mean()
         << " entry_interval_max_ms=" << window.entryInterval.maximum
         << " duration_mean_ms=" << window.duration.mean()
         << " duration_max_ms=" << window.duration.maximum
         << " includes_intentional_pacing=1"
         << " configuration_update_samples=" << window.configurationUpdate.count
         << " configuration_update_mean_ms=" << window.configurationUpdate.mean()
         << " configuration_update_max_ms=" << window.configurationUpdate.maximum
         << " deferred_join_samples=" << window.deferredJoin.count
         << " deferred_join_mean_ms=" << window.deferredJoin.mean()
         << " deferred_join_max_ms=" << window.deferredJoin.maximum;
    line << '\n';
}

void formatPresentWaitWindowText(const PresentWaitWindowPayload& payload,
        const ApplicationPresentWait::Window& window, std::ostream& line) {
    line << "framegen-vulkan: present diagnostics: operation=application-present-wait"
         << " waiter=" << payload.waiter << " pid=" << getpid()
         << " tid=" << payload.thread
         << " sample_end_monotonic_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(payload.finished.time_since_epoch()).count()
         << " device=" << payload.device << " swapchain=" << payload.swapchain
         << " api=" << (payload.api == PresentWaitApi::Khr ? "KHR" : "KHR2")
         << " calls=" << window.calls << " successful=" << window.successful
         << " timeouts=" << window.timeouts << " errors=" << window.errors
         << " polls=" << window.polls
         << " first_present_id=" << window.firstPresentId
         << " last_present_id=" << window.lastPresentId
         << " duration_mean_ms=" << window.duration.mean()
         << " duration_max_ms=" << window.duration.maximum;
    if (window.firstFailure) {
        const auto& failure = *window.firstFailure;
        line << " first_failure_result=" << failure.result
             << " first_failure_present_id=" << failure.presentId
             << " first_failure_timeout_ns=" << failure.timeout
             << " first_failure_duration_ms=" << failure.durationMs;
    }
    line << '\n';
}

void formatCall(const PacingCallPayload& payload, std::ostream& line) {
    constexpr std::array<std::string_view, static_cast<size_t>(LayerCall::Count)> names{
        "queue-present", "acquire-next-image", "acquire-next-image2", "queue-submit2", "wait-semaphores",
        "queue-wait-idle", "device-wait-idle", "wait-for-present", "wait-for-present2",
        "release-swapchain-images", "get-swapchain-images", "latency-sleep"};
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-call version=1"
         << " pid=" << getpid() << " tid=" << payload.thread << " call=" << names[static_cast<size_t>(payload.call)]
         << " enter_ns=" << epochNanoseconds(payload.enter) << " exit_ns=" << epochNanoseconds(payload.exit)
         << " result=" << payload.result;
    if (payload.call == LayerCall::WaitForPresent || payload.call == LayerCall::WaitForPresent2)
        line << " present_id=" << payload.presentId << " timeout_ns=" << payload.timeoutNs;
    else if (payload.call == LayerCall::WaitSemaphores)
        line << " wait_count=" << payload.waitCount << " timeout_ns=" << payload.timeoutNs;
    line << '\n';
}

void formatCallWindow(const PacingCallWindowPayload& payload, std::ostream& line) {
    constexpr std::array<std::string_view, static_cast<size_t>(LayerCall::Count)> names{
        "queue-present", "acquire-next-image", "acquire-next-image2", "queue-submit2", "wait-semaphores",
        "queue-wait-idle", "device-wait-idle", "wait-for-present", "wait-for-present2",
        "release-swapchain-images", "get-swapchain-images", "latency-sleep"};
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-calls version=1"
         << " pid=" << getpid() << " tid=" << payload.thread << " call=" << names[static_cast<size_t>(payload.call)]
         << " count=" << payload.count << " sum_ms=" << static_cast<double>(payload.sumNs) / 1e6
         << " max_ms=" << static_cast<double>(payload.maxNs) / 1e6 << '\n';
}

void formatAppFrame(const PacingAppFramePayload& payload, std::ostream& line) {
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-app-frame version=1"
         << " pid=" << getpid() << " tid=" << payload.thread << " context=" << payload.context << " source=" << payload.source
         << " enter_ns=" << epochNanoseconds(payload.enter) << " exit_ns=" << epochNanoseconds(payload.exit)
         << " previous_exit_ns=" << epochNanoseconds(payload.previousExit)
         << " inside_layer_ns=" << payload.insideLayer << " inside_submit_ns=" << payload.insideSubmit
         << " inside_wait_ns=" << payload.insideWait << " inside_acquire_ns=" << payload.insideAcquire << '\n';
}

void formatAcquire(const PacingAcquirePayload& payload, std::ostream& line) {
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-app-acquire version=1"
         << " pid=" << getpid() << " tid=" << payload.thread
         << " enter_ns=" << epochNanoseconds(payload.enter) << " exit_ns=" << epochNanoseconds(payload.exit)
         << " path=" << (payload.host ? "host" : "queued") << " lock_wait_ns=" << payload.lockWaitNs
         << " tail_wait_value=" << payload.tailWaitValue << " tail_seen_ns=" << payload.tailSeenNs << '\n';
}

void formatReflexMarker(const ReflexMarkerPayload& payload, std::ostream& line) {
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-reflex-marker version=1"
         << " pid=" << getpid() << " tid=" << payload.thread << " present_id=" << payload.presentId
         << " marker=" << static_cast<int>(payload.marker) << " ns=" << payload.ns
         << (payload.layer ? " origin=layer" : " origin=app") << '\n';
}

void formatReflexSleep(const ReflexSleepPayload& payload, std::ostream& line) {
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-reflex-sleep version=1"
         << " pid=" << getpid() << " tid=" << payload.thread
         << " enter_ns=" << epochNanoseconds(payload.enter) << " exit_ns=" << epochNanoseconds(payload.exit)
         << " result=" << payload.result << '\n';
}

void formatReflexMode(const ReflexModePayload& payload, std::ostream& line) {
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-reflex-mode version=2"
         << " pid=" << getpid() << " low_latency=" << payload.lowLatency << " boost=" << payload.boost
         << " min_interval_us=" << payload.minimumIntervalUs << " result=" << payload.result << '\n';
}

void formatReflexTiming(const ReflexTimingPayload& payload, std::ostream& line) {
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-reflex-timing version=1"
         << " pid=" << getpid() << " tid=" << payload.thread
         << " query_ns=" << payload.queryNs << " now_ns=" << payload.nowNs
         << " present_id=" << payload.presentId
         << " input_us=" << payload.inputUs << " sim_start_us=" << payload.simStartUs
         << " sim_end_us=" << payload.simEndUs << " submit_start_us=" << payload.submitStartUs
         << " submit_end_us=" << payload.submitEndUs << " present_start_us=" << payload.presentStartUs
         << " present_end_us=" << payload.presentEndUs << " driver_start_us=" << payload.driverStartUs
         << " driver_end_us=" << payload.driverEndUs << " os_queue_start_us=" << payload.osQueueStartUs
         << " os_queue_end_us=" << payload.osQueueEndUs << " gpu_start_us=" << payload.gpuStartUs
         << " gpu_end_us=" << payload.gpuEndUs << '\n';
}

void formatSubmitTag(const SubmitTagPayload& payload, std::ostream& line) {
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-submit-tag version=1"
         << " pid=" << getpid() << " tid=" << payload.thread << " now_ns=" << payload.nowNs
         << " queue=" << payload.queue << " present_id=" << payload.presentId
         << " previous_id=" << payload.previousId << " submits=" << payload.submits
         << " untagged=" << payload.untagged << '\n';
}

void formatQueueFrame(const QueueFramePayload& payload, std::ostream& line) {
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-queue-frame version=1"
         << " pid=" << getpid() << " queue=" << payload.queue << " family=" << payload.family
         << " present_id=" << payload.presentId << " submits=" << payload.submits
         << " submit_ns=" << payload.submitNs << " last_submit_ns=" << payload.lastSubmitNs
         << " gpu_start_ns=" << payload.gpuStartNs << " gpu_end_ns=" << payload.gpuEndNs
         << " available=" << payload.available << '\n';
}

void formatWaylandSample(const WaylandSamplePayload& payload, std::ostream& out) {
    constexpr std::array<const char*, 6> names{"request", "call", "presented", "discarded", "unavailable", "abandoned"};
    const auto& s = payload.sample;
    out << "framegen-vulkan: present diagnostics: operation=pacing-profile-wayland version=2 pid=" << getpid()
        << " surface_token=" << payload.token << " event=" << names[static_cast<size_t>(s.event)]
        << " feedback=" << s.id;
    if (s.event == WaylandEvent::Request || s.event == WaylandEvent::Unavailable) {
        out << " context=" << s.context << " source=" << s.source
            << " image_index=" << s.image << " generated=" << s.generated
            << " requested_ns=" << s.requested << " clock_id=" << s.clock;
        if (s.event == WaylandEvent::Request)
            out << " presentation_now_ns=" << s.presentationNow << " calibration_span_ns=" << s.calibrationSpan;
        else out << " reason=" << s.reason;
    } else if (s.event == WaylandEvent::Call) {
        out << " started_ns=" << s.requested << " finished_ns=" << s.finished << " result=" << s.result;
    } else {
        out << " received_ns=" << s.received;
        if (s.event == WaylandEvent::Presented)
            out << " clock_id=" << s.clock << " presented_ns=" << s.presented << " sequence=" << s.sequence
                << " refresh_ns=" << s.refresh << " flags=" << s.flags << " output_id=" << s.output;
    }
    out << '\n';
}

void formatWaylandGap(const WaylandGapPayload& payload, std::ostream& out) {
    out << "framegen-vulkan: present diagnostics: operation=pacing-profile-wayland-gap version=1 pid="
        << getpid() << " surface_token=" << payload.token << " lost_records=" << payload.lost << '\n';
}

void formatPacingWindowBody(const PacingWindowPayload& payload, const PacingProfile::Window& window,
        std::ostream& line) {
    constexpr std::array<std::string_view, PacingProfile::MetricCount> names{
        "source_interval", "layer_present", "outside_layer", "output_interval",
        "within_batch", "between_batches", "lower_present", "source_cap_wait",
        "readiness_wait", "output_wait", "profiler_publish", "planned_output_interval",
        "output_wake_lateness", "source_cap_wake_lateness", "source_deadline_lateness",
        "gpu_prepass", "gpu_generation", "gpu_output", "gpu_span", "gpu_readback",
        "present_target_interval"};
    const std::string_view wsi(payload.wsi.data());
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile"
         << " version=2 context=" << payload.context << " pid=" << getpid()
         << " sample_end_monotonic_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(payload.finished.time_since_epoch()).count()
         << " window_ms=" << std::chrono::duration<double, std::milli>(window.elapsed).count()
         << " present_mode=" << payload.mode << " sources=" << window.sources
         << " swapchain=" << payload.swapchain
         << " wsi=" << wsi << " wayland_requested=" << pacingWaylandProfileEnabled()
         << " wayland_observer=" << payload.waylandObserver
         << " output_attempts=" << window.outputs << " output_errors=" << window.errors
         << " ready_batches=" << window.readyBatches << " cap_batches=" << window.capBatches
         << " worker_batches=" << window.workerBatches
         << " worker_readiness_misses=" << window.workerReadinessMisses
         << " generated_skip_readiness=" << window.generatedSkips[PacingProfile::SkipReadiness]
         << " generated_skip_expedite=" << window.generatedSkips[PacingProfile::SkipExpedite]
         << " generated_skip_give_up=" << window.generatedSkips[PacingProfile::SkipGiveUp]
         << " generated_skip_acquire=" << window.generatedSkips[PacingProfile::SkipAcquire]
         << " generated_skip_other=" << window.generatedSkips[PacingProfile::SkipOther]
         << " late_readiness_batches=" << window.lateReadinessBatches
         << " generated_compressed=" << window.generatedCompressed
         << " internal_real_prepared=" << window.internalRealPrepared
         << " internal_real_at_deadline=" << window.internalRealAtDeadline
         << " generated_prepared=" << window.generatedPrepared
         << " generated_at_deadline=" << window.generatedAtDeadline
         << " internal_images=" << (window.internalImages ? 1 : 0)
         << " output_trace_version=" << (pacingFrameTraceEnabled() ? 2 : 1) << " output_trace_samples=" << (pacingFrameTraceEnabled() ? window.outputs : window.outputTraceCount)
         << " output_trace_dropped=" << (pacingFrameTraceEnabled() ? 0 : window.outputTraceDropped)
         << " wait_capture=1"
         << " fifo_caller_timing_sources=" << window.fifoCallerTimingSources
         << " effective_base_cap_fps=" << window.effectiveCapFps
         << " frame_generation_enabled=" << window.frameGenerationEnabled
         << " adaptive=" << window.adaptive
         << " configured_multiplier=" << window.configuredMultiplier
         << " configured_base_cap_fps=" << window.configuredCapFps;
    // What each present actually asked for: the per-present mode overrides
    // the swapchain's create-time mode (present_mode is the layer's tracking).
    line << " swapchain_present_mode=" << window.swapchainPresentMode;
    const auto modes = [&](std::string_view key, const auto& counts) {
        line << ' ' << key << '=';
        bool any = false;
        for (size_t i = 0; i < counts.size(); ++i) {
            if (!counts[i]) continue;
            line << (any ? "," : "") << PacingProfile::presentModeSlotNames[i] << ':' << counts[i];
            any = true;
        }
        if (!any) line << '-';
    };
    modes("application_present_modes", window.applicationModes);
    modes("real_present_modes", window.realModes);
    modes("generated_present_modes", window.generatedModes);
    line << " gpu_requested=" << pacingGpuProfileEnabled();
    constexpr std::array<std::string_view, PacingProfile::OutputTimingCount> timingNames{
        "driver", "application", "ext", "google", "bridge"};
    for (size_t i = 0; i < timingNames.size(); ++i)
        line << " timing_" << timingNames[i] << "_attempts=" << window.timingAttempts[i];
    line << " present_target_discontinuities=" << window.targetDiscontinuities;
    // low_latency: the setting, the mean release delay of the capped sources, and which
    // release-token kinds the application has used, in ReleaseGate::kindBit order, then the
    // acquire fallback, then reflex (bit 4: Reflex stepped the release delay aside in the
    // window; low_latency.hpp reflexKindBit). None when there is none.
    {
        constexpr std::array<std::string_view, 5> holdNames{"vkd3d", "present-id", "dxvk", "acquire", "reflex"};
        line << " low_latency=" << (window.lowLatency ? 1 : 0)
             << " low_latency_extra_ms=" << window.lowLatencyExtra.mean() << " low_latency_hold=";
        bool any = false;
        for (size_t bit = 0; bit < holdNames.size(); ++bit) {
            if (!(window.lowLatencyKinds & (1u << bit))) continue;
            line << (any ? "," : "") << holdNames[bit];
            any = true;
        }
        if (!any) line << "none";
        // gpu_release_ms: the mean hold armed on the window's capped presents (the predicted release
        // minus the game's wait return, or the stepping hold), zero with none. gpu_prev_end_ms: the mean
        // of the resolved previous-batch-end leads (negative: the frame came after the GPU freed its
        // batch). gpu_release: off, pred or fence, the latest present's mode (low_latency.hpp).
        const std::string_view gpuMode = window.gpuReleaseMode == GpuReleaseMode::Predicted ? "pred"
            : window.gpuReleaseMode == GpuReleaseMode::Fence ? "fence"
            : window.gpuReleaseMode == GpuReleaseMode::Waiting ? "wait" : "off";
        line << " gpu_release_ms=" << window.gpuReleaseHold.mean()
             << " gpu_prev_end_ms=" << (window.gpuPrevEndCount
                    ? window.gpuPrevEndSumMs / static_cast<double>(window.gpuPrevEndCount) : 0.0)
             << " gpu_release=" << gpuMode;
        // just_in_time: whether released batches start at their own readiness in this window.
        // batch_hold: the batch_hold setting (0 starts them at readiness, with no 2 ms hold).
        // batch_phase_hold_ms: the mean of their starts after readiness (BatchPhaseLock), zero with none.
        line << " just_in_time=" << (window.justInTime ? 1 : 0)
             << " batch_hold=" << (window.batchHold ? 1 : 0)
             << " batch_phase_hold_ms=" << window.batchPhaseHold.mean();
    }
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
    constexpr std::array<std::string_view, 3> lockNames{"acquire", "submit", "present"};
    const std::array<const PacingProfile::Samples*, 3> lockSamples{
        &window.workerLockAcquire, &window.workerLockSubmit, &window.workerLockPresent};
    for (size_t index = 0; index < lockNames.size(); ++index)
        line << " worker_lock_" << lockNames[index] << "_mean_ms=" << lockSamples[index]->mean()
             << " worker_lock_" << lockNames[index] << "_max_ms=" << lockSamples[index]->maximum;
    for (size_t index = 0; index < names.size(); ++index) {
        const auto& sample = window.metrics[index];
        line << ' ' << names[index] << "_samples=" << sample.count
             << ' ' << names[index] << "_min_ms=" << sample.minimum
             << ' ' << names[index] << "_mean_ms=" << sample.mean()
             << ' ' << names[index] << "_max_ms=" << sample.maximum
             << ' ' << names[index] << "_over_33ms=" << sample.over33ms;
    }
    if (const auto presentIdReleases = fgvk::layer::ReleaseGate::global().presentIdReleases(); presentIdReleases)
        line << " present_id_gate_releases=" << presentIdReleases;
    if (fgvk::layer::realOffsetAuto()) {
        // OS1: screen_samples counts the screen samples accepted since the previous summary.
        line << " real_offset_us=" << fgvk::layer::realOffsetAutoNs.load(std::memory_order_relaxed) / 1000
             << " real_offset_auto=1 real_offset_samples="
             << fgvk::layer::realOffsetAutoSamples.load(std::memory_order_relaxed)
             << " real_offset_held=" << fgvk::layer::realOffsetAutoHeld.load(std::memory_order_relaxed)
             << " offset_source=" << (fgvk::layer::realOffsetScreen.load(std::memory_order_relaxed) ? "screen" : "marker")
             << " screen_samples=" << fgvk::layer::realOffsetScreenSamples.exchange(0, std::memory_order_relaxed)
             << " screen_dropped=" << fgvk::layer::realOffsetScreenDropped.load(std::memory_order_relaxed);
    } else {
        line << " real_offset_us=0";
    }
    // GT1: tick_lock=1 while the grid is valid on a gamescope surface; misses, early catches and snapped outputs
    // count since the previous summary.
    line << " tick_lock=" << (fgvk::layer::tickLockOn.load(std::memory_order_relaxed) ? 1 : 0)
         << " tick_period_us=" << fgvk::layer::tickLockPeriodNs.load(std::memory_order_relaxed) / 1000
         << " tick_lead_us=" << fgvk::layer::tickLockLeadNs.load(std::memory_order_relaxed) / 1000
         << " tick_misses=" << fgvk::layer::tickLockMisses.exchange(0, std::memory_order_relaxed)
         << " tick_early=" << fgvk::layer::tickLockEarly.exchange(0, std::memory_order_relaxed)
         << " tick_snapped=" << fgvk::layer::tickLockSnapped.exchange(0, std::memory_order_relaxed);
    // RL2: the out-of-band type applied to the private present queue (0 none, 1 present, 2 render), and the backend
    // queue priority that FGVK_BACKEND_QUEUE_PRIORITY selects (1 when unset or rejected), so a capture names its arm.
    line << " reflex_oob=" << fgvk::layer::reflexOutOfBandApplied.load(std::memory_order_relaxed)
         << " backend_queue_priority="
         << vk::parseBackendQueuePriority(std::getenv("FGVK_BACKEND_QUEUE_PRIORITY")).priority;
    // RP1: real_lead=1 while the real present lead is in effect, and its current lead in microseconds.
    line << " real_lead=" << (fgvk::layer::realPresentLeadOn.load(std::memory_order_relaxed) ? 1 : 0)
         << " real_lead_us=" << fgvk::layer::realPresentLeadNs.load(std::memory_order_relaxed) / 1000;
    // QH2: 1 when this context's generated copies ran on the transfer queue, and the device created one.
    line << " transfer_copy="
         << (window.transferCopy && fgvk::layer::transferCopyApplied.load(std::memory_order_relaxed) ? 1 : 0);
    // GP2: 1 when FGVK_BACKEND_OVERLAP=1 grouped the backend recording, so a capture names its arm.
    line << " backend_overlap="
         << (vk::parseBackendOverlap(std::getenv("FGVK_BACKEND_OVERLAP")) ? 1 : 0);
    // FI1: 1 when this context's generated frames are scheduled on the application's device (on by default,
    // FGVK_FG_INLINE=0 turns it off).
    line << " fg_inline=" << (window.inlineAttached ? 1 : 0);
    // FI2: outputs of attached, eligible batches copied at the call (generated, real), and worker fallbacks.
    line << " inline_call_gen=" << window.inlineCallGenerated << " inline_call_real=" << window.inlineCallReal
         << " inline_call_fallback=" << window.inlineCallFallback;
    // FP1: the fallbacks by reason (they sum to inline_call_fallback), and the outputs served from a parked image.
    line << " inline_call_fallback_lock=" << window.inlineCallFallbackLock
         << " inline_call_fallback_acquire=" << window.inlineCallFallbackAcquire
         << " inline_call_fallback_budget=" << window.inlineCallFallbackBudget
         << " inline_call_fallback_cascade=" << window.inlineCallFallbackCascade
         << " inline_call_fallback_other=" << window.inlineCallFallbackOther
         << " inline_call_parked=" << window.inlineCallParked;
    // FP4: the waits for the worker's present at a call output: those that did not return None, by outcome, and their total in us.
    line << " inline_call_wait=" << window.inlineCallWait
         << " inline_call_wait_parked=" << window.inlineCallWaitParked
         << " inline_call_wait_unlocked=" << window.inlineCallWaitUnlocked
         << " inline_call_wait_timeout=" << window.inlineCallWaitTimeout
         << " inline_call_wait_us=" << window.inlineCallWaitUs;
    // FI4: the outputs of the last batch that took the call path (generated outputs plus the real), and the swapchain's call
    // images (outputs per batch at the profile's capacity, zero where the worker path cannot run).
    line << " inline_call_outputs=" << window.inlineCallOutputs << " inline_call_spare=" << window.inlineCallSpare;
    // FP3: the worker's lower park outcomes (Drained is not counted). Each outcome is its own field.
    const auto parkOutcome = [&window](const fgvk::layer::ParkOutcome outcome) {
        return window.parkOutcomes[static_cast<size_t>(outcome)];
    };
    line << " park_ok=" << parkOutcome(fgvk::layer::ParkOutcome::Parked)
         << " park_full=" << parkOutcome(fgvk::layer::ParkOutcome::Full)
         << " park_budget=" << parkOutcome(fgvk::layer::ParkOutcome::Budget)
         << " park_slot_busy=" << parkOutcome(fgvk::layer::ParkOutcome::SlotBusy)
         << " park_fence=" << parkOutcome(fgvk::layer::ParkOutcome::Fence)
         << " park_acquire=" << parkOutcome(fgvk::layer::ParkOutcome::Acquire)
         << " park_error=" << parkOutcome(fgvk::layer::ParkOutcome::Error);
    // XH2a: the worker's real polls that found no lower image, by reason, and the longest worker real wait in us.
    line << " worker_real_poll_slot_busy=" << window.workerRealPollSlotBusy
         << " worker_real_poll_fence=" << window.workerRealPollFence
         << " worker_real_poll_acquire=" << window.workerRealPollAcquire
         << " worker_real_poll_error=" << window.workerRealPollError
         << " worker_real_wait_max_us=" << window.workerRealWaitMaxUs;
    // PT1: the relative targets the worker forwarded, by output (reals split by their batch, generated outputs given their share),
    // and the last forwarded target of each in us.
    line << " timing_relative_split_real=" << window.timingRelativeReal
         << " timing_relative_generated=" << window.timingRelativeGenerated
         << " timing_relative_real_last_us=" << window.timingRelativeRealLastNs / 1000
         << " timing_relative_generated_last_us=" << window.timingRelativeGeneratedLastNs / 1000;
    // PT2: the absolute shares given to generated outputs (the last share in us), and the application's target kinds on the
    // worker reals that presented.
    line << " timing_absolute_generated=" << window.timingAbsoluteGenerated
         << " timing_absolute_generated_lead_us=" << window.timingAbsoluteGeneratedLeadNs / 1000
         << " timing_app_relative=" << window.timingAppRelative
         << " timing_app_absolute=" << window.timingAppAbsolute
         << " timing_app_zero=" << window.timingAppZero;
    line << '\n';
    for (size_t i = 0; i < window.outputTraceCount; ++i) {
        const auto& sample = window.outputTrace[i];
        if (!pacingFrameTraceEnabled()) appendPacingOutput(line, payload.context, payload.swapchain, sample);
    }
}

// ---- The writer. Everything below runs on the writer thread (or a flush caller holding writeMutex). ----

void noteThread(Recorder& s, const uint32_t thread) {
    if (!thread) return;
    for (size_t i = 0; i < s.trackedCount; ++i)
        if (s.tracked[i].thread == thread) return;
    if (s.trackedCount < s.tracked.size())
        s.tracked[s.trackedCount++] = TrackedThread{.thread = thread};
}

// Reads one thread's kernel accounting from /proc. Returns false when the thread has gone.
bool readThreadAccounting(const uint32_t thread, uint64_t& runNs, uint64_t& waitNs,
        uint64_t& voluntary, uint64_t& involuntary) {
    const std::string base = "/proc/self/task/" + std::to_string(thread) + "/";
    std::ifstream schedule(base + "schedstat");
    if (!schedule) return false;
    schedule >> runNs >> waitNs;
    if (!schedule) return false;
    std::ifstream status(base + "status");
    if (!status) return false;
    std::string line;
    voluntary = involuntary = 0;
    while (std::getline(status, line)) {
        if (line.starts_with("voluntary_ctxt_switches:"))
            voluntary = std::strtoull(line.c_str() + std::string_view("voluntary_ctxt_switches:").size(), nullptr, 10);
        else if (line.starts_with("nonvoluntary_ctxt_switches:"))
            involuntary = std::strtoull(line.c_str() + std::string_view("nonvoluntary_ctxt_switches:").size(), nullptr, 10);
    }
    return true;
}

void writeThreadRecords(Recorder& s, const Clock::time_point now) {
    for (auto& worker : s.workers) {
        const auto thread = worker.load(std::memory_order_acquire);
        if (!thread) continue;
        bool known = false;
        for (size_t i = 0; i < s.trackedCount; ++i) known = known || s.tracked[i].thread == thread;
        if (!known && s.trackedCount < s.tracked.size()) s.tracked[s.trackedCount++] = TrackedThread{.thread = thread};
        for (size_t i = 0; i < s.trackedCount; ++i)
            if (s.tracked[i].thread == thread) s.tracked[i].worker = true;
    }
    size_t kept = 0;
    for (size_t i = 0; i < s.trackedCount; ++i) {
        auto tracked = s.tracked[i];
        uint64_t run{}, wait{}, voluntary{}, involuntary{};
        if (!readThreadAccounting(tracked.thread, run, wait, voluntary, involuntary)) continue;
        if (tracked.hasPrevious) {
            auto line = classicStream();
            line << "framegen-vulkan: present diagnostics: operation=pacing-profile-thread version=1"
                 << " pid=" << getpid() << " tid=" << tracked.thread
                 << " role=" << (tracked.worker ? "worker" : "app")
                 << " run_ms=" << static_cast<double>(run - tracked.runNs) / 1e6
                 << " wait_ms=" << static_cast<double>(wait - tracked.waitNs) / 1e6
                 << " voluntary=" << (voluntary - tracked.voluntary)
                 << " involuntary=" << (involuntary - tracked.involuntary)
                 << " now_ns=" << trace_recorder::nanoseconds(now) << '\n';
            s.sink.append(line.str());
        }
        tracked.hasPrevious = true;
        tracked.runNs = run;
        tracked.waitNs = wait;
        tracked.voluntary = voluntary;
        tracked.involuntary = involuntary;
        s.tracked[kept++] = tracked;
    }
    s.trackedCount = kept;
}

void writeRecorderSecond(Recorder& s, const Clock::time_point now) {
    const auto probeMax = s.probeMaxNs.exchange(0, std::memory_order_relaxed);
    const auto probeSum = s.probeSumNs.exchange(0, std::memory_order_relaxed);
    const auto us = [](int64_t nanoseconds) { return static_cast<double>(nanoseconds) / 1e3; };
    auto line = classicStream();
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-recorder version=1"
         << " pid=" << getpid() << " records=" << s.second.enqueued << " dropped=" << s.second.dropped
         << " window_slot_drops=" << s.second.slotDrops
         << " enqueue_max_us=" << us(s.second.enqueueMaxNs) << " enqueue_sum_us=" << us(s.second.enqueueSumNs)
         << " writer_lag_max_ms=" << static_cast<double>(s.second.lagMaxNs) / 1e6
         << " writer_cycle_max_ms=" << static_cast<double>(s.second.cycleMaxNs) / 1e6
         << " bytes=" << s.sink.captured()
         << " display_probe_max_us=" << static_cast<double>(probeMax) / 1e3
         << " display_probe_sum_us=" << static_cast<double>(probeSum) / 1e3
         << " queue_frame_dropped=" << fgvk::layer::queueFrameDropped.exchange(0, std::memory_order_relaxed)
         << " queue_frame_skipped=" << fgvk::layer::queueFrameSkipped.exchange(0, std::memory_order_relaxed)
         << " now_ns=" << trace_recorder::nanoseconds(now) << '\n';
    s.sink.append(line.str());
    writeThreadRecords(s, now);
    s.second = SecondState{};
    s.second.start = now;
}

void handleRecord(Recorder& s, const trace_recorder::TraceRecord& record, const int64_t cycleStartNs) {
    const auto lag = cycleStartNs - record.enqueuedNs;
    s.second.lagMaxNs = std::max(s.second.lagMaxNs, lag);
    const auto load = [&](auto& value) {
        std::memcpy(&value, record.payload.data(), sizeof(value));
    };
    const auto emit = [&](auto&& format) {
        auto line = classicStream();
        format(line);
        s.sink.append(line.str());
    };
    switch (record.kind) {
    case RecordKind::None:
    case RecordKind::Count:
        break;
    case RecordKind::Text: {
        TextPayload text{};
        load(text);
        if (text.text) {
            s.sink.append(*text.text);
            s.pendingText.fetch_sub(text.bytes, std::memory_order_relaxed);
            delete text.text;
        }
        break;
    }
    case RecordKind::PacingWindow: {
        PacingWindowPayload payload{};
        load(payload);
        const auto& window = s.pools.pacingWindows.at(payload.slot);
        emit([&](std::ostream& line) { formatPacingWindow(payload, window, line); });
        s.pools.pacingWindows.release(payload.slot);
        break;
    }
    case RecordKind::PacingOutput: {
        PacingOutputPayload payload{};
        load(payload);
        emit([&](std::ostream& line) { formatPacingOutput(payload, line); });
        break;
    }
    case RecordKind::PacingSkip: {
        PacingSkipPayload payload{};
        load(payload);
        emit([&](std::ostream& line) { formatPacingSkip(payload, line); });
        break;
    }
    case RecordKind::PacingSource: {
        PacingSourcePayload payload{};
        load(payload);
        emit([&](std::ostream& line) { formatPacingSource(payload, line); });
        break;
    }
    case RecordKind::PacingGpu: {
        PacingGpuPayload payload{};
        load(payload);
        emit([&](std::ostream& line) { formatPacingGpu(payload, line); });
        break;
    }
    case RecordKind::PacingGpuSplit: {
        PacingGpuSplitPayload payload{};
        load(payload);
        emit([&](std::ostream& line) { formatPacingGpuSplit(payload, line); });
        break;
    }
    case RecordKind::PacingGpuPass: {
        PacingGpuPassPayload payload{};
        load(payload);
        emit([&](std::ostream& line) { formatPacingGpuPass(payload, line); });
        break;
    }
    case RecordKind::PacingOutputGpu: {
        PacingOutputGpuPayload payload{};
        load(payload);
        emit([&](std::ostream& line) { formatPacingOutputGpu(payload, line); });
        break;
    }
    case RecordKind::PacingRealCopyGpu: {
        PacingOutputGpuPayload payload{};
        load(payload);
        emit([&](std::ostream& line) { formatPacingRealCopyGpu(payload, line); });
        break;
    }
    case RecordKind::PacingPresentMarkerGpu: {
        PacingPresentMarkerPayload payload{};
        load(payload);
        emit([&](std::ostream& line) { formatPacingPresentMarkerGpu(payload, line); });
        break;
    }
    case RecordKind::PacingAcquireDriver: {
        PacingAcquireDriverPayload payload{};
        load(payload);
        noteThread(s, payload.thread);
        emit([&](std::ostream& line) { formatAcquireDriver(payload, line); });
        break;
    }
    case RecordKind::PacingApplication: {
        PacingApplicationPayload payload{};
        load(payload);
        noteThread(s, payload.thread);
        emit([&](std::ostream& line) { formatApplication(payload, line); });
        break;
    }
    case RecordKind::PacingWait: {
        PacingWaitPayload payload{};
        load(payload);
        noteThread(s, payload.thread);
        emit([&](std::ostream& line) { formatWait(payload, line); });
        break;
    }
    case RecordKind::PacingCopy: {
        PacingCopyPayload payload{};
        load(payload);
        emit([&](std::ostream& line) { formatCopy(s.pools.copies.at(payload.slot), line); });
        s.pools.copies.release(payload.slot);
        break;
    }
    case RecordKind::ApplicationPresentWindow: {
        ApplicationWindowPayload payload{};
        load(payload);
        noteThread(s, payload.thread);
        const auto& window = s.pools.applicationWindows.at(payload.slot);
        emit([&](std::ostream& line) { formatApplicationWindow(payload, window, line); });
        s.pools.applicationWindows.release(payload.slot);
        break;
    }
    case RecordKind::ApplicationPresentWaitWindow: {
        PresentWaitWindowPayload payload{};
        load(payload);
        noteThread(s, payload.thread);
        const auto& window = s.pools.presentWaitWindows.at(payload.slot);
        emit([&](std::ostream& line) { formatPresentWaitWindow(payload, window, line); });
        s.pools.presentWaitWindows.release(payload.slot);
        break;
    }
    case RecordKind::PacingCall: {
        PacingCallPayload payload{};
        load(payload);
        noteThread(s, payload.thread);
        emit([&](std::ostream& line) { formatCall(payload, line); });
        break;
    }
    case RecordKind::PacingCallWindow: {
        PacingCallWindowPayload payload{};
        load(payload);
        noteThread(s, payload.thread);
        emit([&](std::ostream& line) { formatCallWindow(payload, line); });
        break;
    }
    case RecordKind::PacingAppFrame: {
        PacingAppFramePayload payload{};
        load(payload);
        noteThread(s, payload.thread);
        emit([&](std::ostream& line) { formatAppFrame(payload, line); });
        break;
    }
    case RecordKind::PacingAcquire: {
        PacingAcquirePayload payload{};
        load(payload);
        emit([&](std::ostream& line) { formatAcquire(payload, line); });
        break;
    }
    case RecordKind::ReflexMarker: {
        ReflexMarkerPayload payload{};
        load(payload);
        noteThread(s, payload.thread);
        emit([&](std::ostream& line) { formatReflexMarker(payload, line); });
        break;
    }
    case RecordKind::ReflexSleep: {
        ReflexSleepPayload payload{};
        load(payload);
        noteThread(s, payload.thread);
        emit([&](std::ostream& line) { formatReflexSleep(payload, line); });
        break;
    }
    case RecordKind::ReflexMode: {
        ReflexModePayload payload{};
        load(payload);
        emit([&](std::ostream& line) { formatReflexMode(payload, line); });
        break;
    }
    case RecordKind::ReflexTiming: {
        ReflexTimingPayload payload{};
        load(payload);
        noteThread(s, payload.thread);
        emit([&](std::ostream& line) { formatReflexTiming(payload, line); });
        break;
    }
    case RecordKind::WaylandSample: {
        WaylandSamplePayload payload{};
        load(payload);
        emit([&](std::ostream& line) { formatWaylandSample(payload, line); });
        break;
    }
    case RecordKind::WaylandGap: {
        WaylandGapPayload payload{};
        load(payload);
        emit([&](std::ostream& line) { formatWaylandGap(payload, line); });
        break;
    }
    case RecordKind::SubmitTag: {
        SubmitTagPayload payload{};
        load(payload);
        noteThread(s, payload.thread);
        emit([&](std::ostream& line) { formatSubmitTag(payload, line); });
        break;
    }
    case RecordKind::QueueFrame: {
        QueueFramePayload payload{};
        load(payload);
        emit([&](std::ostream& line) { formatQueueFrame(payload, line); });
        break;
    }
    case RecordKind::PacingPastTiming: {
        PacingPastTimingPayload payload{};
        load(payload);
        emit([&](std::ostream& line) { formatPacingPastTiming(payload, line); });
        break;
    }
    }
}

// Drains everything queued now. The caller holds writeMutex.
void drainLocked(Recorder& s) {
    const auto cycleStart = Clock::now();
    const auto cycleStartNs = epochNanoseconds(cycleStart);
    s.recorder.drain([&](const trace_recorder::TraceRecord& record) {
        handleRecord(s, record, cycleStartNs);
    });
    const auto counters = s.recorder.takeCounters();
    s.second.enqueued += counters.enqueued;
    s.second.dropped += counters.dropped;
    s.second.slotDrops += counters.slotDrops;
    s.second.enqueueSumNs += counters.enqueueSumNs;
    s.second.enqueueMaxNs = std::max(s.second.enqueueMaxNs, counters.enqueueMaxNs);
    s.sink.reportGaps(counters.dropped + s.sink.takeFailedWrites());
}

void writeCycle(Recorder& s) {
    const std::lock_guard lock(s.writeMutex);
    const auto started = Clock::now();
    drainLocked(s);
    s.sink.flush();
    const auto finished = Clock::now();
    s.second.cycleMaxNs = std::max(s.second.cycleMaxNs, nanosecondsOf(finished - started));
    if (finished - s.second.start >= std::chrono::seconds(1))
        writeRecorderSecond(s, finished);
}

void writerMain(Recorder& s) {
    {
        // The writer runs only when the CPU would otherwise idle. A failed request leaves the
        // writer at its inherited scheduling class, as before.
        sched_param parameter{};
        static_cast<void>(sched_setscheduler(0, SCHED_IDLE, &parameter));
        const std::lock_guard lock(s.writeMutex);
        s.second.start = Clock::now();
    }
    for (;;) {
        std::this_thread::sleep_for(writerPeriod);
        writeCycle(s);
    }
}

// Process exit: one best-effort drain. A writer cycle in progress finishes first.
void drainAtExit() {
    Recorder* s = state();
    if (!s) return;
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (s->writeMutex.try_lock()) {
            drainLocked(*s);
            s->sink.flush();
            s->writeMutex.unlock();
            return;
        }
        std::this_thread::sleep_for(writerPeriod);
    }
}

}

// ---- Public surface: the producers and the test hook. ----

// Created on first use when the caller skipped initialize(), as the committed sink was lazy. Producers
// only run with the pacing profile on, so a writer thread starts only for a profiled process.
TraceRecorder* activeRecorder() noexcept {
    auto* s = state();
    if (!s) {
        initialize();
        s = state();
    }
    return s ? &s->recorder : nullptr;
}

Pools* activePools() noexcept {
    auto* s = state();
    if (!s) {
        initialize();
        s = state();
    }
    return s ? &s->pools : nullptr;
}

void initialize() noexcept {
    try {
        const std::lock_guard lock(initializeMutex);
        if (state()) return;
        auto* s = new Recorder;
        activeState.store(s, std::memory_order_release);
        // The writer runs inside this DSO. Pin it first; if the pin fails, no writer starts and
        // records wait for flushRecords() or drainAtExit.
        if (pinLayerLibrary()) std::thread([s] { writerMain(*s); }).detach();
        std::atexit(drainAtExit);
    } catch (...) {}
}

void flushRecords() noexcept {
    Recorder* s = state();
    if (!s) return;
    try {
        const std::lock_guard lock(s->writeMutex);
        drainLocked(*s);
        s->sink.flush();
    } catch (...) {}
}

void writeText(std::string text) noexcept {
    try {
        if (!state()) initialize();
        Recorder* s = state();
        if (!s) return;
        const size_t bytes = text.size();
        auto* heap = new std::string(std::move(text));
        s->pendingText.fetch_add(bytes, std::memory_order_relaxed);
        if (!s->recorder.record(RecordKind::Text, TextPayload{heap, bytes})) {
            s->pendingText.fetch_sub(bytes, std::memory_order_relaxed);
            delete heap;
            return;
        }
        // Rare text that outruns the writer is written now, so memory stays bounded.
        if (s->pendingText.load(std::memory_order_relaxed) > textBudgetBytes) flushRecords();
    } catch (...) {}
}

void registerWorkerThread(const uint32_t thread) noexcept {
    Recorder* s = state();
    if (!s || !thread) return;
    for (auto& slot : s->workers) {
        uint32_t expected = 0;
        if (slot.load(std::memory_order_acquire) == thread) return;
        if (slot.compare_exchange_strong(expected, thread, std::memory_order_acq_rel)) return;
    }
}

void noteDisplayProbeCost(const uint64_t nanoseconds) noexcept {
    Recorder* s = state();
    if (!s) return;
    s->probeSumNs.fetch_add(nanoseconds, std::memory_order_relaxed);
    auto maximum = s->probeMaxNs.load(std::memory_order_relaxed);
    while (nanoseconds > maximum &&
            !s->probeMaxNs.compare_exchange_weak(maximum, nanoseconds, std::memory_order_relaxed)) {
    }
}

void formatPacingOutput(const PacingOutputPayload& payload, std::ostream& line) {
    appendPacingOutput(line, payload.context, payload.swapchain, payload.sample);
}

void formatPacingPastTiming(const PacingPastTimingPayload& payload, std::ostream& line) {
    const auto& entry = payload.entry;
    line << "framegen-vulkan: present diagnostics: operation=pacing-profile-past-timing version=1 pid=" << getpid()
         << " swapchain=" << payload.swapchain
         << " lower_present_id=" << entry.lowerPresentId
         << " app_present_id=" << entry.appPresentId
         << " dropped=" << entry.dropped
         << " target_ns=" << entry.targetTime
         << " time_domain=" << entry.timeDomain
         << " time_domain_id=" << entry.timeDomainId
         << " report_complete=" << entry.reportComplete
         << " stages=";
    for (uint32_t i = 0; i < entry.stageCount; ++i) {
        if (i) line << ',';
        line << entry.stages[i].stage << ':' << entry.stages[i].time;
    }
    line << '\n';
}

void formatPacingWindow(const PacingWindowPayload& payload, const PacingProfile::Window& window,
        std::ostream& line) {
    formatPacingWindowBody(payload, window, line);
}

void formatApplicationWindow(const ApplicationWindowPayload& payload,
        const ApplicationFrameTiming::Window& window, std::ostream& line) {
    formatApplicationWindowText(payload, window, line);
}

void formatPresentWaitWindow(const PresentWaitWindowPayload& payload,
        const ApplicationPresentWait::Window& window, std::ostream& line) {
    formatPresentWaitWindowText(payload, window, line);
}

}

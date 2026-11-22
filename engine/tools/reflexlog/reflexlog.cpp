// framegen-vulkan reflexlog: testing-only pass-through implicit Vulkan layer.
//
// It records VK_NV_low_latency2 sleep, marker, mode and timing calls and
// vkQueuePresentKHR calls, so a capture can be taken with framegen-vulkan
// disabled. It never changes arguments, pNext chains, enabled extensions or
// results. Records are buffered in memory and written by a background thread
// into $REFLEXLOG_DIR (default $HOME/fgvk-logs). Not packaged.

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#define REFLEXLOG_EXPORT __attribute__((visibility("default")))

namespace {

constexpr char kLinePrefix[] = "framegen-vulkan: reflexlog: ";
constexpr uint64_t kMaxLogBytes = uint64_t{1} << 30;  // stop writing past 1 GiB
constexpr size_t kMaxPendingBytes = size_t{64} << 20;  // in-memory backlog cap
constexpr auto kFlushPeriod = std::chrono::milliseconds(200);
constexpr uint64_t kTimingQueryEvery = 16;
constexpr uint32_t kMaxTimingReports = 64;
constexpr int kMaxChainDepth = 64;

// Set once logging has stopped for good (I/O failure, size cap, allocation failure).
std::atomic<bool> g_disabled{false};

void Disable() noexcept { g_disabled.store(true, std::memory_order_relaxed); }

uint64_t NowNs() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

long Pid() noexcept { return static_cast<long>(getpid()); }
long Tid() noexcept { return static_cast<long>(syscall(SYS_gettid)); }
unsigned long long Ull(uint64_t value) noexcept { return value; }

// printf-style formatting; returns an empty string if formatting fails.
std::string Format(const char* fmt, ...) noexcept __attribute__((format(printf, 1, 2)));
std::string Format(const char* fmt, ...) noexcept {
  char buf[4352];
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (n < 0) return {};
  const size_t len = static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n) : sizeof(buf) - 1;
  try {
    return std::string(buf, len);
  } catch (...) {
    return {};
  }
}

std::string SessionHeader() noexcept {
  char exe[4096] = "unknown";
  const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n > 0) exe[n] = '\0';
  return Format("operation=reflexlog-session version=1 pid=%ld exe=%s now_ns=%llu", Pid(), exe,
                Ull(NowNs()));
}

// Buffers complete lines in memory and writes them from a background thread.
// Lock order: life_mu_ before mu_ before io_mu_. No file I/O happens under mu_.
class Logger {
 public:
  // Called from the game's threads. Never blocks on file I/O.
  void Append(const std::string& body) noexcept {
    if (body.empty() || g_disabled.load(std::memory_order_relaxed)) return;
    try {
      std::lock_guard<std::mutex> lock(mu_);
      if (shutting_down_ || g_disabled.load(std::memory_order_relaxed)) return;
      if (!session_written_) {
        session_written_ = true;
        AppendLineLocked(SessionHeader());
      }
      AppendLineLocked(body);
      StartWriterLocked();
    } catch (...) {
      Disable();
    }
  }

  // Final flush and writer stop: vkDestroyInstance of the last instance.
  void StopWriter() noexcept {
    try {
      std::lock_guard<std::mutex> life(life_mu_);
      bool join = false;
      pthread_t thread{};
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (writer_running_) {
          stop_writer_ = true;
          thread = writer_;
          join = true;
          cv_.notify_all();
        }
      }
      if (join) {
        pthread_join(thread, nullptr);
        std::lock_guard<std::mutex> lock(mu_);
        writer_running_ = false;
        stop_writer_ = false;
      }
      std::string batch;
      {
        std::lock_guard<std::mutex> lock(mu_);
        batch.swap(pending_);
      }
      WriteBatch(batch);
    } catch (...) {
      Disable();
    }
  }

  // atexit: final flush, then stop accepting records and close the file.
  void Shutdown() noexcept {
    try {
      std::lock_guard<std::mutex> lock(mu_);
      shutting_down_ = true;
    } catch (...) {
      Disable();
    }
    StopWriter();
    try {
      std::lock_guard<std::mutex> io(io_mu_);
      if (fd_ >= 0) close(fd_);
      fd_ = -1;
    } catch (...) {
      Disable();
    }
  }

  void ForkPrepare() noexcept {
    life_mu_.lock();
    mu_.lock();
    io_mu_.lock();
  }

  void ForkParent() noexcept {
    io_mu_.unlock();
    mu_.unlock();
    life_mu_.unlock();
  }

  // Only the forking thread survives in the child. Its writer thread is gone,
  // so reset to a clean state; the child opens its own file on its first record.
  void ForkChild() noexcept {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    bytes_written_ = 0;
    pending_.clear();
    writer_running_ = false;
    stop_writer_ = false;
    shutting_down_ = false;
    session_written_ = false;
    g_disabled.store(false, std::memory_order_relaxed);
    io_mu_.unlock();
    mu_.unlock();
    life_mu_.unlock();
  }

 private:
  void AppendLineLocked(const std::string& body) {
    if (body.empty()) return;
    if (pending_.size() + (sizeof(kLinePrefix) - 1) + body.size() + 1 > kMaxPendingBytes) return;
    pending_.append(kLinePrefix, sizeof(kLinePrefix) - 1);
    pending_.append(body);
    pending_.push_back('\n');
  }

  void StartWriterLocked() {
    if (writer_running_) return;
    stop_writer_ = false;
    if (pthread_create(&writer_, nullptr, &Logger::WriterMain, this) != 0) {
      Disable();
      return;
    }
    writer_running_ = true;
  }

  static void* WriterMain(void* self) {
    static_cast<Logger*>(self)->WriterLoop();
    return nullptr;
  }

  void WriterLoop() noexcept {
    try {
      std::unique_lock<std::mutex> lock(mu_);
      while (!stop_writer_) {
        cv_.wait_for(lock, kFlushPeriod, [this] { return stop_writer_; });
        std::string batch;
        batch.swap(pending_);
        lock.unlock();
        WriteBatch(batch);
        lock.lock();
      }
    } catch (...) {
      Disable();
    }
  }

  void WriteBatch(const std::string& batch) noexcept {
    if (batch.empty()) return;
    try {
      std::lock_guard<std::mutex> io(io_mu_);
      if (g_disabled.load(std::memory_order_relaxed)) return;
      if (fd_ < 0 && !OpenLogFile()) {
        Disable();
        return;
      }
      size_t len = batch.size();
      bool capped = false;
      const uint64_t room = kMaxLogBytes - bytes_written_;
      if (len > room) {
        // Write whole lines that fit under the cap, then stop.
        capped = true;
        const size_t cut = room == 0 ? std::string::npos : batch.rfind('\n', static_cast<size_t>(room) - 1);
        len = cut == std::string::npos ? 0 : cut + 1;
      }
      if (!WriteAll(batch.data(), len)) {
        Disable();
        return;
      }
      bytes_written_ += len;
      if (capped) Disable();
    } catch (...) {
      Disable();
    }
  }

  bool WriteAll(const char* data, size_t len) noexcept {
    while (len > 0) {
      const ssize_t written = write(fd_, data, len);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) return false;
      data += written;
      len -= static_cast<size_t>(written);
    }
    return true;
  }

  // Requires io_mu_. Creates the file on the first flush that has data.
  bool OpenLogFile() {
    std::string dir;
    const char* override_dir = std::getenv("REFLEXLOG_DIR");
    if (override_dir != nullptr && *override_dir != '\0') {
      dir = override_dir;
    } else {
      const char* home = std::getenv("HOME");
      if (home == nullptr || *home == '\0') return false;
      dir = std::string(home) + "/fgvk-logs";
    }
    mkdir(dir.c_str(), 0755);  // EEXIST is expected; a missing parent makes open() fail
    const std::string path = Format("%s/reflexlog-%lu-%ld-%llu.log", dir.c_str(),
                                    static_cast<unsigned long>(getuid()), Pid(), Ull(NowNs()));
    if (path.empty()) return false;
    fd_ = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0600);
    return fd_ >= 0;
  }

  std::mutex mu_;  // pending_, flags and writer state
  std::condition_variable cv_;
  std::string pending_;
  bool session_written_ = false;
  bool writer_running_ = false;
  bool stop_writer_ = false;
  bool shutting_down_ = false;
  pthread_t writer_{};

  std::mutex life_mu_;  // serialises writer stop with fork handling
  std::mutex io_mu_;    // fd_ and bytes_written_
  int fd_ = -1;
  uint64_t bytes_written_ = 0;
};

// Leaked on purpose: exit-time and fork-time handlers must stay valid.
Logger* g_logger = nullptr;

void ForkPrepare() noexcept { g_logger->ForkPrepare(); }
void ForkParent() noexcept { g_logger->ForkParent(); }
void ForkChild() noexcept { g_logger->ForkChild(); }
void AtExitShutdown() noexcept { g_logger->Shutdown(); }

[[maybe_unused]] const bool g_logger_ready = [] {
  g_logger = new Logger();
  pthread_atfork(ForkPrepare, ForkParent, ForkChild);
  std::atexit(AtExitShutdown);
  return true;
}();

void Emit(const std::string& body) noexcept { g_logger->Append(body); }

void EmitSleep(uint64_t enter_ns, uint64_t exit_ns, VkResult result) noexcept {
  Emit(Format("operation=pacing-profile-reflex-sleep version=1 pid=%ld tid=%ld enter_ns=%llu exit_ns=%llu result=%d",
              Pid(), Tid(), Ull(enter_ns), Ull(exit_ns), static_cast<int>(result)));
}

void EmitMode(const VkLatencySleepModeInfoNV* info, VkResult result) noexcept {
  const int low_latency = info != nullptr && info->lowLatencyMode != VK_FALSE ? 1 : 0;
  const int boost = info != nullptr && info->lowLatencyBoost != VK_FALSE ? 1 : 0;
  const unsigned min_interval_us = info != nullptr ? info->minimumIntervalUs : 0u;
  Emit(Format("operation=pacing-profile-reflex-mode version=2 pid=%ld low_latency=%d boost=%d min_interval_us=%u result=%d",
              Pid(), low_latency, boost, min_interval_us, static_cast<int>(result)));
}

void EmitMarker(uint64_t present_id, int marker, uint64_t ns) noexcept {
  Emit(Format("operation=pacing-profile-reflex-marker version=1 pid=%ld tid=%ld present_id=%llu marker=%d ns=%llu origin=app",
              Pid(), Tid(), Ull(present_id), marker, Ull(ns)));
}

void EmitPresent(uint64_t enter_ns, uint64_t exit_ns, VkResult result, uint64_t present_id) noexcept {
  Emit(Format("operation=reflexlog-present version=1 pid=%ld tid=%ld enter_ns=%llu exit_ns=%llu result=%d present_id=%llu",
              Pid(), Tid(), Ull(enter_ns), Ull(exit_ns), static_cast<int>(result), Ull(present_id)));
}

void EmitTiming(uint64_t query_ns, uint64_t now_ns, const VkLatencyTimingsFrameReportNV& report) noexcept {
  Emit(Format("operation=pacing-profile-reflex-timing version=1 pid=%ld tid=%ld query_ns=%llu now_ns=%llu "
              "present_id=%llu input_us=%llu sim_start_us=%llu sim_end_us=%llu submit_start_us=%llu "
              "submit_end_us=%llu present_start_us=%llu present_end_us=%llu driver_start_us=%llu "
              "driver_end_us=%llu os_queue_start_us=%llu os_queue_end_us=%llu gpu_start_us=%llu "
              "gpu_end_us=%llu",
              Pid(), Tid(), Ull(query_ns), Ull(now_ns), Ull(report.presentID),
              Ull(report.inputSampleTimeUs), Ull(report.simStartTimeUs), Ull(report.simEndTimeUs),
              Ull(report.renderSubmitStartTimeUs), Ull(report.renderSubmitEndTimeUs),
              Ull(report.presentStartTimeUs), Ull(report.presentEndTimeUs), Ull(report.driverStartTimeUs),
              Ull(report.driverEndTimeUs), Ull(report.osRenderQueueStartTimeUs),
              Ull(report.osRenderQueueEndTimeUs), Ull(report.gpuRenderStartTimeUs),
              Ull(report.gpuRenderEndTimeUs)));
}

// Dispatch key: the first word of every loader-wrapped dispatchable handle.
template <typename Handle>
uintptr_t DispatchKey(Handle handle) noexcept {
  return reinterpret_cast<uintptr_t>(*reinterpret_cast<void* const*>(handle));
}

template <typename Handle>
uint64_t HandleValue(Handle handle) noexcept {
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
}

// The next layer's entry points, captured at creation time.
struct InstanceEntry {
  PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
  PFN_vkDestroyInstance destroy = nullptr;
  PFN_vkCreateDevice create_device = nullptr;
};

struct DeviceFns {
  PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
  PFN_vkDestroyDevice destroy = nullptr;
  PFN_vkLatencySleepNV latency_sleep = nullptr;
  PFN_vkSetLatencySleepModeNV set_sleep_mode = nullptr;
  PFN_vkSetLatencyMarkerNV set_marker = nullptr;
  PFN_vkGetLatencyTimingsNV get_timings = nullptr;
  PFN_vkQueuePresentKHR queue_present = nullptr;
};

struct SwapchainState {
  uint64_t sleeps = 0;
  uint64_t last_present_id = 0;
};

struct DeviceEntry {
  DeviceFns fns;
  std::unordered_map<uint64_t, SwapchainState> swapchains;
};

// Every method catches its own exceptions and reports failure instead.
class Registry {
 public:
  bool AddInstance(uintptr_t key, const InstanceEntry& entry) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mu_);
      instances_[key] = entry;
      return true;
    } catch (...) {
      return false;
    }
  }

  bool FindInstance(uintptr_t key, InstanceEntry* out) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mu_);
      const auto it = instances_.find(key);
      if (it == instances_.end()) return false;
      *out = it->second;
      return true;
    } catch (...) {
      return false;
    }
  }

  // Returns how many instances remain registered.
  size_t RemoveInstance(uintptr_t key) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mu_);
      instances_.erase(key);
      return instances_.size();
    } catch (...) {
      return 1;
    }
  }

  bool AddDevice(uintptr_t key, const DeviceFns& fns) noexcept {
    try {
      DeviceEntry entry;
      entry.fns = fns;
      std::lock_guard<std::mutex> lock(mu_);
      devices_[key] = std::move(entry);
      return true;
    } catch (...) {
      return false;
    }
  }

  bool FindDevice(uintptr_t key, DeviceFns* out) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mu_);
      const auto it = devices_.find(key);
      if (it == devices_.end()) return false;
      *out = it->second.fns;
      return true;
    } catch (...) {
      return false;
    }
  }

  void RemoveDevice(uintptr_t key) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mu_);
      devices_.erase(key);
    } catch (...) {
    }
  }

  // Counts one completed sleep on a swapchain and returns the new count.
  bool CountSleep(uintptr_t key, uint64_t swapchain, uint64_t* sleeps) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mu_);
      const auto dev = devices_.find(key);
      if (dev == devices_.end()) return false;
      *sleeps = ++dev->second.swapchains[swapchain].sleeps;
      return true;
    } catch (...) {
      return false;
    }
  }

  // Selects reports with a nonzero presentID greater than the last recorded one
  // for this swapchain (array order) and advances the recorded ID.
  bool ClaimNewReports(uintptr_t key, uint64_t swapchain, const VkLatencyTimingsFrameReportNV* reports,
                       uint32_t count, uint32_t* selected, uint32_t* selected_count) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mu_);
      const auto dev = devices_.find(key);
      if (dev == devices_.end()) return false;
      SwapchainState& state = dev->second.swapchains[swapchain];
      uint32_t n = 0;
      for (uint32_t i = 0; i < count; ++i) {
        const uint64_t id = reports[i].presentID;
        if (id != 0 && id > state.last_present_id) {
          selected[n++] = i;
          state.last_present_id = id;
        }
      }
      *selected_count = n;
      return true;
    } catch (...) {
      return false;
    }
  }

 private:
  std::mutex mu_;
  std::unordered_map<uintptr_t, InstanceEntry> instances_;
  std::unordered_map<uintptr_t, DeviceEntry> devices_;
};

// Leaked on purpose, like the logger.
Registry* const g_registry = new Registry();

template <typename Handle>
bool LookupDevice(Handle handle, DeviceFns* out) noexcept {
  if (handle == VK_NULL_HANDLE) return false;
  return g_registry->FindDevice(DispatchKey(handle), out);
}

// Finds the loader's link structure in a create-info chain.
template <typename LinkInfo>
LinkInfo* FindLinkInfo(const void* chain, VkStructureType type) noexcept {
  const VkBaseInStructure* node = static_cast<const VkBaseInStructure*>(chain);
  for (int depth = 0; node != nullptr && depth < kMaxChainDepth; ++depth, node = node->pNext) {
    if (node->sType != type) continue;
    auto* info = reinterpret_cast<LinkInfo*>(const_cast<VkBaseInStructure*>(node));
    if (info->function == VK_LAYER_LINK_INFO) return info;
  }
  return nullptr;
}

// The first presentID of a chained VkPresentIdKHR, or 0.
uint64_t FirstPresentId(const VkPresentInfoKHR* info) noexcept {
  if (info == nullptr) return 0;
  const VkBaseInStructure* node = static_cast<const VkBaseInStructure*>(info->pNext);
  for (int depth = 0; node != nullptr && depth < kMaxChainDepth; ++depth, node = node->pNext) {
    if (node->sType != VK_STRUCTURE_TYPE_PRESENT_ID_KHR) continue;
    const auto* ids = reinterpret_cast<const VkPresentIdKHR*>(node);
    if (ids->swapchainCount >= 1 && ids->pPresentIds != nullptr) return ids->pPresentIds[0];
    return 0;
  }
  return 0;
}

void QueryTimings(VkDevice device, VkSwapchainKHR swapchain, uintptr_t key, PFN_vkGetLatencyTimingsNV get_timings) noexcept {
  if (get_timings == nullptr) return;

  VkGetLatencyMarkerInfoNV count_info{};
  count_info.sType = VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV;
  const uint64_t start_ns = NowNs();
  get_timings(device, swapchain, &count_info);
  const uint32_t available = count_info.timingCount;
  const uint32_t wanted = available < kMaxTimingReports ? available : kMaxTimingReports;
  VkLatencyTimingsFrameReportNV reports[kMaxTimingReports];
  for (VkLatencyTimingsFrameReportNV& report : reports) {
    report = {};
    report.sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV;
  }
  VkGetLatencyMarkerInfoNV fetch_info{};
  fetch_info.sType = VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV;
  fetch_info.timingCount = wanted;
  fetch_info.pTimings = reports;
  get_timings(device, swapchain, &fetch_info);
  const uint64_t now_ns = NowNs();

  const uint32_t returned = fetch_info.timingCount < wanted ? fetch_info.timingCount : wanted;
  uint32_t selected[kMaxTimingReports];
  uint32_t selected_count = 0;
  if (!g_registry->ClaimNewReports(key, HandleValue(swapchain), reports, returned, selected, &selected_count)) {
    return;
  }
  for (uint32_t i = 0; i < selected_count; ++i) {
    EmitTiming(now_ns - start_ns, now_ns, reports[selected[i]]);
  }
}

// ---- Wrapped device commands -------------------------------------------------

VkResult VKAPI_CALL ReflexLatencySleep(VkDevice device, VkSwapchainKHR swapchain,
                                       const VkLatencySleepInfoNV* sleep_info) {
  DeviceFns fns;
  if (!LookupDevice(device, &fns)) return VK_ERROR_INITIALIZATION_FAILED;
  if (fns.latency_sleep == nullptr) return VK_ERROR_EXTENSION_NOT_PRESENT;
  const uintptr_t key = DispatchKey(device);

  const uint64_t enter_ns = NowNs();
  const VkResult result = fns.latency_sleep(device, swapchain, sleep_info);
  const uint64_t exit_ns = NowNs();
  EmitSleep(enter_ns, exit_ns, result);

  uint64_t sleeps = 0;
  if (g_registry->CountSleep(key, HandleValue(swapchain), &sleeps) && sleeps % kTimingQueryEvery == 0) {
    QueryTimings(device, swapchain, key, fns.get_timings);
  }
  return result;
}

VkResult VKAPI_CALL ReflexSetLatencySleepMode(VkDevice device, VkSwapchainKHR swapchain,
                                              const VkLatencySleepModeInfoNV* mode_info) {
  DeviceFns fns;
  if (!LookupDevice(device, &fns)) return VK_ERROR_INITIALIZATION_FAILED;
  if (fns.set_sleep_mode == nullptr) return VK_ERROR_EXTENSION_NOT_PRESENT;
  const VkResult result = fns.set_sleep_mode(device, swapchain, mode_info);
  EmitMode(mode_info, result);
  return result;
}

void VKAPI_CALL ReflexSetLatencyMarker(VkDevice device, VkSwapchainKHR swapchain,
                                       const VkSetLatencyMarkerInfoNV* marker_info) {
  DeviceFns fns;
  if (!LookupDevice(device, &fns) || fns.set_marker == nullptr) return;
  if (marker_info != nullptr) {
    EmitMarker(marker_info->presentID, static_cast<int>(marker_info->marker), NowNs());
  }
  fns.set_marker(device, swapchain, marker_info);
}

VkResult VKAPI_CALL ReflexQueuePresent(VkQueue queue, const VkPresentInfoKHR* present_info) {
  // A queue shares its device's dispatch key.
  DeviceFns fns;
  if (!LookupDevice(queue, &fns) || fns.queue_present == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  const uint64_t present_id = FirstPresentId(present_info);
  const uint64_t enter_ns = NowNs();
  const VkResult result = fns.queue_present(queue, present_info);
  const uint64_t exit_ns = NowNs();
  EmitPresent(enter_ns, exit_ns, result, present_id);
  return result;
}

// ---- Instance and device lifetime --------------------------------------------

VkResult VKAPI_CALL ReflexCreateInstance(const VkInstanceCreateInfo* create_info,
                                         const VkAllocationCallbacks* allocator, VkInstance* instance) {
  auto* link = FindLinkInfo<VkLayerInstanceCreateInfo>(create_info != nullptr ? create_info->pNext : nullptr,
                                                       VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
  if (link == nullptr || link->u.pLayerInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  VkLayerInstanceLink* next_link = link->u.pLayerInfo;
  const PFN_vkGetInstanceProcAddr next_gipa = next_link->pfnNextGetInstanceProcAddr;
  if (next_gipa == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  const auto next_create = reinterpret_cast<PFN_vkCreateInstance>(next_gipa(nullptr, "vkCreateInstance"));
  if (next_create == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

  link->u.pLayerInfo = next_link->pNext;  // advance before calling down
  const VkResult result = next_create(create_info, allocator, instance);
  if (result != VK_SUCCESS || instance == nullptr || *instance == VK_NULL_HANDLE) return result;

  InstanceEntry entry;
  entry.get_instance_proc_addr = next_gipa;
  entry.destroy = reinterpret_cast<PFN_vkDestroyInstance>(next_gipa(*instance, "vkDestroyInstance"));
  entry.create_device = reinterpret_cast<PFN_vkCreateDevice>(next_gipa(*instance, "vkCreateDevice"));
  g_registry->AddInstance(DispatchKey(*instance), entry);
  return result;
}

void VKAPI_CALL ReflexDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
  if (instance == VK_NULL_HANDLE) return;
  const uintptr_t key = DispatchKey(instance);
  InstanceEntry entry;
  if (!g_registry->FindInstance(key, &entry)) return;
  if (entry.destroy != nullptr) entry.destroy(instance, allocator);
  if (g_registry->RemoveInstance(key) == 0) g_logger->StopWriter();
}

VkResult VKAPI_CALL ReflexCreateDevice(VkPhysicalDevice physical_device, const VkDeviceCreateInfo* create_info,
                                       const VkAllocationCallbacks* allocator, VkDevice* device) {
  auto* link = FindLinkInfo<VkLayerDeviceCreateInfo>(create_info != nullptr ? create_info->pNext : nullptr,
                                                     VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
  if (link == nullptr || link->u.pLayerInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  if (physical_device == VK_NULL_HANDLE) return VK_ERROR_INITIALIZATION_FAILED;
  // A physical device shares its instance's dispatch key.
  InstanceEntry instance_entry;
  if (!g_registry->FindInstance(DispatchKey(physical_device), &instance_entry) ||
      instance_entry.create_device == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkLayerDeviceLink* next_link = link->u.pLayerInfo;
  const PFN_vkGetDeviceProcAddr next_gdpa = next_link->pfnNextGetDeviceProcAddr;
  if (next_gdpa == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

  link->u.pLayerInfo = next_link->pNext;  // advance before calling down
  const VkResult result = instance_entry.create_device(physical_device, create_info, allocator, device);
  if (result != VK_SUCCESS || device == nullptr || *device == VK_NULL_HANDLE) return result;

  DeviceFns fns;
  fns.get_device_proc_addr = next_gdpa;
  fns.destroy = reinterpret_cast<PFN_vkDestroyDevice>(next_gdpa(*device, "vkDestroyDevice"));
  fns.latency_sleep = reinterpret_cast<PFN_vkLatencySleepNV>(next_gdpa(*device, "vkLatencySleepNV"));
  fns.set_sleep_mode =
      reinterpret_cast<PFN_vkSetLatencySleepModeNV>(next_gdpa(*device, "vkSetLatencySleepModeNV"));
  fns.set_marker = reinterpret_cast<PFN_vkSetLatencyMarkerNV>(next_gdpa(*device, "vkSetLatencyMarkerNV"));
  fns.get_timings = reinterpret_cast<PFN_vkGetLatencyTimingsNV>(next_gdpa(*device, "vkGetLatencyTimingsNV"));
  fns.queue_present = reinterpret_cast<PFN_vkQueuePresentKHR>(next_gdpa(*device, "vkQueuePresentKHR"));
  g_registry->AddDevice(DispatchKey(*device), fns);
  return result;
}

void VKAPI_CALL ReflexDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
  if (device == VK_NULL_HANDLE) return;
  const uintptr_t key = DispatchKey(device);
  DeviceFns fns;
  if (!g_registry->FindDevice(key, &fns)) return;
  if (fns.destroy != nullptr) fns.destroy(device, allocator);
  g_registry->RemoveDevice(key);
}

// ---- Dispatch lookup -----------------------------------------------------------

// Layer-owned commands that GetInstanceProcAddr returns.
PFN_vkVoidFunction InstanceLayerCommand(const char* name) noexcept {
  if (strcmp(name, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
  if (strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
  if (strcmp(name, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(ReflexCreateInstance);
  if (strcmp(name, "vkDestroyInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(ReflexDestroyInstance);
  if (strcmp(name, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(ReflexCreateDevice);
  if (strcmp(name, "vkDestroyDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(ReflexDestroyDevice);
  return nullptr;
}

// Device commands this layer wraps. vkGetLatencyTimingsNV is deliberately absent.
PFN_vkVoidFunction WrappedDeviceCommand(const char* name) noexcept {
  if (strcmp(name, "vkLatencySleepNV") == 0) return reinterpret_cast<PFN_vkVoidFunction>(ReflexLatencySleep);
  if (strcmp(name, "vkSetLatencyMarkerNV") == 0) return reinterpret_cast<PFN_vkVoidFunction>(ReflexSetLatencyMarker);
  if (strcmp(name, "vkSetLatencySleepModeNV") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(ReflexSetLatencySleepMode);
  }
  if (strcmp(name, "vkQueuePresentKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(ReflexQueuePresent);
  return nullptr;
}

}  // namespace

extern "C" {

REFLEXLOG_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface* version_struct) {
  if (version_struct == nullptr || version_struct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT ||
      version_struct->loaderLayerInterfaceVersion < 2) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (version_struct->loaderLayerInterfaceVersion > 2) version_struct->loaderLayerInterfaceVersion = 2;
  version_struct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
  version_struct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
  version_struct->pfnGetPhysicalDeviceProcAddr = nullptr;
  return VK_SUCCESS;
}

REFLEXLOG_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                                                const char* name) {
  if (name == nullptr) return nullptr;
  if (PFN_vkVoidFunction own = InstanceLayerCommand(name)) return own;
  if (instance == VK_NULL_HANDLE) return nullptr;
  InstanceEntry entry;
  if (!g_registry->FindInstance(DispatchKey(instance), &entry) || entry.get_instance_proc_addr == nullptr) {
    return nullptr;
  }
  const PFN_vkVoidFunction next = entry.get_instance_proc_addr(instance, name);
  if (next == nullptr) return nullptr;
  if (PFN_vkVoidFunction wrapper = WrappedDeviceCommand(name)) return wrapper;
  return next;
}

REFLEXLOG_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name) {
  if (name == nullptr) return nullptr;
  if (strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
  if (strcmp(name, "vkDestroyDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(ReflexDestroyDevice);
  DeviceFns fns;
  if (!LookupDevice(device, &fns) || fns.get_device_proc_addr == nullptr) return nullptr;
  const PFN_vkVoidFunction next = fns.get_device_proc_addr(device, name);
  if (next == nullptr) return nullptr;
  if (PFN_vkVoidFunction wrapper = WrappedDeviceCommand(name)) return wrapper;
  return next;
}

}  // extern "C"

// Synthetic input-latency probe for unattended fgvk comparisons.
//
// Each frame samples a synthetic input schedule after the frame limiter and the
// optional Reflex sleep, renders a moving scene, and records when the real present
// completes through VK_KHR_present_wait. CSV and JSON output is buffered in memory
// and written only after the frame loop has finished. See
// engine/scripts/compare-latency.py for the orchestrator and the analysis.

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int kExitCompleted = 0;
constexpr int kExitFailure = 1;
constexpr int kExitMissingRequirement = 2;
constexpr int kExitPresentModeUnsupported = 3;
constexpr int kExitInterrupted = 4;

constexpr uint64_t kNsPerSecond = 1000000000ull;
constexpr uint64_t kNsPerMs = 1000000ull;
constexpr double kMinEventGapMs = 20.0;
constexpr uint64_t kFenceTimeoutNs = kNsPerSecond;
constexpr uint64_t kLatencyWaitTimeoutNs = kNsPerSecond;
constexpr uint64_t kCompletionTimeoutNs = kNsPerSecond;
constexpr uint32_t kFramesInFlight = 2;
constexpr uint64_t kMaxFpsCap = 1000000;
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr char kWindowTitle[] = "fgvk latency probe";
constexpr char kVertexShaderName[] = "probe.vert.spv";
constexpr char kFragmentShaderName[] = "probe.frag.spv";

// Push constants consumed by probe.frag. The layout must match the GLSL block.
struct PushConstants {
    float time_s;
    uint32_t event_count;
    uint32_t load;
};
static_assert(sizeof(PushConstants) == 12, "push constant block must match probe.frag");

class ProbeError : public std::runtime_error {
public:
    ProbeError(int exit_code, const std::string& message)
        : std::runtime_error(message), exit_code_(exit_code) {}

    int exit_code() const { return exit_code_; }

private:
    int exit_code_;
};

struct Options {
    std::filesystem::path output_dir;
    double duration_s = 35.0;
    uint32_t fps_cap = 60;
    std::string present_mode = "fifo";
    bool reflex = false;
    uint32_t gpu_load = 0;
    double event_mean_ms = 150.0;
    uint64_t seed = 1;
};

struct FrameRecord {
    uint64_t present_id = 0;
    uint64_t sim_ns = 0;
    uint32_t event_count = 0;
    uint64_t present_call_ns = 0;
    uint64_t present_return_ns = 0;
    VkResult present_result = VK_SUCCESS;
    uint64_t complete_ns = 0;
    VkResult wait_result = VK_SUCCESS;
    bool wait_skipped = true;
};

struct WindowRecord {
    uint64_t ns = 0;
    const char* name = "";
};

struct DeviceInfo {
    std::string name;
    std::string driver_name;
    std::string driver_info;
    uint32_t api_version = 0;
    uint32_t driver_version = 0;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
};

struct ProbeReport {
    Options options;
    uint64_t start_ns = 0;
    std::string exit_reason = "not_started";
    int exit_code = kExitFailure;
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;
    DeviceInfo device;
    uint32_t image_count = 0;
    uint32_t swapchain_format = 0;
    uint32_t swapchain_color_space = 0;
    uint32_t swapchain_width = 0;
    uint32_t swapchain_height = 0;
    double display_refresh_hz = 0.0;
    int display_width = 0;
    int display_height = 0;
    bool reflex_extension = false;
    bool reflex_enabled = false;
    uint32_t recreations = 0;
    std::vector<uint64_t> events;
    std::vector<FrameRecord> frames;
    std::vector<WindowRecord> window_events;
};

struct PresentTicket {
    uint64_t present_id = 0;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    bool waitable = false;
    bool stop = false;
};

struct CompletionRecord {
    uint64_t present_id = 0;
    uint64_t complete_ns = 0;
    VkResult result = VK_SUCCESS;
};

struct SwapchainResources {
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkSemaphore> present_semaphores;
    uint64_t last_present_id = 0;
};

uint64_t monotonic_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * kNsPerSecond + static_cast<uint64_t>(ts.tv_nsec);
}

void sleep_until_monotonic(uint64_t deadline_ns) {
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(deadline_ns / kNsPerSecond);
    ts.tv_nsec = static_cast<long>(deadline_ns % kNsPerSecond);
    int rc = 0;
    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
    } while (rc == EINTR);
}

void log_error(const std::string& message) {
    std::fprintf(stderr, "framegen-vulkan: latency probe: %s\n", message.c_str());
}

std::string vk_result_name(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        default: return "VkResult(" + std::to_string(static_cast<int>(result)) + ")";
    }
}

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw ProbeError(kExitFailure, std::string(what) + " failed: " + vk_result_name(result));
    }
}

// Two-call enumeration helper. VK_INCOMPLETE on the data call is accepted, as the
// count is re-read from the driver's answer.
template <typename T, typename Fn>
std::vector<T> query_list(const char* what, Fn&& call) {
    uint32_t count = 0;
    check(call(&count, nullptr), what);
    std::vector<T> items(count);
    if (count == 0) {
        return items;
    }
    const VkResult result = call(&count, items.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        throw ProbeError(kExitFailure, std::string(what) + " failed: " + vk_result_name(result));
    }
    items.resize(count);
    return items;
}

template <typename T>
T make_info(VkStructureType type) {
    T value{};
    value.sType = type;
    return value;
}

bool has_extension(const std::vector<VkExtensionProperties>& available, const char* name) {
    return std::any_of(available.begin(), available.end(), [name](const VkExtensionProperties& p) {
        return std::strcmp(p.extensionName, name) == 0;
    });
}

bool parse_u64(std::string_view text, uint64_t& value) {
    if (text.empty()) {
        return false;
    }
    uint64_t parsed = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    value = parsed;
    return true;
}

bool parse_positive_double(std::string_view text, double& value) {
    const std::string copy(text);
    char* end = nullptr;
    const double parsed = std::strtod(copy.c_str(), &end);
    if (copy.empty() || end == copy.c_str() || *end != '\0' || !std::isfinite(parsed) || parsed <= 0.0) {
        return false;
    }
    value = parsed;
    return true;
}

std::optional<std::string> parse_options(int argc, char** argv, Options& options, bool& show_help) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            show_help = true;
            return std::nullopt;
        }
        if (i + 1 >= argc) {
            return "missing value for " + std::string(arg);
        }
        const std::string_view value = argv[++i];
        if (arg == "--output-dir") {
            options.output_dir = std::string(value);
        } else if (arg == "--duration") {
            if (!parse_positive_double(value, options.duration_s)) {
                return "--duration must be a positive number of seconds";
            }
        } else if (arg == "--fps-cap") {
            uint64_t cap = 0;
            if (!parse_u64(value, cap) || cap > kMaxFpsCap) {
                return "--fps-cap must be an integer from 0 to 1000000";
            }
            options.fps_cap = static_cast<uint32_t>(cap);
        } else if (arg == "--present-mode") {
            if (value != "fifo" && value != "mailbox" && value != "immediate") {
                return "--present-mode must be fifo, mailbox or immediate";
            }
            options.present_mode = std::string(value);
        } else if (arg == "--reflex") {
            if (value == "on") {
                options.reflex = true;
            } else if (value == "off") {
                options.reflex = false;
            } else {
                return "--reflex must be on or off";
            }
        } else if (arg == "--gpu-load") {
            uint64_t load = 0;
            if (!parse_u64(value, load) || load > UINT32_MAX) {
                return "--gpu-load must be a non-negative integer";
            }
            options.gpu_load = static_cast<uint32_t>(load);
        } else if (arg == "--event-mean-ms") {
            if (!parse_positive_double(value, options.event_mean_ms)) {
                return "--event-mean-ms must be a positive number";
            }
        } else if (arg == "--seed") {
            if (!parse_u64(value, options.seed)) {
                return "--seed must be a non-negative integer";
            }
        } else {
            return "unknown argument " + std::string(arg);
        }
    }
    if (options.output_dir.empty()) {
        return std::string("--output-dir is required");
    }
    return std::nullopt;
}

void print_usage(FILE* stream) {
    std::fputs(
        "Usage: fgvk-latency-probe --output-dir DIR [options]\n"
        "  --output-dir DIR      existing empty directory for the output files\n"
        "  --duration S          measured run length in seconds (default 35)\n"
        "  --fps-cap N           absolute-deadline frame cap, 0 uncapped (default 60)\n"
        "  --present-mode MODE   fifo, mailbox or immediate (default fifo)\n"
        "  --reflex on|off       NVIDIA Reflex sleep and latency markers (default off)\n"
        "  --gpu-load N          fragment-shader loop iterations (default 0)\n"
        "  --event-mean-ms M     mean synthetic input gap in milliseconds (default 150)\n"
        "  --seed S              synthetic input seed (default 1)\n"
        "Exit status: 0 completed, 1 error, 2 required extension or feature missing,\n"
        "3 present mode unsupported, 4 window closed or Escape pressed.\n",
        stream);
}

// Shifted exponential gaps: every gap is at least 20 ms and the mean gap is the
// requested mean. A mean at or below 20 ms yields fixed 20 ms gaps.
std::vector<uint64_t> generate_events(uint64_t seed, double mean_ms, uint64_t start_ns, uint64_t duration_ns) {
    std::mt19937_64 rng(seed);
    const double extra_mean_ms = mean_ms - kMinEventGapMs;
    std::exponential_distribution<double> extra(extra_mean_ms > 0.0 ? 1.0 / extra_mean_ms : 1.0);
    const uint64_t end_ns = start_ns + duration_ns;
    std::vector<uint64_t> events;
    uint64_t t = start_ns;
    while (t < end_ns) {
        const double gap_ms = kMinEventGapMs + (extra_mean_ms > 0.0 ? extra(rng) : 0.0);
        t += static_cast<uint64_t>(std::llround(gap_ms * static_cast<double>(kNsPerMs)));
        events.push_back(t);
    }
    return events;
}

const char* window_event_name(uint32_t type) {
    switch (type) {
        case SDL_EVENT_WINDOW_SHOWN: return "shown";
        case SDL_EVENT_WINDOW_HIDDEN: return "hidden";
        case SDL_EVENT_WINDOW_EXPOSED: return "exposed";
        case SDL_EVENT_WINDOW_MOVED: return "moved";
        case SDL_EVENT_WINDOW_RESIZED: return "resized";
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: return "pixel_size_changed";
        case SDL_EVENT_WINDOW_METAL_VIEW_RESIZED: return "metal_view_resized";
        case SDL_EVENT_WINDOW_MINIMIZED: return "minimized";
        case SDL_EVENT_WINDOW_MAXIMIZED: return "maximized";
        case SDL_EVENT_WINDOW_RESTORED: return "restored";
        case SDL_EVENT_WINDOW_MOUSE_ENTER: return "mouse_enter";
        case SDL_EVENT_WINDOW_MOUSE_LEAVE: return "mouse_leave";
        case SDL_EVENT_WINDOW_FOCUS_GAINED: return "focus_gained";
        case SDL_EVENT_WINDOW_FOCUS_LOST: return "focus_lost";
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: return "close_requested";
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: return "enter_fullscreen";
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: return "leave_fullscreen";
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED: return "display_changed";
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: return "display_scale_changed";
        case SDL_EVENT_WINDOW_OCCLUDED: return "occluded";
        case SDL_EVENT_WINDOW_DESTROYED: return "destroyed";
        case SDL_EVENT_WINDOW_HDR_STATE_CHANGED: return "hdr_state_changed";
        case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED: return "safe_area_changed";
        case SDL_EVENT_WINDOW_HIT_TEST: return "hit_test";
        case SDL_EVENT_WINDOW_ICCPROF_CHANGED: return "iccprof_changed";
        default: return "window_other";
    }
}

std::vector<uint32_t> load_spirv(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        throw ProbeError(kExitFailure, "missing shader " + path.string());
    }
    const std::streamoff size = static_cast<std::streamoff>(in.tellg());
    if (size <= 0 || size % 4 != 0) {
        throw ProbeError(kExitFailure, "invalid SPIR-V size in " + path.string());
    }
    std::vector<uint32_t> words(static_cast<size_t>(size) / sizeof(uint32_t));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(size));
    if (!in) {
        throw ProbeError(kExitFailure, "cannot read " + path.string());
    }
    return words;
}

std::filesystem::path executable_dir() {
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec || exe.empty()) {
        throw ProbeError(kExitFailure, "cannot resolve /proc/self/exe");
    }
    return exe.parent_path();
}

VkCompositeAlphaFlagBitsKHR pick_composite_alpha(VkCompositeAlphaFlagsKHR supported) {
    const std::array<VkCompositeAlphaFlagBitsKHR, 4> candidates = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
    };
    for (VkCompositeAlphaFlagBitsKHR candidate : candidates) {
        if ((supported & candidate) != 0) {
            return candidate;
        }
    }
    throw ProbeError(kExitFailure, "surface reports no composite alpha mode");
}

bool present_mode_from_name(std::string_view name, VkPresentModeKHR& mode) {
    if (name == "fifo") {
        mode = VK_PRESENT_MODE_FIFO_KHR;
    } else if (name == "mailbox") {
        mode = VK_PRESENT_MODE_MAILBOX_KHR;
    } else if (name == "immediate") {
        mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    } else {
        return false;
    }
    return true;
}

// Completion worker: waits for each present ID in order on a dedicated thread.
// Tickets for skipped presents only advance the completed counter.
class CompletionWorker {
public:
    CompletionWorker(VkDevice device, PFN_vkWaitForPresentKHR wait_for_present)
        : device_(device), wait_for_present_(wait_for_present) {}
    CompletionWorker(const CompletionWorker&) = delete;
    CompletionWorker& operator=(const CompletionWorker&) = delete;
    ~CompletionWorker() { stop_and_join(); }

    void start() {
        thread_ = std::thread([this] { run(); });
    }

    void push(const PresentTicket& ticket) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(ticket);
        }
        wake_.notify_one();
    }

    // Drains every queued ticket, then joins. Safe to call more than once.
    void stop_and_join() {
        if (!thread_.joinable()) {
            return;
        }
        PresentTicket stop;
        stop.stop = true;
        push(stop);
        thread_.join();
    }

    uint64_t completed_upto() const { return completed_.load(std::memory_order_acquire); }
    const std::vector<CompletionRecord>& records() const { return records_; }

private:
    void run() {
        for (;;) {
            PresentTicket ticket;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return !queue_.empty(); });
                ticket = queue_.front();
                queue_.pop_front();
            }
            if (ticket.stop) {
                return;
            }
            if (ticket.waitable) {
                const VkResult result = wait_for_present_(device_, ticket.swapchain, ticket.present_id,
                                                          kCompletionTimeoutNs);
                const uint64_t done_ns = monotonic_ns();
                CompletionRecord record;
                record.present_id = ticket.present_id;
                record.complete_ns = result == VK_SUCCESS ? done_ns : 0;
                record.result = result;
                records_.push_back(record);
            }
            completed_.store(ticket.present_id, std::memory_order_release);
        }
    }

    VkDevice device_;
    PFN_vkWaitForPresentKHR wait_for_present_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<PresentTicket> queue_;
    std::atomic<uint64_t> completed_{0};
    std::vector<CompletionRecord> records_;
};

class Renderer {
public:
    explicit Renderer(ProbeReport& report);
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer() { shutdown(); }

    void run();
    void shutdown();

private:
    void init_window();
    void create_instance();
    void create_surface();
    void select_device();
    void create_device();
    void load_device_functions();
    void create_frame_resources();
    void create_shader_objects();
    void ensure_pipeline(VkFormat format);
    SwapchainResources create_swapchain(VkSwapchainKHR old_swapchain);
    void recreate_swapchain();
    void reclaim_retired();
    void destroy_swapchain(SwapchainResources& resources);
    void configure_latency_mode();
    void set_marker(uint64_t present_id, VkLatencyMarkerNV marker);
    void pump_events(bool& quit, bool& need_recreate);
    void record_commands(VkCommandBuffer cmd, uint32_t image_index, uint32_t event_count, float time_s);
    VkShaderModule create_shader(const std::vector<uint32_t>& words);
    void merge_completions();

    ProbeReport& report_;
    VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool sdl_initialized_ = false;
    bool shut_down_ = false;
    bool reflex_active_ = false;

    SDL_Window* window_ = nullptr;
    SDL_WindowID window_id_ = 0;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;

    PFN_vkWaitForPresentKHR wait_for_present_ = nullptr;
    PFN_vkSetLatencySleepModeNV set_latency_sleep_mode_ = nullptr;
    PFN_vkLatencySleepNV latency_sleep_ = nullptr;
    PFN_vkSetLatencyMarkerNV set_latency_marker_ = nullptr;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, kFramesInFlight> command_buffers_{};
    std::array<VkFence, kFramesInFlight> fences_{};
    std::array<VkSemaphore, kFramesInFlight> acquire_semaphores_{};
    VkSemaphore latency_semaphore_ = VK_NULL_HANDLE;
    uint64_t latency_value_ = 0;

    VkShaderModule vertex_shader_ = VK_NULL_HANDLE;
    VkShaderModule fragment_shader_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkFormat pipeline_format_ = VK_FORMAT_UNDEFINED;

    SwapchainResources swapchain_;
    std::vector<SwapchainResources> retired_;
    std::unique_ptr<CompletionWorker> worker_;
};

Renderer::Renderer(ProbeReport& report) : report_(report) {
    try {
        if (!present_mode_from_name(report_.options.present_mode, present_mode_)) {
            throw ProbeError(kExitFailure, "unknown present mode " + report_.options.present_mode);
        }
        init_window();
        create_instance();
        create_surface();
        select_device();
        create_device();
        load_device_functions();
        create_frame_resources();
        create_shader_objects();
        swapchain_ = create_swapchain(VK_NULL_HANDLE);
        report_.image_count = static_cast<uint32_t>(swapchain_.images.size());
        report_.swapchain_format = static_cast<uint32_t>(swapchain_.format);
        report_.swapchain_width = swapchain_.extent.width;
        report_.swapchain_height = swapchain_.extent.height;
        ensure_pipeline(swapchain_.format);
        if (report_.options.reflex) {
            configure_latency_mode();
            reflex_active_ = true;
            report_.reflex_enabled = true;
        }
        worker_ = std::make_unique<CompletionWorker>(device_, wait_for_present_);
        worker_->start();
    } catch (...) {
        shutdown();
        throw;
    }
}

void Renderer::init_window() {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw ProbeError(kExitFailure, std::string("SDL_Init failed: ") + SDL_GetError());
    }
    sdl_initialized_ = true;
    window_ = SDL_CreateWindow(kWindowTitle, kWindowWidth, kWindowHeight, SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN);
    if (window_ == nullptr) {
        throw ProbeError(kExitFailure, std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }
    window_id_ = SDL_GetWindowID(window_);
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window_);
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display)) {
        report_.display_refresh_hz = static_cast<double>(mode->refresh_rate);
        report_.display_width = mode->w;
        report_.display_height = mode->h;
    }
}

void Renderer::create_instance() {
    uint32_t loader_version = 0;
    check(vkEnumerateInstanceVersion(&loader_version), "vkEnumerateInstanceVersion");
    if (loader_version < VK_API_VERSION_1_3) {
        throw ProbeError(kExitFailure, "Vulkan 1.3 loader is required");
    }
    uint32_t count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (sdl_extensions == nullptr) {
        throw ProbeError(kExitFailure, std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());
    }
    report_.instance_extensions.assign(sdl_extensions, sdl_extensions + count);

    VkApplicationInfo app = make_info<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    app.pApplicationName = "fgvk-latency-probe";
    app.applicationVersion = 1;
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo info = make_info<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = count;
    info.ppEnabledExtensionNames = sdl_extensions;
    check(vkCreateInstance(&info, nullptr, &instance_), "vkCreateInstance");
}

void Renderer::create_surface() {
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
        throw ProbeError(kExitFailure, std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());
    }
}

void Renderer::select_device() {
    const auto devices = query_list<VkPhysicalDevice>("vkEnumeratePhysicalDevices",
        [this](uint32_t* count, VkPhysicalDevice* data) {
            return vkEnumeratePhysicalDevices(instance_, count, data);
        });
    if (devices.empty()) {
        throw ProbeError(kExitFailure, "no Vulkan physical devices");
    }
    physical_ = devices.front();
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            physical_ = candidate;
            break;
        }
    }

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, families.data());
    bool found_queue = false;
    for (uint32_t i = 0; i < family_count; ++i) {
        VkBool32 present_supported = VK_FALSE;
        check(vkGetPhysicalDeviceSurfaceSupportKHR(physical_, i, surface_, &present_supported),
              "vkGetPhysicalDeviceSurfaceSupportKHR");
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && present_supported == VK_TRUE) {
            queue_family_ = i;
            found_queue = true;
            break;
        }
    }
    if (!found_queue) {
        throw ProbeError(kExitMissingRequirement, "missing_requirement: graphics and present queue");
    }

    const auto available = query_list<VkExtensionProperties>("vkEnumerateDeviceExtensionProperties",
        [this](uint32_t* count, VkExtensionProperties* data) {
            return vkEnumerateDeviceExtensionProperties(physical_, nullptr, count, data);
        });
    const char* required[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_PRESENT_ID_EXTENSION_NAME,
        VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
    };
    for (const char* name : required) {
        if (!has_extension(available, name)) {
            throw ProbeError(kExitMissingRequirement, std::string("missing_extension: ") + name);
        }
    }
    report_.reflex_extension = has_extension(available, VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
    if (report_.options.reflex && !report_.reflex_extension) {
        throw ProbeError(kExitMissingRequirement, std::string("missing_extension: ") + VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
    }

    VkPhysicalDeviceDriverProperties driver = make_info<VkPhysicalDeviceDriverProperties>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
    VkPhysicalDeviceProperties2 props2 = make_info<VkPhysicalDeviceProperties2>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
    props2.pNext = &driver;
    vkGetPhysicalDeviceProperties2(physical_, &props2);
    report_.device.name = props2.properties.deviceName;
    report_.device.driver_name = driver.driverName;
    report_.device.driver_info = driver.driverInfo;
    report_.device.api_version = props2.properties.apiVersion;
    report_.device.driver_version = props2.properties.driverVersion;
    report_.device.vendor_id = props2.properties.vendorID;
    report_.device.device_id = props2.properties.deviceID;
    report_.device.type = props2.properties.deviceType;
}

void Renderer::create_device() {
    VkPhysicalDeviceDynamicRenderingFeatures dynamic = make_info<VkPhysicalDeviceDynamicRenderingFeatures>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES);
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = make_info<VkPhysicalDeviceTimelineSemaphoreFeatures>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
    VkPhysicalDevicePresentIdFeaturesKHR present_id = make_info<VkPhysicalDevicePresentIdFeaturesKHR>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR);
    VkPhysicalDevicePresentWaitFeaturesKHR present_wait = make_info<VkPhysicalDevicePresentWaitFeaturesKHR>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR);
    VkPhysicalDeviceFeatures2 features = make_info<VkPhysicalDeviceFeatures2>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    features.pNext = &dynamic;
    dynamic.pNext = &timeline;
    timeline.pNext = &present_id;
    present_id.pNext = &present_wait;
    vkGetPhysicalDeviceFeatures2(physical_, &features);

    if (dynamic.dynamicRendering != VK_TRUE) {
        throw ProbeError(kExitMissingRequirement, "missing_feature: dynamicRendering");
    }
    if (timeline.timelineSemaphore != VK_TRUE) {
        throw ProbeError(kExitMissingRequirement, "missing_feature: timelineSemaphore");
    }
    if (present_id.presentId != VK_TRUE) {
        throw ProbeError(kExitMissingRequirement, "missing_feature: presentId");
    }
    if (present_wait.presentWait != VK_TRUE) {
        throw ProbeError(kExitMissingRequirement, "missing_feature: presentWait");
    }
    dynamic.dynamicRendering = VK_TRUE;
    timeline.timelineSemaphore = VK_TRUE;
    present_id.presentId = VK_TRUE;
    present_wait.presentWait = VK_TRUE;

    std::vector<const char*> extensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_PRESENT_ID_EXTENSION_NAME,
        VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
    };
    if (report_.options.reflex) {
        extensions.push_back(VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
    }
    report_.device_extensions.clear();
    for (const char* name : extensions) {
        report_.device_extensions.emplace_back(name);
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = make_info<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
    queue_info.queueFamilyIndex = queue_family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    VkDeviceCreateInfo info = make_info<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    info.pNext = &features;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue_info;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    check(vkCreateDevice(physical_, &info, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
}

void Renderer::load_device_functions() {
    wait_for_present_ = reinterpret_cast<PFN_vkWaitForPresentKHR>(
        vkGetDeviceProcAddr(device_, "vkWaitForPresentKHR"));
    if (wait_for_present_ == nullptr) {
        throw ProbeError(kExitMissingRequirement, "missing_extension: VK_KHR_present_wait entry points");
    }
    if (!report_.options.reflex) {
        return;
    }
    set_latency_sleep_mode_ = reinterpret_cast<PFN_vkSetLatencySleepModeNV>(
        vkGetDeviceProcAddr(device_, "vkSetLatencySleepModeNV"));
    latency_sleep_ = reinterpret_cast<PFN_vkLatencySleepNV>(vkGetDeviceProcAddr(device_, "vkLatencySleepNV"));
    set_latency_marker_ = reinterpret_cast<PFN_vkSetLatencyMarkerNV>(
        vkGetDeviceProcAddr(device_, "vkSetLatencyMarkerNV"));
    if (set_latency_sleep_mode_ == nullptr || latency_sleep_ == nullptr || set_latency_marker_ == nullptr) {
        throw ProbeError(kExitMissingRequirement, "missing_extension: VK_NV_low_latency2 entry points");
    }
}

void Renderer::create_frame_resources() {
    VkCommandPoolCreateInfo pool_info = make_info<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family_;
    check(vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo alloc = make_info<VkCommandBufferAllocateInfo>(
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    alloc.commandPool = command_pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = kFramesInFlight;
    check(vkAllocateCommandBuffers(device_, &alloc, command_buffers_.data()), "vkAllocateCommandBuffers");

    for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
        VkFenceCreateInfo fence_info = make_info<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(vkCreateFence(device_, &fence_info, nullptr, &fences_[slot]), "vkCreateFence");

        VkSemaphoreCreateInfo semaphore_info = make_info<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
        check(vkCreateSemaphore(device_, &semaphore_info, nullptr, &acquire_semaphores_[slot]), "vkCreateSemaphore");
    }

    VkSemaphoreTypeCreateInfo timeline = make_info<VkSemaphoreTypeCreateInfo>(
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
    timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timeline.initialValue = 0;
    VkSemaphoreCreateInfo timeline_info = make_info<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
    timeline_info.pNext = &timeline;
    check(vkCreateSemaphore(device_, &timeline_info, nullptr, &latency_semaphore_), "vkCreateSemaphore (timeline)");
}

VkShaderModule Renderer::create_shader(const std::vector<uint32_t>& words) {
    VkShaderModuleCreateInfo info = make_info<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    info.codeSize = words.size() * sizeof(uint32_t);
    info.pCode = words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device_, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}

void Renderer::create_shader_objects() {
    const std::filesystem::path dir = executable_dir();
    vertex_shader_ = create_shader(load_spirv(dir / kVertexShaderName));
    fragment_shader_ = create_shader(load_spirv(dir / kFragmentShaderName));

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.offset = 0;
    range.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layout = make_info<VkPipelineLayoutCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(device_, &layout, nullptr, &pipeline_layout_), "vkCreatePipelineLayout");
}

void Renderer::ensure_pipeline(VkFormat format) {
    if (pipeline_ != VK_NULL_HANDLE && pipeline_format_ == format) {
        return;
    }
    if (pipeline_ != VK_NULL_HANDLE) {
        check(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle");
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0] = make_info<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader_;
    stages[0].pName = "main";
    stages[1] = make_info<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_shader_;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input = make_info<VkPipelineVertexInputStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    VkPipelineInputAssemblyStateCreateInfo input_assembly = make_info<VkPipelineInputAssemblyStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state = make_info<VkPipelineViewportStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization = make_info<VkPipelineRasterizationStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = make_info<VkPipelineMultisampleStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend = make_info<VkPipelineColorBlendStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    const std::array<VkDynamicState, 2> dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = make_info<VkPipelineDynamicStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
    dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();

    VkFormat color_format = format;
    VkPipelineRenderingCreateInfo rendering = make_info<VkPipelineRenderingCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_format;

    VkGraphicsPipelineCreateInfo info = make_info<VkGraphicsPipelineCreateInfo>(
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    info.pNext = &rendering;
    info.stageCount = static_cast<uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = pipeline_layout_;
    info.renderPass = VK_NULL_HANDLE;
    info.subpass = 0;
    check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_),
          "vkCreateGraphicsPipelines");
    pipeline_format_ = format;
}

SwapchainResources Renderer::create_swapchain(VkSwapchainKHR old_swapchain) {
    VkSurfaceCapabilitiesKHR caps{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps),
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    const auto formats = query_list<VkSurfaceFormatKHR>("vkGetPhysicalDeviceSurfaceFormatsKHR",
        [this](uint32_t* count, VkSurfaceFormatKHR* data) {
            return vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, count, data);
        });
    if (formats.empty()) {
        throw ProbeError(kExitFailure, "surface reports no formats");
    }
    const auto modes = query_list<VkPresentModeKHR>("vkGetPhysicalDeviceSurfacePresentModesKHR",
        [this](uint32_t* count, VkPresentModeKHR* data) {
            return vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, count, data);
        });
    if (std::find(modes.begin(), modes.end(), present_mode_) == modes.end()) {
        throw ProbeError(kExitPresentModeUnsupported,
                         "present mode " + report_.options.present_mode + " is not supported by this surface");
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX || extent.height == UINT32_MAX) {
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window_, &width, &height);
        extent.width = std::clamp(static_cast<uint32_t>(std::max(width, 0)), caps.minImageExtent.width,
                                  caps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(std::max(height, 0)), caps.minImageExtent.height,
                                   caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        throw ProbeError(kExitFailure, "surface extent is zero");
    }

    uint32_t image_count = std::max(caps.minImageCount + 1, 3u);
    if (caps.maxImageCount != 0) {
        image_count = std::min(image_count, caps.maxImageCount);
    }

    const VkSurfaceFormatKHR format = formats.front();
    VkSwapchainLatencyCreateInfoNV latency = make_info<VkSwapchainLatencyCreateInfoNV>(
        VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV);
    latency.latencyModeEnable = VK_TRUE;

    VkSwapchainCreateInfoKHR info = make_info<VkSwapchainCreateInfoKHR>(VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
    info.pNext = report_.options.reflex ? &latency : nullptr;
    info.surface = surface_;
    info.minImageCount = image_count;
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = pick_composite_alpha(caps.supportedCompositeAlpha);
    info.presentMode = present_mode_;
    info.clipped = VK_TRUE;
    info.oldSwapchain = old_swapchain;

    SwapchainResources result;
    check(vkCreateSwapchainKHR(device_, &info, nullptr, &result.handle), "vkCreateSwapchainKHR");
    try {
        result.format = format.format;
        result.extent = extent;
        report_.swapchain_color_space = static_cast<uint32_t>(format.colorSpace);
        result.images = query_list<VkImage>("vkGetSwapchainImagesKHR",
            [this, &result](uint32_t* count, VkImage* data) {
                return vkGetSwapchainImagesKHR(device_, result.handle, count, data);
            });
        for (VkImage image : result.images) {
            VkImageViewCreateInfo view = make_info<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
            view.image = image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format.format;
            view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view.subresourceRange.baseMipLevel = 0;
            view.subresourceRange.levelCount = 1;
            view.subresourceRange.baseArrayLayer = 0;
            view.subresourceRange.layerCount = 1;
            VkImageView image_view = VK_NULL_HANDLE;
            check(vkCreateImageView(device_, &view, nullptr, &image_view), "vkCreateImageView");
            result.views.push_back(image_view);
        }
        for (size_t i = 0; i < result.images.size(); ++i) {
            VkSemaphoreCreateInfo semaphore_info = make_info<VkSemaphoreCreateInfo>(
                VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
            VkSemaphore semaphore = VK_NULL_HANDLE;
            check(vkCreateSemaphore(device_, &semaphore_info, nullptr, &semaphore), "vkCreateSemaphore (present)");
            result.present_semaphores.push_back(semaphore);
        }
    } catch (...) {
        destroy_swapchain(result);
        throw;
    }
    return result;
}

void Renderer::configure_latency_mode() {
    VkLatencySleepModeInfoNV mode = make_info<VkLatencySleepModeInfoNV>(VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV);
    mode.lowLatencyMode = VK_TRUE;
    mode.lowLatencyBoost = VK_FALSE;
    mode.minimumIntervalUs = 0;
    check(set_latency_sleep_mode_(device_, swapchain_.handle, &mode), "vkSetLatencySleepModeNV");
}

void Renderer::set_marker(uint64_t present_id, VkLatencyMarkerNV marker) {
    VkSetLatencyMarkerInfoNV info = make_info<VkSetLatencyMarkerInfoNV>(VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV);
    info.presentID = present_id;
    info.marker = marker;
    set_latency_marker_(device_, swapchain_.handle, &info);
}

void Renderer::destroy_swapchain(SwapchainResources& resources) {
    for (VkImageView view : resources.views) {
        vkDestroyImageView(device_, view, nullptr);
    }
    for (VkSemaphore semaphore : resources.present_semaphores) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    if (resources.handle != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, resources.handle, nullptr);
    }
    resources = SwapchainResources{};
}

void Renderer::recreate_swapchain() {
    SwapchainResources next = create_swapchain(swapchain_.handle);
    SwapchainResources previous = std::exchange(swapchain_, std::move(next));
    if (previous.handle != VK_NULL_HANDLE) {
        retired_.push_back(std::move(previous));
    }
    report_.image_count = static_cast<uint32_t>(swapchain_.images.size());
    report_.swapchain_format = static_cast<uint32_t>(swapchain_.format);
    report_.swapchain_width = swapchain_.extent.width;
    report_.swapchain_height = swapchain_.extent.height;
    ensure_pipeline(swapchain_.format);
    if (reflex_active_) {
        configure_latency_mode();
    }
    ++report_.recreations;
    report_.window_events.push_back(WindowRecord{monotonic_ns(), "recreate"});
}

// A retired swapchain is destroyed only after the completion worker has passed its
// last present ID, so no present wait can still reference the handle.
void Renderer::reclaim_retired() {
    if (retired_.empty()) {
        return;
    }
    const uint64_t completed = worker_->completed_upto();
    const auto ready = [completed](const SwapchainResources& r) {
        return r.last_present_id == 0 || completed >= r.last_present_id;
    };
    if (!std::any_of(retired_.begin(), retired_.end(), ready)) {
        return;
    }
    check(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle");
    for (auto it = retired_.begin(); it != retired_.end();) {
        if (ready(*it)) {
            destroy_swapchain(*it);
            it = retired_.erase(it);
        } else {
            ++it;
        }
    }
}

void Renderer::pump_events(bool& quit, bool& need_recreate) {
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        const uint32_t type = static_cast<uint32_t>(event.type);
        if (type == static_cast<uint32_t>(SDL_EVENT_QUIT)) {
            quit = true;
            report_.exit_reason = "quit";
            report_.exit_code = kExitInterrupted;
            continue;
        }
        if (type == static_cast<uint32_t>(SDL_EVENT_KEY_DOWN)) {
            if (event.key.windowID == window_id_ && event.key.key == SDLK_ESCAPE) {
                quit = true;
                report_.exit_reason = "escape";
                report_.exit_code = kExitInterrupted;
            }
            continue;
        }
        if (type < static_cast<uint32_t>(SDL_EVENT_WINDOW_FIRST) ||
            type > static_cast<uint32_t>(SDL_EVENT_WINDOW_LAST)) {
            continue;
        }
        if (event.window.windowID != window_id_) {
            continue;
        }
        report_.window_events.push_back(WindowRecord{monotonic_ns(), window_event_name(type)});
        if (type == static_cast<uint32_t>(SDL_EVENT_WINDOW_CLOSE_REQUESTED)) {
            quit = true;
            report_.exit_reason = "window_closed";
            report_.exit_code = kExitInterrupted;
        } else if (type == static_cast<uint32_t>(SDL_EVENT_WINDOW_RESIZED) ||
                   type == static_cast<uint32_t>(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)) {
            need_recreate = true;
        }
    }
}

void Renderer::record_commands(VkCommandBuffer cmd, uint32_t image_index, uint32_t event_count, float time_s) {
    VkCommandBufferBeginInfo begin = make_info<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");

    VkImageSubresourceRange color_range{};
    color_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    color_range.levelCount = 1;
    color_range.layerCount = 1;

    VkImageMemoryBarrier to_color = make_info<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    to_color.srcAccessMask = 0;
    to_color.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_color.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.image = swapchain_.images[image_index];
    to_color.subresourceRange = color_range;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_color);

    VkRenderingAttachmentInfo attachment = make_info<VkRenderingAttachmentInfo>(
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    attachment.imageView = swapchain_.views[image_index];
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering = make_info<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
    rendering.renderArea.offset = {0, 0};
    rendering.renderArea.extent = swapchain_.extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    vkCmdBeginRendering(cmd, &rendering);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    VkViewport viewport{};
    viewport.width = static_cast<float>(swapchain_.extent.width);
    viewport.height = static_cast<float>(swapchain_.extent.height);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = swapchain_.extent;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    PushConstants push{};
    push.time_s = time_s;
    push.event_count = event_count;
    push.load = report_.options.gpu_load;
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    VkImageMemoryBarrier to_present = make_info<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    to_present.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.image = swapchain_.images[image_index];
    to_present.subresourceRange = color_range;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_present);
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
}

void Renderer::run() {
    const Options& opt = report_.options;
    const uint64_t duration_ns = static_cast<uint64_t>(std::llround(opt.duration_s * static_cast<double>(kNsPerSecond)));
    const uint64_t start_ns = monotonic_ns();
    const uint64_t end_ns = start_ns + duration_ns;
    report_.start_ns = start_ns;
    report_.events = generate_events(opt.seed, opt.event_mean_ms, start_ns, duration_ns);
    const double expected_frames = opt.duration_s * (opt.fps_cap != 0 ? static_cast<double>(opt.fps_cap) : 240.0);
    report_.frames.reserve(static_cast<size_t>(expected_frames * 1.1) + 64);
    report_.window_events.reserve(256);

    const uint64_t period_ns = opt.fps_cap != 0 ? kNsPerSecond / opt.fps_cap : 0;
    uint64_t deadline_ns = start_ns;
    size_t next_event = 0;
    bool quit = false;
    bool need_recreate = false;

    for (;;) {
        pump_events(quit, need_recreate);
        if (quit) {
            break;
        }
        if (monotonic_ns() >= end_ns) {
            report_.exit_reason = "completed";
            report_.exit_code = kExitCompleted;
            break;
        }
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window_, &width, &height);
        if (width <= 0 || height <= 0) {
            SDL_Delay(10);
            continue;
        }
        if (need_recreate) {
            recreate_swapchain();
            need_recreate = false;
        }
        reclaim_retired();

        const uint64_t frame = report_.frames.size();
        const uint32_t slot = static_cast<uint32_t>(frame % kFramesInFlight);
        check(vkWaitForFences(device_, 1, &fences_[slot], VK_TRUE, kFenceTimeoutNs), "vkWaitForFences");

        if (period_ns != 0) {
            deadline_ns += period_ns;
            const uint64_t after_fence_ns = monotonic_ns();
            if (after_fence_ns > deadline_ns + 2 * period_ns) {
                deadline_ns = after_fence_ns;
            } else {
                sleep_until_monotonic(deadline_ns);
            }
        }

        if (reflex_active_) {
            ++latency_value_;
            VkLatencySleepInfoNV sleep = make_info<VkLatencySleepInfoNV>(VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV);
            sleep.signalSemaphore = latency_semaphore_;
            sleep.value = latency_value_;
            check(latency_sleep_(device_, swapchain_.handle, &sleep), "vkLatencySleepNV");
            VkSemaphoreWaitInfo wait = make_info<VkSemaphoreWaitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO);
            wait.semaphoreCount = 1;
            wait.pSemaphores = &latency_semaphore_;
            wait.pValues = &latency_value_;
            check(vkWaitSemaphores(device_, &wait, kLatencyWaitTimeoutNs), "vkWaitSemaphores");
        }

        const uint64_t present_id = frame + 1;
        if (reflex_active_) {
            set_marker(present_id, VK_LATENCY_MARKER_SIMULATION_START_NV);
        }
        const uint64_t sim_ns = monotonic_ns();
        while (next_event < report_.events.size() && report_.events[next_event] <= sim_ns) {
            ++next_event;
        }
        const uint32_t event_count = static_cast<uint32_t>(next_event);
        if (reflex_active_) {
            set_marker(present_id, VK_LATENCY_MARKER_SIMULATION_END_NV);
        }

        uint32_t image_index = 0;
        const VkResult acquired = vkAcquireNextImageKHR(device_, swapchain_.handle, UINT64_MAX,
                                                        acquire_semaphores_[slot], VK_NULL_HANDLE, &image_index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            need_recreate = true;
            continue;
        }
        if (acquired == VK_SUBOPTIMAL_KHR) {
            need_recreate = true;
        } else {
            check(acquired, "vkAcquireNextImageKHR");
        }

        check(vkResetFences(device_, 1, &fences_[slot]), "vkResetFences");
        VkCommandBuffer cmd = command_buffers_[slot];
        check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
        const float time_s = static_cast<float>(static_cast<double>(sim_ns - start_ns) /
                                                static_cast<double>(kNsPerSecond));
        record_commands(cmd, image_index, event_count, time_s);

        if (reflex_active_) {
            set_marker(present_id, VK_LATENCY_MARKER_RENDERSUBMIT_START_NV);
        }
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphore present_semaphore = swapchain_.present_semaphores[image_index];
        VkSubmitInfo submit = make_info<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquire_semaphores_[slot];
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &present_semaphore;
        check(vkQueueSubmit(queue_, 1, &submit, fences_[slot]), "vkQueueSubmit");
        if (reflex_active_) {
            set_marker(present_id, VK_LATENCY_MARKER_RENDERSUBMIT_END_NV);
            set_marker(present_id, VK_LATENCY_MARKER_PRESENT_START_NV);
        }

        VkPresentIdKHR present_id_info = make_info<VkPresentIdKHR>(VK_STRUCTURE_TYPE_PRESENT_ID_KHR);
        present_id_info.swapchainCount = 1;
        present_id_info.pPresentIds = &present_id;
        VkSwapchainKHR swapchain = swapchain_.handle;
        VkPresentInfoKHR present = make_info<VkPresentInfoKHR>(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
        present.pNext = &present_id_info;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &present_semaphore;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &image_index;

        FrameRecord record;
        record.present_id = present_id;
        record.sim_ns = sim_ns;
        record.event_count = event_count;
        record.present_call_ns = monotonic_ns();
        const VkResult presented = vkQueuePresentKHR(queue_, &present);
        record.present_return_ns = monotonic_ns();
        record.present_result = presented;
        if (reflex_active_) {
            set_marker(present_id, VK_LATENCY_MARKER_PRESENT_END_NV);
        }
        report_.frames.push_back(record);

        const bool queued = presented == VK_SUCCESS || presented == VK_SUBOPTIMAL_KHR;
        swapchain_.last_present_id = present_id;
        PresentTicket ticket;
        ticket.present_id = present_id;
        ticket.swapchain = swapchain;
        ticket.waitable = queued;
        worker_->push(ticket);

        if (presented == VK_SUBOPTIMAL_KHR || presented == VK_ERROR_OUT_OF_DATE_KHR) {
            need_recreate = true;
        }
        if (!queued && presented != VK_ERROR_OUT_OF_DATE_KHR) {
            throw ProbeError(kExitFailure, "vkQueuePresentKHR failed: " + vk_result_name(presented));
        }
    }
}

void Renderer::merge_completions() {
    if (!worker_) {
        return;
    }
    for (const CompletionRecord& record : worker_->records()) {
        if (record.present_id == 0 || record.present_id > report_.frames.size()) {
            continue;
        }
        FrameRecord& frame = report_.frames[static_cast<size_t>(record.present_id - 1)];
        frame.complete_ns = record.complete_ns;
        frame.wait_result = record.result;
        frame.wait_skipped = false;
    }
}

void Renderer::shutdown() {
    if (shut_down_) {
        return;
    }
    shut_down_ = true;
    if (worker_) {
        worker_->stop_and_join();
        merge_completions();
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        for (SwapchainResources& resources : retired_) {
            destroy_swapchain(resources);
        }
        retired_.clear();
        destroy_swapchain(swapchain_);
        if (pipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_, pipeline_, nullptr);
        }
        if (pipeline_layout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        }
        if (vertex_shader_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_, vertex_shader_, nullptr);
        }
        if (fragment_shader_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_, fragment_shader_, nullptr);
        }
        if (latency_semaphore_ != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, latency_semaphore_, nullptr);
        }
        for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
            if (acquire_semaphores_[slot] != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, acquire_semaphores_[slot], nullptr);
            }
            if (fences_[slot] != VK_NULL_HANDLE) {
                vkDestroyFence(device_, fences_[slot], nullptr);
            }
        }
        if (command_pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, command_pool_, nullptr);
        }
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
    }
    if (sdl_initialized_) {
        SDL_Quit();
    }
}

std::string json_string(std::string_view text) {
    std::string out = "\"";
    for (char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(byte));
                    out += buffer;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

std::string json_string_array(const std::vector<std::string>& values) {
    std::string out = "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += json_string(values[i]);
    }
    out += "]";
    return out;
}

const char* device_type_name(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete_gpu";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated_gpu";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual_gpu";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
        default: return "other";
    }
}

std::string api_version_string(uint32_t version) {
    return std::to_string(VK_API_VERSION_MAJOR(version)) + "." + std::to_string(VK_API_VERSION_MINOR(version)) +
           "." + std::to_string(VK_API_VERSION_PATCH(version));
}

std::string build_meta(const ProbeReport& r) {
    const Options& o = r.options;
    std::ostringstream os;
    os << "{\n";
    os << "  \"args\": {\n";
    os << "    \"output_dir\": " << json_string(o.output_dir.string()) << ",\n";
    os << "    \"duration_s\": " << o.duration_s << ",\n";
    os << "    \"fps_cap\": " << o.fps_cap << ",\n";
    os << "    \"present_mode\": " << json_string(o.present_mode) << ",\n";
    os << "    \"reflex\": " << (o.reflex ? "\"on\"" : "\"off\"") << ",\n";
    os << "    \"gpu_load\": " << o.gpu_load << ",\n";
    os << "    \"event_mean_ms\": " << o.event_mean_ms << ",\n";
    os << "    \"seed\": " << o.seed << "\n";
    os << "  },\n";
    os << "  \"start_ns\": " << r.start_ns << ",\n";
    os << "  \"exit_code\": " << r.exit_code << ",\n";
    os << "  \"exit_reason\": " << json_string(r.exit_reason) << ",\n";
    os << "  \"device\": {\n";
    os << "    \"name\": " << json_string(r.device.name) << ",\n";
    os << "    \"type\": " << json_string(device_type_name(r.device.type)) << ",\n";
    os << "    \"vendor_id\": " << r.device.vendor_id << ",\n";
    os << "    \"device_id\": " << r.device.device_id << ",\n";
    os << "    \"api_version\": " << json_string(api_version_string(r.device.api_version)) << ",\n";
    os << "    \"driver_version\": " << r.device.driver_version << ",\n";
    os << "    \"driver_name\": " << json_string(r.device.driver_name) << ",\n";
    os << "    \"driver_info\": " << json_string(r.device.driver_info) << "\n";
    os << "  },\n";
    os << "  \"present_mode\": " << json_string(o.present_mode) << ",\n";
    os << "  \"image_count\": " << r.image_count << ",\n";
    os << "  \"swapchain\": {\n";
    os << "    \"format\": " << r.swapchain_format << ",\n";
    os << "    \"color_space\": " << r.swapchain_color_space << ",\n";
    os << "    \"width\": " << r.swapchain_width << ",\n";
    os << "    \"height\": " << r.swapchain_height << "\n";
    os << "  },\n";
    os << "  \"display\": {\n";
    os << "    \"refresh_hz\": " << r.display_refresh_hz << ",\n";
    os << "    \"width\": " << r.display_width << ",\n";
    os << "    \"height\": " << r.display_height << "\n";
    os << "  },\n";
    os << "  \"instance_extensions\": " << json_string_array(r.instance_extensions) << ",\n";
    os << "  \"device_extensions\": " << json_string_array(r.device_extensions) << ",\n";
    os << "  \"reflex\": {\n";
    os << "    \"requested\": " << (o.reflex ? "true" : "false") << ",\n";
    os << "    \"extension_available\": " << (r.reflex_extension ? "true" : "false") << ",\n";
    os << "    \"enabled\": " << (r.reflex_enabled ? "true" : "false") << ",\n";
    os << "    \"low_latency_mode\": " << (r.reflex_enabled ? "true" : "false") << ",\n";
    os << "    \"low_latency_boost\": false,\n";
    os << "    \"minimum_interval_us\": 0\n";
    os << "  },\n";
    os << "  \"recreations\": " << r.recreations << ",\n";
    os << "  \"frames\": " << r.frames.size() << ",\n";
    os << "  \"events\": " << r.events.size() << ",\n";
    os << "  \"window_events\": " << r.window_events.size() << "\n";
    os << "}\n";
    return os.str();
}

std::string build_frames_csv(const ProbeReport& r) {
    std::string out = "frame,present_id,sim_ns,event_count,present_call_ns,present_return_ns,present_result,"
                      "complete_ns,wait_result\n";
    for (size_t i = 0; i < r.frames.size(); ++i) {
        const FrameRecord& f = r.frames[i];
        out += std::to_string(i) + "," + std::to_string(f.present_id) + "," + std::to_string(f.sim_ns) + "," +
               std::to_string(f.event_count) + "," + std::to_string(f.present_call_ns) + "," +
               std::to_string(f.present_return_ns) + "," + vk_result_name(f.present_result) + "," +
               std::to_string(f.complete_ns) + "," +
               (f.wait_skipped ? std::string("skipped") : vk_result_name(f.wait_result)) + "\n";
    }
    return out;
}

std::string build_events_csv(const ProbeReport& r) {
    std::string out = "index,event_ns\n";
    for (size_t i = 0; i < r.events.size(); ++i) {
        out += std::to_string(i) + "," + std::to_string(r.events[i]) + "\n";
    }
    return out;
}

std::string build_window_csv(const ProbeReport& r) {
    std::string out = "ns,event\n";
    for (const WindowRecord& w : r.window_events) {
        out += std::to_string(w.ns) + "," + w.name + "\n";
    }
    return out;
}

bool write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        log_error("cannot open " + path.string());
        return false;
    }
    out << content;
    out.close();
    if (!out) {
        log_error("cannot write " + path.string());
        return false;
    }
    return true;
}

bool write_outputs(const ProbeReport& report) {
    const std::filesystem::path dir = report.options.output_dir;
    bool ok = true;
    ok = write_file(dir / "frames.csv", build_frames_csv(report)) && ok;
    ok = write_file(dir / "events.csv", build_events_csv(report)) && ok;
    ok = write_file(dir / "window.csv", build_window_csv(report)) && ok;
    ok = write_file(dir / "meta.json", build_meta(report)) && ok;
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    bool show_help = false;
    if (auto error = parse_options(argc, argv, options, show_help)) {
        log_error(*error);
        print_usage(stderr);
        return kExitFailure;
    }
    if (show_help) {
        print_usage(stdout);
        return kExitCompleted;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(options.output_dir, ec) ||
        !std::filesystem::is_empty(options.output_dir, ec)) {
        log_error("--output-dir must be an existing empty directory: " + options.output_dir.string());
        return kExitFailure;
    }

    ProbeReport report;
    report.options = options;
    try {
        Renderer renderer(report);
        renderer.run();
        renderer.shutdown();
    } catch (const ProbeError& error) {
        report.exit_code = error.exit_code();
        report.exit_reason = error.what();
        log_error(error.what());
    } catch (const std::exception& error) {
        report.exit_code = kExitFailure;
        report.exit_reason = std::string("error: ") + error.what();
        log_error(report.exit_reason);
    }

    if (!write_outputs(report) && report.exit_code == kExitCompleted) {
        return kExitFailure;
    }
    return report.exit_code;
}

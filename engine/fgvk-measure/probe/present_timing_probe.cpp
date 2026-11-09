/* SPDX-License-Identifier: GPL-3.0-or-later */

// Present-timing probe for VK_EXT_present_timing. A standalone measurement tool: it opens its own
// xdg_toplevel, never queries other windows or processes, and turns off the implicit Vulkan layers so
// no fgvk layer is loaded into this process. Nothing here is packaged or installed.
//
// Build (native CMake, in the existing fgvk-measure project; no containers, no FGVK_PORTABLE_PACKAGE):
//   cmake -S engine/fgvk-measure -B <build-dir> -G Ninja
//   cmake --build <build-dir> --target fgvk-pt-probe
//   ctest --test-dir <build-dir> -R measure-pt-probe --output-on-failure
//
// Run (the build never runs it; a run opens a fullscreen window on the current session):
//   <build-dir>/fgvk-pt-probe --mode fifo --rate 150 --lead 6 --seconds 20
//   <build-dir>/fgvk-pt-probe --mode immediate --uneven --seconds 20
//   <build-dir>/fgvk-pt-probe --mode fifo --no-target --seconds 20
//
// Each run writes /tmp/fgvk-pt-probe-<uid>-<pid>.log. Analyse a targeted run next to a --no-target run:
//   python3 -I engine/fgvk-measure/probe/analyze_pt_probe.py <targeted.log> <no-target.log>

#include <vulkan/vulkan.h>
#include <wayland-client.h>

#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <poll.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <random>
#include <deque>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kNsPerSec = 1'000'000'000ULL;
constexpr uint32_t kWantedImages = 4;
constexpr uint32_t kTimingQueueSize = 64;
constexpr uint32_t kFallbackWidth = 2560;
constexpr uint32_t kFallbackHeight = 1440;
constexpr uint32_t kStageSlots = 8;
constexpr uint64_t kJitterMaxNs = 2'000'000ULL;
constexpr uint64_t kStartDelayNs = 500'000'000ULL;
constexpr uint64_t kWaitTimeoutNs = 5 * kNsPerSec;
constexpr uint64_t kDrainTimeoutNs = 2 * kNsPerSec;
constexpr uint64_t kConfigureTimeoutNs = 5 * kNsPerSec;
constexpr uint64_t kMaxPresents = 200'000;
// Dark grey background with a dim bar. Nothing alternates, flashes or strobes.
constexpr float kBackground[4] = {0.06f, 0.06f, 0.06f, 1.0f};
constexpr float kBar[4] = {0.16f, 0.16f, 0.16f, 1.0f};

struct Options {
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    bool modeGiven = false;
    double rateHz = 150.0;
    double leadMs = 6.0;
    bool uneven = false;
    bool noTarget = false;
    double seconds = 20.0;
    std::string output;          // wl_output name for the fullscreen request, empty = compositor's choice
    bool targetStageZero = false; // leave targetTimeDomainPresentStage at 0 instead of the queried stage
};

struct ProbeError {
    std::string message;
};

[[noreturn]] void fail(const std::string& message) {
    throw ProbeError{message};
}

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        fail(std::string(what) + " returned VkResult " + std::to_string(static_cast<int>(result)));
    }
}

bool parseNumber(const char* text, double& out) {
    if (!text || !*text) return false;
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !std::isfinite(value)) return false;
    out = value;
    return true;
}

void printUsage(FILE* out) {
    std::fprintf(out,
                 "usage: fgvk-pt-probe --mode immediate|fifo [--rate HZ] [--lead MS] [--uneven]\n"
                 "                     [--no-target] [--seconds N] [--output NAME] [--target-stage-zero]\n"
                 "  --output NAME  fullscreen on this output (wl_output name, for example DP-1)\n"
                 "  --rate HZ      outputs per second (default 150)\n"
                 "  --lead MS      submit this long before each target (default 6)\n"
                 "  --uneven       add 0-2 ms uniform random CPU submit jitter\n"
                 "  --no-target    present on the CPU at the target instead of attaching a target time\n"
                 "  --seconds N    run length (default 20)\n");
}

// Returns 0 to run, 1 for --help, 2 for a usage error.
int parseArgs(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) return 1;
        if (std::strcmp(arg, "--uneven") == 0) {
            options.uneven = true;
            continue;
        }
        if (std::strcmp(arg, "--no-target") == 0) {
            options.noTarget = true;
            continue;
        }
        if (std::strcmp(arg, "--target-stage-zero") == 0) {
            options.targetStageZero = true;
            continue;
        }
        if (std::strcmp(arg, "--output") == 0 && hasValue) {
            options.output = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "--mode") == 0 && hasValue) {
            const char* mode = argv[++i];
            if (std::strcmp(mode, "immediate") == 0) {
                options.mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            } else if (std::strcmp(mode, "fifo") == 0) {
                options.mode = VK_PRESENT_MODE_FIFO_KHR;
            } else {
                std::fprintf(stderr, "framegen-vulkan: pt-probe: --mode takes immediate or fifo\n");
                return 2;
            }
            options.modeGiven = true;
            continue;
        }
        double* target = nullptr;
        if (std::strcmp(arg, "--rate") == 0) {
            target = &options.rateHz;
        } else if (std::strcmp(arg, "--lead") == 0) {
            target = &options.leadMs;
        } else if (std::strcmp(arg, "--seconds") == 0) {
            target = &options.seconds;
        }
        if (!target || !hasValue || !parseNumber(argv[i + 1], *target)) {
            std::fprintf(stderr, "framegen-vulkan: pt-probe: bad or unknown argument %s\n", arg);
            return 2;
        }
        ++i;
    }
    if (!options.modeGiven) {
        std::fprintf(stderr, "framegen-vulkan: pt-probe: --mode immediate|fifo is required\n");
        return 2;
    }
    if (!(options.rateHz > 0.0 && options.rateHz <= 1000.0)) {
        std::fprintf(stderr, "framegen-vulkan: pt-probe: --rate must be in (0, 1000]\n");
        return 2;
    }
    if (!(options.leadMs >= 0.0 && options.leadMs <= 100.0)) {
        std::fprintf(stderr, "framegen-vulkan: pt-probe: --lead must be in [0, 100] ms\n");
        return 2;
    }
    if (!(options.seconds > 0.0 && options.seconds <= 3600.0)) {
        std::fprintf(stderr, "framegen-vulkan: pt-probe: --seconds must be in (0, 3600]\n");
        return 2;
    }
    const double presents = std::llround(options.seconds * options.rateHz);
    if (presents < 1.0 || presents > static_cast<double>(kMaxPresents)) {
        std::fprintf(stderr, "framegen-vulkan: pt-probe: --seconds times --rate must give 1 to %" PRIu64 " presents\n",
                     kMaxPresents);
        return 2;
    }
    return 0;
}

uint64_t nowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * kNsPerSec + static_cast<uint64_t>(ts.tv_nsec);
}

void sleepUntil(uint64_t whenNs) {
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(whenNs / kNsPerSec);
    ts.tv_nsec = static_cast<long>(whenNs % kNsPerSec);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
    }
}

uint32_t boundVersion(uint32_t advertised, int known) {
    return std::min(advertised, static_cast<uint32_t>(known));
}

bool hasExtension(const std::vector<VkExtensionProperties>& available, const char* name) {
    return std::any_of(available.begin(), available.end(),
                       [name](const VkExtensionProperties& p) { return std::strcmp(p.extensionName, name) == 0; });
}

template <typename Fn>
Fn instanceFn(VkInstance instance, const char* name) {
    auto fn = reinterpret_cast<Fn>(vkGetInstanceProcAddr(instance, name));
    if (!fn) fail(std::string("missing instance entry point ") + name);
    return fn;
}

template <typename Fn>
Fn deviceFn(VkDevice device, const char* name) {
    auto fn = reinterpret_cast<Fn>(vkGetDeviceProcAddr(device, name));
    if (!fn) fail(std::string("missing device entry point ") + name);
    return fn;
}

// One record per present. Probe::frames is sized once and never resized, so the feedback callbacks can
// keep a pointer to its entry.
struct FrameRecord {
    uint64_t targetNs = 0;
    uint64_t submitNs = 0;
    uint64_t callNs = 0;
    uint64_t stageNs = 0;
    uint64_t presentedNs = 0;
    uint32_t flags = 0;
    bool discarded = false;
    struct wp_presentation_feedback* feedback = nullptr;
};

struct InstanceFns {
    PFN_vkDestroySurfaceKHR destroySurface = nullptr;
    PFN_vkCreateWaylandSurfaceKHR createWaylandSurface = nullptr;
    PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR waylandPresentationSupport = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR surfaceCapabilities2 = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR surfaceFormats = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR surfacePresentModes = nullptr;
};

struct DeviceFns {
    PFN_vkCreateSwapchainKHR createSwapchain = nullptr;
    PFN_vkDestroySwapchainKHR destroySwapchain = nullptr;
    PFN_vkGetSwapchainImagesKHR swapchainImages = nullptr;
    PFN_vkAcquireNextImageKHR acquireNextImage = nullptr;
    PFN_vkQueuePresentKHR queuePresent = nullptr;
    PFN_vkSetSwapchainPresentTimingQueueSizeEXT setTimingQueueSize = nullptr;
    PFN_vkGetSwapchainTimingPropertiesEXT swapchainTiming = nullptr;
    PFN_vkGetSwapchainTimeDomainPropertiesEXT swapchainTimeDomains = nullptr;
    PFN_vkGetPastPresentationTimingEXT pastTiming = nullptr;
    PFN_vkGetCalibratedTimestampsKHR calibrate = nullptr;
};

struct Probe {
    explicit Probe(const Options& value) : options(value), seed(std::random_device{}()), rng(seed) {}
    ~Probe();
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;

    int run();

    Options options;
    uint64_t seed;
    std::mt19937_64 rng;
    std::uniform_int_distribution<uint64_t> jitterDist{0, kJitterMaxNs};
    uint64_t leadNs = 0;

    // Wayland. The window uses the default queue. wp_presentation and its feedback objects live on
    // feedbackQueue, so feedback events never mix with the window's own events.
    struct wl_display* display = nullptr;
    struct wl_event_queue* feedbackQueue = nullptr;
    struct wl_registry* registry = nullptr;
    struct wl_registry* privateRegistry = nullptr;
    struct wl_compositor* compositor = nullptr;
    struct xdg_wm_base* wmBase = nullptr;
    struct wl_surface* surface = nullptr;
    struct xdg_surface* xdgSurface = nullptr;
    struct xdg_toplevel* toplevel = nullptr;
    struct wp_presentation* presentation = nullptr;
    // Outputs, only to place the probe's own window (wl_output name, version 4).
    struct OutputEntry {
        struct wl_output* output = nullptr;
        std::string name;
    };
    std::deque<OutputEntry> outputs;
    uint32_t clockId = 0;
    bool configured = false;
    bool closed = false;
    int32_t configuredWidth = 0;
    int32_t configuredHeight = 0;

    // Vulkan.
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::string deviceName;
    bool hasSurfaceMaintenance1 = false;
    VkPhysicalDeviceFeatures2 features2{};
    VkPhysicalDeviceVulkan13Features vulkan13{};
    VkPhysicalDevicePresentTimingFeaturesEXT presentTimingFeatures{};
    VkPhysicalDevicePresentId2FeaturesKHR presentId2Features{};
    InstanceFns instFns;
    DeviceFns devFns;
    VkSurfaceKHR vkSurface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkSurfaceFormatKHR format{};
    VkExtent2D extent{};
    VkPresentStageFlagsEXT presentStage = 0;
    uint64_t timeDomainId = 0;
    uint64_t refreshDuration = 0;
    uint64_t refreshInterval = 0;
    // Stage-local time domain: stage time = CLOCK_MONOTONIC + stageOffset (0 when the domain is CLOCK_MONOTONIC).
    bool stageLocal = false;
    int64_t stageOffset = 0;
    int64_t stageOffsetEnd = 0;
    uint64_t calibrationWindowNs = 0;
    bool calibrateStage(int64_t& offset);
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmds;
    std::vector<VkFence> fences;
    std::vector<VkSemaphore> acquireSems;
    std::vector<VkSemaphore> renderSems;

    // Run state.
    std::vector<FrameRecord> frames;
    uint64_t timingQueueFull = 0;
    uint64_t suboptimal = 0;
    uint64_t feedbackRequestFailures = 0;
    bool pastTimingWarned = false;

    void connectWayland();
    void createWindow();
    void initVulkan();
    void linkFeatureChain();
    void pickPhysicalDevice();
    void createDevice();
    void createSwapchain();
    void createFrameResources();
    void pump(int timeoutMs);
    void harvest();
    void recordFrame(uint32_t slot, uint32_t image, uint64_t k);
    void presentFrame(uint64_t k, uint64_t t0);
    void warmUp();
    uint64_t swapchainRecreations = 0;
    void drain();
    bool allReported() const;
    void writeLog();
    const char* stageName() const;
};

// Wayland listeners. Each callback only records state. None of them changes the window.

void onWmBasePing(void*, struct xdg_wm_base* wmBase, uint32_t serial) {
    xdg_wm_base_pong(wmBase, serial);
}

void onSurfaceConfigure(void* data, struct xdg_surface* xdgSurface, uint32_t serial) {
    xdg_surface_ack_configure(xdgSurface, serial);
    static_cast<Probe*>(data)->configured = true;
}

void onToplevelConfigure(void* data, struct xdg_toplevel*, int32_t width, int32_t height, struct wl_array*) {
    auto* probe = static_cast<Probe*>(data);
    probe->configuredWidth = width;
    probe->configuredHeight = height;
}

void onToplevelClose(void* data, struct xdg_toplevel*) {
    static_cast<Probe*>(data)->closed = true;
}

void onToplevelBounds(void*, struct xdg_toplevel*, int32_t, int32_t) {}

void onToplevelCapabilities(void*, struct xdg_toplevel*, struct wl_array*) {}

const xdg_wm_base_listener kWmBaseListener{onWmBasePing};
const xdg_surface_listener kSurfaceListener{onSurfaceConfigure};
const xdg_toplevel_listener kToplevelListener{onToplevelConfigure, onToplevelClose, onToplevelBounds,
                                              onToplevelCapabilities};

void onClockId(void* data, struct wp_presentation*, uint32_t clockId) {
    static_cast<Probe*>(data)->clockId = clockId;
}

const wp_presentation_listener kPresentationListener{onClockId};

void onOutputGeometry(void*, struct wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*,
                      int32_t) {}
void onOutputMode(void*, struct wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
void onOutputDone(void*, struct wl_output*) {}
void onOutputScale(void*, struct wl_output*, int32_t) {}
void onOutputName(void* data, struct wl_output*, const char* name) {
    static_cast<Probe::OutputEntry*>(data)->name = name ? name : "";
}
void onOutputDescription(void*, struct wl_output*, const char*) {}

const wl_output_listener kOutputListener{onOutputGeometry, onOutputMode,        onOutputDone,
                                         onOutputScale,    onOutputName,        onOutputDescription};

void onSyncOutput(void*, struct wp_presentation_feedback*, struct wl_output*) {}

void onPresented(void* data, struct wp_presentation_feedback* feedback, uint32_t secHi, uint32_t secLo, uint32_t nsec,
                 uint32_t, uint32_t, uint32_t, uint32_t flags) {
    auto* frame = static_cast<FrameRecord*>(data);
    const uint64_t seconds = (uint64_t{secHi} << 32) | secLo;
    if (nsec < kNsPerSec && seconds <= (UINT64_MAX - nsec) / kNsPerSec) {
        frame->presentedNs = seconds * kNsPerSec + nsec;
    }
    frame->flags = flags;
    frame->feedback = nullptr;
    wp_presentation_feedback_destroy(feedback);
}

void onDiscarded(void* data, struct wp_presentation_feedback* feedback) {
    auto* frame = static_cast<FrameRecord*>(data);
    frame->discarded = true;
    frame->feedback = nullptr;
    wp_presentation_feedback_destroy(feedback);
}

const wp_presentation_feedback_listener kFeedbackListener{onSyncOutput, onPresented, onDiscarded};

void onDefaultGlobal(void* data, struct wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t version) {
    auto* probe = static_cast<Probe*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0 && !probe->compositor) {
        probe->compositor = static_cast<struct wl_compositor*>(wl_registry_bind(
            registry, name, &wl_compositor_interface, boundVersion(version, wl_compositor_interface.version)));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0 && !probe->wmBase) {
        probe->wmBase = static_cast<struct xdg_wm_base*>(wl_registry_bind(
            registry, name, &xdg_wm_base_interface, boundVersion(version, xdg_wm_base_interface.version)));
        if (probe->wmBase) xdg_wm_base_add_listener(probe->wmBase, &kWmBaseListener, probe);
    } else if (std::strcmp(interface, wl_output_interface.name) == 0 && version >= 4 && !probe->options.output.empty()) {
        auto* output = static_cast<struct wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, 4));
        if (output) {
            probe->outputs.push_back({output, {}});
            wl_output_add_listener(output, &kOutputListener, &probe->outputs.back());
        }
    }
}

void onPrivateGlobal(void* data, struct wl_registry* registry, uint32_t name, const char* interface,
                     uint32_t version) {
    auto* probe = static_cast<Probe*>(data);
    if (std::strcmp(interface, wp_presentation_interface.name) == 0 && !probe->presentation) {
        probe->presentation = static_cast<struct wp_presentation*>(wl_registry_bind(
            registry, name, &wp_presentation_interface, boundVersion(version, wp_presentation_interface.version)));
        if (probe->presentation) wp_presentation_add_listener(probe->presentation, &kPresentationListener, probe);
    }
}

void onGlobalRemove(void*, struct wl_registry*, uint32_t) {}

const wl_registry_listener kDefaultRegistryListener{onDefaultGlobal, onGlobalRemove};
const wl_registry_listener kPrivateRegistryListener{onPrivateGlobal, onGlobalRemove};

Probe::~Probe() {
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
        for (VkFence fence : fences) vkDestroyFence(device, fence, nullptr);
        for (VkSemaphore semaphore : acquireSems) vkDestroySemaphore(device, semaphore, nullptr);
        for (VkSemaphore semaphore : renderSems) vkDestroySemaphore(device, semaphore, nullptr);
        if (commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, commandPool, nullptr);
        for (VkImageView view : views) vkDestroyImageView(device, view, nullptr);
        if (swapchain != VK_NULL_HANDLE && devFns.destroySwapchain) {
            devFns.destroySwapchain(device, swapchain, nullptr);
        }
        vkDestroyDevice(device, nullptr);
    }
    if (instance != VK_NULL_HANDLE) {
        if (vkSurface != VK_NULL_HANDLE && instFns.destroySurface) {
            instFns.destroySurface(instance, vkSurface, nullptr);
        }
        vkDestroyInstance(instance, nullptr);
    }
    for (FrameRecord& frame : frames) {
        if (frame.feedback) wp_presentation_feedback_destroy(frame.feedback);
    }
    for (OutputEntry& entry : outputs) wl_output_destroy(entry.output);
    if (presentation) wp_presentation_destroy(presentation);
    if (toplevel) xdg_toplevel_destroy(toplevel);
    if (xdgSurface) xdg_surface_destroy(xdgSurface);
    if (surface) wl_surface_destroy(surface);
    if (wmBase) xdg_wm_base_destroy(wmBase);
    if (registry) wl_registry_destroy(registry);
    if (privateRegistry) wl_registry_destroy(privateRegistry);
    if (feedbackQueue) wl_event_queue_destroy(feedbackQueue);
    if (display) wl_display_disconnect(display);
}

void Probe::connectWayland() {
    display = wl_display_connect(nullptr);
    if (!display) fail("cannot connect to the Wayland display (is WAYLAND_DISPLAY set for this session?)");
    feedbackQueue = wl_display_create_queue(display);
    if (!feedbackQueue) fail("wl_display_create_queue failed");

    registry = wl_display_get_registry(display);
    if (!registry) fail("wl_display_get_registry failed");
    if (wl_registry_add_listener(registry, &kDefaultRegistryListener, this) != 0) {
        fail("wl_registry_add_listener failed");
    }

    // A wrapper of the display moves the registry request onto the feedback queue.
    void* wrapper = wl_proxy_create_wrapper(display);
    if (!wrapper) fail("wl_proxy_create_wrapper failed");
    wl_proxy_set_queue(static_cast<struct wl_proxy*>(wrapper), feedbackQueue);
    privateRegistry = wl_display_get_registry(static_cast<struct wl_display*>(wrapper));
    wl_proxy_wrapper_destroy(wrapper);
    if (!privateRegistry) fail("binding the registry on the feedback queue failed");
    if (wl_registry_add_listener(privateRegistry, &kPrivateRegistryListener, this) != 0) {
        fail("wl_registry_add_listener failed (feedback queue)");
    }

    if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip_queue(display, feedbackQueue) < 0) {
        fail("Wayland roundtrip failed");
    }
    if (!compositor || !wmBase) fail("the compositor does not offer wl_compositor and xdg_wm_base");
    if (!presentation) fail("the compositor does not offer wp_presentation (presentation-time)");
    // The clock_id event follows the bind. One more roundtrip on the feedback queue collects it.
    if (wl_display_roundtrip_queue(display, feedbackQueue) < 0) fail("Wayland roundtrip failed (clock_id)");
}

void Probe::pump(int timeoutMs) {
    // Take the socket read lock. Queued default-queue events or another reader can refuse it, so dispatch
    // what is queued and retry a few times. If it still fails, skip the socket read this round.
    bool locked = false;
    for (int attempt = 0; attempt < 8 && !locked; ++attempt) {
        if (wl_display_prepare_read(display) == 0) {
            locked = true;
        } else if (wl_display_dispatch_pending(display) < 0) {
            fail("wl_display_dispatch_pending failed");
        }
    }
    if (locked) {
        wl_display_flush(display);
        pollfd pfd{};
        pfd.fd = wl_display_get_fd(display);
        pfd.events = POLLIN;
        const int ready = poll(&pfd, 1, timeoutMs);
        if (ready > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_read_events(display) < 0) fail("wl_display_read_events failed");
        } else {
            wl_display_cancel_read(display);
            if (ready > 0 && (pfd.revents & (POLLERR | POLLHUP))) fail("the Wayland connection closed");
        }
    }
    if (wl_display_dispatch_pending(display) < 0 || wl_display_dispatch_queue_pending(display, feedbackQueue) < 0) {
        fail("Wayland dispatch failed");
    }
    const int displayError = wl_display_get_error(display);
    if (displayError != 0) fail("Wayland display error " + std::to_string(displayError));
    wl_display_flush(display);
}

void Probe::createWindow() {
    surface = wl_compositor_create_surface(compositor);
    if (!surface) fail("wl_compositor_create_surface failed");
    xdgSurface = xdg_wm_base_get_xdg_surface(wmBase, surface);
    if (!xdgSurface) fail("xdg_wm_base_get_xdg_surface failed");
    xdg_surface_add_listener(xdgSurface, &kSurfaceListener, this);
    toplevel = xdg_surface_get_toplevel(xdgSurface);
    if (!toplevel) fail("xdg_surface_get_toplevel failed");
    xdg_toplevel_add_listener(toplevel, &kToplevelListener, this);
    xdg_toplevel_set_title(toplevel, "framegen-vulkan pt-probe");
    xdg_toplevel_set_app_id(toplevel, "framegen-vulkan-pt-probe");
    struct wl_output* target = nullptr;
    if (!options.output.empty()) {
        // The output names arrive after the bind; one more roundtrip collects them.
        if (wl_display_roundtrip(display) < 0) fail("Wayland roundtrip failed (output names)");
        std::string known;
        for (const OutputEntry& entry : outputs) {
            if (entry.name == options.output) target = entry.output;
            known += " " + entry.name;
        }
        if (!target) fail("output " + options.output + " not found; outputs:" + known);
    }
    xdg_toplevel_set_fullscreen(toplevel, target);
    // A commit without a buffer asks the compositor for the first configure. The buffer comes later.
    wl_surface_commit(surface);
    wl_display_flush(display);
    const uint64_t deadline = nowNs() + kConfigureTimeoutNs;
    while (!configured && !closed && nowNs() < deadline) pump(20);
    if (closed) fail("the compositor closed the probe window before it was configured");
    if (!configured) fail("no configure event from the compositor within 5 s");
}

void Probe::linkFeatureChain() {
    features2 = {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &vulkan13;
    vulkan13 = {};
    vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan13.pNext = &presentTimingFeatures;
    presentTimingFeatures = {};
    presentTimingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT;
    presentTimingFeatures.pNext = &presentId2Features;
    presentId2Features = {};
    presentId2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR;
}

void Probe::initVulkan() {
    uint32_t extensionCount = 0;
    check(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr),
          "vkEnumerateInstanceExtensionProperties");
    std::vector<VkExtensionProperties> available(extensionCount);
    check(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, available.data()),
          "vkEnumerateInstanceExtensionProperties");

    std::vector<const char*> enabled{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
                                     VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME};
    for (const char* name : enabled) {
        if (!hasExtension(available, name)) fail(std::string("missing instance extension ") + name);
    }
    hasSurfaceMaintenance1 = hasExtension(available, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    if (hasSurfaceMaintenance1) enabled.push_back(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "fgvk-pt-probe";
    app.applicationVersion = 1;
    app.pEngineName = "framegen-vulkan";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    info.ppEnabledExtensionNames = enabled.data();
    check(vkCreateInstance(&info, nullptr, &instance), "vkCreateInstance");

    instFns.destroySurface = instanceFn<PFN_vkDestroySurfaceKHR>(instance, "vkDestroySurfaceKHR");
    instFns.createWaylandSurface = instanceFn<PFN_vkCreateWaylandSurfaceKHR>(instance, "vkCreateWaylandSurfaceKHR");
    instFns.waylandPresentationSupport = instanceFn<PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR>(
        instance, "vkGetPhysicalDeviceWaylandPresentationSupportKHR");
    instFns.surfaceCapabilities2 = instanceFn<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
        instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
    instFns.surfaceFormats = instanceFn<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
        instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    instFns.surfacePresentModes = instanceFn<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
        instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");

    pickPhysicalDevice();
    createDevice();
}

void Probe::pickPhysicalDevice() {
    uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");

    const char* required[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PRESENT_ID_2_EXTENSION_NAME,
                              VK_EXT_PRESENT_TIMING_EXTENSION_NAME, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME};
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;

        uint32_t extensionCount = 0;
        check(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr),
              "vkEnumerateDeviceExtensionProperties");
        std::vector<VkExtensionProperties> available(extensionCount);
        check(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, available.data()),
              "vkEnumerateDeviceExtensionProperties");
        const bool extensionsOk = std::all_of(std::begin(required), std::end(required), [&available](const char* name) {
            return hasExtension(available, name);
        });
        if (!extensionsOk) continue;

        linkFeatureChain();
        vkGetPhysicalDeviceFeatures2(candidate, &features2);
        if (!vulkan13.dynamicRendering || !presentTimingFeatures.presentTiming ||
            !presentTimingFeatures.presentAtAbsoluteTime || !presentId2Features.presentId2) {
            continue;
        }

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
        bool found = false;
        for (uint32_t i = 0; i < familyCount && !found; ++i) {
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            if (instFns.waylandPresentationSupport(candidate, i, display)) {
                queueFamily = i;
                found = true;
            }
        }
        if (!found) continue;

        physical = candidate;
        deviceName = props.deviceName;
        return;
    }
    fail("no Vulkan device offers VK_EXT_present_timing with absolute targets on this Wayland display");
}

void Probe::createDevice() {
    linkFeatureChain();
    vulkan13.dynamicRendering = VK_TRUE;
    presentTimingFeatures.presentTiming = VK_TRUE;
    presentTimingFeatures.presentAtAbsoluteTime = VK_TRUE;
    presentId2Features.presentId2 = VK_TRUE;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PRESENT_ID_2_EXTENSION_NAME,
                                VK_EXT_PRESENT_TIMING_EXTENSION_NAME, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME};
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &features2;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = static_cast<uint32_t>(std::size(extensions));
    info.ppEnabledExtensionNames = extensions;
    check(vkCreateDevice(physical, &info, nullptr, &device), "vkCreateDevice");
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    devFns.createSwapchain = deviceFn<PFN_vkCreateSwapchainKHR>(device, "vkCreateSwapchainKHR");
    devFns.destroySwapchain = deviceFn<PFN_vkDestroySwapchainKHR>(device, "vkDestroySwapchainKHR");
    devFns.swapchainImages = deviceFn<PFN_vkGetSwapchainImagesKHR>(device, "vkGetSwapchainImagesKHR");
    devFns.acquireNextImage = deviceFn<PFN_vkAcquireNextImageKHR>(device, "vkAcquireNextImageKHR");
    devFns.queuePresent = deviceFn<PFN_vkQueuePresentKHR>(device, "vkQueuePresentKHR");
    devFns.setTimingQueueSize = deviceFn<PFN_vkSetSwapchainPresentTimingQueueSizeEXT>(
        device, "vkSetSwapchainPresentTimingQueueSizeEXT");
    devFns.swapchainTiming = deviceFn<PFN_vkGetSwapchainTimingPropertiesEXT>(device, "vkGetSwapchainTimingPropertiesEXT");
    devFns.swapchainTimeDomains = deviceFn<PFN_vkGetSwapchainTimeDomainPropertiesEXT>(
        device, "vkGetSwapchainTimeDomainPropertiesEXT");
    devFns.pastTiming = deviceFn<PFN_vkGetPastPresentationTimingEXT>(device, "vkGetPastPresentationTimingEXT");
    devFns.calibrate = deviceFn<PFN_vkGetCalibratedTimestampsKHR>(device, "vkGetCalibratedTimestampsKHR");
}

void Probe::createSwapchain() {
    VkWaylandSurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.display = display;
    surfaceInfo.surface = surface;
    if (vkSurface == VK_NULL_HANDLE) {
        check(instFns.createWaylandSurface(instance, &surfaceInfo, nullptr, &vkSurface), "vkCreateWaylandSurfaceKHR");
    }

    // The capability query is per present mode when VK_KHR_surface_maintenance1 is available.
    VkSurfacePresentModeKHR modeInfo{};
    modeInfo.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_KHR;
    modeInfo.presentMode = options.mode;
    VkPhysicalDeviceSurfaceInfo2KHR surfaceQuery{};
    surfaceQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
    surfaceQuery.surface = vkSurface;
    if (hasSurfaceMaintenance1) surfaceQuery.pNext = &modeInfo;

    VkPresentTimingSurfaceCapabilitiesEXT timingCaps{};
    timingCaps.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT;
    VkSurfaceCapabilitiesPresentId2KHR presentIdCaps{};
    presentIdCaps.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR;
    presentIdCaps.pNext = &timingCaps;
    VkSurfaceCapabilities2KHR caps{};
    caps.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
    caps.pNext = &presentIdCaps;
    check(instFns.surfaceCapabilities2(physical, &surfaceQuery, &caps), "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
    if (!presentIdCaps.presentId2Supported) fail("the surface does not support VK_KHR_present_id2 presents");
    if (!timingCaps.presentTimingSupported || !timingCaps.presentAtAbsoluteTimeSupported) {
        fail(std::string("the surface lacks absolute present targets for this mode (presentTimingSupported=") +
             (timingCaps.presentTimingSupported ? "1" : "0") + " presentAtAbsoluteTimeSupported=" +
             (timingCaps.presentAtAbsoluteTimeSupported ? "1" : "0") + ")");
    }
    if (timingCaps.presentStageQueries & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT) {
        presentStage = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
    } else if (timingCaps.presentStageQueries & VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT) {
        presentStage = VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT;
    } else {
        fail("the surface offers neither IMAGE_FIRST_PIXEL_OUT nor QUEUE_OPERATIONS_END present stages");
    }

    uint32_t formatCount = 0;
    check(instFns.surfaceFormats(physical, vkSurface, &formatCount, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR");
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    check(instFns.surfaceFormats(physical, vkSurface, &formatCount, formats.data()),
          "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (formats.empty()) fail("the surface reports no formats");
    format = formats.front();
    for (const VkSurfaceFormatKHR& candidate : formats) {
        if (candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
            (candidate.format == VK_FORMAT_B8G8R8A8_SRGB || candidate.format == VK_FORMAT_R8G8B8A8_SRGB)) {
            format = candidate;
            break;
        }
    }

    uint32_t modeCount = 0;
    check(instFns.surfacePresentModes(physical, vkSurface, &modeCount, nullptr),
          "vkGetPhysicalDeviceSurfacePresentModesKHR");
    std::vector<VkPresentModeKHR> modes(modeCount);
    check(instFns.surfacePresentModes(physical, vkSurface, &modeCount, modes.data()),
          "vkGetPhysicalDeviceSurfacePresentModesKHR");
    if (std::find(modes.begin(), modes.end(), options.mode) == modes.end()) {
        fail("the surface does not offer the requested present mode");
    }

    const VkSurfaceCapabilitiesKHR& sc = caps.surfaceCapabilities;
    if (sc.currentExtent.width != UINT32_MAX) {
        extent = sc.currentExtent;
    } else {
        const uint32_t maxWidth = sc.maxImageExtent.width ? sc.maxImageExtent.width : UINT32_MAX;
        const uint32_t maxHeight = sc.maxImageExtent.height ? sc.maxImageExtent.height : UINT32_MAX;
        const uint32_t wantWidth = configuredWidth > 0 ? static_cast<uint32_t>(configuredWidth) : kFallbackWidth;
        const uint32_t wantHeight = configuredHeight > 0 ? static_cast<uint32_t>(configuredHeight) : kFallbackHeight;
        extent.width = std::clamp(wantWidth, sc.minImageExtent.width, maxWidth);
        extent.height = std::clamp(wantHeight, sc.minImageExtent.height, maxHeight);
    }
    if (extent.width < 64 || extent.height < 64) fail("the window extent is too small for the bar geometry");

    uint32_t imageCount = kWantedImages;
    if (imageCount < sc.minImageCount) fail("the surface needs " + std::to_string(sc.minImageCount) + " images");
    if (sc.maxImageCount > 0 && imageCount > sc.maxImageCount) imageCount = sc.maxImageCount;

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    for (VkCompositeAlphaFlagBitsKHR candidate :
         {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
          VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR}) {
        if (sc.supportedCompositeAlpha & candidate) {
            alpha = candidate;
            break;
        }
    }

    VkSwapchainCreateInfoKHR swapInfo{};
    swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapInfo.flags = static_cast<VkSwapchainCreateFlagsKHR>(VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT |
                                                            VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR);
    swapInfo.surface = vkSurface;
    swapInfo.minImageCount = imageCount;
    swapInfo.imageFormat = format.format;
    swapInfo.imageColorSpace = format.colorSpace;
    swapInfo.imageExtent = extent;
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapInfo.preTransform = sc.currentTransform;
    swapInfo.compositeAlpha = alpha;
    swapInfo.presentMode = options.mode;
    swapInfo.clipped = VK_TRUE;
    check(devFns.createSwapchain(device, &swapInfo, nullptr, &swapchain), "vkCreateSwapchainKHR");
    check(devFns.setTimingQueueSize(device, swapchain, kTimingQueueSize), "vkSetSwapchainPresentTimingQueueSizeEXT");

    VkSwapchainTimeDomainPropertiesEXT domainProps{};
    domainProps.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT;
    uint64_t domainCounter = 0;
    check(devFns.swapchainTimeDomains(device, swapchain, &domainProps, &domainCounter),
          "vkGetSwapchainTimeDomainPropertiesEXT (count)");
    std::vector<VkTimeDomainKHR> domains(domainProps.timeDomainCount);
    std::vector<uint64_t> domainIds(domainProps.timeDomainCount);
    domainProps.pTimeDomains = domains.data();
    domainProps.pTimeDomainIds = domainIds.data();
    check(devFns.swapchainTimeDomains(device, swapchain, &domainProps, &domainCounter),
          "vkGetSwapchainTimeDomainPropertiesEXT");
    bool monotonic = false;
    for (size_t i = 0; i < domains.size(); ++i) {
        if (domains[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR) {
            timeDomainId = domainIds[i];
            monotonic = true;
        }
    }
    if (!monotonic) {
        // The driver may only offer a clock local to a present stage. Calibrate it against CLOCK_MONOTONIC;
        // targets and stage times are converted with that offset, the log stays in CLOCK_MONOTONIC.
        std::string offered;
        for (size_t i = 0; i < domains.size(); ++i) {
            offered += " " + std::to_string(static_cast<int>(domains[i])) + "(id " + std::to_string(domainIds[i]) + ")";
            if (domains[i] == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT && !stageLocal) {
                timeDomainId = domainIds[i];
                stageLocal = true;
            }
        }
        if (!stageLocal) fail("the swapchain offers neither CLOCK_MONOTONIC nor a present-stage-local time domain; offered VkTimeDomainKHR:" + offered);
        if (!calibrateStage(stageOffset)) fail("could not calibrate the present-stage-local clock against CLOCK_MONOTONIC");
    }

    VkSwapchainTimingPropertiesEXT timingProps{};
    timingProps.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT;
    uint64_t timingCounter = 0;
    if (devFns.swapchainTiming(device, swapchain, &timingProps, &timingCounter) == VK_SUCCESS) {
        refreshDuration = timingProps.refreshDuration;
        refreshInterval = timingProps.refreshInterval;
    }

    uint32_t total = 0;
    check(devFns.swapchainImages(device, swapchain, &total, nullptr), "vkGetSwapchainImagesKHR");
    images.assign(total, VK_NULL_HANDLE);
    check(devFns.swapchainImages(device, swapchain, &total, images.data()), "vkGetSwapchainImagesKHR");
    for (VkImage image : images) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        check(vkCreateImageView(device, &viewInfo, nullptr, &view), "vkCreateImageView");
        views.push_back(view);
    }
}

// One vkGetCalibratedTimestampsKHR sample of the stage-local clock, bracketed by CLOCK_MONOTONIC reads.
// Keeps the tightest of 16 samples.
bool Probe::calibrateStage(int64_t& offset) {
    VkSwapchainCalibratedTimestampInfoEXT swapchainInfo{};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT;
    swapchainInfo.swapchain = swapchain;
    swapchainInfo.presentStage = presentStage;
    swapchainInfo.timeDomainId = timeDomainId;
    VkCalibratedTimestampInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
    info.pNext = &swapchainInfo;
    info.timeDomain = VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT;
    uint64_t best = UINT64_MAX;
    for (int attempt = 0; attempt < 16; ++attempt) {
        uint64_t stageTime = 0;
        uint64_t deviation = 0;
        const uint64_t before = nowNs();
        const VkResult result = devFns.calibrate(device, 1, &info, &stageTime, &deviation);
        const uint64_t after = nowNs();
        if (result != VK_SUCCESS || stageTime == 0 || after < before) continue;
        if (after - before < best) {
            best = after - before;
            offset = static_cast<int64_t>(stageTime) - static_cast<int64_t>(before + (after - before) / 2);
        }
    }
    if (best == UINT64_MAX) return false;
    calibrationWindowNs = best;
    return true;
}

void Probe::createFrameResources() {
    const uint32_t slots = static_cast<uint32_t>(views.size());

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    check(vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool), "vkCreateCommandPool");

    cmds.assign(slots, VK_NULL_HANDLE);
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = commandPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = slots;
    check(vkAllocateCommandBuffers(device, &alloc, cmds.data()), "vkAllocateCommandBuffers");

    // Fences start signalled so the first use of each slot does not wait.
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < slots; ++i) {
        VkFence fence = VK_NULL_HANDLE;
        check(vkCreateFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence");
        fences.push_back(fence);
        VkSemaphore acquire = VK_NULL_HANDLE;
        check(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &acquire), "vkCreateSemaphore");
        acquireSems.push_back(acquire);
        VkSemaphore render = VK_NULL_HANDLE;
        check(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &render), "vkCreateSemaphore");
        renderSems.push_back(render);
    }
}

void Probe::harvest() {
    // Partial results are not allowed: a timing is only returned once its stage is complete, so a stage
    // time cannot be consumed before it is known. Records are keyed by present id, so order does not matter.
    VkPastPresentationTimingInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT;
    info.flags = VK_PAST_PRESENTATION_TIMING_ALLOW_OUT_OF_ORDER_RESULTS_BIT_EXT;
    info.swapchain = swapchain;

    // The first call returns how many timings are waiting. The second copies them out.
    VkPastPresentationTimingPropertiesEXT props{};
    props.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT;
    const VkResult countResult = devFns.pastTiming(device, &info, &props);
    if (countResult != VK_SUCCESS) {
        if (!pastTimingWarned) {
            pastTimingWarned = true;
            std::fprintf(stderr, "framegen-vulkan: pt-probe: vkGetPastPresentationTimingEXT returned %d\n",
                         static_cast<int>(countResult));
        }
        return;
    }
    const uint32_t capacity = props.presentationTimingCount;
    if (capacity == 0) return;

    std::vector<VkPastPresentationTimingEXT> timings(capacity);
    std::vector<VkPresentStageTimeEXT> stages(static_cast<size_t>(capacity) * kStageSlots);
    for (uint32_t i = 0; i < capacity; ++i) {
        timings[i].sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT;
        timings[i].pPresentStages = &stages[static_cast<size_t>(i) * kStageSlots];
        timings[i].presentStageCount = kStageSlots;
    }
    props = {};
    props.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT;
    props.presentationTimingCount = capacity;
    props.pPresentationTimings = timings.data();
    if (devFns.pastTiming(device, &info, &props) != VK_SUCCESS) return;

    const uint32_t got = std::min(props.presentationTimingCount, capacity);
    for (uint32_t i = 0; i < got; ++i) {
        const VkPastPresentationTimingEXT& timing = timings[i];
        if (timing.presentId == 0 || timing.presentId > frames.size()) continue;
        FrameRecord& frame = frames[timing.presentId - 1];
        const uint32_t stageCount = std::min(timing.presentStageCount, kStageSlots);
        for (uint32_t s = 0; s < stageCount; ++s) {
            const VkPresentStageTimeEXT& stage = timing.pPresentStages[s];
            if ((stage.stage & presentStage) && frame.stageNs == 0 && stage.time != 0) {
                frame.stageNs = static_cast<uint64_t>(static_cast<int64_t>(stage.time) - stageOffset);
            }
        }
    }
}

void Probe::recordFrame(uint32_t slot, uint32_t image, uint64_t k) {
    const VkCommandBuffer cmd = cmds[slot];
    check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");

    VkImageMemoryBarrier toColor{};
    toColor.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toColor.srcAccessMask = 0;
    toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toColor.image = images[image];
    toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &toColor);

    VkRenderingAttachmentInfo attachment{};
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = views[image];
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    std::memcpy(attachment.clearValue.color.float32, kBackground, sizeof kBackground);
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    vkCmdBeginRendering(cmd, &rendering);

    // The bar moves two pixels per present, about 300 px/s at 150 Hz, and wraps at the right edge.
    const uint32_t barWidth = extent.width / 16;
    const uint32_t barHeight = extent.height / 48;
    const uint32_t barX = static_cast<uint32_t>((k * 2) % (extent.width - barWidth));
    const uint32_t barY = (extent.height - barHeight) / 2;
    VkClearAttachment clear{};
    clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clear.colorAttachment = 0;
    std::memcpy(clear.clearValue.color.float32, kBar, sizeof kBar);
    VkClearRect rect{};
    rect.rect.offset = {static_cast<int32_t>(barX), static_cast<int32_t>(barY)};
    rect.rect.extent = {barWidth, barHeight};
    rect.layerCount = 1;
    vkCmdClearAttachments(cmd, 1, &clear, 1, &rect);
    vkCmdEndRendering(cmd);

    VkImageMemoryBarrier toPresent = toColor;
    toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toPresent);
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
}

void Probe::presentFrame(uint64_t k, uint64_t t0) {
    FrameRecord& frame = frames[k];
    frame.targetNs = t0 + static_cast<uint64_t>(std::llround(static_cast<double>(k) * 1e9 / options.rateHz));
    uint64_t submitAt = frame.targetNs - leadNs;
    if (options.uneven) submitAt += jitterDist(rng);
    sleepUntil(submitAt);
    pump(0);
    harvest();

    const uint32_t slot = static_cast<uint32_t>(k % cmds.size());
    check(vkWaitForFences(device, 1, &fences[slot], VK_TRUE, kWaitTimeoutNs), "vkWaitForFences");
    check(vkResetFences(device, 1, &fences[slot]), "vkResetFences");
    uint32_t image = 0;
    const VkResult acquired = devFns.acquireNextImage(device, swapchain, kWaitTimeoutNs, acquireSems[slot],
                                                      VK_NULL_HANDLE, &image);
    if (acquired == VK_SUBOPTIMAL_KHR) {
        ++suboptimal;
    } else {
        check(acquired, "vkAcquireNextImageKHR");
    }

    recordFrame(slot, image, k);
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquireSems[slot];
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmds[slot];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderSems[image];
    check(vkQueueSubmit(queue, 1, &submit, fences[slot]), "vkQueueSubmit");
    frame.submitNs = nowNs();

    // The feedback request goes out before the present, so it attaches to this present's commit.
    frame.feedback = wp_presentation_feedback(presentation, surface);
    if (frame.feedback && wp_presentation_feedback_add_listener(frame.feedback, &kFeedbackListener, &frame) != 0) {
        wp_presentation_feedback_destroy(frame.feedback);
        frame.feedback = nullptr;
    }
    if (!frame.feedback) ++feedbackRequestFailures;
    wl_display_flush(display);

    if (options.noTarget) sleepUntil(frame.targetNs);
    const uint64_t presentId = k + 1;
    VkPresentTimingInfoEXT timing{};
    timing.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT;
    timing.targetTime = options.noTarget ? 0 : static_cast<uint64_t>(static_cast<int64_t>(frame.targetNs) + stageOffset);
    timing.timeDomainId = timeDomainId;
    timing.presentStageQueries = presentStage;
    timing.targetTimeDomainPresentStage = options.noTarget || (options.targetStageZero && !stageLocal) ? 0 : presentStage;
    VkPresentTimingsInfoEXT timings{};
    timings.sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT;
    timings.swapchainCount = 1;
    timings.pTimingInfos = &timing;
    VkPresentId2KHR ids{};
    ids.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR;
    ids.pNext = &timings;
    ids.swapchainCount = 1;
    ids.pPresentIds = &presentId;
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.pNext = &ids;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderSems[image];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &image;

    frame.callNs = nowNs();
    const VkResult presentResult = devFns.queuePresent(queue, &present);
    if (presentResult == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT) {
        ++timingQueueFull;
    } else if (presentResult == VK_SUBOPTIMAL_KHR) {
        ++suboptimal;
    } else {
        check(presentResult, "vkQueuePresentKHR");
    }
}

// The compositor can invalidate the first swapchain while the window settles (fullscreen, output move).
// Present plain frames for about half a second, recreating the swapchain when it goes out of date, so the
// measured loop starts on a stable swapchain. These presents carry no id or timing and are not logged.
void Probe::warmUp() {
    for (int i = 0; i < 50; ++i) {
        pump(10);
        const uint32_t slot = 0;
        check(vkWaitForFences(device, 1, &fences[slot], VK_TRUE, kWaitTimeoutNs), "vkWaitForFences");
        uint32_t image = 0;
        VkResult result = devFns.acquireNextImage(device, swapchain, kWaitTimeoutNs, acquireSems[slot], VK_NULL_HANDLE,
                                                  &image);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            check(vkResetFences(device, 1, &fences[slot]), "vkResetFences");
            recordFrame(slot, image, 0);
            VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &acquireSems[slot];
            submit.pWaitDstStageMask = &waitStage;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmds[slot];
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &renderSems[image];
            check(vkQueueSubmit(queue, 1, &submit, fences[slot]), "vkQueueSubmit");
            VkPresentInfoKHR present{};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &renderSems[image];
            present.swapchainCount = 1;
            present.pSwapchains = &swapchain;
            present.pImageIndices = &image;
            result = devFns.queuePresent(queue, &present);
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
            for (VkImageView view : views) vkDestroyImageView(device, view, nullptr);
            views.clear();
            images.clear();
            devFns.destroySwapchain(device, swapchain, nullptr);
            swapchain = VK_NULL_HANDLE;
            stageLocal = false;
            createSwapchain();
            ++swapchainRecreations;
            i = 0;
            if (swapchainRecreations > 8) fail("the swapchain keeps going out of date during warm-up");
        } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            check(result, "warm-up present");
        }
    }
    check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
}

bool Probe::allReported() const {
    for (const FrameRecord& frame : frames) {
        if (frame.callNs == 0) continue;
        if (frame.feedback || frame.stageNs == 0) return false;
    }
    return true;
}

void Probe::drain() {
    const uint64_t deadline = nowNs() + kDrainTimeoutNs;
    while (nowNs() < deadline && !allReported()) {
        pump(10);
        harvest();
    }
}

const char* Probe::stageName() const {
    return presentStage == VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT ? "IMAGE_FIRST_PIXEL_OUT"
                                                                           : "QUEUE_OPERATIONS_END";
}

void Probe::writeLog() {
    uint64_t presents = 0;
    uint64_t presented = 0;
    uint64_t staged = 0;
    uint64_t discarded = 0;
    for (const FrameRecord& frame : frames) {
        if (frame.callNs == 0) continue;
        ++presents;
        if (frame.presentedNs) ++presented;
        if (frame.stageNs) ++staged;
        if (frame.discarded) ++discarded;
    }
    const std::string path = "/tmp/fgvk-pt-probe-" + std::to_string(static_cast<unsigned long>(getuid())) + "-" +
                             std::to_string(static_cast<unsigned long>(getpid())) + ".log";
    FILE* out = std::fopen(path.c_str(), "w");
    if (!out) {
        std::fprintf(stderr, "framegen-vulkan: pt-probe: cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(out,
                 "framegen-vulkan: pt-probe session mode=%s rate_hz=%.6g lead_ms=%.6g uneven=%d no_target=%d "
                 "seconds=%.6g presents=%" PRIu64 " presented=%" PRIu64 " staged=%" PRIu64 " discarded=%" PRIu64
                 " stage=%s time_domain=CLOCK_MONOTONIC time_domain_id=%" PRIu64 " wp_clock_id=%u"
                 " refresh_ns=%" PRIu64 " refresh_interval_ns=%" PRIu64 " extent=%ux%u images=%zu timing_queue=%u"
                 " seed=%" PRIu64 " timing_queue_full=%" PRIu64 " suboptimal=%" PRIu64
                 " feedback_request_failures=%" PRIu64 " implicit_layers=disabled stage_local=%d stage_offset_ns=%" PRId64
                 " stage_offset_end_ns=%" PRId64 " calibration_window_ns=%" PRIu64 " swapchain_recreations=%" PRIu64 "\n",
                 options.mode == VK_PRESENT_MODE_FIFO_KHR ? "fifo" : "immediate", options.rateHz, options.leadMs,
                 options.uneven ? 1 : 0, options.noTarget ? 1 : 0, options.seconds, presents, presented, staged,
                 discarded, stageName(), timeDomainId, clockId, refreshDuration, refreshInterval, extent.width,
                 extent.height, views.size(), kTimingQueueSize, seed, timingQueueFull, suboptimal,
                 feedbackRequestFailures, stageLocal ? 1 : 0, stageOffset, stageOffsetEnd, calibrationWindowNs, swapchainRecreations);
    for (uint64_t k = 0; k < frames.size(); ++k) {
        const FrameRecord& frame = frames[k];
        if (frame.callNs == 0) continue;
        std::fprintf(out,
                     "framegen-vulkan: pt-probe present_id=%" PRIu64 " target_ns=%" PRIu64 " submit_ns=%" PRIu64
                     " call_ns=%" PRIu64 " stage_ns=%" PRIu64 " presented_ns=%" PRIu64 " flags=%u\n",
                     k + 1, frame.targetNs, frame.submitNs, frame.callNs, frame.stageNs, frame.presentedNs,
                     frame.flags);
    }
    std::fclose(out);
    std::fprintf(stderr,
                 "framegen-vulkan: pt-probe: presents=%" PRIu64 " presented=%" PRIu64 " staged=%" PRIu64
                 " discarded=%" PRIu64 " timing_queue_full=%" PRIu64 " log=%s\n",
                 presents, presented, staged, discarded, timingQueueFull, path.c_str());
}

int Probe::run() {
    leadNs = static_cast<uint64_t>(std::llround(options.leadMs * 1e6));
    connectWayland();
    createWindow();
    initVulkan();
    createSwapchain();
    createFrameResources();

    warmUp();
    const uint64_t presentCount = static_cast<uint64_t>(std::llround(options.seconds * options.rateHz));
    frames.assign(presentCount, FrameRecord{});
    std::fprintf(stderr,
                 "framegen-vulkan: pt-probe: device=%s mode=%s stage=%s extent=%ux%u images=%zu presents=%" PRIu64
                 " wp_clock_id=%u\n",
                 deviceName.c_str(), options.mode == VK_PRESENT_MODE_FIFO_KHR ? "fifo" : "immediate", stageName(),
                 extent.width, extent.height, views.size(), presentCount, clockId);

    const uint64_t t0 = nowNs() + kStartDelayNs;
    try {
        for (uint64_t k = 0; k < presentCount; ++k) presentFrame(k, t0);
        drain();
        if (stageLocal) {
            stageOffsetEnd = stageOffset;
            calibrateStage(stageOffsetEnd);
        }
    } catch (...) {
        writeLog();
        throw;
    }
    check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
    writeLog();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    const int parsed = parseArgs(argc, argv, options);
    if (parsed == 1) {
        printUsage(stdout);
        return 0;
    }
    if (parsed != 0) {
        printUsage(stderr);
        return 2;
    }
    // Keep fgvk and the other implicit layers out of this process. The loader reads these variables when
    // it creates the instance, which happens later in run().
    setenv("VK_LOADER_LAYERS_DISABLE", "~implicit~", 1);
    unsetenv("VK_INSTANCE_LAYERS");
    unsetenv("VK_LOADER_LAYERS_ENABLE");
    try {
        Probe probe(options);
        return probe.run();
    } catch (const ProbeError& error) {
        std::fprintf(stderr, "framegen-vulkan: pt-probe: error: %s\n", error.message.c_str());
        return 1;
    }
}

/* SPDX-License-Identifier: GPL-3.0-or-later */

// Standalone present measurer: the implicit layer VK_LAYER_FGVK_measure. It records the game's own
// present path (CPU call time, the GPU time the game's frame finished, and the on-screen time from
// wp_presentation) without framegen-vulkan in the process. It links no fgvk code. The pin mirrors
// engine/fgvk-render/src/library_pin.cpp, and the feedback approach mirrors
// engine/fgvk-render/src/wayland_presentation_feedback.cpp.

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan_wayland.h>

#include "events.hpp"
#include "measure_log.hpp"
#include "slot_ring.hpp"
#include "timestamp.hpp"
#include "wayland_feedback.hpp"

#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// The loader resolves the three exported entry points by name; everything else stays hidden.
#define VK_LAYER_EXPORT __attribute__((visibility("default")))

namespace fgvk::measure {
namespace {

constexpr uint32_t kSlotCount = 32;
constexpr uint32_t kMaxWaits = 16;
constexpr uint32_t kMaxPresentSwapchains = 8;
constexpr uint64_t kSecondNs = 1'000'000'000;
constexpr uint64_t kDispatchIntervalNs = 50'000'000;

uint64_t nowNs() noexcept {
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC, &value);
    return static_cast<uint64_t>(value.tv_sec) * kSecondNs + static_cast<uint64_t>(value.tv_nsec);
}

// The loader puts a dispatch table pointer at the start of every dispatchable object. Instances,
// physical devices, devices and queues are keyed by it.
template <class Handle>
void* dispatchKey(Handle handle) noexcept {
    return *reinterpret_cast<void* const*>(handle);
}

enum class Level : uint8_t { Global, Instance, Device };

#define FGVK_MEASURE_INSTANCE_FUNCTIONS(X) \
    X(DestroyInstance) \
    X(CreateDevice) \
    X(EnumerateDeviceExtensionProperties) \
    X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceQueueFamilyProperties) \
    X(GetPhysicalDeviceCalibrateableTimeDomainsKHR) \
    X(GetPhysicalDeviceCalibrateableTimeDomainsEXT) \
    X(CreateWaylandSurfaceKHR) \
    X(DestroySurfaceKHR)

#define FGVK_MEASURE_DEVICE_FUNCTIONS(X) \
    X(DestroyDevice) \
    X(GetDeviceQueue) \
    X(GetDeviceQueue2) \
    X(CreateSwapchainKHR) \
    X(DestroySwapchainKHR) \
    X(GetSwapchainImagesKHR) \
    X(QueuePresentKHR) \
    X(CreateCommandPool) \
    X(DestroyCommandPool) \
    X(AllocateCommandBuffers) \
    X(BeginCommandBuffer) \
    X(EndCommandBuffer) \
    X(CmdResetQueryPool) \
    X(CmdWriteTimestamp) \
    X(CreateQueryPool) \
    X(DestroyQueryPool) \
    X(CreateFence) \
    X(DestroyFence) \
    X(GetFenceStatus) \
    X(ResetFences) \
    X(QueueSubmit) \
    X(GetQueryPoolResults) \
    X(CreateSemaphore) \
    X(DestroySemaphore)

// The next layer's entry points, resolved once when the object is created.
struct InstanceDispatch {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr{};
#define FGVK_MEASURE_DECLARE(name) PFN_vk##name name{};
    FGVK_MEASURE_INSTANCE_FUNCTIONS(FGVK_MEASURE_DECLARE)
#undef FGVK_MEASURE_DECLARE

    void load(PFN_vkGetInstanceProcAddr next, VkInstance instance) noexcept {
        GetInstanceProcAddr = next;
#define FGVK_MEASURE_LOAD(name) name = reinterpret_cast<PFN_vk##name>(next(instance, "vk" #name));
        FGVK_MEASURE_INSTANCE_FUNCTIONS(FGVK_MEASURE_LOAD)
#undef FGVK_MEASURE_LOAD
    }
};

struct DeviceDispatch {
#define FGVK_MEASURE_DECLARE(name) PFN_vk##name name{};
    FGVK_MEASURE_DEVICE_FUNCTIONS(FGVK_MEASURE_DECLARE)
#undef FGVK_MEASURE_DECLARE

    void load(PFN_vkGetDeviceProcAddr next, VkDevice device) noexcept {
#define FGVK_MEASURE_LOAD(name) name = reinterpret_cast<PFN_vk##name>(next(device, "vk" #name));
        FGVK_MEASURE_DEVICE_FUNCTIONS(FGVK_MEASURE_LOAD)
#undef FGVK_MEASURE_LOAD
    }
};

// One submitted timestamp pair: query 0 is TOP_OF_PIPE, query 1 is BOTTOM_OF_PIPE. The command buffer
// is recorded once and re-submitted for each use.
struct Slot {
    VkCommandBuffer commandBuffer{};
    VkQueryPool queryPool{};
    VkFence fence{};
};

// The 32-slot GPU end-point ring of one queue family on one device.
struct Ring {
    uint32_t family{};
    std::atomic<bool> broken{};
    VkCommandPool pool{};
    std::array<Slot, kSlotCount> slots{};
    SlotRing<kSlotCount> states{};
};

struct DeviceState {
    VkDevice device{};
    PFN_vkGetDeviceProcAddr getDeviceProcAddr{};
    DeviceDispatch next{};
    std::string deviceName;
    // The calibration extension is enabled, CLOCK_MONOTONIC and DEVICE domains are available, and
    // the entry point resolved.
    bool calibrated{};
    PFN_vkGetCalibratedTimestampsKHR getCalibrated{};
    double timestampPeriodNs{};
    std::vector<uint32_t> validBits; // Per queue family.
    Calibration calibration{};
    bool hasCalibration{};
    uint64_t calibrationAtNs{};
    std::mutex ringsMutex; // Guards rings and graveyard.
    std::vector<std::unique_ptr<Ring>> rings;
    std::vector<VkSemaphore> graveyard; // Swapchain semaphores retired until the device goes away.

    bool gpuTimingFor(uint32_t family) const noexcept {
        return calibrated && family < validBits.size() && validBits[family] != 0;
    }
};

struct InstanceState {
    InstanceDispatch next{};
};

struct QueueState {
    void* deviceKey{};
    uint32_t family{};
};

struct SurfaceState {
    wl_display* display{};
    wl_surface* surface{};
};

struct DisplayState {
    std::shared_ptr<WaylandFeedback> feedback;
    uint32_t surfaces{};
};

struct SwapchainState {
    uint64_t id{};
    uint32_t imageCount{};
    VkSurfaceKHR surface{};
    wl_surface* wlSurface{}; // Set when the swapchain's surface is a Wayland surface.
    std::shared_ptr<WaylandFeedback> feedback;
    std::vector<VkSemaphore> semaphores; // One per image index. Empty when GPU timing is off.
    std::vector<uint8_t> poisoned;       // Image indices whose semaphore is no longer trusted.
};

struct Registry {
    std::shared_mutex mutex;
    std::unordered_map<void*, std::unique_ptr<InstanceState>> instances;
    std::unordered_map<void*, std::unique_ptr<DeviceState>> devices;
    std::unordered_map<VkQueue, QueueState> queues;
    std::unordered_map<VkSurfaceKHR, SurfaceState> surfaces;
    std::unordered_map<wl_display*, DisplayState> displays;
    std::unordered_map<VkSwapchainKHR, std::shared_ptr<SwapchainState>> swapchains;
};

Registry& registry() noexcept {
    static Registry* value = new Registry();
    return *value;
}

// Never destroyed: the reader thread and atexit may run after static destruction would begin.
MeasureLog& measureLog() noexcept {
    static MeasureLog* value = new MeasureLog();
    return *value;
}

std::atomic<bool> gMeasuring{false};
std::atomic<bool> gSessionWritten{false};
std::atomic<uint64_t> gNextSeq{0};
std::atomic<uint64_t> gNextSwapchainId{0};
std::once_flag gStartFlag;

void reportError(const char* text) noexcept {
    std::fprintf(stderr, "framegen-vulkan: measure: %s\n", text);
}

InstanceState* findInstance(void* key) {
    std::shared_lock lock(registry().mutex);
    const auto it = registry().instances.find(key);
    return it == registry().instances.end() ? nullptr : it->second.get();
}

DeviceState* findDevice(void* key) {
    std::shared_lock lock(registry().mutex);
    const auto it = registry().devices.find(key);
    return it == registry().devices.end() ? nullptr : it->second.get();
}

// A physical device is keyed by its instance's dispatch table. If that lookup ever misses while a
// single instance is known, that instance is the only possible owner.
InstanceState* findInstanceForPhysical(VkPhysicalDevice physical) {
    std::shared_lock lock(registry().mutex);
    const auto it = registry().instances.find(dispatchKey(physical));
    if (it != registry().instances.end()) return it->second.get();
    return registry().instances.size() == 1 ? registry().instances.begin()->second.get() : nullptr;
}

// Keeps this DSO mapped after the loader closes the layer. The reader thread outlives the instance
// and runs this object's code, so an unmapped DSO would crash on its next wake-up.
__attribute__((noinline)) void pinAnchor() noexcept {}

bool pinLayerLibrary() noexcept {
    Dl_info info{};
    void* extra = nullptr;
    if (dladdr1(reinterpret_cast<void*>(&pinAnchor), &info, &extra, RTLD_DL_LINKMAP) == 0 || !extra) {
        reportError("layer pin failed: dladdr found no object");
        return false;
    }
    // The program image has an empty name and is never unloaded. It cannot be reopened by path.
    if (static_cast<const link_map*>(extra)->l_name[0] == '\0') return true;
    if (!dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE)) {
        const char* error = dlerror();
        std::fprintf(stderr, "framegen-vulkan: measure: layer pin failed: %s\n",
                error ? error : "dlopen returned no handle");
        return false;
    }
    return true;
}

// Reader thread: the only place that polls fences and reads queries, converts timestamps, and
// dispatches Wayland. Records are formatted and written here, never on the game thread.
void refreshCalibration(DeviceState& device, uint64_t now) {
    if (!device.getCalibrated) return;
    if (device.hasCalibration && now - device.calibrationAtNs < kSecondNs) return;
    device.calibrationAtNs = now;
    VkCalibratedTimestampInfoKHR infos[2]{};
    infos[0].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
    infos[0].timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
    infos[1].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
    infos[1].timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
    uint64_t stamps[2]{};
    uint64_t maxDeviation = 0;
    if (device.getCalibrated(device.device, 2, infos, stamps, &maxDeviation) != VK_SUCCESS) return;
    device.calibration = Calibration{stamps[0], stamps[1]};
    device.hasCalibration = true;
}

// Reads every submitted slot whose fence signalled. A slot is retired only after its fence was reset,
// so the presenting thread cannot reuse it earlier. `finalPass` runs at device teardown, after the
// application idled the device, and reports slots still unfinished as unavailable.
void pollRing(DeviceState& device, Ring& ring, bool finalPass) {
    for (uint32_t i = 0; i < kSlotCount; ++i) {
        if (ring.broken.load(std::memory_order_acquire)) return;
        if (!ring.states.submitted(i)) continue;
        Slot& slot = ring.slots[i];
        const VkResult status = device.next.GetFenceStatus(device.device, slot.fence);
        if (status == VK_NOT_READY && !finalPass) continue;
        Event event{};
        event.kind = EventKind::Gpu;
        event.seq = ring.states.seq(i);
        if (status != VK_SUCCESS) {
            // Unfinished at teardown, or the fence reports an error. An error means the ring is no
            // longer trusted, so it is never polled or claimed again.
            if (status != VK_NOT_READY) ring.broken.store(true, std::memory_order_release);
            measureLog().push(event);
            continue;
        }
        uint64_t values[2]{};
        const VkResult read = device.next.GetQueryPoolResults(device.device, slot.queryPool, 0, 2,
                sizeof(values), values, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (read == VK_SUCCESS && device.hasCalibration) {
            const TickDomain domain{device.validBits[ring.family], device.timestampPeriodNs};
            const auto start = ticksToHostNs(values[0], device.calibration, domain);
            const auto end = ticksToHostNs(values[1], device.calibration, domain);
            if (start && end) {
                event.available = 1;
                event.gpuStartNs = *start;
                event.gpuEndNs = *end;
            }
        }
        if (device.next.ResetFences(device.device, 1, &slot.fence) != VK_SUCCESS) {
            ring.broken.store(true, std::memory_order_release);
            measureLog().push(event);
            continue;
        }
        ring.states.retire(i);
        measureLog().push(event);
    }
}

void pollDevice(DeviceState& device, uint64_t now) {
    if (!device.calibrated) return;
    std::vector<Ring*> rings;
    {
        const std::lock_guard lock(device.ringsMutex);
        rings.reserve(device.rings.size());
        for (auto& ring : device.rings) rings.push_back(ring.get());
    }
    bool pending = false;
    for (Ring* ring : rings) {
        if (!ring->broken.load(std::memory_order_acquire) && ring->states.anySubmitted()) pending = true;
    }
    if (!pending) return;
    refreshCalibration(device, now);
    for (Ring* ring : rings) pollRing(device, *ring, false);
}

void readerMain() {
    pthread_setname_np(pthread_self(), "fgvk-measure");
    uint64_t lastDispatch = nowNs();
    uint64_t lastSummary = lastDispatch;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        try {
            const uint64_t now = nowNs();
            {
                std::shared_lock lock(registry().mutex);
                for (auto& entry : registry().devices) pollDevice(*entry.second, now);
                if (now - lastDispatch >= kDispatchIntervalNs) {
                    lastDispatch = now;
                    for (auto& entry : registry().displays) {
                        if (entry.second.feedback) entry.second.feedback->dispatch();
                    }
                }
            }
            measureLog().drain();
            if (now - lastSummary >= kSecondNs) {
                lastSummary = now;
                measureLog().writeSummary();
            }
        } catch (...) {
        }
    }
}

void flushAtExit() {
    measureLog().drain();
    measureLog().writeSummary(); // Short runs get a final summary too.
}

// Runs once, at the first vkCreateDevice. Without the pin the reader is not started and nothing is
// measured, so the layer passes every call through.
void startMeasurement() noexcept {
    try {
        if (!pinLayerLibrary()) return;
        if (!measureLog().open()) return;
        std::atexit(flushAtExit);
        std::thread(readerMain).detach();
        gMeasuring.store(true, std::memory_order_release);
    } catch (...) {
    }
}

// Creates the ring's pool, slots and recorded command buffers. Runs on the presenting thread, once
// per family, and never while the reader holds a pointer into the ring.
bool recordSlot(DeviceState& device, Slot& slot) {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (device.next.BeginCommandBuffer(slot.commandBuffer, &begin) != VK_SUCCESS) return false;
    device.next.CmdResetQueryPool(slot.commandBuffer, slot.queryPool, 0, 2);
    device.next.CmdWriteTimestamp(slot.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot.queryPool, 0);
    device.next.CmdWriteTimestamp(slot.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.queryPool, 1);
    return device.next.EndCommandBuffer(slot.commandBuffer) == VK_SUCCESS;
}

bool createRing(DeviceState& device, Ring& ring) {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = ring.family;
    if (device.next.CreateCommandPool(device.device, &poolInfo, nullptr, &ring.pool) != VK_SUCCESS) return false;
    std::array<VkCommandBuffer, kSlotCount> buffers{};
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = ring.pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kSlotCount;
    if (device.next.AllocateCommandBuffers(device.device, &allocInfo, buffers.data()) != VK_SUCCESS) return false;
    for (uint32_t i = 0; i < kSlotCount; ++i) {
        Slot& slot = ring.slots[i];
        slot.commandBuffer = buffers[i];
        VkQueryPoolCreateInfo queryInfo{};
        queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = 2;
        if (device.next.CreateQueryPool(device.device, &queryInfo, nullptr, &slot.queryPool) != VK_SUCCESS) return false;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (device.next.CreateFence(device.device, &fenceInfo, nullptr, &slot.fence) != VK_SUCCESS) return false;
        if (!recordSlot(device, slot)) return false;
    }
    return true;
}

void destroyRing(DeviceState& device, Ring& ring) {
    for (Slot& slot : ring.slots) {
        if (slot.fence) device.next.DestroyFence(device.device, slot.fence, nullptr);
        if (slot.queryPool) device.next.DestroyQueryPool(device.device, slot.queryPool, nullptr);
        slot = Slot{};
    }
    if (ring.pool) device.next.DestroyCommandPool(device.device, ring.pool, nullptr);
    ring.pool = VK_NULL_HANDLE;
}

// Presenting thread. Returns nullptr when the family cannot be measured.
Ring* ringFor(DeviceState& device, uint32_t family) {
    const std::lock_guard lock(device.ringsMutex);
    for (auto& ring : device.rings) {
        if (ring->family == family) return ring->broken.load(std::memory_order_acquire) ? nullptr : ring.get();
    }
    auto ring = std::make_unique<Ring>();
    ring->family = family;
    if (!createRing(device, *ring)) {
        destroyRing(device, *ring);
        ring->broken.store(true, std::memory_order_release);
    }
    Ring* raw = ring.get();
    device.rings.push_back(std::move(ring));
    return raw->broken.load(std::memory_order_acquire) ? nullptr : raw;
}

// Presenting thread: claims a slot and submits the marker. It waits the present's wait semaphores at
// ALL_COMMANDS and signals `signal`, which the present then waits on alone. Returns false if the
// present must go out unchanged.
bool submitMarker(DeviceState& device, uint32_t family, VkQueue queue, const VkPresentInfoKHR& info,
        VkSemaphore signal, uint64_t seq) {
    Ring* ring = ringFor(device, family);
    if (!ring) return false;
    const auto claimed = ring->states.claim();
    if (!claimed) return false;
    Slot& slot = ring->slots[*claimed];
    std::array<VkPipelineStageFlags, kMaxWaits> stages{};
    stages.fill(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = info.waitSemaphoreCount;
    submit.pWaitSemaphores = info.pWaitSemaphores;
    submit.pWaitDstStageMask = stages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &slot.commandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &signal;
    if (device.next.QueueSubmit(queue, 1, &submit, slot.fence) != VK_SUCCESS) {
        ring->states.cancel(*claimed);
        return false;
    }
    ring->states.publish(*claimed, seq);
    return true;
}

VkResult measuredPresent(DeviceState& device, VkQueue queue, const VkPresentInfoKHR& info) {
    const uint64_t callStartNs = nowNs();
    const uint32_t count = info.swapchainCount;
    std::array<std::shared_ptr<SwapchainState>, kMaxPresentSwapchains> swapchains{};
    std::array<uint64_t, kMaxPresentSwapchains> seqs{};
    std::array<uint8_t, kMaxPresentSwapchains> requested{};
    uint32_t family = 0;
    bool familyKnown = false;
    {
        std::shared_lock lock(registry().mutex);
        const auto queueIt = registry().queues.find(queue);
        if (queueIt != registry().queues.end()) {
            family = queueIt->second.family;
            familyKnown = true;
        }
        for (uint32_t k = 0; k < count; ++k) {
            const auto it = registry().swapchains.find(info.pSwapchains[k]);
            if (it != registry().swapchains.end()) swapchains[k] = it->second;
        }
    }
    for (uint32_t k = 0; k < count; ++k) {
        seqs[k] = gNextSeq.fetch_add(1, std::memory_order_relaxed) + 1;
        SwapchainState* state = swapchains[k].get();
        if (state && state->feedback && state->feedback->request(state->wlSurface, seqs[k])) {
            requested[k] = 1;
            measureLog().countFeedback();
        }
    }

    // The GPU marker needs one swapchain: the normal single-swapchain present.
    const bool gpuTiming = familyKnown && device.gpuTimingFor(family);
    SwapchainState* first = count == 1 ? swapchains[0].get() : nullptr;
    const uint32_t image = count ? info.pImageIndices[0] : 0;
    VkSemaphore marker = VK_NULL_HANDLE;
    bool measured = false;
    if (gpuTiming && first && image < first->semaphores.size() && first->poisoned[image] == 0 &&
            info.waitSemaphoreCount <= kMaxWaits) {
        marker = first->semaphores[image];
        measured = submitMarker(device, family, queue, info, marker, seqs[0]);
    }

    VkPresentInfoKHR presentInfo = info;
    if (measured) {
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &marker;
    }
    const VkResult result = device.next.QueuePresentKHR(queue, &presentInfo);
    const uint64_t callEndNs = nowNs();
    // A failed present may not have consumed the marker, so its image index is not measured again.
    if (measured && result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) first->poisoned[image] = 1;

    for (uint32_t k = 0; k < count; ++k) {
        if (swapchains[k] && swapchains[k]->feedback) swapchains[k]->feedback->dispatch();
    }
    for (uint32_t k = 0; k < count; ++k) {
        Event event{};
        event.kind = EventKind::Present;
        event.seq = seqs[k];
        event.swapchain = swapchains[k] ? swapchains[k]->id : 0;
        event.image = info.pImageIndices[k];
        event.waits = info.waitSemaphoreCount;
        event.callStartNs = callStartNs;
        event.callEndNs = callEndNs;
        event.result = result;
        event.measured = measured && k == 0 ? 1 : 0;
        event.gpuUnavailable = gpuTiming ? 0 : 1;
        measureLog().push(event);
    }
    return result;
}

// Calibration plan for one device. Only the first measured device gets a plan; the rest are created
// exactly as the application asked.
struct CalibrationPlan {
    bool enabled{};
    bool khr{};
    const char* appended{}; // Extension appended to the application's enabled list.
};

bool hasDeviceExtension(InstanceState& instance, VkPhysicalDevice physical, const char* name) {
    if (!instance.next.EnumerateDeviceExtensionProperties) return false;
    uint32_t count = 0;
    if (instance.next.EnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr) != VK_SUCCESS) return false;
    if (count == 0) return false;
    std::vector<VkExtensionProperties> properties(count);
    if (instance.next.EnumerateDeviceExtensionProperties(physical, nullptr, &count, properties.data()) != VK_SUCCESS) {
        return false;
    }
    for (const VkExtensionProperties& property : properties) {
        if (std::strcmp(property.extensionName, name) == 0) return true;
    }
    return false;
}

bool clockDomainsSupported(InstanceState& instance, VkPhysicalDevice physical, bool khr) {
    PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR query = nullptr;
    if (khr) {
        query = instance.next.GetPhysicalDeviceCalibrateableTimeDomainsKHR;
    } else {
        query = instance.next.GetPhysicalDeviceCalibrateableTimeDomainsEXT;
    }
    if (!query) return false;
    uint32_t count = 0;
    if (query(physical, &count, nullptr) != VK_SUCCESS || count == 0) return false;
    std::vector<VkTimeDomainKHR> domains(count);
    if (query(physical, &count, domains.data()) != VK_SUCCESS) return false;
    bool device = false;
    bool monotonic = false;
    for (const VkTimeDomainKHR domain : domains) {
        device = device || domain == VK_TIME_DOMAIN_DEVICE_KHR;
        monotonic = monotonic || domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
    }
    return device && monotonic;
}

CalibrationPlan planCalibration(InstanceState& instance, VkPhysicalDevice physical, const VkDeviceCreateInfo& info) {
    CalibrationPlan plan{};
    const bool offersKHR = hasDeviceExtension(instance, physical, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    const bool offersEXT = !offersKHR && hasDeviceExtension(instance, physical, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    if (!offersKHR && !offersEXT) return plan;
    plan.khr = offersKHR;
    const char* name = offersKHR ? VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME : VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    if (!clockDomainsSupported(instance, physical, offersKHR)) return plan;
    bool enabledByApp = false;
    for (uint32_t i = 0; i < info.enabledExtensionCount; ++i) {
        if (std::strcmp(info.ppEnabledExtensionNames[i], name) == 0) enabledByApp = true;
    }
    if (!enabledByApp) plan.appended = name;
    plan.enabled = true;
    return plan;
}

VkLayerInstanceCreateInfo* instanceLayerLink(const VkInstanceCreateInfo* info) noexcept {
    for (auto* node = static_cast<const VkBaseInStructure*>(info->pNext); node; node = node->pNext) {
        if (node->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) continue;
        auto* link = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<VkBaseInStructure*>(node));
        if (link->function == VK_LAYER_LINK_INFO) return link;
    }
    return nullptr;
}

VkLayerDeviceCreateInfo* deviceLayerLink(const VkDeviceCreateInfo* info) noexcept {
    for (auto* node = static_cast<const VkBaseInStructure*>(info->pNext); node; node = node->pNext) {
        if (node->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) continue;
        auto* link = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<VkBaseInStructure*>(node));
        if (link->function == VK_LAYER_LINK_INFO) return link;
    }
    return nullptr;
}

void trackSwapchain(DeviceState& device, VkSurfaceKHR surface, VkSwapchainKHR swapchain) {
    auto state = std::make_shared<SwapchainState>();
    state->id = gNextSwapchainId.fetch_add(1, std::memory_order_relaxed) + 1;
    state->surface = surface;
    if (device.next.GetSwapchainImagesKHR(device.device, swapchain, &state->imageCount, nullptr) != VK_SUCCESS) {
        state->imageCount = 0;
    }
    if (device.calibrated) {
        for (uint32_t i = 0; i < state->imageCount; ++i) {
            VkSemaphoreCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VkSemaphore semaphore = VK_NULL_HANDLE;
            if (device.next.CreateSemaphore(device.device, &info, nullptr, &semaphore) != VK_SUCCESS) {
                for (VkSemaphore created : state->semaphores) {
                    device.next.DestroySemaphore(device.device, created, nullptr);
                }
                state->semaphores.clear();
                break;
            }
            state->semaphores.push_back(semaphore);
        }
    }
    state->poisoned.assign(state->semaphores.size(), 0);
    {
        std::unique_lock lock(registry().mutex);
        const auto surfaceIt = registry().surfaces.find(surface);
        if (surfaceIt != registry().surfaces.end()) {
            wl_display* display = surfaceIt->second.display;
            state->wlSurface = surfaceIt->second.surface;
            DisplayState& entry = registry().displays[display];
            if (!entry.feedback) entry.feedback = WaylandFeedback::create(display, measureLog());
            state->feedback = entry.feedback;
        }
        registry().swapchains[swapchain] = state;
    }
}

void recordQueue(void* deviceKey, VkQueue queue, uint32_t family) {
    if (!queue) return;
    std::unique_lock lock(registry().mutex);
    registry().queues[queue] = QueueState{deviceKey, family};
}

// Device teardown, after the application idled the device. The device is already gone from the
// registry, so the reader cannot reach it.
void teardownDevice(DeviceState& device) {
    bool pending = false;
    for (auto& ring : device.rings) {
        if (!ring->broken.load(std::memory_order_acquire) && ring->states.anySubmitted()) pending = true;
    }
    if (pending && device.calibrated) {
        refreshCalibration(device, nowNs());
        for (auto& ring : device.rings) pollRing(device, *ring, true);
    }
    for (auto& ring : device.rings) destroyRing(device, *ring);
    device.rings.clear();
    for (VkSemaphore semaphore : device.graveyard) device.next.DestroySemaphore(device.device, semaphore, nullptr);
    device.graveyard.clear();
}

// ---- Layer entry points --------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL hookCreateInstance(const VkInstanceCreateInfo* createInfo,
        const VkAllocationCallbacks* allocator, VkInstance* instance) {
    VkLayerInstanceCreateInfo* link = instanceLayerLink(createInfo);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetInstanceProcAddr next = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(next(nullptr, "vkCreateInstance"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = create(createInfo, allocator, instance);
    if (result != VK_SUCCESS) return result;
    auto state = std::make_unique<InstanceState>();
    state->next.load(next, *instance);
    try {
        std::unique_lock lock(registry().mutex);
        registry().instances[dispatchKey(*instance)] = std::move(state);
    } catch (...) {
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL hookDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
    InstanceState* state = findInstance(dispatchKey(instance));
    if (!state) return;
    const PFN_vkDestroyInstance destroy = state->next.DestroyInstance;
    {
        std::unique_lock lock(registry().mutex);
        registry().instances.erase(dispatchKey(instance));
    }
    if (destroy) destroy(instance, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL hookCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* createInfo,
        const VkAllocationCallbacks* allocator, VkDevice* device) {
    InstanceState* instance = findInstanceForPhysical(physical);
    if (!instance || !instance->next.CreateDevice) return VK_ERROR_INITIALIZATION_FAILED;
    VkLayerDeviceCreateInfo* link = deviceLayerLink(createInfo);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const PFN_vkGetDeviceProcAddr nextGetDeviceProcAddr = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    if (!nextGetDeviceProcAddr) return VK_ERROR_INITIALIZATION_FAILED;

    std::call_once(gStartFlag, startMeasurement);
    const bool measuring = gMeasuring.load(std::memory_order_acquire);
    CalibrationPlan plan{};
    if (measuring) plan = planCalibration(*instance, physical, *createInfo);

    VkDeviceCreateInfo copy = *createInfo;
    std::vector<const char*> extensions;
    if (plan.appended) {
        extensions.assign(createInfo->ppEnabledExtensionNames,
                createInfo->ppEnabledExtensionNames + createInfo->enabledExtensionCount);
        extensions.push_back(plan.appended);
        copy.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        copy.ppEnabledExtensionNames = extensions.data();
    }
    const VkResult result = instance->next.CreateDevice(physical, plan.appended ? &copy : createInfo, allocator, device);
    if (result != VK_SUCCESS) return result;

    auto state = std::make_unique<DeviceState>();
    state->device = *device;
    state->getDeviceProcAddr = nextGetDeviceProcAddr;
    state->next.load(nextGetDeviceProcAddr, *device);
    VkPhysicalDeviceProperties properties{};
    instance->next.GetPhysicalDeviceProperties(physical, &properties);
    state->deviceName = properties.deviceName;
    state->timestampPeriodNs = static_cast<double>(properties.limits.timestampPeriod);
    uint32_t familyCount = 0;
    instance->next.GetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    if (familyCount) instance->next.GetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
    for (const VkQueueFamilyProperties& family : families) state->validBits.push_back(family.timestampValidBits);
    if (measuring && plan.enabled) {
        const char* name = plan.khr ? "vkGetCalibratedTimestampsKHR" : "vkGetCalibratedTimestampsEXT";
        state->getCalibrated = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(nextGetDeviceProcAddr(*device, name));
        state->calibrated = state->getCalibrated != nullptr;
    }
    if (measuring && !gSessionWritten.exchange(true)) {
        measureLog().setSession(static_cast<int64_t>(getpid()), state->deviceName, state->calibrated);
    }
    try {
        std::unique_lock lock(registry().mutex);
        registry().devices[dispatchKey(*device)] = std::move(state);
    } catch (...) {
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL hookDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    void* key = dispatchKey(device);
    std::unique_ptr<DeviceState> state;
    {
        std::unique_lock lock(registry().mutex);
        const auto it = registry().devices.find(key);
        if (it != registry().devices.end()) {
            state = std::move(it->second);
            registry().devices.erase(it);
        }
        for (auto queue = registry().queues.begin(); queue != registry().queues.end();) {
            queue = queue->second.deviceKey == key ? registry().queues.erase(queue) : std::next(queue);
        }
    }
    if (!state) return;
    teardownDevice(*state);
    measureLog().drain();
    measureLog().writeSummary();
    if (state->next.DestroyDevice) state->next.DestroyDevice(device, allocator);
}

VKAPI_ATTR void VKAPI_CALL hookGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    DeviceState* state = findDevice(dispatchKey(device));
    if (!state || !state->next.GetDeviceQueue) return;
    state->next.GetDeviceQueue(device, family, index, queue);
    recordQueue(dispatchKey(device), queue ? *queue : nullptr, family);
}

VKAPI_ATTR void VKAPI_CALL hookGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    DeviceState* state = findDevice(dispatchKey(device));
    if (!state || !state->next.GetDeviceQueue2 || !info) return;
    state->next.GetDeviceQueue2(device, info, queue);
    recordQueue(dispatchKey(device), queue ? *queue : nullptr, info->queueFamilyIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL hookCreateWaylandSurfaceKHR(VkInstance instance,
        const VkWaylandSurfaceCreateInfoKHR* createInfo, const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
    InstanceState* state = findInstance(dispatchKey(instance));
    if (!state || !state->next.CreateWaylandSurfaceKHR) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = state->next.CreateWaylandSurfaceKHR(instance, createInfo, allocator, surface);
    if (result == VK_SUCCESS && surface) {
        try {
            std::unique_lock lock(registry().mutex);
            registry().surfaces[*surface] = SurfaceState{createInfo->display, createInfo->surface};
            registry().displays[createInfo->display].surfaces += 1;
        } catch (...) {
        }
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL hookDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
        const VkAllocationCallbacks* allocator) {
    InstanceState* state = findInstance(dispatchKey(instance));
    if (!state || !state->next.DestroySurfaceKHR) return;
    state->next.DestroySurfaceKHR(instance, surface, allocator);
    std::shared_ptr<WaylandFeedback> stopped;
    {
        std::unique_lock lock(registry().mutex);
        const auto surfaceIt = registry().surfaces.find(surface);
        if (surfaceIt != registry().surfaces.end()) {
            const auto displayIt = registry().displays.find(surfaceIt->second.display);
            registry().surfaces.erase(surfaceIt);
            if (displayIt != registry().displays.end() && displayIt->second.surfaces > 0 &&
                    --displayIt->second.surfaces == 0) {
                stopped = std::move(displayIt->second.feedback);
                registry().displays.erase(displayIt);
            }
        }
    }
    if (stopped) stopped->stop();
}

VKAPI_ATTR VkResult VKAPI_CALL hookCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* createInfo,
        const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {
    DeviceState* state = findDevice(dispatchKey(device));
    if (!state || !state->next.CreateSwapchainKHR) return VK_ERROR_DEVICE_LOST;
    const VkResult result = state->next.CreateSwapchainKHR(device, createInfo, allocator, swapchain);
    if (result != VK_SUCCESS || !swapchain || !gMeasuring.load(std::memory_order_acquire)) return result;
    try {
        trackSwapchain(*state, createInfo->surface, *swapchain);
    } catch (...) {
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL hookDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
        const VkAllocationCallbacks* allocator) {
    DeviceState* state = findDevice(dispatchKey(device));
    if (!state || !state->next.DestroySwapchainKHR) return;
    state->next.DestroySwapchainKHR(device, swapchain, allocator);
    std::shared_ptr<SwapchainState> retired;
    {
        std::unique_lock lock(registry().mutex);
        const auto it = registry().swapchains.find(swapchain);
        if (it != registry().swapchains.end()) {
            retired = std::move(it->second);
            registry().swapchains.erase(it);
        }
    }
    if (retired && !retired->semaphores.empty()) {
        // The presentation engine may still hold these, so they wait for device teardown.
        const std::lock_guard lock(state->ringsMutex);
        state->graveyard.insert(state->graveyard.end(), retired->semaphores.begin(), retired->semaphores.end());
    }
}

// The device a queue came from, as recorded by vkGetDeviceQueue. The dispatch key is only the fallback
// for a queue the layer never saw.
DeviceState* deviceForQueue(VkQueue queue) {
    void* key = dispatchKey(queue);
    {
        std::shared_lock lock(registry().mutex);
        const auto it = registry().queues.find(queue);
        if (it != registry().queues.end()) key = it->second.deviceKey;
    }
    return findDevice(key);
}

VKAPI_ATTR VkResult VKAPI_CALL hookQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    DeviceState* device = deviceForQueue(queue);
    if (!device || !device->next.QueuePresentKHR) return VK_ERROR_DEVICE_LOST;
    if (!info || info->swapchainCount == 0 || info->swapchainCount > kMaxPresentSwapchains ||
            !gMeasuring.load(std::memory_order_acquire)) {
        return device->next.QueuePresentKHR(queue, info);
    }
    return measuredPresent(*device, queue, *info);
}

struct HookEntry {
    const char* name;
    PFN_vkVoidFunction function;
    Level level;
};

const HookEntry* findHook(const char* name) noexcept {
    static const HookEntry hooks[] = {
        {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(&hookCreateInstance), Level::Global},
        {"vkDestroyInstance", reinterpret_cast<PFN_vkVoidFunction>(&hookDestroyInstance), Level::Instance},
        {"vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(&hookCreateDevice), Level::Instance},
        {"vkCreateWaylandSurfaceKHR", reinterpret_cast<PFN_vkVoidFunction>(&hookCreateWaylandSurfaceKHR), Level::Instance},
        {"vkDestroySurfaceKHR", reinterpret_cast<PFN_vkVoidFunction>(&hookDestroySurfaceKHR), Level::Instance},
        {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(&hookDestroyDevice), Level::Device},
        {"vkGetDeviceQueue", reinterpret_cast<PFN_vkVoidFunction>(&hookGetDeviceQueue), Level::Device},
        {"vkGetDeviceQueue2", reinterpret_cast<PFN_vkVoidFunction>(&hookGetDeviceQueue2), Level::Device},
        {"vkCreateSwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(&hookCreateSwapchainKHR), Level::Device},
        {"vkDestroySwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(&hookDestroySwapchainKHR), Level::Device},
        {"vkQueuePresentKHR", reinterpret_cast<PFN_vkVoidFunction>(&hookQueuePresentKHR), Level::Device},
    };
    for (const HookEntry& hook : hooks) {
        if (std::strcmp(hook.name, name) == 0) return &hook;
    }
    return nullptr;
}

PFN_vkVoidFunction instanceProcAddr(VkInstance instance, const char* name) {
    if (!name) return nullptr;
    const HookEntry* hook = findHook(name);
    InstanceState* state = instance ? findInstance(dispatchKey(instance)) : nullptr;
    if (!hook) return state ? state->next.GetInstanceProcAddr(instance, name) : nullptr;
    if (hook->level == Level::Global) return hook->function;
    // Instance-level and device-level hooks exist only where the next layer provides the command.
    if (!state || !state->next.GetInstanceProcAddr(instance, name)) return nullptr;
    return hook->function;
}

PFN_vkVoidFunction deviceProcAddr(VkDevice device, const char* name) {
    if (!name) return nullptr;
    DeviceState* state = device ? findDevice(dispatchKey(device)) : nullptr;
    if (!state || !state->getDeviceProcAddr) return nullptr;
    const HookEntry* hook = findHook(name);
    if (!hook || hook->level != Level::Device) return state->getDeviceProcAddr(device, name);
    if (!state->getDeviceProcAddr(device, name)) return nullptr;
    return hook->function;
}

}  // namespace
}  // namespace fgvk::measure

extern "C" {

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    return fgvk::measure::instanceProcAddr(instance, name);
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name) {
    return fgvk::measure::deviceProcAddr(device, name);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* version) {
    if (!version || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (version->loaderLayerInterfaceVersion > CURRENT_LOADER_LAYER_INTERFACE_VERSION) {
        version->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;
    }
    version->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    version->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    version->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

}  // extern "C"

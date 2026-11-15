/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/deferred_present.hpp"
#include "swapchain/private_present_queue.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

using namespace fgvk::layer;
using namespace std::chrono_literals;
namespace {
void expect(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
template<class T> T handle(uintptr_t value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(value);
    else return static_cast<T>(value);
}
std::atomic_uint calls{}, active{}, maximum{};
VkResult lowerResult{VK_SUCCESS};
VkSwapchainKHR observedSwapchain{};
VkSemaphore observedWait{};
VkFence observedFence{};
uint32_t observedImage{};
uint64_t observedId{}, observedId2{};
VkPresentModeKHR observedMode{};
DeferredPresent::Clock::time_point observedAt{};
#if defined(VK_EXT_present_timing)
uint64_t observedTargetTime{};
uint32_t observedTimingNodes{};
#endif
// The identified ID of each lower present in presentation order (0 when it has none).
std::mutex presentLogMutex;
std::vector<uint64_t> presentLog;
bool presentedContains(uint64_t id) {
    const std::lock_guard lock(presentLogMutex);
    return std::find(presentLog.begin(), presentLog.end(), id) != presentLog.end();
}
std::vector<uint64_t> presentedIds() {
    const std::lock_guard lock(presentLogMutex);
    return presentLog;
}
void clearPresentLog() {
    const std::lock_guard lock(presentLogMutex);
    presentLog.clear();
}
// A driver wait that succeeds only once the worker has presented the ID, so a
// wait that returned before its own real would report VK_ERROR_UNKNOWN.
auto awaitedPresent(uint64_t id) {
    return [id](uint64_t) { return presentedContains(id) ? VK_SUCCESS : VK_ERROR_UNKNOWN; };
}
VkResult VKAPI_CALL present(VkQueue queue, const VkPresentInfoKHR* info) {
    expect(queue == handle<VkQueue>(7), "worker borrowed an application queue");
    const auto concurrent = ++active;
    auto previous = maximum.load();
    while (previous < concurrent && !maximum.compare_exchange_weak(previous, concurrent)) {}
    std::this_thread::sleep_for(100us);
    observedAt = DeferredPresent::Clock::now();
    observedSwapchain = info->pSwapchains[0];
    observedImage = info->pImageIndices[0];
    observedWait = info->pWaitSemaphores[0];
    observedFence = VK_NULL_HANDLE;
    observedId = observedId2 = 0;
    observedMode = VK_PRESENT_MODE_MAX_ENUM_KHR;
#if defined(VK_EXT_present_timing)
    observedTargetTime = 0;
    observedTimingNodes = 0;
#endif
    for (const void* chain = info->pNext; chain;) {
        VkStructureType type{}; std::memcpy(&type, chain, sizeof(type));
        if (type == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT) {
            VkSwapchainPresentFenceInfoEXT node{}; std::memcpy(&node, chain, sizeof(node));
            observedFence = node.pFences[0];
        } else if (type == VK_STRUCTURE_TYPE_PRESENT_ID_KHR) {
            VkPresentIdKHR node{}; std::memcpy(&node, chain, sizeof(node));
            observedId = node.pPresentIds[0];
#if defined(VK_KHR_present_id2)
        } else if (type == VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR) {
            VkPresentId2KHR node{}; std::memcpy(&node, chain, sizeof(node));
            observedId2 = node.pPresentIds[0];
#endif
        } else if (type == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT) {
            VkSwapchainPresentModeInfoEXT node{}; std::memcpy(&node, chain, sizeof(node));
            observedMode = node.pPresentModes[0];
#if defined(VK_EXT_present_timing)
        } else if (type == VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT) {
            VkPresentTimingsInfoEXT node{}; std::memcpy(&node, chain, sizeof(node));
            expect(node.swapchainCount == 1 && node.pTimingInfos, "timing request lost its array");
            observedTargetTime = node.pTimingInfos[0].targetTime;
            ++observedTimingNodes;
#endif
        } else expect(false, "unknown metadata reached worker");
        const void* next{};
        std::memcpy(&next, static_cast<const std::byte*>(chain) +
            offsetof(VkBaseInStructure, pNext), sizeof(next));
        chain = next;
    }
    {
        const std::lock_guard lock(presentLogMutex);
        presentLog.push_back(observedId ? observedId : observedId2);
    }
    ++calls;
    --active;
    return lowerResult;
}
void queueRequest() {
    // Room for one extra queue only; a larger family is covered at the end of this function.
    std::array families{VkQueueFamilyProperties{.queueCount = 3}};
    std::array priorities{0.25F, 0.75F};
    VkDeviceQueueCreateInfo requested{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 2, .pQueuePriorities = priorities.data(),
    };
    VkDeviceCreateInfo original{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &requested,
    };
    PrivatePresentQueueRequest reservation(original, families, 0);
    auto modified = original;
    reservation.apply(modified);
    expect(reservation.index == 2 && modified.pQueueCreateInfos[0].queueCount == 3,
        "did not reserve an unexposed queue index");
    expect(requested.queueCount == 2 && modified.pQueueCreateInfos[0].pQueuePriorities != priorities.data() &&
        modified.pQueueCreateInfos[0].pQueuePriorities[0] == 0.25F &&
        modified.pQueueCreateInfos[0].pQueuePriorities[2] == 0.75F,
        "reservation changed caller arrays or priorities");
    families[0].queueCount = 2;
    expect(!PrivatePresentQueueRequest(original, families, 0).index,
        "reserved beyond physical queue capacity");
    families[0].queueCount = 4;
    requested.flags = VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT;
    expect(!PrivatePresentQueueRequest(original, families, 0).index,
        "reserved a protected queue through ordinary GetDeviceQueue");
    requested.flags = 0;
    expect(!PrivatePresentQueueRequest(original, families, std::nullopt).index,
        "reserved a queue for an unprovisioned device");
    std::array split{requested, requested};
    split[1].flags = VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT;
    original.queueCreateInfoCount = 2;
    original.pQueueCreateInfos = split.data();
    expect(!PrivatePresentQueueRequest(original, families, 0).index,
        "ignored other queue requests in the same family");
    // A family with room for two extra queues still gets exactly one: the present
    // queue at index 2, and no second private queue is reserved.
    std::array roomyFamilies{VkQueueFamilyProperties{.queueCount = 4}};
    std::array roomyPriorities{0.25F, 0.75F};
    VkDeviceQueueCreateInfo roomyRequested{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 2, .pQueuePriorities = roomyPriorities.data(),
    };
    VkDeviceCreateInfo roomyOriginal{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &roomyRequested,
    };
    PrivatePresentQueueRequest roomyReservation(roomyOriginal, roomyFamilies, 0);
    auto roomyModified = roomyOriginal;
    roomyReservation.apply(roomyModified);
    expect(roomyReservation.index == 2 && roomyModified.pQueueCreateInfos[0].queueCount == 3,
        "room for two extra queues did not reserve exactly one");
    const float* roomyExtra = roomyModified.pQueueCreateInfos[0].pQueuePriorities;
    expect(roomyExtra[0] == 0.25F && roomyExtra[1] == 0.75F && roomyExtra[2] == 0.75F,
        "the single extra queue did not take the last requested priority");
}
std::mutex readinessMutex;
std::condition_variable readinessChanged;
bool readinessEntered{}, readinessReleased{};
VkResult readinessResult{VK_SUCCESS};
VkResult VKAPI_CALL waitReady(VkDevice device, const VkSemaphoreWaitInfo* info, uint64_t timeout) {
    expect(device == handle<VkDevice>(21) && info->semaphoreCount == 1 &&
        info->pSemaphores[0] == handle<VkSemaphore>(31) && info->pValues[0] == 17 && timeout == 50'000'000,
        "worker did not wait for the submitted backend timeline");
    std::unique_lock lock(readinessMutex);
    readinessEntered = true;
    readinessChanged.notify_one();
    expect(readinessChanged.wait_for(lock, 1s, [] { return readinessReleased; }),
        "producer could not run while worker waited for GPU readiness");
    return readinessResult;
}
void readinessDoesNotBlockProducer() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, present);
    VkSwapchainKHR swapchain = handle<VkSwapchainKHR>(11);
    VkSemaphore wait = handle<VkSemaphore>(12);
    uint32_t image{};
    VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    for (const auto result : {VK_SUCCESS, VK_TIMEOUT, VK_ERROR_DEVICE_LOST}) {
        {
            const std::lock_guard lock(readinessMutex);
            readinessEntered = readinessReleased = false;
            readinessResult = result;
        }
        const auto before = calls.load();
        expect(worker.beginBatch(2, {handle<VkDevice>(21), handle<VkSemaphore>(31), 17, waitReady}),
            "GPU-pending batch rejected");
        // An already expired first slot must move with GPU readiness, while
        // timeout/error paths return every image without timed output debt.
        const auto first = DeferredPresent::Clock::now() - 20ms;
        expect(worker.enqueue(info, first), "GPU-pending generated image rejected");
        ++image;
        expect(worker.enqueue(info, first + 5ms), "GPU-pending real image rejected");
        worker.closeBatch();
        {
            std::unique_lock lock(readinessMutex);
            expect(readinessChanged.wait_for(lock, 1s, [] { return readinessEntered; }),
                "worker did not observe GPU readiness");
            expect(!worker.finish(0) && calls == before,
                "pending GPU work escaped its readiness wait");
            readinessReleased = true;
        }
        readinessChanged.notify_one();
        const auto completion = worker.finish(UINT64_MAX);
        expect(completion && completion->count == 2 && calls == before + 2,
            "GPU readiness path dropped or duplicated an image");
        expect(completion->readinessMiss == (result != VK_SUCCESS) &&
                completion->result == (result < 0 ? result : VK_SUCCESS),
            "readiness timeout/error changed ordinary image-return semantics");
        if (result == VK_SUCCESS)
            expect(completion->readinessShift >= 20ms &&
                    completion->calls[1].started >= completion->calls[0].started + 4ms,
                "late GPU readiness compressed outputs into a catch-up burst");
    }
}

void boundedBatch() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    VkSwapchainKHR swapchain = handle<VkSwapchainKHR>(11);
    VkSemaphore wait = handle<VkSemaphore>(12);
    uint32_t image = 0;
    VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    DeferredPresent worker(queue, present);
    const auto before = calls.load();
    const auto start = DeferredPresent::Clock::now() + 2ms;
    expect(!worker.beginBatch(DeferredPresent::capacity + 1), "unbounded batch admitted");
    expect(worker.beginBatch(5), "bounded complete batch rejected");
    expect(!worker.beginBatch(1), "second source batch admitted");
    for (unsigned i = 0; i < 5; ++i) {
        image = i;
        expect(worker.enqueue(info, start + i * 2ms), "tail slot rejected");
    }
    expect(!worker.enqueue(info, start), "tail exceeded admitted capacity");
    expect(!worker.finish(0), "open batch completed before seal");
    worker.closeBatch();
    const auto completed = worker.finish(UINT64_MAX);
    expect(completed && completed->count == 5 && calls == before + 5 && observedImage == 4,
        "tail dropped or duplicated an acquired image");
    for (size_t i = 1; i < completed->count; ++i)
        expect(completed->calls[i].started >= completed->calls[i - 1].started + 1ms,
            "worker compressed an overdue tail into catch-up presents");
    expect(worker.beginBatch(2), "empty batch admission failed");
    worker.closeBatch();
    expect(worker.finish(0)->count == 0, "empty pressure drain blocked or invented output");
    expect(worker.beginBatch(2), "error batch admission failed");
    lowerResult = VK_ERROR_OUT_OF_DATE_KHR;
    expect(worker.enqueue(info, start) && worker.enqueue(info, start + 2ms), "error tail admission failed");
    worker.closeBatch();
    const auto failed = worker.finish(UINT64_MAX);
    expect(failed && failed->count == 2 && failed->result == VK_ERROR_OUT_OF_DATE_KHR,
        "tail error lost remaining image returns or actual error");
    lowerResult = VK_SUCCESS;

    // Model normal driver work, a late wake, and a long blocking call on
    // the same timeline. Only late starts shift phase; call duration must
    // not be charged a second time after every output interval.
    auto previousRequested = start;
    auto previousStarted = start;
    for (size_t i = 1; i <= 16; ++i) {
        const auto requested = start + i * 10ms;
        const auto finished = previousStarted + (i == 8 ? 15ms : 1ms);
        const auto next = DeferredPresent::nextOutputDeadline(
            requested, previousRequested, previousStarted);
        const auto actual = std::max(next, finished) + (i == 4 ? 7ms : 0ms);
        expect(actual >= previousStarted + 5ms,
            "late output replayed an expired request slot");
        if (i == 3) expect(actual == start + 30ms,
            "normal driver call cost accumulated into the output period");
        if (i == 5) expect(actual == start + 57ms,
            "late worker start lost the following output phase");
        if (i == 9) expect(actual == start + 102ms,
            "a blocking lower call added another full interval after returning");
        previousRequested = requested;
        previousStarted = actual;
    }
    previousRequested = previousStarted = start;
    for (size_t i = 1; i < 1000; ++i) {
        const auto requested = start + i * 10ms;
        previousStarted = DeferredPresent::nextOutputDeadline(
            requested, previousRequested, previousStarted) + 100us;
        expect(previousStarted == requested + 100us,
            "normal wake lateness ratcheted the worker's absolute period");
        previousRequested = requested;
    }
}
std::vector<uint32_t> orderedImages;
std::vector<VkQueue> orderedQueues;
const void* synchronousChain{};
VkResult VKAPI_CALL orderedPresent(VkQueue queue, const VkPresentInfoKHR* info) {
    orderedQueues.push_back(queue);
    orderedImages.push_back(info->pImageIndices[0]);
    if (info->pImageIndices[0] == 20)
        expect(info->pNext == synchronousChain && info->pResults,
            "synchronous fallback copied or lost caller metadata/results");
    return VK_SUCCESS;
}
void completePresentationOrder() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, orderedPresent);
    VkSwapchainKHR swapchain = handle<VkSwapchainKHR>(11);
    VkSemaphore wait = handle<VkSemaphore>(12);
    uint32_t image = 10;
    VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    const auto at = DeferredPresent::Clock::now();
    expect(worker.beginBatch(2), "ordered batch admission failed");
    expect(queue->presentNow(orderedPresent, info) == VK_SUCCESS, "first output failed");
    image = 11; expect(worker.enqueue(info, at + 1ms), "middle output rejected");
    image = 12; expect(worker.enqueue(info, at + 2ms), "real output rejected");
    worker.closeBatch();
    expect(worker.finish(UINT64_MAX)->count == 2, "ordered batch failed to drain");
    // Smooth Cadence off, native-only, partial/error and unsupported-chain
    // paths remain synchronous, without migrating to an application queue.
    VkPresentRegionsKHR damage{.sType = VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR};
    VkResult callerResult{};
    synchronousChain = &damage;
    image = 20; info.pNext = synchronousChain; info.pResults = &callerResult;
    expect(queue->presentNow(orderedPresent, info) == VK_SUCCESS, "native fallback failed");
    info.pNext = nullptr; info.pResults = nullptr;
    expect(worker.beginBatch(5), "next complete ready batch rejected");
    for (uint32_t i = 30; i <= 34; ++i) {
        image = i;
        expect(worker.enqueue(info, at + (i - 28) * 1ms), "next tail output rejected");
    }
    worker.closeBatch();
    expect(worker.finish(UINT64_MAX)->count == 5, "next complete batch failed to drain");
    expect(orderedImages == std::vector<uint32_t>{10, 11, 12, 20, 30, 31, 32, 33, 34},
        "generated/real/native/next-batch sequence changed");
    expect(std::all_of(orderedQueues.begin(), orderedQueues.end(), [&](VkQueue observed) {
        return observed == queue->queue;
    }), "CPU-ordered output sequence split across Vulkan queues");
}
void lifecycle() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    VkSwapchainKHR swapchain = handle<VkSwapchainKHR>(11);
    VkSemaphore wait = handle<VkSemaphore>(12);
    VkFence fence = handle<VkFence>(13);
    uint32_t image = 4;
    VkSwapchainPresentFenceInfoEXT fences{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
        .swapchainCount = 1, .pFences = &fence,
    };
    VkPresentInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = &fences,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image,
    };
    DeferredPresent worker(queue, present);
    std::unique_lock heldQueue(queue->mutex);
    const auto deadline = DeferredPresent::Clock::now() + 30ms;
    expect(worker.enqueue(info, deadline), "valid singleton was rejected");
    expect(!worker.enqueue(info, deadline), "allowed more than one retained image");
    expect(!worker.finish(0), "poll blocked or completed before deadline");
    expect(!worker.finish(100'000), "finite acquire timeout was ignored");
    bool acquired = false;
    expect(worker.pollAcquire([&] { acquired = true; return VK_SUCCESS; }) == VK_NOT_READY && !acquired,
        "acquire poll waited on or bypassed the private queue lock");
    heldQueue.unlock();
    expect(worker.pollAcquire([&] {
        expect(active == 0, "acquire overlapped lower presentation host access");
        acquired = true;
        return VK_SUCCESS;
    }) == VK_SUCCESS && acquired && !worker.finish(0),
        "spare-image acquisition waited for the retained output deadline");
    // The game may now change all caller storage while doing next-frame work.
    swapchain = handle<VkSwapchainKHR>(91);
    wait = handle<VkSemaphore>(92);
    fence = handle<VkFence>(93);
    image = 99;
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->result == VK_SUCCESS && observedAt >= deadline,
        "worker compressed a late interval or lost its actual result");
    expect(observedSwapchain == handle<VkSwapchainKHR>(11) && observedWait == handle<VkSemaphore>(12) &&
        observedFence == handle<VkFence>(13) && observedImage == 4,
        "worker retained caller pointers instead of copying values");
    expect(calls == 1 && worker.finish(0)->started == DeferredPresent::Clock::time_point{},
        "completion was presented or consumed twice");
    lowerResult = VK_ERROR_OUT_OF_DATE_KHR;
    expect(worker.enqueue(info, DeferredPresent::Clock::now()), "completed slot was not reusable");
    expect(worker.finish(UINT64_MAX)->result == VK_ERROR_OUT_OF_DATE_KHR,
        "asynchronous recreation error was discarded");
    lowerResult = VK_SUCCESS;
    VkPresentIdKHR unsupported{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
    info.pNext = &unsupported;
    expect(!worker.enqueue(info, deadline), "borrowed an unknown caller pNext chain");
    uint64_t id = 101;
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    VkPresentIdKHR ids{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .pNext = &fences, .swapchainCount = 1, .pPresentIds = &id};
    VkSwapchainPresentModeInfoEXT modes{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT,
        .pNext = &ids, .swapchainCount = 1, .pPresentModes = &mode};
    info.pNext = &modes;
    expect(DeferredPresent::supportsChain(info.pNext), "caller ID/fence/mode snapshot rejected");
    expect(worker.enqueue(info, DeferredPresent::Clock::now() + 2ms), "caller snapshot admission failed");
    id = 777; mode = VK_PRESENT_MODE_IMMEDIATE_KHR; fence = handle<VkFence>(94);
    ids.pNext = nullptr; modes.pNext = nullptr;
    expect(worker.finish(UINT64_MAX)->result == VK_SUCCESS && observedId == 101 &&
        observedMode == VK_PRESENT_MODE_FIFO_KHR && observedFence == handle<VkFence>(93),
        "worker borrowed caller IDs, modes, fences or chain links");
    ids.pNext = &ids;
    expect(!DeferredPresent::supportsChain(&ids), "duplicate/cyclic ID chain admitted");
    VkPresentRegionsKHR damage{.sType = VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR};
    modes.pNext = &damage;
    expect(!DeferredPresent::supportsChain(&modes), "unknown suffix admitted");
#if defined(VK_KHR_present_id2)
    VkPresentId2KHR ids2{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
        .swapchainCount = 1, .pPresentIds = &id};
    info.pNext = &ids2;
    expect(worker.enqueue(info, DeferredPresent::Clock::now() + 2ms), "ID2 snapshot rejected");
    id = 888;
    expect(worker.finish(UINT64_MAX)->result == VK_SUCCESS && observedId2 == 777,
        "worker lost or borrowed caller ID2");
#endif
#if defined(VK_KHR_present_id2)
    constexpr uint64_t latestId = 777;
#else
    constexpr uint64_t latestId = 101;
#endif
    expect(!worker.serialWaitObserved(), "enabled IDs alone marked a serialized producer");
    DeferredPresent::applicationWait(swapchain, latestId, 0);
    DeferredPresent::applicationWait(swapchain, 1, UINT64_MAX);
    std::thread asynchronous([&] { DeferredPresent::applicationWait(swapchain, latestId, UINT64_MAX); });
    asynchronous.join();
    expect(!worker.serialWaitObserved(), "poll, older ID or asynchronous wait disabled overlap");
    DeferredPresent::applicationWait(swapchain, latestId, UINT64_MAX);
    expect(worker.serialWaitObserved(), "serialized latest-ID completion was not detected");
    info.pNext = nullptr;
    VkResult callerResult{};
    info.pResults = &callerResult;
    expect(!worker.enqueue(info, deadline), "retained a caller result pointer");
    info.pResults = nullptr;
    const auto before = calls.load();
    {
        DeferredPresent retiring(queue, present);
        expect(retiring.enqueue(info, DeferredPresent::Clock::now() + 2ms), "teardown admission failed");
    }
    expect(calls == before + 1, "destruction lost a retained image or left a live worker");
    for (unsigned i = 0; i < 100; ++i) {
        expect(worker.enqueue(info, DeferredPresent::Clock::now()), "repeated handoff failed");
        expect(worker.finish(UINT64_MAX)->result == VK_SUCCESS, "repeated completion failed");
    }
    {
        DeferredPresent other(queue, present);
        expect(worker.enqueue(info, DeferredPresent::Clock::now()) &&
            other.enqueue(info, DeferredPresent::Clock::now()), "independent surfaces could not hand off");
        expect(worker.finish(UINT64_MAX)->result == VK_SUCCESS &&
            other.finish(UINT64_MAX)->result == VK_SUCCESS, "shared-queue completion failed");
    }
    expect(maximum == 1, "private queue was accessed concurrently");
}
}
void waitForRetainedId() {
    for (const bool id2 : {
            false,
#if defined(VK_KHR_present_id2)
            true,
#endif
        }) {
        auto queue = std::make_shared<DeferredPresentQueue>();
        queue->queue = handle<VkQueue>(7);
        const auto device = handle<VkDevice>(42);
        const auto swapchain = handle<VkSwapchainKHR>(43);
        const auto semaphore = handle<VkSemaphore>(44);
        const uint32_t image = 1;
        uint64_t id = 123;
        VkPresentIdKHR ids{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
            .swapchainCount = 1, .pPresentIds = &id};
#if defined(VK_KHR_present_id2)
        VkPresentId2KHR ids2{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
            .swapchainCount = 1, .pPresentIds = &id};
        const void* chain = id2 ? static_cast<void*>(&ids2) : &ids;
#else
        const void* chain = &ids;
#endif
        VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = chain,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &semaphore,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
        unsigned lowerWaits{};
        {
            DeferredPresent worker(queue, present, device);
            expect(worker.enqueue(info, DeferredPresent::Clock::now() + 30ms),
                "retained caller ID was not admitted");
            const auto lowerWait = [&](uint64_t) { ++lowerWaits; return VK_SUCCESS; };
            expect(DeferredPresent::waitForPresent(device, swapchain, id, 0, lowerWait) == VK_TIMEOUT && !lowerWaits,
                "poll forwarded a retained ID that the driver has not received");
            expect(DeferredPresent::waitForPresent(device, swapchain, id, 1'000'000, lowerWait) == VK_TIMEOUT && !lowerWaits,
                "finite wait exceeded the delivery budget or fabricated completion");
            VkResult result = VK_ERROR_UNKNOWN;
            std::thread waiter([&] {
                result = DeferredPresent::waitForPresent(device, swapchain, id, 200'000'000,
                    [&](uint64_t remaining) {
                        ++lowerWaits;
                        expect((id2 ? observedId2 : observedId) == id,
                            "wait2 on another thread reached the driver before the real ID's present returned");
                        expect(remaining > 0 && remaining < 200'000'000,
                            "handoff time was added to the application's finite wait budget");
                        return VK_TIMEOUT;
                    });
            });
            expect(worker.finish(UINT64_MAX).has_value(), "retained present did not complete");
            waiter.join();
            expect(result == VK_TIMEOUT && lowerWaits == 1,
                "lower presentation completion/result was replaced by worker submission");
            expect(DeferredPresent::waitForPresent(device, swapchain, id, UINT64_MAX,
                    [&](uint64_t remaining) {
                        expect(remaining == UINT64_MAX, "infinite driver wait became finite");
                        return VK_SUBOPTIMAL_KHR;
                    }) == VK_SUBOPTIMAL_KHR,
                "completed delivery did not preserve the driver's result");
            expect(worker.takeCompletionPacingWait().waited == 0ns,
                "a completed delivery attributed worker delay already spent before a successful wait");
            ++id;
            expect(worker.beginBatch(1, {.capOwned = true}) &&
                    worker.enqueue(info, DeferredPresent::Clock::now() + 10ms),
                "cap-owned delivery could not retain its real ID");
            worker.closeBatch();
            expect(DeferredPresent::waitForPresent(device, swapchain, id, UINT64_MAX, lowerWait) == VK_SUCCESS,
                "completion-paced source could not join its pending real ID");
            expect(worker.finish(UINT64_MAX).has_value(), "cap-owned delivery did not complete");
            const auto completedBeforePoll = DeferredPresent::Clock::now();
            expect(DeferredPresent::waitForPresent(device, swapchain, id, 0,
                    [](uint64_t) { return VK_SUCCESS; }) == VK_SUCCESS,
                "completed real-ID poll changed driver completion");
            const auto capWait = worker.takeCompletionPacingWait();
            expect(capWait.waited > 0ns && capWait.capOwned && worker.takeCompletionPacingWait().waited == 0ns,
                "completion-paced source did not consume the worker's measured wait exactly once");
            expect(capWait.finished <= completedBeforePoll,
                "repeated successful polls refreshed a stale source-completion timestamp");
            ++id;
            expect(worker.enqueue(info, DeferredPresent::Clock::now() + 2ms),
                "overlapped source ID was not admitted");
            worker.applicationAcquire();
            expect(DeferredPresent::waitForPresent(device, swapchain, id, UINT64_MAX, lowerWait) == VK_SUCCESS,
                "overlapped completion wait failed");
            expect(worker.finish(UINT64_MAX).has_value() && worker.takeCompletionPacingWait().waited == 0ns,
                "concurrent producer inherited a different thread's output delay");
            ++id;
            lowerResult = VK_ERROR_OUT_OF_DATE_KHR;
            expect(worker.enqueue(info, DeferredPresent::Clock::now() + 1ms),
                "error-path caller ID was not admitted");
            expect(DeferredPresent::waitForPresent(device, swapchain, id, UINT64_MAX, lowerWait) ==
                    VK_ERROR_OUT_OF_DATE_KHR && lowerWaits == 3,
                "rejected lower ID was forwarded to a driver wait or its error was lost");
            expect(worker.finish(UINT64_MAX).has_value(), "error-path retained present did not complete");
            lowerResult = VK_SUCCESS;
        }
        expect(DeferredPresent::waitForPresent(device, swapchain, id, 0,
                [&](uint64_t timeout) { expect(timeout == 0, "unmanaged poll timeout changed"); return VK_SUCCESS; }) == VK_SUCCESS,
            "destroyed worker left a stale caller-ID registry entry");
    }
}

namespace {
std::atomic_uint copyChecks{};
VkResult VKAPI_CALL copyStatus(VkDevice device, VkFence fence) {
    expect(device == handle<VkDevice>(3) && fence == handle<VkFence>(5),
        "copy observation borrowed the wrong device or fence");
    expect(active == 0 && observedImage == (copyChecks == 0 ? 2u : 4u),
        "copy observation ran before the corresponding lower present returned");
    return copyChecks++ == 0 ? VK_NOT_READY : VK_ERROR_DEVICE_LOST;
}
void observeCopiesWithoutChangingDelivery() {
    auto queue = std::make_shared<DeferredPresentQueue>(); queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, present);
    const auto swapchain = handle<VkSwapchainKHR>(8);
    const auto semaphore = handle<VkSemaphore>(9);
    uint32_t image = 2;
    const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &semaphore,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    const auto first = DeferredPresent::Clock::now() + 1ms;
    expect(worker.beginBatch(2, {.device = handle<VkDevice>(3),
        .copyFence = handle<VkFence>(5), .copyStatus = copyStatus}) &&
        worker.enqueue(info, first, true), "profiled batch was not admitted");
    image = 4;
    expect(worker.enqueue(info, first + 1ms), "profiled real image was not retained");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 2 && completion->result == VK_SUCCESS && copyChecks == 2 &&
        completion->calls[0].copyChecked && completion->calls[0].copyStatus == VK_NOT_READY &&
        completion->calls[1].copyChecked && completion->calls[1].copyStatus == VK_ERROR_DEVICE_LOST &&
        completion->calls[0].generated && !completion->calls[1].generated &&
        completion->calls[0].imageIndex == 2 && completion->calls[1].imageIndex == 4 &&
        completion->calls[0].scheduled == first &&
        completion->calls[0].copySampledAt >= completion->calls[0].finished &&
        completion->calls[1].copySampledAt >= completion->calls[1].finished,
        "nonblocking copy observation changed delivery or lost output identity/error evidence");
    expect(worker.enqueue(info, first + 2ms), "unobserved batch was not admitted");
    const auto unobserved = worker.finish(UINT64_MAX);
    expect(unobserved && !unobserved->calls[0].copyChecked && copyChecks == 2,
        "disabled copy profiling still queried the GPU or fabricated readiness");
}
struct ObserverState { std::atomic_uint calls{}; };
VkResult observePresent(void* owner, PFN_vkQueuePresentKHR lower, VkQueue queue,
        const VkPresentInfoKHR& info, uint64_t context, uint64_t source, bool generated, uint64_t* feedback) {
    auto& state = *static_cast<ObserverState*>(owner);
    expect(context == 123 && source == 456 && generated == (state.calls == 0),
        "retained observer lost the admitted context/source/output role");
    expect(feedback != nullptr, "retained observer lost the feedback out-parameter");
    ++state.calls;
    return lower(queue, &info);
}
void observerLifetimeAndForwarding() {
    auto queue = std::make_shared<DeferredPresentQueue>(); queue->queue = handle<VkQueue>(7);
    auto state = std::make_shared<ObserverState>();
    std::weak_ptr<ObserverState> weak = state;
    {
        DeferredPresent worker(queue, present);
        const auto swapchain = handle<VkSwapchainKHR>(8);
        const auto semaphore = handle<VkSemaphore>(9);
        uint32_t image = 2;
        const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &semaphore,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
        const auto deadline = DeferredPresent::Clock::now() + 1ms;
        expect(worker.beginBatch(2, {.observer = state, .observe = observePresent, .context = 123, .source = 456}) &&
            worker.enqueue(info, deadline, true), "observed worker batch was not admitted");
        state.reset();
        image = 4;
        expect(worker.enqueue(info, deadline + 1ms), "observed real output was not admitted");
        worker.closeBatch();
        const auto before = calls.load();
        const auto completion = worker.finish(UINT64_MAX);
        expect(completion && completion->count == 2 && completion->result == VK_SUCCESS &&
            !weak.expired() && weak.lock()->calls == 2 && calls >= before,
            "observer changed completion or lost its owned lifetime");
    }
    expect(weak.expired(), "destroyed worker retained observation state");
}
}
void releasedBatchBoundsAndExpedites() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    VkSwapchainKHR swapchain = handle<VkSwapchainKHR>(11);
    VkSemaphore wait = handle<VkSemaphore>(12);
    uint32_t image = 0;
    VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    DeferredPresent worker(queue, present);
    // No output is scheduled after the next expected source present.
    auto start = DeferredPresent::Clock::now();
    const auto limit = start + 40ms;
    expect(worker.beginBatch(2, {.latestOutput = limit, .expediteOnJoin = true}), "released batch rejected");
    expect(worker.enqueue(info, start + 5ms, true) && worker.enqueue(info, start + 2s),
        "released outputs rejected");
    worker.closeBatch();
    while (!worker.finish(0)) std::this_thread::sleep_for(1ms);
    expect(DeferredPresent::Clock::now() - start < 1s && observedAt >= limit,
        "released real output was scheduled past the next source present");
    // A blocking join from the next present drops the remaining spacing.
    start = DeferredPresent::Clock::now();
    expect(worker.beginBatch(2, {.expediteOnJoin = true}), "expedited batch rejected");
    expect(worker.enqueue(info, start + 2s, true) && worker.enqueue(info, start + 4s),
        "expedited outputs rejected");
    worker.closeBatch();
    expect(!worker.finish(0), "poll expedited a released batch");
    const auto joined = worker.finish(UINT64_MAX);
    expect(joined && joined->count == 2 && DeferredPresent::Clock::now() - start < 1s,
        "join slept through released output spacing");
    expect(joined->calls[1].started >= joined->calls[0].started, "expedite reordered outputs");
    // A tolerated join keeps spacing for its bound, then expedites.
    start = DeferredPresent::Clock::now();
    expect(worker.beginBatch(2, {.joinTolerance = 60ms, .expediteOnJoin = true}), "tolerant batch rejected");
    expect(worker.enqueue(info, start + 2s, true) && worker.enqueue(info, start + 4s),
        "tolerant outputs rejected");
    worker.closeBatch();
    const auto tolerated = worker.finish(UINT64_MAX);
    const auto toleratedFor = DeferredPresent::Clock::now() - start;
    expect(tolerated && tolerated->count == 2 && toleratedFor >= 60ms && toleratedFor < 1s,
        "tolerated join did not bound output spacing");
    // Ordinary batches keep their absolute output deadlines on join.
    start = DeferredPresent::Clock::now();
    expect(worker.beginBatch(1), "ordinary batch rejected");
    expect(worker.enqueue(info, start + 30ms), "ordinary output rejected");
    worker.closeBatch();
    expect(worker.finish(UINT64_MAX) && observedAt >= start + 30ms, "ordinary join compressed spacing");
}
std::vector<std::pair<uint32_t, DeferredPresent::Clock::time_point>> timedImages;
VkResult VKAPI_CALL timedPresent(VkQueue, const VkPresentInfoKHR* info) {
    timedImages.emplace_back(info->pImageIndices[0], DeferredPresent::Clock::now());
    return VK_SUCCESS;
}
void overlappingBatchRetainsOlder() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    VkSwapchainKHR swapchain = handle<VkSwapchainKHR>(11);
    VkSemaphore wait = handle<VkSemaphore>(12);
    uint32_t image = 0;
    VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    DeferredPresent worker(queue, timedPresent);
    timedImages.clear();
    const auto start = DeferredPresent::Clock::now();
    expect(worker.beginBatch(2, {.overlap = true}), "overlapping batch A rejected");
    expect(worker.enqueue(info, start), "overlapping A0 rejected");
    image = 1;
    expect(worker.enqueue(info, start + 30ms), "overlapping A1 rejected");
    worker.closeBatch();
    expect(worker.retainable(), "sealed overlapping batch was not retainable");
    const auto retainStarted = DeferredPresent::Clock::now();
    expect(worker.finish(UINT64_MAX, true, true).has_value(), "retained wait failed");
    expect(DeferredPresent::Clock::now() - retainStarted < 5ms,
        "retained wait waited for the older overlapping batch");
    expect(worker.beginBatch(2, {.overlap = true}), "overlapping batch B rejected");
    image = 2;
    expect(worker.enqueue(info, start + 10ms), "overlapping B0 rejected");
    image = 3;
    expect(worker.enqueue(info, start + 20ms), "overlapping B1 rejected");
    worker.closeBatch();
    const auto older = worker.finish(UINT64_MAX);
    expect(older.has_value() && older->count == 2 && older->calls[1].imageIndex == 1,
        "older overlapping batch was not completed first");
    expect(worker.hasCompletion(), "newer overlapping batch completion was lost");
    const auto newer = worker.finish(0);
    expect(newer.has_value() && newer->count == 2 && newer->calls[0].imageIndex == 2,
        "newer overlapping batch completion changed order");
    expect(timedImages.size() == 4 && timedImages[0].first == 0 && timedImages[1].first == 1 &&
            timedImages[2].first == 2 && timedImages[3].first == 3,
        "overlapping presentation order changed");
    expect(timedImages[2].second >= timedImages[1].second,
        "newer overlapping batch started before the older batch's last output");
}
void nonOverlappableDoesNotRetain() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    VkSwapchainKHR swapchain = handle<VkSwapchainKHR>(11);
    VkSemaphore wait = handle<VkSemaphore>(12);
    uint32_t image = 0;
    VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    DeferredPresent worker(queue, timedPresent);
    expect(worker.beginBatch(1), "ordinary batch rejected");
    expect(worker.enqueue(info, DeferredPresent::Clock::now() + 20ms), "ordinary output rejected");
    worker.closeBatch();
    expect(!worker.retainable(), "non-overlappable batch was retained");
    expect(!worker.beginBatch(1, {.overlap = true}), "overlapping batch admitted behind a non-overlappable batch");
    expect(!worker.beginBatch(1), "ordinary batch admitted behind a pending batch");
    expect(worker.finish(UINT64_MAX).has_value(), "ordinary batch did not complete");
}
void overlapAcquireDoesNotDrainFutureOutputs() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, timedPresent);
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto semaphore = handle<VkSemaphore>(12);
    uint32_t image{};
    const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &semaphore,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    timedImages.clear();
    const auto start = DeferredPresent::Clock::now();
    expect(worker.beginBatch(1, {.expediteOnJoin = true, .overlap = true}) &&
        worker.enqueue(info, start + 10ms), "overlap acquire first batch rejected");
    worker.closeBatch();
    ++image;
    expect(worker.beginBatch(1, {.expediteOnJoin = true, .overlap = true}) &&
        worker.enqueue(info, start + 1s), "overlap acquire future batch rejected");
    worker.closeBatch();
    unsigned polls{};
    {
        const std::lock_guard lock(queue->mutex);
        expect(worker.waitAcquire(0, [&] { ++polls; return VK_SUCCESS; }) == VK_NOT_READY && !polls,
            "timeout-zero acquire blocked on the queue lock or called the lower API concurrently");
    }
    expect(worker.waitAcquire(1'000'000, [] { return VK_NOT_READY; }) == VK_TIMEOUT,
        "finite overlap acquire did not exhaust its total budget");
    const auto result = worker.waitAcquire(UINT64_MAX, [&] {
        ++polls;
        return timedImages.empty() ? VK_NOT_READY : VK_SUBOPTIMAL_KHR;
    });
    expect(result == VK_SUBOPTIMAL_KHR && worker.pending(),
        "acquisition waited for the future batch or lost a suboptimal result");
    {
        const std::lock_guard lock(queue->mutex);
        expect(timedImages.size() == 1 && timedImages[0].second >= start + 10ms,
            "image pressure expedited the first output or drained future outputs");
    }
    expect(worker.waitAcquire(UINT64_MAX, [] { return VK_ERROR_OUT_OF_DATE_KHR; }) == VK_ERROR_OUT_OF_DATE_KHR,
        "overlap acquisition swallowed the lower acquire error");
    expect(worker.finish(UINT64_MAX).has_value(), "overlap acquisition teardown failed");
    while (worker.hasCompletion()) static_cast<void>(worker.finish(0));

    lowerResult = VK_ERROR_DEVICE_LOST;
    DeferredPresent failed(queue, present);
    expect(failed.beginBatch(1, {.overlap = true}) && failed.enqueue(info, DeferredPresent::Clock::now()),
        "asynchronous error batch rejected");
    failed.closeBatch();
    expect(failed.waitAcquire(UINT64_MAX, [] { return VK_NOT_READY; }) == VK_ERROR_DEVICE_LOST,
        "overlap acquire ignored an asynchronous present error");
    expect(failed.finish(UINT64_MAX)->result == VK_ERROR_DEVICE_LOST, "asynchronous error was consumed by acquire");
    lowerResult = VK_SUCCESS;
}
void overlapRingOfThree() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    VkSwapchainKHR swapchain = handle<VkSwapchainKHR>(11);
    VkSemaphore wait = handle<VkSemaphore>(12);
    uint32_t image = 0;
    VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    uint64_t id = 201;
    VkPresentIdKHR ids{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .swapchainCount = 1, .pPresentIds = &id};
    VkPresentInfoKHR identified = info;
    identified.pNext = &ids;
    DeferredPresent worker(queue, timedPresent);
    timedImages.clear();
    // Every output is due two seconds away, so only a join may present it early.
    const auto due = DeferredPresent::Clock::now() + 2s;
    image = 0;
    expect(worker.beginBatch(1, {.expediteOnJoin = true, .overlap = true}), "first ring batch rejected");
    expect(worker.enqueue(info, due), "first ring output rejected");
    worker.closeBatch();
    // The second and third batches carry identified outputs, which overlap the pending
    // batches ahead of them and register their own present IDs.
    expect(worker.beginBatch(1, {.expediteOnJoin = true, .overlap = true}), "second ring batch rejected");
    image = 1;
    expect(worker.enqueue(identified, due + 1ms), "identified image rejected behind a pending batch");
    worker.closeBatch();
    expect(worker.beginBatch(1, {.expediteOnJoin = true, .overlap = true}), "third ring batch rejected");
    uint64_t thirdId = 202;
    VkPresentIdKHR thirdIds{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .swapchainCount = 1, .pPresentIds = &thirdId};
    VkPresentInfoKHR thirdIdentified = info;
    thirdIdentified.pNext = &thirdIds;
    image = 2;
    expect(worker.enqueue(thirdIdentified, due + 2ms), "identified image rejected with two pending batches");
    worker.closeBatch();
    expect(!worker.beginBatch(1, {.expediteOnJoin = true, .overlap = true}),
        "fourth overlapped batch admitted beside three pending batches");
    expect(worker.pending() && timedImages.empty(), "ring batch presented before any join");
    // Retaining the newest two joins and expedites only the oldest pending batch.
    const auto retainStarted = DeferredPresent::Clock::now();
    const auto oldest = worker.finish(UINT64_MAX, true, true);
    expect(oldest && oldest->count == 1 && oldest->calls[0].imageIndex == 0 &&
            DeferredPresent::Clock::now() - retainStarted < 1s,
        "retained join did not complete the oldest batch");
    expect(timedImages.size() == 1 && timedImages[0].first == 0,
        "retained join presented a newer batch early");
    expect(!worker.finish(0) && worker.pending(), "retained join awaited a newer batch");
    // The remaining batches complete oldest first.
    const auto second = worker.finish(UINT64_MAX);
    expect(second && second->count == 1 && second->calls[0].imageIndex == 1,
        "second ring batch did not complete before the newest");
    const auto third = worker.finish(UINT64_MAX);
    expect(third && third->count == 1 && third->calls[0].imageIndex == 2,
        "third ring batch did not complete last");
    expect(!worker.hasCompletion() && !worker.pending(), "ring left a batch behind");
    expect(timedImages.size() == 3 && timedImages[0].first == 0 && timedImages[1].first == 1 &&
            timedImages[2].first == 2, "ring presentation order changed");
}
void overlapAcceptsPresentId() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    VkSwapchainKHR swapchain = handle<VkSwapchainKHR>(11);
    VkSemaphore wait = handle<VkSemaphore>(12);
    uint32_t image = 0;
    VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    DeferredPresent worker(queue, timedPresent);
    // Far deadlines keep the head pending until the join, so overlap is
    // deterministic; expediteOnJoin lets finish() release it at once.
    const auto start = DeferredPresent::Clock::now() + 1h;
    expect(worker.beginBatch(1, {.expediteOnJoin = true, .overlap = true}), "overlapping head batch rejected");
    expect(worker.enqueue(info, start), "overlapping head image rejected");
    worker.closeBatch();
    expect(worker.beginBatch(2, {.expediteOnJoin = true, .overlap = true}), "overlapping second batch rejected");
    uint64_t id = 101;
    VkPresentIdKHR ids{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .swapchainCount = 1, .pPresentIds = &id};
    VkPresentInfoKHR identified = info;
    identified.pNext = &ids;
    expect(worker.enqueue(identified, start + 1ms), "overlapping second batch rejected a present ID");
    expect(worker.enqueue(info, start + 1ms), "overlapping second batch rejected an unidentified image");
    worker.closeBatch();
    const auto older = worker.finish(UINT64_MAX);
    expect(older.has_value() && older->count == 1, "overlapping head batch did not complete");
    const auto newer = worker.finish(UINT64_MAX);
    expect(newer.has_value() && newer->count == 2, "overlapping second batch did not complete");
}
void overlapAcceptsOwnedPresentId() {
#if defined(VK_KHR_present_id2)
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    VkSwapchainKHR swapchain = handle<VkSwapchainKHR>(11);
    VkSemaphore wait = handle<VkSemaphore>(12);
    uint32_t image = 0;
    VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    uint64_t owned = 101;
    uint64_t foreign = 102;
    DeferredPresent worker(queue, present);
    worker.setOwnedPresentIdStorage(&owned);
    lowerResult = VK_SUCCESS;
    observedId2 = 0;
    const auto start = DeferredPresent::Clock::now();
    expect(worker.beginBatch(1, {.overlap = true}), "overlapping head batch rejected");
    expect(worker.enqueue(info, start), "overlapping head image rejected");
    worker.closeBatch();
    expect(worker.beginBatch(2, {.overlap = true}), "overlapping second batch rejected");
    VkPresentId2KHR foreignIds{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
        .swapchainCount = 1, .pPresentIds = &foreign};
    VkPresentInfoKHR foreignInfo = info;
    foreignInfo.pNext = &foreignIds;
    expect(worker.enqueue(foreignInfo, start + 1ms), "overlapping second batch rejected an application present ID");
    VkPresentId2KHR ownedIds{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
        .swapchainCount = 1, .pPresentIds = &owned};
    VkPresentInfoKHR ownedInfo = info;
    ownedInfo.pNext = &ownedIds;
    expect(worker.enqueue(ownedInfo, start + 1ms), "overlapping second batch rejected the owned present ID");
    worker.closeBatch();
    const auto older = worker.finish(UINT64_MAX);
    expect(older.has_value() && older->count == 1, "overlapping head batch did not complete");
    const auto newer = worker.finish(UINT64_MAX);
    expect(newer.has_value() && newer->count == 2, "overlapping owned-ID batch did not complete");
    expect(observedId2 == 101, "the lower present did not receive the owned present ID");
#endif
}
// Two overlapping identified batches. Each wait joins its own record: an undelivered
// record is not answered by the driver, an unregistered ID goes straight to it, and
// presentation follows batch order whatever order the waits come in.
void twoIdentifiedBatchesJoinTheirOwnRecords() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    const auto device = handle<VkDevice>(42);
    const auto swapchain = handle<VkSwapchainKHR>(43);
    VkSemaphore wait = handle<VkSemaphore>(44);
    const std::array<uint64_t, 2> ids{401, 402};
    std::array<uint32_t, 2> images{0, 1};
    std::array<VkPresentIdKHR, 2> nodes{};
    std::array<VkPresentInfoKHR, 2> infos{};
    for (size_t i = 0; i < ids.size(); ++i) {
        nodes[i] = VkPresentIdKHR{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
            .swapchainCount = 1, .pPresentIds = &ids[i]};
        infos[i] = VkPresentInfoKHR{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = &nodes[i],
            .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &images[i]};
    }
    DeferredPresent worker(queue, present, device);
    clearPresentLog();
    lowerResult = VK_SUCCESS;
    // Far deadlines: only a join presents a batch.
    const auto due = DeferredPresent::Clock::now() + 2s;
    expect(worker.beginBatch(1, {.expediteOnJoin = true, .overlap = true}) && worker.enqueue(infos[0], due),
        "first identified batch rejected");
    worker.closeBatch();
    expect(worker.beginBatch(1, {.expediteOnJoin = true, .overlap = true}) && worker.enqueue(infos[1], due),
        "overlapping identified batch rejected");
    worker.closeBatch();
    expect(worker.retainable() && presentedIds().empty(), "overlapping identified batches presented before a join");

    std::atomic_int driverCalls{};
    const auto driver = [&driverCalls](uint64_t) { ++driverCalls; return VK_SUCCESS; };
    expect(DeferredPresent::waitForPresent(device, swapchain, ids[1], 0, driver) == VK_TIMEOUT && driverCalls == 0,
        "a poll for an undelivered record reached the driver");
    expect(DeferredPresent::waitForPresent(device, swapchain, 999, 0, driver) == VK_SUCCESS && driverCalls == 1,
        "an ID no record names did not go straight to the driver");

    // The newer wait starts first; each returns only after its own real.
    VkResult newer = VK_ERROR_UNKNOWN, older = VK_ERROR_UNKNOWN;
    std::thread newerWaiter([&] {
        newer = DeferredPresent::waitForPresent(device, swapchain, ids[1], UINT64_MAX, awaitedPresent(ids[1]));
    });
    std::thread olderWaiter([&] {
        older = DeferredPresent::waitForPresent(device, swapchain, ids[0], UINT64_MAX, awaitedPresent(ids[0]));
    });
    const auto first = worker.finish(UINT64_MAX);
    const auto second = worker.finish(UINT64_MAX);
    newerWaiter.join();
    olderWaiter.join();
    expect(older == VK_SUCCESS && newer == VK_SUCCESS,
        "a joined wait returned before its own real was presented");
    expect(presentedIds() == std::vector<uint64_t>{ids[0], ids[1]},
        "overlapping identified batches were presented out of order");
    expect(first.has_value() && first->count == 1 && second.has_value() && second->count == 1,
        "overlapping identified batches did not complete");
}

// Three overlapping identified batches. Joining the oldest leaves the two newer records
// undelivered, so their waits block until a later join presents them. Each wait joins its
// own record, and the newest record keeps the completion pacing. Once the list is full, a
// later identified batch drops the oldest submitted record, and that ID then goes to the driver.
void threeIdentifiedBatchesJoinInAnyOrder() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    const auto device = handle<VkDevice>(42);
    const auto swapchain = handle<VkSwapchainKHR>(43);
    VkSemaphore wait = handle<VkSemaphore>(44);
    const std::array<uint64_t, 3> ids{411, 412, 413};
    std::array<uint32_t, 3> images{0, 1, 2};
    std::array<VkPresentIdKHR, 3> nodes{};
    std::array<VkPresentInfoKHR, 3> infos{};
    for (size_t i = 0; i < ids.size(); ++i) {
        nodes[i] = VkPresentIdKHR{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
            .swapchainCount = 1, .pPresentIds = &ids[i]};
        infos[i] = VkPresentInfoKHR{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = &nodes[i],
            .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &images[i]};
    }
    DeferredPresent worker(queue, present, device);
    clearPresentLog();
    lowerResult = VK_SUCCESS;
    const auto due = DeferredPresent::Clock::now() + 2s;
    for (const auto& info : infos) {
        expect(worker.beginBatch(1, {.expediteOnJoin = true, .overlap = true}) && worker.enqueue(info, due),
            "overlapping identified batch rejected");
        worker.closeBatch();
    }
    expect(!worker.beginBatch(1, {.expediteOnJoin = true, .overlap = true}),
        "a fourth batch was admitted beside three pending batches");

    // Retaining the newest two joins only the oldest batch.
    VkResult oldest = VK_ERROR_UNKNOWN;
    std::thread oldestWaiter([&] {
        oldest = DeferredPresent::waitForPresent(device, swapchain, ids[0], UINT64_MAX, awaitedPresent(ids[0]));
    });
    const auto joinedOldest = worker.finish(UINT64_MAX, true, true);
    oldestWaiter.join();
    expect(joinedOldest.has_value() && joinedOldest->count == 1 && joinedOldest->calls[0].imageIndex == 0,
        "retained join did not complete the oldest batch");
    expect(oldest == VK_SUCCESS, "the oldest wait did not return its own presentation");
    expect(presentedIds() == std::vector<uint64_t>{ids[0]}, "retained join presented a newer batch");
    // The oldest record is not the newest, so its wait leaves completion pacing alone.
    expect(worker.takeCompletionPacingWait().finished == DeferredPresent::Clock::time_point{},
        "a wait on an older record set the newest record's completion pacing");
    expect(DeferredPresent::waitForPresent(device, swapchain, ids[2], 0, [](uint64_t) { return VK_SUCCESS; }) == VK_TIMEOUT,
        "a poll for the newest undelivered record reached the driver");
    expect(DeferredPresent::waitForPresent(device, swapchain, ids[0], 0, [](uint64_t) { return VK_SUCCESS; }) == VK_SUCCESS,
        "a poll for the joined record did not reach the driver");

    // The two newer waits block until their joins; presentation then keeps batch order.
    VkResult middle = VK_ERROR_UNKNOWN, newest = VK_ERROR_UNKNOWN;
    std::thread middleWaiter([&] {
        middle = DeferredPresent::waitForPresent(device, swapchain, ids[1], UINT64_MAX, awaitedPresent(ids[1]));
    });
    std::thread newestWaiter([&] {
        newest = DeferredPresent::waitForPresent(device, swapchain, ids[2], UINT64_MAX, awaitedPresent(ids[2]));
    });
    std::this_thread::sleep_for(10ms);
    expect(presentedIds() == std::vector<uint64_t>{ids[0]}, "a blocked wait presented a batch");
    const auto joinedMiddle = worker.finish(UINT64_MAX);
    const auto joinedNewest = worker.finish(UINT64_MAX);
    middleWaiter.join();
    newestWaiter.join();
    expect(middle == VK_SUCCESS && newest == VK_SUCCESS,
        "a joined wait returned before its own real was presented");
    expect(presentedIds() == std::vector<uint64_t>{ids[0], ids[1], ids[2]},
        "three overlapping identified batches were presented out of order");
    expect(joinedMiddle.has_value() && joinedMiddle->calls[0].imageIndex == 1 &&
            joinedNewest.has_value() && joinedNewest->calls[0].imageIndex == 2,
        "the remaining batches did not complete in order");
    expect(worker.takeCompletionPacingWait().finished != DeferredPresent::Clock::time_point{},
        "the newest record's wait did not set completion pacing");

    // All three are delivered and joined, so a fourth identified batch drops the oldest one.
    const std::array<uint64_t, 1> fourth{414};
    VkPresentIdKHR fourthNode{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .swapchainCount = 1, .pPresentIds = &fourth[0]};
    VkPresentInfoKHR fourthInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = &fourthNode,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &images[0]};
    expect(worker.beginBatch(1, {.overlap = true}) && worker.enqueue(fourthInfo, DeferredPresent::Clock::now()),
        "an identified batch was refused after the earlier records were delivered");
    worker.closeBatch();
    expect(DeferredPresent::waitForPresent(device, swapchain, ids[0], 0, [](uint64_t) { return VK_SUCCESS; }) == VK_SUCCESS,
        "a dropped record did not go straight to the driver");
    expect(worker.finish(UINT64_MAX).has_value(), "the later identified batch did not complete");
}

// The timing request of a VKD3D-Proton present is copied at admission and rebuilt on the real.
void timingRequestIsCopiedAndRebuilt() {
#if defined(VK_EXT_present_timing)
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    const auto device = handle<VkDevice>(42);
    const auto swapchain = handle<VkSwapchainKHR>(43);
    VkSemaphore wait = handle<VkSemaphore>(44);
    uint32_t image = 0;
    uint64_t id = 421;
    VkPresentIdKHR ids{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .swapchainCount = 1, .pPresentIds = &id};
    VkPresentTimingInfoEXT timing{.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT, .targetTime = 12345};
    VkPresentTimingsInfoEXT timings{.sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT,
        .pNext = &ids, .swapchainCount = 1, .pTimingInfos = &timing};
    VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = &timings,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    expect(DeferredPresent::supportsChain(&timings), "a VKD3D-Proton timing request was not accepted");

    // An inner pNext of its own, or a second swapchain, keeps the present synchronous.
    VkPresentTimingInfoEXT chained = timing;
    chained.pNext = &ids;
    VkPresentTimingsInfoEXT chainedTimings = timings;
    chainedTimings.pTimingInfos = &chained;
    expect(!DeferredPresent::supportsChain(&chainedTimings), "a timing info with its own pNext was accepted");
    VkPresentTimingsInfoEXT twoSwapchains = timings;
    twoSwapchains.swapchainCount = 2;
    expect(!DeferredPresent::supportsChain(&twoSwapchains), "a timing request for two swapchains was accepted");

    DeferredPresent worker(queue, present, device);
    clearPresentLog();
    lowerResult = VK_SUCCESS;
    expect(worker.beginBatch(1) && worker.enqueue(info, DeferredPresent::Clock::now() + 20ms),
        "a timing request was not admitted to the worker");
    // The worker keeps its own copy: the application's struct may change after admission.
    timing.targetTime = 999;
    worker.closeBatch();
    expect(worker.finish(UINT64_MAX).has_value(), "the timed batch did not complete");
    expect(observedTimingNodes == 1 && observedTargetTime == 12345,
        "the real present did not carry the copied timing request");
#endif
}

// A VKD3D-Proton chain of six known nodes, each kind once, is still admitted; a seventh is not.
void sixKnownNodesAreAccepted() {
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2) && defined(VK_NV_present_metering)
    uint64_t id = 431, id2 = 432;
    VkFence fenceHandle = handle<VkFence>(0x51);
    VkPresentModeKHR modeValue = VK_PRESENT_MODE_FIFO_KHR;
    VkPresentTimingInfoEXT timing{.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT};
    VkPresentTimingsInfoEXT timings{.sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT,
        .swapchainCount = 1, .pTimingInfos = &timing};
    VkSwapchainPresentModeInfoEXT mode{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT,
        .swapchainCount = 1, .pPresentModes = &modeValue};
    VkPresentId2KHR ids2{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR, .swapchainCount = 1, .pPresentIds = &id2};
    VkPresentIdKHR ids{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .swapchainCount = 1, .pPresentIds = &id};
    VkSwapchainPresentFenceInfoEXT fences{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
        .swapchainCount = 1, .pFences = &fenceHandle};
    VkSetPresentConfigNV metering{.sType = VK_STRUCTURE_TYPE_SET_PRESENT_CONFIG_NV, .numFramesPerBatch = 2};
    timings.pNext = &mode;
    mode.pNext = &ids2;
    ids2.pNext = &ids;
    ids.pNext = &fences;
    fences.pNext = &metering;
    expect(DeferredPresent::supportsChain(&timings), "six known present nodes were not accepted");
    // A seventh node, here a repeated fence, is beyond the bound.
    VkSwapchainPresentFenceInfoEXT extra = fences;
    extra.pNext = nullptr;
    metering.pNext = &extra;
    expect(!DeferredPresent::supportsChain(&timings), "a seventh present node was accepted");
#endif
}

void crossBatchChaining() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    VkSwapchainKHR swapchain = handle<VkSwapchainKHR>(11);
    VkSemaphore wait = handle<VkSemaphore>(12);
    uint32_t image = 0;
    VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    DeferredPresent worker(queue, timedPresent);
    expect(worker.beginBatch(1), "chaining predecessor rejected");
    expect(worker.enqueue(info, DeferredPresent::Clock::now()), "chaining predecessor output rejected");
    worker.closeBatch();
    const auto predecessor = worker.finish(UINT64_MAX);
    expect(predecessor.has_value() && predecessor->count == 1, "chaining predecessor did not complete");
    const auto previousStarted = predecessor->calls[0].started;
    expect(worker.beginBatch(1, {.overlap = true, .outputInterval = 8ms}), "chained successor rejected");
    expect(worker.enqueue(info, previousStarted + 1ms), "chained successor output rejected");
    worker.closeBatch();
    const auto successor = worker.finish(UINT64_MAX);
    expect(successor.has_value() && successor->count == 1 &&
            successor->calls[0].started >= previousStarted + 7ms,
        "successor output ignored the previous batch's planned spacing");
}
void overlappedChainShiftsLaterOutputs() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    VkSwapchainKHR swapchain = handle<VkSwapchainKHR>(11);
    VkSemaphore wait = handle<VkSemaphore>(12);
    uint32_t image = 0;
    VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    DeferredPresent worker(queue, timedPresent);
    const auto interval = 8000us;
    const auto chained = interval - interval / 16;
    const auto tolerance = 500us;
    expect(worker.beginBatch(1), "shift predecessor rejected");
    expect(worker.enqueue(info, DeferredPresent::Clock::now()), "shift predecessor output rejected");
    worker.closeBatch();
    const auto predecessor = worker.finish(UINT64_MAX);
    expect(predecessor && predecessor->count == 1, "shift predecessor did not complete");
    const auto predecessorScheduled = predecessor->calls[0].scheduled;
    // The first output is requested 6.5 ms after the predecessor's slot, so the
    // cross-batch chain pushes it by 1 ms. The second output keeps its 4 ms spacing.
    const auto first = predecessorScheduled + 6500us;
    expect(worker.beginBatch(2, {.overlap = true, .outputInterval = interval}), "shifted successor rejected");
    expect(worker.enqueue(info, first), "shifted first output rejected");
    image = 1;
    expect(worker.enqueue(info, first + 4ms), "shifted second output rejected");
    worker.closeBatch();
    const auto successor = worker.finish(UINT64_MAX);
    expect(successor && successor->count == 2, "shifted successor did not complete");
    const auto& head = successor->calls[0];
    const auto& tail = successor->calls[1];
    expect(head.scheduled >= first + 1ms && head.scheduled <= predecessorScheduled + chained + tolerance,
        "cross-batch chain did not push the first output within its planned slot");
    const auto requestedTail = head.scheduled + 4ms;
    const auto error = tail.scheduled > requestedTail ? tail.scheduled - requestedTail : requestedTail - tail.scheduled;
    expect(error <= tolerance, "chained first output changed the second output's requested spacing");
}
void expeditedOutputAnchorsNextBatch() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    VkSwapchainKHR swapchain = handle<VkSwapchainKHR>(11);
    VkSemaphore wait = handle<VkSemaphore>(12);
    uint32_t image = 0;
    VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    DeferredPresent worker(queue, timedPresent);
    timedImages.clear();
    const auto interval = 40ms;
    // The head output is scheduled 100 ms ahead and leaves only because the
    // join expedites it. Its actual start, not the unreached schedule, anchors
    // the next overlapped batch.
    const auto due = DeferredPresent::Clock::now() + 100ms;
    expect(worker.beginBatch(1, {.expediteOnJoin = true, .overlap = true, .outputInterval = interval}),
        "expedited head batch rejected");
    expect(worker.enqueue(info, due), "expedited head output rejected");
    worker.closeBatch();
    const auto head = worker.finish(UINT64_MAX);
    expect(head && head->count == 1 && head->calls[0].started < due - 50ms,
        "join did not expedite the overlapped head output");
    const auto anchor = head->calls[0].started;
    // The successor is requested now. Its first output may follow the expedited
    // output by one planned slot, but not by the 100 ms schedule it never reached.
    expect(worker.beginBatch(1, {.overlap = true, .outputInterval = interval}), "successor batch rejected");
    expect(worker.enqueue(info, DeferredPresent::Clock::now()), "successor output rejected");
    worker.closeBatch();
    const auto successor = worker.finish(UINT64_MAX);
    expect(successor && successor->count == 1 && successor->calls[0].scheduled <= anchor + interval,
        "successor chained from the expedited output's unreached schedule");
}
namespace {
struct CopySubmit {
    uint32_t count{};
    VkCommandBuffer command{};
    uint32_t waits{};
    std::array<VkSemaphore, 3> waitSemaphores{};
    uint32_t signals{};
    std::array<VkSemaphore, 3> signalSemaphores{};
    VkFence fence{};
};
std::vector<CopySubmit> copySubmits;
std::vector<std::pair<uint32_t, VkSemaphore>> copyPresents;
std::mutex copyObservationMutex;
std::condition_variable copyObservationChanged;
bool copyPresented{};
VkResult copyAcquireResult{VK_SUCCESS};
uint32_t copyAcquireImage{2};
unsigned copyAcquireCalls{};
uint64_t copyAcquireTimeout{};
VkSemaphore copyAcquireSemaphore{};
VkResult VKAPI_CALL copyAcquire(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
        VkSemaphore semaphore, VkFence fence, uint32_t* image) {
    expect(device == handle<VkDevice>(51) && swapchain == handle<VkSwapchainKHR>(11) && fence == VK_NULL_HANDLE,
        "generated acquire borrowed the wrong device, swapchain or fence");
    ++copyAcquireCalls;
    copyAcquireTimeout = timeout;
    copyAcquireSemaphore = semaphore;
    if (copyAcquireResult == VK_SUCCESS) *image = copyAcquireImage;
    return copyAcquireResult;
}
VkResult VKAPI_CALL copySubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* info, VkFence fence) {
    expect(count == 1 && queue == handle<VkQueue>(7) && info->sType == VK_STRUCTURE_TYPE_SUBMIT_INFO &&
        !info->pNext, "generated submit used the wrong queue or submit shape");
    CopySubmit record{};
    record.count = info->commandBufferCount;
    record.command = info->commandBufferCount ? info->pCommandBuffers[0] : VK_NULL_HANDLE;
    record.waits = info->waitSemaphoreCount;
    for (uint32_t i = 0; i < info->waitSemaphoreCount; ++i)
        record.waitSemaphores.at(i) = info->pWaitSemaphores[i];
    record.signals = info->signalSemaphoreCount;
    for (uint32_t i = 0; i < info->signalSemaphoreCount; ++i)
        record.signalSemaphores.at(i) = info->pSignalSemaphores[i];
    record.fence = fence;
    copySubmits.push_back(record);
    return VK_SUCCESS;
}
VkResult VKAPI_CALL recordingPresent(VkQueue, const VkPresentInfoKHR* info) {
    copyPresents.emplace_back(info->pImageIndices[0], info->pWaitSemaphores[0]);
    {
        const std::lock_guard lock(copyObservationMutex);
        copyPresented = true;
    }
    copyObservationChanged.notify_one();
    return VK_SUCCESS;
}
DeferredPresent::GeneratedCopy generatedCopy() {
    DeferredPresent::GeneratedCopy copy{
        .device = handle<VkDevice>(51), .acquire = copyAcquire, .submit = copySubmit,
        .acquireSemaphore = handle<VkSemaphore>(61), .staged = handle<VkSemaphore>(62),
        .continuation = handle<VkSemaphore>(63), .fence = handle<VkFence>(64), .imageCount = 3,
        .giveUp = 2ms,
    };
    for (uint32_t k = 0; k < copy.imageCount; ++k) {
        copy.copies.at(k) = handle<VkCommandBuffer>(80 + k);
        copy.presentWaits.at(k) = handle<VkSemaphore>(70 + k);
    }
    return copy;
}
void generatedCopyPresentsAcquiredImage() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0; copyAcquireResult = VK_SUCCESS;
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto realWait = handle<VkSemaphore>(12);
    uint32_t realImage = 4;
    const VkPresentInfoKHR realInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
    const auto copy = generatedCopy();
    const auto deadline = DeferredPresent::Clock::now();
    expect(worker.beginBatch(2), "generated copy batch rejected");
    expect(worker.enqueueGeneratedCopy(swapchain, nullptr, deadline, copy), "generated copy rejected by an open batch");
    expect(worker.enqueue(realInfo, deadline + 1ms), "real output rejected after generated copy");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 2 && completion->result == VK_SUCCESS &&
        !completion->calls[0].skipped && !completion->calls[1].skipped,
        "generated copy was skipped or changed the batch result");
    expect(copyAcquireCalls == 1 && copyAcquireTimeout == 0 && copyAcquireSemaphore == handle<VkSemaphore>(61),
        "generated image acquisition blocked or used the wrong semaphore");
    expect(copySubmits.size() == 1, "generated copy was not submitted exactly once");
    const auto& submit = copySubmits.front();
    expect(submit.count == 1 && submit.command == handle<VkCommandBuffer>(82) && submit.fence == handle<VkFence>(64) &&
        submit.waits == 2 && submit.waitSemaphores[0] == handle<VkSemaphore>(61) &&
        submit.waitSemaphores[1] == handle<VkSemaphore>(62) &&
        submit.signals == 2 && submit.signalSemaphores[0] == handle<VkSemaphore>(72) &&
        submit.signalSemaphores[1] == handle<VkSemaphore>(63),
        "generated copy did not wait and signal the pre-recorded image contract");
    expect(copyPresents.size() == 2 && copyPresents[0] == std::pair{2u, handle<VkSemaphore>(72)} &&
        copyPresents[1] == std::pair{4u, realWait},
        "worker did not present the copied image before the real output");
}
void generatedCopySkipsWithoutImage() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0; copyAcquireResult = VK_NOT_READY;
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto realWait = handle<VkSemaphore>(12);
    uint32_t realImage = 4;
    const VkPresentInfoKHR realInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
    const auto copy = generatedCopy();
    const auto deadline = DeferredPresent::Clock::now();
    expect(worker.beginBatch(2), "skipped generated copy batch rejected");
    expect(worker.enqueueGeneratedCopy(swapchain, nullptr, deadline, copy), "skipped generated copy rejected");
    expect(worker.enqueue(realInfo, deadline + 1ms), "real output rejected after skipped generated copy");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 2 && completion->result == VK_SUCCESS &&
        completion->calls[0].skipped && completion->calls[0].result == VK_SUCCESS && !completion->calls[1].skipped,
        "unavailable generated image was not a successful skip ahead of the real output");
    expect(copyAcquireCalls >= 1 && copyAcquireTimeout == 0, "unavailable generated image was polled with a blocking timeout");
    expect(copySubmits.size() == 1, "skip did not signal exactly once");
    const auto& submit = copySubmits.front();
    expect(submit.count == 0 && submit.waits == 1 && submit.waitSemaphores[0] == handle<VkSemaphore>(62) &&
        submit.signals == 1 && submit.signalSemaphores[0] == handle<VkSemaphore>(63) &&
        submit.fence == handle<VkFence>(64),
        "skip did not wait the staged copy and signal the continuation and fence");
    expect(copyPresents.size() == 1 && copyPresents[0] == std::pair{4u, realWait},
        "skipped generated image was presented or the real output was lost");
    copyAcquireResult = VK_SUCCESS;
}
void generatedCopySkipsOutOfRangeImage() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0;
    copyAcquireResult = VK_SUCCESS; copyAcquireImage = 3;
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto realWait = handle<VkSemaphore>(12);
    uint32_t realImage = 4;
    const VkPresentInfoKHR realInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
    const auto copy = generatedCopy();
    const auto deadline = DeferredPresent::Clock::now();
    expect(worker.beginBatch(2), "out-of-range generated batch rejected");
    expect(worker.enqueueGeneratedCopy(swapchain, nullptr, deadline, copy), "out-of-range generated copy rejected");
    expect(worker.enqueue(realInfo, deadline + 1ms), "real output rejected after out-of-range generated copy");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 2 && completion->calls[0].skipped &&
        completion->calls[0].result == VK_ERROR_UNKNOWN && !completion->calls[1].skipped,
        "out-of-range acquired image was not reported as a skipped error ahead of the real output");
    expect(copyAcquireCalls == 1 && copyAcquireSemaphore == handle<VkSemaphore>(61),
        "out-of-range image was acquired with the wrong semaphore");
    expect(copySubmits.size() == 1, "acquired out-of-range image was not signalled exactly once");
    const auto& submit = copySubmits.front();
    expect(submit.count == 0 && submit.waits == 2 && submit.waitSemaphores[0] == handle<VkSemaphore>(61) &&
        submit.waitSemaphores[1] == handle<VkSemaphore>(62) && submit.signals == 1 &&
        submit.signalSemaphores[0] == handle<VkSemaphore>(63) && submit.fence == handle<VkFence>(64),
        "out-of-range skip did not wait the acquire and staged copy before signalling the continuation");
    expect(copyPresents.size() == 1 && copyPresents[0] == std::pair{4u, realWait},
        "out-of-range generated image was presented or the real output was lost");
    copyAcquireImage = 2;
}
void generatedCopyNeedsOpenBatch() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto copy = generatedCopy();
    expect(!worker.enqueueGeneratedCopy(swapchain, nullptr, DeferredPresent::Clock::now(), copy),
        "generated copy admitted without an open batch");
    auto invalid = copy;
    invalid.imageCount = DeferredPresent::maxCopyImages + 1;
    expect(worker.beginBatch(1) && !worker.enqueueGeneratedCopy(swapchain, nullptr, DeferredPresent::Clock::now(), invalid),
        "generated copy with an unsupported image count was admitted");
    invalid = copy;
    invalid.staged = VK_NULL_HANDLE;
    expect(!worker.enqueueGeneratedCopy(swapchain, nullptr, DeferredPresent::Clock::now(), invalid),
        "generated copy without a staged signal was admitted");
    worker.closeBatch();
    expect(worker.finish(UINT64_MAX).has_value(), "refused generated copy left its batch open");
}
// W1: a second generated copy of a batch with `preceding` waits the first copy's signal once,
// between its acquire and its staged signal, and both copies keep their order. The first copy
// has no fence, as every copy but the last of a batch.
void generatedCopyChainWaitsPreceding() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0; copyAcquireResult = VK_SUCCESS;
    copyAcquireImage = 2;
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto realWait = handle<VkSemaphore>(12);
    uint32_t realImage = 4;
    const VkPresentInfoKHR realInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
    auto head = generatedCopy();
    head.continuation = handle<VkSemaphore>(65);
    head.fence = VK_NULL_HANDLE;
    auto tail = generatedCopy();
    tail.acquireSemaphore = handle<VkSemaphore>(66);
    tail.staged = handle<VkSemaphore>(67);
    tail.preceding = handle<VkSemaphore>(65);
    for (uint32_t k = 0; k < tail.imageCount; ++k) {
        tail.copies.at(k) = handle<VkCommandBuffer>(84 + k);
        tail.presentWaits.at(k) = handle<VkSemaphore>(74 + k);
    }
    const auto deadline = DeferredPresent::Clock::now();
    expect(worker.beginBatch(3), "generated chain batch rejected");
    expect(worker.enqueueGeneratedCopy(swapchain, nullptr, deadline, head),
        "first generated copy of a chain with no fence was rejected");
    expect(worker.enqueueGeneratedCopy(swapchain, nullptr, deadline + 1ms, tail),
        "chained generated copy with a preceding signal was rejected");
    expect(worker.enqueue(realInfo, deadline + 2ms), "real output rejected after the chain");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 3 && completion->result == VK_SUCCESS &&
        !completion->calls[0].skipped && !completion->calls[1].skipped && !completion->calls[2].skipped,
        "chained generated copies were skipped or changed the batch result");
    expect(copySubmits.size() == 2, "chained generated copies were not submitted exactly once each");
    const auto& first = copySubmits.front();
    expect(first.count == 1 && first.command == handle<VkCommandBuffer>(82) && first.fence == VK_NULL_HANDLE &&
        first.waits == 2 && first.waitSemaphores[0] == handle<VkSemaphore>(61) &&
        first.waitSemaphores[1] == handle<VkSemaphore>(62) &&
        first.signals == 2 && first.signalSemaphores[0] == handle<VkSemaphore>(72) &&
        first.signalSemaphores[1] == handle<VkSemaphore>(65),
        "first chain copy did not wait acquire and staged, or signal present wait and its copied signal");
    const auto& second = copySubmits.back();
    expect(second.count == 1 && second.command == handle<VkCommandBuffer>(86) && second.fence == handle<VkFence>(64) &&
        second.waits == 3 && second.waitSemaphores[0] == handle<VkSemaphore>(66) &&
        second.waitSemaphores[1] == handle<VkSemaphore>(65) && second.waitSemaphores[2] == handle<VkSemaphore>(67) &&
        second.signals == 2 && second.signalSemaphores[0] == handle<VkSemaphore>(76) &&
        second.signalSemaphores[1] == handle<VkSemaphore>(63),
        "chained copy did not wait acquire, preceding and staged in order, or signal present wait then continuation");
    expect(copyPresents.size() == 3 && copyPresents[0] == std::pair{2u, handle<VkSemaphore>(72)} &&
        copyPresents[1] == std::pair{2u, handle<VkSemaphore>(76)} && copyPresents[2] == std::pair{4u, realWait},
        "chain did not present its copies in order before the real output");
}
// W1: a skipped chain copy waits its preceding signal and then its staged copy, and signals only
// its continuation. The head skip has no fence.
void generatedCopyChainSkipWaitsPreceding() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0; copyAcquireResult = VK_NOT_READY;
    copyAcquireImage = 2;
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto realWait = handle<VkSemaphore>(12);
    uint32_t realImage = 4;
    const VkPresentInfoKHR realInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
    auto head = generatedCopy();
    head.continuation = handle<VkSemaphore>(65);
    head.fence = VK_NULL_HANDLE;
    auto tail = generatedCopy();
    tail.acquireSemaphore = handle<VkSemaphore>(66);
    tail.staged = handle<VkSemaphore>(67);
    tail.preceding = handle<VkSemaphore>(65);
    const auto deadline = DeferredPresent::Clock::now();
    expect(worker.beginBatch(3), "skipped generated chain batch rejected");
    expect(worker.enqueueGeneratedCopy(swapchain, nullptr, deadline, head), "skipped chain head rejected");
    expect(worker.enqueueGeneratedCopy(swapchain, nullptr, deadline + 1ms, tail), "skipped chain tail rejected");
    expect(worker.enqueue(realInfo, deadline + 2ms), "real output rejected after the skipped chain");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 3 && completion->result == VK_SUCCESS &&
        completion->calls[0].skipped && completion->calls[1].skipped && !completion->calls[2].skipped,
        "skipped chain did not skip both generated outputs ahead of the real output");
    expect(copySubmits.size() == 2, "skipped chain did not signal each output exactly once");
    const auto& first = copySubmits.front();
    expect(first.count == 0 && first.fence == VK_NULL_HANDLE && first.waits == 1 &&
        first.waitSemaphores[0] == handle<VkSemaphore>(62) && first.signals == 1 &&
        first.signalSemaphores[0] == handle<VkSemaphore>(65),
        "skipped chain head did not wait staged and signal only its continuation");
    const auto& second = copySubmits.back();
    expect(second.count == 0 && second.fence == handle<VkFence>(64) && second.waits == 2 &&
        second.waitSemaphores[0] == handle<VkSemaphore>(65) && second.waitSemaphores[1] == handle<VkSemaphore>(67) &&
        second.signals == 1 && second.signalSemaphores[0] == handle<VkSemaphore>(63),
        "skipped chain tail did not wait preceding and staged, or signal only its continuation");
    expect(copyPresents.size() == 1 && copyPresents[0] == std::pair{4u, realWait},
        "skipped chain presented a generated image or lost the real output");
    copyAcquireResult = VK_SUCCESS;
}
VkResult VKAPI_CALL copyReadinessTimeout(VkDevice, const VkSemaphoreWaitInfo*, uint64_t) {
    return VK_TIMEOUT;
}
// Readiness still skips a generated output. For an overlapped batch an expedite or a
// passed batch limit no longer skips it (F7 rules 2 and 4): it presents from a free
// lower image, and the real output after it follows (see generatedCopyTailSurvivesAJoin).
void generatedCopyUnsafeSpacingModes() {
    for (unsigned mode = 0; mode < 3; ++mode) {
        auto queue = std::make_shared<DeferredPresentQueue>();
        queue->queue = handle<VkQueue>(7);
        DeferredPresent worker(queue, recordingPresent);
        copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0; copyAcquireResult = VK_SUCCESS;
        const auto swapchain = handle<VkSwapchainKHR>(11);
        const auto realWait = handle<VkSemaphore>(12);
        uint32_t realImage = 4;
        const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
        const auto now = DeferredPresent::Clock::now();
        const auto due = mode == 1 ? now + 1s : now;
        DeferredPresent::Readiness ready{
            .wait = mode == 0 ? copyReadinessTimeout : nullptr,
            .deadline = now,
            .latestOutput = mode == 2 ? now : DeferredPresent::Clock::time_point::max(),
            .expediteOnJoin = mode == 1,
            .overlap = true, .outputInterval = 10ms,
        };
        expect(worker.beginBatch(2, ready), "pressure copy batch rejected");
        expect(worker.enqueueGeneratedCopy(swapchain, nullptr, due, generatedCopy()) &&
            worker.enqueue(info, due + 10ms), "pressure copy outputs rejected");
        worker.closeBatch();
        const auto completion = worker.finish(UINT64_MAX);
        expect(completion && completion->count == 2 && !completion->calls[1].skipped &&
            completion->calls[0].skipped == (mode == 0),
            "generated output skipped outside the readiness rule, or the real output was lost");
        expect(completion->readinessMiss == (mode == 0), "skip changed the readiness result");
        if (mode == 0) {
            expect(completion->calls[0].skipReason == DeferredPresent::SkipReason::Readiness &&
                copyAcquireCalls == 0 && copySubmits.size() == 1 && copySubmits[0].count == 0 &&
                copySubmits[0].waits == 1 && copySubmits[0].waitSemaphores[0] == handle<VkSemaphore>(62) &&
                copySubmits[0].signals == 1 && copySubmits[0].signalSemaphores[0] == handle<VkSemaphore>(63) &&
                copySubmits[0].fence == handle<VkFence>(64),
                "readiness skip failed to consume staged work and signal continuation/fence exactly once");
            expect(copyPresents.size() == 1 && copyPresents[0].first == 4,
                "readiness skip lost the real image or emitted a generated image");
        } else {
            expect(copyAcquireCalls == 1 && copySubmits.size() == 1 && copySubmits[0].count == 1 &&
                copyPresents.size() == 2 && copyPresents[0].first == 2 && copyPresents[1].first == 4,
                "unskipped generated output was not copied and presented ahead of the real output");
        }
    }
}
void generatedCopyTailSurvivesAJoin() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0; copyAcquireResult = VK_SUCCESS;
    { const std::lock_guard lock(copyObservationMutex); copyPresented = false; }
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto realWait = handle<VkSemaphore>(12);
    uint32_t realImage = 4;
    const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
    const auto now = DeferredPresent::Clock::now();
    auto copy = generatedCopy();
    copy.giveUp = 1s;
    expect(worker.beginBatch(2, {.latestOutput = now + 60ms, .joinTolerance = 0ns,
        .expediteOnJoin = true, .overlap = true, .outputInterval = 30ms}), "paced copy batch rejected");
    expect(worker.enqueueGeneratedCopy(swapchain, nullptr, now, copy) &&
        worker.enqueue(info, now + 30ms), "paced copy outputs rejected");
    worker.closeBatch();
    {
        std::unique_lock lock(copyObservationMutex);
        expect(copyObservationChanged.wait_for(lock, 1s, [] { return copyPresented; }),
            "generated output never reached the lower present");
    }
    const auto completion = worker.finish(UINT64_MAX);
    // F7 rule 4: the join may pull the tail to a quarter interval after its generated
    // output, never earlier, and never past its normal deadline.
    expect(completion && completion->count == 2 && !completion->calls[0].skipped && !completion->calls[1].skipped &&
        completion->calls[1].started >= completion->calls[0].started + 7500us &&
        completion->calls[1].started <= completion->calls[1].scheduled + 5ms,
        "join did not pull the tail to a quarter interval, or the tail passed its normal deadline");
}
// Release-gated overlap: a generated output due between latest - interval and
// latest - interval / 4 is still presented, and the real after it is pulled to
// the batch limit instead of keeping its full output spacing.
void lateGeneratedCopyPresentsBeforeLimit() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0;
    copyAcquireResult = VK_SUCCESS; copyAcquireImage = 2;
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto realWait = handle<VkSemaphore>(12);
    uint32_t realImage = 4;
    const VkPresentInfoKHR realInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
    // Wake-up jitter only: the planned spacing is checked exactly through scheduled.
    const auto tolerance = 5ms;
    const auto interval = 80ms;
    const auto latest = DeferredPresent::Clock::now() + 100ms;
    auto copy = generatedCopy();
    copy.giveUp = 1s;
    const auto generatedDue = latest - interval / 2;
    expect(worker.beginBatch(2, {.latestOutput = latest, .overlap = true, .outputInterval = interval}),
        "late generated batch rejected");
    expect(worker.enqueueGeneratedCopy(swapchain, nullptr, generatedDue, copy) &&
        worker.enqueue(realInfo, generatedDue + interval), "late generated outputs rejected");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 2 && completion->result == VK_SUCCESS &&
        !completion->calls[0].skipped && !completion->calls[1].skipped,
        "generated output inside the release window was skipped");
    expect(completion->calls[1].started <= latest + tolerance && completion->calls[1].scheduled <= latest,
        "real output after a late generated copy passed the batch limit");
    expect(copyAcquireCalls == 1 && copySubmits.size() == 1 && copySubmits.front().count == 1 &&
        copySubmits.front().command == handle<VkCommandBuffer>(82) && copySubmits.front().fence == handle<VkFence>(64),
        "late generated image was not copied exactly once");
    expect(copyPresents.size() == 2 && copyPresents[0] == std::pair{2u, handle<VkSemaphore>(72)} &&
        copyPresents[1] == std::pair{4u, realWait},
        "worker did not present the late generated image before the real output");
}
// The same batch with a generated output due after latest - interval / 4 is still skipped.
void lateGeneratedCopyPastQuarterSkips() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0;
    copyAcquireResult = VK_NOT_READY; copyAcquireImage = 2;
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto realWait = handle<VkSemaphore>(12);
    uint32_t realImage = 4;
    const VkPresentInfoKHR realInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
    const auto interval = 80ms;
    const auto latest = DeferredPresent::Clock::now() + 100ms;
    auto copy = generatedCopy();
    copy.giveUp = 1s;
    const auto generatedDue = latest - interval / 8;
    expect(worker.beginBatch(2, {.latestOutput = latest, .overlap = true, .outputInterval = interval}),
        "past-window generated batch rejected");
    expect(worker.enqueueGeneratedCopy(swapchain, nullptr, generatedDue, copy) &&
        worker.enqueue(realInfo, generatedDue + interval), "past-window outputs rejected");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 2 && completion->result == VK_SUCCESS &&
        completion->calls[0].skipped && !completion->calls[1].skipped,
        "generated output past the release window was presented or the real output was lost");
    expect(completion->calls[1].scheduled <= latest,
        "real output after a skipped generated output passed the batch limit");
    expect(completion->calls[0].skipReason == DeferredPresent::SkipReason::Acquire,
        "generated output with no lower image was not skipped as an acquire miss");
    expect(copyAcquireCalls == 1, "past-quarter generated output did not make its single attempt");
    copyAcquireResult = VK_SUCCESS;
    expect(copySubmits.size() == 1 && copySubmits.front().count == 0 && copySubmits.front().waits == 1 &&
        copySubmits.front().waitSemaphores[0] == handle<VkSemaphore>(62) && copySubmits.front().signals == 1 &&
        copySubmits.front().signalSemaphores[0] == handle<VkSemaphore>(63) &&
        copySubmits.front().fence == handle<VkFence>(64),
        "skip did not wait the staged copy and signal the continuation and fence");
    expect(copyPresents.size() == 1 && copyPresents[0] == std::pair{4u, realWait},
        "skipped generated output was presented or the real output was lost");
}
// Without a release limit, a copy tail keeps the full output spacing.
void generatedCopyTailKeepsSpacingWithoutLimit() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0;
    copyAcquireResult = VK_SUCCESS; copyAcquireImage = 2;
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto realWait = handle<VkSemaphore>(12);
    uint32_t realImage = 4;
    const VkPresentInfoKHR realInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
    const auto interval = 80ms;
    const auto now = DeferredPresent::Clock::now();
    auto copy = generatedCopy();
    copy.giveUp = 1s;
    expect(worker.beginBatch(2, {.overlap = true, .outputInterval = interval}),
        "unlimited copy batch rejected");
    expect(worker.enqueueGeneratedCopy(swapchain, nullptr, now, copy) &&
        worker.enqueue(realInfo, now + 1ms), "unlimited copy outputs rejected");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 2 && completion->result == VK_SUCCESS &&
        !completion->calls[0].skipped && !completion->calls[1].skipped,
        "unlimited generated copy was skipped");
    expect(completion->calls[1].started >= completion->calls[0].started + interval,
        "copy tail lost its full output spacing without a release limit");
    expect(copyPresents.size() == 2 && copyPresents[0] == std::pair{2u, handle<VkSemaphore>(72)} &&
        copyPresents[1] == std::pair{4u, realWait},
        "unlimited copy presented the wrong images");
}

// F7 readiness script: the first wait and the bounded extension each return a scripted result.
unsigned scriptedCalls{};
uint64_t scriptedTimeouts[2]{};
VkResult scriptedResults[2]{};
VkResult VKAPI_CALL scriptedReadiness(VkDevice device, const VkSemaphoreWaitInfo* info, uint64_t timeout) {
    expect(device == handle<VkDevice>(21) && info->semaphoreCount == 1 &&
        info->pSemaphores[0] == handle<VkSemaphore>(31) && info->pValues[0] == 17,
        "readiness waited on the wrong device, semaphore or timeline value");
    const unsigned call = scriptedCalls++;
    expect(call < 2, "readiness waited more than twice for one batch");
    scriptedTimeouts[call] = timeout;
    return scriptedResults[call];
}

// (a) An overlapped batch expedited before its generated deadline presents the
// generated output at once, then the real output no earlier than a quarter interval
// after it and no later than its normal deadline. Neither output is skipped.
void expeditedOverlapKeepsGeneratedAndTail() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0;
    copyAcquireResult = VK_SUCCESS; copyAcquireImage = 2;
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto realWait = handle<VkSemaphore>(12);
    uint32_t realImage = 4;
    const VkPresentInfoKHR realInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
    const auto interval = 40ms;
    const auto now = DeferredPresent::Clock::now();
    expect(worker.beginBatch(2, {.joinTolerance = 0ns, .expediteOnJoin = true, .overlap = true,
        .outputInterval = interval}), "expedited overlapped batch rejected");
    expect(worker.enqueueGeneratedCopy(swapchain, nullptr, now + 2s, generatedCopy()) &&
        worker.enqueue(realInfo, now + 2s + 1ms), "expedited overlapped outputs rejected");
    worker.closeBatch();
    expect(!worker.finish(0), "poll expedited a released batch before its join");
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 2 && completion->result == VK_SUCCESS &&
        !completion->calls[0].skipped && !completion->calls[1].skipped,
        "expedite skipped the generated or the real output");
    const auto& generated = completion->calls[0];
    const auto& real = completion->calls[1];
    expect(generated.started < now + 1s, "expedite did not present the generated output before its deadline");
    expect(real.started >= generated.started + interval / 4 && real.started < now + 1s,
        "real output was not pulled to a quarter interval after its generated output, or waited for its normal deadline");
    expect(copyAcquireCalls == 1 && copyPresents.size() == 2 &&
        copyPresents[0] == std::pair{2u, handle<VkSemaphore>(72)} && copyPresents[1] == std::pair{4u, realWait},
        "expedited generated and real outputs were presented out of order");
}

// (b) An expedite that arrives while the real output's tail is sleeping pulls it in.
void expeditePullsSleepingTail() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0;
    copyAcquireResult = VK_SUCCESS; copyAcquireImage = 2;
    { const std::lock_guard lock(copyObservationMutex); copyPresented = false; }
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto realWait = handle<VkSemaphore>(12);
    uint32_t realImage = 4;
    const VkPresentInfoKHR realInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
    const auto interval = 40ms;
    const auto now = DeferredPresent::Clock::now();
    expect(worker.beginBatch(2, {.joinTolerance = 0ns, .expediteOnJoin = true, .overlap = true,
        .outputInterval = interval}), "tail batch rejected");
    expect(worker.enqueueGeneratedCopy(swapchain, nullptr, now, generatedCopy()) &&
        worker.enqueue(realInfo, now + 2s), "tail outputs rejected");
    worker.closeBatch();
    {
        std::unique_lock lock(copyObservationMutex);
        expect(copyObservationChanged.wait_for(lock, 5s, [] { return copyPresented; }),
            "generated output was not presented before the join");
    }
    // The real output now sleeps toward its normal deadline, two seconds away.
    std::this_thread::sleep_for(20ms);
    const auto joinStarted = DeferredPresent::Clock::now();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 2 && !completion->calls[0].skipped && !completion->calls[1].skipped,
        "join during the sleeping tail skipped an output");
    expect(completion->calls[1].started >= joinStarted && completion->calls[1].started < now + 1s,
        "join did not pull the sleeping tail in, or it waited for its normal deadline");
    expect(completion->calls[1].started >= completion->calls[0].started + interval / 4,
        "the pulled-in tail came before a quarter interval after its generated output");
    expect(copyPresents.size() == 2 && copyPresents[1] == std::pair{4u, realWait},
        "pulled-in tail was presented with the wrong image");
}

// (c) A generated output reaching the worker after its give-up point is presented
// when a lower image is free, and skipped for acquire when none is.
void lateGeneratedUsesOneAttempt() {
    for (const bool imageFree : {true, false}) {
        auto queue = std::make_shared<DeferredPresentQueue>();
        queue->queue = handle<VkQueue>(7);
        DeferredPresent worker(queue, recordingPresent);
        copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0;
        copyAcquireResult = imageFree ? VK_SUCCESS : VK_NOT_READY; copyAcquireImage = 2;
        const auto swapchain = handle<VkSwapchainKHR>(11);
        const auto realWait = handle<VkSemaphore>(12);
        uint32_t realImage = 4;
        const VkPresentInfoKHR realInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
        const auto now = DeferredPresent::Clock::now();
        auto copy = generatedCopy();
        copy.giveUp = 2ms;
        // Due 50 ms ago: the give-up point (deadline + giveUp) has already passed on arrival.
        expect(worker.beginBatch(2, {.overlap = true, .outputInterval = 40ms}), "late copy batch rejected");
        expect(worker.enqueueGeneratedCopy(swapchain, nullptr, now - 50ms, copy) &&
            worker.enqueue(realInfo, now + 1ms), "late copy outputs rejected");
        worker.closeBatch();
        const auto completion = worker.finish(UINT64_MAX);
        expect(completion && completion->count == 2 && !completion->calls[1].skipped,
            "late copy batch lost its real output");
        expect(copyAcquireCalls == 1, "a generated output past its give-up point did not make exactly one attempt");
        if (imageFree) {
            expect(!completion->calls[0].skipped && copyPresents.size() == 2 &&
                copyPresents[0] == std::pair{2u, handle<VkSemaphore>(72)} && copyPresents[1] == std::pair{4u, realWait},
                "late generated output with a free lower image was not presented first");
        } else {
            expect(completion->calls[0].skipped && completion->calls[0].skipReason == DeferredPresent::SkipReason::Acquire &&
                copySubmits.size() == 1 && copySubmits[0].count == 0 && copyPresents.size() == 1 &&
                copyPresents[0] == std::pair{4u, realWait},
                "late generated output without a lower image was not skipped for acquire");
        }
    }
    copyAcquireResult = VK_SUCCESS;
}

// (d) Readiness that misses the first wait but lands inside the bounded extension
// presents the generated output. Readiness that misses the extension skips it.
void readinessExtensionIsBounded() {
    for (const bool signalledInExtension : {true, false}) {
        auto queue = std::make_shared<DeferredPresentQueue>();
        queue->queue = handle<VkQueue>(7);
        DeferredPresent worker(queue, recordingPresent);
        copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0;
        copyAcquireResult = VK_SUCCESS; copyAcquireImage = 2;
        const auto swapchain = handle<VkSwapchainKHR>(11);
        const auto realWait = handle<VkSemaphore>(12);
        uint32_t realImage = 4;
        const VkPresentInfoKHR realInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
        scriptedCalls = 0;
        scriptedTimeouts[0] = scriptedTimeouts[1] = 0;
        scriptedResults[0] = VK_TIMEOUT;
        scriptedResults[1] = signalledInExtension ? VK_SUCCESS : VK_TIMEOUT;
        const auto now = DeferredPresent::Clock::now();
        // Batch limit 2 and a 10 ms interval give a 20 ms extension, inside the 50 ms cap.
        expect(worker.beginBatch(2, {.device = handle<VkDevice>(21), .semaphore = handle<VkSemaphore>(31),
            .value = 17, .wait = scriptedReadiness, .deadline = now + 1ms, .overlap = true,
            .outputInterval = 10ms}), "readiness batch rejected");
        expect(worker.enqueueGeneratedCopy(swapchain, nullptr, now + 1ms, generatedCopy()) &&
            worker.enqueue(realInfo, now + 2ms), "readiness outputs rejected");
        worker.closeBatch();
        const auto completion = worker.finish(UINT64_MAX);
        expect(completion && completion->count == 2 && !completion->calls[1].skipped,
            "readiness extension lost the real output");
        expect(scriptedCalls == 2 && scriptedTimeouts[1] == 20'000'000,
            "readiness extension was not bounded by the output interval times the batch limit");
        if (signalledInExtension) {
            expect(completion->readyAt != DeferredPresent::Clock::time_point{} &&
                completion->slack == now + 1ms - completion->readyAt &&
                completion->calls[0].scheduled == now + 1ms + completion->readinessShift,
                "readiness diagnostics lost the pre-shift deadline or applied margin");
            expect(!completion->readinessMiss && completion->calls[0].lateReadiness &&
                completion->calls[0].readinessResult == VK_SUCCESS && !completion->calls[0].skipped &&
                copyAcquireCalls == 1 && copyPresents.size() == 2 &&
                copyPresents[0] == std::pair{2u, handle<VkSemaphore>(72)} && copyPresents[1] == std::pair{4u, realWait},
                "readiness inside the extension was a miss, or the generated output was not presented");
        } else {
            expect(completion->readinessMiss && !completion->calls[0].lateReadiness &&
                completion->calls[0].skipped && completion->calls[0].skipReason == DeferredPresent::SkipReason::Readiness &&
                copyAcquireCalls == 0 && copyPresents.size() == 1 && copyPresents[0] == std::pair{4u, realWait},
                "readiness missed after the extension was not a readiness skip");
        }
    }
}

// (e) Outside an overlapped batch, expedite still skips a generated output, a
// generated output that polls out its give-up window is skipped, and an arrival past
// its give-up point with a free image is still presented.
void nonOverlapKeepsSkipBehaviour() {
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto realWait = handle<VkSemaphore>(12);
    uint32_t realImage = 4;
    const VkPresentInfoKHR realInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
    {
        auto queue = std::make_shared<DeferredPresentQueue>();
        queue->queue = handle<VkQueue>(7);
        DeferredPresent worker(queue, recordingPresent);
        copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0;
        copyAcquireResult = VK_SUCCESS; copyAcquireImage = 2;
        const auto now = DeferredPresent::Clock::now();
        expect(worker.beginBatch(2, {.expediteOnJoin = true, .outputInterval = 10ms}), "expedited batch rejected");
        expect(worker.enqueueGeneratedCopy(swapchain, nullptr, now + 2s, generatedCopy()) &&
            worker.enqueue(realInfo, now + 2s + 1ms), "expedited non-overlap outputs rejected");
        worker.closeBatch();
        const auto completion = worker.finish(UINT64_MAX);
        expect(completion && completion->count == 2 && completion->calls[0].skipped &&
            completion->calls[0].skipReason == DeferredPresent::SkipReason::Expedite &&
            !completion->calls[1].skipped && copyAcquireCalls == 0 && copyPresents.size() == 1 &&
            copyPresents[0] == std::pair{4u, realWait},
            "non-overlapped expedite no longer skipped its generated output");
    }
    {
        auto queue = std::make_shared<DeferredPresentQueue>();
        queue->queue = handle<VkQueue>(7);
        DeferredPresent worker(queue, recordingPresent);
        copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0;
        copyAcquireResult = VK_NOT_READY;
        auto copy = generatedCopy();
        copy.giveUp = 2ms;
        const auto now = DeferredPresent::Clock::now();
        expect(worker.beginBatch(2), "give-up batch rejected");
        expect(worker.enqueueGeneratedCopy(swapchain, nullptr, now, copy) &&
            worker.enqueue(realInfo, now + 1ms), "give-up outputs rejected");
        worker.closeBatch();
        const auto completion = worker.finish(UINT64_MAX);
        expect(completion && completion->count == 2 && completion->calls[0].skipped &&
            completion->calls[0].skipReason == DeferredPresent::SkipReason::Acquire && !completion->calls[1].skipped &&
            copyAcquireCalls >= 2 && copyAcquireTimeout == 0 && copyPresents.size() == 1 &&
            copyPresents[0] == std::pair{4u, realWait},
            "non-overlapped generated output did not skip after its give-up window");
        expect(completion->calls[0].started >= completion->calls[0].scheduled + 2ms,
            "generated output gave up before polling its whole give-up window");
    }
    {
        // Readiness on a non-overlapped batch keeps the single wait: no extension, even
        // with an output interval, so a timeout is a readiness miss at once.
        auto queue = std::make_shared<DeferredPresentQueue>();
        queue->queue = handle<VkQueue>(7);
        DeferredPresent worker(queue, recordingPresent);
        copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0;
        copyAcquireResult = VK_SUCCESS; copyAcquireImage = 2;
        scriptedCalls = 0;
        scriptedTimeouts[0] = scriptedTimeouts[1] = 0;
        scriptedResults[0] = VK_TIMEOUT;
        scriptedResults[1] = VK_SUCCESS;
        const auto now = DeferredPresent::Clock::now();
        expect(worker.beginBatch(2, {.device = handle<VkDevice>(21), .semaphore = handle<VkSemaphore>(31),
            .value = 17, .wait = scriptedReadiness, .deadline = now + 1ms, .outputInterval = 10ms}),
            "non-overlapped readiness batch rejected");
        expect(worker.enqueueGeneratedCopy(swapchain, nullptr, now + 1ms, generatedCopy()) &&
            worker.enqueue(realInfo, now + 2ms), "non-overlapped readiness outputs rejected");
        worker.closeBatch();
        const auto completion = worker.finish(UINT64_MAX);
        expect(completion && completion->count == 2 && completion->readinessMiss &&
            completion->calls[0].skipped && completion->calls[0].skipReason == DeferredPresent::SkipReason::Readiness &&
            !completion->calls[0].lateReadiness && !completion->calls[1].skipped && scriptedCalls == 1 &&
            copyAcquireCalls == 0,
            "non-overlapped readiness timeout was extended or did not skip as a readiness miss");
    }
    {
        auto queue = std::make_shared<DeferredPresentQueue>();
        queue->queue = handle<VkQueue>(7);
        DeferredPresent worker(queue, recordingPresent);
        copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0;
        copyAcquireResult = VK_SUCCESS; copyAcquireImage = 2;
        auto copy = generatedCopy();
        copy.giveUp = 2ms;
        const auto now = DeferredPresent::Clock::now();
        expect(worker.beginBatch(2), "past-window batch rejected");
        expect(worker.enqueueGeneratedCopy(swapchain, nullptr, now - 50ms, copy) &&
            worker.enqueue(realInfo, now + 1ms), "past-window outputs rejected");
        worker.closeBatch();
        const auto completion = worker.finish(UINT64_MAX);
        expect(completion && completion->count == 2 && !completion->calls[0].skipped && !completion->calls[1].skipped &&
            copyAcquireCalls == 1 && copyPresents.size() == 2,
            "non-overlapped generated output past its give-up point with a free image was not presented");
    }
}

// (f) generated_compressed counts a presented generated output whose following real
// output started under half an interval after it. A normally spaced pair is not counted.
void compressedPairsAreCounted() {
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto realWait = handle<VkSemaphore>(12);
    uint32_t realImage = 4;
    const VkPresentInfoKHR realInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
    const auto interval = 400ms;
    {
        // The batch limit pulls the real output to a quarter interval after its generated output.
        auto queue = std::make_shared<DeferredPresentQueue>();
        queue->queue = handle<VkQueue>(7);
        DeferredPresent worker(queue, recordingPresent);
        copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0;
        copyAcquireResult = VK_SUCCESS; copyAcquireImage = 2;
        const auto now = DeferredPresent::Clock::now();
        expect(worker.beginBatch(2, {.latestOutput = now + 2ms, .overlap = true, .outputInterval = interval}),
            "compressed batch rejected");
        expect(worker.enqueueGeneratedCopy(swapchain, nullptr, now, generatedCopy()) &&
            worker.enqueue(realInfo, now + 2s), "compressed outputs rejected");
        worker.closeBatch();
        const auto completion = worker.finish(UINT64_MAX);
        expect(completion && completion->count == 2 && !completion->calls[0].skipped && !completion->calls[1].skipped &&
            completion->calls[1].started - completion->calls[0].started < interval / 2,
            "compressed pair was not produced by the release limit");
        expect(completion->compressedGeneratedPairs() == 1, "compressed generated pair was not counted");
    }
    {
        // Ordinary spacing: the real output keeps the full interval after its generated output.
        auto queue = std::make_shared<DeferredPresentQueue>();
        queue->queue = handle<VkQueue>(7);
        DeferredPresent worker(queue, recordingPresent);
        copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0;
        copyAcquireResult = VK_SUCCESS; copyAcquireImage = 2;
        const auto now = DeferredPresent::Clock::now();
        expect(worker.beginBatch(2, {.overlap = true, .outputInterval = interval}), "spaced batch rejected");
        expect(worker.enqueueGeneratedCopy(swapchain, nullptr, now, generatedCopy()) &&
            worker.enqueue(realInfo, now + 1ms), "spaced outputs rejected");
        worker.closeBatch();
        const auto completion = worker.finish(UINT64_MAX);
        expect(completion && completion->count == 2 && !completion->calls[0].skipped && !completion->calls[1].skipped &&
            completion->calls[1].started >= completion->calls[0].started + interval,
            "spaced pair lost its full interval");
        expect(completion->compressedGeneratedPairs() == 0, "normally spaced pair was counted as compressed");
    }
}
}

// Wraps the fake present above. The latch is the only difference from its
// contract, so the worker's real present call stays observable.
std::mutex latchMutex;
std::condition_variable latchChanged;
bool latchOpen{true};
bool latchEntered{};
VkResult VKAPI_CALL latchedPresent(VkQueue queue, const VkPresentInfoKHR* info) {
    {
        std::unique_lock lock(latchMutex);
        latchEntered = true;
        latchChanged.notify_all();
        expect(latchChanged.wait_for(lock, 5s, [] { return latchOpen; }),
            "latched present was never released");
    }
    return present(queue, info);
}
void unboundedDrainWaitsForLatchedPresent() {
    // current behaviour, see handoff: unbounded drain. A finite finish returns
    // nullopt while the present is latched; an unbounded finish waits for it.
    {
        const std::lock_guard lock(latchMutex);
        latchOpen = false;
        latchEntered = false;
    }
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    VkSwapchainKHR swapchain = handle<VkSwapchainKHR>(11);
    VkSemaphore wait = handle<VkSemaphore>(12);
    uint32_t image = 0;
    VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    DeferredPresent worker(queue, latchedPresent);
    const auto before = calls.load();
    expect(worker.beginBatch(1), "latched batch rejected");
    expect(worker.enqueue(info, DeferredPresent::Clock::now()), "latched image rejected");
    worker.closeBatch();
    {
        std::unique_lock lock(latchMutex);
        expect(latchChanged.wait_for(lock, 5s, [] { return latchEntered; }),
            "worker never entered the latched present");
    }
    expect(!worker.finish(1'000'000), "finite drain completed while its present was latched");

    std::mutex drainMutex;
    std::condition_variable drainChanged;
    bool drainReturned{};
    std::optional<DeferredPresent::Completion> drained;
    std::thread drain([&] {
        auto completion = worker.finish(UINT64_MAX);
        const std::lock_guard lock(drainMutex);
        drained = std::move(completion);
        drainReturned = true;
        drainChanged.notify_all();
    });
    {
        std::unique_lock lock(drainMutex);
        expect(!drainChanged.wait_for(lock, 50ms, [&] { return drainReturned; }),
            "unbounded drain returned while its present was still latched");
    }
    {
        const std::lock_guard lock(latchMutex);
        latchOpen = true;
    }
    latchChanged.notify_all();
    {
        std::unique_lock lock(drainMutex);
        expect(drainChanged.wait_for(lock, 5s, [&] { return drainReturned; }),
            "unbounded drain did not return after the latch was released");
    }
    drain.join();
    expect(drained && drained->count == 1 && calls == before + 1,
        "released unbounded drain dropped or duplicated the latched image");
}

// Test seam for F9b. Production sets stopping only in ~DeferredPresent, after every
// batch has retired, so the stop-during-wait case is reached through this struct.
namespace fgvk::layer {
struct DeferredPresentTestAccess {
    static void requestStop(DeferredPresent& worker) {
        {
            const std::lock_guard lock(worker.mutex);
            worker.stopping = true;
        }
        worker.changed.notify_all();
    }
    // Sets the automatic real offset before a batch is admitted; the worker reads it only after admission.
    static void setRealOffset(DeferredPresent& worker, std::chrono::nanoseconds offset) {
        worker.realOffsetController.offset = offset;
    }
};
}

// Internal real outputs. The fake callbacks script the lower acquire and record
// what the worker submitted, so the worker's contract is observable without Vulkan.
namespace {
struct InternalFake {
    int notReadyPolls{};
    VkResult failure{VK_SUCCESS};
    uint32_t lowerIndex{7};
    VkSemaphore presentWait{};
    VkResult submitResult{VK_SUCCESS};
    int tryCalls{}, submits{}, abandons{};
    uint32_t submittedApplication{}, submittedLower{}, abandonedApplication{};
    VkSemaphore submittedContinuation{}, abandonedContinuation{};
    // When the worker submitted the copy. The test reads it after taking the completion.
    DeferredPresent::Clock::time_point submittedAt{};
    // Set by the worker's submit; the test thread polls it.
    std::atomic_bool submitted{};
    // Sleeps inside the lower callbacks, so the D6 lock-hold timings are nonzero.
    bool slow{};
};
VkResult fakeTryAcquire(void* owner, uint32_t* lowerIndex) {
    auto& state = *static_cast<InternalFake*>(owner);
    ++state.tryCalls;
    if (state.slow) std::this_thread::sleep_for(100us);
    if (state.notReadyPolls > 0) {
        --state.notReadyPolls;
        return VK_NOT_READY;
    }
    if (state.failure < 0) return state.failure;
    *lowerIndex = state.lowerIndex;
    return VK_SUCCESS;
}
VkResult fakeSubmit(void* owner, VkQueue queue, uint32_t applicationIndex, uint32_t lowerIndex,
        VkSemaphore continuation, VkSemaphore* presentWait) {
    auto& state = *static_cast<InternalFake*>(owner);
    expect(queue == handle<VkQueue>(7), "internal copy submitted on the wrong queue");
    ++state.submits;
    state.submittedAt = DeferredPresent::Clock::now();
    state.submittedApplication = applicationIndex;
    state.submittedLower = lowerIndex;
    state.submittedContinuation = continuation;
    *presentWait = state.presentWait;
    if (state.slow) std::this_thread::sleep_for(100us);
    state.submitted.store(true);
    return state.submitResult;
}
VkResult slowRecordingPresent(VkQueue queue, const VkPresentInfoKHR* info) {
    std::this_thread::sleep_for(100us);
    return recordingPresent(queue, info);
}
VkResult fakeAbandon(void* owner, VkQueue queue, uint32_t applicationIndex, VkSemaphore continuation) {
    auto& state = *static_cast<InternalFake*>(owner);
    expect(queue == handle<VkQueue>(7), "abandon submitted on the wrong queue");
    ++state.abandons;
    state.abandonedApplication = applicationIndex;
    state.abandonedContinuation = continuation;
    return VK_SUCCESS;
}
DeferredPresent::InternalRealCopy internalCopy(const std::shared_ptr<InternalFake>& fake, uint32_t applicationIndex) {
    return DeferredPresent::InternalRealCopy{
        .owner = fake, .tryAcquire = fakeTryAcquire, .submit = fakeSubmit,
        .abandon = fakeAbandon, .applicationIndex = applicationIndex,
    };
}
}

// (a) Lower images that are not ready are polled, never skipped: the output is
// presented once, after its deadline, with the lower index and presentWait the
// worker obtained from its callbacks.
void internalRealRetriesThenPresents() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    copyPresents.clear();
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto continuation = handle<VkSemaphore>(12);
    uint32_t applicationImage = 1;
    const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &continuation,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &applicationImage};
    auto fake = std::make_shared<InternalFake>();
    fake->notReadyPolls = 3;
    fake->lowerIndex = 6;
    fake->presentWait = handle<VkSemaphore>(95);
    const auto copy = internalCopy(fake, 1);
    auto broken = copy;
    broken.submit = nullptr;
    const auto deadline = DeferredPresent::Clock::now() + 5ms;
    expect(worker.beginBatch(1), "internal real batch rejected");
    expect(!worker.enqueueInternalReal(info, deadline, broken),
        "internal real output admitted without every callback");
    expect(worker.enqueueInternalReal(info, deadline, copy), "internal real output rejected");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 1 && completion->result == VK_SUCCESS &&
        !completion->calls[0].skipped, "internal real output was skipped or failed");
    expect(completion->calls[0].started >= deadline, "internal real output presented before its deadline");
    expect(completion->calls[0].imageIndex == 6 && completion->calls[0].workerCopy &&
        !completion->calls[0].generated && completion->calls[0].presentWait == handle<VkSemaphore>(95),
        "internal real output did not record the lower index, presentWait and worker copy");
    expect(fake->tryCalls == 4 && fake->submits == 1 && fake->abandons == 0,
        "internal real output did not poll three times and then submit once");
    expect(fake->submittedApplication == 1 && fake->submittedLower == 6 &&
        fake->submittedContinuation == continuation,
        "internal copy used the wrong application image, lower index or continuation");
    expect(copyPresents.size() == 1 && copyPresents[0] == std::pair{6u, handle<VkSemaphore>(95)},
        "internal real output was not presented exactly once with the lower index and presentWait");
}

// (b) Expedite and joins never skip an internal real output. The worker keeps
// polling while the caller finishes with a timeout, and the output still presents.
void internalRealNeverSkippedUnderExpedite() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    copyPresents.clear();
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto continuation = handle<VkSemaphore>(12);
    uint32_t applicationImage = 0;
    const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &continuation,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &applicationImage};
    auto fake = std::make_shared<InternalFake>();
    fake->notReadyPolls = 20;
    fake->lowerIndex = 3;
    fake->presentWait = handle<VkSemaphore>(96);
    const auto now = DeferredPresent::Clock::now();
    expect(worker.beginBatch(1, {.latestOutput = now, .expediteOnJoin = true,
        .outputInterval = 10ms}), "expedited internal batch rejected");
    expect(worker.enqueueInternalReal(info, now, internalCopy(fake, 0)), "expedited internal output rejected");
    worker.closeBatch();
    std::optional<DeferredPresent::Completion> completion;
    for (int attempt = 0; attempt < 5000 && !completion; ++attempt)
        completion = worker.finish(1'000'000);
    expect(completion && completion->count == 1 && completion->result == VK_SUCCESS &&
        !completion->calls[0].skipped, "expedite skipped the internal real output");
    expect(fake->abandons == 0 && fake->submits == 1 && copyPresents.size() == 1 &&
        copyPresents[0] == std::pair{3u, handle<VkSemaphore>(96)},
        "expedite did not present the internal real output exactly once");
}

// (c) A lower error abandons the application image once and the batch carries the error.
void internalRealErrorAbandonsOnce() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    copyPresents.clear();
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto continuation = handle<VkSemaphore>(12);
    uint32_t applicationImage = 2;
    const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &continuation,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &applicationImage};
    auto fake = std::make_shared<InternalFake>();
    fake->notReadyPolls = 1;
    fake->failure = VK_ERROR_OUT_OF_DATE_KHR;
    expect(worker.beginBatch(1), "failing internal batch rejected");
    expect(worker.enqueueInternalReal(info, DeferredPresent::Clock::now(), internalCopy(fake, 2)),
        "failing internal output rejected");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 1 && completion->result == VK_ERROR_OUT_OF_DATE_KHR &&
        completion->calls[0].skipped && completion->calls[0].result == VK_ERROR_OUT_OF_DATE_KHR,
        "a lower error was not carried by the completion");
    expect(fake->abandons == 1 && fake->abandonedApplication == 2 &&
        fake->abandonedContinuation == continuation && fake->submits == 0 && copyPresents.empty(),
        "a lower error did not abandon the application image exactly once without presenting");
}

// (d) An internal real output after a worker generated copy keeps the full copy-tail spacing.
void internalRealAfterGeneratedKeepsCopyTail() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0;
    copyAcquireResult = VK_SUCCESS; copyAcquireImage = 2;
    const auto swapchain = handle<VkSwapchainKHR>(11);
    uint32_t applicationImage = 1;
    const auto continuation = handle<VkSemaphore>(63);
    const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &continuation,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &applicationImage};
    auto fake = std::make_shared<InternalFake>();
    fake->lowerIndex = 5;
    fake->presentWait = handle<VkSemaphore>(97);
    const auto interval = 80ms;
    const auto now = DeferredPresent::Clock::now();
    auto generated = generatedCopy();
    generated.giveUp = 1s;
    expect(worker.beginBatch(2, {.overlap = true, .outputInterval = interval}),
        "internal copy-tail batch rejected");
    expect(worker.enqueueGeneratedCopy(swapchain, nullptr, now, generated) &&
        worker.enqueueInternalReal(info, now + 1ms, internalCopy(fake, 1)),
        "internal copy-tail outputs rejected");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 2 && completion->result == VK_SUCCESS &&
        !completion->calls[0].skipped && !completion->calls[1].skipped,
        "internal output after a generated copy was skipped");
    expect(completion->calls[1].started >= completion->calls[0].started + interval,
        "the real output after a worker generated copy lost its copy-tail spacing");
    expect(copyPresents.size() == 2 && copyPresents[0] == std::pair{2u, handle<VkSemaphore>(72)} &&
        copyPresents[1] == std::pair{5u, handle<VkSemaphore>(97)},
        "the generated copy and the internal real output were presented in the wrong order");
}

// (e) Teardown while an internal output polls ends with exactly one outcome: the
// output is presented or the application image is abandoned, never neither.
void internalRealTeardownPresentsOrAbandons() {
    for (unsigned mode = 0; mode < 2; ++mode) {
        auto queue = std::make_shared<DeferredPresentQueue>();
        queue->queue = handle<VkQueue>(7);
        copyPresents.clear();
        const auto swapchain = handle<VkSwapchainKHR>(11);
        const auto continuation = handle<VkSemaphore>(12);
        uint32_t applicationImage = 0;
        const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &continuation,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &applicationImage};
        auto fake = std::make_shared<InternalFake>();
        fake->notReadyPolls = 2;
        fake->lowerIndex = 4;
        fake->presentWait = handle<VkSemaphore>(98);
        if (mode == 1) fake->failure = VK_ERROR_SURFACE_LOST_KHR;
        {
            DeferredPresent worker(queue, recordingPresent);
            expect(worker.beginBatch(1), "teardown batch rejected");
            expect(worker.enqueueInternalReal(info, DeferredPresent::Clock::now(), internalCopy(fake, 0)),
                "teardown internal output rejected");
            worker.closeBatch();
        }
        const auto presented = copyPresents.size();
        const auto abandoned = static_cast<size_t>(fake->abandons);
        expect(presented + abandoned == 1, "teardown neither presented nor abandoned the polling output");
        expect(mode == 0 ? presented == 1 && fake->submits == 1 : abandoned == 1 && fake->submits == 0,
            "teardown ended the polling output with the wrong outcome");
    }
}

// (d) F9b: a real with a free lower image is acquired and copied when the worker takes
// it, before its deadline. The present still waits for the deadline, and the completion
// records the preparation.
void internalRealPreparedBeforeDeadline() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    copyPresents.clear();
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto continuation = handle<VkSemaphore>(12);
    uint32_t applicationImage = 1;
    const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &continuation,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &applicationImage};
    auto fake = std::make_shared<InternalFake>();
    fake->lowerIndex = 3;
    fake->presentWait = handle<VkSemaphore>(99);
    // A wide margin: the worker must reach the preparation before this deadline under load.
    const auto deadline = DeferredPresent::Clock::now() + 200ms;
    expect(worker.beginBatch(1), "prepared internal batch rejected");
    expect(worker.enqueueInternalReal(info, deadline, internalCopy(fake, 1)), "prepared internal real rejected");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 1 && completion->result == VK_SUCCESS &&
        !completion->calls[0].skipped, "prepared internal real was skipped or failed");
    expect(completion->calls[0].internalPrepared, "prepared internal real did not record its preparation");
    expect(fake->tryCalls == 1 && fake->submits == 1 && fake->abandons == 0,
        "preparation did not acquire and submit the lower image exactly once");
    expect(fake->submittedAt < deadline, "lower image was not acquired and copied before the deadline");
    expect(completion->calls[0].started >= deadline, "prepared internal real was presented before its deadline");
    expect(completion->calls[0].imageIndex == 3 && completion->calls[0].presentWait == handle<VkSemaphore>(99) &&
        copyPresents.size() == 1 && copyPresents[0] == std::pair{3u, handle<VkSemaphore>(99)},
        "prepared internal real was not presented once with the prepared lower index and presentWait");
}

// (e) F9b: without a lower image at preparation, the at-deadline poll acquires and copies,
// and the output is not marked prepared.
void internalRealNotPreparedPollsAtDeadline() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    copyPresents.clear();
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto continuation = handle<VkSemaphore>(12);
    uint32_t applicationImage = 0;
    const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &continuation,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &applicationImage};
    auto fake = std::make_shared<InternalFake>();
    fake->notReadyPolls = 1;
    fake->lowerIndex = 4;
    fake->presentWait = handle<VkSemaphore>(100);
    const auto deadline = DeferredPresent::Clock::now() + 20ms;
    expect(worker.beginBatch(1), "unprepared internal batch rejected");
    expect(worker.enqueueInternalReal(info, deadline, internalCopy(fake, 0)), "unprepared internal real rejected");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 1 && completion->result == VK_SUCCESS &&
        !completion->calls[0].skipped, "unprepared internal real was skipped or failed");
    expect(!completion->calls[0].internalPrepared, "an unprepared internal real was marked prepared");
    expect(fake->tryCalls == 2 && fake->submits == 1 && fake->abandons == 0,
        "the at-deadline poll did not follow the failed preparation");
    expect(fake->submittedAt >= deadline, "copy was submitted before the deadline without a lower image");
    expect(completion->calls[0].started >= deadline && copyPresents.size() == 1 &&
        copyPresents[0] == std::pair{4u, handle<VkSemaphore>(100)},
        "unprepared internal real was not presented once at its deadline");
}

// (f) F9b: a prepared real whose lower image was acquired is presented when stopping is
// set during its wait. No release exists for an acquired image.
void internalRealPreparedPresentsWhenStopping() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    copyPresents.clear();
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto continuation = handle<VkSemaphore>(12);
    uint32_t applicationImage = 0;
    const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &continuation,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &applicationImage};
    auto fake = std::make_shared<InternalFake>();
    fake->lowerIndex = 2;
    fake->presentWait = handle<VkSemaphore>(101);
    DeferredPresent worker(queue, recordingPresent);
    expect(worker.beginBatch(1), "stopping internal batch rejected");
    expect(worker.enqueueInternalReal(info, DeferredPresent::Clock::now() + 200ms, internalCopy(fake, 0)),
        "stopping internal real rejected");
    worker.closeBatch();
    for (int attempt = 0; attempt < 50000 && !fake->submitted.load(); ++attempt)
        std::this_thread::sleep_for(100us);
    expect(fake->submitted.load(), "preparation did not submit the lower copy before the stop");
    DeferredPresentTestAccess::requestStop(worker);
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 1 && completion->result == VK_SUCCESS &&
        !completion->calls[0].skipped, "a prepared real was abandoned while stopping");
    expect(completion->calls[0].internalPrepared && fake->tryCalls == 1 && fake->submits == 1 &&
        fake->abandons == 0, "a prepared real while stopping was not kept as prepared");
    expect(copyPresents.size() == 1 && copyPresents[0] == std::pair{2u, handle<VkSemaphore>(101)},
        "a prepared real while stopping was not presented with its lower image");
}

// (g) F9b: a preparation tryAcquire error abandons the application image once and settles
// the job: no wait, no second acquire, no present.
void internalRealPreparationErrorAbandonsOnce() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    copyPresents.clear();
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto continuation = handle<VkSemaphore>(12);
    uint32_t applicationImage = 2;
    const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &continuation,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &applicationImage};
    auto fake = std::make_shared<InternalFake>();
    fake->failure = VK_ERROR_OUT_OF_DATE_KHR;
    const auto deadline = DeferredPresent::Clock::now() + 1s;
    expect(worker.beginBatch(1), "failing preparation batch rejected");
    expect(worker.enqueueInternalReal(info, deadline, internalCopy(fake, 2)),
        "failing preparation output rejected");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 1 && completion->result == VK_ERROR_OUT_OF_DATE_KHR &&
        completion->calls[0].skipped && completion->calls[0].skipReason == DeferredPresent::SkipReason::Error &&
        completion->calls[0].result == VK_ERROR_OUT_OF_DATE_KHR,
        "a preparation error was not carried as an Error skip");
    expect(completion->calls[0].finished < deadline, "a settled preparation waited for its deadline");
    expect(fake->tryCalls == 1 && fake->abandons == 1 && fake->abandonedApplication == 2 &&
        fake->abandonedContinuation == continuation && fake->submits == 0 && copyPresents.empty(),
        "a preparation error did not abandon the application image exactly once without presenting");
}

// (h) F9b: a preparation submit error does not abandon: submitCopyLocked has already freed
// the image. The job is an Error skip with the submit result and the prepared lower index.
void internalRealPreparationSubmitErrorSkipsWithoutAbandon() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    copyPresents.clear();
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto continuation = handle<VkSemaphore>(12);
    uint32_t applicationImage = 3;
    const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &continuation,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &applicationImage};
    auto fake = std::make_shared<InternalFake>();
    fake->lowerIndex = 6;
    fake->submitResult = VK_ERROR_DEVICE_LOST;
    const auto deadline = DeferredPresent::Clock::now() + 1s;
    expect(worker.beginBatch(1), "failing submit batch rejected");
    expect(worker.enqueueInternalReal(info, deadline, internalCopy(fake, 3)),
        "failing submit output rejected");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 1 && completion->result == VK_ERROR_DEVICE_LOST &&
        completion->calls[0].skipped && completion->calls[0].skipReason == DeferredPresent::SkipReason::Error &&
        completion->calls[0].result == VK_ERROR_DEVICE_LOST && completion->calls[0].imageIndex == 6,
        "a preparation submit error was not carried as an Error skip with its lower index");
    expect(completion->calls[0].finished < deadline, "a settled submit error waited for its deadline");
    expect(fake->tryCalls == 1 && fake->submits == 1 && fake->abandons == 0 && copyPresents.empty(),
        "a preparation submit error abandoned the image, polled again or presented");
}

// (j) D6: the lock-hold split is recorded on the completion's call only with the pacing
// profile. Each field is the longest worker call under queue->mutex for that output.
void internalRealLockSplitIsProfileOnly() {
    for (unsigned mode = 0; mode < 2; ++mode) {
        auto queue = std::make_shared<DeferredPresentQueue>();
        queue->queue = handle<VkQueue>(7);
        DeferredPresent worker(queue, slowRecordingPresent);
        copyPresents.clear();
        const auto swapchain = handle<VkSwapchainKHR>(11);
        const auto continuation = handle<VkSemaphore>(12);
        uint32_t applicationImage = 0;
        const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &continuation,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &applicationImage};
        auto fake = std::make_shared<InternalFake>();
        fake->slow = true;
        fake->lowerIndex = 5;
        fake->presentWait = handle<VkSemaphore>(102);
        expect(worker.beginBatch(1, {.profile = mode == 1}), "lock split batch rejected");
        expect(worker.enqueueInternalReal(info, DeferredPresent::Clock::now() + 5ms, internalCopy(fake, 0)),
            "lock split internal real rejected");
        worker.closeBatch();
        const auto completion = worker.finish(UINT64_MAX);
        expect(completion && completion->count == 1 && !completion->calls[0].skipped,
            "lock split internal real was skipped or failed");
        const auto& call = completion->calls[0];
        if (mode == 0)
            expect(call.lockAcquireNs == 0 && call.lockSubmitNs == 0 && call.lockPresentNs == 0,
                "the lock-hold split was recorded without the pacing profile");
        else
            expect(call.lockAcquireNs > 0 && call.lockSubmitNs > 0 && call.lockPresentNs > 0,
                "the lock-hold split was not recorded on the completion's call with the pacing profile");
    }
}

// F9c: submitPrivate and the private queue's tail timeline. The fake records what the
// driver would receive for each submit, and returns the result the test scripts.
namespace {
struct PrivateSubmit {
    const VkSubmitInfo* info{};
    VkQueue queue{};
    uint32_t commands{};
    VkFence fence{};
    bool chained{};
    VkStructureType chain{};
    std::vector<VkSemaphore> waits, signals;
    std::vector<uint64_t> waitValues, signalValues;
};
std::vector<PrivateSubmit> privateSubmits;
VkResult privateSubmitResult{VK_SUCCESS};
VkResult VKAPI_CALL recordPrivateSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* info, VkFence fence) {
    expect(count == 1, "a private submit did not submit exactly one batch");
    PrivateSubmit record;
    record.info = info; record.queue = queue; record.commands = info->commandBufferCount; record.fence = fence;
    record.chained = info->pNext != nullptr;
    if (info->pNext) {
        std::memcpy(&record.chain, info->pNext, sizeof(record.chain));
        if (record.chain == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
            const auto* timeline = static_cast<const VkTimelineSemaphoreSubmitInfo*>(info->pNext);
            record.waitValues.assign(timeline->pWaitSemaphoreValues,
                timeline->pWaitSemaphoreValues + timeline->waitSemaphoreValueCount);
            record.signalValues.assign(timeline->pSignalSemaphoreValues,
                timeline->pSignalSemaphoreValues + timeline->signalSemaphoreValueCount);
        }
    }
    record.waits.assign(info->pWaitSemaphores, info->pWaitSemaphores + info->waitSemaphoreCount);
    record.signals.assign(info->pSignalSemaphores, info->pSignalSemaphores + info->signalSemaphoreCount);
    privateSubmits.push_back(std::move(record));
    return privateSubmitResult;
}
}

void privateSubmitWithoutTailIsUnchanged() {
    DeferredPresentQueue queue;
    queue.queue = handle<VkQueue>(7);
    privateSubmits.clear(); privateSubmitResult = VK_SUCCESS;
    const std::array waits{handle<VkSemaphore>(21)};
    const std::array signals{handle<VkSemaphore>(22)};
    const VkSubmitInfo info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = waits.data(),
        .signalSemaphoreCount = 1, .pSignalSemaphores = signals.data(),
    };
    expect(submitPrivate(queue, recordPrivateSubmit, info, handle<VkFence>(31)) == VK_SUCCESS,
        "a submit without a tail failed");
    expect(privateSubmits.size() == 1 && privateSubmits[0].info == &info &&
        privateSubmits[0].queue == handle<VkQueue>(7) && privateSubmits[0].fence == handle<VkFence>(31) &&
        !privateSubmits[0].chained && privateSubmits[0].signals == std::vector{handle<VkSemaphore>(22)},
        "a submit without a tail was not passed through unchanged");
    expect(queue.tailNext == 0 && queue.tailSubmitted.load() == 0,
        "a submit without a tail advanced a tail");
}

void privateSubmitAppendsTail() {
    DeferredPresentQueue queue;
    queue.queue = handle<VkQueue>(7);
    queue.tail = handle<VkSemaphore>(90);
    privateSubmits.clear(); privateSubmitResult = VK_SUCCESS;
    // A timeline signal at 5 and a binary signal, with one binary wait.
    const std::array waits{handle<VkSemaphore>(21)};
    const std::array signals{handle<VkSemaphore>(40), handle<VkSemaphore>(41)};
    const std::array signalValues{uint64_t{5}, uint64_t{0}};
    const VkTimelineSemaphoreSubmitInfo timeline{
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount = 2, .pSignalSemaphoreValues = signalValues.data(),
    };
    const VkSubmitInfo info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &timeline,
        .waitSemaphoreCount = 1, .pWaitSemaphores = waits.data(),
        .signalSemaphoreCount = 2, .pSignalSemaphores = signals.data(),
    };
    expect(submitPrivate(queue, recordPrivateSubmit, info, VK_NULL_HANDLE) == VK_SUCCESS,
        "a tailed submit failed");
    expect(privateSubmits.size() == 1, "a tailed submit was not made exactly once");
    const auto& record = privateSubmits.front();
    expect(record.signals == std::vector{handle<VkSemaphore>(40), handle<VkSemaphore>(41), handle<VkSemaphore>(90)} &&
        record.signalValues == std::vector<uint64_t>{5, 0, 1},
        "the tail was not appended after the existing signals with the value tailNext + 1");
    expect(record.waits == std::vector{handle<VkSemaphore>(21)} && record.waitValues.empty(),
        "the wait list of a tailed submit changed");
    expect(queue.tailNext == 1 && queue.tailSubmitted.load() == 1,
        "a successful tailed submit did not advance the tail");

    // Binary-only signals take value 0 and the tail takes the next value.
    const std::array binary{handle<VkSemaphore>(42)};
    const VkSubmitInfo second{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .signalSemaphoreCount = 1, .pSignalSemaphores = binary.data(),
    };
    expect(submitPrivate(queue, recordPrivateSubmit, second, VK_NULL_HANDLE) == VK_SUCCESS,
        "a binary tailed submit failed");
    expect(privateSubmits.size() == 2 &&
        privateSubmits[1].signals == std::vector{handle<VkSemaphore>(42), handle<VkSemaphore>(90)} &&
        privateSubmits[1].signalValues == std::vector<uint64_t>{0, 2},
        "a binary signal did not take value 0 with the tail at its next value");
    expect(queue.tailNext == 2 && queue.tailSubmitted.load() == 2,
        "the second tailed submit did not advance the tail");
}

void privateSubmitFailureLeavesTail() {
    DeferredPresentQueue queue;
    queue.queue = handle<VkQueue>(7);
    queue.tail = handle<VkSemaphore>(90);
    privateSubmits.clear();
    const std::array signals{handle<VkSemaphore>(40)};
    const VkSubmitInfo info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .signalSemaphoreCount = 1, .pSignalSemaphores = signals.data(),
    };
    privateSubmitResult = VK_ERROR_DEVICE_LOST;
    expect(submitPrivate(queue, recordPrivateSubmit, info, VK_NULL_HANDLE) == VK_ERROR_DEVICE_LOST,
        "a failed tailed submit reported success");
    expect(queue.tailNext == 0 && queue.tailSubmitted.load() == 0,
        "a failed tailed submit advanced the tail");
    privateSubmitResult = VK_SUCCESS;
    expect(submitPrivate(queue, recordPrivateSubmit, info, VK_NULL_HANDLE) == VK_SUCCESS &&
        privateSubmits.size() == 2 && privateSubmits[1].signalValues == std::vector<uint64_t>{0, 1} &&
        queue.tailNext == 1 && queue.tailSubmitted.load() == 1,
        "the retry did not reuse the tail value that was never published");
}

void privateSubmitForeignChainBypassesTail() {
    DeferredPresentQueue queue;
    queue.queue = handle<VkQueue>(7);
    queue.tail = handle<VkSemaphore>(90);
    privateSubmits.clear(); privateSubmitResult = VK_SUCCESS;
    const std::array signals{handle<VkSemaphore>(40)};
    const VkSemaphoreSignalInfo foreign{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    const VkSubmitInfo withForeign{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &foreign,
        .signalSemaphoreCount = 1, .pSignalSemaphores = signals.data(),
    };
    expect(submitPrivate(queue, recordPrivateSubmit, withForeign, VK_NULL_HANDLE) == VK_SUCCESS &&
        privateSubmits.size() == 1 && privateSubmits[0].info == &withForeign &&
        privateSubmits[0].signals == std::vector{handle<VkSemaphore>(40)},
        "a foreign pNext chain was not submitted unchanged");
    // A timeline whose own pNext is set is not understood either.
    const std::array nestedValues{uint64_t{3}};
    const VkTimelineSemaphoreSubmitInfo nested{
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .pNext = &foreign,
        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = nestedValues.data(),
    };
    const VkSubmitInfo withNested{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &nested,
        .signalSemaphoreCount = 1, .pSignalSemaphores = signals.data(),
    };
    expect(submitPrivate(queue, recordPrivateSubmit, withNested, VK_NULL_HANDLE) == VK_SUCCESS &&
        privateSubmits.size() == 2 && privateSubmits[1].info == &withNested &&
        privateSubmits[1].signals == std::vector{handle<VkSemaphore>(40)},
        "a timeline with its own pNext was not submitted unchanged");
    expect(queue.tailNext == 0 && queue.tailSubmitted.load() == 0,
        "an unsupported chain advanced the tail");
}

// D7b: true when no history record was written.
bool historyUnwritten(const DeferredPresentQueue& queue) {
    if (queue.recentNext != 0) return false;
    for (const auto& record : queue.recent)
        if (record.tail != 0) return false;
    return true;
}

// D7b: with history set, a successful tailed submit writes one record with its tail value,
// tag and wait count. Entries are written at recentNext % 16, so the 17th submit wraps to slot 0.
// D7c: the record also carries the tag's skip reason and source ordinal.
void privateSubmitRecordsHistory() {
    DeferredPresentQueue queue;
    queue.queue = handle<VkQueue>(7);
    queue.tail = handle<VkSemaphore>(90);
    queue.history = true;
    privateSubmits.clear(); privateSubmitResult = VK_SUCCESS;
    const std::array waits{handle<VkSemaphore>(21), handle<VkSemaphore>(23)};
    const std::array signals{handle<VkSemaphore>(40)};
    const VkSubmitInfo info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 2, .pWaitSemaphores = waits.data(),
        .signalSemaphoreCount = 1, .pSignalSemaphores = signals.data(),
    };
    const SubmitTag copy{.kind = submitKindCopy, .applicationIndex = 3, .lowerIndex = 1, .source = 42};
    expect(submitPrivate(queue, recordPrivateSubmit, info, handle<VkFence>(31), copy) == VK_SUCCESS,
        "a tagged submit with history failed");
    expect(queue.recentNext == 1 && queue.recent[0].tail == 1 &&
        queue.recent[0].kind == submitKindCopy && queue.recent[0].applicationIndex == 3 &&
        queue.recent[0].lowerIndex == 1 && queue.recent[0].waits == 2 &&
        queue.recent[0].fence == handle<VkFence>(31),
        "a successful submit did not write one record with its tail value, tag and wait count");
    expect(queue.recent[0].reason == 0 && queue.recent[0].source == 42,
        "a copy record did not carry its tag's source with no skip reason");
    for (size_t slot = 1; slot < queue.recent.size(); ++slot)
        expect(queue.recent[slot].tail == 0, "a successful submit wrote more than one record");

    // D7c: a generated skip records its reason and its batch ordinal.
    const SubmitTag skip{.kind = submitKindGeneratedSkip, .applicationIndex = UINT32_MAX,
        .lowerIndex = UINT32_MAX, .reason = static_cast<uint8_t>(DeferredPresent::SkipReason::Acquire),
        .source = 43};
    expect(submitPrivate(queue, recordPrivateSubmit, info, VK_NULL_HANDLE, skip) == VK_SUCCESS &&
        queue.recentNext == 2 && queue.recent[1].tail == 2 &&
        queue.recent[1].kind == submitKindGeneratedSkip &&
        queue.recent[1].reason == static_cast<uint8_t>(DeferredPresent::SkipReason::Acquire) &&
        queue.recent[1].source == 43,
        "a generated skip did not record its skip reason and source");

    // An untagged submit records unknown with no indices, reason or source. The 17th submit
    // overwrites slot 0.
    for (uint32_t submitted = 3; submitted <= 17; ++submitted)
        expect(submitPrivate(queue, recordPrivateSubmit, info, VK_NULL_HANDLE) == VK_SUCCESS,
            "an untagged submit with history failed");
    expect(queue.recentNext == 17 && queue.recent[0].tail == 17 &&
        queue.recent[0].kind == submitKindUnknown && queue.recent[0].applicationIndex == UINT32_MAX &&
        queue.recent[0].lowerIndex == UINT32_MAX && queue.recent[0].fence == VK_NULL_HANDLE &&
        queue.recent[0].reason == 0 && queue.recent[0].source == 0 &&
        queue.recent[1].tail == 2 && queue.recent[1].kind == submitKindGeneratedSkip &&
        queue.recent[1].source == 43,
        "the history did not wrap at recentNext % 16 with the newest record in its slot");
}

// D7c: the probe names skip reasons in SkipReason order, and an unknown value has its own name.
void skipReasonNamesFollowEnumOrder() {
    const std::array<const char*, 7> expected{
        "none", "readiness", "stopping", "expedite", "giveup", "acquire", "error"};
    for (size_t reason = 0; reason < expected.size(); ++reason)
        expect(std::strcmp(DeferredPresent::skipReasonName(static_cast<uint8_t>(reason)), expected[reason]) == 0,
            "a skip reason name is not in SkipReason order");
    expect(std::strcmp(DeferredPresent::skipReasonName(200), "unknown") == 0,
        "an out-of-range skip reason has no unknown name");
}

// D7b: a failed submit writes no history record, and the next successful one takes its slot.
void privateSubmitHistoryIgnoresFailure() {
    DeferredPresentQueue queue;
    queue.queue = handle<VkQueue>(7);
    queue.tail = handle<VkSemaphore>(90);
    queue.history = true;
    privateSubmits.clear();
    const std::array signals{handle<VkSemaphore>(40)};
    const VkSubmitInfo info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .signalSemaphoreCount = 1, .pSignalSemaphores = signals.data(),
    };
    const SubmitTag release{.kind = submitKindRelease, .applicationIndex = 2};
    privateSubmitResult = VK_ERROR_DEVICE_LOST;
    expect(submitPrivate(queue, recordPrivateSubmit, info, VK_NULL_HANDLE, release) == VK_ERROR_DEVICE_LOST,
        "a failed tagged submit reported success");
    expect(historyUnwritten(queue), "a failed submit wrote a history record");
    privateSubmitResult = VK_SUCCESS;
    expect(submitPrivate(queue, recordPrivateSubmit, info, VK_NULL_HANDLE, release) == VK_SUCCESS &&
        queue.recentNext == 1 && queue.recent[0].tail == 1 && queue.recent[0].kind == submitKindRelease &&
        queue.recent[0].applicationIndex == 2,
        "the retry after a failed submit did not record the release in slot 0");
}

// D7b: without history, a successful submit writes nothing and the tail still advances.
void privateSubmitWithoutHistoryRecordsNothing() {
    DeferredPresentQueue queue;
    queue.queue = handle<VkQueue>(7);
    queue.tail = handle<VkSemaphore>(90);
    privateSubmits.clear(); privateSubmitResult = VK_SUCCESS;
    const std::array signals{handle<VkSemaphore>(40)};
    const VkSubmitInfo info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .signalSemaphoreCount = 1, .pSignalSemaphores = signals.data(),
    };
    const SubmitTag copy{.kind = submitKindCopy, .applicationIndex = 3, .lowerIndex = 1};
    expect(submitPrivate(queue, recordPrivateSubmit, info, VK_NULL_HANDLE, copy) == VK_SUCCESS &&
        queue.tailNext == 1, "a submit without history did not succeed and advance the tail");
    expect(historyUnwritten(queue), "a submit without history wrote a history record");
}

void generatedCopyAddsPrivateTail() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    queue->tail = handle<VkSemaphore>(90);
    DeferredPresent worker(queue, recordingPresent);
    privateSubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0;
    copyAcquireResult = VK_SUCCESS; copyAcquireImage = 2; privateSubmitResult = VK_SUCCESS;
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto realWait = handle<VkSemaphore>(12);
    uint32_t realImage = 4;
    const VkPresentInfoKHR realInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
    auto copy = generatedCopy();
    copy.submit = recordPrivateSubmit;
    const auto deadline = DeferredPresent::Clock::now();
    expect(worker.beginBatch(2), "tailed generated copy batch rejected");
    expect(worker.enqueueGeneratedCopy(swapchain, nullptr, deadline, copy), "tailed generated copy rejected");
    expect(worker.enqueue(realInfo, deadline + 1ms), "real output rejected after tailed generated copy");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 2 && completion->result == VK_SUCCESS &&
        !completion->calls[0].skipped && !completion->calls[1].skipped,
        "tailed generated copy was skipped or changed the batch result");
    expect(privateSubmits.size() == 1, "tailed generated copy was not submitted exactly once");
    const auto& submit = privateSubmits.front();
    expect(submit.queue == handle<VkQueue>(7) && submit.commands == 1 && submit.fence == handle<VkFence>(64) &&
        submit.waits == std::vector{handle<VkSemaphore>(61), handle<VkSemaphore>(62)} &&
        submit.signals == std::vector{handle<VkSemaphore>(72), handle<VkSemaphore>(63), handle<VkSemaphore>(90)} &&
        submit.signalValues == std::vector<uint64_t>{0, 0, 1},
        "tailed generated copy did not append the tail after its present and continuation signals");
    expect(queue->tailNext == 1 && queue->tailSubmitted.load() == 1,
        "tailed generated copy did not advance the tail");
    expect(copyPresents.size() == 2 && copyPresents[0] == std::pair{2u, handle<VkSemaphore>(72)} &&
        copyPresents[1] == std::pair{4u, realWait},
        "tailed generated copy changed the presentation order");
}

void generatedSkipAddsPrivateTail() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    queue->tail = handle<VkSemaphore>(90);
    DeferredPresent worker(queue, recordingPresent);
    privateSubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0;
    copyAcquireResult = VK_NOT_READY; privateSubmitResult = VK_SUCCESS;
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto realWait = handle<VkSemaphore>(12);
    uint32_t realImage = 4;
    const VkPresentInfoKHR realInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
    auto copy = generatedCopy();
    copy.submit = recordPrivateSubmit;
    const auto deadline = DeferredPresent::Clock::now();
    expect(worker.beginBatch(2), "tailed skip batch rejected");
    expect(worker.enqueueGeneratedCopy(swapchain, nullptr, deadline, copy), "tailed skip copy rejected");
    expect(worker.enqueue(realInfo, deadline + 1ms), "real output rejected after tailed skip");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 2 && completion->calls[0].skipped &&
        completion->calls[0].result == VK_SUCCESS && !completion->calls[1].skipped,
        "tailed skip was not a successful skip ahead of the real output");
    expect(privateSubmits.size() == 1, "tailed skip was not submitted exactly once");
    const auto& submit = privateSubmits.front();
    expect(submit.commands == 0 && submit.fence == handle<VkFence>(64) &&
        submit.waits == std::vector{handle<VkSemaphore>(62)} &&
        submit.signals == std::vector{handle<VkSemaphore>(63), handle<VkSemaphore>(90)} &&
        submit.signalValues == std::vector<uint64_t>{0, 1},
        "tailed skip did not signal the continuation with the tail");
    expect(queue->tailNext == 1 && queue->tailSubmitted.load() == 1,
        "tailed skip did not advance the tail");
    expect(copyPresents.size() == 1 && copyPresents[0] == std::pair{4u, realWait},
        "tailed skip presented the skipped image or lost the real output");
    copyAcquireResult = VK_SUCCESS;
}

// D7d: true when no lower record was written.
bool lowerUnwritten(const DeferredPresentQueue& queue) {
    if (queue.lowerNext != 0) return false;
    for (const auto& record : queue.lower)
        if (record.startNs != 0) return false;
    return true;
}

// D7d: noteLower writes nothing without history. With history it writes one record with the current
// tail value, and the 25th record wraps into slot 0 of the 24 entries.
void lowerRecordsFollowHistory() {
    DeferredPresentQueue queue;
    queue.queue = handle<VkQueue>(7);
    const auto swapchain = handle<VkSwapchainKHR>(11);
    queue.noteLower(lowerOpPresentReal, swapchain, 3, VK_SUCCESS, 42, 100, 150);
    expect(lowerUnwritten(queue), "a lower record was written without history");
    queue.history = true;
    queue.tailNext = 5;
    queue.noteLower(lowerOpPresentReal, swapchain, 3, VK_SUCCESS, 42, 100, 150);
    const LowerRecord& first = queue.lower[0];
    expect(queue.lowerNext == 1 && first.tail == 5 && first.op == lowerOpPresentReal &&
        first.lowerIndex == 3 && first.source == 42 && first.swapchain == 11 &&
        first.result == VK_SUCCESS && first.startNs == 100 && first.endNs == 150,
        "a noted lower call did not record its op, lower index, source, swapchain, tail and times");
    for (int64_t start = 101; start <= 124; ++start)
        queue.noteLower(lowerOpAcquireReal, swapchain, UINT32_MAX, VK_SUCCESS, 0, start, start + 1);
    expect(queue.lowerNext == 25 && queue.lower[0].startNs == 124 && queue.lower[1].startNs == 101 &&
        queue.lower[0].op == lowerOpAcquireReal,
        "the 25th lower record did not wrap into slot 0");
}

// D7d: beginCall and endCall set and clear the in-flight op, and do nothing without history.
void inFlightCallFollowsHistory() {
    DeferredPresentQueue queue;
    queue.beginCall(lowerOpPresentReal, 3);
    expect(queue.callOp.load() == lowerOpNone, "an in-flight call was noted without history");
    queue.history = true;
    queue.beginCall(lowerOpPresentReal, 3);
    expect(queue.callOp.load() == lowerOpPresentReal && queue.callLower.load() == 3 &&
        queue.callSinceNs.load() > 0, "beginCall did not set the op, lower index and start");
    queue.endCall();
    expect(queue.callOp.load() == lowerOpNone, "endCall did not clear the in-flight op");
}

// D7d: a synchronous present reaches the queue either way. With history it leaves one
// lowerOpPresentSync record with its lower index and swapchain, and no call in flight.
void synchronousPresentIsNoted() {
    DeferredPresentQueue queue;
    queue.queue = handle<VkQueue>(7);
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto wait = handle<VkSemaphore>(12);
    uint32_t image = 4;
    const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    copyPresents.clear();
    expect(queue.presentNow(recordingPresent, info) == VK_SUCCESS && copyPresents.size() == 1 &&
        copyPresents[0] == std::pair{4u, wait}, "a synchronous present did not reach the queue");
    expect(lowerUnwritten(queue), "a synchronous present was noted without history");
    queue.history = true;
    expect(queue.presentNow(recordingPresent, info) == VK_SUCCESS && copyPresents.size() == 2 &&
        copyPresents[1] == std::pair{4u, wait}, "a noted synchronous present did not reach the queue");
    const LowerRecord& record = queue.lower[0];
    expect(queue.lowerNext == 1 && record.op == lowerOpPresentSync && record.lowerIndex == 4 &&
        record.swapchain == 11 && record.source == 0 && record.result == VK_SUCCESS &&
        record.startNs > 0 && record.endNs >= record.startNs && queue.callOp.load() == lowerOpNone,
        "a synchronous present did not record one lowerOpPresentSync entry");
}

// D7d: a generated output and an internal real output each leave one lower record, with their
// lower index and source, when history is set. Without history nothing is recorded.
void presentsAreNotedWithHistory() {
    for (const bool history : {false, true}) {
        auto queue = std::make_shared<DeferredPresentQueue>();
        queue->queue = handle<VkQueue>(7);
        queue->history = history;
        DeferredPresent worker(queue, recordingPresent);
        copyPresents.clear();
        const auto swapchain = handle<VkSwapchainKHR>(11);
        const auto generatedWait = handle<VkSemaphore>(12);
        const auto realWait = handle<VkSemaphore>(13);
        uint32_t generatedImage = 5;
        uint32_t realImage = 4;
        const VkPresentInfoKHR generatedInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &generatedWait,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &generatedImage};
        const VkPresentInfoKHR realInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
        auto fake = std::make_shared<InternalFake>();
        fake->lowerIndex = 6;
        fake->presentWait = handle<VkSemaphore>(95);
        DeferredPresent::Readiness ready;
        ready.source = 77;
        const auto deadline = DeferredPresent::Clock::now();
        expect(worker.beginBatch(2, ready), "noted batch rejected");
        expect(worker.enqueue(generatedInfo, deadline, true), "noted generated output rejected");
        expect(worker.enqueueInternalReal(realInfo, deadline, internalCopy(fake, 1)),
            "noted internal real output rejected");
        worker.closeBatch();
        const auto completion = worker.finish(UINT64_MAX);
        expect(completion && completion->count == 2 && completion->result == VK_SUCCESS,
            "noted batch was skipped or failed");
        expect(copyPresents.size() == 2 && copyPresents[0] == std::pair{5u, generatedWait} &&
            copyPresents[1] == std::pair{6u, handle<VkSemaphore>(95)},
            "noted outputs were not presented once each with their lower image and wait");
        if (!history) {
            expect(lowerUnwritten(*queue), "outputs were noted without history");
            continue;
        }
        expect(queue->lowerNext == 2, "noted outputs did not leave one record each");
        const LowerRecord& generated = queue->lower[0];
        const LowerRecord& real = queue->lower[1];
        expect(generated.op == lowerOpPresentGenerated && generated.lowerIndex == 5 &&
            generated.source == 77 && generated.swapchain == 11 && generated.result == VK_SUCCESS,
            "the generated output record lacks its op, lower index, source or swapchain");
        expect(real.op == lowerOpPresentReal && real.lowerIndex == 6 &&
            real.source == 77 && real.swapchain == 11 && real.result == VK_SUCCESS,
            "the internal real record lacks its op, lower index, source or swapchain");
    }
}

namespace {
// One lower image pool shared by generated and internal-real callbacks. These
// tests measure outstanding acquisitions, submission times and lower order.
struct PreparedPool {
    uint32_t next{}, held{}, peak{};
    unsigned attempts{}, misses{};
    bool failAcquire{}, failSubmit{};
    VkResult presentResult{VK_SUCCESS};
    std::vector<uint32_t> order;
    std::vector<DeferredPresent::Clock::time_point> acquiredAt, copiedAt;
    std::atomic_uint submitted{};
    std::atomic_bool holdSubmission{}, releaseSubmission{};
};
PreparedPool* preparedPool{};
VkResult poolAcquire(uint32_t* image) {
    auto& pool = *preparedPool;
    ++pool.attempts;
    if (pool.failAcquire) return VK_ERROR_OUT_OF_DATE_KHR;
    if (pool.misses) { --pool.misses; return VK_NOT_READY; }
    *image = pool.next++;
    ++pool.held;
    pool.peak = std::max(pool.peak, pool.held);
    expect(pool.held <= layerHeldLowerImages, "early copies exceeded the lower-image budget");
    pool.acquiredAt.push_back(DeferredPresent::Clock::now());
    return VK_SUCCESS;
}
VkResult VKAPI_CALL preparedAcquire(VkDevice, VkSwapchainKHR, uint64_t timeout,
        VkSemaphore, VkFence, uint32_t* image) {
    expect(timeout == 0, "early generated acquire blocked");
    return poolAcquire(image);
}
VkResult VKAPI_CALL preparedSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* info, VkFence fence) {
    if (preparedPool->failSubmit && info->commandBufferCount) {
        preparedPool->failSubmit = false;
        return VK_ERROR_DEVICE_LOST;
    }
    const auto result = copySubmit(queue, count, info, fence);
    preparedPool->copiedAt.push_back(DeferredPresent::Clock::now());
    ++preparedPool->submitted;
    if (preparedPool->holdSubmission.load()) {
        const auto limit = DeferredPresent::Clock::now() + 2s;
        while (!preparedPool->releaseSubmission.load() && DeferredPresent::Clock::now() < limit)
            std::this_thread::sleep_for(100us);
        expect(preparedPool->releaseSubmission.load(), "test did not release the preparation callback");
    }
    return result;
}
VkResult VKAPI_CALL preparedPresent(VkQueue, const VkPresentInfoKHR* info) {
    auto& pool = *preparedPool;
    expect(pool.held > 0, "prepared present returned an unacquired lower image");
    --pool.held;
    pool.order.push_back(info->pImageIndices[0]);
    return pool.presentResult;
}
VkResult poolRealAcquire(void*, uint32_t* image) { return poolAcquire(image); }
VkResult poolRealSubmit(void*, VkQueue, uint32_t, uint32_t, VkSemaphore, VkSemaphore* wait) {
    *wait = handle<VkSemaphore>(100);
    return VK_SUCCESS;
}
VkResult poolRealAbandon(void*, VkQueue, uint32_t, VkSemaphore) { return VK_SUCCESS; }
DeferredPresent::GeneratedCopy preparedCopy() {
    auto copy = generatedCopy();
    copy.acquire = preparedAcquire;
    copy.submit = preparedSubmit;
    copy.internalImages = true;
    copy.imageCount = 8;
    for (uint32_t i = 0; i < copy.imageCount; ++i) {
        copy.copies[i] = handle<VkCommandBuffer>(80 + i);
        copy.presentWaits[i] = handle<VkSemaphore>(70 + i);
    }
    return copy;
}
// At 2x, 3x and 4x only the next output is prepared. Each lower present returns
// its image before the next acquire; the following internal real uses that pool.
void generatedPreparedOrderAndBudget() {
    for (const size_t multiplier : {2u, 3u, 4u}) {
        auto pool = std::make_shared<PreparedPool>();
        preparedPool = pool.get();
        copySubmits.clear();
        auto queue = std::make_shared<DeferredPresentQueue>();
        queue->queue = handle<VkQueue>(7);
        DeferredPresent worker(queue, preparedPresent);
        const auto swapchain = handle<VkSwapchainKHR>(11);
        const auto wait = handle<VkSemaphore>(12);
        const uint32_t image = 4;
        const VkPresentInfoKHR real{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
        const auto first = DeferredPresent::Clock::now() + 60ms;
        expect(worker.beginBatch(multiplier, {.overlap = true, .outputInterval = 10ms}),
            "prepared multiplier batch rejected");
        for (size_t i = 0; i + 1 < multiplier; ++i)
            expect(worker.enqueueGeneratedCopy(swapchain, nullptr, first + 10ms * i, preparedCopy()),
                "prepared generated output rejected");
        const DeferredPresent::InternalRealCopy realCopy{.owner = pool, .tryAcquire = poolRealAcquire,
            .submit = poolRealSubmit, .abandon = poolRealAbandon};
        expect(worker.enqueueInternalReal(real, first + 10ms * (multiplier - 1), realCopy),
            "prepared internal real rejected");
        worker.closeBatch();
        const auto done = worker.finish(UINT64_MAX, false);
        expect(done && done->count == multiplier && done->result == VK_SUCCESS,
            "prepared multiplier batch failed");
        expect(done->readyAt == DeferredPresent::Clock::time_point{} &&
            done->slack == DeferredPresent::Clock::duration{},
            "a batch without a readiness wait reported readiness or margin data");
        expect(pool->held == 0 && pool->peak <= 2 && pool->order.size() == multiplier,
            "prepared multiplier batch leaked or over-acquired images");
        for (size_t i = 0; i < multiplier; ++i) {
            expect(pool->order[i] == i && !done->calls[i].skipped,
                "prepared lower outputs changed order");
            if (i + 1 < multiplier)
                expect(done->calls[i].generatedPrepared &&
                    pool->copiedAt[i] < done->calls[i].scheduled &&
                    done->calls[i].started >= done->calls[i].scheduled,
                    "generated copy was not submitted before its unchanged present slot");
        }
        expect(copySubmits.size() == multiplier - 1,
            "prepared generated work was submitted more than once");
    }
}
// W1: in a three-output batch the automatic real offset moves only the real output. The second
// generated output keeps its spacing after the first (its move is under 1 ms), and the real starts
// at least a full offset (1 ms) after it, plus the real's own spacing of 10 ms.
void generatedChainRealOffsetMovesOnlyTheReal() {
    auto pool = std::make_shared<PreparedPool>();
    preparedPool = pool.get();
    copySubmits.clear();
    realOffsetAutoConfigured.store(true);
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, preparedPresent);
    DeferredPresentTestAccess::setRealOffset(worker, std::chrono::microseconds{1000});
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto wait = handle<VkSemaphore>(12);
    const uint32_t image = 4;
    const VkPresentInfoKHR real{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    const auto first = DeferredPresent::Clock::now() + 60ms;
    expect(worker.beginBatch(3, {.overlap = true, .outputInterval = 10ms}), "offset batch rejected");
    expect(worker.enqueueGeneratedCopy(swapchain, nullptr, first, preparedCopy()) &&
        worker.enqueueGeneratedCopy(swapchain, nullptr, first + 10ms, preparedCopy()),
        "offset batch generated outputs rejected");
    const DeferredPresent::InternalRealCopy realCopy{.owner = pool, .tryAcquire = poolRealAcquire,
        .submit = poolRealSubmit, .abandon = poolRealAbandon};
    expect(worker.enqueueInternalReal(real, first + 20ms, realCopy), "offset batch real rejected");
    worker.closeBatch();
    const auto done = worker.finish(UINT64_MAX, false);
    realOffsetAutoConfigured.store(false);
    expect(done && done->count == 3 && done->result == VK_SUCCESS &&
        !done->calls[0].skipped && !done->calls[1].skipped && !done->calls[2].skipped,
        "offset batch failed");
    expect(done->calls[1].scheduled - (first + 10ms) < 1ms,
        "the second generated output was moved by the real offset");
    expect(done->calls[2].scheduled - done->calls[1].scheduled >= 11ms,
        "the real output did not take the real offset");
}
// A failed early acquire must not consume staged work. The existing deadline
// acquire then submits exactly once. The direct and non-overlap gates do not try early.
void generatedPreparationFallbackAndGates(bool disabled = false) {
    for (unsigned mode = disabled ? 3 : 0; mode < (disabled ? 4u : 3u); ++mode) {
        PreparedPool pool;
        preparedPool = &pool;
        if (mode == 0) pool.misses = 1;
        copySubmits.clear();
        auto queue = std::make_shared<DeferredPresentQueue>();
        queue->queue = handle<VkQueue>(7);
        DeferredPresent worker(queue, preparedPresent);
        auto copy = preparedCopy();
        if (mode == 1) copy.internalImages = false;
        const auto due = DeferredPresent::Clock::now() + 60ms;
        expect(worker.beginBatch(1, {.overlap = mode != 2}), "fallback batch rejected");
        expect(worker.enqueueGeneratedCopy(handle<VkSwapchainKHR>(11), nullptr, due, copy),
            "fallback generated output rejected");
        worker.closeBatch();
        const auto done = worker.finish(UINT64_MAX, false);
        expect(done && done->count == 1 && !done->calls[0].skipped &&
            !done->calls[0].generatedPrepared && pool.held == 0 &&
            pool.attempts == (mode == 0 ? 2u : 1u) && copySubmits.size() == 1 &&
            pool.acquiredAt[0] >= due, "fallback or preparation gate changed deadline delivery");
    }
}
// Retirement drains the worker before resources die (the destructor is that
// drain). An out-of-date lower present still consumes the prepared image once.
void generatedPreparedRetirement() {
    PreparedPool pool;
    preparedPool = &pool;
    pool.presentResult = VK_ERROR_OUT_OF_DATE_KHR;
    copySubmits.clear();
    {
        auto queue = std::make_shared<DeferredPresentQueue>();
        queue->queue = handle<VkQueue>(7);
        DeferredPresent worker(queue, preparedPresent);
        expect(worker.beginBatch(1, {.overlap = true}), "retiring batch rejected");
        expect(worker.enqueueGeneratedCopy(handle<VkSwapchainKHR>(11), nullptr,
            DeferredPresent::Clock::now() + 60ms, preparedCopy()), "retiring copy rejected");
        worker.closeBatch();
        const auto limit = DeferredPresent::Clock::now() + 2s;
        while (!pool.submitted.load() && DeferredPresent::Clock::now() < limit)
            std::this_thread::sleep_for(100us);
        expect(pool.submitted.load() == 1, "retirement did not reach early preparation");
    }
    expect(pool.held == 0 && pool.order.size() == 1 && copySubmits.size() == 1,
        "retirement lost or duplicated a prepared image");
}
void generatedPreparedStop() {
    PreparedPool pool;
    pool.holdSubmission = true;
    preparedPool = &pool;
    copySubmits.clear();
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, preparedPresent);
    expect(worker.beginBatch(1, {.expediteOnJoin = true, .overlap = true}), "stop batch rejected");
    expect(worker.enqueueGeneratedCopy(handle<VkSwapchainKHR>(11), nullptr,
        DeferredPresent::Clock::now() + 2s, preparedCopy()), "stop copy rejected");
    worker.closeBatch();
    const auto limit = DeferredPresent::Clock::now() + 2s;
    while (!pool.submitted.load() && DeferredPresent::Clock::now() < limit)
        std::this_thread::sleep_for(100us);
    expect(pool.submitted.load() == 1, "stop did not reach early preparation");
    DeferredPresentTestAccess::requestStop(worker);
    pool.releaseSubmission = true;
    const auto done = worker.finish(1'000'000'000, false);
    expect(done && done->count == 1 && done->calls[0].generatedPrepared &&
        !done->calls[0].skipped && pool.held == 0 && pool.order.size() == 1 && copySubmits.size() == 1,
        "stop stranded a prepared image or failed to finish");
}
// S7: the real offset moves a tail real by the offset, keeps a quarter interval after the
// generated present, and changes nothing when it is zero.
namespace real_offset_checks {
using Ns = std::chrono::nanoseconds;
using Tp = std::chrono::time_point<std::chrono::steady_clock, Ns>;
constexpr Tp at(const int64_t us) { return Tp{std::chrono::microseconds{us}}; }
constexpr Ns us(const int64_t value) { return std::chrono::microseconds{value}; }
static_assert(realDeadlineWithOffset(at(6500), at(0), us(6500), us(0)) == at(6500));
static_assert(realDeadlineWithOffset(at(6500), at(0), us(6500), us(-550)) == at(5950));
static_assert(realDeadlineWithOffset(at(6500), at(0), us(6500), us(400)) == at(6900));
// Short interval: the quarter-interval bound holds.
static_assert(realDeadlineWithOffset(at(2000), at(0), us(2000), us(-1000)) == at(1000));
static_assert(realDeadlineWithOffset(at(1000), at(0), us(2000), us(-1000)) == at(500));
// A deadline already inside the bound is not pushed later by a negative offset.
static_assert(realDeadlineWithOffset(at(300), at(0), us(2000), us(-1000)) == at(300));
}

// Errors must consume the staging/continuation contract once without retrying
// an acquire or submitting a failed copy twice.
void generatedPreparationErrors() {
    for (const bool submitFailure : {false, true}) {
        PreparedPool pool;
        preparedPool = &pool;
        pool.failAcquire = !submitFailure;
        pool.failSubmit = submitFailure;
        copySubmits.clear();
        auto queue = std::make_shared<DeferredPresentQueue>();
        queue->queue = handle<VkQueue>(7);
        DeferredPresent worker(queue, preparedPresent);
        expect(worker.beginBatch(1, {.overlap = true}), "error preparation batch rejected");
        expect(worker.enqueueGeneratedCopy(handle<VkSwapchainKHR>(11), nullptr,
            DeferredPresent::Clock::now() + 60ms, preparedCopy()), "error preparation rejected");
        worker.closeBatch();
        const auto done = worker.finish(UINT64_MAX, false);
        expect(done && done->count == 1 && done->calls[0].skipped && done->result < 0 &&
            pool.attempts == 1 && pool.order.empty() && copySubmits.size() == 1 &&
            copySubmits[0].count == 0 && copySubmits[0].waits == (submitFailure ? 2u : 1u) &&
            copySubmits[0].signals == 1, "early error retried or double-consumed staged work");
        // As in the old submit-error path, device-loss cleanup owns the acquired
        // lower image when no copy could be submitted; no present waits on it.
    }
}
}

namespace {
// S8b: markers of real k-1, generated k, real k at a 13 ms source period. The generated
// output lands `bias` after the midpoint of the unmoved reals; each real lands at its slot
// plus the controller's current offset (its response). Noise moves reals in pairs and the
// generated output alternately, so the midpoint itself moves.
constexpr bool realOffsetSimulation(std::chrono::nanoseconds bias, bool noisy = false) {
    using namespace std::chrono_literals;
    RealOffsetController controller;
    constexpr std::chrono::nanoseconds period = 13ms; // nanoseconds: half of it is exact
    const auto realAt = [&](int k) {
        return period * k + controller.offset + (noisy ? ((k / 2) % 2 ? 500us : -500us) : 0us);
    };
    (void)controller.observe(false, 0, realAt(0));
    for (int k = 1; k <= 400; ++k) {
        const auto generated = period * k - period / 2 + bias + (noisy ? (k % 2 ? 300us : -300us) : 0us);
        (void)controller.observe(true, static_cast<uint64_t>(k), generated);
        if (!controller.observe(false, static_cast<uint64_t>(k), realAt(k))) return false;
        if (controller.offset > RealOffsetController::limit || controller.offset < -RealOffsetController::limit)
            return false;
    }
    const auto miss = controller.offset - bias;
    return controller.accepted == 400 && miss <= 50us && miss >= -50us;
}
static_assert(realOffsetSimulation(500us));
static_assert(realOffsetSimulation(-500us));
static_assert(realOffsetSimulation(0us));
static_assert(realOffsetSimulation(400us, true));
static_assert(realOffsetSimulation(-600us, true));
static_assert([] {
    using namespace std::chrono_literals;
    using C = RealOffsetController;
    C c;
    // A sample needs real k-1, generated k, real k in that order (midpoint = real k-1 + 6.5 ms).
    (void)c.observe(false, 1, 13ms);
    if (c.observe(false, 2, 26ms)) return false;                     // no generated
    (void)c.observe(true, 3, 32ms);
    if (c.observe(false, 4, 39ms)) return false;                     // generated of another source
    (void)c.observe(true, 6, 45ms);
    if (c.observe(false, 6, 52ms)) return false;                     // source gap 4 -> 6
    (void)c.observe(true, 7, 70ms);
    if (c.observe(false, 7, 78ms)) return false;                     // step 26 ms > 25
    (void)c.observe(true, 8, 81ms);
    if (c.observe(false, 8, 85ms)) return false;                     // step 7 ms < 8
    (void)c.observe(true, 9, 85ms + 6500us + 3500us);
    if (c.observe(false, 9, 98ms)) return false;                     // error +3.5 ms
    (void)c.observe(true, 10, 98ms + 6500us - 3500us);
    if (c.observe(false, 10, 111ms)) return false;                   // error -3.5 ms
    (void)c.observe(true, 11, 112ms);
    if (c.observe(false, 11, 111ms)) return false;                   // real not after generated
    if (c.offset != 0ns || c.accepted != 0) return false;
    (void)c.observe(true, 12, 111ms + 6500us + 3ms);
    if (!c.observe(false, 12, 124ms)) return false;                  // error +3 ms, clamped to 1 ms
    if (c.offset != C::Duration{1ms} / C::gain || c.lastError != 3ms) return false;
    c.forget();
    (void)c.observe(true, 13, 130ms);
    if (c.observe(false, 13, 137ms)) return false;                   // history dropped
    if (c.offset != C::Duration{1ms} / C::gain) return false;                     // offset kept
    for (int k = 14; k < 400; ++k) {                                 // +1 ms each: saturates
        const auto previous = 137ms + 13ms * (k - 14);
        (void)c.observe(true, static_cast<uint64_t>(k), previous + 6500us + 1ms);
        if (!c.observe(false, static_cast<uint64_t>(k), previous + 13ms)) return false;
    }
    return c.offset == C::limit;
}());
// S8d: a real the join expedite started before its moved deadline took only part of the move.
namespace real_offset_checks {
static_assert(realOffsetEffective(us(1000), at(4300), at(5300), at(4600)) == us(300));
static_assert(realOffsetEffective(us(1000), at(4300), at(5300), at(4000)) == us(0));     // before unmoved
static_assert(realOffsetEffective(us(1000), at(4300), at(5300), at(5400)) == us(1000));  // late wake
static_assert(realOffsetEffective(us(-500), at(4300), at(3800), at(3000)) == us(-500));  // earlier: kept
static_assert(realOffsetEffective(us(0), at(4300), at(4300), at(4000)) == us(0));
static_assert(realOffsetLimited(us(1000), us(300)) == 1);
static_assert(realOffsetLimited(us(1000), us(960)) == 0);
static_assert(realOffsetLimited(us(-600), us(-200)) == -1);
static_assert(realOffsetLimited(us(-600), us(-600)) == 0);
static_assert(realOffsetLimited(us(0), us(0)) == 0);
}
// S8d: while limited one way, samples pushing that way are held; the other way integrates.
static_assert([] {
    using namespace std::chrono_literals;
    using C = RealOffsetController;
    C c;
    const auto sample = [&](int k, std::chrono::nanoseconds error) {
        const auto previous = 13ms * k;
        (void)c.observe(false, static_cast<uint64_t>(k), previous);
        (void)c.observe(true, static_cast<uint64_t>(k + 1), previous + 6500us + error);
        return c.observe(false, static_cast<uint64_t>(k + 1), previous + 13ms);
    };
    if (!sample(1, 640us) || c.offset != C::Duration{10us}) return false;
    c.limited = 1;
    for (int k = 3; k < 300; k += 2)
        if (sample(k, 640us)) return false;                          // held, never accepted
    if (c.offset != C::Duration{10us} || c.held != 149 || c.accepted != 1) return false;
    if (!sample(301, -640us) || c.offset != 0ns) return false;       // opposite sign integrates
    c.limited = -1;
    if (sample(303, -640us) || !sample(305, 640us)) return false;
    c.forget();
    if (c.limited != -1) return false;                               // forget keeps it
    c.limited = 0;
    return sample(307, 640us) && c.offset == C::Duration{20us} && c.held == 150;
}());
// W1: N - 1 generated outputs per source, evenly placed between two reals 12 ms apart. Their mean
// is the midpoint for every N, so `late` moves the mean by exactly `late`. `dropLast` omits the
// last generated output of each source.
constexpr RealOffsetController evenlySpacedReals(uint32_t expected, std::chrono::nanoseconds late,
        bool dropLast = false) {
    using namespace std::chrono_literals;
    RealOffsetController c;
    c.expectedGenerated = expected;
    constexpr std::chrono::nanoseconds period = 12ms;
    const auto step = period / static_cast<int64_t>(expected + 1);
    (void)c.observe(false, 0, std::chrono::nanoseconds{});
    for (int k = 1; k <= 20; ++k) {
        const auto previous = period * (k - 1);
        for (uint32_t j = 1; j <= expected; ++j)
            if (!dropLast || j < expected)
                (void)c.observe(true, static_cast<uint64_t>(k), previous + step * j + late);
        (void)c.observe(false, static_cast<uint64_t>(k), period * k);
    }
    return c;
}
static_assert([] {
    using namespace std::chrono_literals;
    using C = RealOffsetController;
    for (const uint32_t expected : {2u, 4u}) {
        const auto even = evenlySpacedReals(expected, 0ns);
        if (even.accepted != 20 || even.offset != 0ns || even.lastError != 0ns) return false;
        const auto late = evenlySpacedReals(expected, 2ms);
        if (late.accepted != 20 || late.lastError != 2ms || late.offset != 20 * (C::Duration{1ms} / C::gain))
            return false;
        const auto fewer = evenlySpacedReals(expected, 2ms, true);
        if (fewer.accepted != 0 || fewer.offset != 0ns) return false;   // one marker short: no sample
    }
    // expectedGenerated 1 is today's rule: a second generated marker of the same source gives no sample.
    C c;
    (void)c.observe(false, 0, 0ms);
    (void)c.observe(true, 1, 4ms);
    (void)c.observe(true, 1, 8ms);
    if (c.observe(false, 1, 12ms) || c.accepted != 0) return false;
    return true;
}());
// S8d: the cap-bound windup. The real cannot start later than `reach` after its unmoved slot
// (the next source present joins), so the error stays at `bias - reach` once the offset passes
// `reach`. With the hold the offset stops at the reach; without it, it runs to the limit.
constexpr std::chrono::nanoseconds realOffsetWindup(bool hold) {
    using namespace std::chrono_literals;
    RealOffsetController c;
    constexpr std::chrono::nanoseconds period = 8600us, bias = 650us, reach = 200us;
    for (int k = 1; k <= 600; ++k) {
        const auto effective = std::min(c.offset, reach);
        const auto previous = period * k;
        (void)c.observe(false, static_cast<uint64_t>(k), previous + effective);
        (void)c.observe(true, static_cast<uint64_t>(k + 1), previous + period / 2 + bias);
        (void)c.observe(false, static_cast<uint64_t>(k + 1), previous + period + effective);
        if (hold) c.limited = realOffsetLimited(c.offset, effective);
        c.forget();
    }
    return c.offset;
}
static_assert(realOffsetWindup(false) == RealOffsetController::limit);
static_assert(realOffsetWindup(true) <= std::chrono::microseconds{270});
static_assert(realOffsetWindup(true) >= std::chrono::microseconds{200});
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
    generatedPreparedOrderAndBudget(); generatedPreparationFallbackAndGates();
    generatedChainRealOffsetMovesOnlyTheReal();
    generatedPreparedRetirement(); generatedPreparedStop(); generatedPreparationErrors();
    queueRequest();
    privateSubmitWithoutTailIsUnchanged(); privateSubmitAppendsTail();
    privateSubmitFailureLeavesTail(); privateSubmitForeignChainBypassesTail();
    privateSubmitRecordsHistory(); privateSubmitHistoryIgnoresFailure();
    privateSubmitWithoutHistoryRecordsNothing(); skipReasonNamesFollowEnumOrder();
    lifecycle(); boundedBatch(); completePresentationOrder(); readinessDoesNotBlockProducer();
    waitForRetainedId(); observeCopiesWithoutChangingDelivery(); observerLifetimeAndForwarding();
    releasedBatchBoundsAndExpedites(); overlappingBatchRetainsOlder(); nonOverlappableDoesNotRetain();
    overlapAcquireDoesNotDrainFutureOutputs();
    overlapRingOfThree(); overlapAcceptsPresentId(); overlapAcceptsOwnedPresentId(); crossBatchChaining();
    twoIdentifiedBatchesJoinTheirOwnRecords(); threeIdentifiedBatchesJoinInAnyOrder();
    timingRequestIsCopiedAndRebuilt(); sixKnownNodesAreAccepted();
    overlappedChainShiftsLaterOutputs(); expeditedOutputAnchorsNextBatch();
    generatedCopyPresentsAcquiredImage(); generatedCopySkipsWithoutImage(); generatedCopySkipsOutOfRangeImage();
    generatedCopyNeedsOpenBatch(); generatedCopyChainWaitsPreceding(); generatedCopyChainSkipWaitsPreceding();
    generatedCopyAddsPrivateTail(); generatedSkipAddsPrivateTail();
    generatedCopyUnsafeSpacingModes(); generatedCopyTailSurvivesAJoin();
    lateGeneratedCopyPresentsBeforeLimit(); lateGeneratedCopyPastQuarterSkips();
    generatedCopyTailKeepsSpacingWithoutLimit();
    expeditedOverlapKeepsGeneratedAndTail(); expeditePullsSleepingTail();
    lateGeneratedUsesOneAttempt(); readinessExtensionIsBounded();
    nonOverlapKeepsSkipBehaviour(); compressedPairsAreCounted();
    unboundedDrainWaitsForLatchedPresent();
    internalRealRetriesThenPresents(); internalRealNeverSkippedUnderExpedite();
    internalRealErrorAbandonsOnce(); internalRealAfterGeneratedKeepsCopyTail();
    internalRealTeardownPresentsOrAbandons();
    internalRealPreparedBeforeDeadline(); internalRealNotPreparedPollsAtDeadline();
    internalRealPreparedPresentsWhenStopping(); internalRealPreparationErrorAbandonsOnce();
    internalRealPreparationSubmitErrorSkipsWithoutAbandon(); internalRealLockSplitIsProfileOnly();
    lowerRecordsFollowHistory(); inFlightCallFollowsHistory(); synchronousPresentIsNoted();
    presentsAreNotedWithHistory();
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/deferred_present.hpp"
#include "swapchain/private_present_queue.hpp"
#include "swapchain/tick_lock.hpp"
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
    return [id](uint64_t, uint64_t) { return presentedContains(id) ? VK_SUCCESS : VK_ERROR_UNKNOWN; };
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
    // Native-only, partial/error and unsupported-chain paths remain
    // synchronous, without migrating to an application queue.
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
            const auto lowerWait = [&](uint64_t, uint64_t) { ++lowerWaits; return VK_SUCCESS; };
            expect(DeferredPresent::waitForPresent(device, swapchain, id, 0, lowerWait) == VK_TIMEOUT && !lowerWaits,
                "poll forwarded a retained ID that the driver has not received");
            expect(DeferredPresent::waitForPresent(device, swapchain, id, 1'000'000, lowerWait) == VK_TIMEOUT && !lowerWaits,
                "finite wait exceeded the delivery budget or fabricated completion");
            VkResult result = VK_ERROR_UNKNOWN;
            std::thread waiter([&] {
                result = DeferredPresent::waitForPresent(device, swapchain, id, 200'000'000,
                    [&](uint64_t, uint64_t remaining) {
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
                    [&](uint64_t, uint64_t remaining) {
                        expect(remaining == UINT64_MAX, "infinite driver wait became finite");
                        return VK_SUBOPTIMAL_KHR;
                    }) == VK_SUBOPTIMAL_KHR,
                "completed delivery did not preserve the driver's result");
            expect(worker.takeCompletionPacingWait().waited == 0ns,
                "a completed delivery attributed worker delay already spent before a successful wait");
            ++id;
            expect(worker.beginBatch(1) &&
                    worker.enqueue(info, DeferredPresent::Clock::now() + 10ms),
                "delivery could not retain its real ID");
            worker.closeBatch();
            expect(DeferredPresent::waitForPresent(device, swapchain, id, UINT64_MAX, lowerWait) == VK_SUCCESS,
                "completion-paced source could not join its pending real ID");
            expect(worker.finish(UINT64_MAX).has_value(), "delivery did not complete");
            const auto completedBeforePoll = DeferredPresent::Clock::now();
            expect(DeferredPresent::waitForPresent(device, swapchain, id, 0,
                    [](uint64_t, uint64_t) { return VK_SUCCESS; }) == VK_SUCCESS,
                "completed real-ID poll changed driver completion");
            const auto capWait = worker.takeCompletionPacingWait();
            expect(capWait.waited > 0ns && worker.takeCompletionPacingWait().waited == 0ns,
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
                [&](uint64_t, uint64_t timeout) { expect(timeout == 0, "unmanaged poll timeout changed"); return VK_SUCCESS; }) == VK_SUCCESS,
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
void overlappingBatchAdmitsAfterWorkerRetires() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    VkSwapchainKHR swapchain = handle<VkSwapchainKHR>(11);
    VkSemaphore wait = handle<VkSemaphore>(12);
    uint32_t image = 0;
    VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    DeferredPresent worker(queue, present);
    const auto retireBatch = [&] {
        expect(worker.beginBatch(1, {.overlap = true}), "retiring overlap batch rejected");
        expect(worker.enqueue(info, DeferredPresent::Clock::now()), "retiring overlap output rejected");
        worker.closeBatch();
        const auto deadline = DeferredPresent::Clock::now() + 1s;
        while (worker.pending() && DeferredPresent::Clock::now() < deadline)
            std::this_thread::sleep_for(100us);
        expect(!worker.pending() && worker.hasCompletion(), "worker did not queue its retired batch");
    };
    // Model a completion that arrives after the game thread's last poll.
    expect(!worker.hasCompletion(), "unexpected completion before the first batch");
    retireBatch();
    expect(!worker.beginBatch(1), "ordinary batch bypassed an uncollected completion");
    for (image = 1; image < 4; ++image) retireBatch();
    expect(!worker.beginBatch(1, {.overlap = true}), "batch admitted without a completion slot");
    for (image = 0; image < 4; ++image) {
        const auto completion = worker.finish(0);
        expect(completion && completion->count == 1 && completion->calls[0].imageIndex == image,
            "overlap admission lost or reordered a completion");
    }

    lowerResult = VK_ERROR_OUT_OF_DATE_KHR;
    retireBatch();
    expect(!worker.beginBatch(1, {.overlap = true}), "overlap batch bypassed an uncollected error");
    const auto failed = worker.finish(0);
    expect(failed && failed->result == VK_ERROR_OUT_OF_DATE_KHR, "present error was lost");
    lowerResult = VK_SUCCESS;
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
    const auto driver = [&driverCalls](uint64_t, uint64_t) { ++driverCalls; return VK_SUCCESS; };
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
    expect(DeferredPresent::waitForPresent(device, swapchain, ids[2], 0, [](uint64_t, uint64_t) { return VK_SUCCESS; }) == VK_TIMEOUT,
        "a poll for the newest undelivered record reached the driver");
    expect(DeferredPresent::waitForPresent(device, swapchain, ids[0], 0, [](uint64_t, uint64_t) { return VK_SUCCESS; }) == VK_SUCCESS,
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
    expect(DeferredPresent::waitForPresent(device, swapchain, ids[0], 0, [](uint64_t, uint64_t) { return VK_SUCCESS; }) == VK_SUCCESS,
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
void generatedCopyTailKeepsSpacingWithoutLimit(const bool direct) {
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
    const auto first = direct ? now + interval : now;
    uint32_t generatedImage = 2;
    const auto generatedWait = handle<VkSemaphore>(72);
    const VkPresentInfoKHR generatedInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &generatedWait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &generatedImage};
    auto copy = generatedCopy();
    copy.giveUp = 1s;
    expect(worker.beginBatch(2, {.overlap = true, .outputInterval = interval}),
        "unlimited copy batch rejected");
    expect(worker.canPreacquireGenerated(), "empty batch held a direct reservation");
    expect(direct ? worker.enqueue(generatedInfo, first, true, {}, true)
                  : worker.enqueueGeneratedCopy(swapchain, nullptr, first, copy),
        "unlimited generated copy rejected");
    if (direct) expect(!worker.canPreacquireGenerated(), "direct reservation was not counted");
    expect(worker.enqueue(realInfo, first + 1ms), "unlimited real output rejected");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 2 && completion->result == VK_SUCCESS &&
        !completion->calls[0].skipped && !completion->calls[1].skipped,
        "unlimited generated copy was skipped");
    expect(completion->calls[1].started >= completion->calls[0].started + interval,
        "copy tail lost its full output spacing without a release limit");
    if (direct) {
        expect(worker.canPreacquireGenerated(), "presented direct reservation was retained");
        expect(copySubmits.empty() && copyAcquireCalls == 0 && completion->calls[0].generatedPrepared,
            "worker tried to acquire or copy a producer-prepared generated image");
    }
    expect(copyPresents.size() == 2 && copyPresents[0] == std::pair{2u, generatedWait} &&
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
    // Sets the automatic output corrections before a batch is admitted; the worker reads them only after
    // admission. The expected count is set first, so the worker's own setExpected keeps the offsets.
    static void setOutputOffsets(DeferredPresent& worker, uint32_t expected,
            std::array<std::chrono::nanoseconds, 4> offsets) {
        const std::lock_guard lock(worker.mutex);
        worker.outputOffsetController.setExpected(expected);
        worker.outputOffsetController.offset = offsets;
    }
    // OS1b: accepted correction samples of the worker's controller. Read only after the worker's batch was finished:
    // finish takes the worker mutex that orders the controller's writes before this read.
    static uint64_t acceptedOffsetSamples(const DeferredPresent& worker) {
        return worker.outputOffsetController.accepted;
    }
    // OS1c: a copy of the worker's controller, read only after the batch was finished (see acceptedOffsetSamples).
    static OutputOffsetController offsetController(const DeferredPresent& worker) {
        return worker.outputOffsetController;
    }
    // RP1: presets the worker's real present lead before a batch is admitted (the worker reads it on the batch's first output).
    static void setRealLead(DeferredPresent& worker, const std::chrono::nanoseconds lead) {
        const std::lock_guard lock(worker.mutex);
        worker.realPresentLead.lead = lead;
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
    // FI2: the call-side copy (InternalRealCopy::callPrepare). A VK_NOT_READY callResult leaves the image for the
    // worker; any other result consumes it, as the real submit does.
    int callCalls{};
    VkResult callResult{VK_SUCCESS};
    uint32_t callLower{4};
    VkSemaphore callWait{};
    VkQueue callQueue{};
    VkSemaphore callContinuation{};
    // FP2: parked images. parkedReported is what the owner's count returns, parkedAvailable is how many the parked take
    // can consume, and parkedLower and parkedWait are the image it returns.
    int parkedCalls{};
    size_t parkedReported{};
    size_t parkedAvailable{};
    uint32_t parkedLower{4};
    VkSemaphore parkedWait{};
    // FP2: the park hook. Each call records the outputs call-held when it ran.
    int parks{};
    size_t parkHeld{};
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
// FI2: the call-side copy. It runs at admission on the game queue (handle 9), with the given call budget.
VkResult fakeCallPrepare(void* owner, VkQueue callQueue, [[maybe_unused]] uint32_t callFamily,
        [[maybe_unused]] uint32_t applicationIndex, VkSemaphore continuation, uint32_t* lowerIndex,
        VkSemaphore* presentWait, bool* consumed) {
    auto& state = *static_cast<InternalFake*>(owner);
    ++state.callCalls;
    state.callQueue = callQueue;
    state.callContinuation = continuation;
    *consumed = state.callResult != VK_NOT_READY;
    if (!*consumed) return VK_NOT_READY;
    *lowerIndex = state.callLower;
    *presentWait = state.callWait;
    return state.callResult;
}
// FP2: the parked take. It consumes one parked image when one is available and reports NOT_READY otherwise. It never takes
// a queue lock, so the test can hold queue->mutex across it.
VkResult fakeCallPrepareParked(void* owner, VkQueue callQueue, [[maybe_unused]] uint32_t callFamily,
        [[maybe_unused]] uint32_t applicationIndex, VkSemaphore continuation, uint32_t* lowerIndex,
        VkSemaphore* presentWait, bool* consumed) {
    auto& state = *static_cast<InternalFake*>(owner);
    ++state.parkedCalls;
    state.callQueue = callQueue;
    state.callContinuation = continuation;
    *consumed = state.parkedAvailable > 0;
    if (!*consumed) return VK_NOT_READY;
    --state.parkedAvailable;
    --state.parkedReported;
    *lowerIndex = state.parkedLower;
    *presentWait = state.parkedWait;
    return VK_SUCCESS;
}
size_t fakeParkedCount(void* owner) {
    return static_cast<InternalFake*>(owner)->parkedReported;
}
ParkOutcome recordPark(void* owner, size_t callHeld) {
    auto& state = *static_cast<InternalFake*>(owner);
    ++state.parks;
    state.parkHeld = callHeld;
    return ParkOutcome::Parked;
}
DeferredPresent::InternalRealCopy internalParkedCallCopy(const std::shared_ptr<InternalFake>& fake,
        uint32_t applicationIndex, uint32_t callBudget) {
    auto copy = DeferredPresent::InternalRealCopy{
        .owner = fake, .tryAcquire = fakeTryAcquire, .submit = fakeSubmit,
        .abandon = fakeAbandon, .applicationIndex = applicationIndex,
        .callPrepare = fakeCallPrepare, .callQueue = handle<VkQueue>(9), .callFamily = 0,
        .callBudget = callBudget,
    };
    copy.callPrepareParked = fakeCallPrepareParked;
    copy.parkedCount = fakeParkedCount;
    return copy;
}
DeferredPresent::InternalRealCopy internalCallCopy(const std::shared_ptr<InternalFake>& fake, uint32_t applicationIndex,
        uint32_t callBudget) {
    return DeferredPresent::InternalRealCopy{
        .owner = fake, .tryAcquire = fakeTryAcquire, .submit = fakeSubmit,
        .abandon = fakeAbandon, .applicationIndex = applicationIndex,
        .callPrepare = fakeCallPrepare, .callQueue = handle<VkQueue>(9), .callFamily = 0,
        .callBudget = callBudget,
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
    VkFence acquireFence{};
    unsigned acquireFencePolls{}, acquireFenceResets{};
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
        VkSemaphore, VkFence fence, uint32_t* image) {
    expect(timeout == 0, "early generated acquire blocked");
    preparedPool->acquireFence = fence;
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

void generatedCopyCanPrecedeHostReadiness() {
    struct Case { bool enabled, unavailable, timeout; };
    for (const auto test : {Case{}, Case{true,false,false}, Case{true,true,false}, Case{true,false,true}}) {
        auto pool = std::make_shared<PreparedPool>();
        preparedPool = pool.get();
        pool->misses = test.unavailable ? 1 : 0;
        copySubmits.clear();
        auto queue = std::make_shared<DeferredPresentQueue>();
        queue->queue = handle<VkQueue>(7);
        DeferredPresent worker(queue, preparedPresent);
        readinessResult = test.timeout ? VK_TIMEOUT : VK_SUCCESS;
        const auto swapchain = handle<VkSwapchainKHR>(11);
        const auto wait = handle<VkSemaphore>(12);
        const uint32_t image = 4;
        const VkPresentInfoKHR real{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
        const auto first = DeferredPresent::Clock::now() + 8ms;
        const DeferredPresent::InternalRealCopy realCopy{.owner = pool, .tryAcquire = poolRealAcquire,
            .submit = poolRealSubmit, .abandon = poolRealAbandon};
        const auto timedWait = [](VkDevice, const VkSemaphoreWaitInfo*, uint64_t) -> VkResult {
            return readinessResult;
        };
        DeferredPresent::AcquireProbe probe{
            .fence = handle<VkFence>(110),
            .status = [](VkDevice, VkFence) -> VkResult {
                return ++preparedPool->acquireFencePolls == 1 ? VK_NOT_READY : VK_SUCCESS;
            },
            .reset = [](VkDevice, uint32_t, const VkFence*) -> VkResult {
                ++preparedPool->acquireFenceResets;
                return VK_SUCCESS;
            },
        };
        auto copy = preparedCopy();
        copy.acquireProbe = &probe;
        expect(worker.beginBatch(2, {.device = handle<VkDevice>(21), .semaphore = handle<VkSemaphore>(31),
            .value = 17, .wait = timedWait, .overlap = true, .batchHold = false,
            .copyBeforeReadiness = test.enabled, .outputInterval = 5ms, .profile = true}),
            "pre-readiness experiment batch rejected");
        expect(worker.enqueueGeneratedCopy(swapchain, nullptr, first, copy) &&
            worker.enqueueInternalReal(real, first + 5ms, realCopy), "pre-readiness experiment outputs rejected");
        worker.closeBatch();
        const auto done = worker.finish(UINT64_MAX, false);
        expect(done && done->count == 2 && done->readinessMiss == test.timeout &&
            done->calls[0].generatedPrepared && !done->calls[0].skipped &&
            !done->calls[1].skipped && pool->held == 0 && pool->order == std::vector<uint32_t>{0,1} &&
            copySubmits.size() == 1 && copySubmits[0].count == 1,
            "pre-readiness experiment lost, reordered or duplicated a copy or present");
        const auto& generated = done->calls[0];
        expect(probe.used && pool->acquireFence == probe.fence &&
            generated.copyAcquireBounds.pendingNs != 0 &&
            generated.copyAcquireBounds.readyNs >= generated.copyAcquireBounds.pendingNs &&
            pool->acquireFencePolls == 2 && pool->acquireFenceResets == 0,
            "probe must observe the successful acquire and stop polling once the fence signals");
        expect(probe.prepare(handle<VkDevice>(21)) == probe.fence && !probe.used &&
            pool->acquireFenceResets == 1, "a signaled acquire fence must reset before reuse");
        expect(generated.copyAcquireReturned != DeferredPresent::Clock::time_point{} &&
            generated.copyAcquireReturned <= generated.copySubmitStarted &&
            generated.copySubmitStarted <= generated.copySubmitReturned &&
            generated.copySubmitReturned <= generated.started,
            "generated copy acquire and submit timestamps must precede the present");
        expect(
            (test.enabled && !test.unavailable
                ? generated.copySubmitReturned <= generated.readinessStarted
                : generated.copySubmitStarted >= generated.readinessFinished),
            "final copy was submitted on the wrong side of the host readiness wait");
    }
    readinessResult = VK_SUCCESS;
}
// W2: in a three-output batch each generated output takes its own correction and the real is never
// moved. The first generated output moves 1 ms earlier from its own slot. The second moves 1 ms later
// from its own slot, so the first's move does not push it. The real keeps its slot, so the second's
// move does not push it.
void generatedChainOutputOffsetsMoveEachOutput() {
    auto pool = std::make_shared<PreparedPool>();
    preparedPool = pool.get();
    copySubmits.clear();
    setRealOffsetAutoConfigured(true);
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, preparedPresent);
    DeferredPresentTestAccess::setOutputOffsets(worker, 2, {-1000us, 1000us, 0ns, 0ns});
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
    setRealOffsetAutoConfigured(false);
    expect(done && done->count == 3 && done->result == VK_SUCCESS &&
        !done->calls[0].skipped && !done->calls[1].skipped && !done->calls[2].skipped,
        "offset batch failed");
    expect(done->calls[0].scheduled >= first - 1ms && done->calls[0].scheduled < first,
        "the first generated output did not take its own offset");
    // Each later slot follows the previous output's start in unmoved time, so a late wake
    // under load moves the slot, not the check.
    const auto secondSlot = std::max(first + 10ms,
        done->calls[0].started - done->calls[0].offset + 10ms);
    expect(done->calls[1].scheduled >= secondSlot + 1ms && done->calls[1].scheduled < secondSlot + 2ms,
        "the second generated output did not take its own offset alone");
    const auto realSlot = std::max(first + 20ms,
        done->calls[1].started - done->calls[1].offset + 10ms);
    expect(done->calls[2].offset == DeferredPresent::Clock::duration::zero() &&
        done->calls[2].scheduled >= realSlot && done->calls[2].scheduled < realSlot + 1ms,
        "the real output was moved");
    expect(done->calls[0].offset == -1ms && done->calls[1].offset == 1ms && done->calls[2].offset == 0ns,
        "the recorded corrections are not the effective moves");
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
        expect(worker.beginBatch(1, {.overlap = mode != 2, .profile = mode == 0}), "fallback batch rejected");
        expect(worker.enqueueGeneratedCopy(handle<VkSwapchainKHR>(11), nullptr, due, copy),
            "fallback generated output rejected");
        worker.closeBatch();
        const auto done = worker.finish(UINT64_MAX, false);
        expect(done && done->count == 1 && !done->calls[0].skipped &&
            !done->calls[0].generatedPrepared && pool.held == 0 &&
            pool.attempts == (mode == 0 ? 2u : 1u) && copySubmits.size() == 1 &&
            pool.acquiredAt[0] >= due, "fallback or preparation gate changed deadline delivery");
        const auto& call = done->calls[0];
        if (mode == 0)
            expect(call.copyAcquireReturned >= due && call.copyAcquireReturned <= call.copySubmitStarted &&
                call.copySubmitStarted <= call.copySubmitReturned && call.copySubmitReturned <= call.started,
                "fallback timestamps must identify the acquire that returned an image and the submitted copy");
        else
            expect(call.copyAcquireReturned == DeferredPresent::Clock::time_point{} &&
                call.copySubmitStarted == DeferredPresent::Clock::time_point{} &&
                call.copySubmitReturned == DeferredPresent::Clock::time_point{},
                "disabled profiling recorded generated copy timestamps");
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
// W2: a generated output's own correction keeps a quarter interval after the previous output's start,
// and changes nothing when it is zero.
namespace output_offset_checks {
using Ns = std::chrono::nanoseconds;
using Tp = std::chrono::time_point<std::chrono::steady_clock, Ns>;
constexpr Tp at(const int64_t us) { return Tp{std::chrono::microseconds{us}}; }
constexpr Ns us(const int64_t value) { return std::chrono::microseconds{value}; }
static_assert(deadlineWithOffset(at(6500), at(0), us(6500), us(0)) == at(6500));
static_assert(deadlineWithOffset(at(6500), at(0), us(6500), us(-550)) == at(5950));
static_assert(deadlineWithOffset(at(6500), at(0), us(6500), us(400)) == at(6900));
// Short interval: the quarter-interval bound holds.
static_assert(deadlineWithOffset(at(2000), at(0), us(2000), us(-1000)) == at(1000));
static_assert(deadlineWithOffset(at(1000), at(0), us(2000), us(-1000)) == at(500));
// A deadline already inside the bound is not pushed later by a negative offset.
static_assert(deadlineWithOffset(at(300), at(0), us(2000), us(-1000)) == at(300));
// OS2: a first output's correction is measured from its readiness start, never before readyAt.
static_assert(readyDeadlineWithOffset(at(26000), at(28000), at(20000), true, Tp::max(), us(8000), us(0))
    == at(28000));
static_assert(readyDeadlineWithOffset(at(26000), at(28000), at(20000), true, Tp::max(), us(8000), us(1000))
    == at(29000));
static_assert(readyDeadlineWithOffset(at(26000), at(28000), at(20000), true, Tp::max(), us(8000), us(-1000))
    == at(27000));
static_assert(readyDeadlineWithOffset(at(26000), at(28000), at(20000), true, Tp::max(), us(8000), us(-3000))
    == at(26000));
static_assert(readyDeadlineWithOffset(at(26000), at(28000), at(0), false, Tp::max(), us(8000), us(-1000))
    == at(27000));
// A positive correction stops at the release limit, or at the unmoved start when that is already later.
static_assert(readyDeadlineWithOffset(at(26000), at(28000), at(20000), true, at(28500), us(8000), us(1000))
    == at(28500));
static_assert(readyDeadlineWithOffset(at(26000), at(28000), at(20000), true, at(27000), us(8000), us(1000))
    == at(28000));
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
// W2: markers of real k-1, generated k, real k at a 13 ms source period. The reals stay on their
// slots. The generated output lands `bias` after the midpoint of its reals, plus its own correction
// (its response). Noise moves the reals in pairs and the generated output alternately, so the
// midpoint itself moves. The correction converges to -bias: the output moves earlier by the bias.
constexpr bool outputOffsetSimulation(std::chrono::nanoseconds bias, bool noisy = false) {
    using namespace std::chrono_literals;
    OutputOffsetController controller;
    constexpr std::chrono::nanoseconds period = 13ms; // nanoseconds: half of it is exact
    const auto realAt = [&](int k) {
        return period * k + (noisy ? ((k / 2) % 2 ? 500us : -500us) : 0us);
    };
    (void)controller.observe(false, 0, realAt(0));
    for (int k = 1; k <= 400; ++k) {
        const auto generated = period * k - period / 2 + bias + controller.offset[0] +
            (noisy ? (k % 2 ? 300us : -300us) : 0us);
        (void)controller.observe(true, static_cast<uint64_t>(k), generated);
        if (!controller.observe(false, static_cast<uint64_t>(k), realAt(k))) return false;
        if (controller.offset[0] > OutputOffsetController::limit ||
                controller.offset[0] < -OutputOffsetController::limit)
            return false;
    }
    const auto miss = controller.offset[0] + bias;
    return controller.accepted == 400 && miss <= 50us && miss >= -50us;
}
static_assert(outputOffsetSimulation(500us));
static_assert(outputOffsetSimulation(-500us));
static_assert(outputOffsetSimulation(0us));
static_assert(outputOffsetSimulation(400us, true));
static_assert(outputOffsetSimulation(-600us, true));
// OS1: the swapchain's source counter starts at 0 and advances once per present, with or without the pacing
// profile. Its values are nonzero and consecutive, so each batch's real carries the previous present's source
// and its generated output and closing real carry this one. The controller integrates with no profile at all.
constexpr bool unprofiledSourcesIntegrate(std::chrono::nanoseconds bias) {
    using namespace std::chrono_literals;
    OutputOffsetController controller;
    constexpr std::chrono::nanoseconds period = 13ms;
    uint64_t sourceSerial{};
    uint64_t previous{};
    for (int k = 1; k <= 400; ++k) {
        // The present advances the counter before its batch is marked, as Swapchain::present does.
        const uint64_t source = ++sourceSerial;
        if (source == 0 || source != previous + 1) return false;
        previous = source;
        const auto generated = period * k - period / 2 + bias + controller.offset[0];
        (void)controller.observe(true, source, generated);
        // The first closing real has no opener yet, so it only starts the chain.
        const bool used = controller.observe(false, source, period * k);
        if (k > 1 && !used) return false;
        if (controller.offset[0] > OutputOffsetController::limit ||
                controller.offset[0] < -OutputOffsetController::limit)
            return false;
    }
    const auto miss = controller.offset[0] + bias;
    return controller.accepted == 399 && previous == 400 && miss <= 50us && miss >= -50us;
}
static_assert(unprofiledSourcesIntegrate(500us));
static_assert(unprofiledSourcesIntegrate(-500us));
// W2: at 4x each generated position has its own bias. Each converges to its own bias, within 10 us,
// and the mean of the biases alone would not give that (it would leave every position off by its
// own deviation from the mean).
constexpr bool fourXOutputOffsetSimulation(std::array<std::chrono::nanoseconds, 3> bias) {
    using namespace std::chrono_literals;
    OutputOffsetController c;
    c.setExpected(3);
    constexpr std::chrono::nanoseconds period = 12ms; // three generated positions at 3, 6 and 9 ms
    (void)c.observe(false, 0, std::chrono::nanoseconds{});
    for (int k = 1; k <= 400; ++k) {
        const auto previous = period * (k - 1);
        for (uint32_t j = 0; j < 3; ++j)
            (void)c.observe(true, static_cast<uint64_t>(k),
                previous + period * (j + 1) / 4 + bias[j] + c.offset[j]);
        if (!c.observe(false, static_cast<uint64_t>(k), period * k)) return false;
    }
    for (uint32_t j = 0; j < 3; ++j) {
        const auto miss = c.offset[j] + bias[j];
        if (miss > 10us || miss < -10us) return false;
    }
    return c.accepted == 400 && c.held == 0;
}
static_assert(fourXOutputOffsetSimulation({-120us, -160us, 170us}));
static_assert(fourXOutputOffsetSimulation({500us, -500us, 0us}));
static_assert([] {
    using namespace std::chrono_literals;
    using C = OutputOffsetController;
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
    if (c.offset[0] != 0ns || c.accepted != 0) return false;
    (void)c.observe(true, 12, 111ms + 6500us + 3ms);
    if (!c.observe(false, 12, 124ms)) return false;                  // error +3 ms, clamped to 1 ms
    if (c.offset[0] != -(C::Duration{1ms} / C::gain) || c.lastError != 3ms) return false;  // late: earlier
    c.forget();
    (void)c.observe(true, 13, 130ms);
    if (c.observe(false, 13, 137ms)) return false;                   // history dropped
    if (c.offset[0] != -(C::Duration{1ms} / C::gain)) return false;                       // offset kept
    for (int k = 14; k < 400; ++k) {                                 // +1 ms each: saturates
        const auto previous = 137ms + 13ms * (k - 14);
        (void)c.observe(true, static_cast<uint64_t>(k), previous + 6500us + 1ms);
        if (!c.observe(false, static_cast<uint64_t>(k), previous + 13ms)) return false;
    }
    return c.offset[0] == -C::limit;
}());
// W2: a generated output's move counts for the part that took effect. A later move counts up to its
// final scheduled deadline, and an earlier move only as far as that deadline went: a raise that undoes
// part of it leaves the rest, and a raise past the unmoved slot leaves none.
namespace output_offset_checks {
static_assert(outputOffsetEffective(us(1000), at(4300), at(5300), at(4600)) == us(300));
static_assert(outputOffsetEffective(us(1000), at(4300), at(5300), at(4000)) == us(0));     // before unmoved
static_assert(outputOffsetEffective(us(1000), at(4300), at(5300), at(5400)) == us(1000));  // late wake
static_assert(outputOffsetEffective(us(-500), at(4300), at(3800), at(3000)) == us(-500));  // earlier: kept
static_assert(outputOffsetEffective(us(0), at(4300), at(4300), at(4000)) == us(0));
static_assert(outputOffsetEffective(us(-1000), at(4300), at(3300), at(3300)) == us(-1000)); // at the moved slot
static_assert(outputOffsetEffective(us(-1000), at(4300), at(3800), at(3800)) == us(-500));  // raised half way
static_assert(outputOffsetEffective(us(-1000), at(4300), at(4400), at(4400)) == us(0));     // raised past unmoved
static_assert(outputOffsetEffective(us(-1000), at(4300), at(3300), at(3500)) == us(-1000)); // late wake ignored
static_assert(outputOffsetLimited(us(1000), us(300)) == 1);
static_assert(outputOffsetLimited(us(1000), us(960)) == 0);
static_assert(outputOffsetLimited(us(-600), us(-200)) == -1);
static_assert(outputOffsetLimited(us(-600), us(-600)) == 0);
static_assert(outputOffsetLimited(us(0), us(0)) == 0);
}
// W2: while a position is limited one way, samples pushing that way are held; the other way integrates.
static_assert([] {
    using namespace std::chrono_literals;
    using C = OutputOffsetController;
    C c;
    const auto sample = [&](int k, std::chrono::nanoseconds error) {
        const auto previous = 13ms * k;
        (void)c.observe(false, static_cast<uint64_t>(k), previous);
        (void)c.observe(true, static_cast<uint64_t>(k + 1), previous + 6500us + error);
        return c.observe(false, static_cast<uint64_t>(k + 1), previous + 13ms);
    };
    if (!sample(1, 640us) || c.offset[0] != C::Duration{-10us}) return false; // late: moves earlier
    c.limited[0] = -1;                                               // could not move earlier
    for (int k = 3; k < 300; k += 2)
        if (sample(k, 640us)) return false;                          // pushes earlier: held, never accepted
    if (c.offset[0] != C::Duration{-10us} || c.held != 149 || c.accepted != 1) return false;
    if (!sample(301, -640us) || c.offset[0] != 0ns) return false;    // pushes later: integrates
    c.limited[0] = 1;                                                // could not move later
    if (sample(303, -640us) || !sample(305, 640us)) return false;    // pushes later: held; earlier: integrates
    c.forget();
    if (c.limited[0] != 1) return false;                             // forget keeps it
    c.limited[0] = 0;
    return sample(307, 640us) && c.offset[0] == C::Duration{-20us} && c.held == 150;
}());
// W1: N - 1 generated outputs per source, evenly placed between two reals 12 ms apart. Each is at its
// ideal point, moved by `late`. `dropLast` omits the last generated output of each source.
constexpr OutputOffsetController evenlySpacedOutputs(uint32_t expected, std::chrono::nanoseconds late,
        bool dropLast = false) {
    using namespace std::chrono_literals;
    OutputOffsetController c;
    c.setExpected(expected);
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
constexpr bool allPositionsZero(const std::array<std::chrono::nanoseconds, 4>& offsets, uint32_t count) {
    for (uint32_t k = 0; k < count; ++k)
        if (offsets[k] != std::chrono::nanoseconds{}) return false;
    return true;
}
static_assert([] {
    using namespace std::chrono_literals;
    using C = OutputOffsetController;
    for (const uint32_t expected : {2u, 4u}) {
        const auto even = evenlySpacedOutputs(expected, 0ns);
        if (even.accepted != 20 || !allPositionsZero(even.offset, expected) || even.lastError != 0ns) return false;
        // Every position is late by 2 ms: each takes the clamped 1 ms error once per source.
        const auto late = evenlySpacedOutputs(expected, 2ms);
        if (late.accepted != 20 || late.lastError != 2ms) return false;
        for (uint32_t k = 0; k < expected; ++k)
            if (late.offset[k] != -20 * (C::Duration{1ms} / C::gain)) return false;
        const auto fewer = evenlySpacedOutputs(expected, 2ms, true);
        if (fewer.accepted != 0 || !allPositionsZero(fewer.offset, expected)) return false; // one short: no sample
    }
    // expectedGenerated 1 is today's rule: a second generated marker of the same source gives no sample.
    C c;
    (void)c.observe(false, 0, 0ms);
    (void)c.observe(true, 1, 4ms);
    (void)c.observe(true, 1, 8ms);
    if (c.observe(false, 1, 12ms) || c.accepted != 0) return false;
    return true;
}());
// W2: a hold stops only the position it limits. Three generated outputs in a 12 ms step, each 640 us
// late: position 1 is held, positions 0 and 2 integrate.
static_assert([] {
    using namespace std::chrono_literals;
    using C = OutputOffsetController;
    C c;
    c.setExpected(3);
    c.limited[1] = -1;                                               // position 1 could not move earlier
    (void)c.observe(false, 0, 0ms);
    for (uint32_t j = 0; j < 3; ++j)
        (void)c.observe(true, 1, 3ms * (j + 1) + 640us);
    if (!c.observe(false, 1, 12ms)) return false;
    return c.offset[0] == -10us && c.offset[1] == 0ns && c.offset[2] == -10us &&
        c.held == 1 && c.accepted == 1 && c.lastError == 640us;
}());
// W2: setExpected with a new count starts every position again; the same count keeps them.
static_assert([] {
    using namespace std::chrono_literals;
    using C = OutputOffsetController;
    C c;
    c.setExpected(2);
    c.offset[0] = 10us;
    c.offset[1] = -20us;
    c.limited[1] = 1;
    c.setExpected(2);
    if (c.offset[0] != 10us || c.offset[1] != -20us || c.limited[1] != 1) return false;
    c.setExpected(3);
    return c.expectedGenerated == 3 && allPositionsZero(c.offset, 4) && c.limited[1] == 0;
}());
// W2: a fifth generated marker gives no sample, and four do (every position at its ideal point).
static_assert([] {
    using namespace std::chrono_literals;
    using C = OutputOffsetController;
    C c;
    c.setExpected(4);
    (void)c.observe(false, 0, 0ms);
    for (uint32_t j = 0; j < 4; ++j)
        (void)c.observe(true, 1, 12000us * (j + 1) / 5);   // 2.4, 4.8, 7.2, 9.6 ms: exact in microseconds
    if (!c.observe(false, 1, 12ms) || c.accepted != 1 || !allPositionsZero(c.offset, 4)) return false;
    C five;
    five.setExpected(4);
    (void)five.observe(false, 0, 0ms);
    for (uint32_t j = 0; j < 5; ++j)
        (void)five.observe(true, 1, 2ms * (j + 1));
    return !five.observe(false, 1, 12ms) && five.accepted == 0 && allPositionsZero(five.offset, 4);
}());
// W2: the cap-bound windup. A generated output cannot move earlier than `reach` before its unmoved slot
// (the previous output's start plus a quarter interval), so the error stays at `bias - reach` once the
// offset passes `-reach`. With the hold the offset stops past the reach; without it, it runs to the limit.
constexpr std::chrono::nanoseconds outputOffsetWindup(bool hold) {
    using namespace std::chrono_literals;
    OutputOffsetController c;
    constexpr std::chrono::nanoseconds period = 8600us, bias = 650us, reach = 200us;
    for (int k = 1; k <= 600; ++k) {
        const auto effective = std::max(c.offset[0], -reach);
        const auto previous = period * k;
        (void)c.observe(false, static_cast<uint64_t>(k), previous);
        (void)c.observe(true, static_cast<uint64_t>(k + 1), previous + period / 2 + bias + effective);
        (void)c.observe(false, static_cast<uint64_t>(k + 1), previous + period);
        if (hold) c.limited[0] = outputOffsetLimited(c.offset[0], effective);
        c.forget();
    }
    return c.offset[0];
}
static_assert(outputOffsetWindup(false) == -OutputOffsetController::limit);
static_assert(outputOffsetWindup(true) >= -std::chrono::microseconds{270});
static_assert(outputOffsetWindup(true) <= -std::chrono::microseconds{200});
// OS1: the controller limit is 2.5 ms. The scheduler separately bounds the
// applied offset against readiness, the previous output and the release limit.
static_assert(OutputOffsetController::limit == std::chrono::microseconds{2500});

// L2 (low_latency): readiness at a simulated instant. The wait returns once the value's instant has
// passed, within its timeout, and otherwise times out at the timeout.
std::array<DeferredPresent::Clock::time_point, 3> simulatedReady{};
VkResult VKAPI_CALL readyAtInstant(VkDevice, const VkSemaphoreWaitInfo* info, uint64_t timeout) {
    const auto ready = simulatedReady.at(info->pValues[0]);
    const auto limit = DeferredPresent::Clock::now() + std::chrono::nanoseconds(static_cast<int64_t>(timeout));
    if (ready > limit) {
        std::this_thread::sleep_until(limit);
        return VK_TIMEOUT;
    }
    std::this_thread::sleep_until(ready);
    return VK_SUCCESS;
}
struct ReleasedPair {
    std::optional<DeferredPresent::Completion> first, second;
    DeferredPresent::Clock::time_point start{};
};
// Two overlapped released batches of two real outputs each, 8 ms apart. Batch A is ready 10 ms after
// the start and batch B `readyB` after it. B is admitted while A is still outputting. With
// `generatedOffset`, B's first output is a generated output (internal images) with that automatic
// correction, and B's second output is the real one.
ReleasedPair releasedPair(const bool justInTime, const std::chrono::milliseconds readyB,
        const std::optional<std::chrono::nanoseconds> generatedOffset = std::nullopt,
        const bool batchHold = true) {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    VkSwapchainKHR swapchain = handle<VkSwapchainKHR>(11);
    VkSemaphore wait = handle<VkSemaphore>(12);
    uint32_t image = 0;
    VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    DeferredPresent worker(queue, timedPresent);
    ReleasedPair pair;
    pair.start = DeferredPresent::Clock::now();
    const auto interval = 8ms;
    simulatedReady[1] = pair.start + 10ms;
    simulatedReady[2] = pair.start + readyB;
    // OS2: the automatic correction is on for this pair only; the offset is set before B is admitted.
    if (generatedOffset) {
        setRealOffsetAutoConfigured(true);
        copySubmits.clear();
        copyAcquireResult = VK_SUCCESS;
        copyAcquireImage = 2;
        DeferredPresentTestAccess::setOutputOffsets(worker, 1, {*generatedOffset, 0ns, 0ns, 0ns});
    }
    expect(worker.beginBatch(2, {.device = handle<VkDevice>(21), .semaphore = handle<VkSemaphore>(31),
            .value = 1, .wait = readyAtInstant, .deadline = pair.start + 1s, .overlap = true,
            .justInTime = justInTime, .batchHold = batchHold, .outputInterval = interval}),
        "first released batch rejected");
    expect(worker.enqueue(info, pair.start), "first released batch's first output rejected");
    image = 1;
    expect(worker.enqueue(info, pair.start + interval), "first released batch's second output rejected");
    worker.closeBatch();
    expect(worker.beginBatch(2, {.device = handle<VkDevice>(21), .semaphore = handle<VkSemaphore>(31),
            .value = 2, .wait = readyAtInstant, .deadline = pair.start + 1s, .overlap = true,
            .justInTime = justInTime, .batchHold = batchHold, .outputInterval = interval}),
        "second released batch rejected while the first was outputting");
    if (generatedOffset) {
        auto copy = generatedCopy();
        copy.internalImages = true;
        expect(worker.enqueueGeneratedCopy(swapchain, nullptr, pair.start, copy),
            "second released batch's generated output rejected");
    } else {
        image = 2;
        expect(worker.enqueue(info, pair.start), "second released batch's first output rejected");
    }
    image = 3;
    expect(worker.enqueue(info, pair.start + interval), "second released batch's second output rejected");
    worker.closeBatch();
    pair.first = worker.finish(UINT64_MAX);
    pair.second = worker.finish(UINT64_MAX);
    setRealOffsetAutoConfigured(false);
    return pair;
}
bool releasedPairCompleted(const ReleasedPair& pair) {
    return pair.first && pair.second && pair.first->count == 2 && pair.second->count == 2 &&
        !pair.first->calls[0].skipped && !pair.first->calls[1].skipped &&
        !pair.second->calls[0].skipped && !pair.second->calls[1].skipped;
}
// The chain (worker run(): previousScheduled + interval - interval / 16) holds the next batch's first output
// to the previous batch's last output: its scheduled slot, or its actual start when that came first.
DeferredPresent::Clock::time_point chainedFloorAfter(const DeferredPresent::Completion& previous,
        const std::chrono::nanoseconds interval) {
    const auto& last = previous.calls[previous.count - 1];
    return std::min(last.scheduled, last.started) + interval - interval / 16;
}
// L3 (low_latency): a released batch starts 0 to 4 ms after its own readiness, phase-locked to the batch
// cadence (batch_phase.hpp). Batch A is ready 10 ms after the start and starts at 12 ms. Batch B becomes
// ready one batch period after A's readiness (2 × 8 ms), the steady cadence, so the lock starts it 2 ms
// after its readiness.
void justInTimeBatchStartsAtItsReadiness() {
    const auto interval = 8ms;
    const auto justInTime = releasedPair(true, 26ms);
    expect(releasedPairCompleted(justInTime), "just-in-time released pair did not complete");
    const auto readyB = justInTime.start + 26ms;
    const auto& firstOfB = justInTime.second->calls[0];
    expect(firstOfB.scheduled >= readyB && firstOfB.scheduled < readyB + 4ms,
        "just-in-time batch did not start within [0, 4 ms] of its own readiness");
    expect(justInTime.second->calls[1].started >= firstOfB.started + interval - 1ms,
        "just-in-time batch lost its spacing from its first output");

    // The same two batches without just-in-time keep the chained start.
    const auto chained = releasedPair(false, 26ms);
    expect(releasedPairCompleted(chained), "chained released pair did not complete");
    expect(chained.second->calls[0].scheduled >= chainedFloorAfter(*chained.first, interval),
        "chained batch was not held to the previous batch's planned phase");
}
// L2 (low_latency): a batch ready before the previous real output started keeps a quarter interval after
// that real output's actual start. Its phase lock still starts it at or after that bound.
void justInTimeBatchKeepsQuarterIntervalAfterPreviousReal() {
    const auto interval = 8ms;
    const auto early = releasedPair(true, 12ms);
    expect(releasedPairCompleted(early), "early-ready released pair did not complete");
    const auto previousReal = early.first->calls[1].started;
    const auto& firstOfB = early.second->calls[0];
    expect(firstOfB.scheduled >= previousReal + interval / 4,
        "a batch ready before the previous real output started did not keep a quarter interval after it");
    expect(firstOfB.scheduled < chainedFloorAfter(*early.first, interval),
        "a batch ready before the previous real output started was held to the planned phase");
}
// BH1 (batch_hold): the same just-in-time pair with the hold on and off. On, batch B starts 2 ms after its readiness
// (the phase lock, as in justInTimeBatchStartsAtItsReadiness). Off, it starts at its readiness with no lock and no
// margin, and its spacing is kept.
void justInTimeBatchHoldOffStartsAtReadiness() {
    const auto interval = 8ms;
    const auto on = releasedPair(true, 26ms, std::nullopt, true);
    const auto off = releasedPair(true, 26ms, std::nullopt, false);
    expect(releasedPairCompleted(on) && releasedPairCompleted(off),
        "batch hold pair did not complete");
    expect(on.second->batchPhaseHold > 1ms && on.second->batchPhaseHold < 3ms,
        "batch hold on did not hold the batch about 2 ms after its readiness");
    expect(on.second->calls[0].scheduled == on.second->readyAt + on.second->batchPhaseHold,
        "batch hold on did not start the first output at its readiness plus the hold");
    expect(off.second->batchPhaseHold == 0ns,
        "batch hold off still held the batch after its readiness");
    expect(off.second->calls[0].scheduled == off.second->readyAt,
        "batch hold off did not start the first output at its readiness");
    expect(off.second->calls[1].started >= off.second->calls[0].started + interval - 1ms,
        "batch hold off lost its spacing from its first output");
}
// BH2 (batch_hold, low_latency off): a batch that is not just in time takes the same phase-locked hold. Batch B is
// ready 26 ms after the start, and its lock starts it 2 ms later. The chained floor (27.5 ms here) is 0.5 ms earlier,
// so the hold sets the start, and the batch starts at its readiness plus the recorded hold.
void chainedBatchHoldStartsAfterReadiness() {
    const auto interval = 8ms;
    const auto on = releasedPair(false, 26ms, std::nullopt, true);
    expect(releasedPairCompleted(on), "low-latency-off batch hold pair did not complete");
    expect(on.second->batchPhaseHold > 1ms && on.second->batchPhaseHold < 3ms,
        "batch hold on did not hold a batch that is not just in time about 2 ms after its readiness");
    expect(on.second->calls[0].scheduled == on.second->readyAt + on.second->batchPhaseHold,
        "batch hold on did not start a batch that is not just in time at its readiness plus the hold");
    expect(on.second->calls[1].started >= on.second->calls[0].started + interval - 1ms,
        "batch hold on lost the spacing of a batch that is not just in time");
}
// BH2: the same batch shape with the hold off, ready 30 ms after the start (after the chained floor): it starts at its
// readiness, with no lock and no margin.
void chainedBatchHoldOffStartsAtReadiness() {
    const auto off = releasedPair(false, 30ms, std::nullopt, false);
    expect(releasedPairCompleted(off), "low-latency-off batch hold off pair did not complete");
    expect(off.second->batchPhaseHold == 0ns,
        "batch hold off still held a batch that is not just in time after its readiness");
    expect(off.second->calls[0].scheduled == off.second->readyAt,
        "batch hold off did not start a batch that is not just in time at its readiness");
}
// BH2: a batch ready 20 ms after the start has its schedule (the chained floor) well after its readiness plus the
// hold, so the hold does not move it: it starts at the schedule with the hold on and with it off.
void chainedBatchHoldKeepsItsSchedule() {
    const auto interval = 8ms;
    for (const bool batchHold : {true, false}) {
        const auto pair = releasedPair(false, 20ms, std::nullopt, batchHold);
        expect(releasedPairCompleted(pair), "low-latency-off batch schedule pair did not complete");
        expect(pair.second->calls[0].scheduled == chainedFloorAfter(*pair.first, interval),
            "a batch ready well before its schedule was moved off its schedule by the batch hold");
    }
}
// OS2: a just-in-time batch's first generated output takes its automatic correction after the batch's readiness
// start (readyAt plus the phase-locked hold, 2 ms here). The start replaces the unmoved deadline, so a correction
// taken before it was lost. The real after the output keeps its unmoved spacing in every run.
void offsetAppliesAfterReadinessStart() {
    const auto interval = 8ms;
    const auto zero = releasedPair(true, 26ms, 0ns);
    const auto late = releasedPair(true, 26ms, 1ms);
    const auto early = releasedPair(true, 26ms, -1ms);
    const auto deep = releasedPair(true, 26ms, -3ms);
    for (const auto* pair : {&zero, &late, &early, &deep})
        expect(releasedPairCompleted(*pair), "offset just-in-time released pair did not complete");
    const auto startOf = [](const ReleasedPair& pair) {
        return pair.second->readyAt + pair.second->batchPhaseHold;
    };
    // The real is one interval after the generated output's unmoved start, whatever the correction.
    const auto realFollowsUnmovedStart = [&](const ReleasedPair& pair) {
        const auto& generated = pair.second->calls[0];
        return pair.second->calls[1].scheduled == generated.started - generated.offset + interval;
    };
    // The -1 ms case needs its start 1 ms or more after readyAt, and the -3 ms case needs it less than 3 ms after.
    expect(zero.second->batchPhaseHold > 1ms && zero.second->batchPhaseHold < 3ms,
        "the readiness hold is outside the 1 to 3 ms this test needs");

    // Zero correction: the first generated output starts at its readiness start, as before.
    const auto& zeroGenerated = zero.second->calls[0];
    expect(zeroGenerated.generated && !zeroGenerated.skipped && zeroGenerated.offset == 0ns &&
            zeroGenerated.scheduled == startOf(zero),
        "a zero correction moved the first generated output off its readiness start");
    expect(realFollowsUnmovedStart(zero), "a zero correction moved the real output");

    // +1 ms: 1 ms after the readiness start, and the recorded offset is the 1 ms taken.
    const auto& lateGenerated = late.second->calls[0];
    expect(!lateGenerated.skipped && lateGenerated.offset == 1ms &&
            lateGenerated.scheduled == startOf(late) + 1ms,
        "a +1 ms correction did not move the first generated output 1 ms after its readiness start");
    expect(realFollowsUnmovedStart(late), "a +1 ms correction moved the real output");
    const auto lateReal = late.second->calls[1].scheduled - startOf(late);
    const auto zeroReal = zero.second->calls[1].scheduled - startOf(zero);
    expect(lateReal - zeroReal < 2ms && zeroReal - lateReal < 2ms,
        "the real output after a +1 ms correction is not where the zero-correction run puts it");

    // -1 ms: earlier than the start, never before readyAt; the offset is the part taken.
    const auto& earlyGenerated = early.second->calls[0];
    expect(!earlyGenerated.skipped && earlyGenerated.offset == -1ms &&
            earlyGenerated.scheduled == startOf(early) - 1ms &&
            earlyGenerated.scheduled >= early.second->readyAt,
        "a -1 ms correction did not move the first generated output 1 ms earlier, not before readyAt");
    expect(realFollowsUnmovedStart(early), "a -1 ms correction moved the real output");

    // -3 ms, deeper than the hold: the output stops at readyAt and the offset records the 2 ms taken.
    const auto& deepGenerated = deep.second->calls[0];
    expect(!deepGenerated.skipped && deepGenerated.scheduled == deep.second->readyAt &&
            deepGenerated.offset == -deep.second->batchPhaseHold,
        "a correction deeper than the readiness hold moved the first generated output before readyAt, "
        "or recorded more than it took");
    expect(realFollowsUnmovedStart(deep), "a deep correction moved the real output");
}
// OS2: the same correction on a non-just-in-time overlap pair. Where its readiness is before the chained floor
// (chainedFloorAfter: the previous real output's scheduled or actual start, whichever is earlier, plus the interval
// less a sixteenth), the output is at that floor plus the correction, as before OS2. Where its readiness is after
// the floor, the output is at its readiness start plus the correction: that start is its unmoved slot.
void chainedPairOffsetsFromItsUnmovedStart() {
    const auto interval = 8ms;
    const auto early = releasedPair(false, 20ms, 1ms);
    expect(releasedPairCompleted(early), "chained offset pair did not complete");
    const auto& earlyGenerated = early.second->calls[0];
    expect(!earlyGenerated.skipped && earlyGenerated.offset == 1ms &&
            earlyGenerated.scheduled == chainedFloorAfter(*early.first, interval) + 1ms,
        "a chained batch's correction did not move its chained floor by the correction");
    expect(early.second->calls[1].scheduled == earlyGenerated.started - earlyGenerated.offset + interval,
        "a chained batch's real output did not follow its generated output's unmoved start");

    const auto late = releasedPair(false, 30ms, 1ms);
    expect(releasedPairCompleted(late), "late-ready chained offset pair did not complete");
    const auto& lateGenerated = late.second->calls[0];
    expect(!lateGenerated.skipped && lateGenerated.offset == 1ms &&
            lateGenerated.scheduled == late.second->readyAt + 1ms,
        "a chained batch ready after its floor did not take its correction after its readiness start");
}
// OS1b: one worker batch without the release gate (overlap false): a generated output with internal images, then its
// real. Its readiness is 10 ms after the start, and the batch is just in time with batch_hold on, as a detached ready
// batch is with low_latency. The generated output takes `generatedOffset` after its readiness start.
DeferredPresent::Completion nonOverlapGeneratedBatch(const std::chrono::nanoseconds generatedOffset) {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    VkSwapchainKHR swapchain = handle<VkSwapchainKHR>(11);
    VkSemaphore wait = handle<VkSemaphore>(12);
    uint32_t image = 0;
    VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    DeferredPresent worker(queue, timedPresent);
    const auto start = DeferredPresent::Clock::now();
    const auto interval = 8ms;
    simulatedReady[1] = start + 10ms;
    setRealOffsetAutoConfigured(true);
    copySubmits.clear();
    copyAcquireResult = VK_SUCCESS;
    copyAcquireImage = 2;
    DeferredPresentTestAccess::setOutputOffsets(worker, 1, {generatedOffset, 0ns, 0ns, 0ns});
    expect(worker.beginBatch(2, {.device = handle<VkDevice>(21), .semaphore = handle<VkSemaphore>(31),
            .value = 1, .wait = readyAtInstant, .deadline = start + 1s, .overlap = false,
            .justInTime = true, .batchHold = true, .outputInterval = interval}),
        "non-overlap batch rejected");
    auto copy = generatedCopy();
    copy.internalImages = true;
    expect(worker.enqueueGeneratedCopy(swapchain, nullptr, start, copy),
        "non-overlap batch's generated output rejected");
    image = 1;
    expect(worker.enqueue(info, start + interval), "non-overlap batch's real output rejected");
    worker.closeBatch();
    auto completion = worker.finish(UINT64_MAX);
    setRealOffsetAutoConfigured(false);
    expect(completion && completion->count == 2 && !completion->calls[0].skipped && !completion->calls[1].skipped,
        "non-overlap batch did not complete");
    return *completion;
}
// OS1b: a worker batch without the release gate takes the automatic correction of its generated output, as a released
// batch does. +1 ms moves that output 1 ms after its readiness start; its real output keeps its unmoved slot.
void nonOverlapBatchOffsetMovesGeneratedOnly() {
    const auto interval = 8ms;
    const auto startOf = [](const DeferredPresent::Completion& batch) { return batch.readyAt + batch.batchPhaseHold; };
    const auto zero = nonOverlapGeneratedBatch(0ns);
    const auto& zeroGenerated = zero.calls[0];
    expect(zeroGenerated.generated && zeroGenerated.offset == 0ns && zeroGenerated.scheduled == startOf(zero),
        "a zero correction moved a non-overlap batch's generated output off its readiness start");

    const auto late = nonOverlapGeneratedBatch(1ms);
    const auto& lateGenerated = late.calls[0];
    expect(lateGenerated.generated && lateGenerated.offset == 1ms && lateGenerated.scheduled == startOf(late) + 1ms,
        "a +1 ms correction did not move a non-overlap batch's generated output 1 ms after its readiness start");
    const auto& lateReal = late.calls[1];
    expect(!lateReal.generated && lateReal.offset == 0ns, "a non-overlap batch's real output carries a correction");
    expect(lateReal.scheduled == lateGenerated.started - lateGenerated.offset + interval,
        "a +1 ms correction moved a non-overlap batch's real output off its unmoved slot");
}
// OS1b: the controller keeps its pending real across consecutive worker batches without the release gate. Batch A's
// real screen sample is read at batch B's first present, after B was admitted. With A's generated output (source 1,
// midway between the reals) it completes a sample, which is accepted. The batch boundary used to forget that real.
void nonOverlapPendingRealSurvivesBatches() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    VkSwapchainKHR swapchain = handle<VkSwapchainKHR>(11);
    VkSemaphore wait = handle<VkSemaphore>(12);
    uint32_t image = 0;
    VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    DeferredPresent worker(queue, timedPresent);
    auto ring = std::make_shared<ScreenSampleRing>();
    worker.setScreenSamples(ring);
    setRealOffsetAutoConfigured(true);
    const int64_t base = steadyNanoseconds(DeferredPresent::Clock::now());
    const auto push = [&](const uint64_t source, const bool generated, const int64_t offsetNs) {
        ring->push(ScreenSample{0, source, generated, base + offsetNs, 0, 0});
    };
    const auto admit = [&](const bool second) {
        expect(worker.beginBatch(2, {.device = handle<VkDevice>(21), .semaphore = handle<VkSemaphore>(31),
                .value = 1, .overlap = false, .outputInterval = 8ms}),
            second ? "second non-overlap batch rejected after the first was finished"
                   : "first non-overlap batch rejected");
    };
    const auto now = DeferredPresent::Clock::now();

    // Batch A: the real of source 0 and the generated output of source 1 are read at A's first present.
    push(0, false, 0);
    push(1, true, 6'500'000);
    admit(false);
    expect(worker.enqueue(info, now), "first non-overlap batch's first output rejected");
    image = 1;
    expect(worker.enqueue(info, now + 8ms), "first non-overlap batch's second output rejected");
    worker.closeBatch();
    expect(worker.finish(UINT64_MAX).has_value(), "first non-overlap batch did not finish");

    // Batch B is admitted, then the real of source 1 arrives. Its first present reads that sample.
    admit(true);
    push(1, false, 13'000'000);
    expect(worker.enqueue(info, now), "second non-overlap batch's first output rejected");
    image = 2;
    expect(worker.enqueue(info, now + 8ms), "second non-overlap batch's second output rejected");
    worker.closeBatch();
    expect(worker.finish(UINT64_MAX).has_value(), "second non-overlap batch did not finish");
    const auto accepted = DeferredPresentTestAccess::acceptedOffsetSamples(worker);
    setRealOffsetAutoConfigured(false);
    expect(accepted == 1,
        "a screen sample of the previous non-overlap batch's real was not accepted across the batch boundary");
}

void pacingCorrectionReloadsOnAnExistingWorker() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, timedPresent);
    auto ring = std::make_shared<ScreenSampleRing>();
    worker.setScreenSamples(ring);
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto wait = handle<VkSemaphore>(12);
    const uint32_t image = 0;
    const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    const auto batch = [&](const uint64_t source) {
        const auto at = DeferredPresent::Clock::now() + 2ms;
        expect(worker.beginBatch(2, {.outputInterval = 5ms, .source = source}),
            "live correction batch was rejected");
        expect(worker.enqueue(info, at, true) && worker.enqueue(info, at + 5ms),
            "live correction outputs were rejected");
        worker.closeBatch();
        const auto done = worker.finish(UINT64_MAX);
        expect(done && done->count == 2 && done->calls[1].offset == 0ns,
            "live correction changed the real output's offset");
        return done->calls[0].offset;
    };
    const auto base = steadyNanoseconds(DeferredPresent::Clock::now());
    const auto sample = [&](const uint64_t source, const bool generated, const int64_t ms) {
        ring->push(ScreenSample{0, source, generated, base + ms * 1'000'000});
    };

    setRealOffsetAutoConfigured(false);
    expect(batch(1) == 0ns, "a worker started with correction disabled moved an output");
    sample(0, false, 0);
    sample(1, true, 7);
    sample(1, false, 12);
    setRealOffsetAutoConfigured(true);
    expect(batch(2) == 0ns && DeferredPresentTestAccess::acceptedOffsetSamples(worker) == 0,
        "enabling correction consumed feedback from earlier batches");
    sample(2, false, 24);
    sample(3, true, 31);
    sample(3, false, 36);
    static_cast<void>(batch(3));
    expect(batch(4) < 0ns, "fresh feedback did not drive the existing worker after enabling");
    setRealOffsetAutoConfigured(false);
    expect(batch(5) == 0ns, "disabling correction left a generated offset active");
    setRealOffsetAutoConfigured(true);
    expect(batch(6) == 0ns, "re-enabling correction reused its old offset");
    setRealOffsetAutoConfigured(false);
}

// OS1c: one worker batch whose first output is a plain deferred generated present (no worker copy: its lower image is
// acquired and copied before admission), then its real output. Readiness is `ready` after the start, and the batch is
// just in time with batch_hold on. The generated output takes `generatedOffset`. `expedite` joins the batch at once, as
// a released batch does (expediteOnJoin is set on released batches only, retirement.cpp). The result holds the batch,
// the worker's controller read after the batch finished, and every lower present in presentation order.
struct PlainGeneratedBatch {
    DeferredPresent::Completion batch;
    OutputOffsetController controller;
    std::vector<std::pair<uint32_t, VkSemaphore>> presents;
};
PlainGeneratedBatch plainGeneratedBatch(const std::chrono::nanoseconds generatedOffset,
        const std::chrono::milliseconds ready, const bool overlap, const bool expedite) {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    VkSwapchainKHR swapchain = handle<VkSwapchainKHR>(11);
    VkSemaphore generatedWait = handle<VkSemaphore>(71);
    VkSemaphore realWait = handle<VkSemaphore>(12);
    uint32_t generatedImage = 2, realImage = 1;
    VkPresentInfoKHR generatedInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &generatedWait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &generatedImage};
    VkPresentInfoKHR realInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
    copyPresents.clear();
    DeferredPresent worker(queue, recordingPresent);
    const auto start = DeferredPresent::Clock::now();
    const auto interval = 8ms;
    simulatedReady[1] = start + ready;
    setRealOffsetAutoConfigured(true);
    DeferredPresentTestAccess::setOutputOffsets(worker, 1, {generatedOffset, 0ns, 0ns, 0ns});
    expect(worker.beginBatch(2, {.device = handle<VkDevice>(21), .semaphore = handle<VkSemaphore>(31),
            .value = 1, .wait = readyAtInstant, .deadline = start + 1s, .expediteOnJoin = expedite,
            .overlap = overlap, .justInTime = true, .batchHold = true, .outputInterval = interval}),
        "plain generated batch rejected");
    expect(worker.enqueue(generatedInfo, start, true), "plain generated output rejected");
    expect(worker.enqueue(realInfo, start + interval), "plain batch's real output rejected");
    worker.closeBatch();
    auto completion = worker.finish(UINT64_MAX);
    PlainGeneratedBatch result;
    result.controller = DeferredPresentTestAccess::offsetController(worker);
    setRealOffsetAutoConfigured(false);
    expect(completion && completion->count == 2 && !completion->calls[0].skipped && !completion->calls[1].skipped,
        "plain generated batch did not complete");
    result.batch = *completion;
    result.presents = copyPresents;
    return result;
}
// OS1c: a plain deferred generated present takes the automatic correction in a non-overlap batch. +1 ms moves it 1 ms
// after its readiness start, and the recorded offset is that 1 ms. The real keeps its unmoved slot. The presents go out
// in admission order, each with the image and wait it was admitted with.
void plainGeneratedPresentOffsetMoves() {
    const auto interval = 8ms;
    const auto startOf = [](const DeferredPresent::Completion& batch) { return batch.readyAt + batch.batchPhaseHold; };
    const auto zero = plainGeneratedBatch(0ns, 10ms, false, false);
    const auto& zeroGenerated = zero.batch.calls[0];
    expect(zeroGenerated.generated && zeroGenerated.offset == 0ns && zeroGenerated.scheduled == startOf(zero.batch),
        "a zero correction moved a plain generated present off its readiness start");

    const auto late = plainGeneratedBatch(1ms, 10ms, false, false);
    const auto& lateGenerated = late.batch.calls[0];
    expect(lateGenerated.generated && !lateGenerated.skipped && lateGenerated.offset == 1ms &&
            lateGenerated.scheduled == startOf(late.batch) + 1ms,
        "a +1 ms correction did not move a plain generated present 1 ms after its readiness start");
    const auto& lateReal = late.batch.calls[1];
    expect(!lateReal.generated && lateReal.offset == 0ns && lateReal.scheduled == startOf(late.batch) + interval,
        "a +1 ms correction moved the real output of a plain generated batch off its unmoved slot");
    const std::vector<std::pair<uint32_t, VkSemaphore>> admitted{
        {2, handle<VkSemaphore>(71)}, {1, handle<VkSemaphore>(12)}};
    expect(late.presents == admitted,
        "a plain generated present went out with an image or wait other than the one it was admitted with");
}
// OS1c: a plain generated move that is cut sets `limited`, so the controller holds further positive integration. The
// expedite joins the batch before its readiness: the output starts at readiness, before its moved slot, so the
// correction records only the part taken (zero here) and the limit is set in the direction of the move.
void plainGeneratedPresentCutLimitsOffset() {
    const auto cut = plainGeneratedBatch(1ms, 40ms, true, true);
    const auto& generated = cut.batch.calls[0];
    expect(generated.generated && !generated.skipped && generated.started < generated.scheduled &&
            generated.offset < 1ms,
        "an expedited +1 ms plain generated present kept its whole move");
    expect(cut.controller.limited[0] == 1, "a cut plain generated move did not set its limit");

    // Further positive integration is held. A generated output landing 0.5 ms before its ideal point (8 ms step,
    // ideal at 4 ms) would raise the offset, so it is counted as held and the offset stays.
    OutputOffsetController controller = cut.controller;
    controller.forget();
    const auto heldBefore = controller.held;
    const auto offsetBefore = controller.offset[0];
    (void)controller.observe(false, 100, 0ns);
    (void)controller.observe(true, 101, 3500us);
    (void)controller.observe(false, 101, 8ms);
    expect(controller.held == heldBefore + 1 && controller.offset[0] == offsetBefore,
        "a cut plain generated move did not hold further positive integration");
}
}

// X3: the empty submits the worker makes to signal an application present fence that no lower
// present received. Each one is recorded in order.
namespace {
std::mutex fenceSignalMutex;
std::vector<VkFence> fenceSignals;
VkResult VKAPI_CALL fenceSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* info, VkFence fence) {
    expect(queue == handle<VkQueue>(7) && count == 0 && info == nullptr,
        "application present fence was not signalled by an empty submit on the private queue");
    const std::lock_guard lock(fenceSignalMutex);
    fenceSignals.push_back(fence);
    return VK_SUCCESS;
}
std::vector<VkFence> signalledFences() {
    const std::lock_guard lock(fenceSignalMutex);
    return fenceSignals;
}
void clearSignalledFences() {
    const std::lock_guard lock(fenceSignalMutex);
    fenceSignals.clear();
}
// Lower callbacks for an internal output that never becomes ready, so the worker is still
// inside the job when it is asked to stop.
struct StoppingFake {
    std::atomic_int polls{};
    std::atomic_int abandons{};
};
VkResult stoppingTryAcquire(void* owner, uint32_t*) {
    ++static_cast<StoppingFake*>(owner)->polls;
    return VK_NOT_READY;
}
VkResult stoppingSubmit(void*, VkQueue, uint32_t, uint32_t, VkSemaphore, VkSemaphore*) {
    expect(false, "a stopped internal output was copied");
    return VK_ERROR_UNKNOWN;
}
VkResult stoppingAbandon(void* owner, VkQueue queue, uint32_t, VkSemaphore) {
    expect(queue == handle<VkQueue>(7), "abandon submitted on the wrong queue");
    ++static_cast<StoppingFake*>(owner)->abandons;
    return VK_SUCCESS;
}
}

// (X3a) An internal output that ends skipped on an acquire error signals its application fence
// exactly once, and only when the application passed one. It never reaches a lower present.
void skippedInternalRealSignalsItsFenceOnce() {
    for (const bool withFence : {false, true}) {
        auto queue = std::make_shared<DeferredPresentQueue>();
        queue->queue = handle<VkQueue>(7);
        DeferredPresent worker(queue, recordingPresent, VK_NULL_HANDLE, fenceSubmit);
        copyPresents.clear();
        clearSignalledFences();
        const auto swapchain = handle<VkSwapchainKHR>(11);
        const auto continuation = handle<VkSemaphore>(12);
        const auto fence = handle<VkFence>(0x41);
        VkSwapchainPresentFenceInfoEXT fences{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
            .swapchainCount = 1, .pFences = &fence,
        };
        uint32_t applicationImage = 2;
        const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = withFence ? &fences : nullptr,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &continuation,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &applicationImage};
        auto fake = std::make_shared<InternalFake>();
        fake->notReadyPolls = 1;
        fake->failure = VK_ERROR_OUT_OF_DATE_KHR;
        expect(worker.beginBatch(1), "fenced failing internal batch rejected");
        expect(worker.enqueueInternalReal(info, DeferredPresent::Clock::now(), internalCopy(fake, 2)),
            "fenced failing internal output rejected");
        worker.closeBatch();
        const auto completion = worker.finish(UINT64_MAX);
        expect(completion && completion->count == 1 && completion->calls[0].skipped &&
            fake->abandons == 1 && fake->submits == 0 && copyPresents.empty(),
            "failing internal output did not skip after abandoning its image once");
        expect(signalledFences() == (withFence ? std::vector{fence} : std::vector<VkFence>{}),
            "a skipped job's application present fence was not signalled exactly once");
    }
}

// (X3b) An internal output still waiting for its deadline when the worker stops ends skipped as
// Stopping, and its application fence is signalled exactly once.
void stoppedInternalRealSignalsItsFenceOnce() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent, VK_NULL_HANDLE, fenceSubmit);
    clearSignalledFences();
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto continuation = handle<VkSemaphore>(12);
    const auto fence = handle<VkFence>(0x42);
    VkSwapchainPresentFenceInfoEXT fences{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
        .swapchainCount = 1, .pFences = &fence,
    };
    uint32_t applicationImage = 1;
    const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = &fences,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &continuation,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &applicationImage};
    auto fake = std::make_shared<StoppingFake>();
    const DeferredPresent::InternalRealCopy copy{
        .owner = fake, .tryAcquire = stoppingTryAcquire, .submit = stoppingSubmit,
        .abandon = stoppingAbandon, .applicationIndex = 1,
    };
    expect(worker.beginBatch(1), "stopped internal batch rejected");
    // A long deadline keeps the job in flight until the stop, even under a loaded test run.
    expect(worker.enqueueInternalReal(info, DeferredPresent::Clock::now() + 500ms, copy),
        "stopped internal output rejected");
    worker.closeBatch();
    // Stop only once the worker is inside the job, so the job is not dropped before it starts.
    for (int waited = 0; fake->polls.load() == 0 && waited < 5000; ++waited)
        std::this_thread::sleep_for(1ms);
    expect(fake->polls.load() > 0, "the worker never started the internal output");
    DeferredPresentTestAccess::requestStop(worker);
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 1 && completion->calls[0].skipped &&
        completion->calls[0].skipReason == DeferredPresent::SkipReason::Stopping &&
        fake->abandons == 1,
        "stopped internal output did not skip as Stopping after abandoning its image once");
    expect(signalledFences() == std::vector{fence},
        "a stopped job's application present fence was not signalled exactly once");
}

// (X3c) A present whose lower present received the fence never gets a layer signal, whatever that
// present returned: plain and internal outputs, with success, out-of-date and surface loss.
void presentedJobsKeepTheirFenceForTheLowerPresent() {
    for (const VkResult lower : {VK_SUCCESS, VK_ERROR_OUT_OF_DATE_KHR, VK_ERROR_SURFACE_LOST_KHR}) {
        for (const bool internal : {false, true}) {
            lowerResult = lower;
            auto queue = std::make_shared<DeferredPresentQueue>();
            queue->queue = handle<VkQueue>(7);
            DeferredPresent worker(queue, present, VK_NULL_HANDLE, fenceSubmit);
            clearSignalledFences();
            const auto swapchain = handle<VkSwapchainKHR>(11);
            const auto continuation = handle<VkSemaphore>(12);
            const auto fence = handle<VkFence>(0x43);
            VkSwapchainPresentFenceInfoEXT fences{
                .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
                .swapchainCount = 1, .pFences = &fence,
            };
            uint32_t applicationImage = 0;
            const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .pNext = &fences,
                .waitSemaphoreCount = 1, .pWaitSemaphores = &continuation,
                .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &applicationImage};
            const auto deadline = DeferredPresent::Clock::now();
            auto fake = std::make_shared<InternalFake>();
            fake->presentWait = handle<VkSemaphore>(95);
            expect(worker.beginBatch(1), "fenced presented batch rejected");
            const bool admitted = internal
                ? worker.enqueueInternalReal(info, deadline, internalCopy(fake, 0))
                : worker.enqueue(info, deadline);
            expect(admitted, "fenced presented output rejected");
            worker.closeBatch();
            const auto completion = worker.finish(UINT64_MAX);
            expect(completion && completion->count == 1 && !completion->calls[0].skipped &&
                completion->calls[0].result == lower,
                "fenced presented output was skipped or lost its lower result");
            expect(observedFence == fence,
                "the lower present did not receive the application present fence");
            expect(signalledFences().empty(),
                "the layer signalled a present fence that the lower present had received");
        }
    }
    lowerResult = VK_SUCCESS;
}

// RA1: two outputs of one batch, admitted with Reflex IDs 7 and 8, present on the worker each inside its own
// out-of-band pair, one pair after the other: 10(7), present, 11(7), 10(8), present, 11(8).
namespace {
struct SentMarker {
    uint64_t presentId{};
    VkLatencyMarkerNV marker{};
    // Lower presents finished when the marker was sent.
    size_t presentsDone{};
    bool operator==(const SentMarker& other) const {
        return presentId == other.presentId && marker == other.marker && presentsDone == other.presentsDone;
    }
};
std::mutex sentMarkerMutex;
std::vector<SentMarker> sentMarkers;
void VKAPI_CALL recordSentMarker(VkDevice device, VkSwapchainKHR swapchain, const VkSetLatencyMarkerInfoNV* info) {
    expect(device == handle<VkDevice>(9) && swapchain == handle<VkSwapchainKHR>(11) && info,
        "a worker marker went to the wrong device or swapchain");
    const auto presentsDone = presentedIds().size();
    const std::lock_guard lock(sentMarkerMutex);
    sentMarkers.push_back({info->presentID, info->marker, presentsDone});
}
}

void reflexMarkersEncloseEachWorkerPresent() {
    clearPresentLog();
    {
        const std::lock_guard lock(sentMarkerMutex);
        sentMarkers.clear();
    }
    lowerResult = VK_SUCCESS;
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, present);
    worker.setReflexMarkers({.device = handle<VkDevice>(9), .mark = recordSentMarker});
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto wait = handle<VkSemaphore>(12);
    uint32_t image = 0;
    const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
    const auto deadline = DeferredPresent::Clock::now();
    expect(worker.beginBatch(2), "reflex batch rejected");
    expect(worker.enqueue(info, deadline, false, {}, false, 7), "the output with Reflex ID 7 was rejected");
    image = 1;
    expect(worker.enqueue(info, deadline, false, {}, false, 8), "the output with Reflex ID 8 was rejected");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 2 && !completion->calls[0].skipped && !completion->calls[1].skipped &&
        completion->calls[0].result == VK_SUCCESS && completion->calls[1].result == VK_SUCCESS,
        "a Reflex output was skipped or lost its lower result");
    std::vector<SentMarker> sent;
    {
        const std::lock_guard lock(sentMarkerMutex);
        sent = sentMarkers;
    }
    const std::vector<SentMarker> expected{
        {7, VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV, 0},
        {7, VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV, 1},
        {8, VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV, 1},
        {8, VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV, 2},
    };
    expect(sent == expected, "the worker's out-of-band pairs did not each enclose one lower present in order");
}

// (X3d) A generated output that skips for want of a lower image signals its application fence once.
void skippedGeneratedCopySignalsItsFenceOnce() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent, VK_NULL_HANDLE, fenceSubmit);
    copySubmits.clear(); copyPresents.clear(); copyAcquireCalls = 0; copyAcquireResult = VK_NOT_READY;
    clearSignalledFences();
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto realWait = handle<VkSemaphore>(12);
    const auto fence = handle<VkFence>(0x44);
    VkSwapchainPresentFenceInfoEXT fences{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
        .swapchainCount = 1, .pFences = &fence,
    };
    uint32_t realImage = 4;
    const VkPresentInfoKHR realInfo{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &realWait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &realImage};
    const auto copy = generatedCopy();
    const auto deadline = DeferredPresent::Clock::now();
    expect(worker.beginBatch(2), "fenced skipped generated batch rejected");
    expect(worker.enqueueGeneratedCopy(swapchain, &fences, deadline, copy),
        "fenced skipped generated copy rejected");
    expect(worker.enqueue(realInfo, deadline + 1ms), "real output rejected after fenced skipped generated copy");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 2 && completion->calls[0].skipped &&
        !completion->calls[1].skipped,
        "fenced generated output was not a skip ahead of the real output");
    expect(copySubmits.size() == 1 && copySubmits.front().fence == handle<VkFence>(64) &&
        copyPresents.size() == 1 && copyPresents[0] == std::pair{4u, realWait},
        "fenced generated skip presented an image or lost the real output");
    expect(signalledFences() == std::vector{fence},
        "a skipped generated output's application present fence was not signalled exactly once");
    copyAcquireResult = VK_SUCCESS;
}

namespace {
// GT1 schedule simulation, in the worker's order (rule 4). The grid learns from each output's screen sample two outputs
// after it started, as the watcher reports them, and an output snaps once the lock is snapping. The stream is a game
// frame every 1/115 s: a real output opens the frame's batch, and a generated output follows half an interval later.
// The real output's floor is its readiness, the generated output's the previous actual start plus P/2. Each start is
// moved by up to 800 us and by up to 100 us of
// wake time. The compositor latches an output on the first tick at or after its start plus 0.6 ms and up to 0.3 ms
// more. The free-latch variant has no ticks at all (VRR-like), so its samples never fit a grid.
constexpr int64_t gt1Grid = 1'000'000'000'000;
constexpr int64_t gt1Period = 4'166'667; // 240 Hz, the refresh period the cap uses
constexpr int64_t gt1Interval = 8'695'652; // 115 frames per second
constexpr int64_t gt1Jitter = 800'000;
constexpr int64_t gt1Wake = 100'000;
constexpr int64_t gt1Latency = 600'000;
constexpr int64_t gt1Ready = 2'000'000; // a frame's batch is ready this long before its real output
constexpr int64_t gt1LatchSpan = 300'000;
constexpr size_t gt1Lag = 2;

uint64_t gt1Mix(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}
// A deterministic value in [0, span) for output `index`; the salt separates the uses.
int64_t gt1Spread(const size_t index, const uint64_t salt, const int64_t span) {
    return static_cast<int64_t>(gt1Mix(index * 2654435761ull + salt) % static_cast<uint64_t>(span));
}
int64_t gt1Latch(const int64_t started, const size_t index) {
    const int64_t ready = started + gt1Latency + gt1Spread(index, 77, gt1LatchSpan);
    return gt1Grid + ceilDivide(ready - gt1Grid, gt1Period) * gt1Period;
}

struct GT1Output {
    int64_t deadline{};
    int64_t started{};
    int64_t screen{};
    int64_t target{};
    bool snapped{};
};
struct GT1Stream {
    std::vector<GT1Output> outputs;
    size_t firstSnapped{};
    size_t misses{};
    size_t early{};
    bool valid{};
};

// tickLock false is today's schedule. tickLatch false is the VRR-like latch.
GT1Stream gt1Stream(const size_t count, const bool tickLock, const bool tickLatch) {
    TickLock lock;
    lock.setRefresh(gt1Period);
    GT1Stream stream;
    stream.outputs.reserve(count);
    stream.firstSnapped = count;
    int64_t previousStarted = 0;
    for (size_t i = 0; i < count; ++i) {
        if (tickLock && i >= gt1Lag) {
            const GT1Output& seen = stream.outputs[i - gt1Lag];
            const TickVerdict verdict = lock.observe(TickSample{seen.screen, seen.started, seen.target});
            if (verdict == TickVerdict::miss) ++stream.misses;
            else if (verdict == TickVerdict::early) ++stream.early;
        }
        const int64_t frame = gt1Grid + 5 * gt1Period + static_cast<int64_t>(i / 2) * gt1Interval +
            (i % 2 ? gt1Interval / 2 : 0);
        GT1Output out;
        out.deadline = frame + gt1Spread(i, 11, gt1Jitter) - gt1Jitter / 2;
        if (tickLock && lock.snapping()) {
            // The batch's first output (the real one) floors at its readiness; a later output at the previous start + P/2.
            const int64_t floorNs = i % 2 == 0 ? frame - gt1Ready : previousStarted + gt1Period / 2;
            const TickSlot slot = lock.snap(out.deadline, floorNs);
            out.deadline = slot.deadline;
            out.target = slot.target;
            out.snapped = true;
            if (stream.firstSnapped == count) stream.firstSnapped = i;
        }
        out.started = out.deadline + gt1Spread(i, 23, gt1Wake);
        out.screen = tickLatch ? gt1Latch(out.started, i) : out.started + gt1Latency + gt1Spread(i, 91, gt1Period);
        previousStarted = out.started;
        stream.outputs.push_back(out);
    }
    stream.valid = lock.gridValid();
    return stream;
}

// Consecutive outputs from `from` on that were latched on the same tick.
size_t gt1SameTickPairs(const GT1Stream& stream, const size_t from) {
    size_t pairs = 0;
    for (size_t i = from; i + 1 < stream.outputs.size(); ++i)
        if (stream.outputs[i].screen == stream.outputs[i + 1].screen) ++pairs;
    return pairs;
}

bool gt1SameSchedule(const GT1Stream& a, const GT1Stream& b) {
    if (a.outputs.size() != b.outputs.size()) return false;
    for (size_t i = 0; i < a.outputs.size(); ++i) {
        const GT1Output& x = a.outputs[i];
        const GT1Output& y = b.outputs[i];
        if (x.deadline != y.deadline || x.started != y.started || x.screen != y.screen || x.target != y.target ||
                x.snapped != y.snapped)
            return false;
    }
    return true;
}
}

// GT1 rule 4: once the lead is known, the gamescope-gated pair stream at 115 fps with a simulated latch has no
// same-tick pair, and the same stream without the tick lock has some.
void gamescopeTickLockRemovesSameTickPairs() {
    constexpr size_t count = 1600;
    const GT1Stream locked = gt1Stream(count, true, true);
    const GT1Stream plain = gt1Stream(count, false, true);
    expect(locked.firstSnapped < 300 && locked.valid, "the tick lock did not learn its grid and lead early");
    expect(std::all_of(locked.outputs.begin() + static_cast<std::ptrdiff_t>(locked.firstSnapped), locked.outputs.end(),
            [](const GT1Output& output) { return output.snapped; }),
        "an output after the lead was known was not snapped");
    expect(locked.misses == 0 && locked.early == 0, "a snapped output missed or caught an early tick");
    expect(gt1SameTickPairs(locked, locked.firstSnapped) == 0, "the tick lock left same-tick pairs");
    expect(gt1SameTickPairs(plain, locked.firstSnapped) > 0,
        "the same stream without the tick lock has no same-tick pairs");
}

// GT1 rule 1: every non-gamescope gate leaves the tick lock off, and the schedule is exactly today's.
void nonGamescopeGatesScheduleAsToday() {
    constexpr size_t count = 400;
    const GT1Stream today = gt1Stream(count, false, true);
    expect(today.firstSnapped == count, "a stream without the tick lock snapped an output");
    struct Gate {
        bool environment;
        bool gamescope;
        bool watcher;
        bool realOffset;
    };
    const std::array<Gate, 4> gates{{
        {false, true, true, true},
        {true, false, true, true},
        {true, true, false, true},
        {true, true, true, false},
    }};
    for (const Gate& gate : gates) {
        const TickLockPlan plan = tickLockPlan(gate.environment, gate.gamescope, gate.watcher, gate.realOffset);
        expect(!plan.enabled, "a non-gamescope gate enabled the tick lock");
        const GT1Stream run = gt1Stream(count, plan.enabled, true);
        expect(run.firstSnapped == count && gt1SameSchedule(run, today),
            "a non-gamescope gate changed the schedule");
    }
}

// GT1 zero snap: a stream whose samples never fit a tick snaps nothing, and its schedule is today's exactly.
void zeroSnapIsIdentical() {
    constexpr size_t count = 1600;
    const GT1Stream today = gt1Stream(count, false, false);
    const GT1Stream gated = gt1Stream(count, true, false);
    expect(gated.firstSnapped == count && !gated.valid, "a stream off the tick grid snapped an output");
    expect(gt1SameSchedule(gated, today), "a zero-snap tick lock changed the schedule");
}

// RP1: a real after a generated output of its batch is presented earlier by the preset lead, and its offset is that lead
// (negative). With the lead off the real keeps its slot. Where the lead would pass the quarter-interval floor after the
// generated output's unmoved start, the real stops at that floor instead.
void realPresentLeadMovesRealEarlier() {
    struct Outcome {
        DeferredPresent::Completion done;
        DeferredPresent::Clock::time_point first;
    };
    const auto leadBatch = [](const bool on, const std::chrono::nanoseconds preset,
            const std::chrono::nanoseconds interval) {
        auto pool = std::make_shared<PreparedPool>();
        preparedPool = pool.get();
        copySubmits.clear();
        auto queue = std::make_shared<DeferredPresentQueue>();
        queue->queue = handle<VkQueue>(7);
        setRealPresentLeadConfigured(on);
        DeferredPresent worker(queue, preparedPresent);
        DeferredPresentTestAccess::setRealLead(worker, preset);
        const auto swapchain = handle<VkSwapchainKHR>(11);
        const auto wait = handle<VkSemaphore>(12);
        const uint32_t image = 4;
        const VkPresentInfoKHR real{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image};
        const auto first = DeferredPresent::Clock::now() + 60ms;
        expect(worker.beginBatch(2, {.overlap = true, .outputInterval = interval}), "lead batch rejected");
        expect(worker.enqueueGeneratedCopy(swapchain, nullptr, first, preparedCopy()),
            "lead generated output rejected");
        const DeferredPresent::InternalRealCopy realCopy{.owner = pool, .tryAcquire = poolRealAcquire,
            .submit = poolRealSubmit, .abandon = poolRealAbandon};
        expect(worker.enqueueInternalReal(real, first + interval, realCopy), "lead real rejected");
        worker.closeBatch();
        const auto done = worker.finish(UINT64_MAX, false);
        setRealPresentLeadConfigured(false);
        expect(done && done->count == 2 && done->result == VK_SUCCESS && !done->calls[0].skipped &&
            !done->calls[1].skipped, "lead batch failed");
        return Outcome{*done, first};
    };
    // The real is planned one interval after the generated output's start (or its own slot, when that is later).
    const auto plannedReal = [](const Outcome& outcome, const std::chrono::nanoseconds interval) {
        return std::max(outcome.first, outcome.done.calls[0].started) + interval;
    };

    const auto leadOn = leadBatch(true, 1ms, 10ms);
    const auto& generated = leadOn.done.calls[0];
    const auto& leadReal = leadOn.done.calls[1];
    expect(generated.offset == 0ns, "the generated output took an offset from the real present lead");
    expect(leadReal.offset == -1ms && leadReal.scheduled == plannedReal(leadOn, 10ms) - 1ms,
        "the real output was not presented 1 ms earlier with the lead on");

    const auto leadOff = leadBatch(false, 1ms, 10ms);
    expect(leadOff.done.calls[1].offset == 0ns && leadOff.done.calls[1].scheduled == plannedReal(leadOff, 10ms),
        "the real output moved with the lead off");

    // A 3 ms interval and a 2.5 ms lead: the lead would reach 0.5 ms after the generated start, under the quarter-interval
    // floor of 0.75 ms, so the real stops at that floor.
    const auto floorBatch = leadBatch(true, 2500us, 3ms);
    const auto& floorGenerated = floorBatch.done.calls[0];
    const auto& floorReal = floorBatch.done.calls[1];
    expect(floorReal.scheduled == floorGenerated.unmovedStart() + 750us && floorReal.offset == -2250us,
        "the real output went before the generated output's start plus a quarter interval");
}

// RF1: while the real present lead is in effect, the marker correction gets no samples and holds at zero. The lead and
// the marker correction would integrate the same error. Screen samples keep driving the correction with or without the
// lead, and then the lead's markers do not change its offsets.
namespace {
std::vector<PresentMarkerSample> holdMarkers;
size_t holdMarkerRead(void*, uint64_t* const cursor, PresentMarkerSample* const out, const size_t max) noexcept {
    size_t count = 0;
    while (*cursor < holdMarkers.size() && count < max) out[count++] = holdMarkers[(*cursor)++];
    return count;
}
PresentMarkerSample holdMarker(const uint64_t source, const bool generated, const std::chrono::nanoseconds at) {
    return PresentMarkerSample{.context = 0, .source = source, .gpuNs = at.count(), .generated = generated};
}
struct HoldOutcome {
    std::array<std::chrono::nanoseconds, OutputOffsetController::maxPositions> offsets{};
    uint64_t accepted{};
    std::vector<std::chrono::nanoseconds> generatedOffsets;
};
// Six batches (sources 1 to 6) of one generated and one real output. On the GPU clock the reals are 20 ms apart and each
// generated marker is 12 ms after the previous real, 2 ms after the ideal midpoint. The same times are the screen samples.
HoldOutcome holdOutcome(const bool lead, const bool screen) {
    holdMarkers.clear();
    auto ring = std::make_shared<ScreenSampleRing>();
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    queue->marker = std::make_shared<int>(0);
    queue->readMarkerSamples = holdMarkerRead;
    DeferredPresent worker(queue, timedPresent);
    if (screen) worker.setScreenSamples(ring);
    setRealOffsetAutoConfigured(true);
    setRealPresentLeadConfigured(lead);
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto wait = handle<VkSemaphore>(12);
    const uint32_t image = 0;
    const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = nullptr,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &wait,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &image, .pResults = nullptr};
    HoldOutcome outcome;
    holdMarkers.push_back(holdMarker(0, false, 0ms));
    ring->push(ScreenSample{0, 0, false, 0});
    for (uint64_t source = 1; source <= 6; ++source) {
        const auto previous = 20ms * static_cast<int64_t>(source - 1);
        const auto generatedAt = previous + 12ms;
        const auto realAt = previous + 20ms;
        holdMarkers.push_back(holdMarker(source, true, generatedAt));
        holdMarkers.push_back(holdMarker(source, false, realAt));
        ring->push(ScreenSample{0, source, true, std::chrono::nanoseconds{generatedAt}.count()});
        ring->push(ScreenSample{0, source, false, std::chrono::nanoseconds{realAt}.count()});
        const auto at = DeferredPresent::Clock::now() + 2ms;
        expect(worker.beginBatch(2, {.outputInterval = 10ms, .source = source}), "a marker-hold batch was rejected");
        expect(worker.enqueue(info, at, true) && worker.enqueue(info, at + 10ms),
            "a marker-hold batch output was rejected");
        worker.closeBatch();
        const auto done = worker.finish(UINT64_MAX);
        expect(done && done->count == 2 && done->result == VK_SUCCESS, "a marker-hold batch failed");
        outcome.generatedOffsets.push_back(done->calls[0].offset);
    }
    setRealPresentLeadConfigured(false);
    setRealOffsetAutoConfigured(false);
    outcome.offsets = DeferredPresentTestAccess::offsetController(worker).offset;
    outcome.accepted = DeferredPresentTestAccess::acceptedOffsetSamples(worker);
    return outcome;
}
}

static void realLeadHoldsMarkerCorrection() {
    const auto markersOff = holdOutcome(false, false);
    expect(markersOff.accepted > 0 && markersOff.offsets[0] != 0ns,
        "the marker correction did not move without the lead, so the fixture does not drive it");
    const auto markersOn = holdOutcome(true, false);
    expect(markersOn.accepted == 0, "the marker correction took samples with the real present lead on");
    for (const auto offset : markersOn.offsets)
        expect(offset == 0ns, "the marker correction's offset was not held at zero with the real present lead on");
    for (const auto offset : markersOn.generatedOffsets)
        expect(offset == 0ns, "a generated output took a marker offset with the real present lead on");

    const auto screenOff = holdOutcome(false, true);
    const auto screenOn = holdOutcome(true, true);
    expect(screenOff.accepted > 0 && screenOff.offsets[0] != 0ns,
        "the screen correction did not move in the fixture");
    expect(screenOn.accepted == screenOff.accepted && screenOn.offsets == screenOff.offsets,
        "the screen-mode correction changed with the real present lead on");
}

// FI2: a real copied at its present call on the game queue. The copy is made at admission, so the worker never calls
// tryAcquire or submit for it. It is presented at its deadline with the call's lower index and wait, and it is call-held
// from admission until its present returns.
void internalRealCallPreparedAtCall() {
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
    fake->callLower = 5;
    fake->callWait = handle<VkSemaphore>(96);
    expect(worker.beginBatch(1), "call-prepared batch rejected");
    InlineCallOutcome outcome{};
    expect(worker.enqueueInternalReal(info, DeferredPresent::Clock::now() + 5ms, internalCallCopy(fake, 1, 4),
            {}, 0, &outcome), "call-prepared internal real rejected");
    expect(outcome == InlineCallOutcome::Call && fake->callCalls == 1, "the copy was not made at the present call");
    expect(fake->callQueue == handle<VkQueue>(9) && fake->callContinuation == continuation,
        "the call copy used the wrong queue or continuation");
    expect(worker.callHeldOutputs() == 1, "a call-prepared real was not counted as call-held before its present");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 1 && completion->result == VK_SUCCESS &&
        !completion->calls[0].skipped, "call-prepared real was skipped or failed");
    expect(completion->calls[0].callPrepared && completion->calls[0].internalPrepared &&
        !completion->calls[0].workerCopy && completion->calls[0].imageIndex == 5 &&
        completion->calls[0].presentWait == handle<VkSemaphore>(96),
        "call-prepared real did not record its lower index, wait and call copy");
    expect(fake->tryCalls == 0 && fake->submits == 0 && fake->abandons == 0,
        "call-prepared real called the worker's acquire, submit or abandon");
    expect(copyPresents.size() == 1 && copyPresents[0] == std::pair{5u, handle<VkSemaphore>(96)},
        "call-prepared real was not presented exactly once with its lower index and wait");
    expect(worker.callHeldOutputs() == 0, "a presented call-prepared real stayed call-held");
}

// FI2: a call-prepared real in flight when the worker is stopped is still presented, as an F9b prepared real is. The stop
// follows a 50 ms wait, so the worker has taken the job and is inside its 300 ms deadline wait when stopping is set.
void internalRealCallPreparedWhileStopping() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    copyPresents.clear();
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto continuation = handle<VkSemaphore>(12);
    uint32_t applicationImage = 1;
    const VkPresentInfoKHR info{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &continuation,
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &applicationImage};
    auto fake = std::make_shared<InternalFake>();
    fake->callLower = 5;
    fake->callWait = handle<VkSemaphore>(96);
    DeferredPresent worker(queue, recordingPresent);
    expect(worker.beginBatch(1), "stopping call-prepared batch rejected");
    InlineCallOutcome outcome{};
    expect(worker.enqueueInternalReal(info, DeferredPresent::Clock::now() + 300ms, internalCallCopy(fake, 1, 4),
            {}, 0, &outcome) && outcome == InlineCallOutcome::Call, "stopping call-prepared internal real rejected");
    worker.closeBatch();
    std::this_thread::sleep_for(50ms);
    DeferredPresentTestAccess::requestStop(worker);
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 1 && completion->result == VK_SUCCESS &&
        !completion->calls[0].skipped, "a call-prepared real was abandoned while stopping");
    expect(completion->calls[0].callPrepared && fake->tryCalls == 0 && fake->submits == 0 && fake->abandons == 0,
        "a call-prepared real while stopping called the worker's acquire, submit or abandon");
    expect(copyPresents.size() == 1 && copyPresents[0] == std::pair{5u, handle<VkSemaphore>(96)},
        "a call-prepared real while stopping was not presented with its lower index and wait");
}

// FI2: a refused call copy (no free lower image) leaves the application image for the worker, exactly as today. A zero
// call budget never asks for the copy.
void internalRealCallRefusedFallsBack() {
    for (const bool zeroBudget : {false, true}) {
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
        fake->callResult = VK_NOT_READY;
        fake->lowerIndex = 6;
        fake->presentWait = handle<VkSemaphore>(95);
        expect(worker.beginBatch(1), "refused call-copy batch rejected");
        InlineCallOutcome outcome = InlineCallOutcome::Call;
        expect(worker.enqueueInternalReal(info, DeferredPresent::Clock::now() + 5ms,
                internalCallCopy(fake, 1, zeroBudget ? 0 : 4), {}, 0, &outcome),
            "refused call copy rejected the internal real");
        // FP1: a zero budget refuses before the copy (budget); a not-ready lower image is an acquire fallback.
        expect(!inlineCallServed(outcome) && outcome == (zeroBudget ? InlineCallOutcome::Budget : InlineCallOutcome::Acquire),
            "a refused call copy was not classified by its reason");
        expect(worker.callHeldOutputs() == 0, "a refused call copy was counted as call-held");
        expect(fake->callCalls == (zeroBudget ? 0 : 1),
            "a zero call budget asked for the copy, or a budget that allows it did not");
        worker.closeBatch();
        const auto completion = worker.finish(UINT64_MAX);
        expect(completion && completion->count == 1 && completion->result == VK_SUCCESS &&
            !completion->calls[0].skipped, "a refused call copy was skipped");
        expect(!completion->calls[0].callPrepared && completion->calls[0].workerCopy &&
            completion->calls[0].imageIndex == 6, "a refused call copy did not fall back to the worker copy");
        expect(fake->tryCalls == 1 && fake->submits == 1 && fake->abandons == 0,
            "the worker did not acquire and submit once after a refused call copy");
        expect(copyPresents.size() == 1 && copyPresents[0] == std::pair{6u, handle<VkSemaphore>(95)},
            "a refused call copy was not presented once by the worker with its own lower image");
    }
}

// FI2: a call copy whose submit fails has consumed the application image. The job settles as an error skip: nothing is
// presented, and the image is neither acquired nor abandoned again.
void internalRealCallSubmitErrorSettles() {
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
    fake->callResult = VK_ERROR_DEVICE_LOST;
    expect(worker.beginBatch(1), "failed call-copy batch rejected");
    InlineCallOutcome outcome{};
    expect(worker.enqueueInternalReal(info, DeferredPresent::Clock::now() + 5ms, internalCallCopy(fake, 1, 4),
            {}, 0, &outcome), "failed call copy rejected the internal real");
    expect(outcome == InlineCallOutcome::Other && fake->callCalls == 1,
        "the consumed call copy was not admitted as prepared, or its error was not classified as other");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 1 && completion->calls[0].skipped &&
        completion->calls[0].skipReason == DeferredPresent::SkipReason::Error,
        "a failed call copy did not settle as an error skip");
    expect(fake->tryCalls == 0 && fake->submits == 0 && fake->abandons == 0,
        "a failed call copy was acquired, submitted or abandoned again");
    expect(copyPresents.empty(), "a failed call copy was presented");
    expect(worker.callHeldOutputs() == 0, "a settled failed call copy stayed call-held");
}

// FI2 rule 2: a call budget of one holds one real at the call. A second real in the same batch finds no room, so it is
// not copied at the call and takes the worker's copy, which does not count toward the call budget.
void internalRealCallBudgetLimitsSecondReal() {
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
    fake->callLower = 5;
    fake->callWait = handle<VkSemaphore>(96);
    fake->lowerIndex = 6;
    fake->presentWait = handle<VkSemaphore>(95);
    expect(worker.beginBatch(2), "two-output call batch rejected");
    InlineCallOutcome first{}, second{};
    expect(worker.enqueueInternalReal(info, DeferredPresent::Clock::now() + 2ms, internalCallCopy(fake, 1, 1),
            {}, 0, &first), "first call real rejected");
    expect(worker.enqueueInternalReal(info, DeferredPresent::Clock::now() + 4ms, internalCallCopy(fake, 2, 1),
            {}, 0, &second), "second call real rejected");
    expect(first == InlineCallOutcome::Call && second == InlineCallOutcome::Budget && fake->callCalls == 1,
        "a call budget of one did not hold exactly one call copy, or the second real was not budget-refused");
    expect(worker.callHeldOutputs() == 1, "the held call copy was not the only call-held output");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 2 && completion->result == VK_SUCCESS &&
        !completion->calls[0].skipped && !completion->calls[1].skipped, "a budget-limited batch was skipped or failed");
    expect(completion->calls[0].callPrepared && completion->calls[0].imageIndex == 5,
        "the first real was not presented from its call copy");
    expect(!completion->calls[1].callPrepared && completion->calls[1].workerCopy &&
        completion->calls[1].imageIndex == 6, "the second real did not take the worker copy");
    expect(fake->tryCalls == 1 && fake->submits == 1 && fake->abandons == 0,
        "the second real did not acquire and submit once on the worker");
    expect(copyPresents.size() == 2 && copyPresents[0] == std::pair{5u, handle<VkSemaphore>(96)} &&
        copyPresents[1] == std::pair{6u, handle<VkSemaphore>(95)},
        "the budget-limited batch was not presented once per real with its own lower image");
}

// FP2 rule 3: a parked image serves the real at the call without queue->mutex. The test holds the mutex across the
// admission, so a try-lock would have failed. The parked image is presented with its own index and wait.
void internalRealParkedTakeIsLockFree() {
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
    fake->parkedReported = fake->parkedAvailable = 1;
    fake->parkedLower = 4;
    fake->parkedWait = handle<VkSemaphore>(97);
    expect(worker.beginBatch(1), "parked batch rejected");
    InlineCallOutcome outcome{};
    {
        const std::lock_guard held(queue->mutex);
        expect(worker.enqueueInternalReal(info, DeferredPresent::Clock::now() + 5ms,
                internalParkedCallCopy(fake, 1, 4), {}, 0, &outcome), "parked real rejected");
    }
    expect(outcome == InlineCallOutcome::Parked && fake->parkedCalls == 1 && fake->callCalls == 0,
        "a parked real was not served from the parked image without the queue lock");
    expect(fake->parkedReported == 0, "the parked image was not consumed by the call");
    expect(worker.callHeldOutputs() == 1, "a parked real was not call-held from admission");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 1 && completion->result == VK_SUCCESS &&
        !completion->calls[0].skipped && completion->calls[0].callPrepared &&
        completion->calls[0].imageIndex == 4, "a parked real did not present from its parked image");
    expect(fake->tryCalls == 0 && fake->submits == 0, "a parked real called the worker's acquire or submit");
    expect(copyPresents.size() == 1 && copyPresents[0] == std::pair{4u, handle<VkSemaphore>(97)},
        "a parked real was not presented once with its parked index and wait");
}

// FP1: with no parked image and the queue held by the worker, the call falls back as lock, and never calls the copy.
void internalRealCallLockMissFallsBack() {
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
    fake->lowerIndex = 6;
    fake->presentWait = handle<VkSemaphore>(95);
    expect(worker.beginBatch(1), "lock-miss batch rejected");
    InlineCallOutcome outcome{};
    {
        const std::lock_guard held(queue->mutex);
        expect(worker.enqueueInternalReal(info, DeferredPresent::Clock::now() + 5ms,
                internalParkedCallCopy(fake, 1, 4), {}, 0, &outcome), "lock-miss real rejected");
    }
    expect(outcome == InlineCallOutcome::Lock && fake->callCalls == 0 && fake->parkedCalls == 0,
        "a call with no parked image and a busy queue was not classified as lock");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 1 && !completion->calls[0].skipped &&
        !completion->calls[0].callPrepared && completion->calls[0].workerCopy,
        "a lock-miss real did not fall back to the worker copy");
    expect(copyPresents.size() == 1 && copyPresents[0] == std::pair{6u, handle<VkSemaphore>(95)},
        "a lock-miss real was not presented once by the worker");
}

// FP2 rule 2, the mutation check: parked images count toward the budget. One parked image fills a budget of one, so a
// fresh copy at the call is refused as budget and never asked for. Dropping the parked count admits it.
void internalRealParkedCountsTowardBudget() {
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
    fake->parkedReported = 1;   // one parked image, none available to the take
    fake->parkedAvailable = 0;
    fake->lowerIndex = 6;
    fake->presentWait = handle<VkSemaphore>(95);
    expect(worker.beginBatch(1), "budget batch rejected");
    InlineCallOutcome outcome{};
    expect(worker.enqueueInternalReal(info, DeferredPresent::Clock::now() + 5ms,
            internalParkedCallCopy(fake, 1, 1), {}, 0, &outcome), "budget real rejected");
    expect(outcome == InlineCallOutcome::Budget && fake->callCalls == 0,
        "a parked image did not count toward the call budget");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 1 && !completion->calls[0].skipped && completion->calls[0].workerCopy,
        "a budget-refused real did not fall back to the worker copy");
}

// FP2 rule 2: the park hook runs after a lower present with queue->mutex held, and reports the outputs call-held after
// that present. The output just presented is not counted: a call-prepared one leaves the count.
void parkHookReceivesHeldOutputs() {
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
    fake->callLower = 5;
    fake->callWait = handle<VkSemaphore>(96);
    worker.setParkHook(fake, &recordPark);
    expect(worker.beginBatch(1), "park hook batch rejected");
    InlineCallOutcome outcome{};
    expect(worker.enqueueInternalReal(info, DeferredPresent::Clock::now() + 5ms, internalCallCopy(fake, 1, 4),
            {}, 0, &outcome) && outcome == InlineCallOutcome::Call, "park hook call real rejected");
    worker.closeBatch();
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 1 && !completion->calls[0].skipped, "park hook real was skipped");
    expect(fake->parks == 1 && fake->parkHeld == 0,
        "the park hook did not run once after the call-prepared present, with that output excluded");
}

// FP3: a park hook with scripted outcomes. The outcomes are set before the hook is installed, and the last one repeats.
struct ParkScript {
    std::vector<ParkOutcome> outcomes;
    std::atomic<size_t> calls{};
};
ParkOutcome scriptedPark(void* owner, size_t) {
    auto& script = *static_cast<ParkScript*>(owner);
    const size_t call = script.calls.fetch_add(1);
    return script.outcomes[std::min(call, script.outcomes.size() - 1)];
}
// FP3: polls for at most two seconds, so each test waits on the worker's own steps and never on a fixed sleep.
template<class Condition>
bool awaitCondition(const Condition& condition) {
    const auto until = DeferredPresent::Clock::now() + 2s;
    while (!condition()) {
        if (DeferredPresent::Clock::now() >= until) return false;
        std::this_thread::sleep_for(100us);
    }
    return true;
}
void addParkCounts(ParkCounts& total, const ParkCounts& next) {
    for (size_t i = 0; i < next.outcomes.size(); ++i) total.outcomes[i] += next.outcomes[i];
}
// FP3: the present an admission carries. The pointers must outlive the admission.
VkPresentInfoKHR parkTestInfo(const VkSwapchainKHR* swapchain, const VkSemaphore* continuation, const uint32_t* image) {
    return VkPresentInfoKHR{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = nullptr,
        .waitSemaphoreCount = 1, .pWaitSemaphores = continuation,
        .swapchainCount = 1, .pSwapchains = swapchain, .pImageIndices = image, .pResults = nullptr};
}

// FP3: each park outcome is counted in its own field, and Drained is never counted. Each present makes one park attempt.
void parkCountersKeepEachOutcome() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    copyPresents.clear();
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto continuation = handle<VkSemaphore>(12);
    auto fake = std::make_shared<InternalFake>();
    auto script = std::make_shared<ParkScript>();
    script->outcomes = {ParkOutcome::Parked, ParkOutcome::Full, ParkOutcome::Budget, ParkOutcome::Error,
        ParkOutcome::Drained};
    worker.setParkHook(script, &scriptedPark);
    ParkCounts total{};
    for (uint32_t image = 1; image <= script->outcomes.size(); ++image) {
        const VkPresentInfoKHR info = parkTestInfo(&swapchain, &continuation, &image);
        expect(worker.beginBatch(1), "park counter batch rejected");
        expect(worker.enqueueInternalReal(info, DeferredPresent::Clock::now(), internalCopy(fake, 1)),
            "park counter real rejected");
        worker.closeBatch();
        const auto completion = worker.finish(UINT64_MAX);
        expect(completion && completion->count == 1 && !completion->calls[0].skipped, "park counter real was skipped");
        addParkCounts(total, worker.takeParkCounts());
    }
    expect(script->calls.load() == 5, "a present did not make exactly one park attempt");
    expect(total.outcomes[static_cast<size_t>(ParkOutcome::Parked)] == 1 &&
        total.outcomes[static_cast<size_t>(ParkOutcome::Full)] == 1 &&
        total.outcomes[static_cast<size_t>(ParkOutcome::Budget)] == 1 &&
        total.outcomes[static_cast<size_t>(ParkOutcome::Error)] == 1 &&
        total.outcomes[static_cast<size_t>(ParkOutcome::SlotBusy)] == 0 &&
        total.outcomes[static_cast<size_t>(ParkOutcome::Fence)] == 0 &&
        total.outcomes[static_cast<size_t>(ParkOutcome::Acquire)] == 0,
        "a park outcome was counted in the wrong field");
    size_t counted = 0;
    for (const size_t value : total.outcomes) counted += value;
    expect(counted == 4, "Drained was counted, or a park outcome was lost");
    const auto again = worker.takeParkCounts();
    expect(std::all_of(again.outcomes.begin(), again.outcomes.end(),
        [](size_t value) { return value == 0; }), "a take did not reset the park counts");
}

// FP4: the worker's present of a lower image blocks until the test opens the gate, while it holds queue->mutex. A call that
// starts its wait then finds the queue locked and nothing parked, and the worker's park is still to come.
std::atomic<bool> presentGate{};
std::atomic<bool> presentEntered{};
VkResult VKAPI_CALL gatedPresent(VkQueue queue, const VkPresentInfoKHR* info) {
    presentEntered.store(true);
    while (!presentGate.load()) std::this_thread::yield();
    return recordingPresent(queue, info);
}
// FP4: the park hook of the gated test. It counts the images it parks.
struct ParkedImages {
    std::atomic<size_t> parked{};
};
ParkOutcome parkOneImage(void* owner, size_t) {
    static_cast<ParkedImages*>(owner)->parked.fetch_add(1);
    return ParkOutcome::Parked;
}
// FP4: holds queue->mutex on its own thread until release is set. A test thread cannot hold the lock itself, because the wait
// try-locks it, and a try-lock by the owning thread is undefined. The destructor releases and joins.
struct QueueHolder {
    std::shared_ptr<DeferredPresentQueue> queue;
    std::atomic<bool> locked{};
    std::atomic<bool> release{};
    std::thread thread;
    explicit QueueHolder(std::shared_ptr<DeferredPresentQueue> owner) : queue(std::move(owner)), thread([this] {
        const std::lock_guard lock(queue->mutex);
        locked.store(true);
        while (!release.load()) std::this_thread::yield();
    }) {}
    QueueHolder(const QueueHolder&) = delete;
    QueueHolder& operator=(const QueueHolder&) = delete;
    ~QueueHolder() {
        release.store(true);
        thread.join();
    }
};

// FP4: a free queue lock and a parked image both return None at once, and neither is counted as a wait.
void awaitCallLowerSkipsTheWait() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    std::atomic<size_t> parked{1};
    expect(worker.awaitCallLower([&] { return parked.load(); }) == CallLowerWait::None,
        "a parked image was waited for");
    parked.store(0);
    expect(worker.awaitCallLower([&] { return parked.load(); }) == CallLowerWait::None,
        "a free queue lock was waited for");
    const auto counts = worker.takeCallWaitCounts();
    expect(counts.waits == 0 && counts.parked == 0 && counts.unlocked == 0 && counts.timeout == 0 && counts.waitUs == 0,
        "a wait that returned None was counted");
}

// FP4: an image parked during the wait ends it at once as Parked. The park happens on the fourth poll, while another thread
// still holds queue->mutex, so the wait returns Parked before the lock is free. A take resets the counts.
void awaitCallLowerReturnsParkedWhileLocked() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    std::atomic<size_t> parked{};
    QueueHolder holder(queue);
    expect(awaitCondition([&] { return holder.locked.load(); }), "the holder did not take queue->mutex");
    size_t polls = 0;
    const auto result = worker.awaitCallLower([&] {
        if (++polls == 4) parked.store(1);
        return parked.load();
    });
    expect(result == CallLowerWait::Parked, "an image parked during the wait did not end it as Parked");
    const auto counts = worker.takeCallWaitCounts();
    expect(counts.waits == 1 && counts.parked == 1 && counts.unlocked == 0 && counts.timeout == 0,
        "a parked wait was not counted once as parked");
    expect(worker.takeCallWaitCounts().waits == 0, "a take did not reset the call waits");
}

// FP4: the queue lock freed during the wait, with nothing parked, ends it as Unlocked. The holder releases on the fourth poll,
// a few microseconds into the wait, so the free lock is seen well inside the 500 us limit.
void awaitCallLowerReturnsUnlockedWhenTheLockFrees() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    QueueHolder holder(queue);
    expect(awaitCondition([&] { return holder.locked.load(); }), "the holder did not take queue->mutex");
    size_t polls = 0;
    const auto result = worker.awaitCallLower([&] {
        if (++polls == 4) holder.release.store(true);
        return size_t{0};
    });
    expect(result == CallLowerWait::Unlocked, "a freed queue lock with nothing parked did not end the wait as Unlocked");
    const auto counts = worker.takeCallWaitCounts();
    expect(counts.waits == 1 && counts.unlocked == 1 && counts.parked == 0 && counts.timeout == 0,
        "an unlocked wait was not counted once as unlocked");
}

// FP4: a lock that stays held for the whole wait ends it as Timeout, after the 500 us limit and well before the loose
// 50 ms bound that leaves room for sanitizers. The holder releases only after the call returns, so the lock cannot free
// inside the wait.
void awaitCallLowerTimesOutWhileLocked() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, recordingPresent);
    QueueHolder holder(queue);
    expect(awaitCondition([&] { return holder.locked.load(); }), "the holder did not take queue->mutex");
    const auto started = DeferredPresent::Clock::now();
    const auto result = worker.awaitCallLower([] { return size_t{0}; });
    const auto elapsed = DeferredPresent::Clock::now() - started;
    expect(result == CallLowerWait::Timeout, "a lock held for the whole wait did not end it as Timeout");
    expect(elapsed >= inlineCallLowerWait && elapsed < 50ms, "the wait did not last the 500 us limit");
    const auto counts = worker.takeCallWaitCounts();
    expect(counts.waits == 1 && counts.timeout == 1 && counts.parked == 0 && counts.unlocked == 0 &&
        counts.waitUs >= 500, "a timed-out wait was not counted once as a timeout");
}

// FP4, the lock order. The wait holds no DeferredPresent mutex, so a worker present that parks an image while the call waits
// completes, and the call sees the park as Parked. The gate opens on the caller's fourth poll, so the park runs inside the
// wait. Mutation check: taking the DeferredPresent mutex inside the wait stops the park until the wait times out, and this
// test then reports Timeout instead of Parked, without hanging, because the wait is bounded by inlineCallLowerWait.
void awaitCallLowerSeesWorkerParkDuringWait() {
    auto queue = std::make_shared<DeferredPresentQueue>();
    queue->queue = handle<VkQueue>(7);
    DeferredPresent worker(queue, gatedPresent);
    copyPresents.clear();
    presentGate.store(false);
    presentEntered.store(false);
    auto parkedImages = std::make_shared<ParkedImages>();
    worker.setParkHook(parkedImages, &parkOneImage);
    const auto swapchain = handle<VkSwapchainKHR>(11);
    const auto continuation = handle<VkSemaphore>(12);
    uint32_t image = 1;
    const VkPresentInfoKHR info = parkTestInfo(&swapchain, &continuation, &image);
    auto fake = std::make_shared<InternalFake>();
    expect(worker.beginBatch(1), "wait batch rejected");
    expect(worker.enqueueInternalReal(info, DeferredPresent::Clock::now(), internalCopy(fake, 1)),
        "wait real rejected");
    worker.closeBatch();
    expect(awaitCondition([&] { return presentEntered.load(); }), "the worker's present did not start");
    size_t polls = 0;
    const auto result = worker.awaitCallLower([&] {
        if (++polls == 4) presentGate.store(true);
        return parkedImages->parked.load();
    });
    expect(result == CallLowerWait::Parked,
        "the call did not see the worker's park during its wait (a DeferredPresent mutex held across the wait stops it)");
    const auto completion = worker.finish(UINT64_MAX);
    expect(completion && completion->count == 1 && !completion->calls[0].skipped, "the wait real was skipped");
    expect(parkedImages->parked.load() == 1, "the worker did not park exactly one image");
    expect(copyPresents.size() == 1, "the wait real was not presented exactly once");
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
    generatedPreparedOrderAndBudget(); generatedCopyCanPrecedeHostReadiness(); generatedPreparationFallbackAndGates();
    generatedChainOutputOffsetsMoveEachOutput();
    generatedPreparedRetirement(); generatedPreparedStop(); generatedPreparationErrors();
    queueRequest();
    privateSubmitWithoutTailIsUnchanged(); privateSubmitAppendsTail();
    privateSubmitFailureLeavesTail(); privateSubmitForeignChainBypassesTail();
    privateSubmitRecordsHistory(); privateSubmitHistoryIgnoresFailure();
    privateSubmitWithoutHistoryRecordsNothing(); skipReasonNamesFollowEnumOrder();
    lifecycle(); boundedBatch(); completePresentationOrder(); readinessDoesNotBlockProducer();
    waitForRetainedId(); observeCopiesWithoutChangingDelivery(); observerLifetimeAndForwarding();
    releasedBatchBoundsAndExpedites(); overlappingBatchRetainsOlder(); overlappingBatchAdmitsAfterWorkerRetires();
    nonOverlappableDoesNotRetain();
    overlapAcquireDoesNotDrainFutureOutputs();
    overlapRingOfThree(); overlapAcceptsPresentId(); overlapAcceptsOwnedPresentId(); crossBatchChaining();
    twoIdentifiedBatchesJoinTheirOwnRecords(); threeIdentifiedBatchesJoinInAnyOrder();
    timingRequestIsCopiedAndRebuilt(); sixKnownNodesAreAccepted();
    overlappedChainShiftsLaterOutputs(); expeditedOutputAnchorsNextBatch();
    justInTimeBatchStartsAtItsReadiness(); justInTimeBatchKeepsQuarterIntervalAfterPreviousReal();
    justInTimeBatchHoldOffStartsAtReadiness();
    chainedBatchHoldStartsAfterReadiness(); chainedBatchHoldOffStartsAtReadiness(); chainedBatchHoldKeepsItsSchedule();
    offsetAppliesAfterReadinessStart(); chainedPairOffsetsFromItsUnmovedStart();
    nonOverlapBatchOffsetMovesGeneratedOnly(); nonOverlapPendingRealSurvivesBatches(); pacingCorrectionReloadsOnAnExistingWorker();
    plainGeneratedPresentOffsetMoves(); plainGeneratedPresentCutLimitsOffset();
    generatedCopyPresentsAcquiredImage(); generatedCopySkipsWithoutImage(); generatedCopySkipsOutOfRangeImage();
    generatedCopyNeedsOpenBatch(); generatedCopyChainWaitsPreceding(); generatedCopyChainSkipWaitsPreceding();
    generatedCopyAddsPrivateTail(); generatedSkipAddsPrivateTail();
    generatedCopyUnsafeSpacingModes(); generatedCopyTailSurvivesAJoin();
    lateGeneratedCopyPresentsBeforeLimit(); lateGeneratedCopyPastQuarterSkips();
    generatedCopyTailKeepsSpacingWithoutLimit(false);
    generatedCopyTailKeepsSpacingWithoutLimit(true);
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
    internalRealCallPreparedAtCall(); internalRealCallPreparedWhileStopping(); internalRealCallRefusedFallsBack();
    internalRealCallSubmitErrorSettles(); internalRealCallBudgetLimitsSecondReal();
    internalRealParkedTakeIsLockFree(); internalRealCallLockMissFallsBack();
    internalRealParkedCountsTowardBudget(); parkHookReceivesHeldOutputs();
    parkCountersKeepEachOutcome();
    awaitCallLowerSkipsTheWait(); awaitCallLowerReturnsParkedWhileLocked(); awaitCallLowerReturnsUnlockedWhenTheLockFrees();
    awaitCallLowerTimesOutWhileLocked(); awaitCallLowerSeesWorkerParkDuringWait();
    lowerRecordsFollowHistory(); inFlightCallFollowsHistory(); synchronousPresentIsNoted();
    presentsAreNotedWithHistory();
    // X3 last: presentedJobsKeepTheirFenceForTheLowerPresent uses the shared `present` fake, which
    // counts calls for the earlier lifecycle test.
    skippedInternalRealSignalsItsFenceOnce(); stoppedInternalRealSignalsItsFenceOnce();
    presentedJobsKeepTheirFenceForTheLowerPresent(); skippedGeneratedCopySignalsItsFenceOnce();
    reflexMarkersEncloseEachWorkerPresent();
    gamescopeTickLockRemovesSameTickPairs(); nonGamescopeGatesScheduleAsToday(); zeroSnapIsIdentical();
    realPresentLeadMovesRealEarlier();
    realLeadHoldsMarkerCorrection();
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/deferred_present.hpp"
#include "swapchain/private_present_queue.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
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
        } else expect(false, "unknown metadata reached worker");
        const void* next{};
        std::memcpy(&next, static_cast<const std::byte*>(chain) +
            offsetof(VkBaseInStructure, pNext), sizeof(next));
        chain = next;
    }
    ++calls;
    --active;
    return lowerResult;
}
void queueRequest() {
    std::array families{VkQueueFamilyProperties{.queueCount = 4}};
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
        const VkPresentInfoKHR& info, uint64_t context, uint64_t source, bool generated) {
    auto& state = *static_cast<ObserverState*>(owner);
    expect(context == 123 && source == 456 && generated == (state.calls == 0),
        "retained observer lost the admitted context/source/output role");
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
    expect(worker.beginBatch(1, {.expediteOnJoin = true, .overlap = true}), "second ring batch rejected");
    expect(!worker.enqueue(identified, due + 1ms), "identified image admitted behind a pending batch");
    image = 1;
    expect(worker.enqueue(info, due + 1ms), "second ring output rejected");
    worker.closeBatch();
    expect(worker.beginBatch(1, {.expediteOnJoin = true, .overlap = true}), "third ring batch rejected");
    expect(!worker.enqueue(identified, due + 2ms), "identified image admitted with two pending batches");
    image = 2;
    expect(worker.enqueue(info, due + 2ms), "third ring output rejected");
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
void overlapRejectsPresentId() {
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
    expect(!worker.enqueue(identified, start + 1ms), "overlapping second batch registered a present ID");
    expect(worker.enqueue(info, start + 1ms), "overlapping second batch rejected an unidentified image");
    worker.closeBatch();
    const auto older = worker.finish(UINT64_MAX);
    expect(older.has_value() && older->count == 1, "overlapping head batch did not complete");
    const auto newer = worker.finish(UINT64_MAX);
    expect(newer.has_value() && newer->count == 1, "overlapping second batch did not complete");
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
    uint64_t foreign = 101;
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
    expect(!worker.enqueue(foreignInfo, start + 1ms), "overlapping second batch accepted a present ID from other storage");
    VkPresentId2KHR ownedIds{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
        .swapchainCount = 1, .pPresentIds = &owned};
    VkPresentInfoKHR ownedInfo = info;
    ownedInfo.pNext = &ownedIds;
    expect(worker.enqueue(ownedInfo, start + 1ms), "overlapping second batch rejected the owned present ID");
    worker.closeBatch();
    const auto older = worker.finish(UINT64_MAX);
    expect(older.has_value() && older->count == 1, "overlapping head batch did not complete");
    const auto newer = worker.finish(UINT64_MAX);
    expect(newer.has_value() && newer->count == 1, "overlapping owned-ID batch did not complete");
    expect(observedId2 == 101, "the lower present did not receive the owned present ID");
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
    std::array<VkSemaphore, 2> waitSemaphores{};
    uint32_t signals{};
    std::array<VkSemaphore, 2> signalSemaphores{};
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
};
VkResult fakeTryAcquire(void* owner, uint32_t* lowerIndex) {
    auto& state = *static_cast<InternalFake*>(owner);
    ++state.tryCalls;
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
    state.submittedApplication = applicationIndex;
    state.submittedLower = lowerIndex;
    state.submittedContinuation = continuation;
    *presentWait = state.presentWait;
    return state.submitResult;
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

int main() {
    queueRequest(); lifecycle(); boundedBatch(); completePresentationOrder(); readinessDoesNotBlockProducer();
    waitForRetainedId(); observeCopiesWithoutChangingDelivery(); observerLifetimeAndForwarding();
    releasedBatchBoundsAndExpedites(); overlappingBatchRetainsOlder(); nonOverlappableDoesNotRetain();
    overlapAcquireDoesNotDrainFutureOutputs();
    overlapRingOfThree(); overlapRejectsPresentId(); overlapAcceptsOwnedPresentId(); crossBatchChaining();
    overlappedChainShiftsLaterOutputs(); expeditedOutputAnchorsNextBatch();
    generatedCopyPresentsAcquiredImage(); generatedCopySkipsWithoutImage(); generatedCopySkipsOutOfRangeImage();
    generatedCopyNeedsOpenBatch();
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
}

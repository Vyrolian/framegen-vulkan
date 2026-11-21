/* SPDX-License-Identifier: GPL-3.0-or-later */
// RT1: the Reflex queue drain's extra-wait policy and the relay's semaphore protocol, with fake driver functions. RL2:
// the out-of-band switch's parse. No Vulkan device.
#include "swapchain/reflex_out_of_band.hpp"
#include "swapchain/reflex_queue_drain.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

using namespace fgvk::layer;
using namespace std::chrono_literals;

namespace {
using Duration = ReflexQueueDrain::Duration;
using Clock = ReflexSleepRelay::Clock;

void expect(const bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

template<class T> T handle(uintptr_t value) { return reinterpret_cast<T>(value); }

// The steady clock as the drain's `now`, offset so the fixtures start well away from zero.
Duration at(const Duration offset) { return offset + 1s; }

// ---- RL2 ----

void outOfBandParse() {
    expect(parseReflexOutOfBandRequest(nullptr) == ReflexOutOfBandType::Present,
        "an unset FGVK_REFLEX_OUT_OF_BAND did not mark the present queue");
    expect(parseReflexOutOfBandRequest("present") == ReflexOutOfBandType::Present, "present was not the present type");
    expect(parseReflexOutOfBandRequest("render") == ReflexOutOfBandType::Render, "render was not the render type");
    expect(parseReflexOutOfBandRequest("off") == ReflexOutOfBandType::None, "off did not mark nothing");
    for (const char* garbage : {"", "OFF", "0", "1", "none", "render "})
        expect(parseReflexOutOfBandRequest(garbage) == ReflexOutOfBandType::Present,
            "an unrecognised FGVK_REFLEX_OUT_OF_BAND value did not fall back to the present type");
}

// ---- The drain's extra wait ----

// Converges to the limit under a steady 4 ms queue and holds there.
void steadyQueueConvergesAtLimit() {
    ReflexQueueDrain drain;
    const std::array<int64_t, 1> queued{4000};
    Duration now = at(0ms);
    for (int step = 0; step < 5; ++step) {
        drain.observe(queued, now);
        now += 10ms;
    }
    expect(drain.extra(now) == ReflexQueueDrain::limit, "a steady 4 ms queue did not converge to the 4 ms limit");
    for (int step = 0; step < 20; ++step) {
        drain.observe(queued, now);
        now += 10ms;
    }
    expect(drain.extra(now) == ReflexQueueDrain::limit, "the extra wait went above the limit");
}

// A median at the 300 us target leaves the extra wait where it is.
void queueAtTargetHolds() {
    ReflexQueueDrain drain;
    Duration now = at(0ms);
    for (int step = 0; step < 3; ++step) {
        drain.observe(std::array<int64_t, 1>{4000}, now);
        now += 10ms;
    }
    const auto held = drain.extra(now);
    expect(held > Duration{}, "the extra wait did not rise under a 4 ms queue");
    for (int step = 0; step < 10; ++step) {
        drain.observe(std::array<int64_t, 1>{300}, now);
        now += 10ms;
    }
    expect(drain.extra(now) == held, "a queue at the target moved the extra wait");
}

// A queue of zero decays the extra wait to zero, a quarter of the target each observation.
void emptyQueueDecaysToZero() {
    ReflexQueueDrain drain;
    Duration now = at(0ms);
    for (int step = 0; step < 5; ++step) {
        drain.observe(std::array<int64_t, 1>{4000}, now);
        now += 10ms;
    }
    expect(drain.extra(now) == ReflexQueueDrain::limit, "the drain did not reach the limit before decaying");
    for (int step = 0; step < 60; ++step) {
        drain.observe(std::array<int64_t, 1>{0}, now);
        now += 10ms;
    }
    expect(drain.extra(now) == Duration{}, "a queue of zero did not decay the extra wait to zero");
}

// An empty group changes nothing; the median uses the samples given, each clamped at zero.
void emptyGroupAndNegativeSamples() {
    ReflexQueueDrain drain;
    drain.observe(std::span<const int64_t>{}, at(0ms));
    expect(drain.extra(at(0ms)) == Duration{}, "an empty group counted as an observation");
    drain.observe(std::array<int64_t, 3>{4000, 4000, 4000}, at(10ms));
    const auto raised = drain.extra(at(10ms));
    drain.observe(std::array<int64_t, 2>{-9000, -1}, at(20ms));
    expect(drain.extra(at(20ms)) < raised, "negative queued times were not clamped to a zero median");
}

// The period bound: the extra wait is at most a third of the output period, and never above the limit.
void periodBoundsTheExtra() {
    ReflexQueueDrain drain;
    drain.setPeriodUs(12000);
    Duration now = at(0ms);
    for (int step = 0; step < 5; ++step) {
        drain.observe(std::array<int64_t, 1>{4000}, now);
        now += 10ms;
    }
    expect(drain.extra(now) == 4000us, "a 12 ms period did not cap the extra wait at 4 ms");

    drain.setPeriodUs(6000);
    expect(drain.extra(now) == 2000us, "a 6 ms period did not cap the extra wait at 2 ms");
    for (int step = 0; step < 5; ++step) {
        drain.observe(std::array<int64_t, 1>{4000}, now);
        now += 10ms;
    }
    expect(drain.extra(now) == 2000us, "observations under a 6 ms period went above 2 ms");

    ReflexQueueDrain unbounded;
    unbounded.setPeriodUs(0);
    for (int step = 0; step < 5; ++step) {
        unbounded.observe(std::array<int64_t, 1>{4000}, now);
        now += 10ms;
    }
    expect(unbounded.extra(now) == ReflexQueueDrain::limit, "a zero period bounded the extra wait below the limit");
}

// An observation older than one second gives no extra wait.
void staleObservationGivesNothing() {
    ReflexQueueDrain drain;
    drain.observe(std::array<int64_t, 1>{4000}, at(0ms));
    drain.observe(std::array<int64_t, 1>{4000}, at(0ms) + 1ms);
    expect(drain.extra(at(0ms) + 1ms + 1s) > Duration{}, "an observation within one second was stale");
    expect(drain.extra(at(0ms) + 1ms + 1s + 1ns) == Duration{}, "an observation older than one second was used");
}

void resetZeroesEverything() {
    ReflexQueueDrain drain;
    drain.setPeriodUs(6000);
    for (int step = 0; step < 5; ++step) drain.observe(std::array<int64_t, 1>{4000}, at(step * 10ms));
    expect(drain.extra(at(50ms)) == 2000us, "the bounded extra wait was not in place before the reset");
    drain.reset();
    expect(drain.extra(at(50ms)) == Duration{}, "reset kept the extra wait");
    drain.observe(std::array<int64_t, 1>{4000}, at(60ms));
    expect(drain.extra(at(60ms)) == 925us, "reset kept the period bound");
}

// ---- The relay with fake driver functions ----

// A fake driver timeline: values per semaphore, the application's signals in order with their times, and call counts.
struct FakeDriver {
    struct Signal {
        VkSemaphore semaphore{};
        uint64_t value{};
        Clock::time_point at{};
    };
    std::mutex mutex;
    std::condition_variable changed;
    std::map<uintptr_t, uint64_t> values;
    std::vector<Signal> signals;
    int creates{};
    int destroys{};
    bool createFails{};
    bool waitFails{};
    uintptr_t nextHandle{0x100};
};
FakeDriver driver;

void resetDriver() {
    const std::lock_guard lock(driver.mutex);
    driver.values.clear();
    driver.signals.clear();
    driver.creates = 0;
    driver.destroys = 0;
    driver.createFails = false;
    driver.waitFails = false;
    driver.nextHandle = 0x100;
}

VkResult VKAPI_CALL fakeCreate(VkDevice, const VkSemaphoreCreateInfo* info, const VkAllocationCallbacks*,
        VkSemaphore* out) {
    const std::lock_guard lock(driver.mutex);
    expect(info && info->pNext, "the private semaphore was created without its type");
    const auto* type = static_cast<const VkSemaphoreTypeCreateInfo*>(info->pNext);
    expect(type->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO &&
        type->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE && type->initialValue == 0,
        "the private semaphore was not a timeline starting at zero");
    if (driver.createFails) return VK_ERROR_OUT_OF_HOST_MEMORY;
    ++driver.creates;
    const auto value = driver.nextHandle++;
    driver.values[value] = 0;
    *out = handle<VkSemaphore>(value);
    return VK_SUCCESS;
}

void VKAPI_CALL fakeDestroy(VkDevice, VkSemaphore semaphore, const VkAllocationCallbacks*) {
    const std::lock_guard lock(driver.mutex);
    ++driver.destroys;
    driver.values.erase(reinterpret_cast<uintptr_t>(semaphore));
}

// One semaphore, as the relay waits on it. Times out after `timeout` nanoseconds.
VkResult VKAPI_CALL fakeWait(VkDevice, const VkSemaphoreWaitInfo* info, uint64_t timeout) {
    std::unique_lock lock(driver.mutex);
    if (driver.waitFails) return VK_ERROR_DEVICE_LOST;
    expect(info && info->semaphoreCount == 1, "the relay waited on more than one semaphore");
    const auto key = reinterpret_cast<uintptr_t>(info->pSemaphores[0]);
    const uint64_t wanted = info->pValues[0];
    const bool reached = driver.changed.wait_for(lock, std::chrono::nanoseconds{timeout},
        [&] { return driver.values[key] >= wanted; });
    return reached ? VK_SUCCESS : VK_TIMEOUT;
}

VkResult VKAPI_CALL fakeCounter(VkDevice, VkSemaphore semaphore, uint64_t* value) {
    const std::lock_guard lock(driver.mutex);
    *value = driver.values[reinterpret_cast<uintptr_t>(semaphore)];
    return VK_SUCCESS;
}

VkResult VKAPI_CALL fakeSignal(VkDevice, const VkSemaphoreSignalInfo* info) {
    const std::lock_guard lock(driver.mutex);
    const auto key = reinterpret_cast<uintptr_t>(info->semaphore);
    if (driver.values[key] >= info->value) return VK_ERROR_UNKNOWN;
    driver.values[key] = info->value;
    driver.signals.push_back({info->semaphore, info->value, Clock::now()});
    driver.changed.notify_all();
    return VK_SUCCESS;
}

// The driver's own sleep completing: the private value is reached.
void driverSignal(const VkSemaphore semaphore, const uint64_t value) {
    const std::lock_guard lock(driver.mutex);
    driver.values[reinterpret_cast<uintptr_t>(semaphore)] = value;
    driver.changed.notify_all();
}

void setAppValue(const VkSemaphore semaphore, const uint64_t value) {
    const std::lock_guard lock(driver.mutex);
    driver.values[reinterpret_cast<uintptr_t>(semaphore)] = value;
}

uint64_t appValue(const VkSemaphore semaphore) {
    const std::lock_guard lock(driver.mutex);
    return driver.values[reinterpret_cast<uintptr_t>(semaphore)];
}

std::size_t signalCount() {
    const std::lock_guard lock(driver.mutex);
    return driver.signals.size();
}

// Waits until the application's semaphore reaches `value`, as the game's own wait would see it. It reads the fake
// timeline directly, so a failed private wait in a test does not affect it.
bool waitForAppValue(const VkSemaphore semaphore, const uint64_t value, const std::chrono::nanoseconds timeout) {
    std::unique_lock lock(driver.mutex);
    const auto key = reinterpret_cast<uintptr_t>(semaphore);
    return driver.changed.wait_for(lock, timeout, [&] { return driver.values[key] >= value; });
}

ReflexSemaphoreFunctions fakeFunctions() {
    return ReflexSemaphoreFunctions{
        .create = fakeCreate, .destroy = fakeDestroy, .wait = fakeWait, .counter = fakeCounter, .signal = fakeSignal,
    };
}

const auto lowerDevice = handle<VkDevice>(0x9100);
const auto applicationSemaphore = handle<VkSemaphore>(0x9200);

VkLatencySleepInfoNV applicationSleep(const uint64_t value) {
    return VkLatencySleepInfoNV{
        .sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV,
        .pNext = nullptr,
        .signalSemaphore = applicationSemaphore,
        .value = value,
    };
}

// The driver's sleep takes the relay's private semaphore: it is not the application's, and its value is the next one.
// The application's value is not signalled until the driver's sleep finishes and the extra wait has passed.
void relayWaitsForPrivateThenExtra() {
    resetDriver();
    setAppValue(applicationSemaphore, 4);
    ReflexSleepRelay relay;
    relay.attach(lowerDevice, fakeFunctions());
    VkLatencySleepInfoNV driverSaw{};
    const auto application = applicationSleep(5);
    const auto result = relay.sleep(application, 20ms, [&](const VkLatencySleepInfoNV& sub) {
        driverSaw = sub;
        return VK_SUCCESS;
    });
    expect(result && *result == VK_SUCCESS, "the substituted sleep was not made");
    expect(driverSaw.signalSemaphore != applicationSemaphore && driverSaw.value == 1,
        "the driver's sleep did not get the private semaphore and its next value");

    std::this_thread::sleep_for(30ms);
    expect(appValue(applicationSemaphore) == 4 && signalCount() == 0,
        "the application value was signalled before the driver's sleep finished");
    const auto driverDone = Clock::now();
    driverSignal(driverSaw.signalSemaphore, driverSaw.value);
    expect(waitForAppValue(applicationSemaphore, 5, 2s), "the application value was never signalled");
    {
        const std::lock_guard lock(driver.mutex);
        expect(driver.signals.size() == 1 && driver.signals[0].value == 5 &&
            driver.signals[0].semaphore == applicationSemaphore, "the application value was not signalled exactly once");
        expect(driver.signals[0].at - driverDone >= 20ms,
            "the application value was signalled before the extra wait after the driver's sleep");
    }
    relay.stop();
    expect(signalCount() == 1, "stop signalled the application value again");
    expect(driver.creates == 1 && driver.destroys == 1, "the private semaphore was not created once and destroyed once");
}

// A counter already at the application's value takes no signal.
void applicationValueAlreadyReachedIsNotSignalled() {
    resetDriver();
    setAppValue(applicationSemaphore, 9);
    ReflexSleepRelay relay;
    relay.attach(lowerDevice, fakeFunctions());
    VkLatencySleepInfoNV driverSaw{};
    const auto result = relay.sleep(applicationSleep(9), 1ms, [&](const VkLatencySleepInfoNV& sub) {
        driverSaw = sub;
        return VK_SUCCESS;
    });
    expect(result && *result == VK_SUCCESS, "the substituted sleep was not made");
    driverSignal(driverSaw.signalSemaphore, driverSaw.value);
    relay.stop();
    expect(signalCount() == 0 && appValue(applicationSemaphore) == 9,
        "a reached application value was signalled again");
}

// Stop flushes every queued job at once: no extra wait, one private wait each, and each application value is signalled.
void stopFlushesEveryPendingJob() {
    resetDriver();
    ReflexSleepRelay relay;
    relay.attach(lowerDevice, fakeFunctions());
    for (uint64_t value = 1; value <= 3; ++value) {
        const auto result = relay.sleep(applicationSleep(value), 10s, [](const VkLatencySleepInfoNV&) {
            return VK_SUCCESS;
        });
        expect(result && *result == VK_SUCCESS, "a queued substitution was refused");
    }
    const auto stopping = Clock::now();
    relay.stop();
    const auto stopped = Clock::now();
    expect(signalCount() == 3, "stop did not signal every queued application value");
    expect(appValue(applicationSemaphore) == 3, "stop left an application value behind");
    // The driver never signalled the private values here, so stop also spends its one-second settle wait.
    expect(stopped - stopping < 3s, "stop waited for the extra wait instead of finishing the queue at once");
}

// A failed private wait signals the application's value at once, without the extra wait.
void waitErrorSignalsImmediately() {
    resetDriver();
    {
        const std::lock_guard lock(driver.mutex);
        driver.waitFails = true;
    }
    ReflexSleepRelay relay;
    relay.attach(lowerDevice, fakeFunctions());
    const auto result = relay.sleep(applicationSleep(2), 5s, [](const VkLatencySleepInfoNV&) {
        return VK_SUCCESS;
    });
    expect(result && *result == VK_SUCCESS, "the substituted sleep was not made");
    expect(waitForAppValue(applicationSemaphore, 2, 1s), "a failed private wait did not signal the application at once");
    relay.stop();
}

// A failed substituted sleep returns its result with no job queued, so the caller forwards the original sleep.
void failedSubstitutionQueuesNothing() {
    resetDriver();
    ReflexSleepRelay relay;
    relay.attach(lowerDevice, fakeFunctions());
    const auto result = relay.sleep(applicationSleep(1), 1ms, [](const VkLatencySleepInfoNV&) {
        return VK_ERROR_DEVICE_LOST;
    });
    expect(result && *result == VK_ERROR_DEVICE_LOST, "a failed substituted sleep did not return its result");
    const auto next = relay.sleep(applicationSleep(1), 1ms, [](const VkLatencySleepInfoNV& sub) {
        expect(sub.value == 1, "a failed substitution used up a private value");
        return VK_SUCCESS;
    });
    expect(next && *next == VK_SUCCESS, "the relay did not substitute after a failed substitution");
    relay.stop();
}

// Without attach, or when the semaphore cannot be created, the relay makes no driver call.
void relayRefusesWithoutAttachOrSemaphore() {
    resetDriver();
    ReflexSleepRelay detached;
    bool called = false;
    expect(!detached.sleep(applicationSleep(1), 1ms, [&](const VkLatencySleepInfoNV&) {
        called = true;
        return VK_SUCCESS;
    }), "an unattached relay substituted a sleep");
    expect(!called, "an unattached relay called the driver");

    ReflexSleepRelay unable;
    {
        const std::lock_guard lock(driver.mutex);
        driver.createFails = true;
    }
    unable.attach(lowerDevice, fakeFunctions());
    expect(!unable.sleep(applicationSleep(1), 1ms, [&](const VkLatencySleepInfoNV&) {
        called = true;
        return VK_SUCCESS;
    }), "a relay without a private semaphore substituted a sleep");
    expect(!called, "a relay without a private semaphore called the driver");
    {
        const std::lock_guard lock(driver.mutex);
        driver.createFails = false;
    }
}

// Device destruction stops the relays of that device only.
void retirementStopsOnlyTheDevice() {
    resetDriver();
    const auto swapchain = handle<VkSwapchainKHR>(0x9300);
    const auto otherSwapchain = handle<VkSwapchainKHR>(0x9301);
    const auto otherDevice = handle<VkDevice>(0x9400);
    const auto relay = ReflexSleepRelayRegistry::get(swapchain);
    relay->attach(lowerDevice, fakeFunctions());
    const auto other = ReflexSleepRelayRegistry::get(otherSwapchain);
    other->attach(otherDevice, fakeFunctions());
    expect(relay->sleep(applicationSleep(1), 1ms, [](const VkLatencySleepInfoNV& sub) {
        driverSignal(sub.signalSemaphore, sub.value);
        return VK_SUCCESS;
    }).has_value(), "the relay for the destroyed device did not substitute");
    expect(other->sleep(applicationSleep(1), 1ms, [](const VkLatencySleepInfoNV&) { return VK_SUCCESS; }).has_value(),
        "the relay for the other device did not substitute");
    retireReflexSleepRelays(lowerDevice);
    expect(ReflexSleepRelayRegistry::find(swapchain) == nullptr, "a relay of the destroyed device stayed registered");
    expect(ReflexSleepRelayRegistry::find(otherSwapchain) != nullptr, "a relay of another device was removed");
    expect(driver.destroys == 1, "retiring the device did not destroy its private semaphore once");
    retireReflexSleepRelay(otherSwapchain);
    expect(ReflexSleepRelayRegistry::find(otherSwapchain) == nullptr, "a retired swapchain's relay stayed registered");
}

}

// busy() covers a job from the queue until its application value is signalled, so the caller keeps later sleeps on
// the relay and the application's timeline never goes backwards.
void busyUntilTheApplicationValueIsSignalled() {
    resetDriver();
    ReflexSleepRelay relay;
    relay.attach(lowerDevice, fakeFunctions());
    expect(!relay.busy(), "a new relay was busy");
    VkSemaphore privateSemaphore{};
    const auto result = relay.sleep(applicationSleep(1), 1ms, [&](const VkLatencySleepInfoNV& sub) {
        privateSemaphore = sub.signalSemaphore;
        return VK_SUCCESS;
    });
    expect(result && *result == VK_SUCCESS, "the substituted sleep was not made");
    expect(relay.busy(), "a queued job did not make the relay busy");
    driverSignal(privateSemaphore, 1);
    expect(waitForAppValue(applicationSemaphore, 1, 1s), "the application value was not signalled");
    const auto deadline = Clock::now() + 1s;
    while (relay.busy() && Clock::now() < deadline) std::this_thread::sleep_for(1ms);
    expect(!relay.busy(), "the relay stayed busy after signalling");
    relay.stop();
    const std::lock_guard lock(driver.mutex);
    expect(driver.destroys == 1, "a settled private semaphore was not destroyed");
}

// A private value the driver never signals keeps the semaphore alive at stop (destroying it would be invalid).
void unsettledPrivateSemaphoreIsNotDestroyed() {
    resetDriver();
    ReflexSleepRelay relay;
    relay.attach(lowerDevice, fakeFunctions());
    const auto result = relay.sleep(applicationSleep(1), 1ms, [](const VkLatencySleepInfoNV&) { return VK_SUCCESS; });
    expect(result && *result == VK_SUCCESS, "the substituted sleep was not made");
    relay.stop();
    const std::lock_guard lock(driver.mutex);
    expect(driver.destroys == 0, "a private semaphore with a pending driver signal was destroyed");
    expect(driver.values[reinterpret_cast<uintptr_t>(applicationSemaphore)] == 1,
        "stop did not signal the application value");
}

int main() {
    outOfBandParse();
    steadyQueueConvergesAtLimit();
    queueAtTargetHolds();
    emptyQueueDecaysToZero();
    emptyGroupAndNegativeSamples();
    periodBoundsTheExtra();
    staleObservationGivesNothing();
    resetZeroesEverything();
    relayWaitsForPrivateThenExtra();
    applicationValueAlreadyReachedIsNotSignalled();
    stopFlushesEveryPendingJob();
    waitErrorSignalsImmediately();
    failedSubstitutionQueuesNothing();
    relayRefusesWithoutAttachOrSemaphore();
    retirementStopsOnlyTheDevice();
    busyUntilTheApplicationValueIsSignalled();
    unsettledPrivateSemaphoreIsNotDestroyed();
    std::cout << "reflex queue drain tests passed\n";
    return 0;
}

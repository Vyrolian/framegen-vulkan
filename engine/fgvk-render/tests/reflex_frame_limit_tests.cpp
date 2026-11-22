/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/reflex_frame_limit.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

using namespace fgvk::layer;

namespace {
void expect(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

constexpr uintptr_t mainQueue = 0x100;

template<class T> T handle(uintptr_t value) { return reinterpret_cast<T>(value); }

const auto swapchain = handle<VkSwapchainKHR>(0x3000);

VkLatencySleepModeInfoNV request(const bool mode, const uint32_t intervalUs, const bool boost = false) {
    return VkLatencySleepModeInfoNV{
        .sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV,
        .pNext = nullptr,
        .lowLatencyMode = mode,
        .lowLatencyBoost = boost,
        .minimumIntervalUs = intervalUs,
    };
}

// The rule: the cap's interval is the least Reflex may hold, and only with Reflex on and frame generation on.
void intervalRule() {
    expect(reflexMinimumIntervalUs(request(false, 0), true, 60.0) == 0,
        "Reflex off raised the interval");
    expect(reflexMinimumIntervalUs(request(true, 0), false, 60.0) == 0,
        "frame generation off raised the interval");
    expect(reflexMinimumIntervalUs(request(true, 0), true, 0.0) == 0,
        "an absent cap raised the interval");
    expect(reflexMinimumIntervalUs(request(true, 0), true, 60.0) == 16667,
        "60 fps did not map to ceil(1e6 / 60) us");
    expect(reflexMinimumIntervalUs(request(true, 10000), true, 60.0) == 16667,
        "a shorter application interval was kept above the cap interval");
    expect(reflexMinimumIntervalUs(request(true, 20000), true, 60.0) == 20000,
        "a longer application interval was lowered to the cap interval");
    expect(reflexMinimumIntervalUs(request(true, 0), true, 48.0) == 20834,
        "48 fps did not map to ceil(1e6 / 48) us");
    expect(reflexMinimumIntervalUs(request(true, 0), true, 59.94) == 16684,
        "a refresh-derived cap did not round its interval up");
    expect(reflexMinimumIntervalUs(request(true, 7), true, std::numeric_limits<double>::infinity()) == 7,
        "an infinite cap changed the application interval");
    expect(reflexMinimumIntervalUs(request(true, 7), true, std::numeric_limits<double>::quiet_NaN()) == 7,
        "a NaN cap changed the application interval");
    expect(reflexMinimumIntervalUs(request(true, 0), true, 1e-9) == std::numeric_limits<uint32_t>::max(),
        "a vanishing cap did not saturate the interval");
}

struct Recorder {
    std::vector<ReflexFrameLimit::Forward> calls;
    VkResult result{VK_SUCCESS};

    auto send() {
        return [this](const ReflexFrameLimit::Forward& forward) {
            calls.push_back(forward);
            return result;
        };
    }
};

// Application requests and present-path observations on one swapchain, in the order a game makes them.
void forwardsOnlyChanges() {
    ReflexFrameLimit limit;
    Recorder driver;

    limit.observe(swapchain, true, 60.0, driver.send());
    expect(driver.calls.empty(), "a present before any Reflex request called the driver");

    limit.applicationRequest(swapchain, request(true, 0), driver.send());
    expect(driver.calls.size() == 1 && driver.calls.back().info.minimumIntervalUs == 16667 &&
            driver.calls.back().applicationIntervalUs == 0 &&
            driver.calls.back().previousIntervalUs == 0 &&
            std::string_view(driver.calls.back().reason) == "application",
        "the application request was not forwarded with the cap interval");

    limit.observe(swapchain, true, 60.0, driver.send());
    expect(driver.calls.size() == 1, "an unchanged interval reached the driver again");

    limit.observe(swapchain, true, 48.0, driver.send());
    expect(driver.calls.size() == 2 && driver.calls.back().info.minimumIntervalUs == 20834 &&
            driver.calls.back().previousIntervalUs == 16667 &&
            std::string_view(driver.calls.back().reason) == "cap",
        "a cap change did not re-send once with the new interval");

    limit.observe(swapchain, false, 48.0, driver.send());
    expect(driver.calls.size() == 3 && driver.calls.back().info.minimumIntervalUs == 0 &&
            std::string_view(driver.calls.back().reason) == "frame-generation",
        "frame generation off did not re-send the application's value");

    limit.observe(swapchain, false, 48.0, driver.send());
    expect(driver.calls.size() == 3, "a repeated frame-generation-off present called the driver");

    limit.applicationRequest(swapchain, request(false, 5000), driver.send());
    expect(driver.calls.size() == 4 && driver.calls.back().info.minimumIntervalUs == 5000,
        "a request with Reflex off was not forwarded unchanged");

    limit.observe(swapchain, true, 60.0, driver.send());
    expect(driver.calls.size() == 4, "Reflex off did not leave the interval alone at the cap");

    limit.applicationRequest(swapchain, request(true, 0, true), driver.send());
    expect(driver.calls.size() == 5 && driver.calls.back().info.minimumIntervalUs == 16667 &&
            driver.calls.back().info.lowLatencyBoost == VK_TRUE,
        "the boost flag was not forwarded with the cap interval");
}

// A re-send carries the stored request, never the application's pNext, which may already be gone.
void reSendDropsPNext() {
    ReflexFrameLimit limit;
    Recorder driver;
    const int extension = 0;
    auto withChain = request(true, 0);
    withChain.pNext = &extension;

    limit.applicationRequest(swapchain, withChain, driver.send());
    expect(driver.calls.size() == 1 && driver.calls.back().info.pNext == &extension,
        "the application request did not keep its own pNext");

    limit.observe(swapchain, true, 60.0, driver.send());
    expect(driver.calls.size() == 2 && driver.calls.back().info.pNext == nullptr &&
            driver.calls.back().info.minimumIntervalUs == 16667,
        "a re-send carried the application's pNext");
}

void driverResultIsReturned() {
    ReflexFrameLimit limit;
    Recorder driver;
    driver.result = VK_ERROR_EXTENSION_NOT_PRESENT;
    const auto result = limit.applicationRequest(swapchain, request(true, 0), driver.send());
    expect(result == VK_ERROR_EXTENSION_NOT_PRESENT,
        "the application request did not return the driver's result");
    expect(!limit.reflexLowLatency(), "a rejected Reflex-on request changed the accepted mode");
    limit.observe(swapchain, true, 60.0, driver.send());
    expect(driver.calls.size() == 1, "a rejected application request was replayed by the present path");
}

void nullRequestDisablesReflex() {
    ReflexFrameLimit limit;
    Recorder driver;
    limit.observe(swapchain, true, 60.0, driver.send());
    limit.applicationRequest(swapchain, request(true, 0, true), driver.send());
    const auto failed = limit.clearRequest([] { return VK_ERROR_UNKNOWN; });
    expect(failed == VK_ERROR_UNKNOWN && limit.reflexLowLatency(),
        "a rejected clear changed the accepted mode");

    const auto cleared = limit.clearRequest([] { return VK_SUCCESS; });
    expect(cleared == VK_SUCCESS && !limit.reflexLowLatency(),
        "a successful clear left Reflex on");
    limit.observe(swapchain, true, 48.0, driver.send());
    expect(driver.calls.size() == 1, "a cap change replayed the disabled Reflex request");

    limit.applicationRequest(swapchain, request(true, 0), driver.send());
    expect(driver.calls.size() == 2 && driver.calls.back().previousIntervalUs == 0 &&
            driver.calls.back().info.minimumIntervalUs == 20834,
        "re-enabling Reflex did not use the current cap and reset driver interval");
}

void failedCapUpdateIsNotApplied() {
    ReflexFrameLimit limit;
    Recorder driver;
    limit.applicationRequest(swapchain, request(true, 0), driver.send());
    driver.result = VK_ERROR_UNKNOWN;
    limit.observe(swapchain, true, 60.0, driver.send());
    driver.result = VK_SUCCESS;
    limit.observe(swapchain, true, 60.0, driver.send());
    expect(driver.calls.size() == 3 && driver.calls.back().previousIntervalUs == 0 &&
            driver.calls.back().info.minimumIntervalUs == 16667,
        "a rejected interval was cached as applied");
    limit.observe(swapchain, true, 60.0, driver.send());
    expect(driver.calls.size() == 3, "an accepted retry was sent again");
}

// The present path reads whether the last request asks for low latency mode. A present does not change it.
void lowLatencyStateFollowsTheLastRequest() {
    ReflexFrameLimit limit;
    Recorder driver;
    expect(!limit.reflexLowLatency(), "a swapchain with no Reflex request reported low latency");
    limit.applicationRequest(swapchain, request(true, 0), driver.send());
    expect(limit.reflexLowLatency(), "a Reflex-on request was not reported");
    limit.observe(swapchain, true, 60.0, driver.send());
    expect(limit.reflexLowLatency(), "a present changed the reported Reflex state");
    limit.applicationRequest(swapchain, request(false, 0), driver.send());
    expect(!limit.reflexLowLatency(), "a Reflex-off request was still reported as on");
    limit.applicationRequest(swapchain, request(true, 0), driver.send());
    expect(limit.reflexLowLatency(), "a second Reflex-on request was not reported");
}

// The application calls from another thread while presents observe the cap. Every driver call
// happens under the lock, so the final driver state matches the last decision.
void concurrentRequestsKeepDriverState() {
    ReflexFrameLimit limit;
    std::vector<uint32_t> driverIntervals;
    const auto send = [&](const ReflexFrameLimit::Forward& forward) {
        driverIntervals.push_back(forward.info.minimumIntervalUs);
        return VK_SUCCESS;
    };
    std::thread presenter([&] {
        for (int frame = 0; frame < 20000; ++frame)
            limit.observe(swapchain, true, frame % 2 ? 60.0 : 48.0, send);
    });
    for (int call = 0; call < 2000; ++call)
        limit.applicationRequest(swapchain, request(true, 0), send);
    presenter.join();
    limit.observe(swapchain, true, 60.0, send);
    expect(!driverIntervals.empty() && driverIntervals.back() == 16667,
        "the driver did not end on the final cap interval");
}
// RDC1: the dynamic cap raises the forwarded interval only with its setting, Reflex low latency mode, frame generation
// and a cap. A change under 50 us is not sent, a return to the static interval always is, and an application request
// carries the raised interval too.
void dynamicCapForwarding() {
    ReflexFrameLimit limit;
    Recorder driver;
    const auto cap = std::make_shared<ReflexDynamicCap>();
    const int64_t now = reflexClockNs();
    const auto feedWindow = [&](const int64_t waitNs) {
        for (size_t frame = 0; frame < reflexDynamicWindowFrames; ++frame)
            cap->feed(mainQueue, 18, waitNs, now);
    };
    const auto observe = [&](const bool setting, Recorder& recorder) {
        limit.observe(swapchain, true, 119.99, ReflexDynamicInput{.controller = cap, .setting = setting, .nowNs = now},
            recorder.send());
    };

    limit.applicationRequest(swapchain, request(true, 0), driver.send());
    observe(true, driver);
    expect(driver.calls.size() == 2 && driver.calls.back().info.minimumIntervalUs == 8335 &&
            std::string_view(driver.calls.back().reason) == "frame-generation",
        "the 119.99 fps cap interval was not forwarded");

    // A 5 ms median raises it by 5 %: 8335 -> 8752. (A refresh-derived 119.99 fps cap is 8335 us.)
    feedWindow(5'000'000);
    observe(true, driver);
    expect(driver.calls.size() == 3 && driver.calls.back().info.minimumIntervalUs == 8752 &&
            driver.calls.back().previousIntervalUs == 8335 && driver.calls.back().dynamic &&
            std::string_view(driver.calls.back().reason) == "dynamic",
        "a raised interval was not forwarded as dynamic");
    observe(true, driver);
    expect(driver.calls.size() == 3, "an unchanged dynamic interval reached the driver again");

    // The first 0.5 % decay moves 44 us, under the 50 us threshold: nothing is sent.
    feedWindow(100'000);
    observe(true, driver);
    expect(driver.calls.size() == 3, "a dynamic change under 50 us reached the driver");
    // The second decay moves 87 us from the sent value and is sent.
    feedWindow(100'000);
    observe(true, driver);
    expect(driver.calls.size() == 4 && driver.calls.back().info.minimumIntervalUs == 8665 &&
            driver.calls.back().previousIntervalUs == 8752,
        "a dynamic change of 50 us or more was not sent");

    // Decays continue until the static interval, and the return to it is exact.
    for (int window = 0; window < 400 && driver.calls.back().info.minimumIntervalUs != 8335; ++window) {
        feedWindow(100'000);
        observe(true, driver);
    }
    expect(driver.calls.back().info.minimumIntervalUs == 8335 && !driver.calls.back().dynamic &&
            std::string_view(driver.calls.back().reason) == "dynamic",
        "the return to the static interval was not exact");
    const auto settled = driver.calls.size();
    observe(true, driver);
    expect(driver.calls.size() == settled, "the static interval was sent again");

    // The setting off: even high waits forward the static interval only.
    feedWindow(5'000'000);
    observe(true, driver);
    expect(driver.calls.back().info.minimumIntervalUs == 8752, "setup did not raise the interval again");
    observe(false, driver);
    expect(driver.calls.back().info.minimumIntervalUs == 8335 && !driver.calls.back().dynamic,
        "turning the setting off did not return the static interval");
    const auto off = driver.calls.size();
    feedWindow(5'000'000);
    observe(false, driver);
    expect(driver.calls.size() == off, "a setting that is off forwarded a dynamic interval");

    // An application request while the cap is raised carries the raised interval.
    feedWindow(5'000'000);
    observe(true, driver);
    expect(driver.calls.back().info.minimumIntervalUs == 8752, "setup did not raise the interval for the request");
    limit.applicationRequest(swapchain, request(true, 0), driver.send());
    expect(driver.calls.back().info.minimumIntervalUs == 8752 &&
            std::string_view(driver.calls.back().reason) == "application" &&
            driver.calls.back().applicationIntervalUs == 0,
        "an application request did not carry the dynamic interval");
}
}

int main() {
    intervalRule();
    forwardsOnlyChanges();
    reSendDropsPNext();
    driverResultIsReturned();
    nullRequestDisablesReflex();
    failedCapUpdateIsNotApplied();
    lowLatencyStateFollowsTheLastRequest();
    concurrentRequestsKeepDriverState();
    dynamicCapForwarding();
    std::cout << "reflex frame limit tests passed\n";
}

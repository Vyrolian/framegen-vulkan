/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/anti_lag_frame_limit.hpp"
#include "swapchain/low_latency.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <vector>

using namespace fgvk::layer;
using namespace std::chrono_literals;

namespace {
void expect(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

template<class T> T handle(uintptr_t value) { return reinterpret_cast<T>(value); }

VkDevice fakeDevice() { return handle<VkDevice>(0x1000); }

VkAntiLagDataAMD request(const VkAntiLagModeAMD mode, const uint32_t maxFps = 0) {
    return VkAntiLagDataAMD{
        .sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD,
        .pNext = nullptr,
        .mode = mode,
        .maxFPS = maxFps,
        .pPresentationInfo = nullptr,
    };
}

// The rule: the limit is the application's own, or the cap rounded up when that is lower. OFF, frame generation
// off and no cap pass the application's limit through. A positive cap never rounds to 0, which the driver reads as
// no limit.
void maxFpsRule() {
    constexpr auto on = VK_ANTI_LAG_MODE_ON_AMD;
    constexpr auto off = VK_ANTI_LAG_MODE_OFF_AMD;
    constexpr auto driver = VK_ANTI_LAG_MODE_DRIVER_CONTROL_AMD;
    expect(antiLagMaxFps(off, 30, true, 60.0) == 30, "OFF did not pass the application's limit through");
    expect(antiLagMaxFps(off, 0, true, 60.0) == 0, "OFF with no limit applied the cap");
    expect(antiLagMaxFps(on, 0, false, 60.0) == 0, "frame generation off applied the cap");
    expect(antiLagMaxFps(on, 45, false, 60.0) == 45, "frame generation off changed the application's limit");
    expect(antiLagMaxFps(on, 0, true, 0.0) == 0, "an absent cap applied a limit");
    expect(antiLagMaxFps(on, 45, true, 0.0) == 45, "an absent cap changed the application's limit");
    expect(antiLagMaxFps(on, 0, true, 60.0) == 60, "no application limit did not take the cap");
    expect(antiLagMaxFps(on, 45, true, 60.0) == 45, "a lower application limit was raised to the cap");
    expect(antiLagMaxFps(on, 90, true, 60.0) == 60, "a higher application limit was not clamped to the cap");
    expect(antiLagMaxFps(on, 0, true, 71.99) == 72, "a fractional cap was not rounded up");
    expect(antiLagMaxFps(on, 0, true, 72.0) == 72, "an exact cap was not kept");
    expect(antiLagMaxFps(on, 0, true, 60.0000005) == 60, "the rounding tolerance moved an exact cap up");
    expect(antiLagMaxFps(driver, 0, true, 60.0) == 60, "DRIVER_CONTROL did not take the cap");
    expect(antiLagMaxFps(driver, 0, false, 60.0) == 0, "DRIVER_CONTROL with frame generation off applied the cap");
    expect(antiLagMaxFps(on, 0, true, 1e-9) == 1, "a vanishing cap disabled the limit");
    expect(antiLagMaxFps(on, 0, true, std::numeric_limits<double>::infinity()) ==
            std::numeric_limits<uint32_t>::max(), "an infinite cap did not saturate");
    expect(antiLagMaxFps(on, 7, true, std::numeric_limits<double>::quiet_NaN()) == 7,
        "a NaN cap changed the application's limit");
}

struct Recorder {
    std::vector<VkAntiLagDataAMD> calls;
    std::vector<const VkAntiLagDataAMD*> pointers;
    VkResult result{VK_SUCCESS};

    auto send() {
        return [this](const VkAntiLagDataAMD* forwarded) {
            pointers.push_back(forwarded);
            if (forwarded) calls.push_back(*forwarded);
            return result;
        };
    }
};

// The driver gets a copy: only maxFPS is limited, and the mode and the presentation stage pass through untouched.
void forwardsCopyWithOnlyMaxFpsChanged() {
    AntiLagFrameLimit limit;
    Recorder driver;
    limit.observe(true, 60.0);
    const VkAntiLagPresentationInfoAMD info{
        .sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD,
        .pNext = nullptr,
        .stage = VK_ANTI_LAG_STAGE_PRESENT_AMD,
        .frameIndex = 9,
    };
    auto application = request(VK_ANTI_LAG_MODE_ON_AMD, 90);
    application.pPresentationInfo = &info;
    limit.applicationUpdate(application, driver.send());
    expect(driver.calls.size() == 1 && driver.calls.back().maxFPS == 60,
        "the application's limit was not clamped to the cap");
    expect(driver.calls.back().mode == VK_ANTI_LAG_MODE_ON_AMD &&
            driver.calls.back().pPresentationInfo == &info && info.frameIndex == 9 &&
            info.stage == VK_ANTI_LAG_STAGE_PRESENT_AMD,
        "the mode or the presentation stage did not pass through untouched");
    expect(application.maxFPS == 90, "the application's own request was changed");
}

// The present path's observation is the cap of the next update. Without frame generation the application's limit stands.
void observeSetsTheCapForTheNextUpdate() {
    AntiLagFrameLimit limit;
    Recorder driver;
    limit.applicationUpdate(request(VK_ANTI_LAG_MODE_ON_AMD), driver.send());
    expect(driver.calls.back().maxFPS == 0, "an update before any present applied a cap");
    limit.observe(true, 48.0);
    limit.applicationUpdate(request(VK_ANTI_LAG_MODE_ON_AMD), driver.send());
    expect(driver.calls.back().maxFPS == 48, "a present's cap was not applied to the next update");
    limit.observe(false, 48.0);
    limit.applicationUpdate(request(VK_ANTI_LAG_MODE_ON_AMD), driver.send());
    expect(driver.calls.back().maxFPS == 0, "frame generation off kept the cap");
}

// Holds the game while the last accepted call asked for ON, for 500 ms after that call. Times are injected by `now`.
void holdsGameFollowsTheLastOnCall() {
    AntiLagFrameLimit limit;
    Recorder driver;
    expect(!limit.holdsGame(AntiLagFrameLimit::Clock::now()), "a game that never called anti-lag was held");
    const auto before = AntiLagFrameLimit::Clock::now();
    limit.applicationUpdate(request(VK_ANTI_LAG_MODE_ON_AMD), driver.send());
    const auto after = AntiLagFrameLimit::Clock::now();
    expect(limit.holdsGame(after), "ON was not holding the game");
    expect(limit.holdsGame(before + 499ms), "ON stopped holding the game before 500 ms");
    expect(!limit.holdsGame(after + 500ms), "ON still held the game 500 ms after its last call");
    limit.applicationUpdate(request(VK_ANTI_LAG_MODE_DRIVER_CONTROL_AMD), driver.send());
    expect(!limit.holdsGame(after), "DRIVER_CONTROL was reported as holding the game");
    limit.applicationUpdate(request(VK_ANTI_LAG_MODE_ON_AMD), driver.send());
    const auto on = AntiLagFrameLimit::Clock::now();
    limit.applicationUpdate(request(VK_ANTI_LAG_MODE_OFF_AMD), driver.send());
    expect(!limit.holdsGame(on), "OFF was not reported as releasing the game");
}

// A null pData is forwarded unchanged and changes neither the mode nor the time of the last call.
void nullDataForwardsUnchanged() {
    AntiLagFrameLimit limit;
    Recorder driver;
    limit.applicationUpdate(static_cast<const VkAntiLagDataAMD*>(nullptr), driver.send());
    expect(driver.pointers.size() == 1 && driver.pointers.back() == nullptr,
        "a null pData was not forwarded unchanged");
    expect(!limit.holdsGame(AntiLagFrameLimit::Clock::now()), "a null pData made the game held");
    limit.applicationUpdate(request(VK_ANTI_LAG_MODE_ON_AMD), driver.send());
    const auto updated = AntiLagFrameLimit::Clock::now();
    limit.applicationUpdate(static_cast<const VkAntiLagDataAMD*>(nullptr), driver.send());
    expect(driver.pointers.size() == 3 && driver.pointers.back() == nullptr,
        "a null pData after an update was not forwarded unchanged");
    expect(limit.holdsGame(updated), "a null pData changed the game's anti-lag state");
}

// The present path never waits behind the driver call: the INPUT stage may sleep in the driver. With the lock held
// across the call, observe and holdsGame inside it would deadlock on the same thread.
void driverCallHoldsNoLock() {
    AntiLagFrameLimit limit;
    limit.observe(true, 60.0);
    bool reached = false;
    limit.applicationUpdate(request(VK_ANTI_LAG_MODE_ON_AMD), [&](const VkAntiLagDataAMD*) {
        limit.observe(true, 60.0);
        static_cast<void>(limit.holdsGame(AntiLagFrameLimit::Clock::now()));
        reached = true;
        return VK_SUCCESS;
    });
    expect(reached, "the driver call was not made");
    expect(limit.holdsGame(AntiLagFrameLimit::Clock::now()), "the call was not recorded after the driver returned");
}

// One std::clog line when the applied limit or the mode changes, and none for a repeated call.
void logsOnlyChanges() {
    std::ostringstream captured;
    auto* previous = std::clog.rdbuf(captured.rdbuf());
    AntiLagFrameLimit limit(fakeDevice());
    Recorder driver;
    limit.observe(true, 60.0);
    limit.applicationUpdate(request(VK_ANTI_LAG_MODE_ON_AMD), driver.send());
    limit.applicationUpdate(request(VK_ANTI_LAG_MODE_ON_AMD), driver.send());
    limit.applicationUpdate(request(VK_ANTI_LAG_MODE_ON_AMD, 90), driver.send());
    limit.applicationUpdate(request(VK_ANTI_LAG_MODE_ON_AMD, 45), driver.send());
    limit.applicationUpdate(request(VK_ANTI_LAG_MODE_DRIVER_CONTROL_AMD, 45), driver.send());
    std::clog.rdbuf(previous);
    const auto text = captured.str();
    expect(std::count(text.begin(), text.end(), '\n') == 3, "the log did not write one line per change");
    expect(text.find("framegen-vulkan: anti-lag frame limit: device=") == 0 &&
            text.find(" mode=on app_max_fps=0 applied_max_fps=60 result=0\n") != std::string::npos &&
            text.find(" mode=on app_max_fps=45 applied_max_fps=45 result=0\n") != std::string::npos &&
            text.find(" mode=driver app_max_fps=45 applied_max_fps=45 result=0\n") != std::string::npos,
        "the log line does not carry the mode, the application's limit and the applied limit");
}

void registryKeepsOneEntryPerDevice() {
    const auto first = AntiLagFrameLimitRegistry::get(fakeDevice());
    expect(first == AntiLagFrameLimitRegistry::get(fakeDevice()), "the device did not keep one anti-lag entry");
    const auto other = handle<VkDevice>(0x2000);
    expect(AntiLagFrameLimitRegistry::get(other) != first, "two devices shared one anti-lag entry");
    AntiLagFrameLimitRegistry::erase(fakeDevice());
    expect(AntiLagFrameLimitRegistry::get(fakeDevice()) != first, "an erased device kept its anti-lag entry");
    AntiLagFrameLimitRegistry::erase(fakeDevice());
    AntiLagFrameLimitRegistry::erase(other);
}

// While the game's anti-lag holds it, low_latency's release delay steps aside and the just-in-time batch start stays.
void antiLagHoldKeepsJustInTimeAndStepsReleaseDelayAside() {
    AntiLagFrameLimit limit;
    Recorder driver;
    limit.observe(true, 60.0);
    limit.applicationUpdate(request(VK_ANTI_LAG_MODE_ON_AMD), driver.send());
    const bool antiLagHolds = limit.holdsGame(AntiLagFrameLimit::Clock::now());
    expect(antiLagHolds, "the anti-lag hold was not visible to the present path");
    // The present path passes reflexLowLatency || antiLagHoldsGame; Reflex is off in this test.
    const auto held = lowLatencyPlan(true, true, true, antiLagHolds);
    expect(held.justInTime, "anti-lag holding turned off the just-in-time batch start");
    expect(!held.releaseDelay, "anti-lag holding left the release delay running");
    expect(held.reflexHolds, "anti-lag holding was not reported as holding the game");
    limit.applicationUpdate(request(VK_ANTI_LAG_MODE_OFF_AMD), driver.send());
    const auto released = lowLatencyPlan(true, true, true, limit.holdsGame(AntiLagFrameLimit::Clock::now()));
    expect(released.releaseDelay && !released.reflexHolds, "OFF did not return the release delay");
}
}

int main() {
    maxFpsRule();
    forwardsCopyWithOnlyMaxFpsChanged();
    observeSetsTheCapForTheNextUpdate();
    holdsGameFollowsTheLastOnCall();
    nullDataForwardsUnchanged();
    driverCallHoldsNoLock();
    logsOnlyChanges();
    registryKeepsOneEntryPerDevice();
    antiLagHoldKeepsJustInTimeAndStepsReleaseDelayAside();
    std::cout << "anti-lag frame limit tests passed\n";
}

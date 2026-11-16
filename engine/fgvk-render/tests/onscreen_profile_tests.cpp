/* SPDX-License-Identifier: GPL-3.0-or-later */
// GP1 and OS1 tests: the on-screen gate, the lower present ID owner with its translation, and the
// watcher with its screen samples. No Vulkan: the watcher's wait is a fake function.
#include "swapchain/onscreen_watch.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

using namespace fgvk::layer;
using namespace std::chrono_literals;

namespace {
void expect(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

// Collects the records the watcher emits. The watcher thread adds, the test reads.
class Recorder {
public:
    void add(const OnScreenRecord& record) {
        const std::lock_guard lock(mutex);
        records.push_back(record);
    }
    [[nodiscard]] std::vector<OnScreenRecord> snapshot() const {
        const std::lock_guard lock(mutex);
        return records;
    }
    [[nodiscard]] bool waitForCount(const size_t count, const std::chrono::milliseconds limit = 5s) const {
        const auto until = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < until) {
            if (snapshot().size() >= count) return true;
            std::this_thread::sleep_for(1ms);
        }
        return snapshot().size() >= count;
    }
private:
    mutable std::mutex mutex;
    std::vector<OnScreenRecord> records;
};

// OS1: the screen samples the watcher hands over. The watcher thread adds, the test reads.
class ScreenRecorder {
public:
    void add(const ScreenSample& sample) {
        const std::lock_guard lock(mutex);
        samples.push_back(sample);
    }
    [[nodiscard]] std::vector<ScreenSample> snapshot() const {
        const std::lock_guard lock(mutex);
        return samples;
    }
private:
    mutable std::mutex mutex;
    std::vector<ScreenSample> samples;
};

OnScreenWatch::Emit emitInto(const std::shared_ptr<Recorder>& recorder) {
    return [recorder](const OnScreenRecord& record) { recorder->add(record); };
}

OnScreenWatch::Observe observeInto(const std::shared_ptr<ScreenRecorder>& recorder) {
    return [recorder](const ScreenSample& sample) { recorder->add(sample); };
}

OnScreenWatch::Observe ignoreScreen() {
    return [](const ScreenSample&) {};
}

// ---- Gate -----------------------------------------------------------------------------------------

void gateNamesTheFirstFalseInput() {
    using Kind = OnScreenKind;
    const auto active = onScreenGate(true, Kind::Gamescope, true, true, true);
    expect(active.active && active.reason == nullptr && active.kind == Kind::Gamescope,
        "gate is active when every input holds, and names its session kind");
    expect(onScreenGate(true, Kind::X11, true, true, true).active,
        "an X11 surface (Xwayland or plain X11) passes the session kind");
    expect(std::string_view(onScreenGate(false, Kind::Gamescope, true, true, true).reason) == "disabled",
        "the enable (the profile or the correction) is the first input");
    expect(std::string_view(onScreenGate(true, Kind::None, true, true, true).reason) == "surface-kind",
        "a native Wayland surface outside gamescope is refused");
    expect(std::string_view(onScreenGate(true, Kind::X11, false, true, true).reason) == "present-id2-flag",
        "the id2 flag is the third input");
    expect(std::string_view(onScreenGate(true, Kind::X11, true, false, true).reason) == "present-wait2-flag",
        "the wait2 flag is the fourth input");
    expect(std::string_view(onScreenGate(true, Kind::X11, true, true, false).reason) == "wait-function",
        "the wait function is the fifth input");
    // Every combination of the enable, the session kind (three values) and the three flags.
    for (unsigned mask = 0; mask < 48; ++mask) {
        const bool enabled = (mask % 2) != 0;
        const auto kind = static_cast<Kind>((mask / 2) % 3);
        const unsigned flags = mask / 6;
        const auto gate = onScreenGate(enabled, kind, (flags & 1) != 0, (flags & 2) != 0, (flags & 4) != 0);
        expect(gate.active == (enabled && kind != Kind::None && flags == 7),
            "gate is active exactly when the profile is on, the surface kind is known and all flags hold");
        expect(gate.active == (gate.reason == nullptr), "an inactive gate always names a reason");
        expect(gate.kind == kind, "the gate carries its session kind");
    }
    expect(std::string_view(onScreenKindName(Kind::Gamescope)) == "gamescope" &&
        std::string_view(onScreenKindName(Kind::X11)) == "x11" &&
        std::string_view(onScreenKindName(Kind::None)) == "none", "session kind names");
}

// ---- Lower ID owner -------------------------------------------------------------------------------

void idsHandOutIncreasingFromOne() {
    OnScreenIds ids;
    expect(ids.allocate() == 1, "the first lower ID is 1");
    expect(ids.allocate() == 2, "lower IDs rise by one");
    expect(ids.allocate() == 3, "lower IDs keep rising");
}

void translateIsPresentWaitAtLeast() {
    OnScreenIds ids;
    ids.record(5, 101);
    ids.record(7, 102);
    ids.record(8, 103);
    expect(ids.translate(5) == 101, "an exact application ID gives its lower ID");
    expect(ids.translate(6) == 102, "a gap gives the next recorded application ID");
    expect(ids.translate(7) == 102, "the same gap rule holds at the next entry");
    expect(ids.translate(8) == 103, "the newest recorded entry is found");
    expect(!ids.translate(9), "an ID newer than every record has no translation");
    expect(ids.translate(1) == 101, "an ID older than the recorded entries gives the oldest entry");
}

void ringKeepsTheNewestEntries() {
    OnScreenIds ids;
    for (uint64_t app = 1; app <= 300; ++app) ids.record(app, app + 1000);
    // 300 records into a 256-entry ring: 1..44 are gone, 45..300 remain.
    expect(ids.translate(1) == 1045, "an ID older than the ring gives the oldest kept entry");
    expect(ids.translate(45) == 1045, "the oldest kept entry is found");
    expect(ids.translate(300) == 1300, "the newest entry is found after wrapping");
    expect(!ids.translate(301), "an ID newer than the ring has no translation");
}

void zeroIdsAreNotRecorded() {
    OnScreenIds ids;
    ids.record(0, 5);
    ids.record(5, 0);
    expect(!ids.translate(0) && !ids.translate(1), "zero application or lower IDs are never recorded");
}

void firstMissIsReportedOnce() {
    OnScreenIds ids;
    expect(ids.firstMiss(), "the first translation miss is reported");
    expect(!ids.firstMiss(), "later translation misses are not reported again");
}

void registryFindsOnlyWhileRegistered() {
    const OnScreenRegistry::Key key{11, 22};
    expect(!OnScreenRegistry::anyActive(), "no owner is registered at the start");
    expect(!OnScreenRegistry::find(key), "nothing is found before registration");
    auto ids = std::make_shared<OnScreenIds>();
    OnScreenRegistry::add(key, ids);
    expect(OnScreenRegistry::anyActive(), "a registered owner makes the registry active");
    expect(OnScreenRegistry::find(key) == ids, "the registered owner is found by its device and swapchain");
    OnScreenRegistry::remove(key);
    expect(!OnScreenRegistry::find(key), "a removed owner is no longer found");
    expect(!OnScreenRegistry::anyActive(), "the registry is inactive again after the last removal");
}

// ---- Watcher ----------------------------------------------------------------------------------------

void watcherWaitsEachIdInOrder() {
    auto recorder = std::make_shared<Recorder>();
    auto calls = std::make_shared<std::vector<uint64_t>>();
    OnScreenWatch watch([calls](const uint64_t id, const uint64_t) {
        calls->push_back(id);
        return VK_SUCCESS;
    }, emitInto(recorder), ignoreScreen());
    for (uint64_t id = 1; id <= 5; ++id) watch.push(id, 7, 0, false);
    expect(recorder->waitForCount(5), "every pushed ID is emitted");
    watch.stop();
    expect(*calls == std::vector<uint64_t>{1, 2, 3, 4, 5}, "IDs are waited in push order");
    const auto records = recorder->snapshot();
    for (size_t i = 0; i < records.size(); ++i) {
        expect(records[i].lowerId == i + 1, "each record names its lower ID");
        expect(records[i].context == 7, "each record carries the swapchain's context");
        expect(records[i].result == VK_SUCCESS, "a completed wait is a success");
        expect(records[i].doneNs >= records[i].waitEnteredNs, "done is not before the wait entered");
    }
}

void timeoutsAreRetried() {
    auto recorder = std::make_shared<Recorder>();
    auto attempts = std::make_shared<int>(0);
    OnScreenWatch watch([attempts](const uint64_t id, const uint64_t) {
        if (id == 2 && ++*attempts <= 3) return VK_TIMEOUT;
        return VK_SUCCESS;
    }, emitInto(recorder), ignoreScreen());
    for (uint64_t id = 1; id <= 3; ++id) watch.push(id, 1, 0, false);
    expect(recorder->waitForCount(3), "a wait that times out is retried until it completes");
    watch.stop();
    expect(*attempts == 4, "ID 2 was waited four times: three timeouts, then success");
    for (const auto& record : recorder->snapshot())
        expect(record.result == VK_SUCCESS, "the retried wait is recorded as a success");
}

void unfinishedWaitEndsAtTheBudget() {
    auto recorder = std::make_shared<Recorder>();
    auto attempts = std::make_shared<std::atomic<int>>(0);
    OnScreenWatch watch([attempts](const uint64_t, const uint64_t) {
        ++*attempts;
        std::this_thread::sleep_for(10ms);
        return VK_TIMEOUT;
    }, emitInto(recorder), ignoreScreen());
    watch.push(9, 1, 0, false);
    expect(recorder->waitForCount(1), "a wait that never completes is recorded at its budget");
    watch.stop();
    const auto records = recorder->snapshot();
    expect(records.size() == 1 && records[0].result == VK_TIMEOUT, "the budget end is recorded as a timeout");
    expect(*attempts > 1, "the timed-out wait was retried before the budget ended");
    expect(records[0].doneNs - records[0].waitEnteredNs >= 900'000'000,
        "the retries covered (about) the one-second budget");
}

void anErrorEndsTheWaitsOnce() {
    auto recorder = std::make_shared<Recorder>();
    auto calls = std::make_shared<std::vector<uint64_t>>();
    OnScreenWatch watch([calls](const uint64_t id, const uint64_t) {
        calls->push_back(id);
        return id == 1 ? VK_ERROR_OUT_OF_DATE_KHR : VK_SUCCESS;
    }, emitInto(recorder), ignoreScreen());
    watch.push(1, 1, 0, false);
    expect(recorder->waitForCount(1), "the error is emitted");
    watch.push(2, 1, 0, false);
    watch.push(3, 1, 0, false);
    watch.stop();
    expect(*calls == std::vector<uint64_t>{1}, "no wait follows an error for this swapchain");
    const auto records = recorder->snapshot();
    expect(records.size() == 1 && records[0].result == VK_ERROR_OUT_OF_DATE_KHR,
        "the error is emitted once, with its result");
}

// OS1: a completed wait (success or suboptimal) is a screen sample carrying the pushed source and
// generated flag; an error is not, and it ends the waits.
void watcherObservesCompletedWaitsOnly() {
    auto recorder = std::make_shared<Recorder>();
    auto screen = std::make_shared<ScreenRecorder>();
    OnScreenWatch watch([](const uint64_t id, const uint64_t) {
        if (id == 2) return VK_ERROR_OUT_OF_DATE_KHR;
        if (id == 3) return VK_SUBOPTIMAL_KHR;
        return VK_SUCCESS;
    }, emitInto(recorder), observeInto(screen));
    watch.push(1, 7, 4, false);
    watch.push(3, 7, 5, true);
    watch.push(2, 7, 6, false);
    expect(recorder->waitForCount(3), "all three waits are emitted");
    watch.stop();
    const auto samples = screen->snapshot();
    expect(samples.size() == 2, "only the two completed waits are screen samples");
    expect(samples[0].context == 7 && samples[0].source == 4 && !samples[0].generated,
        "the first sample carries the pushed context, source and generated flag");
    expect(samples[1].context == 7 && samples[1].source == 5 && samples[1].generated,
        "a suboptimal wait is a sample with its own generated flag");
    expect(samples[0].screenNs > 0 && samples[1].screenNs >= samples[0].screenNs,
        "screen samples carry steady-clock times in present order");
    const auto records = recorder->snapshot();
    expect(records.size() == 3 && records[2].result == VK_ERROR_OUT_OF_DATE_KHR,
        "the error is still emitted as a record");
}

void fullRingDropsAndCounts() {
    auto recorder = std::make_shared<Recorder>();
    auto gate = std::make_shared<std::mutex>();
    auto changed = std::make_shared<std::condition_variable>();
    auto entered = std::make_shared<std::atomic_bool>(false);
    auto released = std::make_shared<bool>(false);
    OnScreenWatch watch([gate, changed, entered, released](const uint64_t id, const uint64_t) {
        if (id != 1) return VK_SUCCESS;
        entered->store(true);
        std::unique_lock lock(*gate);
        changed->wait(lock, [&] { return *released; });
        return VK_SUCCESS;
    }, emitInto(recorder), ignoreScreen());
    watch.push(1, 1, 0, false);
    const auto until = std::chrono::steady_clock::now() + 5s;
    while (!entered->load() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(1ms);
    expect(entered->load(), "the first wait is in progress");
    for (uint64_t id = 2; id < 2 + 512; ++id) watch.push(id, 1, 0, false);
    for (uint64_t id = 1000; id < 1010; ++id) watch.push(id, 1, 0, false);
    expect(watch.droppedCount() == 10, "entries pushed into a full ring are dropped and counted");
    {
        const std::lock_guard lock(*gate);
        *released = true;
    }
    changed->notify_all();
    expect(recorder->waitForCount(513), "the ring's entries are still waited after the drops");
    watch.stop();
    const auto records = recorder->snapshot();
    expect(records.back().dropped == 10, "the record carries the drop count so far");
}

void stopJoinsTheWaitInProgress() {
    auto recorder = std::make_shared<Recorder>();
    auto inFlight = std::make_shared<std::atomic_bool>(false);
    OnScreenWatch watch([inFlight](const uint64_t, const uint64_t) {
        inFlight->store(true);
        std::this_thread::sleep_for(60ms);
        inFlight->store(false);
        return VK_SUCCESS;
    }, emitInto(recorder), ignoreScreen());
    watch.push(1, 1, 0, false);
    const auto until = std::chrono::steady_clock::now() + 5s;
    while (!inFlight->load() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(1ms);
    expect(inFlight->load(), "the wait is in progress before stop");
    watch.stop();
    expect(!inFlight->load(), "stop returned while a wait was still in progress: the thread was not joined");
}

void stopIsBoundedByOneSlice() {
    OnScreenWatch watch([](const uint64_t, const uint64_t) {
        std::this_thread::sleep_for(std::chrono::milliseconds{OnScreenWatch::sliceNs / 1'000'000});
        return VK_TIMEOUT;
    }, [](const OnScreenRecord&) {}, ignoreScreen());
    watch.push(1, 1, 0, false);
    std::this_thread::sleep_for(20ms);
    const auto started = std::chrono::steady_clock::now();
    watch.stop();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    expect(elapsed < 250ms, "stop waits at most about one wait slice, not the whole retry budget");
}

void pushAfterStopIsIgnored() {
    auto calls = std::make_shared<int>(0);
    OnScreenWatch watch([calls](const uint64_t, const uint64_t) {
        ++*calls;
        return VK_SUCCESS;
    }, [](const OnScreenRecord&) {}, ignoreScreen());
    watch.stop();
    watch.push(1, 1, 0, false);
    expect(watch.droppedCount() == 0, "a push after stop is not counted as a drop");
    expect(*calls == 0, "a push after stop never reaches the wait");
}

void recordLineHasTheDocumentedFields() {
    OnScreenRecord record{};
    record.context = 17;
    record.lowerId = 5;
    record.result = VK_SUCCESS;
    record.waitEnteredNs = 100;
    record.doneNs = 250;
    record.dropped = 3;
    const auto line = formatOnScreenRecord(record, 4242);
    expect(line.rfind("framegen-vulkan: ", 0) == 0, "the record line starts with the framegen-vulkan prefix");
    expect(line.find("operation=pacing-profile-onscreen version=1") != std::string::npos,
        "the record names its operation and version");
    expect(line.find(" lower_present_id=5 ") != std::string::npos, "the record carries the lower ID");
    expect(line.find(" done_ns=250 dropped=3\n") != std::string::npos, "the record ends with done and dropped");
}

void profileRegistersUntilStopped() {
    const OnScreenRegistry::Key key{31, 32};
    OnScreenProfile profile(31, 32, 7, [](const uint64_t, const uint64_t) { return VK_SUCCESS; },
        [](const OnScreenRecord&) {}, ignoreScreen());
    expect(OnScreenRegistry::find(key).get() == &profile.lowerIds(),
        "a started profile registers its owner for application waits");
    profile.stop();
    expect(!OnScreenRegistry::find(key), "stopping a profile unregisters its owner");
    profile.stop();
    expect(!OnScreenRegistry::find(key), "stop is idempotent");
}
} // namespace

int main() {
    gateNamesTheFirstFalseInput();
    idsHandOutIncreasingFromOne();
    translateIsPresentWaitAtLeast();
    ringKeepsTheNewestEntries();
    zeroIdsAreNotRecorded();
    firstMissIsReportedOnce();
    registryFindsOnlyWhileRegistered();
    watcherWaitsEachIdInOrder();
    timeoutsAreRetried();
    unfinishedWaitEndsAtTheBudget();
    anErrorEndsTheWaitsOnce();
    watcherObservesCompletedWaitsOnly();
    fullRingDropsAndCounts();
    stopJoinsTheWaitInProgress();
    stopIsBoundedByOneSlice();
    pushAfterStopIsIgnored();
    recordLineHasTheDocumentedFields();
    profileRegistersUntilStopped();
    std::cout << "framegen-vulkan: on-screen profile tests passed\n";
    return 0;
}

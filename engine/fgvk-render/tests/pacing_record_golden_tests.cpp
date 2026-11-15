/* SPDX-License-Identifier: GPL-3.0-or-later */
// Golden text for every pacing record that the recorder writes. Each scenario runs in its own
// forked child with one gate set, writes through the real log with fixed inputs, and exits. The
// parent normalises only process and thread identities and compares the rest byte for byte.
// FGVK_PACING_RECORD_GOLDEN_WRITE=<path> regenerates the golden instead of comparing with it.
#include "present_diagnostics.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace fgvk::layer::present_diagnostics;
using namespace std::chrono_literals;

namespace {
void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

// Fixed, distinct handles. Non-dispatchable handles print as integers and dispatchable ones as pointers.
VkDevice fakeDevice() { return reinterpret_cast<VkDevice>(uintptr_t{0x1000}); }
VkQueue fakeQueue() { return reinterpret_cast<VkQueue>(uintptr_t{0x2000}); }
VkSwapchainKHR fakeSwapchain() { return reinterpret_cast<VkSwapchainKHR>(uint64_t{0x3000}); }
VkSemaphore fakeSemaphore(uint64_t value) { return reinterpret_cast<VkSemaphore>(value); }
VkCommandBuffer fakeCommand() { return reinterpret_cast<VkCommandBuffer>(uintptr_t{0x4000}); }

// Two scenario windows, each closed by a sample one second later, so every window record is written.
void profileScenario() {
    const auto start = Clock::time_point{10s};
    PacingProfile profile;
    profile.beginSource(start);
    profile.settings(true, true, 2, 60);
    // low_latency on, a 3 ms release delay, and the vkd3d, dxvk and acquire kinds used.
    profile.lowLatency(true, 3ms, 1u | 4u | 8u, true);
    profile.policy(false, 60, true);
    profile.backendScheduled(0);
    profile.wait(PacingProfile::SourceCapWait, 3ms);
    profile.wait(PacingProfile::ReadinessWait, 2ms);
    profile.wait(PacingProfile::OutputWait, 4ms);
    profile.phaseTiming({5ms, 1ms, 2ms, 3ms, 4ms, 5ms, 6ms, 7ms, 8ms});
    profile.gpuMicroseconds(PacingProfile::GpuPrepass, 2000);
    profile.gpuMicroseconds(PacingProfile::GpuGeneration, 8000);
    profile.gpuMicroseconds(PacingProfile::GpuOutput, 3000);
    profile.gpuMicroseconds(PacingProfile::GpuSpan, 12000);
    profile.gpuMicroseconds(PacingProfile::GpuReadback, 20);
    profile.completedWorkerBatch(true);
    profile.outputTiming(PacingProfile::ExtTiming, start + 10ms);
    profile.outputTiming(PacingProfile::ExtTiming, start + 25ms);
    profile.generatedSkipped(PacingProfile::SkipReadiness);
    profile.generatedSkipped(PacingProfile::SkipAcquire);
    profile.internalRealPresented(true);
    profile.internalRealPresented(false);
    profile.generatedPresented(true);
    profile.generatedPresented(false);
    profile.workerLockSplit(1000000, 2000000, 3000000);
    profile.internalImagesActive();
    profile.publicationCost(1ms);
    profile.output(start + 2ms, start + 3ms, VK_SUCCESS, {
        .scheduled = start + 1ms, .imageIndex = 4, .generated = true, .deferred = true,
        .copyChecked = true, .copyStatus = VK_NOT_READY, .copySampledAt = start + 3ms,
        .source = 1, .enqueued = start + 1ms, .queueLockStarted = start + 1ms,
        .queueLockAcquired = start + 2ms, .readinessChecked = true, .readinessStarted = start + 1ms,
        .readinessFinished = start + 2ms, .readinessDeadline = start + 2ms,
        .readinessResult = VK_SUCCESS, .queue = fakeQueue(), .presentWait = fakeSemaphore(0x5000),
        .arrival = start, .readyAt = start + 2ms, .slack = -1ms});
    const auto sample = profile.output(start + 9ms, start + 10ms, VK_ERROR_OUT_OF_DATE_KHR, {
        .scheduled = start + 9ms, .imageIndex = 2, .deferred = true, .copyChecked = true,
        .copyStatus = VK_SUCCESS, .copySampledAt = start + 10ms});
    profile.beginSource(start + 40ms);
    profile.wait(PacingProfile::SourceCapWait, 1ms);
    const auto source = profile.sourceSample(start + 45ms);
    const auto window = profile.finishSource(start + 1s);
    expect(window.has_value(), "the profile window did not close");
    logPacingProfile(17, VK_PRESENT_MODE_FIFO_KHR, start + 1s, *window, fakeSwapchain(), "wayland", true);
    logPacingProfile(18, VK_PRESENT_MODE_IMMEDIATE_KHR, start + 1s, *window);
    logPacingOutput(17, fakeSwapchain(), sample);
    logPacingSource(17, fakeSwapchain(), source);
    logPacingSkip(17, 1, "readiness", start + 1ms, start + 2ms);
    logPacingSkip(17, 2, "give-up", start + 3ms, start + 4ms);
    const std::array generated{125.0, 8.5};
    logPacingGpuFrame(17, 1, 1, 0, 1.5, 2.25, 3.0, 125.0, 250.0, 1.75, generated);
    logPacingGpuSplitFrame(17, 1, 0, 1000, 2000, 3000, 4000, 5000, 6000);
    // The output copy's timestamps; an unavailable output keeps no times even when given some.
    logPacingOutputGpu(17, 1, true, 10003000, 10004500);
    logPacingOutputGpu(17, 2, false, 10005000, 10006000);
    // T3b: the real copy's timestamps share the format under their own operation.
    logPacingRealCopyGpu(17, 3, true, 10007000, 10007100);
    // T4: the private queue marker after a lower present; unavailable keeps no times.
    logPacingPresentMarkerGpu(17, 4, 9, true, true, 10008000, 10008050);
    logPacingPresentMarkerGpu(17, 5, 10, false, false, 10009000, 10009050);

    // Application windows: the second call closes the first window.
    ApplicationAcquireDetails details{};
    details.internal = InternalAcquireTiming{1ms, 2ms, 3ms, false};
    recordApplicationFrameTiming(ApplicationFrameApi::AcquireKhr, 1, fakeSwapchain(), start,
        start + 2ms, VK_SUCCESS, 1000000, {}, {}, 2u, 17, 1, &details);
    recordApplicationFrameTiming(ApplicationFrameApi::AcquireKhr, 1, fakeSwapchain(), start + 1s,
        start + 1s + 2ms, VK_TIMEOUT, 0, {}, {}, std::nullopt, 17, 1, nullptr);
    recordApplicationFrameTiming(ApplicationFrameApi::Present, 1, fakeSwapchain(), start,
        start + 7ms, VK_SUCCESS, {}, 1ms, 5ms, 3u, 17, 1);
    recordApplicationFrameTiming(ApplicationFrameApi::Present, 1, fakeSwapchain(), start + 1s,
        start + 1s + 7ms, VK_SUCCESS, {}, 1ms, 5ms, 3u, 17, 1);
    recordApplicationFrameTiming(ApplicationFrameApi::AcquireKhr2, 1, fakeSwapchain(), start + 2s,
        start + 2s + 1ms, VK_NOT_READY, 0, {}, {}, std::nullopt, 17, 1);
    recordApplicationPresentWait(fakeDevice(), fakeSwapchain(), PresentWaitApi::Khr2, 123, 0,
        VK_TIMEOUT, start, start + 2ms);
    recordApplicationPresentWait(fakeDevice(), fakeSwapchain(), PresentWaitApi::Khr, 124, 1000,
        VK_SUCCESS, start + 1s, start + 1s + 3ms);
    recordApplicationPresentWait(fakeDevice(), fakeSwapchain(), PresentWaitApi::Khr, 125, 1000,
        VK_SUCCESS, start + 2s, start + 2s + 3ms);
    {
        ApplicationAcquireScope scope;
        recordApplicationAcquireDriver(90, VK_NOT_READY, start, start + 1ms, std::nullopt);
        recordApplicationAcquireDriver(0, VK_SUCCESS, start + 1ms, start + 2ms, 3u);
    }
}

void traceScenario() {
    const auto start = Clock::time_point{10s};
    PacingProfile profile;
    profile.beginSource(start);
    profile.backendScheduled(0);
    profile.wait(PacingProfile::SourceCapWait, 2ms);
    profile.phaseTiming({5ms, 1ms});
    logPacingSource(77, fakeSwapchain(), profile.sourceSample(start + 5ms));
    const auto sample = profile.output(start + 1ms, start + 2ms, VK_SUCCESS, {
        .imageIndex = 4, .generated = true, .deferred = true, .source = 1,
        .enqueued = start, .queueLockStarted = start, .queueLockAcquired = start + 1ms,
        .readinessChecked = true, .readinessStarted = start, .readinessFinished = start + 1ms,
        .readinessDeadline = start + 1ms, .readinessResult = VK_SUCCESS, .queue = fakeQueue(),
        .presentWait = fakeSemaphore(0x6000)});
    logPacingOutput(77, fakeSwapchain(), sample);
    recordApplicationFrameTiming(ApplicationFrameApi::Present, 1, fakeSwapchain(), start,
        start + 7ms, VK_SUCCESS, {}, 1ms, 5ms, 3u, 77, 1);
    recordApplicationPresentWait(fakeDevice(), fakeSwapchain(), PresentWaitApi::Khr2, 123, 0,
        VK_TIMEOUT, start, start + 2ms);
    recordApplicationPresentWait(fakeDevice(), fakeSwapchain(), PresentWaitApi::Khr, 124, 1000,
        VK_SUCCESS, start + 1ms, start + 4ms);
    {
        ApplicationAcquireScope scope;
        recordApplicationAcquireDriver(90, VK_NOT_READY, start, start + 1ms, std::nullopt);
    }
    // Eighteen waits: the record reports the count and keeps the first sixteen.
    std::array<VkSemaphore, 18> waits{};
    for (size_t i = 0; i < waits.size(); ++i) waits[i] = fakeSemaphore(0x7000 + i);
    const std::array signals{fakeSemaphore(0x8000), fakeSemaphore(0x8001)};
    const CopySubmission copy{
        .context = 77, .source = 1, .swapchain = fakeSwapchain(), .generated = true,
        .imageIndex = 3, .pass = 1, .device = fakeDevice(), .queue = fakeQueue(),
        .command = fakeCommand(), .waits = waits, .signals = signals,
        .timelineWait = fakeSemaphore(0x9000), .timelineSignal = fakeSemaphore(0x9001),
        .waitValue = 7, .signalValue = 8, .fence = reinterpret_cast<VkFence>(uintptr_t{0xa000}),
    };
    recordCopySubmission(copy, start, start + 3ms, true);
    recordCopySubmission(copy, start + 3ms, start + 4ms, false);
}

// Replaces the identities that differ between runs. Every other token is kept as written.
std::string normalize(const std::string& text) {
    std::istringstream lines(text);
    std::string line;
    std::string output;
    while (std::getline(lines, line)) {
        if (line.find("operation=pacing-profile-session") != std::string::npos) continue;
        // The writer reports its own scheduling, cost and process state. Those vary per run and are not records.
        if (line.find("operation=pacing-profile-writer") != std::string::npos ||
                line.find("operation=pacing-profile-recorder") != std::string::npos ||
                line.find("operation=pacing-profile-thread") != std::string::npos) continue;
        std::string rebuilt;
        size_t position = 0;
        while (position <= line.size()) {
            const size_t space = line.find(' ', position);
            const size_t end = space == std::string::npos ? line.size() : space;
            std::string token = line.substr(position, end - position);
            for (const char* key : {"pid=", "tid=", "stream=", "waiter=", "call_id="}) {
                if (token.starts_with(key)) token = std::string(key) + "N";
            }
            if (position) rebuilt += ' ';
            rebuilt += token;
            if (space == std::string::npos) break;
            position = space + 1;
        }
        output += rebuilt + '\n';
    }
    return output;
}

// Runs one scenario in a child with a fixed gate set and returns its normalised capture.
std::string runScenario(const std::filesystem::path& root, const char* name, bool profile) {
    const auto log = root / (std::string(name) + ".log");
    const auto child = fork();
    expect(child >= 0, "cannot fork golden scenario");
    if (child == 0) {
        for (const char* gate : {"FGVK_PACING_PROFILE", "FGVK_PACING_PROFILE_GPU", "FGVK_PACING_PROFILE_TRACE",
                "FGVK_PACING_PROFILE_WAYLAND",
                "FGVK_PACING_PROFILE_GPU_SPLIT", "FGVK_PRESENT_DIAGNOSTICS"})
            unsetenv(gate);
        setenv("FGVK_PACING_PROFILE", profile ? "1" : "0", 1);
        if (!profile) setenv("FGVK_PACING_PROFILE_TRACE", "1", 1);
        setenv("FGVK_PACING_PROFILE_LOG", log.c_str(), 1);
        initializePacingProfileLog();
        if (profile) profileScenario(); else traceScenario();
        flushPacingProfileForTests();
        std::exit(0);
    }
    int status{};
    expect(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "golden scenario child failed");
    std::ifstream stream(log);
    const std::string text{std::istreambuf_iterator<char>(stream), {}};
    return normalize(text);
}
}

int main(int argc, char** argv) {
    // The gates are read once per process, so no gate may be read before the children fork.
    char directory[] = "/tmp/fgvk-pacing-golden.XXXXXX";
    expect(mkdtemp(directory) != nullptr, "cannot create golden fixture");
    const std::filesystem::path root{directory};
    const std::string captured = runScenario(root, "profile", true) + runScenario(root, "trace", false);
    std::filesystem::remove_all(root);
    if (const char* write = std::getenv("FGVK_PACING_RECORD_GOLDEN_WRITE")) {
        std::ofstream out(write, std::ios::trunc | std::ios::binary);
        expect(out.good(), "cannot write the golden record file");
        out << captured;
        std::cout << "pacing record golden written\n";
        return 0;
    }
    expect(argc == 2, "usage: fgvk-pacing-record-golden-tests <golden-file>");
    std::ifstream golden(argv[1], std::ios::binary);
    expect(golden.is_open(), "cannot open the golden record file");
    const std::string expected{std::istreambuf_iterator<char>(golden), {}};
    if (captured != expected) {
        std::cerr << "FAIL: pacing record text differs from the golden file\n--- captured\n"
            << captured << "--- golden\n" << expected;
        return 1;
    }
    std::cout << "pacing record golden text matches\n";
    return 0;
}

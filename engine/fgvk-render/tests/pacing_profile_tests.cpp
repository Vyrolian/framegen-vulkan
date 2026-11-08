/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "present_diagnostics.hpp"
#include "swapchain/present/internal.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace fgvk::layer::present_diagnostics;
using namespace std::chrono_literals;
namespace {
void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path);
    return {std::istreambuf_iterator<char>(stream), {}};
}
void setupFixture() {
    const char* incomingNames[]{"VK_KHR_swapchain"};
    const char* forwardedNames[]{"VK_KHR_swapchain", "VK_KHR_present_wait2"};
    VkPhysicalDevicePresentWait2FeaturesKHR wait{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR,
        .presentWait2 = VK_TRUE};
    VkDeviceQueueCreateInfo queue{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 3, .queueCount = 2};
    VkDeviceCreateInfo incoming{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = incomingNames};
    auto forwarded = incoming;
    forwarded.pNext = &wait;
    forwarded.enabledExtensionCount = 2;
    forwarded.ppEnabledExtensionNames = forwardedNames;
    traceVulkanSetup("frame-generation", incoming, forwarded, VK_SUCCESS, {}, {});
    expect(incoming.pNext == nullptr && incoming.enabledExtensionCount == 1 &&
        forwarded.pNext == &wait && wait.presentWait2 == VK_TRUE && queue.queueCount == 2,
        "setup observer mutated caller or forwarded inputs");
    // Deliberately cyclic unknown chain: bounded traversal must expose truncation.
    VkBaseInStructure unknown{static_cast<VkStructureType>(1234567), nullptr};
    unknown.pNext = &unknown;
    forwarded.pNext = &unknown;
    std::array<const char*, 257> manyNames;
    manyNames.fill("VK_TEST_extension");
    forwarded.enabledExtensionCount = manyNames.size();
    forwarded.ppEnabledExtensionNames = manyNames.data();
    traceVulkanSetup("frame-generation", incoming, forwarded, VK_ERROR_FEATURE_NOT_PRESENT, {}, {});
    VkSwapchainCreateInfoKHR swap{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .minImageCount = 3, .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
        .imageExtent = {2560, 1440}, .presentMode = VK_PRESENT_MODE_FIFO_KHR};
    auto lower = swap; lower.minImageCount = 6;
    traceVulkanSetup("frame-generation", swap, lower, VK_SUCCESS, {}, {});
    traceVulkanSurface("frame-generation", "vkCreateWaylandSurfaceKHR", {}, {}, VK_SUCCESS);
}
void timingObservations() {
    PacingProfile profile;
    const auto start = Clock::time_point{10s};
    profile.beginSource(start);
    profile.completedWorkerBatch(false);
    profile.completedWorkerBatch(true);
    profile.outputTiming(PacingProfile::ExtTiming, start + 10ms);
    profile.outputTiming(PacingProfile::ExtTiming, start + 15ms);
    profile.outputTiming(PacingProfile::ExtTiming, start + 15ms);
    profile.outputTiming(PacingProfile::ExtTiming, start + 14ms);
    profile.outputTiming(PacingProfile::ApplicationTiming);
    profile.outputTiming(PacingProfile::ExtTiming, start + 100ms);
    profile.outputTiming(PacingProfile::GoogleTiming, start + 200ms);
    profile.outputTiming(PacingProfile::BridgeTiming);
    profile.outputTiming(PacingProfile::DriverTiming);
    const auto window = profile.finishSource(start + 1s);
    expect(window && window->workerBatches == 2 && window->workerReadinessMisses == 1 &&
        window->timingAttempts[PacingProfile::ExtTiming] == 5 &&
        window->timingAttempts[PacingProfile::ApplicationTiming] == 1 &&
        window->timingAttempts[PacingProfile::GoogleTiming] == 1 &&
        window->timingAttempts[PacingProfile::BridgeTiming] == 1 &&
        window->timingAttempts[PacingProfile::DriverTiming] == 1 &&
        window->metrics[PacingProfile::PresentTargetInterval].count == 1 &&
        window->metrics[PacingProfile::PresentTargetInterval].mean() == 5 &&
        window->targetDiscontinuities == 2,
        "requested target spacing must not cross timing owners, untimed outputs or repeated/backwards targets");
    profile.beginSource(start + 1s);
    profile.outputTiming(PacingProfile::ExtTiming, start + 1010ms);
    const auto next = profile.finishSource(start + 2s);
    expect(next && next->workerBatches == 0 && next->workerReadinessMisses == 0 &&
        next->timingAttempts[PacingProfile::ExtTiming] == 1 &&
        next->timingAttempts[PacingProfile::DriverTiming] == 0 &&
        next->targetDiscontinuities == 0 &&
        next->metrics[PacingProfile::PresentTargetInterval].count == 0,
        "timing counters must reset with the window without inventing cross-owner intervals");
    profile.beginSource(start + 2s);
    profile.outputTiming(PacingProfile::ExtTiming, start + 1015ms);
    const auto continued = profile.finishSource(start + 3s);
    expect(continued && continued->metrics[PacingProfile::PresentTargetInterval].count == 1 &&
        continued->metrics[PacingProfile::PresentTargetInterval].mean() == 5,
        "a window boundary must not discard a consecutive target interval");
    PacingProfile other;
    other.beginSource(start);
    other.outputTiming(PacingProfile::ExtTiming, start + 10ms);
    expect(other.finishSource(start + 1s)->metrics[PacingProfile::PresentTargetInterval].count == 0,
        "target intervals must stay swapchain-local");
}
void outputSequence() {
    PacingProfile profile;
    const auto start = Clock::time_point{10s};
    profile.beginSource(start);
    profile.output(start + 2ms, start + 3ms, VK_SUCCESS, {
        .scheduled = start + 1ms, .imageIndex = 4, .generated = true,
        .deferred = true, .copyChecked = true, .copyStatus = VK_NOT_READY, .copySampledAt = start + 3ms});
    profile.output(start + 9ms, start + 10ms, VK_ERROR_OUT_OF_DATE_KHR, {
        .scheduled = start + 9ms, .imageIndex = 2, .deferred = true,
        .copyChecked = true, .copyStatus = VK_SUCCESS, .copySampledAt = start + 10ms});
    const auto window = profile.finishSource(start + 1s);
    expect(window && window->outputTraceCount == 2 && window->outputTraceDropped == 0 &&
        window->outputTrace[0].source == 1 && window->outputTrace[1].source == 1 &&
        window->outputTrace[0].ordinal == 1 && window->outputTrace[1].ordinal == 2 &&
        window->outputTrace[0].observation.generated && !window->outputTrace[1].observation.generated &&
        window->outputTrace[0].observation.copyStatus == VK_NOT_READY &&
        window->outputTrace[1].observation.copyStatus == VK_SUCCESS &&
        window->outputTrace[0].started == start + 2ms &&
        window->outputTrace[1].result == VK_ERROR_OUT_OF_DATE_KHR,
        "output trace lost real/generated order, timestamps, source ownership or observed copy state");
    profile.beginSource(start + 1s);
    for (size_t i = 0; i < PacingProfile::outputTraceCapacity + 7; ++i)
        profile.output(start + 1s + i * 1us, start + 1s + i * 1us, VK_SUCCESS);
    const auto full = profile.finishSource(start + 2s);
    expect(full && full->outputTraceCount == PacingProfile::outputTraceCapacity &&
        full->outputTraceDropped == 7 && full->outputTrace[0].ordinal == 3 &&
        full->outputTrace[0].source == 2 && !full->outputTrace[0].observation.copyChecked,
        "trace overflow was silent, crossed source ownership or invented copy readiness");
    profile.beginSource(start + 2s);
    profile.output(start + 2s, start + 2s, VK_SUCCESS);
    const auto next = profile.finishSource(start + 3s);
    expect(next && next->outputTraceCount == 1 && next->outputTraceDropped == 0 &&
        next->outputTrace[0].ordinal == 3 + PacingProfile::outputTraceCapacity + 7,
        "trace reset concealed omitted outputs or reused an old output ordinal");
}
void sourceIdentity() {
    PacingProfile profile;
    const auto now = Clock::time_point{10s};
    profile.beginSource(now);
    profile.settings(true, false, 2, 60);
    profile.policy(false, 60);
    profile.backendScheduled(0);
    profile.wait(PacingProfile::SourceCapWait, 2ms);
    profile.phaseTiming({5ms, 1ms});
    auto sample = profile.sourceSample(now + 5ms);
    expect(sample.source == 1 && sample.backendScheduled && sample.backendEpoch == 1 &&
        sample.backendFrame == 0 && sample.waits[0] == 2ms && sample.phases[1] == 1ms &&
        sample.frameGeneration && sample.multiplier == 2 && sample.cap == 60,
        "source trace lost actual settings, backend identity or source-local phases");
    profile.beginSource(now + 16ms);
    expect(!profile.sourceSample(now + 20ms).backendScheduled &&
        profile.sourceForBackend(0) == 1 && profile.sourceSample(now + 20ms).waits[0] == 0ms,
        "source trace reused old work or lost the delayed GPU source identity");
    for (size_t frame = 1; frame < 10; ++frame) profile.backendScheduled(frame);
    expect(profile.sourceForBackend(0) == 0 && profile.sourceForBackend(9) == 2,
        "bounded backend mapping invented an overwritten source identity");
    profile.resetBackend();
    expect(profile.sourceForBackend(9) == 0 && profile.backendGeneration() == 2,
        "resource replacement retained previous backend identities");
    profile.backendScheduled(0);
    expect(profile.sourceForBackend(0) == 2, "backend frame zero was confused with unavailable evidence");
}

void detailedTrace() {
    expect(pacingFrameTraceEnabled() && pacingProfileEnabled() && pacingGpuProfileEnabled() && pacingWaylandProfileEnabled() && !enabled(),
        "the full trace must activate its observers independently of broad diagnostics");
    initializePacingProfileLog();
    PacingProfile profile;
    {
        ApplicationPresentScope outer({}, {}, 3);
        profile.beginSource(Clock::now());
        bindApplicationPresentSource(77, 1);
        profile.backendScheduled(0);
        {
            ApplicationPresentScope nested({}, {}, 4);
            bindApplicationPresentSource(88, 2);
        }
        logPacingSource(77, {}, profile.sourceSample(Clock::now()));
        expect(outer.returned(VK_SUBOPTIMAL_KHR) == VK_SUBOPTIMAL_KHR, "present observation changed its result");
    }
    uint32_t index = 19;
    const auto success = observeApplicationAcquire({}, {}, ApplicationFrameApi::AcquireKhr, 100,
        [&] {
            bindApplicationAcquireContext(77);
            expect(observeApplicationAcquireDriver(0, [] { return VK_NOT_READY; },
                reinterpret_cast<const uint32_t*>(1)) == VK_NOT_READY, "lower poll changed result");
            expect(observeApplicationAcquire({}, {}, ApplicationFrameApi::AcquireKhr2, 0, [&] {
                bindApplicationAcquireContext(88);
                return observeApplicationAcquireDriver(0, [] { return VK_TIMEOUT; },
                    reinterpret_cast<const uint32_t*>(1));
            }, reinterpret_cast<const uint32_t*>(1)) == VK_TIMEOUT, "nested acquisition changed result");
            return observeApplicationAcquireDriver(90, [&] { index = 3; return VK_SUCCESS; }, &index);
        }, &index);
    expect(success == VK_SUCCESS && index == 3, "trace changed acquisition output");
    expect(observeApplicationAcquire({}, {}, ApplicationFrameApi::AcquireKhr, 0,
        [] { return VK_TIMEOUT; }, reinterpret_cast<const uint32_t*>(1)) == VK_TIMEOUT,
        "trace read an unavailable acquisition image");
    size_t submissions{};
    std::array<VkSemaphore, 18> binaryWaits{};
    const CopySubmission copy{
        .context = 77, .source = 1, .imageIndex = 3, .waits = binaryWaits,
        .signalValue = 4,
    };
    observeCopySubmission(copy, [&] { ++submissions; });
    try {
        observeCopySubmission(copy, [&] { ++submissions; throw std::runtime_error("submission failure"); });
        expect(false, "copy observer swallowed a submission exception");
    } catch (const std::runtime_error& error) {
        expect(std::string_view(error.what()) == "submission failure", "copy observer changed exception");
    }
    expect(submissions == 2, "copy observer repeated or skipped a submission");
    const auto now = Clock::now();
    recordApplicationPresentWait({}, {}, PresentWaitApi::Khr2, 123, 0, VK_TIMEOUT, now, now);
    const std::array generated{125.0};
    logPacingGpuFrame(77, 1, 1, 0, 1, 2, 3, 125, 250, 1, generated);
    const auto sample = profile.output(now, now, VK_SUCCESS, {
        .imageIndex = 3, .generated = true, .deferred = true, .source = 1,
        .enqueued = now, .queueLockStarted = now, .queueLockAcquired = now,
        .readinessChecked = true, .readinessStarted = now, .readinessFinished = now,
        .readinessDeadline = now, .readinessResult = VK_SUCCESS});
    logPacingOutput(77, {}, sample);
}

void emit() {
    PacingProfile profile;
    const auto start = Clock::time_point{10s};
    profile.beginSource(start);
    profile.policy(true, 60, true);
    profile.settings(true, false, 2, 60);
    profile.wait(PacingProfile::SourceCapWait, 3ms);
    profile.wait(PacingProfile::ReadinessWait, 2ms);
    profile.wait(PacingProfile::OutputWait, 4ms);
    profile.gpuMicroseconds(PacingProfile::GpuPrepass, 2000);
    profile.gpuMicroseconds(PacingProfile::GpuGeneration, 8000);
    profile.gpuMicroseconds(PacingProfile::GpuOutput, 3000);
    profile.gpuMicroseconds(PacingProfile::GpuOutput, 5000);
    profile.gpuMicroseconds(PacingProfile::GpuSpan, 12000);
    profile.gpuMicroseconds(PacingProfile::GpuReadback, 20);
    profile.gpuMicroseconds(PacingProfile::GpuSpan, -1);
    profile.gpuMicroseconds(PacingProfile::GpuSpan, std::numeric_limits<double>::infinity());
    profile.output(start + 1ms, start + 2ms, VK_SUCCESS, {.imageIndex = 4, .generated = true});
    profile.output(start + 9ms, start + 10ms, VK_SUCCESS, {.imageIndex = 2, .copyChecked = true, .copyStatus = VK_SUCCESS, .copySampledAt = start + 10ms});
    {
        ContextScope scope(17, &profile);
        fgvk::layer::present_detail::logSlowPresentBreakdown(17, 1, 1, 5ms, {.renderFence = 3ms});
        ContextScope wrongOwner(18);
        recordPacingProfilePhases(17, {99ms});
    }
    expect(!profile.finishSource(start + 12ms), "short window emitted early");
    profile.publicationCost(1ms);
    profile.beginSource(start + 60ms);
    const auto window = profile.finishSource(start + 1s);
    expect(window && window->fifoCallerTimingSources == 1 && window->worstSource.interval == 60ms &&
        window->frameGenerationEnabled && !window->adaptive && window->configuredMultiplier == 2 && window->configuredCapFps == 60 &&
        window->worstSource.layer == 12ms && window->worstSource.outside == 48ms &&
        window->worstSource.capWait == 3ms && window->worstSource.readinessWait == 2ms &&
        window->worstSource.outputWait == 4ms && window->worstSource.profilerPublish == 1ms &&
        window->worstSource.phases[1] == 3ms && window->phases[1].maximum == 3 &&
        window->metrics[PacingProfile::ProfilerPublish].maximum == 1,
        "hitch lost correlated waits, phase ownership or profiler cost");
    expect(window->metrics[PacingProfile::GpuPrepass].mean() == 2 &&
        window->metrics[PacingProfile::GpuGeneration].mean() == 8 &&
        window->metrics[PacingProfile::GpuOutput].count == 2 &&
        window->metrics[PacingProfile::GpuOutput].mean() == 4 &&
        window->metrics[PacingProfile::GpuSpan].count == 1 &&
        window->metrics[PacingProfile::GpuSpan].mean() == 12 &&
        window->metrics[PacingProfile::GpuReadback].mean() == 0.02,
        "GPU aggregate units, sample rejection or batch/output scopes are incorrect");
    const auto empty = profile.finishSource(start + 2s);
    expect(empty && empty->metrics[PacingProfile::GpuSpan].count == 0,
        "a previous GPU batch was repeated into a later window");
    logPacingProfile(17, VK_PRESENT_MODE_IMMEDIATE_KHR, start + 1s, *window);
    for (size_t i = 0; i < 6000; ++i)
        logPacingProfile(17, VK_PRESENT_MODE_IMMEDIATE_KHR, start + 1s, *window);
    int calls = 0;
    const auto result = observeApplicationAcquire({}, {}, ApplicationFrameApi::AcquireKhr, 0,
        [&] { ++calls; return VK_TIMEOUT; });
    expect(result == VK_TIMEOUT && calls == 1, "profiling changed acquisition forwarding");
    for (auto api : {PresentWaitApi::Khr, PresentWaitApi::Khr2}) {
        const auto waited = observeApplicationPresentWait({}, {}, api, 123, 0,
            [&] { ++calls; return VK_TIMEOUT; });
        expect(waited == VK_TIMEOUT, "lightweight wait capture changed the driver result");
        const auto device = reinterpret_cast<VkDevice>(1);
        recordApplicationPresentWait(device, {}, api, 123, 0, VK_TIMEOUT, start, start + 2ms);
        recordApplicationPresentWait(device, {}, api, 124, 1000, VK_SUCCESS, start + 1s, start + 1s + 3ms);
    }
    expect(calls == 3, "lightweight wait capture changed lower wait call count");
    recordApplicationFrameTiming(ApplicationFrameApi::AcquireKhr, 1, {}, start, start + 2ms, VK_SUCCESS, 0);
    recordApplicationFrameTiming(ApplicationFrameApi::AcquireKhr, 1, {}, start + 1s, start + 1s + 2ms, VK_TIMEOUT, 0);
    recordApplicationFrameTiming(ApplicationFrameApi::Present, 1, {}, start, start + 7ms, {}, {}, 1ms, 5ms);
    recordApplicationFrameTiming(ApplicationFrameApi::Present, 1, {}, start + 1s, start + 1s + 7ms, {}, {}, 1ms, 5ms);
}
}
int main() {
    timingObservations();
    outputSequence();
    sourceIdentity();
    char directory[] = "/tmp/fgvk-pacing-tests.XXXXXX";
    expect(mkdtemp(directory) != nullptr, "cannot create temporary fixture");
    const std::filesystem::path root{directory};
    const auto sentinel = root / "sentinel";
    { std::ofstream stream(sentinel); stream << "preserve"; }
    const auto link = root / "capture-link";
    std::filesystem::create_symlink(sentinel, link);
    const auto fifo = root / "capture-fifo";
    expect(mkfifo(fifo.c_str(), 0600) == 0, "cannot create FIFO fixture");
    for (const auto& path : {link, fifo, sentinel, root / "capture.log"}) {
        const auto child = fork();
        expect(child >= 0, "cannot fork isolated process-start policy");
        if (child == 0) {
            const bool gpuOnly = path == root / "capture.log";
            setenv("FGVK_PACING_PROFILE", gpuOnly ? "0" : "1", 1);
            setenv("FGVK_PACING_PROFILE_GPU", gpuOnly ? "1" : "0", 1);
            setenv("FGVK_PACING_PROFILE_TRACE", "0", 1);
            unsetenv("FGVK_PRESENT_DIAGNOSTICS");
            setenv("FGVK_PACING_PROFILE_LOG", path.c_str(), 1);
            expect(pacingProfileEnabled() && !enabled(), "profiler enabled broad diagnostics");
            expect(pacingGpuProfileEnabled() == gpuOnly, "GPU timing must remain separately opt-in");
            initializePacingProfileLog();
            setupFixture();
            emit();
            std::exit(0);
        }
        int status{};
        expect(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "isolated profiler scenario failed");
    }
    expect(read(sentinel) == "preserve", "capture replaced an existing file or followed a symlink");
    const auto capture = root / "capture.log";
    struct stat status{};
    expect(stat(capture.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600,
        "capture must be private to its owner");
    const auto output = read(capture);
    expect(output.find("pacing-profile-vulkan-setup") == std::string::npos,
        "setup tracing must remain full-trace-only");
    expect(output.find("operation=pacing-profile version=2") != std::string::npos &&
        output.find("frame_generation_enabled=1 adaptive=0 configured_multiplier=2 configured_base_cap_fps=60") != std::string::npos &&
        output.find("gpu_requested=1") != std::string::npos &&
        output.find("gpu_span_mean_ms=12") != std::string::npos &&
        output.find("worst_source_interval_ms=60") != std::string::npos &&
        output.find("worst_source_render_fence_ms=3") != std::string::npos &&
        output.find("worst_source_profiler_publish_ms=1") != std::string::npos &&
        output.find("operation=application-acquire") != std::string::npos &&
        output.find("deferred_join_mean_ms=5 deferred_join_max_ms=5") != std::string::npos &&
        output.find("operation=application-present-wait") != std::string::npos &&
        output.find("api=KHR2 calls=") != std::string::npos &&
        output.find("operation=pacing-profile-output version=1 context=17") != std::string::npos &&
        output.find("generated=1 deferred=0 image_index=4") != std::string::npos &&
        output.find("copy_checked=1 copy_status=0") != std::string::npos &&
        output.find("operation=pacing-profile-log-wrap") != std::string::npos &&
        std::filesystem::file_size(capture) <= 16 * 1024 * 1024,
        "capture lost detailed timing or application acquisition records");
    const auto retained = root / "capture-beginning.log";
    const auto child = fork();
    expect(child >= 0, "cannot fork retention regression");
    if (child == 0) {
        setenv("FGVK_PACING_PROFILE", "0", 1);
        setenv("FGVK_PACING_PROFILE_GPU", "0", 1);
        setenv("FGVK_PACING_PROFILE_WAYLAND", "1", 1);
        setenv("FGVK_PACING_PROFILE_LOG", retained.c_str(), 1);
        initializePacingProfileLog();
        writePacingProfileRecords("framegen-vulkan: present diagnostics: operation=pacing-profile-test phase=fg-on\n");
        writePacingProfileRecords("framegen-vulkan: present diagnostics: operation=pacing-profile-test phase=fg-off\n");
        const std::string filler(1024 * 1024, 'x');
        for (int i = 0; i < 70; ++i) writePacingProfileRecords(filler);
        writePacingProfileRecords("this must not replace earlier evidence\n");
        std::exit(0);
    }
    int childStatus{};
    expect(waitpid(child, &childStatus, 0) == child && WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 0,
        "retention regression child failed");
    const auto beginning = read(retained);
    expect(beginning.starts_with("framegen-vulkan: present diagnostics: operation=pacing-profile-session") &&
        beginning.find("capture_limit_bytes=67108864 retention=stop") != std::string::npos &&
        beginning.find("phase=fg-on") != std::string::npos && beginning.find("phase=fg-off") != std::string::npos &&
        beginning.find("operation=pacing-profile-capture-limit") != std::string::npos &&
        beginning.find("operation=pacing-profile-log-wrap") == std::string::npos &&
        beginning.find("this must not replace") == std::string::npos && beginning.size() <= 64 * 1024 * 1024,
        "Wayland capture overwrote earlier phases, lost its limit marker or exceeded its bound");
    const auto trace = root / "detailed.log";
    const auto traceChild = fork();
    expect(traceChild >= 0, "cannot fork detailed trace regression");
    if (traceChild == 0) {
        setenv("FGVK_PACING_PROFILE_TRACE", "1", 1);
        setenv("FGVK_PACING_PROFILE", "0", 1);
        setenv("FGVK_PACING_PROFILE_GPU", "0", 1);
        setenv("FGVK_PACING_PROFILE_WAYLAND", "0", 1);
        unsetenv("FGVK_PRESENT_DIAGNOSTICS");
        setenv("FGVK_PACING_PROFILE_LOG", trace.c_str(), 1);
        setupFixture();
        detailedTrace();
        std::exit(0);
    }
    expect(waitpid(traceChild, &childStatus, 0) == traceChild && WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 0,
        "detailed trace regression failed");
    const auto details = read(trace);
    expect(details.find("kind=device boundary=incoming result=0") != std::string::npos &&
        details.find("extensions=VK_KHR_swapchain extensions_truncated=0") != std::string::npos &&
        details.find("extensions=VK_KHR_swapchain,VK_KHR_present_wait2") != std::string::npos &&
        details.find("presentWait2=1 queue_count=1 queues=3:2:0 queues_truncated=0") != std::string::npos &&
        details.find("extensions_truncated=1") != std::string::npos &&
        details.find("pnext_truncated=1") != std::string::npos &&
        details.find("kind=device boundary=forwarded result=-8") != std::string::npos &&
        details.find("min_images=3 format=44") != std::string::npos &&
        details.find("min_images=6 format=44") != std::string::npos &&
        details.find("api=vkCreateWaylandSurfaceKHR") != std::string::npos,
        "setup trace lost boundary, enabled features, failure or truncation evidence");
    expect(details.find("renderer_build_id=unavailable") == std::string::npos && details.find("renderer_build_id=") != std::string::npos &&
        details.find("retention=stop wayland_requested=1 frame_trace=1") != std::string::npos &&
        details.find("operation=pacing-profile-source version=1") != std::string::npos &&
        details.find("context=77 source=1") != std::string::npos && details.find("context=88 source=2") != std::string::npos &&
        details.find("api=acquire image_index=3") != std::string::npos &&
        details.find("api=acquire image_index=4294967295") != std::string::npos &&
        details.find("operation=pacing-profile-gpu version=1") != std::string::npos &&
        details.find("operation=pacing-profile-wait version=1") != std::string::npos &&
        details.find("operation=pacing-profile-output version=2") != std::string::npos &&
        details.find("ready_checked=1") != std::string::npos,
        "detailed trace lost short-session evidence, nested identity, build identity or unavailable acquisition");
    expect(details.find("operation=pacing-profile-acquire-driver version=1") != std::string::npos &&
        details.find("attempt=2 timeout_ns=90") != std::string::npos &&
        details.find("lower_calls=2") != std::string::npos &&
        details.find("result_known=1 result=1000001003") != std::string::npos &&
        details.find("caller=") != std::string::npos &&
        details.find("operation=pacing-profile-copy version=1") != std::string::npos &&
        details.find("submitted=0") != std::string::npos &&
        details.find("wait_count=18 wait_recorded=16") != std::string::npos &&
        details.find("wait_16=") == std::string::npos,
        "detailed trace lost acquire partitions, actual return results or bounded submission dependencies");
    std::filesystem::remove_all(root);
    std::cout << "pacing profile tests passed\n";
}

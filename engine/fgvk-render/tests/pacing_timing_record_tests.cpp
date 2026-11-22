/* SPDX-License-Identifier: GPL-3.0-or-later */
// PT4: the text of an output's forwarded timing and of a past presentation timing entry, and the copies behind them: a driver
// entry's written stages and the first timing request of a present's chain. Pure formatting and copying, with no driver and no
// gate read in this process.
#include "present_diagnostics.hpp"
#include "present_records.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

using namespace fgvk::layer;
using namespace fgvk::layer::present_diagnostics;

namespace {
void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

bool endsWith(const std::string& text, const std::string_view suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

VkSwapchainKHR fakeSwapchain() { return reinterpret_cast<VkSwapchainKHR>(uint64_t{0x3000}); }

// The output record keeps the five timing fields last, after every field that was there before PT4.
void outputPrintsTheTimingFieldsLast() {
    present_records::PacingOutputPayload payload{.context = 17, .swapchain = fakeSwapchain()};
    payload.sample.observation.timingTarget = 123456;
    payload.sample.observation.timingFlags = 2;
    payload.sample.observation.timingStage = 8;
    payload.sample.observation.timingDomainId = 7;
    payload.sample.observation.timingKind = 4;
    std::ostringstream timed;
    present_records::formatPacingOutput(payload, timed);
    const std::string text = timed.str();
    expect(text.starts_with("framegen-vulkan: present diagnostics: operation=pacing-profile-output version=1 "),
        "the output record lost its prefix");
    // The kind is a number: a uint8_t printed as a character would show a control byte here.
    expect(endsWith(text, " timing_target_ns=123456 timing_flags=2 timing_stage=8 timing_domain_id=7 timing_kind=4\n"),
        "the output record does not end with the five timing fields in order");
    // Without a request every field is zero, and the record keeps the same order.
    present_records::PacingOutputPayload plain{.context = 17, .swapchain = fakeSwapchain()};
    std::ostringstream untimed;
    present_records::formatPacingOutput(plain, untimed);
    expect(endsWith(untimed.str(), " timing_target_ns=0 timing_flags=0 timing_stage=0 timing_domain_id=0 timing_kind=0\n"),
        "an output without a request does not print zero timing fields");
}

// A past presentation timing entry prints its stages as stage:time pairs, in the order the driver wrote them.
void pastTimingFormatsStages() {
    present_records::PacingPastTimingPayload payload{
        .swapchain = fakeSwapchain(),
        .entry = PastTimingEntry{.lowerPresentId = 5, .appPresentId = 17, .dropped = false, .targetTime = 16'666'666,
            .timeDomain = 1, .timeDomainId = 9, .reportComplete = true, .stageCount = 2,
            .stages = {PastTimingStage{4, 2'000'000}, PastTimingStage{8, 3'000'000}}},
    };
    std::ostringstream stream;
    present_records::formatPacingPastTiming(payload, stream);
    const std::string text = stream.str();
    expect(text.starts_with("framegen-vulkan: present diagnostics: operation=pacing-profile-past-timing version=1 pid="),
        "the past timing record lost its prefix");
    expect(endsWith(text, " swapchain=0x3000 lower_present_id=5 app_present_id=17 dropped=0 target_ns=16666666 time_domain=1 "
        "time_domain_id=9 report_complete=1 stages=4:2000000,8:3000000\n"),
        "the past timing stages are not formatted as stage:time pairs in order");

    // A dropped entry with no stages keeps its fields and prints an empty stage list.
    present_records::PacingPastTimingPayload dropped{
        .swapchain = fakeSwapchain(),
        .entry = PastTimingEntry{.lowerPresentId = 6, .appPresentId = 0, .dropped = true, .targetTime = 0,
            .timeDomain = 1, .timeDomainId = 9, .reportComplete = false, .stageCount = 0},
    };
    std::ostringstream none;
    present_records::formatPacingPastTiming(dropped, none);
    expect(endsWith(none.str(), " swapchain=0x3000 lower_present_id=6 app_present_id=0 dropped=1 target_ns=0 time_domain=1 "
        "time_domain_id=9 report_complete=0 stages=\n"),
        "a dropped entry without stages is not formatted with an empty stage list");
}

#if defined(VK_EXT_present_timing)
// The driver's entry is copied with the stages it wrote, up to four; a missing stage array copies none.
void driverEntryKeepsItsWrittenStages() {
    constexpr auto queue = VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT;
    constexpr auto dequeued = VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT;
    constexpr auto firstOut = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
    constexpr auto visible = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT;
    std::array<VkPresentStageTimeEXT, 4> stages{{{queue, 100}, {dequeued, 200}, {firstOut, 300}, {visible, 400}}};
    VkPastPresentationTimingEXT entry{VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT, nullptr, 42, 999, 2, stages.data(),
        VK_TIME_DOMAIN_DEVICE_KHR, 7, VK_TRUE};
    const auto two = pastTimingEntry(entry);
    expect(two.lowerPresentId == 42 && two.targetTime == 999 && two.timeDomainId == 7 && two.reportComplete &&
        two.stageCount == 2 && two.stages[0].stage == queue && two.stages[0].time == 100 &&
        two.stages[1].stage == dequeued && two.stages[1].time == 200,
        "the driver entry did not keep its scalars and its written stages");

    // A count above the four stages the entry can hold keeps the first four, never the array's tail beyond them.
    entry.presentStageCount = 6;
    const auto capped = pastTimingEntry(entry);
    expect(capped.stageCount == 4 && capped.stages[3].stage == visible && capped.stages[3].time == 400,
        "the stage count was not capped to the four stage flags");

    entry.pPresentStages = nullptr;
    expect(pastTimingEntry(entry).stageCount == 0, "a missing stage array was read");
}

// The chain's first VkPresentTimingsInfoEXT request is read; a chain without one, or with none on the swapchain, is not.
void forwardedRequestIsTheFirstInTheChain() {
    VkPresentTimingInfoEXT timing{
        .sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT, .pNext = nullptr,
        .flags = VK_PRESENT_TIMING_INFO_PRESENT_AT_NEAREST_REFRESH_CYCLE_BIT_EXT, .targetTime = 777,
        .timeDomainId = 5, .presentStageQueries = 0, .targetTimeDomainPresentStage = 8,
    };
    VkPresentTimingsInfoEXT timings{.sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT, .pNext = nullptr,
        .swapchainCount = 1, .pTimingInfos = &timing};
    const uint64_t lowerId = 3;
    VkPresentIdKHR ids{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .pNext = &timings, .swapchainCount = 1, .pPresentIds = &lowerId};
    VkPresentTimingInfoEXT found{};
    expect(forwardedPresentTiming(&ids, found), "the request behind a present ID node was not found");
    expect(found.targetTime == 777 && found.flags == VK_PRESENT_TIMING_INFO_PRESENT_AT_NEAREST_REFRESH_CYCLE_BIT_EXT &&
        found.timeDomainId == 5 && found.targetTimeDomainPresentStage == 8,
        "the forwarded request was not copied whole");

    VkPresentTimingInfoEXT untouched{};
    expect(!forwardedPresentTiming(nullptr, untouched), "a null chain has a request");
    VkPresentIdKHR plain{.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .pNext = nullptr, .swapchainCount = 1,
        .pPresentIds = &lowerId};
    expect(!forwardedPresentTiming(&plain, untouched), "a chain without a timing node has a request");
    timings.swapchainCount = 0;
    expect(!forwardedPresentTiming(&ids, untouched) && untouched.targetTime == 0,
        "a timing node with no swapchain was read");
}
#endif
}

int main() {
    outputPrintsTheTimingFieldsLast();
    pastTimingFormatsStages();
#if defined(VK_EXT_present_timing)
    driverEntryKeepsItsWrittenStages();
    forwardedRequestIsTheFirstInTheChain();
#endif
    std::cout << "pacing timing record tests passed\n";
    return 0;
}

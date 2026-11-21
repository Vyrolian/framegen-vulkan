/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/reflex_present_attribution.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fgvk::layer;

namespace {
void expect(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

template<class T> T handle(uintptr_t value) { return reinterpret_cast<T>(value); }

const auto device = handle<VkDevice>(0x5100);
const auto swapchain = handle<VkSwapchainKHR>(0x5200);

// One thing that happened, in order: a driver marker or a lower present.
struct Event {
    std::string what;
    uint64_t presentId{};
    VkLatencyMarkerNV marker{};
    bool operator==(const Event& other) const {
        return what == other.what && presentId == other.presentId && marker == other.marker;
    }
};
std::vector<Event> events;

void VKAPI_CALL fakeMark(VkDevice markDevice, VkSwapchainKHR markSwapchain,
        const VkSetLatencyMarkerInfoNV* info) {
    expect(markDevice == device && markSwapchain == swapchain, "a marker went to the wrong device or swapchain");
    expect(info && info->sType == VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV && info->pNext == nullptr,
        "a marker had the wrong structure");
    events.push_back({"mark", info->presentID, info->marker});
}

void fakeRecord(uint64_t presentId, VkLatencyMarkerNV marker) noexcept {
    events.push_back({"record", presentId, marker});
}

VkResult fakePresent() {
    events.push_back({"present"});
    return VK_SUCCESS;
}

// The rule of the application's markers: the out-of-band pair is kept, every other marker is forwarded.
void markerForwarding() {
    for (uint32_t value = 0; value <= VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV; ++value) {
        const auto marker = static_cast<VkLatencyMarkerNV>(value);
        ReflexPresentAttribution attribution;
        const bool kept = marker == VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV ||
            marker == VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV;
        expect(attribution.applicationMarker(marker, 5) == !kept,
            "an application marker was forwarded or kept against the rule");
    }
}

// An open out-of-band ID is the answer of every present until its end, whatever the presents do.
void openOutOfBandWinsUntilEnd() {
    ReflexPresentAttribution attribution;
    expect(!attribution.applicationMarker(VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV, 5),
        "an out-of-band start was forwarded");
    expect(attribution.takeApplicationPresent() == 5 && attribution.takeApplicationPresent() == 5 &&
        attribution.takeApplicationPresent() == 5, "an open out-of-band ID was not returned for every present");
    expect(!attribution.applicationMarker(VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV, 5),
        "an out-of-band end was forwarded");
    expect(attribution.takeApplicationPresent() == 0,
        "a present after the out-of-band end was attributed to the closed ID");
}

// An open out-of-band pair takes its present before a newer PRESENT_START, which waits unmapped for the next present.
void openOutOfBandWinsOverPresentStart() {
    ReflexPresentAttribution attribution;
    expect(!attribution.applicationMarker(VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV, 5),
        "an out-of-band start was forwarded");
    expect(attribution.applicationMarker(VK_LATENCY_MARKER_PRESENT_START_NV, 9),
        "a PRESENT_START was not forwarded");
    expect(attribution.takeApplicationPresent() == 5, "a newer PRESENT_START beat the open out-of-band ID");
    expect(!attribution.applicationMarker(VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV, 5),
        "an out-of-band end was forwarded");
    expect(attribution.takeApplicationPresent() == 9,
        "the PRESENT_START that the open pair shadowed was not mapped by the next present");
    expect(attribution.takeApplicationPresent() == 0, "a PRESENT_START was mapped twice");
}

// Without an open pair, a PRESENT_START is returned once, to the next present, and then nothing.
void presentStartIsMappedOnce() {
    ReflexPresentAttribution attribution;
    expect(attribution.applicationMarker(VK_LATENCY_MARKER_PRESENT_START_NV, 6),
        "a PRESENT_START was not forwarded");
    expect(attribution.takeApplicationPresent() == 6, "a fresh PRESENT_START was not returned");
    expect(attribution.takeApplicationPresent() == 0, "a mapped PRESENT_START was returned again");
    expect(attribution.applicationMarker(VK_LATENCY_MARKER_PRESENT_START_NV, 7),
        "a second PRESENT_START was not forwarded");
    expect(attribution.takeApplicationPresent() == 7, "a second PRESENT_START was not returned");
}

void noMarkersGivesNoFrame() {
    ReflexPresentAttribution attribution;
    expect(attribution.takeApplicationPresent() == 0, "a present without markers was attributed");
    expect(attribution.takeApplicationPresent() == 0, "a second present without markers was attributed");
}

// The registry makes an entry only with get, and forgets it at erase.
void registryFindNeverCreates() {
    const auto key = handle<VkSwapchainKHR>(0x5300);
    expect(ReflexPresentAttributionRegistry::find(key) == nullptr, "find created an entry");
    const auto made = ReflexPresentAttributionRegistry::get(key);
    expect(made != nullptr, "get returned no entry");
    expect(ReflexPresentAttributionRegistry::find(key) == made, "find did not return the entry get made");
    expect(ReflexPresentAttributionRegistry::get(key) == made, "get made a second entry");
    ReflexPresentAttributionRegistry::erase(key);
    expect(ReflexPresentAttributionRegistry::find(key) == nullptr, "erase left the entry behind");
}

// With a driver function, the wrapped present is exactly 10(id), present, 11(id), with the device and swapchain.
void attributedPresentBracketsThePresent() {
    events.clear();
    const ReflexMarkerSink sink{.device = device, .mark = fakeMark, .record = fakeRecord};
    const auto result = attributedPresent(sink, swapchain, 7, [] {
        return fakePresent();
    });
    expect(result == VK_SUCCESS, "the present result was lost");
    const std::vector<Event> expected{
        {"mark", 7, VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV},
        {"record", 7, VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV},
        {"present"},
        {"mark", 7, VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV},
        {"record", 7, VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV},
    };
    expect(events == expected, "the wrapped present was not 10(7), present, 11(7)");
}

// Nothing is sent for ID 0, nor without a driver function; the present still runs and returns its result.
void attributedPresentWithoutAMarkerIsPlain() {
    events.clear();
    const ReflexMarkerSink withMarkers{.device = device, .mark = fakeMark, .record = fakeRecord};
    const ReflexMarkerSink withoutFunction{.device = device, .mark = nullptr, .record = fakeRecord};
    const auto zero = attributedPresent(withMarkers, swapchain, 0, [] { return fakePresent(); });
    const auto unbound = attributedPresent(withoutFunction, swapchain, 7, [] { return fakePresent(); });
    expect(zero == VK_SUCCESS && unbound == VK_SUCCESS, "a plain present lost its result");
    expect(events == std::vector<Event>{{"present"}, {"present"}},
        "a marker was sent for ID 0 or without a driver function");
}

// The end marker follows a present that throws, and the exception still reaches the caller.
void attributedPresentEndsWhenPresentThrows() {
    events.clear();
    const ReflexMarkerSink sink{.device = device, .mark = fakeMark, .record = nullptr};
    bool thrown = false;
    try {
        static_cast<void>(attributedPresent(sink, swapchain, 7, []() -> VkResult {
            events.push_back({"present"});
            throw std::runtime_error("lower present failed");
        }));
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    expect(thrown, "the present's exception was swallowed");
    const std::vector<Event> expected{
        {"mark", 7, VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV},
        {"present"},
        {"mark", 7, VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV},
    };
    expect(events == expected, "the out-of-band end was not sent after a throwing present");
}

// The timing query is due on every 16th sleep: calls 1 to 15 are not, 16 is, 17 to 31 are not, and 32 is.
void timingQueryIsDueEverySixteenthCall() {
    ReflexTimingCursor cursor;
    for (uint32_t call = 1; call <= 32; ++call)
        expect(cursor.due() == (call == 16 || call == 32), "the timing query was not due on every 16th sleep");
}

// RT1: the queue drain reads on every fourth sleep: calls 1 to 3 are not due, 4 is, 5 to 7 are not, and 8 is.
void timingQueryIsDueEveryFourthCallForTheDrain() {
    ReflexTimingCursor cursor;
    for (uint32_t call = 1; call <= 8; ++call)
        expect(cursor.due(4) == (call == 4 || call == 8), "the timing query was not due on every fourth sleep");
}

// A report is fresh once; an equal or older present ID is not fresh, and a newer one is.
void timingReportsAreFreshOnlyWhenNewer() {
    ReflexTimingCursor cursor;
    expect(cursor.fresh(5), "the first report was not fresh");
    expect(!cursor.fresh(5), "a repeated report was fresh");
    expect(!cursor.fresh(3), "an older report was fresh");
    expect(cursor.fresh(6), "a newer report was not fresh");
}
}

int main() {
    markerForwarding();
    openOutOfBandWinsUntilEnd();
    openOutOfBandWinsOverPresentStart();
    presentStartIsMappedOnce();
    noMarkersGivesNoFrame();
    registryFindNeverCreates();
    attributedPresentBracketsThePresent();
    attributedPresentWithoutAMarkerIsPlain();
    attributedPresentEndsWhenPresentThrows();
    timingQueryIsDueEverySixteenthCall();
    timingQueryIsDueEveryFourthCallForTheDrain();
    timingReportsAreFreshOnlyWhenNewer();
    std::cout << "reflex present attribution tests passed\n";
}

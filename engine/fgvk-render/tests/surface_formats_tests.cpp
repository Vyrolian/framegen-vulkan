/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "surface_formats.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>

using namespace fgvk::layer;

namespace {
void expect(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

// VK_FORMAT_B8G8R8A8_UNORM (44), VK_FORMAT_R16G16B16A16_SFLOAT (97),
// VK_COLOR_SPACE_SRGB_NONLINEAR_KHR (0), VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT (1000104002),
// VK_COLOR_SPACE_HDR10_ST2084_EXT (1000104008).
const std::array<SurfaceFormatPair, 2> offered{
    SurfaceFormatPair{44, 0}, SurfaceFormatPair{97, 1000104002}};

void hashing() {
    const std::array<SurfaceFormatPair, 2> same{
        SurfaceFormatPair{44, 0}, SurfaceFormatPair{97, 1000104002}};
    const std::array<SurfaceFormatPair, 2> reordered{offered[1], offered[0]};
    const std::array<SurfaceFormatPair, 2> colourChanged{
        SurfaceFormatPair{44, 0}, SurfaceFormatPair{97, 1000104008}};
    const std::array<SurfaceFormatPair, 1> prefix{offered[0]};
    const std::span<const SurfaceFormatPair> empty{};

    expect(surfaceFormatListHash(offered) == surfaceFormatListHash(same),
        "identical lists did not share a key");
    expect(surfaceFormatListHash(offered) != surfaceFormatListHash(reordered),
        "pair order did not change the key");
    expect(surfaceFormatListHash(offered) != surfaceFormatListHash(colourChanged),
        "a colour space change did not change the key");
    expect(surfaceFormatListHash(offered) != surfaceFormatListHash(prefix),
        "a list and its prefix shared a key");
    expect(surfaceFormatListHash(empty) != surfaceFormatListHash(prefix),
        "an empty list shared a key with a one-pair list");

    // The streaming key is what the wrappers compute from the driver's array; it must match.
    SurfaceFormatListKey streamed(2);
    streamed.add(offered[0]);
    streamed.add(offered[1]);
    expect(streamed.value() == surfaceFormatListHash(offered),
        "the streaming key differs from the whole-list key");
}

void formatting() {
    expect(surfaceFormatListLine(offered) == "count=2 pairs=44:0,97:1000104002",
        "a two-pair list did not format as count and pairs");
    const std::array<SurfaceFormatPair, 1> single{SurfaceFormatPair{44, 0}};
    expect(surfaceFormatListLine(single) == "count=1 pairs=44:0",
        "a one-pair list did not format without a separator");
    expect(surfaceFormatListLine(std::span<const SurfaceFormatPair>{}) == "count=0 pairs=",
        "an empty list did not format as count=0 with no pairs");
}

void logPolicy() {
    expect(logsSurfaceFormatQuery(VK_SUCCESS, true),
        "a complete list was not logged");
    expect(!logsSurfaceFormatQuery(VK_SUCCESS, false),
        "a count-only query was logged");
    expect(!logsSurfaceFormatQuery(VK_INCOMPLETE, true),
        "a truncated VK_INCOMPLETE list was logged");
    expect(!logsSurfaceFormatQuery(VK_ERROR_SURFACE_LOST_KHR, true),
        "a failed query was logged");
}

void dedupe() {
    SurfaceFormatListLog log;
    expect(log.claim(1), "the first list was not claimed");
    expect(!log.claim(1), "a repeated list was claimed again");
    expect(log.claim(2), "a second distinct list was not claimed");
    for (uint64_t hash = 3; hash <= maxLoggedSurfaceFormatLists; ++hash)
        expect(log.claim(hash), "a distinct list under the cap was refused");
    expect(!log.claim(maxLoggedSurfaceFormatLists + 1),
        "a list past the per-process cap was claimed");
    expect(!log.claim(1), "an earlier list was claimed after the cap");

    // A zero key is an ordinary key: the log tracks its entries by count, not by sentinel.
    SurfaceFormatListLog zero;
    expect(zero.claim(0), "a zero key was not claimed");
    expect(!zero.claim(0), "a repeated zero key was claimed again");
}
} // namespace

int main() {
    hashing();
    formatting();
    logPolicy();
    dedupe();
    std::cout << "surface format tests passed\n";
    return 0;
}

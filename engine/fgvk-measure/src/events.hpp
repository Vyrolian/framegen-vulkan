/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace fgvk::measure {

enum class EventKind : uint8_t { Present, Gpu, Presented, Discarded, Clock };

// One record. It is trivially copyable so the game thread can queue it without formatting. Only the
// fields of its kind are meaningful.
struct Event {
    EventKind kind{EventKind::Present};
    // Present
    uint8_t measured{};
    uint8_t gpuUnavailable{}; // A gpu line with available=0 follows the present line.
    int32_t result{};
    uint32_t image{};
    uint32_t waits{};
    uint64_t swapchain{};
    uint64_t callStartNs{};
    uint64_t callEndNs{};
    // Gpu
    uint8_t available{};
    uint64_t gpuStartNs{};
    uint64_t gpuEndNs{};
    // Presented
    uint64_t presentedNs{};
    uint32_t refreshNs{};
    uint32_t flags{};
    uint64_t msc{};
    // Clock
    uint32_t clockId{};
    // Every kind carries the present sequence number.
    uint64_t seq{};
};

struct Summary {
    uint64_t presents{};
    uint64_t measured{};
    uint64_t unmeasured{};
    uint64_t feedbacks{};
    uint64_t presented{};
    uint64_t discarded{};
};

// Each function appends whole lines, each ending in a newline, with the prefix
// `framegen-vulkan: measure: `.
void appendEvent(std::string& out, const Event& event);
void appendSession(std::string& out, int64_t pid, std::string_view device, bool calibrated);
void appendSummary(std::string& out, const Summary& summary);

}  // namespace fgvk::measure

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "events.hpp"

#include <string>

namespace fgvk::measure {
namespace {

constexpr std::string_view kPrefix = "framegen-vulkan: measure: ";

void unsignedField(std::string& out, std::string_view name, uint64_t value) {
    out += ' ';
    out += name;
    out += '=';
    out += std::to_string(value);
}

void signedField(std::string& out, std::string_view name, int64_t value) {
    out += ' ';
    out += name;
    out += '=';
    out += std::to_string(value);
}

void appendGpu(std::string& out, uint64_t seq, bool available, uint64_t startNs, uint64_t endNs) {
    out += kPrefix;
    out += "gpu";
    unsignedField(out, "seq", seq);
    unsignedField(out, "available", available ? 1 : 0);
    unsignedField(out, "gpu_start_ns", startNs);
    unsignedField(out, "gpu_end_ns", endNs);
    out += '\n';
}

}  // namespace

void appendEvent(std::string& out, const Event& event) {
    switch (event.kind) {
    case EventKind::Present:
        out += kPrefix;
        out += "present";
        unsignedField(out, "seq", event.seq);
        unsignedField(out, "swapchain", event.swapchain);
        unsignedField(out, "image", event.image);
        unsignedField(out, "waits", event.waits);
        unsignedField(out, "call_start_ns", event.callStartNs);
        unsignedField(out, "call_end_ns", event.callEndNs);
        signedField(out, "result", event.result);
        unsignedField(out, "measured", event.measured ? 1 : 0);
        out += '\n';
        if (event.gpuUnavailable) appendGpu(out, event.seq, false, 0, 0);
        return;
    case EventKind::Gpu:
        appendGpu(out, event.seq, event.available != 0, event.gpuStartNs, event.gpuEndNs);
        return;
    case EventKind::Presented:
        out += kPrefix;
        out += "presented";
        unsignedField(out, "seq", event.seq);
        unsignedField(out, "presented_ns", event.presentedNs);
        unsignedField(out, "refresh_ns", event.refreshNs);
        unsignedField(out, "flags", event.flags);
        unsignedField(out, "msc", event.msc);
        out += '\n';
        return;
    case EventKind::Discarded:
        out += kPrefix;
        out += "discarded";
        unsignedField(out, "seq", event.seq);
        out += '\n';
        return;
    case EventKind::Clock:
        out += kPrefix;
        out += "clock";
        unsignedField(out, "clock_id", event.clockId);
        out += '\n';
        return;
    }
}

void appendSession(std::string& out, int64_t pid, std::string_view device, bool calibrated) {
    out += kPrefix;
    out += "session";
    signedField(out, "pid", pid);
    out += " device=";
    out += device;
    out += " calibrated=";
    out += calibrated ? "1" : "0";
    out += '\n';
}

void appendSummary(std::string& out, const Summary& summary) {
    out += kPrefix;
    out += "summary";
    unsignedField(out, "presents", summary.presents);
    unsignedField(out, "measured", summary.measured);
    unsignedField(out, "unmeasured", summary.unmeasured);
    unsignedField(out, "feedbacks", summary.feedbacks);
    unsignedField(out, "presented", summary.presented);
    unsignedField(out, "discarded", summary.discarded);
    out += '\n';
}

}  // namespace fgvk::measure

/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "swapchain/present_marker.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace fgvk::layer {

// RP1: the real present lead. A real output after a generated output of its batch is presented earlier by `lead`, so
// the GPU spacing of the pair stays even while the real is not delayed. The lead converges to the real's extra GPU
// wait: a real that moved earlier by `lead` waits `lead` less on the GPU, so the spacing error falls to zero as the lead
// reaches that wait. Worker thread only.
struct RealPresentLead {
    using Duration = std::chrono::nanoseconds;
    static constexpr Duration limit = std::chrono::microseconds{2500};
    static constexpr Duration errorClamp = std::chrono::milliseconds{1};
    static constexpr Duration errorReject = std::chrono::milliseconds{3};
    static constexpr int64_t gain = 8;
    // The present lead of a real output, from 0 up to limit.
    Duration lead{};

    // One worker present, in present order: its context and source, whether it was generated, its actual start and its
    // unmoved deadline (both steady-clock nanoseconds). A generated output's unmoved deadline is not used.
    void call(uint64_t context, uint64_t source, bool generated, Duration startedNs, Duration unmovedNs) noexcept {
        calls[next] = CallRecord{context, source, generated, true, startedNs, unmovedNs};
        next = (next + 1) % calls.size();
    }

    // One finished marker, in present order (raw GPU nanoseconds). A generated marker becomes the last generated marker of
    // its context. A real marker of the same context and source as that marker, with both call records present, gives one
    // error sample: the GPU spacing of the pair less the planned spacing (the real's unmoved deadline less the generated
    // output's actual start). A sample beyond errorReject is dropped. Otherwise the lead moves by the clamped error over
    // gain and stays within [0, limit]. Returns whether the lead moved. Any real marker clears the last generated marker.
    bool observe(const PresentMarkerSample& sample) noexcept {
        if (sample.generated) {
            generated = Marker{sample.context, sample.source, sample.gpuNs, true};
            return false;
        }
        const Marker last = std::exchange(generated, Marker{});
        if (!last.valid || last.context != sample.context || last.source != sample.source) return false;
        const CallRecord* const generatedCall = find(sample.context, sample.source, true);
        const CallRecord* const realCall = find(sample.context, sample.source, false);
        if (!generatedCall || !realCall) return false;
        const Duration error = Duration{sample.gpuNs - last.gpuNs} - (realCall->unmovedNs - generatedCall->startedNs);
        if (error > errorReject || error < -errorReject) return false;
        lead = std::clamp(lead + std::clamp(error, -errorClamp, errorClamp) / gain, Duration{}, limit);
        return true;
    }

    // Drops the pending markers and the call records. The lead is kept.
    void forget() noexcept {
        calls = {};
        next = 0;
        generated = {};
    }
    // Drops everything, and zeroes the lead.
    void reset() noexcept {
        forget();
        lead = {};
    }

private:
    struct CallRecord {
        uint64_t context{};
        uint64_t source{};
        bool generated{};
        bool valid{};
        Duration startedNs{};
        Duration unmovedNs{};
    };
    struct Marker {
        uint64_t context{};
        uint64_t source{};
        int64_t gpuNs{};
        bool valid{};
    };
    // The newest call record of this context, source and kind, or null.
    [[nodiscard]] const CallRecord* find(const uint64_t context, const uint64_t source, const bool generatedKind) const noexcept {
        for (size_t back = 1; back <= calls.size(); ++back) {
            const CallRecord& record = calls[(next + calls.size() - back) % calls.size()];
            if (record.valid && record.context == context && record.source == source && record.generated == generatedKind)
                return &record;
        }
        return nullptr;
    }
    std::array<CallRecord, 16> calls{};
    size_t next{};
    Marker generated{};
};

// FGVK_REAL_PRESENT_LEAD=1 turns the real present lead on and =0 turns it off. Any other value, or unset, leaves the
// [global] real_present_lead setting in charge. Read once.
[[nodiscard]] inline int realPresentLeadEnvironment() noexcept {
    static const int fromEnvironment = [] {
        const char* text = std::getenv("FGVK_REAL_PRESENT_LEAD");
        if (!text) return -1;
        if (std::strcmp(text, "1") == 0) return 1;
        if (std::strcmp(text, "0") == 0) return 0;
        return -1;
    }();
    return fromEnvironment;
}

// The low bit is the setting. The whole value lets a worker detect even a pair of changes between batches.
inline std::atomic<uint64_t> realPresentLeadConfiguration{0};
inline void setRealPresentLeadConfigured(const bool enabled) noexcept {
    auto state = realPresentLeadConfiguration.load(std::memory_order_relaxed);
    while ((state & 1) != enabled && !realPresentLeadConfiguration.compare_exchange_weak(
            state, state + 1, std::memory_order_relaxed)) {}
}

[[nodiscard]] inline uint64_t realPresentLeadState() noexcept {
    const auto override = realPresentLeadEnvironment();
    return override < 0 ? realPresentLeadConfiguration.load(std::memory_order_relaxed)
                        : static_cast<uint64_t>(override);
}

// Diagnostics only: the last worker's effective lead setting (on, and markers available) and its lead in nanoseconds.
inline std::atomic<bool> realPresentLeadOn{false};
inline std::atomic<int64_t> realPresentLeadNs{0};

}

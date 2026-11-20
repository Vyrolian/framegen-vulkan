/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "screen_samples.hpp"

namespace fgvk::layer {

[[nodiscard]] inline bool predictivePacingEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("FGVK_PREDICTIVE_PACING");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

// Worker-local, bounded histories. The fit uses native Wayland feedback delivered by readiness,
// with IMMEDIATE, 2x, Batch Hold off, and hardware-timed, unsynchronized zero-copy feedback.
class PredictivePacing {
public:
    void observe(const ScreenSample& sample) noexcept {
        constexpr uint32_t asynchronousDirectFeedback = 0xE;
        if (sample.flags != asynchronousDirectFeedback || sample.startedNs <= 0 ||
                sample.screenNs < sample.startedNs || sample.receivedNs < sample.screenNs) {
            generated = {};
            real = {};
            return;
        }
        auto& history = sample.generated ? generated : real;
        if (sample.receivedNs - history.received > maxAge) history = {};
        history.delays.push(sample.screenNs - sample.startedNs);
        history.received = sample.receivedNs;
    }

    [[nodiscard]] std::chrono::nanoseconds advance(const uint64_t source, const int64_t arrival,
            const int64_t ready) noexcept {
        if (lastSource == 0 || source != lastSource + 1 || arrival <= lastArrival || arrival - lastArrival > maxAge)
            intervals = {};
        else
            intervals.push(arrival - lastArrival);
        lastSource = source;
        lastArrival = arrival;

        if (!intervals.full() || !generated.delays.full() || !real.delays.full() ||
                ready < arrival || ready - arrival > maxAge ||
                !fresh(generated, ready) || !fresh(real, ready))
            return {};

        // Ridge fit of signed midpoint error in milliseconds on the first half of the
        // 2026-11-20 Cyberpunk staging capture. Clip standardized inputs to the fit's range.
        const auto z = [](const double ns, const double meanMs, const double scaleMs) {
            return std::clamp((ns / 1'000'000.0 - meanMs) / scaleMs, -5.0, 5.0);
        };
        const double errorMs = 0.241510
            + 0.060016 * z(intervals.latest(), 13.400962, 2.318369)
            - 0.080708 * z(intervals.median(), 13.290946, 0.879177)
            + 0.105609 * z(ready - arrival, 14.343906, 2.988430)
            + 1.279837 * z(ready - generated.received, 6.482132, 2.668543)
            - 0.305956 * z(ready - real.received, 13.126813, 2.065892)
            + 0.251016 * z(generated.delays.median() - real.delays.median(), 0.233668, 0.490685);
        return std::chrono::nanoseconds{static_cast<int64_t>(std::clamp(errorMs, 0.0, 1.0) * 1'000'000)};
    }

private:
    static constexpr int64_t maxAge = 100'000'000;

    struct Window {
        std::array<int64_t, 8> samples{};
        size_t next{}, size{};

        void push(const int64_t value) noexcept {
            samples[next] = value;
            next = (next + 1) % samples.size();
            size = std::min(size + 1, samples.size());
        }
        [[nodiscard]] bool full() const noexcept { return size == samples.size(); }
        [[nodiscard]] int64_t latest() const noexcept {
            return samples[(next + samples.size() - 1) % samples.size()];
        }
        [[nodiscard]] double median() const noexcept {
            auto ordered = samples;
            std::sort(ordered.begin(), ordered.end());
            return (ordered[3] + ordered[4]) / 2.0;
        }
    };
    struct Feedback {
        Window delays;
        int64_t received{};
    };

    [[nodiscard]] static bool fresh(const Feedback& history, const int64_t now) noexcept {
        return history.received <= now && now - history.received <= maxAge;
    }

    Window intervals;
    Feedback generated, real;
    uint64_t lastSource{};
    int64_t lastArrival{};
};

} // namespace fgvk::layer

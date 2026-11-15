/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace fgvk::layer {

// A log-only check for a completion spiral. The spiral is a source period that keeps
// growing while the application's wait on our own delivery grows with it, so each frame
// waits longer for the previous output. Pacing already subtracts the measured wait from the
// source's work; this class only reports the pattern. It reads the learned source period and
// each source's completion-pacing wait, and changes no pacing.
class CompletionSpiralDetector {
public:
    using Clock = std::chrono::steady_clock;

    enum class Event : uint8_t { none, suspected, cleared };
    struct Report {
        Event event{Event::none};
        // Values of the window that produced the event: learned source period, mean
        // completion wait per source of that window, and the consecutive windows that met the rule.
        double periodMs{};
        double meanCompletionWaitMs{};
        size_t windows{};
    };

    static constexpr double growthRatio = 1.10;
    static constexpr size_t requiredWindows = 3;
    static constexpr Clock::duration windowLength = std::chrono::seconds{1};
    // The same gap the source pacer treats as an interruption.
    static constexpr Clock::duration pauseGap = std::chrono::milliseconds{250};

    // The rule, for one window against the previous one. The window's summed completion wait
    // is divided by its sources, so frame rate changes do not change the comparison. A window
    // meets the rule when the learned period grew by more than 10%, the mean wait per source
    // grew, and that mean covers at least half of the period's growth. Three consecutive
    // windows that meet it report `suspected` once per episode. The first window that does
    // not meet it reports `cleared` when an episode was active. A window with no sources is
    // no evidence and leaves the state unchanged.
    [[nodiscard]] Report observeWindow(double periodMs, double summedCompletionWaitMs, size_t sources) {
        if (sources == 0) return {};
        const double meanWaitMs = summedCompletionWaitMs / static_cast<double>(sources);
        const bool growing = this->haveReference && this->previousPeriodMs > 0.0 &&
            periodMs > growthRatio * this->previousPeriodMs &&
            meanWaitMs > this->previousMeanWaitMs &&
            meanWaitMs >= 0.5 * (periodMs - this->previousPeriodMs);
        this->haveReference = true;
        this->previousPeriodMs = periodMs;
        this->previousMeanWaitMs = meanWaitMs;
        if (!growing) {
            this->streak = 0;
            if (!this->suspected) return {};
            this->suspected = false;
            return {Event::cleared, periodMs, meanWaitMs, 0};
        }
        if (++this->streak < requiredWindows || this->suspected) return {};
        this->suspected = true;
        return {Event::suspected, periodMs, meanWaitMs, this->streak};
    }

    // One source present with its completion-pacing wait and the learned source period.
    // Sources are grouped into windows of one second. A window closes on the first source
    // at least one second after it opened, and that source opens the next window. A gap longer
    // than pauseGap restarts the windows and the rule, and ends an active episode.
    [[nodiscard]] Report observeSource(const Clock::time_point now, const Clock::duration completionWait,
            const Clock::duration sourcePeriod) {
        Report report{};
        if (!this->windowStart || now - *this->lastSource > pauseGap) {
            if (this->suspected) {
                this->suspected = false;
                report = {Event::cleared, this->previousPeriodMs, this->previousMeanWaitMs, 0};
            }
            this->haveReference = false;
            this->streak = 0;
            this->windowStart = now;
            this->windowWaitMs = 0.0;
            this->windowSources = 0;
        } else if (now - *this->windowStart >= windowLength) {
            report = observeWindow(this->windowPeriodMs, this->windowWaitMs, this->windowSources);
            this->windowStart = now;
            this->windowWaitMs = 0.0;
            this->windowSources = 0;
        }
        this->lastSource = now;
        this->windowPeriodMs = milliseconds(sourcePeriod);
        this->windowWaitMs += milliseconds(completionWait);
        ++this->windowSources;
        return report;
    }

    [[nodiscard]] bool active() const noexcept { return this->suspected; }

private:
    [[nodiscard]] static double milliseconds(Clock::duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    }

    std::optional<Clock::time_point> windowStart, lastSource;
    double windowPeriodMs{}, windowWaitMs{};
    size_t windowSources{};
    bool haveReference{};
    double previousPeriodMs{}, previousMeanWaitMs{};
    size_t streak{};
    bool suspected{};
};

} // namespace fgvk::layer

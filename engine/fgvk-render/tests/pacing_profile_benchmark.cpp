/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "present_diagnostics.hpp"
#include "swapchain/present/internal.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <iostream>

using namespace fgvk::layer::present_diagnostics;
using namespace std::chrono_literals;

// Observer/publication CPU cost only: no game, sleeping, Vulkan or GPU work.
// Virtual time emits the normal number of records per 60 FPS source frame.
int main() {
    if (!pacingProfileEnabled()) {
        std::cerr << "Set FGVK_PACING_PROFILE=1; select FGVK_PACING_PROFILE_LOG=stderr for a fast sink or leave it unset for a private file.\n";
        return 1;
    }
    if (!std::freopen("/dev/null", "w", stderr)) return 1;
    initializePacingProfileLog();
    constexpr size_t frames = 120000;
    constexpr auto period = 16666666ns;
    size_t sample{};
    const auto run = [&](bool enabled) {
        PacingProfile profile;
        const auto origin = Clock::time_point{10s} + period * static_cast<int64_t>(sample++ * frames);
        const auto started = Clock::now();
        volatile uint64_t checksum{};
        for (size_t i = 0; i < frames; ++i) {
            const auto now = origin + period * static_cast<int64_t>(i);
            checksum = checksum + i;
            if (!enabled) continue;
            (void)Clock::now();
            profile.beginSource(now);
            ContextScope context(1, &profile);
            profile.policy(true, 60);
            std::array<Clock::duration, 9> phases{};
            for (size_t phase = 1; phase < phases.size(); ++phase) {
                const auto phaseStarted = fgvk::layer::present_detail::startPhaseTimer();
                phases[phase] = fgvk::layer::present_detail::finishPhaseTimer(phaseStarted);
                phases[0] += phases[phase];
            }
            recordPacingProfilePhases(1, phases);
            for (const auto offset : {1ms, 9ms}) {
                profile.outputTiming(PacingProfile::DriverTiming);
                const auto outputStarted = Clock::now();
                const auto duration = Clock::now() - outputStarted;
                profile.output(now + offset, now + offset + duration, VK_SUCCESS);
            }
            profile.wait(PacingProfile::ReadinessWait, 1ms);
            profile.wait(PacingProfile::OutputWait, 7ms);
            profile.pacedBatch(true, false);
            recordApplicationFrameTiming(ApplicationFrameApi::Present, 1, {}, now + 2ms, now + 12ms, {}, {}, 10us);
            const auto publicationStarted = Clock::now();
            const auto window = profile.finishSource(now + 10ms);
            if (window) logPacingProfile(1, VK_PRESENT_MODE_IMMEDIATE_KHR, now + 10ms, *window);
            profile.publicationCost(Clock::now() - publicationStarted);
        }
        (void)checksum;
        return std::chrono::duration<double, std::micro>(Clock::now() - started).count() / frames;
    };
    std::array<double, 7> deltas{};
    for (size_t pair = 0; pair < deltas.size(); ++pair) {
        double baseline{}, observed{};
        if (pair % 2 == 0) { baseline = run(false); observed = run(true); }
        else { observed = run(true); baseline = run(false); }
        deltas[pair] = observed - baseline;
        std::cout << "pair=" << pair << " baseline_us_per_frame=" << baseline
                  << " observed_us_per_frame=" << observed << " delta_us=" << deltas[pair] << '\n';
    }
    std::sort(deltas.begin(), deltas.end());
    std::cout << "median_observer_us_per_frame=" << deltas[3]
              << " percent_60fps_budget=" << deltas[3] / 16666.6667 * 100
              << " scope=synthetic-cpu-only\n";
}

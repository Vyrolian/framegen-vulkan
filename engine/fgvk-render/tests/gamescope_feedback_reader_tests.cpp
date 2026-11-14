/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gamescope_feedback.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

extern "C" {
    void fgvk_test_feedback_reset();
    unsigned fgvk_test_feedback_hdr_atoms();
    unsigned fgvk_test_feedback_hdr_reads();
    void fgvk_test_feedback_refresh(unsigned);
    void fgvk_test_feedback_identity(unsigned, unsigned);
}
namespace {
    void expect(bool value, const char* message) {
        if (!value) throw std::runtime_error(message);
    }
}
int main() {
    try {
        setenv("DISPLAY", ":777", 1);
        setenv("SteamAppId", "730", 1);
        unsetenv("UMU_STEAM_GAME_ID");
        unsetenv("SteamGameId");
        fgvk_test_feedback_reset();
        fgvk::layer::GamescopeFeedbackReader reader;
        auto sample = reader.diagnosticSample();
        expect(sample.gamescopeDetected && sample.gamescopePid == 77 &&
            sample.xwaylandServerId == 0 && sample.refreshHz == 90 &&
            sample.outputWidth == 1920 && sample.presentation.vrrActive == true,
            "presentation monitoring must retain identity, geometry, refresh and VRR");
        fgvk_test_feedback_refresh(144);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        do {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            sample = reader.diagnosticSample();
        } while (sample.refreshHz != 144 && std::chrono::steady_clock::now() < deadline);
        expect(sample.refreshHz == 144, "presentation monitoring must remain live");
        expect(fgvk_test_feedback_hdr_atoms() == 0 && fgvk_test_feedback_hdr_reads() == 0,
            "presentation monitoring must perform no HDR atom lookup or property read");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}

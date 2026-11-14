/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "presentation_policy.hpp"

#include <algorithm>
#include <chrono>
#include <charconv>
#include <memory>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fgvk::layer {

    [[nodiscard]] inline std::optional<bool> gamescopeBooleanFeedback(
            const std::optional<uint32_t> value) noexcept {
        if (!value || *value > 1)
            return std::nullopt;
        return *value != 0;
    }

    [[nodiscard]] inline std::optional<uint32_t> gamescopeApplicationId(
            const std::string_view value) {
        if (value.empty())
            return std::nullopt;
        uint32_t id{};
        const auto result = std::from_chars(value.data(), value.data() + value.size(), id);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
                id == 0 || id == 769)
            return std::nullopt;
        return id;
    }

    /// SteamGameId is either a plain Steam app or a packed shortcut CGameID.
    /// A shortcut has type 2 and no app in its low word; its high word is the
    /// Gamescope app identity. Reject other types rather than truncating them.
    [[nodiscard]] inline std::optional<uint32_t> gamescopeSteamGameId(
            const std::string_view value) {
        if (value.empty())
            return std::nullopt;
        uint64_t id{};
        const auto result = std::from_chars(value.data(), value.data() + value.size(), id);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
            return std::nullopt;
        if (id <= 0x00ffffff)
            return gamescopeApplicationId(value);
        const auto shortcut = static_cast<uint32_t>(id >> 32);
        if ((id & 0xffffffff) != 0x02000000 || (shortcut & 0x80000000) == 0)
            return std::nullopt;
        return shortcut;
    }

    struct GamescopeApplicationIdentityHints {
        std::string_view steamAppId;
        std::string_view compatAppId;
        std::string_view steamGameId;
        std::string_view umuSteamGameId;
    };

    /// Resolve once at process startup. UMU preserves the Steam launch identity
    /// before replacing the normal IDs with its store ID or prefix hash.
    [[nodiscard]] inline std::optional<uint32_t> resolveGamescopeApplicationId(
            const GamescopeApplicationIdentityHints& hints) {
        if (const auto id = gamescopeSteamGameId(hints.umuSteamGameId))
            return id;
        if (const auto id = gamescopeApplicationId(hints.steamAppId))
            return id;
        if (const auto id = gamescopeApplicationId(hints.compatAppId))
            return id;
        return gamescopeSteamGameId(hints.steamGameId);
    }

    /// Gamescope identifies Steam's UI as app 769. Missing/empty properties,
    /// another game, and unidentifiable launchers are NOT menu evidence.
    [[nodiscard]] inline std::optional<bool> classifyGamescopeFocus(
            const std::optional<uint32_t> applicationId,
            const std::optional<uint32_t> inputApp,
            const std::optional<uint32_t> graphicsApp) {
        if (!applicationId || !inputApp || !graphicsApp)
            return std::nullopt;
        if (*inputApp == *applicationId && *graphicsApp == *applicationId)
            return true;
        if (*inputApp == 769 && (*graphicsApp == *applicationId || *graphicsApp == 769))
            return false;
        return std::nullopt;
    }

    /// Debounce on the existing background reader, even when the game stops
    /// presenting. Unknown/stale evidence breaks the open/close chain.
    class GamescopeFocusTracker {
    public:
        [[nodiscard]] GamescopeFocusFeedback observe(
                const GamescopeFocusFeedback::Clock::time_point now,
                const std::optional<bool> focused) {
            if (!this->feedback.fresh(now) || !focused) {
                const auto sequence = this->feedback.returnSequence;
                this->feedback = {};
                this->feedback.returnSequence = sequence;
                this->candidate.reset();
            }
            this->feedback.sampledAt = now;
            if (!focused)
                return this->feedback;
            if (this->candidate != focused) {
                this->candidate = focused;
                this->candidateSince = now;
            }
            if (now - this->candidateSince >= std::chrono::milliseconds{250} &&
                    this->feedback.gameFocused != focused) {
                if (!*focused) {
                    this->feedback.openedAt = this->candidateSince;
                    this->feedback.returnedAt.reset();
                } else if (this->feedback.gameFocused == false) {
                    this->feedback.returnedAt = now;
                    ++this->feedback.returnSequence;
                }
                this->feedback.gameFocused = focused;
            }
            return this->feedback;
        }
    private:
        GamescopeFocusFeedback feedback;
        std::optional<bool> candidate;
        GamescopeFocusFeedback::Clock::time_point candidateSince{};
    };

    /// Conservative process-start hint used only for safety gates that must
    /// fail closed before X11 feedback can confirm compositor identity.
    [[nodiscard]] bool gamescopeProcessEnvironmentHint();

    struct GamescopeFeedbackSample {
        std::optional<uint32_t> refreshHz;
        GamescopePresentationFeedback presentation;
        bool gamescopeDetected{false};
        std::optional<uint32_t> gamescopePid;
        std::optional<uint32_t> xwaylandServerId;
        std::optional<uint32_t> outputWidth;
        std::optional<uint32_t> outputHeight;
        std::string status;
        std::string display;
        std::string resolverStatus;
        std::string resolverCandidates;
        GamescopeFocusFeedback focus;
    };

    struct GamescopePresentationTarget {
        uint32_t width{0};
        uint32_t height{0};

        bool operator==(const GamescopePresentationTarget&) const = default;
    };

    /// Server zero owns Gamescope's real presentation dimensions. Treat the
    /// X11 root geometry as an output ceiling only after the same resolver
    /// that owns refresh feedback has proven Gamescope identity.
    [[nodiscard]] inline std::optional<GamescopePresentationTarget>
    confirmedGamescopePresentationTarget(
            const GamescopeFeedbackSample& sample) {
        if (!sample.gamescopeDetected || !sample.xwaylandServerId ||
                *sample.xwaylandServerId != 0 || !sample.outputWidth ||
                !sample.outputHeight || *sample.outputWidth == 0 ||
                *sample.outputHeight == 0) {
            return std::nullopt;
        }
        return GamescopePresentationTarget{
            .width = *sample.outputWidth,
            .height = *sample.outputHeight,
        };
    }

    /// Keep compositor feedback responsive when Gamescope is present, but do
    /// not wake an ordinary desktop Vulkan process four times per second just
    /// because it has an X11 display. The idle probe preserves late discovery
    /// without making non-Gamescope monitoring a hot background loop.
    [[nodiscard]] inline std::chrono::milliseconds
    gamescopeFeedbackPollInterval(
            const bool environmentHint,
            const bool gamescopeDetected) {
        return environmentHint || gamescopeDetected
            ? std::chrono::milliseconds{250}
            : std::chrono::milliseconds{1'000};
    }

    struct GamescopeXwaylandDisplay {
        std::string display;
        std::optional<uint32_t> gamescopePid;
        std::optional<uint32_t> serverId;
    };

    /// Choose Gamescope's server-zero Xwayland display from displays belonging
    /// to the same compositor process as the game's current display.
    [[nodiscard]] inline std::optional<std::string> selectGamescopeRootDisplay(
            const GamescopeXwaylandDisplay& current,
            const std::vector<GamescopeXwaylandDisplay>& candidates) {
        if (!current.gamescopePid || !current.serverId)
            return std::nullopt;
        if (*current.serverId == 0)
            return current.display;

        const auto candidate = std::ranges::find_if(candidates,
            [&current](const GamescopeXwaylandDisplay& value) {
                return value.gamescopePid == current.gamescopePid &&
                    value.serverId && *value.serverId == 0;
            });
        if (candidate == candidates.end())
            return std::nullopt;
        return candidate->display;
    }

    /// Monitors Gamescope refresh, focus and presentation feedback without a
    /// mandatory X11 link-time dependency or an X11 round trip to the frame
    /// presentation path.
    class GamescopeFeedbackReader {
    public:
        GamescopeFeedbackReader();
        ~GamescopeFeedbackReader();

        GamescopeFeedbackReader(const GamescopeFeedbackReader&) = delete;
        GamescopeFeedbackReader& operator=(const GamescopeFeedbackReader&) = delete;
        GamescopeFeedbackReader(GamescopeFeedbackReader&&) noexcept;
        GamescopeFeedbackReader& operator=(GamescopeFeedbackReader&&) noexcept;

        /// Return the cached compositor state and its diagnostic reason.
        [[nodiscard]] GamescopeFeedbackSample diagnosticSample() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

}

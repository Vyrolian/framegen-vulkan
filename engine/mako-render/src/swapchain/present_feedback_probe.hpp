/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstdint>
#include <optional>

namespace mako::layer {

// Decides whether MAKO may tag its real-frame output with its own present IDs
// so that the driver starts reporting VkSwapchainTimingPropertiesEXT refresh
// timing. Presentation feedback is requested only through successful timeout-0
// vkWaitForPresent2KHR polls, and the application-branch refresh duration is
// trusted only after those polls have succeeded.
//
// The probe is pure policy: the caller performs every Vulkan call and reports
// the outcome. Any application-supplied present ID ends the probe for good, so
// MAKO never shares or races the application's present-ID namespace.
class PresentFeedbackProbe {
public:
    enum class Stop { None, ApplicationIds, Collision, Error };
    enum class PollResult { Success, Timeout, Error };
    static constexpr uint32_t confirmationPolls = 4;

    // Sets eligibility and resets all other state.
    void configure(bool eligible) {
        *this = PresentFeedbackProbe{};
        this->eligible = eligible;
    }

    [[nodiscard]] bool active() const { return this->eligible && this->stop == Stop::None; }
    // Confirmation survives a later ApplicationIds stop; an Error revokes it.
    [[nodiscard]] bool confirmed() const {
        return this->stop != Stop::Error && this->successCount >= confirmationPolls;
    }
    [[nodiscard]] Stop stopReason() const { return this->stop; }
    [[nodiscard]] uint32_t successes() const { return this->successCount; }
    [[nodiscard]] uint64_t lastTagged() const { return this->lastTaggedId; }

    // Once per application present, before tagging. applicationId is the
    // non-zero present ID the application attached (VkPresentIdKHR or
    // VkPresentId2KHR), or nullopt/0 when none. Any non-zero ID stops the
    // probe for good: Collision when *applicationId <= lastTagged, else
    // ApplicationIds. Returns true only on the call that stopped it.
    bool observeApplicationId(std::optional<uint64_t> applicationId) {
        if (!this->eligible || this->stop != Stop::None)
            return false;
        if (!applicationId || *applicationId == 0)
            return false;
        this->stop = *applicationId <= this->lastTaggedId ? Stop::Collision : Stop::ApplicationIds;
        return true;
    }

    // When active: ++presentCount, store it as lastTagged and return it (the
    // ID for this present's real output). Otherwise nullopt. IDs are the
    // per-swapchain application present count starting at 1, so they are
    // strictly increasing and never exceed the application's own present
    // count (VKD3D-Proton later uses max(prev + 1, present_count)).
    std::optional<uint64_t> nextTag() {
        if (!this->active())
            return std::nullopt;
        this->lastTaggedId = ++this->presentCount;
        return this->lastTaggedId;
    }

    // When active: the ID to poll now, i.e. lastTagged - 2 when that is >= 1
    // and greater than lastPolledSuccess; otherwise nullopt.
    [[nodiscard]] std::optional<uint64_t> pollTarget() const {
        if (!this->active() || this->lastTaggedId < 3)
            return std::nullopt;
        const uint64_t target = this->lastTaggedId - 2;
        if (target <= this->lastPolledSuccess)
            return std::nullopt;
        return target;
    }

    // Result of a timeout-0 poll: success -> ++successes,
    // lastPolledSuccess = id, returns true when this success first reaches
    // confirmationPolls. timeout -> no change. error -> stop = Error and
    // successes = 0. Returns whether confirmation was newly reached. Polls
    // after any stop are ignored, so an ApplicationIds stop keeps confirmation.
    bool pollResult(uint64_t id, PollResult result) {
        if (!this->active())
            return false;
        switch (result) {
            case PollResult::Success: {
                const bool first = this->successCount < confirmationPolls;
                if (this->successCount != UINT32_MAX)
                    ++this->successCount;
                this->lastPolledSuccess = id;
                return first && this->successCount >= confirmationPolls;
            }
            case PollResult::Error:
                this->stop = Stop::Error;
                this->successCount = 0;
                return false;
            case PollResult::Timeout:
                return false;
        }
        return false;
    }

private:
    bool eligible{};
    Stop stop{Stop::None};
    uint64_t presentCount{};
    uint64_t lastTaggedId{};
    uint64_t lastPolledSuccess{};
    uint32_t successCount{};
};

}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace fgvk::layer {

// Decides whether framegen-vulkan may tag its real-frame output with its own present IDs
// so that the driver starts reporting VkSwapchainTimingPropertiesEXT refresh
// timing. Presentation feedback is requested only through successful timeout-0
// vkWaitForPresent2KHR polls, and the application-branch refresh duration is
// trusted only after those polls have succeeded.
//
// The probe is pure policy: the caller performs every Vulkan call and reports
// the outcome. The first application present ID above every tag switches the
// probe to following the application's own IDs (Stop::ApplicationIds): tagging
// stops, and each later timeout-0 poll targets the application's ID two presents
// back, so the same confirmation still reaches the refresh query. An application
// ID at or below the last tag collides with framegen-vulkan's namespace and stops
// the probe for good (Stop::Collision).
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

    // framegen-vulkan's own tags are live: nothing has stopped tagging.
    [[nodiscard]] bool active() const { return this->eligible && this->stop == Stop::None; }
    // Polls are made for the own tags, and for the application's IDs after ApplicationIds.
    [[nodiscard]] bool polling() const {
        return this->eligible && (this->stop == Stop::None || this->stop == Stop::ApplicationIds);
    }
    // The application owns the present IDs and framegen-vulkan follows them.
    [[nodiscard]] bool following() const { return this->eligible && this->stop == Stop::ApplicationIds; }
    // Confirmation survives a later ApplicationIds stop; an Error revokes it.
    [[nodiscard]] bool confirmed() const {
        return this->stop != Stop::Error && this->successCount >= confirmationPolls;
    }
    [[nodiscard]] Stop stopReason() const { return this->stop; }
    [[nodiscard]] uint32_t successes() const { return this->successCount; }
    [[nodiscard]] uint64_t lastTagged() const { return this->lastTaggedId; }

    // Once per application present, before tagging or polling. applicationId is the
    // largest non-zero present ID the application attached (VkPresentIdKHR or
    // VkPresentId2KHR; id2 says which), or nullopt/0 when none. While following,
    // every present records its ID (zero when none) for the poll two presents later.
    // Otherwise a non-zero ID stops tagging: Collision when *applicationId <= lastTagged,
    // else ApplicationIds. Returns true only on the call that stopped tagging.
    bool observeApplicationId(std::optional<uint64_t> applicationId, bool id2 = false) {
        if (!this->eligible)
            return false;
        const Recorded current{applicationId.value_or(0), id2};
        bool stoppedNow = false;
        if (this->stop == Stop::None && current.id != 0) {
            this->stop = current.id <= this->lastTaggedId ? Stop::Collision : Stop::ApplicationIds;
            stoppedNow = true;
        }
        if (this->stop == Stop::ApplicationIds) {
            this->recent[0] = this->recent[1];
            this->recent[1] = this->recent[2];
            this->recent[2] = current;
        }
        return stoppedNow;
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

    // When polling: after ApplicationIds, the application's ID recorded two presents
    // earlier; otherwise lastTagged - 2 while that is >= 1. Nullopt when that ID is zero
    // or not greater than lastPolledSuccess.
    [[nodiscard]] std::optional<uint64_t> pollTarget() const {
        if (this->stop == Stop::ApplicationIds) {
            const uint64_t target = this->recent[0].id;
            if (target == 0 || target <= this->lastPolledSuccess)
                return std::nullopt;
            return target;
        }
        if (!this->active() || this->lastTaggedId < 3)
            return std::nullopt;
        const uint64_t target = this->lastTaggedId - 2;
        if (target <= this->lastPolledSuccess)
            return std::nullopt;
        return target;
    }
    // True when pollTarget is an application ID that came from VkPresentId2KHR.
    [[nodiscard]] bool pollTargetIsId2() const {
        return this->stop == Stop::ApplicationIds && this->recent[0].id2;
    }

    // Result of a timeout-0 poll: success -> ++successes,
    // lastPolledSuccess = id, returns true when this success first reaches
    // confirmationPolls. timeout -> no change. error -> stop = Error and
    // successes = 0. Returns whether confirmation was newly reached. Polls are
    // ignored after a Collision or Error. Confirmation survives an ApplicationIds
    // stop, and polls of the application's IDs count toward it.
    bool pollResult(uint64_t id, PollResult result) {
        if (!this->polling())
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
    struct Recorded {
        uint64_t id{};
        bool id2{};
    };
    bool eligible{};
    Stop stop{Stop::None};
    uint64_t presentCount{};
    uint64_t lastTaggedId{};
    uint64_t lastPolledSuccess{};
    uint32_t successCount{};
    // While following: the application IDs of the last three presents, oldest first.
    std::array<Recorded, 3> recent{};
};

}

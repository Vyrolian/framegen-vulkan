/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "fgvk-backend/fgvk.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>

#include <vulkan/vulkan_core.h>

namespace fgvk::layer {

// DG1: generate writes straight into the parked lower image. FGVK_DIRECT_WRITE=0 turns it off: only the exact value "0"
// is off, so unset (null), empty and every other value keep it on.
[[nodiscard]] constexpr bool parseDirectWriteSwitch(const char* value) noexcept {
    return !(value && std::string_view(value) == "0");
}

// Read once per process.
[[nodiscard]] inline bool directWriteRequested() noexcept {
    static const bool requested = parseDirectWriteSwitch(std::getenv("FGVK_DIRECT_WRITE"));
    return requested;
}

// DG1 rule 2: the lower swapchain gets STORAGE usage only for an FG-provisioned, attached, Sdr8 swapchain with exactly
// R8G8B8A8_UNORM images, when the surface and the format both support storage (storageSupported).
[[nodiscard]] constexpr bool directWriteLowerCapable(const bool frameGenerationProvisioned,
        const bool inlineRequested, const bool switchOn, const bool sdr8, const VkFormat lowerFormat,
        const bool storageSupported) noexcept {
    return frameGenerationProvisioned && inlineRequested && switchOn && sdr8 &&
        lowerFormat == VK_FORMAT_R8G8B8A8_UNORM && storageSupported;
}

// DG1 rule 7: a lower image is registered only when it has the destination's extent, since the direct write runs at that
// extent and in that format.
[[nodiscard]] constexpr bool directWriteTargetsMatch(const VkFormat lowerFormat, const VkExtent2D lowerExtent,
        const VkExtent2D destinationExtent) noexcept {
    return lowerFormat == VK_FORMAT_R8G8B8A8_UNORM &&
        lowerExtent.width == destinationExtent.width && lowerExtent.height == destinationExtent.height;
}

// DG1 rule 1: a batch takes a parked lower image before its schedule only when the swapchain was created for direct
// writes and the attached batch has one output whose lower acquire is on the owner's swapchain. A preacquired output
// would leave its image unused, so it is never taken for.
[[nodiscard]] constexpr bool directWriteBatchEligible(const bool capable, const bool attached,
        const bool sameLowerSwapchain, const size_t scheduledOutputs, const bool preacquired) noexcept {
    return capable && attached && sameLowerSwapchain && scheduledOutputs == 1 && !preacquired;
}

// DG1 rules 5 and 6: the parked lower image that one single-output batch writes. The hold owns the take from takeParked
// until the generated output adopts it. Destroyed before the backend's schedule starts, the image goes back to the pool.
// Destroyed after the schedule started, the backend may have consumed the image's acquire wait, so the slot fails (sticky)
// and the output count ends. An adopted take ends its count at the end of the generated output's iteration.
template <typename Owner, typename Take>
class DirectWriteHold {
public:
    DirectWriteHold() = default;
    DirectWriteHold(const DirectWriteHold&) = delete;
    DirectWriteHold& operator=(const DirectWriteHold&) = delete;
    DirectWriteHold(DirectWriteHold&&) = delete;
    DirectWriteHold& operator=(DirectWriteHold&&) = delete;
    ~DirectWriteHold() {
        if (!this->take_ || this->adopted_)
            return;
        if (this->scheduleStarted_) {
            this->owner_->failParkedTake(this->take_->slot);
            this->owner_->endCallOutput();
        } else {
            this->owner_->returnParked(*this->take_);
        }
    }

    // Takes the oldest parked image of owner. False when none is parked, and the batch then writes its usual destination.
    [[nodiscard]] bool take(Owner& owner) {
        this->owner_ = &owner;
        this->take_ = owner.takeParked();
        return this->take_.has_value();
    }

    // The backend target of the batch, or nullptr when no image was taken.
    [[nodiscard]] const fgvk::backend::DirectTarget* target() {
        if (!this->take_)
            return nullptr;
        this->direct_ = fgvk::backend::DirectTarget{
            .lowerIndex = this->take_->lowerIndex,
            .wait = this->take_->semaphore,
        };
        return &this->direct_;
    }

    // Called before the backend's schedule with target(). From here the backend may have consumed the image's wait.
    void beginSchedule() noexcept { this->scheduleStarted_ = true; }

    // The image the generated output writes. Its iteration ends the output count, so the hold no longer does.
    [[nodiscard]] std::optional<Take> adopt() noexcept {
        if (!this->holding())
            return std::nullopt;
        this->adopted_ = true;
        return this->take_;
    }

    // True while a taken image is not adopted yet.
    [[nodiscard]] bool holding() const noexcept { return this->take_.has_value() && !this->adopted_; }

private:
    Owner* owner_{};
    std::optional<Take> take_;
    fgvk::backend::DirectTarget direct_{};
    bool scheduleStarted_{};
    bool adopted_{};
};

}  // namespace fgvk::layer

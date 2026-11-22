/* SPDX-License-Identifier: GPL-3.0-or-later */

// DG1: the FGVK_DIRECT_WRITE parse, the lower-usage and batch eligibility rules, the target match, and the hold that owns a
// parked lower image from its take to its adoption or failure. The hold is a template, so a fake owner counts the calls.

#include "swapchain/direct_write.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
    using fgvk::layer::DirectWriteHold;

    void require(const bool condition, const std::string& message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    // The fields a hold reads from a take. ParkedLowerTake has the same fields in the owner.
    struct FakeTake {
        uint32_t lowerIndex{};
        uint32_t slot{};
        VkSemaphore semaphore{VK_NULL_HANDLE};
    };

    // Counts the owner calls a hold makes, and hands out one take per takeParked while one is available.
    struct FakeOwner {
        std::optional<FakeTake> available{FakeTake{.lowerIndex = 1, .slot = 0, .semaphore = reinterpret_cast<VkSemaphore>(0x7)}};
        size_t takes{};
        size_t returns{};
        size_t fails{};
        size_t ends{};

        std::optional<FakeTake> takeParked() {
            ++this->takes;
            auto taken = this->available;
            this->available.reset();
            return taken;
        }
        void returnParked(const FakeTake&) noexcept { ++this->returns; }
        void failParkedTake(uint32_t) noexcept { ++this->fails; }
        void endCallOutput() noexcept { ++this->ends; }
    };

    using Hold = DirectWriteHold<FakeOwner, FakeTake>;

    // DG1: unset (null), empty and every value other than "0" keep direct write on. Only "0" exactly turns it off.
    void testSwitchParse() {
        using fgvk::layer::parseDirectWriteSwitch;
        static_assert(parseDirectWriteSwitch(nullptr), "the switch parse must be usable in constant expressions");
        static_assert(!parseDirectWriteSwitch("0"), "the switch parse must be usable in constant expressions");
        require(parseDirectWriteSwitch(nullptr), "an unset FGVK_DIRECT_WRITE must keep direct write on");
        require(parseDirectWriteSwitch(""), "an empty FGVK_DIRECT_WRITE must keep direct write on");
        require(parseDirectWriteSwitch("1"), "FGVK_DIRECT_WRITE=1 must keep direct write on");
        require(!parseDirectWriteSwitch("0"), "FGVK_DIRECT_WRITE=0 must turn direct write off");
        require(parseDirectWriteSwitch("00"), "only the exact value 0 turns direct write off, not 00");
        require(parseDirectWriteSwitch("off"), "an unrecognised FGVK_DIRECT_WRITE value must keep direct write on");
    }

    // DG1 rule 2: STORAGE usage needs every condition: FG provisioned, attached, the switch on, Sdr8, exactly RGBA8, and storage.
    void testLowerCapable() {
        using fgvk::layer::directWriteLowerCapable;
        constexpr auto rgba = VK_FORMAT_R8G8B8A8_UNORM;
        static_assert(directWriteLowerCapable(true, true, true, true, rgba, true));
        require(directWriteLowerCapable(true, true, true, true, rgba, true),
            "an attached Sdr8 RGBA8 swapchain with storage must be a direct write target");
        require(!directWriteLowerCapable(false, true, true, true, rgba, true),
            "a swapchain without frame generation must not be a direct write target");
        require(!directWriteLowerCapable(true, false, true, true, rgba, true),
            "a swapchain without the attached path must not be a direct write target");
        require(!directWriteLowerCapable(true, true, false, true, rgba, true),
            "FGVK_DIRECT_WRITE=0 must not create storage lower images");
        require(!directWriteLowerCapable(true, true, true, false, rgba, true),
            "a high-precision swapchain must not be a direct write target");
        require(!directWriteLowerCapable(true, true, true, true, VK_FORMAT_B8G8R8A8_UNORM, true),
            "a BGRA swapchain must not be a direct write target");
        require(!directWriteLowerCapable(true, true, true, true, VK_FORMAT_R8G8B8A8_SRGB, true),
            "an sRGB swapchain must not be a direct write target");
        require(!directWriteLowerCapable(true, true, true, true, rgba, false),
            "a surface or format without storage support must not be a direct write target");
    }

    // DG1 rule 7: the registered lower image has the destination's format and extent exactly.
    void testTargetsMatch() {
        using fgvk::layer::directWriteTargetsMatch;
        const VkExtent2D extent{2560, 1440};
        require(directWriteTargetsMatch(VK_FORMAT_R8G8B8A8_UNORM, extent, {2560, 1440}),
            "a lower image of the destination extent must match");
        require(!directWriteTargetsMatch(VK_FORMAT_R8G8B8A8_UNORM, extent, {2560, 1400}),
            "a lower image of another height must not match");
        require(!directWriteTargetsMatch(VK_FORMAT_R8G8B8A8_UNORM, extent, {2550, 1440}),
            "a lower image of another width must not match");
        require(!directWriteTargetsMatch(VK_FORMAT_B8G8R8A8_UNORM, extent, extent),
            "a non-RGBA8 lower image must not match");
    }

    // DG1 rule 1: a batch takes before its schedule only with the swapchain capable, attached, on the owner's lower swapchain,
    // one output, and no preacquired output.
    void testBatchEligible() {
        using fgvk::layer::directWriteBatchEligible;
        require(directWriteBatchEligible(true, true, true, 1, false),
            "a capable attached one-output batch on the lower swapchain must take a parked image");
        require(!directWriteBatchEligible(false, true, true, 1, false),
            "an incapable swapchain must not take a parked image");
        require(!directWriteBatchEligible(true, false, true, 1, false),
            "a detached batch must not take a parked image");
        require(!directWriteBatchEligible(true, true, false, 1, false),
            "a batch on another swapchain must not take a parked image");
        require(!directWriteBatchEligible(true, true, true, 2, false),
            "a two-output batch must not take a parked image");
        require(!directWriteBatchEligible(true, true, true, 0, false),
            "an empty batch must not take a parked image");
        require(!directWriteBatchEligible(true, true, true, 1, true),
            "a preacquired output must not take a parked image");
    }

    // DG1 rule 6: a take that never reached the backend's schedule goes back to the pool, and the owner's count ends once.
    void testHoldReturnsBeforeSchedule() {
        FakeOwner owner;
        {
            Hold hold;
            require(hold.take(owner), "a parked image must be taken");
            require(owner.takes == 1, "the take must be one owner call");
        }
        require(owner.returns == 1, "a take that never scheduled must be returned exactly once");
        require(owner.fails == 0, "a take that never scheduled must not fail its slot");
        require(owner.ends == 0, "a returned take must not end the output count again");
    }

    // DG1 rule 6: a take whose schedule started may have had its wait consumed, so the slot fails and the count ends.
    void testHoldFailsAfterScheduleStarts() {
        FakeOwner owner;
        {
            Hold hold;
            require(hold.take(owner), "a parked image must be taken");
            hold.beginSchedule();
        }
        require(owner.fails == 1, "a take whose schedule started must fail its slot once");
        require(owner.ends == 1, "a take whose schedule started must end the output count once");
        require(owner.returns == 0, "a take whose schedule started must not be returned");
    }

    // DG1 rule 5: an adopted take is the generated output's. The hold makes no owner call after the adoption.
    void testHoldAdoptsOnce() {
        FakeOwner owner;
        {
            Hold hold;
            require(hold.take(owner), "a parked image must be taken");
            const auto target = hold.target();
            require(target && target->lowerIndex == 1, "the target must name the taken lower image");
            require(target->wait == reinterpret_cast<VkSemaphore>(0x7), "the target must wait the image's acquire");
            hold.beginSchedule();
            const auto adopted = hold.adopt();
            require(adopted && adopted->lowerIndex == 1, "the generated output must adopt the taken image");
            require(!hold.holding(), "an adopted image must no longer be held");
            require(!hold.adopt(), "an image must be adopted once");
        }
        require(owner.returns == 0 && owner.fails == 0 && owner.ends == 0,
            "an adopted image must end with its output, not with the hold");
    }

    // DG1 rule 1: with nothing parked the batch has no target and the hold owns nothing.
    void testHoldWithoutParkedImage() {
        FakeOwner owner;
        owner.available.reset();
        {
            Hold hold;
            require(!hold.take(owner), "an empty pool must give no take");
            require(hold.target() == nullptr, "a batch without a take must have no direct target");
            require(!hold.holding(), "a batch without a take must hold nothing");
            require(!hold.adopt(), "a batch without a take must adopt nothing");
        }
        require(owner.returns == 0 && owner.fails == 0 && owner.ends == 0,
            "a batch without a take must make no owner call beyond the take");
    }
}  // namespace

int main() {
    try {
        testSwitchParse();
        testLowerCapable();
        testTargetsMatch();
        testBatchEligible();
        testHoldReturnsBeforeSchedule();
        testHoldFailsAfterScheduleStarts();
        testHoldAdoptsOnce();
        testHoldWithoutParkedImage();
    } catch (const std::exception& error) {
        std::cerr << "Direct write test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "direct write tests passed\n";
    return 0;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/present_feedback_probe.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>

using namespace fgvk::layer;

namespace {
using Probe = PresentFeedbackProbe;

void expect(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

// Tags one present and polls the target that becomes available, if any.
void tagAndPoll(Probe& probe, Probe::PollResult result) {
    expect(probe.nextTag().has_value(), "an active probe did not tag");
    if (const auto target = probe.pollTarget())
        probe.pollResult(*target, result);
}

void ineligibleNeverTagsOrPolls() {
    Probe probe;
    probe.configure(false);
    expect(!probe.active(), "an ineligible probe is active");
    expect(!probe.observeApplicationId(std::nullopt) && !probe.observeApplicationId(7),
        "an ineligible probe stopped on an application ID");
    expect(!probe.nextTag().has_value(), "an ineligible probe tagged a present");
    for (int i = 0; i < 8; ++i) {
        expect(!probe.pollTarget().has_value(), "an ineligible probe offered a poll");
        expect(!probe.pollResult(static_cast<uint64_t>(i), Probe::PollResult::Success),
            "an ineligible probe confirmed");
    }
    expect(!probe.confirmed() && probe.successes() == 0 && probe.lastTagged() == 0,
        "an ineligible probe changed its state");
}

void tagsIncreaseFromOne() {
    Probe probe;
    probe.configure(true);
    expect(probe.active() && probe.stopReason() == Probe::Stop::None,
        "an eligible probe did not start active");
    for (uint64_t expected = 1; expected <= 16; ++expected) {
        const auto tag = probe.nextTag();
        expect(tag && *tag == expected, "tags did not run 1, 2, 3, ...");
        expect(probe.lastTagged() == expected, "lastTagged did not follow the tag");
    }
}

void pollTargetLagsByTwo() {
    Probe probe;
    probe.configure(true);
    expect(!probe.pollTarget().has_value(), "a poll was offered before any tag");
    expect(probe.nextTag() == std::optional<uint64_t>{1}, "the first tag was not 1");
    expect(!probe.pollTarget().has_value(), "a poll was offered after tag 1");
    expect(probe.nextTag() == std::optional<uint64_t>{2}, "the second tag was not 2");
    expect(!probe.pollTarget().has_value(), "a poll was offered after tag 2");
    expect(probe.nextTag() == std::optional<uint64_t>{3}, "the third tag was not 3");
    expect(probe.pollTarget() == std::optional<uint64_t>{1}, "tag 3 did not poll tag 1");
}

void successAdvancesPollAndTimeoutDoesNot() {
    Probe probe;
    probe.configure(true);
    for (int i = 0; i < 3; ++i) probe.nextTag();
    expect(probe.pollTarget() == std::optional<uint64_t>{1}, "the first poll target is wrong");
    expect(!probe.pollResult(1, Probe::PollResult::Success), "one success confirmed the probe");
    expect(probe.successes() == 1, "a success was not counted");
    expect(!probe.pollTarget().has_value(), "a polled target was offered again");

    probe.nextTag();
    expect(probe.pollTarget() == std::optional<uint64_t>{2}, "success did not advance the target");
    expect(!probe.pollResult(2, Probe::PollResult::Timeout), "a timeout reported confirmation");
    expect(probe.successes() == 1, "a timeout changed the success count");
    expect(probe.pollTarget() == std::optional<uint64_t>{2},
        "a timeout did not leave the same target pending");
}

void confirmationAtFourthSuccess() {
    static_assert(Probe::confirmationPolls == 4);
    Probe probe;
    probe.configure(true);
    probe.nextTag();
    probe.nextTag();
    for (uint64_t poll = 1; poll <= 3; ++poll) {
        probe.nextTag();
        expect(probe.pollTarget() == std::optional<uint64_t>{poll}, "poll target sequence is wrong");
        expect(!probe.pollResult(poll, Probe::PollResult::Success),
            "confirmation was reported before the fourth success");
        expect(!probe.confirmed(), "confirmed before the fourth success");
    }
    probe.nextTag();
    expect(probe.pollTarget() == std::optional<uint64_t>{4}, "the fourth poll target is wrong");
    expect(probe.pollResult(4, Probe::PollResult::Success), "the fourth success did not confirm");
    expect(probe.confirmed() && probe.successes() == 4, "the fourth success was not confirmed");
    probe.nextTag();
    expect(!probe.pollResult(5, Probe::PollResult::Success),
        "confirmation was reported more than once");
    expect(probe.confirmed() && probe.successes() == 5, "later success changed confirmation");
}

void applicationIdAboveTaggedStopsButKeepsConfirmation() {
    Probe probe;
    probe.configure(true);
    for (int i = 0; i < 6; ++i) tagAndPoll(probe, Probe::PollResult::Success);
    expect(probe.confirmed(), "the probe was not confirmed before the application ID");
    expect(probe.observeApplicationId(probe.lastTagged() + 10),
        "an application ID above lastTagged did not stop the probe");
    expect(probe.stopReason() == Probe::Stop::ApplicationIds,
        "an application ID above lastTagged was not ApplicationIds");
    expect(!probe.active(), "a stopped probe is still active");
    expect(probe.confirmed(), "an ApplicationIds stop revoked confirmation");
    expect(!probe.nextTag().has_value(), "a stopped probe tagged a present");
    expect(!probe.pollTarget().has_value(), "a stopped probe offered a poll");
    expect(!probe.observeApplicationId(999), "a second stop was reported as new");
    expect(probe.stopReason() == Probe::Stop::ApplicationIds,
        "a later application ID changed the stop reason");
}

void applicationIdAtOrBelowTaggedCollides() {
    Probe probe;
    probe.configure(true);
    probe.nextTag();
    probe.nextTag();
    expect(probe.observeApplicationId(2), "a colliding application ID did not stop the probe");
    expect(probe.stopReason() == Probe::Stop::Collision,
        "an application ID equal to lastTagged was not a collision");
    expect(!probe.active() && !probe.confirmed(), "a collision left the probe active");

    Probe lower;
    lower.configure(true);
    lower.nextTag();
    lower.nextTag();
    lower.nextTag();
    expect(lower.observeApplicationId(1) && lower.stopReason() == Probe::Stop::Collision,
        "an application ID below lastTagged was not a collision");
}

void errorStopsAndRevokesConfirmation() {
    Probe probe;
    probe.configure(true);
    for (int i = 0; i < 6; ++i) tagAndPoll(probe, Probe::PollResult::Success);
    expect(probe.confirmed(), "the probe was not confirmed before the error");
    probe.nextTag();
    expect(!probe.pollResult(probe.lastTagged() - 2, Probe::PollResult::Error),
        "an error reported confirmation");
    expect(probe.stopReason() == Probe::Stop::Error, "a poll error did not stop the probe");
    expect(!probe.confirmed() && probe.successes() == 0,
        "a poll error did not clear confirmation");
    expect(!probe.active() && !probe.nextTag().has_value(),
        "a probe stopped by an error still tagged a present");
    expect(!probe.observeApplicationId(100) && probe.stopReason() == Probe::Stop::Error,
        "an application ID replaced the error stop");
}

void nullAndZeroApplicationIdsAreIgnored() {
    Probe probe;
    probe.configure(true);
    expect(!probe.observeApplicationId(std::nullopt), "a missing application ID stopped the probe");
    expect(!probe.observeApplicationId(0), "a zero application ID stopped the probe");
    expect(probe.active() && probe.stopReason() == Probe::Stop::None,
        "ignored application IDs changed the probe state");
    expect(probe.nextTag() == std::optional<uint64_t>{1},
        "ignored application IDs changed the tag sequence");
}

void configureResets() {
    Probe probe;
    probe.configure(true);
    for (int i = 0; i < 6; ++i) tagAndPoll(probe, Probe::PollResult::Success);
    expect(probe.confirmed() && probe.lastTagged() == 6, "the probe setup did not confirm");
    expect(probe.observeApplicationId(50), "the probe did not stop before reconfiguration");
    probe.configure(true);
    expect(probe.active() && probe.stopReason() == Probe::Stop::None,
        "configure did not clear the stop");
    expect(!probe.confirmed() && probe.successes() == 0 && probe.lastTagged() == 0,
        "configure did not clear confirmation or tags");
    expect(!probe.pollTarget().has_value(), "configure kept a poll target");
    expect(probe.nextTag() == std::optional<uint64_t>{1}, "configure did not restart tags at 1");
    probe.configure(false);
    expect(!probe.active() && !probe.nextTag().has_value(),
        "configure(false) did not disable the probe");
}
}

int main() {
    ineligibleNeverTagsOrPolls();
    tagsIncreaseFromOne();
    pollTargetLagsByTwo();
    successAdvancesPollAndTimeoutDoesNot();
    confirmationAtFourthSuccess();
    applicationIdAboveTaggedStopsButKeepsConfirmation();
    applicationIdAtOrBelowTaggedCollides();
    errorStopsAndRevokesConfirmation();
    nullAndZeroApplicationIdsAreIgnored();
    configureResets();
    std::cout << "present feedback probe tests passed\n";
    return 0;
}

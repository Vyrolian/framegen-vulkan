/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/release_gate.hpp"

#include <cstdlib>
#include <iostream>
#include <thread>

using namespace fgvk::layer;
using namespace std::chrono_literals;

namespace {
void expect(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
template<class T> T handle(uintptr_t value) { return reinterpret_cast<T>(value); }

const auto queue = handle<VkQueue>(0x10);
const auto completion = handle<VkSemaphore>(0x20);

// VKD3D-Proton's dxgi_vk_swap_chain_present_signal_blit_semaphore shape.
struct SignalOnly {
    VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = completion, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal};
    explicit SignalOnly(uint64_t value) { signal.value = value; }
    SignalOnly(const SignalOnly&) = delete;
    SignalOnly& operator=(const SignalOnly&) = delete;
};

struct Wait {
    VkSemaphore semaphore{completion};
    uint64_t value{};
    VkSemaphoreWaitInfo info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1, .pSemaphores = &semaphore, .pValues = &value};
    explicit Wait(uint64_t target, VkSemaphoreWaitFlags flags = 0) {
        value = target;
        info.flags = flags;
    }
    Wait(const Wait&) = delete;
    Wait& operator=(const Wait&) = delete;
};

void present(ReleaseGate& gate, const ReleaseGateOwner& owner, uint64_t value,
        ReleaseGate::Clock::time_point deadline, ReleaseGate::Clock::time_point now) {
    owner.arm(queue, deadline);
    const SignalOnly signal(value);
    gate.observeSubmit2(queue, 1, &signal.submit, VK_NULL_HANDLE, now);
}
}

// A present with an application present ID: the token is that ID on the swapchain.
namespace {
const auto swapchain = handle<VkSwapchainKHR>(0x50);
const auto unbounded = ReleaseGate::Clock::time_point::max();

void presentIdTokenReleasesAtDeadline() {
    using Clock = ReleaseGate::Clock;
    const auto now = Clock::now();
    auto& gate = ReleaseGate::global();
    expect(gate.empty(), "release tokens were left by an earlier test");
    ReleaseGateOwner owner;
    owner.recordPresentId(swapchain, 5, now + 12ms);
    expect(!gate.empty(), "a present ID token was not recorded");
    const auto release = gate.releasedPresent(swapchain, 5, now, unbounded);
    expect(release && release->consumed && release->deadline == now + 12ms && gate.empty(),
        "the present ID wait did not end at the token's deadline");
    expect(gate.presentIdReleases() > 0, "a consumed present ID token was not counted");
}

void olderOrEqualPresentIdsReleaseTogether() {
    using Clock = ReleaseGate::Clock;
    const auto now = Clock::now();
    auto& gate = ReleaseGate::global();
    expect(gate.empty(), "release tokens were left by an earlier test");
    ReleaseGateOwner owner;
    owner.recordPresentId(swapchain, 5, now + 5ms);
    owner.recordPresentId(swapchain, 6, now + 9ms);
    expect(!gate.releasedPresent(swapchain, 4, now, unbounded) && !gate.empty(),
        "a wait for an earlier present ID released a later token");
    const auto release = gate.releasedPresent(swapchain, 6, now, unbounded);
    expect(release && release->consumed && release->deadline == now + 9ms && gate.empty(),
        "a wait covering two present IDs did not use the later deadline");
    expect(!gate.releasedPresent(swapchain, 6, now, unbounded) && gate.empty(),
        "a consumed present ID token was released again");
}

void tokenKindsNeverMatch() {
    using Clock = ReleaseGate::Clock;
    const auto now = Clock::now();
    auto& gate = ReleaseGate::global();
    expect(gate.empty(), "release tokens were left by an earlier test");
    // A semaphore and a swapchain that share one handle value, and one token number.
    const auto semaphore = handle<VkSemaphore>(0x50);
    uint64_t value = 3;
    VkSemaphoreWaitInfo waitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1, .pSemaphores = &semaphore, .pValues = &value};
    {
        // A present ID token is not satisfied by a semaphore wait, and a semaphore wait
        // does not consume it.
        ReleaseGateOwner owner;
        owner.recordPresentId(swapchain, 3, now + 9ms);
        expect(!gate.released(waitInfo, now, unbounded) && !gate.empty(),
            "a semaphore wait consumed a present ID token");
        expect(!gate.releasedPresent(swapchain, 2, now, unbounded) && !gate.empty(),
            "a present ID wait for a lower ID consumed a token");
        const auto release = gate.releasedPresent(swapchain, 3, now, unbounded);
        expect(release && release->consumed && release->deadline == now + 9ms && gate.empty(),
            "the present ID token was not released by its own wait");
    }
    {
        // A semaphore token is not satisfied by a present ID wait on the same handle value.
        ReleaseGateOwner owner;
        owner.arm(queue, now + 4ms);
        VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = semaphore, .value = 3, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal};
        gate.observeSubmit2(queue, 1, &submit, VK_NULL_HANDLE, now);
        expect(!gate.empty(), "the semaphore release token was not recorded");
        expect(!gate.releasedPresent(swapchain, 3, now, unbounded) && !gate.empty(),
            "a present ID wait consumed a semaphore token");
        const auto release = gate.released(waitInfo, now, unbounded);
        expect(release && release->consumed && release->deadline == now + 4ms && gate.empty(),
            "the semaphore token was not released by its own wait");
    }
}

void presentIdTimeoutKeepsToken() {
    using Clock = ReleaseGate::Clock;
    const auto now = Clock::now();
    auto& gate = ReleaseGate::global();
    expect(gate.empty(), "release tokens were left by an earlier test");
    ReleaseGateOwner owner;
    owner.recordPresentId(swapchain, 7, now + 20ms);
    const auto early = gate.releasedPresent(swapchain, 7, now, now + 5ms);
    expect(early && !early->consumed && early->deadline == now + 20ms && !gate.empty(),
        "a present ID wait past its timeout consumed the token");
    const auto late = gate.releasedPresent(swapchain, 7, now, unbounded);
    expect(late && late->consumed && late->deadline == now + 20ms && gate.empty(),
        "the kept present ID token was not released by a later wait");
}

void presentIdAndSemaphoreTokensShareActivation() {
    using Clock = ReleaseGate::Clock;
    const auto now = Clock::now();
    auto& gate = ReleaseGate::global();
    expect(gate.empty(), "release tokens were left by an earlier test");
    ReleaseGateOwner owner;
    owner.recordPresentId(swapchain, 1, now);
    expect(gate.releasedPresent(swapchain, 1, now, unbounded) && !owner.active(),
        "one consumed present ID token activated the gate");
    owner.arm(queue, now);
    const SignalOnly signal(1);
    gate.observeSubmit2(queue, 1, &signal.submit, VK_NULL_HANDLE, now);
    const Wait wait(1);
    expect(gate.released(wait.info, now, unbounded) && owner.active() && gate.empty(),
        "a present ID token and a semaphore token did not share one activation run");
}
}

int main() {
    using Clock = ReleaseGate::Clock;
    const auto now = Clock::now();
    auto& gate = ReleaseGate::global();

    {
        const SignalOnly signal(1);
        gate.observeSubmit2(queue, 1, &signal.submit, VK_NULL_HANDLE, now);
        expect(gate.empty(), "an unarmed submit recorded a release token");
    }
    {
        ReleaseGateOwner owner;
        // Work submitted after the present consumes the arm without a token.
        owner.arm(queue, now + 10ms);
        const VkCommandBufferSubmitInfo buffer{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = handle<VkCommandBuffer>(0x30)};
        SignalOnly withWork(1);
        withWork.submit.commandBufferInfoCount = 1;
        withWork.submit.pCommandBufferInfos = &buffer;
        gate.observeSubmit2(queue, 1, &withWork.submit, VK_NULL_HANDLE, now);
        expect(gate.empty() && !ReleaseGate::armed(), "a submit with work carried the release token");
        const SignalOnly late(1);
        gate.observeSubmit2(queue, 1, &late.submit, VK_NULL_HANDLE, now);
        expect(gate.empty(), "the arm survived the submit that followed its present");

        owner.arm(queue, now + 10ms);
        const SignalOnly other(1);
        gate.observeSubmit2(handle<VkQueue>(0x11), 1, &other.submit, VK_NULL_HANDLE, now);
        expect(gate.empty(), "a signal on another queue carried the release token");
    }
    {
        ReleaseGateOwner owner;
        expect(!owner.active(), "a new owner skipped its present hold before any wait");
        // Learning: the hold already happened, so the deadline has passed.
        for (uint64_t value = 1; value <= 2; ++value) {
            present(gate, owner, value, now, now);
            const Wait wait(value);
            const auto release = gate.released(wait.info, now, Clock::time_point::max());
            expect(release && release->consumed && release->deadline == now,
                "a satisfied wait did not consume its token");
        }
        expect(owner.active() && gate.empty(), "two observed release waits did not activate the gate");

        present(gate, owner, 3, now + 12ms, now);
        {
            const Wait other(3, VK_SEMAPHORE_WAIT_ANY_BIT);
            expect(!gate.released(other.info, now, Clock::time_point::max()),
                "a wait-any call was delayed");
        }
        {
            const Wait older(2);
            expect(!gate.released(older.info, now, Clock::time_point::max()),
                "a wait for an earlier value consumed a later token");
        }
        {
            const Wait poll(3);
            const auto release = gate.released(poll.info, now, now);
            expect(release && !release->consumed && !gate.empty(),
                "a poll before the deadline released or discarded the token");
        }
        {
            const Wait wait(3);
            const auto release = gate.released(wait.info, now, Clock::time_point::max());
            expect(release && release->consumed && release->deadline == now + 12ms && gate.empty(),
                "the release wait did not end at the cap deadline");
        }
        {
            present(gate, owner, 4, now + 1h, now);
            const Wait wait(4);
            const auto release = gate.released(wait.info, now, Clock::time_point::max());
            expect(release && release->deadline == now + ReleaseGate::maxDelay,
                "a release delay was not bounded");
        }
        {
            present(gate, owner, 5, now + 5ms, now);
            present(gate, owner, 6, now + 10ms, now);
            const Wait wait(6);
            const auto release = gate.released(wait.info, now, Clock::time_point::max());
            expect(release && release->deadline == now + 10ms && gate.empty(),
                "a wait covering two tokens did not use the later deadline");
        }
        // The application stops waiting: the backlog deactivates the gate.
        for (uint64_t value = 7; value < 7 + ReleaseGate::maxOutstanding + 1; ++value)
            present(gate, owner, value, now + 1ms, now);
        expect(!owner.active(), "an unobserved token backlog kept presents unheld");
    }
    expect(gate.empty(), "a destroyed owner left release tokens behind");
    {
        ReleaseGateOwner first;
        present(gate, first, 1, now + 50ms, now);
        ReleaseGateOwner second(std::move(first));
        const Wait wait(1);
        expect(gate.released(wait.info, now, Clock::time_point::max()).has_value(),
            "moving an owner retired its tokens");
    }
    {
        // Reused handle or restarted counter: a lower value replaces old tokens.
        ReleaseGateOwner owner;
        present(gate, owner, 40, now + 5ms, now);
        present(gate, owner, 1, now + 6ms, now);
        const Wait wait(1);
        const auto release = gate.released(wait.info, now, Clock::time_point::max());
        expect(release && release->deadline == now + 6ms && gate.empty(),
            "a restarted timeline kept a stale token");
    }
    {
        // The arm belongs to the presenting thread only.
        ReleaseGateOwner owner;
        owner.arm(queue, now + 5ms);
        std::thread other([&] {
            const SignalOnly signal(1);
            gate.observeSubmit2(queue, 1, &signal.submit, VK_NULL_HANDLE, now);
        });
        other.join();
        expect(gate.empty() && ReleaseGate::armed(), "another thread consumed the present's arm");
        ReleaseGate::disarm();
    }
    presentIdTokenReleasesAtDeadline();
    olderOrEqualPresentIdsReleaseTogether();
    tokenKindsNeverMatch();
    presentIdTimeoutKeepsToken();
    presentIdAndSemaphoreTokensShareActivation();
    std::cout << "release gate tests passed\n";
}

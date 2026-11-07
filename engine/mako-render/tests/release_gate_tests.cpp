/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "swapchain/release_gate.hpp"

#include <cstdlib>
#include <iostream>
#include <thread>

using namespace mako::layer;
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
    std::cout << "release gate tests passed\n";
}

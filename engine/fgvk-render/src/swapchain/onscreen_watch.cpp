/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "swapchain/onscreen_watch.hpp"

#include <pthread.h>

namespace fgvk::layer {

namespace {
int64_t nanoseconds(const OnScreenWatch::Clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}
}

OnScreenWatch::OnScreenWatch(Wait waitFunction, Emit emitFunction) :
        wait(std::move(waitFunction)), emit(std::move(emitFunction)),
        thread([this] { run(); }) {}

void OnScreenWatch::stop() noexcept {
    {
        const std::lock_guard lock(mutex);
        stopping = true;
    }
    changed.notify_all();
    if (thread.joinable()) thread.join();
}

void OnScreenWatch::run() {
    pthread_setname_np(pthread_self(), "fgvk-onscreen");
    for (;;) {
        Entry entry{};
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, [this] { return stopping || count != 0; });
            if (stopping) return;
            entry = ring[head];
            head = (head + 1) % capacity;
            --count;
            if (dead) continue;
        }
        const auto entered = Clock::now();
        VkResult result = VK_TIMEOUT;
        bool interrupted = false;
        for (;;) {
            result = wait(entry.lowerId, sliceNs);
            if (result != VK_TIMEOUT) break;
            if (Clock::now() - entered >= std::chrono::nanoseconds{budgetNs}) break;
            if (stopRequested()) {
                interrupted = true;
                break;
            }
        }
        // A wait cut short by stop() is not an observation.
        if (interrupted) return;
        const auto done = Clock::now();
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
            sawSuccess.store(true, std::memory_order_release);
        emit(OnScreenRecord{entry.context, entry.lowerId, result, nanoseconds(entered), nanoseconds(done),
            droppedCount()});
        if (result < 0) {
            // Out of date, surface lost or any other error: no further waits for this swapchain.
            const std::lock_guard lock(mutex);
            dead = true;
            count = 0;
        }
    }
}

} // namespace fgvk::layer

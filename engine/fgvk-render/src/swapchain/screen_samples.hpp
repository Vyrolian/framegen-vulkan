/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

// OS1: the screen samples that drive the automatic output correction. A sample is the moment an output
// reached the screen: from the on-screen watcher (Xwayland, plain X11, gamescope) or from the compositor's
// wp_presentation feedback (native Wayland). Producers push without waiting for the consumer; the worker
// reads the ring in present order. Nothing here touches Vulkan or Wayland, so the rules are unit-tested.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <time.h>

namespace fgvk::layer {

// One output on screen: its batch's context and source, whether it was generated, and when it reached the
// screen in steady-clock nanoseconds (the clock of the output records' started_ns). GT1: startedNs is when its
// lower present started, and targetNs its target tick (0 when it was not snapped). Only the watcher fills them.
struct ScreenSample {
    uint64_t context{};
    uint64_t source{};
    bool generated{};
    int64_t screenNs{};
    int64_t targetNs{};
    int64_t startedNs{};
};

// Wayland timestamps on CLOCK_MONOTONIC share the steady clock's domain; no other clock gives a sample.
[[nodiscard]] constexpr bool screenSampleClockSupported(const uint32_t clockId) noexcept {
    return clockId == static_cast<uint32_t>(CLOCK_MONOTONIC);
}

// Bounded ring of screen samples with one reader, the worker that corrects the outputs. push never blocks on
// anything but its mutex and never allocates. When the ring is full the oldest unread sample is dropped and
// counted at push. close() ends the ring: a push after it is a no-op, and accepting() reports that.
class ScreenSampleRing {
public:
    static constexpr size_t capacity = 256;

    void push(const ScreenSample& sample) noexcept {
        const std::lock_guard lock(mutex);
        if (closed) return;
        if (tail - floor == capacity) {
            ++floor;
            ++droppedCount;
        }
        slots[tail % capacity] = sample;
        ++tail;
    }

    // Copies up to `max` samples from *cursor, in push order, and advances the cursor. A cursor behind the
    // oldest unread sample resumes there; the samples in between were dropped and counted by push.
    size_t read(uint64_t* const cursor, ScreenSample* const out, const size_t max) noexcept {
        const std::lock_guard lock(mutex);
        if (*cursor < floor) *cursor = floor;
        if (*cursor > tail) *cursor = tail;
        size_t count = 0;
        while (count < max && *cursor < tail) out[count++] = slots[(*cursor)++ % capacity];
        floor = *cursor;
        return count;
    }

    [[nodiscard]] uint64_t dropped() const noexcept {
        const std::lock_guard lock(mutex);
        return droppedCount;
    }
    void close() noexcept {
        const std::lock_guard lock(mutex);
        closed = true;
    }
    [[nodiscard]] bool accepting() const noexcept {
        const std::lock_guard lock(mutex);
        return !closed;
    }

private:
    mutable std::mutex mutex;
    std::array<ScreenSample, capacity> slots{};
    uint64_t tail{};
    // The oldest sample not yet read or dropped. Pushes count unread samples as tail - floor.
    uint64_t floor{};
    uint64_t droppedCount{};
    bool closed{};
};

// OS1: which producer drives one context's correction. Screen mode starts with the first screen sample of
// the context and ends after timeoutNs without one (steady clock, checked at each observe). Each switch
// returns true once, so the caller forgets its pending markers there.
class ScreenOffsetMode {
public:
    static constexpr int64_t timeoutNs = 1'000'000'000;

    [[nodiscard]] bool screen() const noexcept { return on; }
    // A screen sample was read at nowNs. True when this starts screen mode.
    [[nodiscard]] bool sample(const int64_t nowNs) noexcept {
        lastNs = nowNs;
        if (on) return false;
        on = true;
        return true;
    }
    // Checked at each observe. True when screen mode ends for lack of samples.
    [[nodiscard]] bool expire(const int64_t nowNs) noexcept {
        if (!on || nowNs - lastNs < timeoutNs) return false;
        on = false;
        return true;
    }

private:
    bool on{};
    int64_t lastNs{};
};

// OS1: the producer of the screen samples of one lower swapchain.
enum class ScreenSource : uint8_t { None, Watcher, Wayland };

[[nodiscard]] constexpr const char* screenSourceName(const ScreenSource source) noexcept {
    switch (source) {
        case ScreenSource::Watcher: return "watcher";
        case ScreenSource::Wayland: return "wayland";
        case ScreenSource::None: break;
    }
    return "none";
}

struct ScreenCorrection {
    ScreenSource source{ScreenSource::None};
    const char* reason{"none"};
};

// The source rule. The correction needs the real offset and not FGVK_ONSCREEN_CORRECTION=0. Its batch identities
// come from the swapchain's source counter, which every present advances with or without the pacing profile, so
// the profile is not an input. Then the watcher when its gate is active, else native Wayland feedback when the
// surface has an observer, else none with the gate's reason.
[[nodiscard]] constexpr ScreenCorrection screenCorrectionSource(const bool realOffset, const bool onScreenEnabled,
        const bool watcherActive, const char* gateReason, const bool waylandFeedback) noexcept {
    if (!realOffset) return {ScreenSource::None, "real-offset-off"};
    if (!onScreenEnabled) return {ScreenSource::None, "env-off"};
    if (watcherActive) return {ScreenSource::Watcher, "active"};
    if (waylandFeedback) return {ScreenSource::Wayland, "active"};
    return {ScreenSource::None, gateReason ? gateReason : "gate"};
}

// FGVK_ONSCREEN_CORRECTION=0 turns the on-screen sources off; any other value leaves them on. Read once.
[[nodiscard]] inline bool onScreenCorrectionEnvironment() noexcept {
    static const bool enabled = [] {
        const char* text = std::getenv("FGVK_ONSCREEN_CORRECTION");
        return !(text && std::strcmp(text, "0") == 0);
    }();
    return enabled;
}

} // namespace fgvk::layer

/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include "events.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fgvk::measure {

// Queues records from the game thread and writes them from the reader, the device-destroy path and
// atexit. The game thread only copies a record into a vector under a short lock.
class MeasureLog {
public:
    static constexpr uint64_t kLimitBytes = uint64_t{1} << 30;

    MeasureLog();

    // Opens /tmp/fgvk-measure-<uid>-<pid>.log: O_CLOEXEC, O_NOFOLLOW, mode 0600.
    bool open() noexcept;

    // Game thread safe: counts the event and queues it. No formatting and no I/O.
    void push(const Event& event) noexcept;
    void setSession(int64_t pid, std::string device, bool calibrated) noexcept;
    void countFeedback() noexcept;

    // Formats and writes everything queued so far. Safe from any thread.
    void drain() noexcept;
    void writeSummary() noexcept;

private:
    struct Session {
        int64_t pid{};
        std::string device;
        bool calibrated{};
    };

    void writeLocked(std::string_view text) noexcept;

    std::mutex queueMutex_;
    std::vector<Event> events_;
    std::optional<Session> session_;

    std::mutex writeMutex_;
    std::vector<Event> pending_;
    std::string buffer_;
    int fd_{-1};
    int64_t owner_{};
    uint64_t bytes_{};
    bool full_{};

    std::atomic<uint64_t> presents_{};
    std::atomic<uint64_t> measured_{};
    std::atomic<uint64_t> unmeasured_{};
    std::atomic<uint64_t> feedbacks_{};
    std::atomic<uint64_t> presented_{};
    std::atomic<uint64_t> discarded_{};
};

}  // namespace fgvk::measure

/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "measure_log.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace fgvk::measure {

MeasureLog::MeasureLog() {
    events_.reserve(4096);
    pending_.reserve(4096);
}

bool MeasureLog::open() noexcept {
    char path[96];
    std::snprintf(path, sizeof path, "/tmp/fgvk-measure-%u-%d.log", static_cast<unsigned>(getuid()),
            static_cast<int>(getpid()));
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        std::fprintf(stderr, "framegen-vulkan: measure: cannot open %s: %s\n", path, std::strerror(errno));
        return false;
    }
    std::lock_guard write(writeMutex_);
    fd_ = fd;
    owner_ = static_cast<int64_t>(getpid());
    std::fprintf(stderr, "framegen-vulkan: measure: writing %s\n", path);
    return true;
}

void MeasureLog::push(const Event& event) noexcept {
    try {
        switch (event.kind) {
        case EventKind::Present:
            presents_.fetch_add(1, std::memory_order_relaxed);
            (event.measured ? measured_ : unmeasured_).fetch_add(1, std::memory_order_relaxed);
            break;
        case EventKind::Presented:
            presented_.fetch_add(1, std::memory_order_relaxed);
            break;
        case EventKind::Discarded:
            discarded_.fetch_add(1, std::memory_order_relaxed);
            break;
        case EventKind::Gpu:
        case EventKind::Clock:
            break;
        }
        const std::lock_guard lock(queueMutex_);
        events_.push_back(event);
    } catch (...) {
    }
}

void MeasureLog::setSession(int64_t pid, std::string device, bool calibrated) noexcept {
    try {
        const std::lock_guard lock(queueMutex_);
        session_ = Session{pid, std::move(device), calibrated};
    } catch (...) {
    }
}

void MeasureLog::countFeedback() noexcept {
    feedbacks_.fetch_add(1, std::memory_order_relaxed);
}

void MeasureLog::drain() noexcept {
    try {
        std::lock_guard write(writeMutex_);
        std::optional<Session> session;
        {
            const std::lock_guard lock(queueMutex_);
            // pending_ is empty here (cleared last drain), so swapping keeps both capacities.
            std::swap(events_, pending_);
            session = std::move(session_);
            session_.reset();
        }
        if (!pending_.empty() || session) {
            buffer_.clear();
            if (session) appendSession(buffer_, session->pid, session->device, session->calibrated);
            for (const Event& event : pending_) appendEvent(buffer_, event);
            writeLocked(buffer_);
        }
        pending_.clear();
    } catch (...) {
    }
}

void MeasureLog::writeSummary() noexcept {
    try {
        const Summary summary{
            presents_.load(std::memory_order_relaxed), measured_.load(std::memory_order_relaxed),
            unmeasured_.load(std::memory_order_relaxed), feedbacks_.load(std::memory_order_relaxed),
            presented_.load(std::memory_order_relaxed), discarded_.load(std::memory_order_relaxed)};
        std::lock_guard write(writeMutex_);
        buffer_.clear();
        appendSummary(buffer_, summary);
        writeLocked(buffer_);
    } catch (...) {
    }
}

void MeasureLog::writeLocked(std::string_view text) noexcept {
    if (fd_ < 0 || full_ || text.empty() || static_cast<int64_t>(getpid()) != owner_) return;
    if (bytes_ + text.size() > kLimitBytes) {
        full_ = true;
        return;
    }
    std::size_t done = 0;
    while (done < text.size()) {
        const ssize_t written = ::write(fd_, text.data() + done, text.size() - done);
        if (written < 0) {
            if (errno == EINTR) continue;
            full_ = true;
            return;
        }
        done += static_cast<std::size_t>(written);
    }
    bytes_ += text.size();
}

}  // namespace fgvk::measure

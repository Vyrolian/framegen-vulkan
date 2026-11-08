/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "mark.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace fgvk::cli::mark {

namespace {
    // The note is one record on one line: a newline or carriage return would split it.
    std::string joinNote(const std::vector<std::string_view>& words) {
        std::string note;
        for (const auto word : words) {
            if (!note.empty()) note += ' ';
            note += word;
        }
        for (char& character : note) {
            if (character == '\n' || character == '\r') character = ' ';
        }
        return note;
    }
}

int run(const std::vector<std::string_view>& words) {
    // The layer's log and this mark share CLOCK_MONOTONIC, so the analyser can align them.
    timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        std::cerr << "fgvk-cli: mark cannot read CLOCK_MONOTONIC: " << std::strerror(errno) << '\n';
        return EXIT_FAILURE;
    }
    const auto monotonicNs = static_cast<int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;

    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) {
        std::cerr << "fgvk-cli: mark needs XDG_RUNTIME_DIR, which is unset\n";
        return EXIT_FAILURE;
    }
    const std::string directory = std::string(runtime) + "/framegen-vulkan";
    if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
        std::cerr << "fgvk-cli: mark cannot create " << directory << ": " << std::strerror(errno) << '\n';
        return EXIT_FAILURE;
    }
    const std::string path = directory + "/marks.log";
    // O_NOFOLLOW refuses a symlink planted at the log path; O_APPEND keeps each mark's write whole.
    const int descriptor = ::open(path.c_str(),
        O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        std::cerr << "fgvk-cli: mark cannot open " << path << ": " << std::strerror(errno) << '\n';
        return EXIT_FAILURE;
    }

    const std::string line = "framegen-vulkan: mark monotonic_ns=" + std::to_string(monotonicNs) +
        " note=" + joinNote(words) + "\n";
    const ssize_t written = ::write(descriptor, line.data(), line.size());
    const bool complete = written == static_cast<ssize_t>(line.size());
    if (::close(descriptor) != 0 || !complete) {
        std::cerr << "fgvk-cli: mark could not append to " << path << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

}

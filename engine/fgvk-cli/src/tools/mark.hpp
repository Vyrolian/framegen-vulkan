/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <string_view>
#include <vector>

namespace fgvk::cli::mark {

    /// run the "mark" command: append one timestamped line to the marks log
    /// @param words the note words; they are joined with single spaces
    /// @return the process exit status
    int run(const std::vector<std::string_view>& words);

}

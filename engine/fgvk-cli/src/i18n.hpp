/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace fgvk::cli::i18n {

    enum class Language {
        English,
        PortugueseBrazil,
        PortuguesePortugal,
        Spanish,
    };

    inline constexpr std::array supported_language_codes{
        std::string_view{"en"},
        std::string_view{"pt-BR"},
        std::string_view{"pt-PT"},
        std::string_view{"es"},
    };

    struct Strings {
        std::string_view validation_success;
        std::string_view validation_missing_file;
        std::string_view validation_failed;
    };

    [[nodiscard]] const Strings& strings(Language language) noexcept;
    [[nodiscard]] std::optional<Language> language_from_code(
        std::string_view code
    ) noexcept;
    [[nodiscard]] std::string_view code(Language language) noexcept;

}

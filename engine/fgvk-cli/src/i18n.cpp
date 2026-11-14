/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "i18n.hpp"

using namespace fgvk::cli::i18n;

namespace {
    constexpr Strings english{
        .validation_success = "Validation success",
        .validation_missing_file = "Validation failed: configuration file does not exist",
        .validation_failed = "Validation failed: ",
    };

    constexpr Strings portuguese_brazil{
        .validation_success = "Validação concluída com sucesso",
        .validation_missing_file = "Falha na validação: arquivo de configuração não existe",
        .validation_failed = "Falha na validação: ",
    };

    constexpr Strings portuguese_portugal{
        .validation_success = "Validação concluída com sucesso",
        .validation_missing_file = "Falha na validação: ficheiro de configuração não existe",
        .validation_failed = "Falha na validação: ",
    };

    constexpr Strings spanish{
        .validation_success = "Validación completada correctamente",
        .validation_missing_file = "Falló la validación: el archivo de configuración no existe",
        .validation_failed = "Falló la validación: ",
    };
}

const Strings& fgvk::cli::i18n::strings(const Language language) noexcept {
    switch (language) {
        case Language::English:
            return english;
        case Language::PortugueseBrazil:
            return portuguese_brazil;
        case Language::PortuguesePortugal:
            return portuguese_portugal;
        case Language::Spanish:
            return spanish;
    }
    return english;
}

std::optional<Language> fgvk::cli::i18n::language_from_code(
        const std::string_view code_value) noexcept {
    if (code_value == "en")
        return Language::English;
    if (code_value == "pt-BR")
        return Language::PortugueseBrazil;
    if (code_value == "pt-PT")
        return Language::PortuguesePortugal;
    if (code_value == "es")
        return Language::Spanish;
    return std::nullopt;
}

std::string_view fgvk::cli::i18n::code(const Language language) noexcept {
    switch (language) {
        case Language::English:
            return supported_language_codes.at(0);
        case Language::PortugueseBrazil:
            return supported_language_codes.at(1);
        case Language::PortuguesePortugal:
            return supported_language_codes.at(2);
        case Language::Spanish:
            return supported_language_codes.at(3);
    }
    return supported_language_codes.front();
}

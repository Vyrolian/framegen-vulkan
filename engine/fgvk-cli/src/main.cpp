/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "arguments.hpp"
#include "i18n.hpp"
#include "tools/inspect_dll.hpp"
#include "tools/mark.hpp"
#include "tools/validate.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <getopt.h> // NOLINT (IWYU)

using namespace fgvk::cli;

namespace {
    /// print usage information
    void usage(const std::string& prog) {
        std::cerr <<
R"(Validate and inspect framegen-vulkan.

USAGE:
    )" << prog << R"( [GLOBAL OPTIONS] <COMMAND> [OPTIONS] [ARGS]

GLOBAL OPTIONS:
    --lang <LANG>                       Output language: en, pt-BR, pt-PT, es
    -h, --help                          Show this help

COMMANDS:
    validate    Validate a configuration file
    mark        Append a timestamped hitch mark to $XDG_RUNTIME_DIR/framegen-vulkan/marks.log
    inspect-dll Inspect and fingerprint licensed model-resource compatibility

SUBCOMMAND OPTIONS:

    validate
        -c, --config <PATH>             Optional path to the configuration file

    inspect-dll
        -d, --dll <PATH>                Path to Lossless.dll
            --lsfg                     Probe the selected LSFG FP16 model; emits JSON
            --no-fp16                  With --lsfg, select FP32
)" << '\n';
    }

    /// parse the validate command options
    [[noreturn]] void on_validate(int argc, char** argv,
            const i18n::Language language, const std::string& program) {
        validate::Options opts{};

        const std::array<option, 3> GETOPT {{
            { "config", required_argument, nullptr, 'c' },
            { nullptr,        no_argument, nullptr,  0  }
        }};

        int c{0};
        while ((c = getopt_long(argc, argv, "c:", GETOPT.data(), nullptr)) != -1) {
            switch (c) {
                case 'c':
                    opts.config.emplace(optarg);
                    break;
                case '?':
                default:
                    usage(program);
                    std::exit(EXIT_FAILURE);
            }
        }

        if (optind < argc) {
            usage(program);
            std::exit(EXIT_FAILURE);
        }

        std::exit(validate::run(opts, language));
    }

    [[noreturn]] void on_inspect_dll(
            int argc, char** argv, const std::string& program) {
        inspect_dll::Options opts{};
        const std::array<option, 4> GETOPT {{
            { "dll", required_argument, nullptr, 'd' },
            { "lsfg", no_argument, nullptr, 'f' },
            { "no-fp16", no_argument, nullptr, 'A' },
            { nullptr, no_argument, nullptr, 0 }
        }};
        int option{0};
        while ((option = getopt_long(
                argc, argv, "d:", GETOPT.data(), nullptr)) != -1) {
            switch (option) {
                case 'd':
                    opts.dll = optarg;
                    break;
                case 'f':
                    opts.lsfg = true;
                    break;
                case 'A':
                    opts.allowFp16 = false;
                    break;
                case '?':
                default:
                    usage(program);
                    std::exit(EXIT_FAILURE);
            }
        }
        if (optind < argc || opts.dll.empty() ||
                (!opts.allowFp16 && !opts.lsfg)) {
            usage(program);
            std::exit(EXIT_FAILURE);
        }
        std::exit(inspect_dll::run(opts));
    }
}

int main(int argc, char** argv) {
    const std::string program{argv[0]};
    std::vector<std::string_view> arguments;
    if (argc > 1)
        arguments.reserve(static_cast<size_t>(argc - 1));
    for (int index = 1; index < argc; ++index)
        arguments.emplace_back(argv[index]);

    const GlobalArgumentResult parsed = parse_global_arguments(
        std::span<const std::string_view>{arguments}
    );
    if (const auto* error = std::get_if<std::string>(&parsed)) {
        std::cerr << "fgvk-cli: " << *error << "\n\n";
        usage(program);
        return EXIT_FAILURE;
    }
    const GlobalArguments global = std::get<GlobalArguments>(parsed);
    if (global.show_help) {
        usage(program);
        return EXIT_SUCCESS;
    }

    const int command_offset = static_cast<int>(global.command_index) + 1;
    const int command_argc = argc - command_offset;
    char** command_argv = argv + command_offset;
    const std::string command{command_argv[0]};

    if (command == "validate")
        on_validate(command_argc, command_argv, global.language, program);
    else if (command == "inspect-dll")
        on_inspect_dll(command_argc, command_argv, program);
    else if (command == "mark") {
        std::vector<std::string_view> words;
        for (int index = 1; index < command_argc; ++index)
            words.emplace_back(command_argv[index]);
        std::exit(mark::run(words));
    }

    usage(program);
    return EXIT_FAILURE;
}

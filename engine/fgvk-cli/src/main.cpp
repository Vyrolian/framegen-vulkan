/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "arguments.hpp"
#include "i18n.hpp"
#include "tools/benchmark.hpp"
#include "tools/debug.hpp"
#include "tools/inspect_dll.hpp"
#include "tools/mark.hpp"
#include "tools/quality.hpp"
#include "tools/validate.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <getopt.h> // NOLINT (IWYU)

using namespace fgvk::cli;

namespace {
    template<typename Number>
    Number numericArgument(
            const char* const text, const std::string_view option) {
        Number value{};
        const char* const begin = text[0] == '+' ? text + 1 : text;
        const char* const end = text + std::strlen(text);
        const auto [parsed, error] = std::from_chars(begin, end, value);
        const bool nonFinite = [](const Number parsedValue) {
            if constexpr (std::is_floating_point_v<Number>)
                return !std::isfinite(parsedValue);
            return false;
        }(value);
        if (begin == end || error != std::errc{} || parsed != end || nonFinite) {
            std::cerr << "error: " << option
                << " requires a valid finite number: " << text << '\n';
            std::exit(EXIT_FAILURE);
        }
        return value;
    }

    /// print usage information
    void usage(const std::string& prog) {
        std::cerr <<
R"(Validate, benchmark, debug, and inspect framegen-vulkan.

USAGE:
    )" << prog << R"( [GLOBAL OPTIONS] <COMMAND> [OPTIONS] [ARGS]

GLOBAL OPTIONS:
    --lang <LANG>                       Output language: en, pt-BR, pt-PT, es
    -h, --help                          Show this help

COMMANDS:
    validate    Validate a configuration file
    mark        Append a timestamped hitch mark to $XDG_RUNTIME_DIR/framegen-vulkan/marks.log
    benchmark   Run a benchmark
    debug       Run fgvk on a set of images
    inspect-dll Inspect and fingerprint licensed model-resource compatibility
    quality-regression
                Run a procedural LSFG image-quality regression
    synchronization-validation-canary
                Record an intentional hazard to prove synchronization validation

SUBCOMMAND OPTIONS:

    validate
        -c, --config <PATH>             Optional path to the configuration file

    benchmark & debug
        -d, --dll <PATH>                Path to Lossless.dll
        -a, --allow-fp16                Request FP16 arithmetic (default)
            --no-fp16                  Use FP32 arithmetic
        -w, --width <INT>               Width of the input frames
        -h, --height <INT>              Height of the input frames
        -f, --flow <FLOAT>              Flow scale
        -m, --multiplier <INT>          Multiplier
        -p, --performance-mode          Use performance mode
        -g, --gpu <STRING>              GPU to use

    inspect-dll
        -d, --dll <PATH>                Path to Lossless.dll
            --lsfg                     Probe the selected LSFG FP16 model; emits JSON
            --no-fp16                  With --lsfg, select FP32

    benchmark
        -t, --duration <SECONDS>        Benchmark duration in seconds
            --profile                  Separate CPU/GPU diagnostic mode; ignores duration
            --profile-samples <INT>     Measured iterations, 1-10000 (default 200)
            --profile-warmup <INT>      Warm-up iterations, 6-10000 (default 200)

    debug
        <folder>                        Path to the debug frames

    quality-regression
        -d, --dll <PATH>                Path to Lossless.dll
        -a, --allow-fp16                Request FP16 arithmetic (default)
            --no-fp16                  Use FP32 arithmetic
        -g, --gpu <STRING>              GPU to use
        -o, --output <DIRECTORY>        Write generated/reference PPM artifacts
        -s, --scene <NAME>              Procedural scene name
            --sequence-plan <PLAN>     Long temporal sequence; semicolon-separated frames
            --width/--height <PIXELS>   Temporal sequence extents (default 321x181)
        -t, --interpolation <FLOAT>     Generated timestamp between 0 and 1
        -f, --flow <FLOAT>              Flow scale from 0.25 to 1.0
        -p, --performance-mode          Use the lighter LSFG model

    synchronization-validation-canary
        -g, --gpu <STRING>              GPU to use
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

    /// parse the benchmark command options
    [[noreturn]] void on_benchmark(int argc, char** argv,
            const i18n::Language language, const std::string& program) {
        benchmark::Options opts{};
        bool profileControlsExplicit{false};

        const std::array<option, 14> GETOPT {{
            { "dll",              required_argument, nullptr, 'd' },
            { "allow-fp16",       no_argument,       nullptr, 'a' },
            { "no-fp16",          no_argument,       nullptr, 'A' },
            { "width",            required_argument, nullptr, 'w' },
            { "height",           required_argument, nullptr, 'h' },
            { "flow",             required_argument, nullptr, 'f' },
            { "multiplier",       required_argument, nullptr, 'm' },
            { "performance-mode",       no_argument, nullptr, 'p' },
            { "gpu",              required_argument, nullptr, 'g' },
            { "duration",         required_argument, nullptr, 't' },
            { "profile",          no_argument,       nullptr, 1000 },
            { "profile-samples",  required_argument, nullptr, 1001 },
            { "profile-warmup",   required_argument, nullptr, 1002 },
            { nullptr,                  no_argument, nullptr,  0  }
        }};

        int c{0};
        while ((c = getopt_long(argc, argv, "d:aw:h:f:m:pg:t:", GETOPT.data(), nullptr)) != -1) {
            switch (c) {
                case 'd':
                    opts.dll.emplace(optarg);
                    break;
                case 'a':
                    opts.allow_fp16 = true;
                    break;
                case 'A':
                    opts.allow_fp16 = false;
                    break;
                case 'w':
                    opts.width = numericArgument<int>(optarg, "--width");
                    break;
                case 'h':
                    opts.height = numericArgument<int>(optarg, "--height");
                    break;
                case 'f':
                    opts.flow = numericArgument<float>(optarg, "--flow");
                    break;
                case 'm':
                    opts.multiplier = numericArgument<int>(
                        optarg, "--multiplier"
                    );
                    break;
                case 'p':
                    opts.performance_mode = true;
                    break;
                case 'g':
                    opts.gpu.emplace(optarg);
                    break;
                case 't':
                    opts.duration = numericArgument<int>(optarg, "--duration");
                    break;
                case 1000:
                    opts.profile = true;
                    break;
                case 1001:
                    opts.profile_samples = numericArgument<int>(optarg, "--profile-samples");
                    profileControlsExplicit = true;
                    break;
                case 1002:
                    opts.profile_warmup = numericArgument<int>(optarg, "--profile-warmup");
                    profileControlsExplicit = true;
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

        if ((profileControlsExplicit && !opts.profile) || opts.profile_samples < 1 ||
                opts.profile_samples > 10000 || opts.profile_warmup < 6 || opts.profile_warmup > 10000) {
            std::cerr << "Frame profiling requires --profile, 1-10000 samples and 6-10000 warm-up iterations\n";
            std::exit(EXIT_FAILURE);
        }
        std::exit(benchmark::run(opts, language));
    }

    /// parse the debug command options
    [[noreturn]] void on_debug(int argc, char** argv,
            const i18n::Language language, const std::string& program) {
        debug::Options opts{};

        const std::array<option, 10> GETOPT {{
            { "dll",              required_argument, nullptr, 'd' },
            { "allow-fp16",       no_argument,       nullptr, 'a' },
            { "no-fp16",          no_argument,       nullptr, 'A' },
            { "width",            required_argument, nullptr, 'w' },
            { "height",           required_argument, nullptr, 'h' },
            { "flow",             required_argument, nullptr, 'f' },
            { "multiplier",       required_argument, nullptr, 'm' },
            { "performance-mode",       no_argument, nullptr, 'p' },
            { "gpu",              required_argument, nullptr, 'g' },
            { nullptr,                  no_argument, nullptr,  0  }
        }};

        int c{0};
        while ((c = getopt_long(argc, argv, "d:aw:h:f:m:pg:", GETOPT.data(), nullptr)) != -1) {
            switch (c) {
                case 'd':
                    opts.dll.emplace(optarg);
                    break;
                case 'a':
                    opts.allow_fp16 = true;
                    break;
                case 'A':
                    opts.allow_fp16 = false;
                    break;
                case 'w':
                    opts.width = numericArgument<int>(optarg, "--width");
                    break;
                case 'h':
                    opts.height = numericArgument<int>(optarg, "--height");
                    break;
                case 'f':
                    opts.flow = numericArgument<float>(optarg, "--flow");
                    break;
                case 'm':
                    opts.multiplier = numericArgument<int>(
                        optarg, "--multiplier"
                    );
                    break;
                case 'p':
                    opts.performance_mode = true;
                    break;
                case 'g':
                    opts.gpu.emplace(optarg);
                    break;
                case '?':
                default:
                    usage(program);
                    std::exit(EXIT_FAILURE);
            }
        }

        if ((optind + 1) != argc) {
            usage(program);
            std::exit(EXIT_FAILURE);
        }

        opts.path = argv[optind];

        std::exit(debug::run(opts, language));
    }

    /// parse the quality-regression command options
    [[noreturn]] void on_quality_regression(int argc, char** argv,
            const std::string& program) {
        quality::Options opts{};
        const std::array<option, 13> GETOPT {{
            { "width",            required_argument, nullptr, 1001 },
            { "height",           required_argument, nullptr, 1002 },
            { "sequence-plan",    required_argument, nullptr, 1000 },
            { "dll",              required_argument, nullptr, 'd' },
            { "allow-fp16",       no_argument,       nullptr, 'a' },
            { "no-fp16",          no_argument,       nullptr, 'A' },
            { "gpu",              required_argument, nullptr, 'g' },
            { "output",           required_argument, nullptr, 'o' },
            { "scene",            required_argument, nullptr, 's' },
            { "interpolation",    required_argument, nullptr, 't' },
            { "flow",             required_argument, nullptr, 'f' },
            { "performance-mode", no_argument,       nullptr, 'p' },
            { nullptr,             no_argument,       nullptr,  0  }
        }};

        int c{0};
        while ((c = getopt_long(
                argc, argv, "d:ag:o:s:t:f:p", GETOPT.data(), nullptr)) != -1) {
            switch (c) {
                case 1001:
                    opts.width = numericArgument<uint32_t>(optarg, "--width");
                    break;
                case 1002:
                    opts.height = numericArgument<uint32_t>(optarg, "--height");
                    break;
                case 1000:
                    opts.sequence_plan = optarg;
                    break;
                case 'd':
                    opts.dll.emplace(optarg);
                    break;
                case 'a':
                    opts.allow_fp16 = true;
                    break;
                case 'A':
                    opts.allow_fp16 = false;
                    break;
                case 'g':
                    opts.gpu.emplace(optarg);
                    break;
                case 'o':
                    opts.output.emplace(optarg);
                    break;
                case 's':
                    opts.scene = optarg;
                    break;
                case 't':
                    opts.interpolation = numericArgument<float>(
                        optarg, "--interpolation"
                    );
                    break;
                case 'f':
                    opts.flow_scale = numericArgument<float>(optarg, "--flow");
                    break;
                case 'p':
                    opts.performance_mode = true;
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
        std::exit(quality::run(opts));
    }

    /// Parse the synchronization-validation-canary command options.
    [[noreturn]] void on_synchronization_validation_canary(
            int argc, char** argv, const std::string& program) {
        quality::SynchronizationCanaryOptions opts{};
        const std::array<option, 2> GETOPT {{
            { "gpu", required_argument, nullptr, 'g' },
            { nullptr,   no_argument, nullptr,  0  }
        }};

        int c{0};
        while ((c = getopt_long(
                argc, argv, "g:", GETOPT.data(), nullptr)) != -1) {
            switch (c) {
                case 'g':
                    opts.gpu.emplace(optarg);
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
        std::exit(quality::runSynchronizationCanary(opts));
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
    else if (command == "benchmark")
        on_benchmark(command_argc, command_argv, global.language, program);
    else if (command == "debug")
        on_debug(command_argc, command_argv, global.language, program);
    else if (command == "quality-regression")
        on_quality_regression(command_argc, command_argv, program);
    else if (command == "synchronization-validation-canary")
        on_synchronization_validation_canary(
            command_argc, command_argv, program
        );
    else if (command == "mark") {
        std::vector<std::string_view> words;
        for (int index = 1; index < command_argc; ++index)
            words.emplace_back(command_argv[index]);
        std::exit(mark::run(words));
    }

    usage(program);
    return EXIT_FAILURE;
}

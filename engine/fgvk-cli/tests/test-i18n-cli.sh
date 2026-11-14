#!/usr/bin/env bash
# End-to-end contract for framegen-vulkan CLI language selection and command dispatch.
set -euo pipefail

cli=${1:?fgvk-cli path is required}
missing_config=/tmp/fgvk-cli-i18n-missing-config.toml

fail() {
    printf 'fgvk-cli i18n contract failed: %s\n' "$*" >&2
    exit 1
}

run_failure() {
    set +e
    command_output="$("$@" 2>&1)"
    command_status=$?
    set -e
    [[ $command_status -eq 1 ]] || fail "unexpected status $command_status: $command_output"
}

run_failure "$cli" validate --config "$missing_config"
[[ $command_output == 'Validation failed: configuration file does not exist' ]] ||
    fail "English validation output changed: $command_output"

run_failure "$cli" --lang pt-BR validate --config "$missing_config"
[[ $command_output == 'Falha na validação: arquivo de configuração não existe' ]] ||
    fail "Brazilian Portuguese validation output changed: $command_output"

run_failure "$cli" --lang=pt-PT validate --config "$missing_config"
[[ $command_output == 'Falha na validação: ficheiro de configuração não existe' ]] ||
    fail "European Portuguese validation output changed: $command_output"

run_failure "$cli" --lang es validate --config "$missing_config"
[[ $command_output == 'Falló la validación: el archivo de configuración no existe' ]] ||
    fail "Spanish validation output changed: $command_output"

run_failure "$cli" --lang invalid validate
[[ $command_output == *"unsupported language 'invalid'"* ]] ||
    fail "invalid language was not rejected: $command_output"

run_failure "$cli" --lang
[[ $command_output == *'--lang requires en, pt-BR, pt-PT, or es'* ]] ||
    fail "missing language value was not rejected: $command_output"

help_output="$("$cli" --help 2>&1)" || fail "--help failed: $help_output"
[[ $help_output == *'inspect-dll'* ]] ||
    fail "framegen-vulkan inspect-dll command disappeared from help"
[[ $help_output != *'spatial-quality-regression'* &&
        $help_output != *'spatial-profile'* &&
        $help_output != *'combined-quality-regression'* ]] ||
    fail "retired scaling commands are still listed in help"

run_failure "$cli" inspect-dll --dll /tmp/fgvk-cli-missing-lossless.dll
[[ $command_output == *'framegen-vulkan model DLL inspection failed: failed to stat dll file'* ]] ||
    fail "missing DLL inspection did not fail cleanly: $command_output"

run_failure "$cli" inspect-dll --dll /tmp/fgvk-cli-missing-lossless.dll --ls1 ls1
[[ $command_output == *'USAGE:'* ]] ||
    fail "retired LS1 inspection option was not rejected: $command_output"

for retired in spatial-quality-regression spatial-profile combined-quality-regression; do
    run_failure "$cli" "$retired" --method native
    [[ $command_output == *'USAGE:'* ]] ||
        fail "retired scaling command $retired was not rejected: $command_output"
done

printf 'fgvk-cli i18n contract: PASS\n'

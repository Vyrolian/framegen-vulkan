#!/usr/bin/env bash
# Production CLI parsing with tool-entry probes; no Vulkan device or DLL.
set -euo pipefail

cli=${1:?precision probe path is required}

expect_precision() {
    local expected="$1"
    shift
    local output
    output="$("$cli" "$@" 2>&1)" || {
        printf 'CLI precision command failed: %s\n' "$output" >&2
        exit 1
    }
    [[ "$output" == "$expected" ]] || {
        printf 'CLI precision expected %s, got %s\n' "$expected" "$output" >&2
        exit 1
    }
}

for command in spatial-quality-regression spatial-profile combined-quality-regression; do
    if "$cli" "$command" >/dev/null 2>&1; then
        printf 'Retired scaling command accepted: %s\n' "$command" >&2
        exit 1
    fi
done

expect_precision fp16-allowed inspect-dll --dll "synthetic input" --lsfg
expect_precision fp32 inspect-dll --dll "synthetic input" --lsfg --no-fp16
for invalid in "--no-fp16" "--lsfg --ls1 ls1"; do
    read -r -a args <<< "$invalid"
    if "$cli" inspect-dll --dll "synthetic input" "${args[@]}" >/dev/null 2>&1; then
        printf 'Conflicting inspection options accepted: %s\n' "$invalid" >&2
        exit 1
    fi
done

printf 'CLI precision contract: PASS\n'

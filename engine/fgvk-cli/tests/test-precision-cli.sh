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

for command in benchmark debug quality-regression; do
    arguments=()
    [[ "$command" != debug ]] || arguments=("frames with spaces")
    expect_precision fp16-allowed "$command" "${arguments[@]}"
    expect_precision fp32 "$command" --no-fp16 "${arguments[@]}"
    expect_precision fp16-allowed "$command" --allow-fp16 "${arguments[@]}"
    expect_precision fp16-allowed "$command" -a "${arguments[@]}"
    expect_precision fp32 "$command" --allow-fp16 --no-fp16 "${arguments[@]}"
    expect_precision fp16-allowed "$command" --no-fp16 --allow-fp16 "${arguments[@]}"
done

expect_precision 'profile samples=200 warmup=200' benchmark --profile
expect_precision 'profile samples=11 warmup=6' benchmark --profile --profile-samples 11 --profile-warmup 6
for arguments in '--profile-hdr-reduced-precision' '--profile --profile-hdr-reduced-precision' '--profile-hdr10' '--profile-samples 10' '--profile --profile-samples 0' '--profile --profile-samples 10001' '--profile --profile-warmup 5' '--profile --profile-warmup 10001'; do
    read -r -a args <<< "$arguments"
    if "$cli" benchmark "${args[@]}" >/dev/null 2>&1; then
        printf 'Invalid frame-profile options accepted: %s\n' "$arguments" >&2
        exit 1
    fi
done

for arguments in '--hdr10' '--hdr-reduced-precision' '--hdr-white-nits 203'; do
    read -r -a args <<< "$arguments"
    if "$cli" quality-regression "${args[@]}" >/dev/null 2>&1; then
        printf 'Removed colour options accepted: %s\n' "$arguments" >&2
        exit 1
    fi
done

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

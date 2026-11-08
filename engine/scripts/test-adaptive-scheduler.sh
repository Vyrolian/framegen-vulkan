#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sanitizer_mode="${FGVK_ENABLE_SANITIZERS:-OFF}"

case "$sanitizer_mode" in
    ON|OFF) ;;
    *)
        echo "FGVK_ENABLE_SANITIZERS must be ON or OFF" >&2
        exit 2
        ;;
esac

for command in cmake ctest; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required command not found: $command" >&2
        exit 1
    fi
done

build_root="$(mktemp -d "${TMPDIR:-/tmp}/fgvk-adaptive-tests.XXXXXX")"
cleanup() {
    rm -rf "$build_root"
}
trap cleanup EXIT

generator="Unix Makefiles"
if command -v ninja >/dev/null 2>&1; then
    generator="Ninja"
fi

cmake -S "$repo_root" -B "$build_root" -G "$generator" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DFGVK_BUILD_VK_LAYER=OFF \
    -DFGVK_BUILD_CLI=OFF \
    -DFGVK_ENABLE_SANITIZERS="$sanitizer_mode" \
    -DBUILD_TESTING=ON
cmake --build "$build_root" --target fgvk-portable-policy-test-binaries
ctest --test-dir "$build_root" --output-on-failure \
    -L '^portable-policy$'

echo "framegen-vulkan portable policy tests passed."

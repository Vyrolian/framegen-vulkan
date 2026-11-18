#!/bin/sh
# Verify that engine/dist/arch/PKGBUILD is still pinned to the release that
# framegen-vulkan validated.
#
# engine/renderer-release.json remote_binary[0] is written by the release flow only after
# the native build and its gates pass, so it is the authoritative version, URL
# and checksum for the host archive this package repackages. This script fails
# when the PKGBUILD drifts from that pin.
#
# It is read-only and needs no network access.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
repo_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd -P)
pkgbuild="${1:-$script_dir/PKGBUILD}"
install_script="$script_dir/framegen-vulkan-bin.install"
pin_file="${2:-$repo_root/engine/renderer-release.json}"

fail() {
    printf 'check-release-pin: %s\n' "$1" >&2
    exit 1
}

[ -f "$pkgbuild" ] || fail "missing $pkgbuild"
[ -f "$install_script" ] || fail "missing $install_script"
[ -f "$pin_file" ] || fail "missing $pin_file"

python3 "$script_dir/sync-release-pin.py" --check "$pin_file" "$pkgbuild" ||
    fail "release pin verification failed"

grep -Fq -- 'install="${pkgname}.install"' "$pkgbuild" ||
    fail 'PKGBUILD does not declare install="${pkgname}.install"'
grep -Fq -- 'provides=("fgvk-renderer=${pkgver}")' "$pkgbuild" ||
    fail 'PKGBUILD does not provide the versioned fgvk-renderer package identity'
grep -Fq -- "conflicts=('fgvk-renderer')" "$pkgbuild" ||
    fail 'PKGBUILD does not conflict with the alternate fgvk-renderer package'
bash -n "$pkgbuild" || fail "$pkgbuild is not valid bash syntax"
sh -n "$install_script" || fail "$install_script is not valid POSIX sh syntax"

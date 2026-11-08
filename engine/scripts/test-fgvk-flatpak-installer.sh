#!/usr/bin/env bash
# Deterministic contract tests for the graphical framegen-vulkan Flatpak installer.
set -euo pipefail

installer="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/fgvk-install-flatpaks}"

fail() {
    echo "fgvk-flatpak-installer test failed: $*" >&2
    exit 1
}

[[ -x "$installer" ]] || fail "installer is not executable: $installer"
bash -n "$installer"

test_root="$(mktemp -d /tmp/fgvk-flatpak-installer-contract.XXXXXX)"
trap 'rm -rf -- "$test_root"' EXIT
package_root="$test_root/package"
fake_bin="$test_root/bin"
invocation_log="$test_root/flatpak.log"
mkdir -p "$package_root" "$fake_bin"
cp "$installer" "$package_root/Install framegen-vulkan Flatpak Extensions"
chmod 0755 "$package_root/Install framegen-vulkan Flatpak Extensions"
touch "$package_root/org.freedesktop.Platform.VulkanLayer.fgvkrender-24.08.flatpak"
printf '%s\n' \
    '#!/usr/bin/env bash' \
    'printf "%s\\n" "$*" > "$FGVK_TEST_FLATPAK_LOG"' \
    > "$fake_bin/flatpak"
chmod 0755 "$fake_bin/flatpak"

env -u DISPLAY -u WAYLAND_DISPLAY \
    PATH="$fake_bin:$PATH" \
    FGVK_TEST_FLATPAK_LOG="$invocation_log" \
    FGVK_INSTALLER_ASSUME_YES=1 \
    FGVK_FLATPAK_RUNTIME=24.08 \
    "$package_root/Install framegen-vulkan Flatpak Extensions" >/dev/null

package_root_physical="$(cd "$package_root" && pwd -P)"
expected="install --user --noninteractive $package_root_physical/org.freedesktop.Platform.VulkanLayer.fgvkrender-24.08.flatpak"
[[ "$(<"$invocation_log")" == "$expected" ]] ||
    fail "installer did not invoke the selected user-scoped Flatpak bundle"

touch "$package_root/org.freedesktop.Platform.VulkanLayer.fgvkrender-25.08.flatpak"
printf '1\n' | env -u DISPLAY -u WAYLAND_DISPLAY -u FGVK_FLATPAK_RUNTIME \
    PATH="$fake_bin:$PATH" \
    FGVK_TEST_FLATPAK_LOG="$invocation_log" \
    FGVK_INSTALLER_ASSUME_YES=1 \
    "$package_root/Install framegen-vulkan Flatpak Extensions" >"$test_root/terminal.log" 2>&1
[[ "$(<"$invocation_log")" == "$expected" ]] ||
    fail "terminal runtime menu polluted the selected bundle path"

rm "$invocation_log"
touch "$package_root/org.freedesktop.Platform.VulkanLayer.fgvkrender-26.08.flatpak"
env -u DISPLAY -u WAYLAND_DISPLAY \
    PATH="$fake_bin:$PATH" \
    FGVK_TEST_FLATPAK_LOG="$invocation_log" \
    FGVK_INSTALLER_ASSUME_YES=1 \
    FGVK_FLATPAK_RUNTIME=26.08 \
    "$package_root/Install framegen-vulkan Flatpak Extensions" >/dev/null
expected="install --user --noninteractive $package_root_physical/org.freedesktop.Platform.VulkanLayer.fgvkrender-26.08.flatpak"
[[ "$(<"$invocation_log")" == "$expected" ]] ||
    fail "installer did not discover the new runtime bundle"

rm "$invocation_log"
printf '%s\n' '#!/usr/bin/env bash' 'exit 1' > "$fake_bin/kdialog"
chmod 0755 "$fake_bin/kdialog"
env -u FGVK_FLATPAK_RUNTIME \
    PATH="$fake_bin:$PATH" DISPLAY=:test \
    FGVK_TEST_FLATPAK_LOG="$invocation_log" \
    FGVK_INSTALLER_ASSUME_YES=1 \
    "$package_root/Install framegen-vulkan Flatpak Extensions" >/dev/null
[[ ! -e "$invocation_log" ]] || fail "cancelling the runtime menu still invoked Flatpak"

printf '%s\n' 'fgvk-flatpak-installer contract test passed'

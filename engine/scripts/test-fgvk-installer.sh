#!/usr/bin/env bash
# Deterministic contract tests for the standalone framegen-vulkan installer.
set -euo pipefail

installer="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/fgvk-installer}"

fail() {
    echo "fgvk-installer test failed: $*" >&2
    exit 1
}

[[ -x "$installer" ]] || fail "installer is not executable: $installer"
bash -n "$installer"

test_root="$(mktemp -d /tmp/fgvk-installer-contract.XXXXXX)"
trap 'rm -rf -- "$test_root"' EXIT
export HOME="$test_root/home"
mkdir -p "$HOME"
package_root="$test_root/package"
install_prefix="$test_root/installed prefix"
config_home="$test_root/config"
mkdir -p "$package_root/bin" "$package_root/libexec/framegen-vulkan" "$package_root/share/framegen-vulkan"
cp "$installer" "$package_root/Install framegen-vulkan"
cp "$installer" "$package_root/bin/fgvk-installer"
chmod 0755 "$package_root/Install framegen-vulkan" "$package_root/bin/fgvk-installer"
printf '%s\n' '#!/usr/bin/env bash' 'exit 0' > "$package_root/bin/fgvk-cli"
printf '%s\n' '#!/usr/bin/env bash' 'exit 0' > "$package_root/bin/fgvk"
printf '%s\n' '#!/usr/bin/env bash' 'exit 0' > "$package_root/libexec/framegen-vulkan/fgvk-launch"
chmod 0755 "$package_root/bin/fgvk-cli" "$package_root/bin/fgvk" "$package_root/libexec/framegen-vulkan/fgvk-launch"
printf '%s\n' 'payload' > "$package_root/share/framegen-vulkan/payload.txt"
printf '%s\n' 'test-version' > "$package_root/framegen-vulkan-version.txt"
write_manifest() {
    (
        cd "$package_root"
        find bin libexec share -type f -print | LC_ALL=C sort | xargs sha256sum
    ) > "$package_root/framegen-vulkan-install-manifest.txt"
}
write_manifest

FGVK_INSTALL_PREFIX="$install_prefix" \
XDG_CONFIG_HOME="$config_home" \
FGVK_INSTALLER_ASSUME_YES=1 \
"$package_root/Install framegen-vulkan" --install >"$test_root/install.log"

[[ -x "$install_prefix/bin/fgvk-cli" ]] || fail "CLI was not installed"
[[ -x "$install_prefix/bin/fgvk" ]] || fail "fgvk entry point was not installed"
[[ -x "$install_prefix/libexec/framegen-vulkan/fgvk-launch" ]] ||
    fail "private launch helper was not installed under libexec"
[[ ! -e "$install_prefix/bin/fgvk-launch" ]] || fail "launch helper was installed on PATH"
grep -Fq 'For a native Steam or Proton game, use this Steam launch option:' \
    "$test_root/install.log" && fail "completion still describes the removed configuration app"
grep -Fq "\"$install_prefix/bin/fgvk\" %command%" \
    "$test_root/install.log" ||
    fail "installer completion did not show the selected prefix's fgvk entry point"
grep -Fq "$config_home/framegen-vulkan/conf.toml" "$test_root/install.log" ||
    fail "installer completion did not name the configuration file"
desktop_entry="$install_prefix/share/applications/framegen-vulkan.desktop"
[[ -f "$desktop_entry" ]] || fail "installer did not write the settings menu entry"
grep -Fxq "Exec=\"$install_prefix/bin/fgvk\"" "$desktop_entry" ||
    fail "menu entry does not open this prefix's fgvk"
grep -Fxq "share/applications/framegen-vulkan.desktop" \
    <(sed 's/^[0-9a-f]*  //' "$install_prefix/share/framegen-vulkan/installer/installed-files.sha256") ||
    fail "menu entry is not in the installer's ownership record"
[[ "$(find "$install_prefix/share/applications" -type f | wc -l)" == 1 && ! -e "$install_prefix/share/icons" ]] ||
    fail "installer wrote other desktop entries or icons"

dialog_bin="$test_root/dialog-bin"
dialog_capture="$test_root/kdialog.log"
dialog_install_prefix="$test_root/dialog-install"
mkdir -p "$dialog_bin"
printf '%s\n' \
    '#!/usr/bin/env bash' \
    'printf '\''%s\n'\'' "$@" >> "$FGVK_DIALOG_CAPTURE"' \
    'exit 0' > "$dialog_bin/kdialog"
chmod 0755 "$dialog_bin/kdialog"
PATH="$dialog_bin:$PATH" \
DISPLAY=:fgvk-installer-test \
FGVK_DIALOG_CAPTURE="$dialog_capture" \
FGVK_INSTALL_PREFIX="$dialog_install_prefix" \
XDG_CONFIG_HOME="$config_home" \
FGVK_INSTALLER_ASSUME_YES=0 \
"$package_root/Install framegen-vulkan" --install >/dev/null
grep -Fxq -- '--title' "$dialog_capture" ||
    fail "KDialog invocation did not set a title"
grep -Fxq 'framegen-vulkan' "$dialog_capture" ||
    fail "KDialog invocation did not use the framegen-vulkan title"
if grep -Fxq -- '--icon' "$dialog_capture"; then
    fail "KDialog invocation still requests an application icon"
fi
grep -Fxq -- '--yesno' "$dialog_capture" ||
    fail "KDialog confirmation was not exercised"
grep -Fxq -- '--msgbox' "$dialog_capture" ||
    fail "KDialog completion was not exercised"
[[ -f "$install_prefix/share/framegen-vulkan/installer/installed-files.sha256" ]] ||
    fail "installer state was not written"
grep -Fq '"owner": "standalone"' \
    "$install_prefix/share/framegen-vulkan/active-renderer.json" ||
    fail "installer did not select the standalone Renderer as active"
(
    cd "$install_prefix"
    sha256sum --check --status share/framegen-vulkan/installer/installed-files.sha256
) || fail "installed state does not describe the installed payload"

printf '%s\n' '#!/usr/bin/env bash' 'exit 42' > "$package_root/bin/fgvk-cli"
chmod 0755 "$package_root/bin/fgvk-cli"
write_manifest

FGVK_INSTALL_PREFIX="$install_prefix" \
XDG_CONFIG_HOME="$config_home" \
FGVK_INSTALLER_ASSUME_YES=1 \
"$package_root/Install framegen-vulkan" --install >/dev/null

grep -Fq 'exit 42' "$install_prefix/bin/fgvk-cli" || fail "update did not replace the CLI"
printf '%s\n' 'user modification' > "$install_prefix/share/framegen-vulkan/payload.txt"
rm -f -- "$install_prefix/bin/fgvk-cli"
mkdir -p "$config_home/framegen-vulkan"
printf '%s\n' 'version = 2' > "$config_home/framegen-vulkan/conf.toml"
printf '%s\n' 'retain diagnostic session' > "$config_home/framegen-vulkan/present-diagnostics.log"
printf '%s\n' 'unowned' > "$install_prefix/share/framegen-vulkan/unowned.txt"

HOME="$test_root" \
FGVK_INSTALL_PREFIX="$install_prefix" \
XDG_CONFIG_HOME="$config_home" \
FGVK_INSTALLER_ASSUME_YES=1 \
"$install_prefix/bin/fgvk-installer" --uninstall >/dev/null

[[ ! -e "$install_prefix/bin/fgvk-cli" ]] || fail "uninstaller left the managed CLI"
[[ ! -e "$install_prefix/bin/fgvk" ]] || fail "uninstaller left the managed fgvk entry point"
[[ ! -e "$desktop_entry" ]] || fail "uninstaller left the settings menu entry"
[[ ! -e "$install_prefix/libexec/framegen-vulkan/fgvk-launch" ]] ||
    fail "uninstaller left the private launch helper"
[[ ! -e "$install_prefix/share/framegen-vulkan/active-renderer.json" ]] ||
    fail "uninstaller left the active Renderer identity"
[[ -f "$install_prefix/share/framegen-vulkan/payload.txt" ]] ||
    fail "uninstaller removed a modified file"
[[ -f "$install_prefix/share/framegen-vulkan/unowned.txt" ]] ||
    fail "uninstaller removed a file outside its ownership record"
[[ -f "$config_home/framegen-vulkan/conf.toml" ]] || fail "uninstaller removed configuration by default"
[[ -f "$config_home/framegen-vulkan/present-diagnostics.log" ]] ||
    fail "uninstaller removed diagnostics by default"

printf '%s\n' 'payload' > "$package_root/share/framegen-vulkan/payload.txt"
rm -f -- "$package_root/share/framegen-vulkan/unowned.txt"
write_manifest
FGVK_INSTALL_PREFIX="$install_prefix" \
XDG_CONFIG_HOME="$config_home" \
FGVK_INSTALLER_ASSUME_YES=1 \
"$package_root/Install framegen-vulkan" --install >/dev/null
printf 'n\n' | HOME="$test_root" \
        DISPLAY= \
        WAYLAND_DISPLAY= \
        FGVK_INSTALL_PREFIX="$install_prefix" \
        XDG_CONFIG_HOME="$config_home" \
        FGVK_INSTALLER_ASSUME_YES=0 \
        "$install_prefix/bin/fgvk-installer" --uninstall \
        >"$test_root/declined-uninstall.log" 2>&1
[[ -x "$install_prefix/bin/fgvk-cli" ]] ||
    fail "declining the uninstall removed the managed CLI"

HOME="$test_root" \
FGVK_INSTALL_PREFIX="$install_prefix" \
XDG_CONFIG_HOME="$config_home" \
FGVK_INSTALLER_ASSUME_YES=1 \
"$install_prefix/bin/fgvk-installer" --uninstall >/dev/null

[[ ! -e "$install_prefix/bin/fgvk-cli" ]] ||
    fail "default uninstaller left the managed CLI"
[[ ! -e "$install_prefix/share/framegen-vulkan/active-renderer.json" ]] ||
    fail "default uninstaller left the active Renderer identity"
[[ -f "$config_home/framegen-vulkan/present-diagnostics.log" ]] ||
    fail "shared Renderer uninstall removed diagnostics without confirmation"

# An invalid late payload must not partially update an existing installation.
rollback_prefix="$test_root/rollback"
export FGVK_INSTALL_PREFIX="$rollback_prefix" FGVK_INSTALLER_ASSUME_YES=1
printf 'obsolete payload\n' > "$package_root/bin/obsolete"
mkdir -p "$package_root/share/framegen-vulkan/obsolete-dir/nested"
printf 'obsolete nested payload\n' > "$package_root/share/framegen-vulkan/obsolete-dir/nested/old.txt"
write_manifest
"$package_root/Install framegen-vulkan" --install >/dev/null
cp "$rollback_prefix/share/framegen-vulkan/installer/installed-files.sha256" "$test_root/before.sha256"
rm "$package_root/bin/obsolete"
rm -r "$package_root/share/framegen-vulkan/obsolete-dir"
printf 'new payload\n' > "$package_root/bin/new-file"
printf '%s\n' '#!/usr/bin/env bash' 'exit 77' > "$package_root/bin/fgvk-cli"
write_manifest
late_payload="$package_root/share/framegen-vulkan/payload.txt"
cp "$late_payload" "$test_root/late-payload"
printf '%s\n' 'corrupt late file' > "$late_payload"
if "$package_root/Install framegen-vulkan" --install >"$test_root/rejected.log" 2>&1; then
    fail "installer accepted a corrupted late package file"
fi
(
    cd "$rollback_prefix"
    sha256sum --check --status "$test_root/before.sha256"
) || fail "failed package verification partially replaced the old installation"
cp "$test_root/late-payload" "$late_payload"

# Fail the last identity rename once, after binaries and manifests changed.
mkdir -p "$test_root/failing-bin"
cat > "$test_root/failing-bin/mv" <<'EOF'
#!/usr/bin/env bash
if [[ "${@: -1}" == "$FGVK_TEST_FAIL_DESTINATION" && ! -e "$FGVK_TEST_FAILURE_MARKER" ]]; then
    touch "$FGVK_TEST_FAILURE_MARKER"
    exit 1
fi
exec /usr/bin/mv "$@"
EOF
chmod 0755 "$test_root/failing-bin/mv"
cp "$rollback_prefix/share/framegen-vulkan/active-renderer.json" "$test_root/before-active.json"
if PATH="$test_root/failing-bin:$PATH" \
        FGVK_TEST_FAIL_DESTINATION="$rollback_prefix/share/framegen-vulkan/active-renderer.json" \
        FGVK_TEST_FAILURE_MARKER="$test_root/failed-once" \
        "$package_root/Install framegen-vulkan" --install >"$test_root/rollback.log" 2>&1; then
    fail "installer reported success after the active identity rename failed"
fi
[[ -f "$test_root/failed-once" ]] || fail "test never reached the final identity write"
(
    cd "$rollback_prefix"
    sha256sum --check --status "$test_root/before.sha256"
) || fail "late failure did not roll back installed files and their ownership record"
[[ ! -e "$rollback_prefix/bin/new-file" ]] || fail "late failure left a new payload file behind"
cmp "$test_root/before-active.json" "$rollback_prefix/share/framegen-vulkan/active-renderer.json" ||
    fail "late failure changed the selected Renderer owner"
[[ -z "$(find "$rollback_prefix" -name '.fgvk-install-*' -o -name '*.fgvk-install.*')" ]] ||
    fail "installer left transaction files after rollback"

if ((EUID != 0)); then
    chmod 0500 "$rollback_prefix"
    if "$package_root/Install framegen-vulkan" --install >"$test_root/prefix-permissions.log" 2>&1; then
        fail "installer ignored a non-writable installation prefix"
    fi
    chmod 0755 "$rollback_prefix"
    grep -Fq 'previous installation was preserved' "$test_root/prefix-permissions.log" ||
        fail "installer did not report a staging permission failure"
    chmod 0500 "$rollback_prefix/bin"
    if "$package_root/Install framegen-vulkan" --install >"$test_root/permissions.log" 2>&1; then
        fail "installer ignored a non-writable destination directory"
    fi
    chmod 0755 "$rollback_prefix/bin"
    (
        cd "$rollback_prefix"
        sha256sum --check --status "$test_root/before.sha256"
    ) || fail "permission failure damaged the existing installation"
fi

"$package_root/Install framegen-vulkan" --install >/dev/null
[[ ! -e "$rollback_prefix/bin/obsolete" && -f "$rollback_prefix/bin/new-file" ]] ||
    fail "successful retry did not update the obsolete and new payloads"
[[ ! -e "$rollback_prefix/share/framegen-vulkan/obsolete-dir" && -d "$rollback_prefix/share/framegen-vulkan" ]] ||
    fail "successful retry left the obsolete payload's empty directories or removed their parent"
(
    cd "$rollback_prefix"
    sha256sum --check --status share/framegen-vulkan/installer/installed-files.sha256
) || fail "successful retry left an incorrect ownership record"

purge_prefix="$test_root/purge-install"
purge_config_home="$test_root/purge-config"
FGVK_INSTALL_PREFIX="$purge_prefix" \
XDG_CONFIG_HOME="$purge_config_home" \
"$package_root/Install framegen-vulkan" --install >/dev/null
mkdir -p "$purge_config_home/framegen-vulkan"
printf '%s\n' 'version = 2' > "$purge_config_home/framegen-vulkan/conf.toml"
printf '%s\n' 'private diagnostics' > \
    "$purge_config_home/framegen-vulkan/present-diagnostics.log"
FGVK_INSTALL_PREFIX="$purge_prefix" \
XDG_CONFIG_HOME="$purge_config_home" \
"$purge_prefix/bin/fgvk-installer" --uninstall --purge-configuration >/dev/null
[[ ! -e "$purge_config_home/framegen-vulkan" ]] ||
    fail "explicit configuration purge left profiles or diagnostics behind"

printf '%s\n' 'fgvk-installer contract test passed'

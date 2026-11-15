#!/usr/bin/env bash
# Deterministic contract tests for the standalone framegen-vulkan launcher.
set -euo pipefail

launcher="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/fgvk-launch}"

fail() {
    echo "fgvk-launch test failed: $*" >&2
    exit 1
}

if [[ ! -f "$launcher" ]]; then
    fail "launcher does not exist: $launcher"
fi

launcher_dir="$(cd -- "$(dirname -- "$launcher")" && pwd -P)"
# The helper is installed as <prefix>/libexec/framegen-vulkan/fgvk-launch.
install_prefix="$(cd -- "$launcher_dir/../.." && pwd -P)"
test_root="$(mktemp -d /tmp/fgvk-launch-contract.XXXXXX)"
trap 'rm -rf -- "$test_root"' EXIT
test_data_home="$test_root/data"
test_config_home="$test_root/config"
mkdir -p "$test_data_home" "$test_config_home"
export XDG_CONFIG_HOME="$test_config_home"
overlay_enabled_config="$test_root/overlay-enabled.conf"
printf '%s\n' 'version=1' 'disable_steam_overlay=0' > "$overlay_enabled_config"
# Gamescope sessions are detected from this variable alone. Clear it so every
# case below is a desktop launch; the early-exit case sets its own. The opt-in
# is cleared too, so only the cases that set it see it.
unset GAMESCOPE_WAYLAND_DISPLAY FGVK_GAMESCOPE
expected_layer_path="$install_prefix/share/framegen-vulkan/vulkan/implicit_layer.d"
if [[ ! -d "$expected_layer_path" && -d "$test_data_home/framegen-vulkan/vulkan/implicit_layer.d" ]]; then
    expected_layer_path="$test_data_home/framegen-vulkan/vulkan/implicit_layer.d"
elif [[ ! -d "$expected_layer_path" ]]; then
    expected_layer_path="$install_prefix/share/vulkan/implicit_layer.d"
fi

default_output="$({
    env -u ENABLE_FGVK -u DISABLE_FGVK \
        -u ENABLE_VKBASALT -u DISABLE_VKBASALT -u VKBASALT_CONFIG_FILE -u VKBASALT_CONFIG_RELOAD \
        -u DXVK_HDR \
        -u DISABLE_LSFG -u DISABLE_LSFGVK \
        -u FGVK_ALLOW_COMPETING_LAYERS \
        XDG_DATA_HOME="$test_data_home" \
        VK_IMPLICIT_LAYER_PATH="/caller/override" \
        VK_ADD_IMPLICIT_LAYER_PATH="/caller/additional" \
        VK_INSTANCE_LAYERS="VK_LAYER_existing:VK_LAYER_VKBASALT_post_processing" \
        FGVK_PROFILE="profile with spaces" \
        "$launcher" bash -c '
            printf "%s\n" \
                "${ENABLE_FGVK:-unset}" \
                "${DISABLE_LSFG:-unset}" \
                "${DISABLE_LSFGVK:-unset}" \
                "${DXVK_HDR:-unset}" \
                "${VK_IMPLICIT_LAYER_PATH:-unset}" \
                "${VK_ADD_IMPLICIT_LAYER_PATH:-unset}" \
                "${VK_INSTANCE_LAYERS:-unset}" \
                "${ENABLE_VKBASALT:-unset}" \
                "${DISABLE_VKBASALT:-unset}" \
                "${VKBASALT_CONFIG_FILE:-unset}" \
                "${VKBASALT_CONFIG_RELOAD:-unset}" \
                "${FGVK_PROFILE:-unset}" \
                "$1" "$2"
        ' _ "argument with spaces" '$literal'
} 2>&1)" || fail "default launch failed: $default_output"

expected_default="$(printf '1\n1\n1\nunset\n%s\nunset\nVK_LAYER_existing\nunset\n1\nunset\nunset\nprofile with spaces\nargument with spaces\n$literal' "$expected_layer_path")"
if [[ "$default_output" != "$expected_default" ]]; then
    fail "default environment or argument forwarding changed:\n$default_output"
fi

steam_overlay_dir="$test_data_home/vulkan/implicit_layer.d"
mkdir -p "$steam_overlay_dir"
touch "$steam_overlay_dir/steamoverlay_x86_64.json"
touch "$steam_overlay_dir/steamoverlay_i386.json"
desktop_overlay_output="$({
    FGVK_LAUNCH_CONFIG="$overlay_enabled_config" \
    ENABLE_VK_LAYER_VALVE_steam_overlay_1=1 \
        XDG_DATA_HOME="$test_data_home" \
        VK_LAYER_PATH="/caller/explicit" \
        VK_ADD_LAYER_PATH="/caller/additional-explicit" \
        VK_INSTANCE_LAYERS="VK_LAYER_existing:VK_LAYER_VALVE_steam_overlay_64" \
        "$launcher" bash -c '
            printf "%s\n" \
                "${ENABLE_FGVK:-unset}" \
                "${VK_INSTANCE_LAYERS:-unset}" \
                "${VK_LAYER_PATH:-unset}" \
                "${VK_ADD_LAYER_PATH:-unset}"
        '
} 2>&1)" || fail "Desktop Steam overlay preservation failed: $desktop_overlay_output"
expected_desktop_overlay="$(printf 'unset\nVK_LAYER_FGVK_render:VK_LAYER_VALVE_steam_overlay_64:VK_LAYER_VALVE_steam_overlay_32:VK_LAYER_existing\n%s:/caller/explicit\n/caller/additional-explicit' "$steam_overlay_dir")"
if [[ "$desktop_overlay_output" != "$expected_desktop_overlay" ]]; then
    fail "Desktop Steam overlay was not ordered after framegen-vulkan:\n$desktop_overlay_output"
fi

# A Gamescope session is not supported. The launcher must print one notice and
# exec the command with its environment untouched, so no framegen-vulkan
# setting, layer or preload change reaches the child.
gamescope_stderr="$test_root/gamescope.stderr"
gamescope_output="$(
    FGVK_LAUNCH_CONFIG="$overlay_enabled_config" \
    ENABLE_VK_LAYER_VALVE_steam_overlay_1=1 \
        GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 \
        WAYLAND_DISPLAY=gamescope-0 \
        XDG_DATA_HOME="$test_data_home" \
        "$launcher" bash -c '
            printf "%s\n" \
                "${ENABLE_FGVK:-unset}" \
                "${VK_IMPLICIT_LAYER_PATH:-unset}" \
                "${VK_INSTANCE_LAYERS:-unset}" \
                "${VK_LAYER_PATH:-unset}" \
                "${ENABLE_VK_LAYER_VALVE_steam_overlay_1:-unset}" \
                "${FGVK_LAUNCH_CONFIG:-unset}"
        ' 2> "$gamescope_stderr"
)" || fail "Gamescope early exit failed"
expected_gamescope="$(printf 'unset\nunset\nunset\nunset\n1\n%s' "$overlay_enabled_config")"
if [[ "$gamescope_output" != "$expected_gamescope" ]]; then
    fail "Gamescope session changed the launch environment:\n$gamescope_output"
fi
if [[ "$(cat "$gamescope_stderr")" != "framegen-vulkan: Gamescope sessions are not supported; starting without frame generation" ]]; then
    fail "Gamescope early-exit notice was not printed exactly once:\n$(cat "$gamescope_stderr")"
fi

# FGVK_GAMESCOPE=1 is the experimental opt-in. The same Gamescope session then
# takes the normal launch path with no launcher notice; only the exact value 1
# opts in, and every other value keeps the early exit above.
gamescope_optin_stderr="$test_root/gamescope-optin.stderr"
gamescope_optin_output="$(
    FGVK_LAUNCH_CONFIG="$overlay_enabled_config" \
        GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 \
        WAYLAND_DISPLAY=gamescope-0 \
        FGVK_GAMESCOPE=1 \
        XDG_DATA_HOME="$test_data_home" \
        "$launcher" bash -c '
            printf "%s\n" \
                "${ENABLE_FGVK:-unset}" \
                "${DISABLE_LSFG:-unset}" \
                "${FGVK_GAMESCOPE:-unset}" \
                "${GAMESCOPE_WAYLAND_DISPLAY:-unset}" \
                "${VK_IMPLICIT_LAYER_PATH:-unset}"
        ' 2> "$gamescope_optin_stderr"
)" || fail "Gamescope opt-in launch failed"
expected_gamescope_optin="$(printf '1\n1\n1\ngamescope-0\n%s' "$expected_layer_path")"
if [[ "$gamescope_optin_output" != "$expected_gamescope_optin" ]]; then
    fail "Gamescope opt-in did not reach the normal launch path:\n$gamescope_optin_output"
fi
if [[ -s "$gamescope_optin_stderr" ]]; then
    fail "Gamescope opt-in printed a launcher notice:\n$(cat "$gamescope_optin_stderr")"
fi
for gamescope_optin_value in 0 true yes ''; do
    gamescope_value_output="$(
        FGVK_GAMESCOPE="$gamescope_optin_value" \
            GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 \
            WAYLAND_DISPLAY=gamescope-0 \
            XDG_DATA_HOME="$test_data_home" \
            "$launcher" bash -c 'printf "%s\n" "${ENABLE_FGVK:-unset}"' \
            2> "$gamescope_stderr"
    )" || fail "Gamescope early exit failed for FGVK_GAMESCOPE='$gamescope_optin_value'"
    if [[ "$gamescope_value_output" != "unset" ]]; then
        fail "FGVK_GAMESCOPE='$gamescope_optin_value' opted in to a Gamescope session"
    fi
    if [[ "$(cat "$gamescope_stderr")" != "framegen-vulkan: Gamescope sessions are not supported; starting without frame generation" ]]; then
        fail "FGVK_GAMESCOPE='$gamescope_optin_value' changed the early-exit notice:\n$(cat "$gamescope_stderr")"
    fi
done

fake_flatpak="$test_root/flatpak"
printf '%s\n' '#!/usr/bin/env bash' \
    'printf "%s\n" "${VK_INSTANCE_LAYERS:-unset}" "${VK_LAYER_PATH:-unset}"' > "$fake_flatpak"
chmod +x "$fake_flatpak"
flatpak_overlay_output="$({
    FGVK_LAUNCH_CONFIG="$overlay_enabled_config" \
    ENABLE_VK_LAYER_VALVE_steam_overlay_1=1 \
        XDG_DATA_HOME="$test_data_home" \
        "$launcher" "$fake_flatpak" run example.App
} 2>&1)" || fail "Flatpak overlay boundary failed: $flatpak_overlay_output"
if [[ "$flatpak_overlay_output" != $'unset\nunset' ]]; then
    fail "Flatpak unexpectedly used Desktop Steam overlay preservation:\n$flatpak_overlay_output"
fi
rm -f "$steam_overlay_dir/steamoverlay_x86_64.json"
rm -f "$steam_overlay_dir/steamoverlay_i386.json"

# Load synthetic hooks through the actual dynamic linker, then check symbol
# visibility in the final application rather than just printing LD_PRELOAD.
preload_fixture="$(dirname -- "${BASH_SOURCE[0]}")/test-launch-preload.cpp"
preload_compiler="${CXX:-c++}"
mkdir -p "$test_root/preload/64" "$test_root/preload/32"
"$preload_compiler" -shared -fPIC -DFGVK_TEST_OVERLAY "$preload_fixture" \
    -o "$test_root/preload/64/gameoverlayrenderer.so"
cp "$test_root/preload/64/gameoverlayrenderer.so" "$test_root/preload/32/gameoverlayrenderer.so"
"$preload_compiler" -shared -fPIC -DFGVK_TEST_RETAINED "$preload_fixture" \
    -o "$test_root/preload/retained.so"
"$preload_compiler" "$preload_fixture" -ldl -o "$test_root/preload/probe"
for separator in ':' ' ' $'\t' $'\n' $'\r' $'\v' $'\f'; do
    preload_output="$(
        XDG_DATA_HOME="$test_data_home" \
        LD_PRELOAD="$test_root/preload/64/gameoverlayrenderer.so${separator}$test_root/preload/retained.so${separator}$test_root/preload/32/gameoverlayrenderer.so" \
        "$launcher" "$test_root/preload/probe" 2> "$test_root/preload/stderr"
    )" || fail "isolated preload launch failed"
    [[ "$preload_output" == 'overlay=0 retained=1' ]] ||
        fail "isolated launch retained an orphaned hook or lost another preload: $preload_output"
done
preload_output="$(
    XDG_DATA_HOME="$test_data_home" \
    LD_PRELOAD="$test_root/preload/64/gameoverlayrenderer.so" \
    "$launcher" "$test_root/preload/probe" 2> "$test_root/preload/stderr"
)" || fail "overlay-only preload launch failed"
[[ "$preload_output" == 'overlay=0 retained=0' ]] || fail "overlay-only hooks survived exec"
preload_output="$(
    XDG_DATA_HOME="$test_data_home" \
    LD_PRELOAD="$test_root/preload/64/gameoverlayrenderer.so:$test_root/preload/retained.so" \
    "$launcher" "$test_root/preload/probe" 2> "$test_root/preload/stderr"
)" || fail "default Steam preload removal failed"
[[ "$preload_output" == 'overlay=0 retained=1' ]] || fail "default launch retained Steam input hooks"
touch "$steam_overlay_dir/steamoverlay_x86_64.json"
preload_output="$(
    env -u GAMESCOPE_WAYLAND_DISPLAY \
    FGVK_LAUNCH_CONFIG="$overlay_enabled_config" \
    ENABLE_VK_LAYER_VALVE_steam_overlay_1=1 \
    XDG_DATA_HOME="$test_data_home" \
    LD_PRELOAD="$test_root/preload/64/gameoverlayrenderer.so:$test_root/preload/retained.so" \
    "$launcher" "$test_root/preload/probe" 2> "$test_root/preload/stderr"
)" || fail "paired Desktop overlay preload launch failed"
[[ "$preload_output" == 'overlay=1 retained=1' ]] || fail "paired Desktop overlay hooks were removed"
desktop_disabled_output="$(
    env -u GAMESCOPE_WAYLAND_DISPLAY \
    ENABLE_VK_LAYER_VALVE_steam_overlay_1=1 \
    XDG_DATA_HOME="$test_data_home" \
    VK_INSTANCE_LAYERS="VK_LAYER_existing:VK_LAYER_VALVE_steam_overlay_64:VK_LAYER_VALVE_steam_overlay_32" \
    LD_PRELOAD="$test_root/preload/64/gameoverlayrenderer.so:$test_root/preload/retained.so" \
    "$launcher" bash -c 'printf "%s\n" "${VK_INSTANCE_LAYERS:-unset}" "${DISABLE_VK_LAYER_VALVE_steam_overlay_1:-unset}"; exec "$1"' _ "$test_root/preload/probe" 2> "$test_root/preload/stderr"
)" || fail "Desktop overlay disable setting failed"
[[ "$desktop_disabled_output" == $'VK_LAYER_existing\n1\noverlay=0 retained=1' ]] || fail "Desktop overlay request bypassed the default Performance setting"
rm -f "$steam_overlay_dir/steamoverlay_x86_64.json"

allow_output="$({
    DISABLE_LSFG=1 DISABLE_LSFGVK=1 \
        DXVK_HDR=1 \
        XDG_DATA_HOME="$test_data_home" \
        VK_IMPLICIT_LAYER_PATH="/caller/override" \
        VK_ADD_IMPLICIT_LAYER_PATH="/caller/additional" \
        FGVK_ALLOW_COMPETING_LAYERS=1 \
        "$launcher" bash -c '
            printf "%s\n" \
                "${ENABLE_FGVK:-unset}" \
                "${DISABLE_LSFG:-unset}" \
                "${DISABLE_LSFGVK:-unset}" \
                "${DXVK_HDR:-unset}" \
                "${VK_IMPLICIT_LAYER_PATH:-unset}" \
                "${VK_ADD_IMPLICIT_LAYER_PATH:-unset}" \
                "${FGVK_ALLOW_COMPETING_LAYERS:-unset}"
        '
} 2>&1)" || fail "advanced opt-out launch failed: $allow_output"

expected_allow="$(printf '1\nunset\nunset\n1\n%s\nunset\nunset' "$expected_layer_path")"
if [[ "$allow_output" != "$expected_allow" ]]; then
    fail "advanced opt-out did not remove only the conflict guards:\n$allow_output"
fi

disable_output="$({
    DISABLE_FGVK=1 "$launcher" bash -c \
        'printf "%s\n" "${ENABLE_FGVK:-unset}" "${DISABLE_FGVK:-unset}"'
} 2>&1)" || fail "framegen-vulkan disable-gate forwarding failed: $disable_output"
if [[ "$disable_output" != $'1\n1' ]]; then
    fail "DISABLE_FGVK was not preserved:\n$disable_output"
fi

private_prefix="$test_root/private-prefix"
private_launcher="$private_prefix/libexec/framegen-vulkan/fgvk-launch"
private_fgvk_layer_dir="$private_prefix/share/framegen-vulkan/vulkan/implicit_layer.d"
private_vkbasalt_layer_dir="$private_prefix/share/framegen-vulkan/vulkan/vkbasalt.d"
private_vkbasalt_library64="$private_prefix/lib/vkbasalt/libvkbasalt.so"
private_vkbasalt_library32="$private_prefix/lib32/vkbasalt/libvkbasalt.so"
mkdir -p "$(dirname -- "$private_launcher")" "$private_fgvk_layer_dir" "$private_vkbasalt_layer_dir" \
    "$(dirname -- "$private_vkbasalt_library64")" \
    "$(dirname -- "$private_vkbasalt_library32")"
cp -- "$launcher" "$private_launcher"
chmod +x "$private_launcher"
touch "$private_fgvk_layer_dir/VkLayer_FGVK_render.json"
touch "$private_fgvk_layer_dir/VkLayer_FGVK_render.x86.json"
touch "$private_vkbasalt_layer_dir/vkBasalt.json"
touch "$private_vkbasalt_layer_dir/vkBasalt.x86.json"
touch "$private_vkbasalt_library64"
touch "$private_vkbasalt_library32"
vkbasalt_config="$test_root/vkBasalt.conf"
printf '%s\n' 'effects = cas' > "$vkbasalt_config"
touch "$steam_overlay_dir/steamoverlay_x86_64.json"
touch "$steam_overlay_dir/steamoverlay_i386.json"

# vkBasalt is no longer bundled. A leftover private payload, a vkBasalt config
# path and an ENABLE_VKBASALT=1 launch option must never join the launch: the
# request is ignored with one notice, and DISABLE_VKBASALT=1 stays exported.
ignored_vkbasalt_stderr="$test_root/ignored-vkbasalt.stderr"
ignored_vkbasalt_output="$(
    env -u GAMESCOPE_WAYLAND_DISPLAY -u WAYLAND_DISPLAY \
    FGVK_LAUNCH_CONFIG="$overlay_enabled_config" \
    ENABLE_VK_LAYER_VALVE_steam_overlay_1=1 \
        ENABLE_VKBASALT=1 \
        VKBASALT_CONFIG_FILE="$vkbasalt_config" \
        VK_INSTANCE_LAYERS="VK_LAYER_existing:VK_LAYER_VKBASALT_post_processing" \
        VK_IMPLICIT_LAYER_PATH="/caller/override" \
        VK_ADD_IMPLICIT_LAYER_PATH="/caller/additional" \
        XDG_DATA_HOME="$test_data_home" \
        "$private_launcher" bash -c '
            printf "%s\n" \
                "${ENABLE_FGVK:-unset}" \
                "${ENABLE_VKBASALT:-unset}" \
                "${DISABLE_VKBASALT:-unset}" \
                "${VKBASALT_CONFIG_FILE:-unset}" \
                "${VKBASALT_CONFIG_RELOAD:-unset}" \
                "${VK_INSTANCE_LAYERS:-unset}" \
                "${VK_IMPLICIT_LAYER_PATH:-unset}" \
                "${VK_ADD_IMPLICIT_LAYER_PATH:-unset}"
        ' 2> "$ignored_vkbasalt_stderr"
)" || fail "ignored ENABLE_VKBASALT request stopped the launch"
expected_ignored_vkbasalt="$(printf 'unset\nunset\n1\nunset\nunset\nVK_LAYER_FGVK_render:VK_LAYER_VALVE_steam_overlay_64:VK_LAYER_VALVE_steam_overlay_32:VK_LAYER_existing\n%s\nunset' \
    "$private_fgvk_layer_dir")"
if [[ "$ignored_vkbasalt_output" != "$expected_ignored_vkbasalt" ]]; then
    fail "ignored ENABLE_VKBASALT request changed the launch environment:\n$ignored_vkbasalt_output"
fi
if [[ "$(cat "$ignored_vkbasalt_stderr")" != "fgvk-launch: vkBasalt is no longer bundled; ENABLE_VKBASALT ignored." ]]; then
    fail "ignored ENABLE_VKBASALT notice was not printed exactly once:\n$(cat "$ignored_vkbasalt_stderr")"
fi
rm -f "$steam_overlay_dir/steamoverlay_x86_64.json"
rm -f "$steam_overlay_dir/steamoverlay_i386.json"

launch_config="$test_config_home/framegen-vulkan/launcher.conf"
mkdir -p "$(dirname -- "$launch_config")"
printf '%s\n' \
    'version=1' \
    'enable_zink=1' \
    'force_alsa_audio=1' > "$launch_config"
compatibility_output="$({
    WINEDLLOVERRIDES='d3d11=n' \
        FGVK_LAUNCH_CONFIG="$launch_config" \
        "$launcher" bash -c '
            printf "%s\n" \
                "${__GLX_VENDOR_LIBRARY_NAME:-unset}" \
                "${MESA_LOADER_DRIVER_OVERRIDE:-unset}" \
                "${GALLIUM_DRIVER:-unset}" \
                "${SDL_AUDIODRIVER:-unset}" \
                "${WINEDLLOVERRIDES:-unset}" \
                "${FGVK_LAUNCH_CONFIG:-unset}"
        '
} 2>&1)" || fail "standalone compatibility launch failed: $compatibility_output"
expected_compatibility=$'mesa\nzink\nzink\nalsa\nd3d11=n;winepulse.drv=d;winealsa.drv=b\nunset'
if [[ "$compatibility_output" != "$expected_compatibility" ]]; then
    fail "standalone compatibility settings were not applied safely:\n$compatibility_output"
fi

for disable_hdr in 0 1; do
    printf '%s\n' 'version=1' "disable_hdr_exposure=$disable_hdr" > "$launch_config"
    hdr_output="$(FGVK_LAUNCH_CONFIG="$launch_config" DXVK_HDR=1 "$launcher" bash -c \
        'printf "%s" "${DXVK_HDR:-unset}"')"
    expected_hdr="1"
    [[ "$hdr_output" == "$expected_hdr" ]] || fail "retired HDR key changed DXVK_HDR ownership: $hdr_output"
done

printf '%s\n' 'version=1' 'unknown_setting=1' > "$launch_config"
invalid_config_output="$({
    FGVK_LAUNCH_CONFIG="$launch_config" \
        "$launcher" bash -c \
        'printf "ZINK=%s ALSA=%s\n" "${GALLIUM_DRIVER:-unset}" "${SDL_AUDIODRIVER:-unset}"'
} 2>&1)" || fail "invalid launcher configuration did not fail closed"
if [[ "$invalid_config_output" != *"ignoring invalid launcher configuration"* ||
        "$invalid_config_output" != *"ZINK=unset ALSA=unset"* ]]; then
    fail "invalid launcher configuration was not rejected safely:\n$invalid_config_output"
fi
rm -f -- "$launch_config"

# Logging switches in launcher.conf export the renderer's diagnostic variables as
# 1. The probe prints each variable separately, with "unset" for an absent one, so
# a caller's explicit 0 and a missing export are both visible.
logging_probe='printf "%s|%s|%s|%s\n" "${FGVK_PACING_PROFILE-unset}" "${FGVK_PACING_PROFILE_TRACE-unset}" "${FGVK_PACING_PROFILE_WAYLAND-unset}" "${FGVK_PACING_PROFILE_GPU_SPLIT-unset}"'
probe_logging() {
    local config="$1"
    shift
    env -u FGVK_PACING_PROFILE -u FGVK_PACING_PROFILE_TRACE -u FGVK_PACING_PROFILE_WAYLAND \
        -u FGVK_PACING_PROFILE_GPU_SPLIT -u DISABLE_FGVK \
        -u GAMESCOPE_WAYLAND_DISPLAY -u WAYLAND_DISPLAY \
        FGVK_LAUNCH_CONFIG="$config" XDG_DATA_HOME="$test_data_home" "$@" \
        "$launcher" bash -c "$logging_probe" 2> "$test_root/logging.stderr"
}
expect_logging() {
    local label="$1" expected="$2" config="$3"
    shift 3
    local actual
    actual="$(probe_logging "$config" "$@")" || fail "logging probe failed: $label"
    if [[ "$actual" != "$expected" ]]; then
        fail "$label: expected $expected, got $actual"
    fi
}

logging_keys=(log_pacing log_trace log_wayland log_gpu_cost)
logging_on_expected=('1|unset|unset|unset' 'unset|1|unset|unset' 'unset|unset|1|unset' '1|unset|unset|1')
for index in "${!logging_keys[@]}"; do
    key="${logging_keys[$index]}"
    on_config="$test_root/$key-on.conf"
    off_config="$test_root/$key-off.conf"
    printf '%s\n' 'version=1' "$key=1" > "$on_config"
    printf '%s\n' 'version=1' "$key=0" > "$off_config"
    expect_logging "$key=1 exports its variable" "${logging_on_expected[$index]}" "$on_config"
    expect_logging "$key=0 exports nothing" 'unset|unset|unset|unset' "$off_config"
done

logging_none="$test_root/logging-none.conf"
printf '%s\n' 'version=1' > "$logging_none"
expect_logging "absent switches export nothing" 'unset|unset|unset|unset' "$logging_none"

logging_all="$test_root/logging-all.conf"
printf '%s\n' 'version=1' 'log_pacing=1' 'log_trace=1' 'log_wayland=1' 'log_gpu_cost=1' > "$logging_all"
expect_logging "every switch exports its variable" '1|1|1|1' "$logging_all"

# A variable typed by the caller always wins, including an explicit 0.
expect_logging "caller FGVK_PACING_PROFILE=0 keeps pacing off" \
    '0|unset|unset|unset' "$test_root/log_pacing-on.conf" FGVK_PACING_PROFILE=0
expect_logging "caller FGVK_PACING_PROFILE=0 wins over GPU cost" \
    '0|unset|unset|1' "$test_root/log_gpu_cost-on.conf" FGVK_PACING_PROFILE=0
expect_logging "caller GPU split=0 wins over GPU cost" \
    '1|unset|unset|0' "$test_root/log_gpu_cost-on.conf" FGVK_PACING_PROFILE_GPU_SPLIT=0
expect_logging "caller trace=0 wins over trace" \
    'unset|0|unset|unset' "$test_root/log_trace-on.conf" FGVK_PACING_PROFILE_TRACE=0
expect_logging "caller value is kept when the switch is off" \
    '1|unset|unset|unset' "$logging_none" FGVK_PACING_PROFILE=1

expect_logging "DISABLE_FGVK=1 exports no logging variables" \
    'unset|unset|unset|unset' "$logging_all" DISABLE_FGVK=1

# A duplicate or non-boolean switch makes the whole file invalid, so nothing is exported.
logging_duplicate="$test_root/logging-duplicate.conf"
printf '%s\n' 'version=1' 'log_pacing=1' 'log_trace=1' 'log_trace=0' > "$logging_duplicate"
expect_logging "duplicate logging switch exports nothing" \
    'unset|unset|unset|unset' "$logging_duplicate"
[[ "$(cat "$test_root/logging.stderr")" == *"ignoring invalid launcher configuration"* ]] ||
    fail "duplicate logging switch was not reported"
logging_boolean="$test_root/logging-boolean.conf"
printf '%s\n' 'version=1' 'log_present=true' > "$logging_boolean"
expect_logging "non-boolean retired logging key exports nothing" \
    'unset|unset|unset|unset' "$logging_boolean"
[[ "$(cat "$test_root/logging.stderr")" == *"ignoring invalid launcher configuration"* ]] ||
    fail "non-boolean retired logging key was not reported"

# The retired keys stay accepted and ignored beside the logging switches.
logging_retired="$test_root/logging-retired.conf"
printf '%s\n' 'version=1' 'disable_hdr_exposure=1' 'log_present=1' 'log_wayland=1' > "$logging_retired"
expect_logging "retired keys are accepted beside a logging switch" \
    'unset|unset|1|unset' "$logging_retired"
[[ ! -s "$test_root/logging.stderr" ]] || fail "retired keys produced a notice"

# A Flatpak run receives the same exported variables as a native launch.
logging_flatpak_dir="$test_root/logging-flatpak"
mkdir -p "$logging_flatpak_dir"
printf '%s\n' '#!/usr/bin/env bash' "$logging_probe" > "$logging_flatpak_dir/flatpak"
chmod +x "$logging_flatpak_dir/flatpak"
flatpak_logging_output="$(env -u FGVK_PACING_PROFILE -u FGVK_PACING_PROFILE_TRACE \
    -u FGVK_PACING_PROFILE_WAYLAND -u FGVK_PACING_PROFILE_GPU_SPLIT \
    -u DISABLE_FGVK -u GAMESCOPE_WAYLAND_DISPLAY -u WAYLAND_DISPLAY \
    FGVK_LAUNCH_CONFIG="$test_root/log_gpu_cost-on.conf" XDG_DATA_HOME="$test_data_home" \
    "$launcher" "$logging_flatpak_dir/flatpak" run example.App)" ||
    fail "Flatpak logging launch failed"
[[ "$flatpak_logging_output" == '1|unset|unset|1' ]] ||
    fail "Flatpak launch did not receive the logging variables: $flatpak_logging_output"

set +e
no_command_output="$({ "$launcher"; } 2>&1)"
no_command_status=$?
invalid_flag_output="$({ FGVK_ALLOW_COMPETING_LAYERS=yes "$launcher" true; } 2>&1)"
invalid_flag_status=$?
"$launcher" bash -c 'exit 23'
forwarded_status=$?
set -e

if [[ $no_command_status -ne 2 || "$no_command_output" != *"requires a command"* ]]; then
    fail "missing-command validation changed"
fi
if [[ $invalid_flag_status -ne 2 || "$invalid_flag_output" != *"must be 0 or 1"* ]]; then
    fail "invalid escape-hatch validation changed"
fi
if [[ $forwarded_status -ne 23 ]]; then
    fail "child exit status was not forwarded: $forwarded_status"
fi

help_output="$("$launcher" --help)" || fail "--help failed"
if [[ "$help_output" != *"Usage: fgvk [--] COMMAND"* ]]; then
    fail "--help output is incomplete"
fi

echo "fgvk-launch contract: PASS"

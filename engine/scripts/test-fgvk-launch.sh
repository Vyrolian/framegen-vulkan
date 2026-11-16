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
# case below is a desktop launch; the Gamescope cases set their own. The opt-out
# is cleared too, so only the cases that set it see it.
unset GAMESCOPE_WAYLAND_DISPLAY FGVK_GAMESCOPE
# Gamescope sets the WSI switches for its children, so a test run inside a
# gamescope session must not inherit them either.
unset ENABLE_GAMESCOPE_WSI DISABLE_GAMESCOPE_WSI
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

# FGVK_GAMESCOPE=0 is the Gamescope opt-out. The launcher must print one notice
# and exec the command with its environment untouched, so no framegen-vulkan
# setting, layer or preload change reaches the child.
gamescope_stderr="$test_root/gamescope.stderr"
gamescope_output="$(
    FGVK_LAUNCH_CONFIG="$overlay_enabled_config" \
    ENABLE_VK_LAYER_VALVE_steam_overlay_1=1 \
        GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 \
        WAYLAND_DISPLAY=gamescope-0 \
        FGVK_GAMESCOPE=0 \
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
if [[ "$(cat "$gamescope_stderr")" != "framegen-vulkan: Gamescope session: starting without frame generation (FGVK_GAMESCOPE=0)" ]]; then
    fail "Gamescope early-exit notice was not printed exactly once:\n$(cat "$gamescope_stderr")"
fi

# A Gamescope session runs frame generation by default. With FGVK_GAMESCOPE
# unset, the launcher takes the normal launch path with no launcher notice.
gamescope_default_stderr="$test_root/gamescope-default.stderr"
gamescope_default_output="$(
    FGVK_LAUNCH_CONFIG="$overlay_enabled_config" \
        GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 \
        WAYLAND_DISPLAY=gamescope-0 \
        XDG_DATA_HOME="$test_data_home" \
        "$launcher" bash -c '
            printf "%s\n" \
                "${ENABLE_FGVK:-unset}" \
                "${DISABLE_LSFG:-unset}" \
                "${FGVK_GAMESCOPE:-unset}" \
                "${GAMESCOPE_WAYLAND_DISPLAY:-unset}" \
                "${VK_IMPLICIT_LAYER_PATH:-unset}"
        ' 2> "$gamescope_default_stderr"
)" || fail "Gamescope default launch failed"
expected_gamescope_default="$(printf '1\n1\nunset\ngamescope-0\n%s' "$expected_layer_path")"
if [[ "$gamescope_default_output" != "$expected_gamescope_default" ]]; then
    fail "Gamescope session without FGVK_GAMESCOPE did not reach the normal launch path:\n$gamescope_default_output"
fi
if [[ -s "$gamescope_default_stderr" ]]; then
    fail "Gamescope default launch printed a launcher notice:\n$(cat "$gamescope_default_stderr")"
fi

# FGVK_GAMESCOPE=1 takes the same normal launch path as the default.
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

# Only the exact value 0 opts out. Every other value keeps the normal launch path
# with no launcher notice.
gamescope_value_stderr="$test_root/gamescope-value.stderr"
for gamescope_value in '' true yes 00; do
    gamescope_value_output="$(
        FGVK_GAMESCOPE="$gamescope_value" \
            GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 \
            WAYLAND_DISPLAY=gamescope-0 \
            XDG_DATA_HOME="$test_data_home" \
            "$launcher" bash -c 'printf "%s\n" "${ENABLE_FGVK:-unset}"' \
            2> "$gamescope_value_stderr"
    )" || fail "Gamescope launch failed for FGVK_GAMESCOPE='$gamescope_value'"
    if [[ "$gamescope_value_output" != "1" ]]; then
        fail "FGVK_GAMESCOPE='$gamescope_value' did not run frame generation in a Gamescope session"
    fi
    if [[ -s "$gamescope_value_stderr" ]]; then
        fail "FGVK_GAMESCOPE='$gamescope_value' printed a launcher notice:\n$(cat "$gamescope_value_stderr")"
    fi
done

# H2d: a gamescope session names gamescope's WSI layer in VK_INSTANCE_LAYERS, so the loader loads
# it as an explicit layer right after framegen-vulkan, or first when framegen-vulkan is implicit.
# The launcher links each VkLayer_FROG_gamescope_wsi*.json it finds, in the loader's search order
# (the first directory that holds a file name wins), into
# $XDG_DATA_HOME/framegen-vulkan/gamescope-wsi/explicit_layer.d, prepends that directory to
# VK_LAYER_PATH, and never names the layer in VK_IMPLICIT_LAYER_PATH. The probe points the XDG
# config and data directories at fixtures, so the host's /usr/share manifest cannot change the
# expected values.
gamescope_wsi_root="$test_root/gamescope-wsi"
gamescope_wsi_stderr="$test_root/gamescope-wsi.stderr"
gamescope_wsi_dir="$test_data_home/framegen-vulkan/gamescope-wsi/explicit_layer.d"
gamescope_wsi_retired_dir="$test_data_home/framegen-vulkan/gamescope-wsi/implicit_layer.d"
gamescope_wsi_probe() {
    # Later assignments win, so each case passes only the variables it changes.
    env -u GAMESCOPE_WAYLAND_DISPLAY -u WAYLAND_DISPLAY -u FGVK_GAMESCOPE \
        -u ENABLE_GAMESCOPE_WSI -u DISABLE_GAMESCOPE_WSI -u DISABLE_FGVK \
        -u ENABLE_VK_LAYER_VALVE_steam_overlay_1 -u MANGOHUD -u FLATPAK_ID \
        -u VK_IMPLICIT_LAYER_PATH -u VK_LAYER_PATH -u VK_INSTANCE_LAYERS \
        FGVK_LAUNCH_CONFIG="$overlay_enabled_config" \
        XDG_CONFIG_HOME="$gamescope_wsi_root/config-empty" \
        XDG_CONFIG_DIRS="$gamescope_wsi_root/xdg-config" \
        XDG_DATA_HOME="$test_data_home" \
        XDG_DATA_DIRS="$gamescope_wsi_root/system" \
        XDG_CACHE_HOME="$test_root/cache" \
        "$@" \
        "$launcher" bash -c 'printf "%s\n" "${VK_IMPLICIT_LAYER_PATH:-unset}" "${VK_LAYER_PATH:-unset}" "${VK_INSTANCE_LAYERS:-unset}"' \
        2> "$gamescope_wsi_stderr"
}

# The three values the probe prints: the implicit layer path, VK_LAYER_PATH and VK_INSTANCE_LAYERS.
gamescope_wsi_values() {
    printf '%s\n%s\n%s' "$1" "$2" "$3"
}

# Prints the entries of the directory $1, sorted: "name -> target" for a link, "name (not a link)"
# for anything else.
gamescope_wsi_links_in() {
    local directory="$1" entry
    {
        for entry in "$directory"/* "$directory"/.[!.]*; do
            if [[ -L "$entry" ]]; then
                printf '%s -> %s\n' "${entry##*/}" "$(readlink -- "$entry")"
            elif [[ -e "$entry" ]]; then
                printf '%s (not a link)\n' "${entry##*/}"
            fi
        done
    } | LC_ALL=C sort
}

gamescope_wsi_links() {
    gamescope_wsi_links_in "$gamescope_wsi_dir"
}

# Checks one probe result: OUTPUT must equal the expected values, and the only notice must be
# EXPECTED_NOTICE ("" when the launcher prints nothing).
gamescope_wsi_check() {
    local label="$1" output="$2" expected_values="$3" expected_notice="$4"
    if [[ "$output" != "$expected_values" ]]; then
        fail "$label: loader values changed:\n$output"
    fi
    if [[ "$(cat "$gamescope_wsi_stderr")" != "$expected_notice" ]]; then
        fail "$label: launcher notice changed:\n$(cat "$gamescope_wsi_stderr")"
    fi
}

# Launches the probe with the given variables, then checks the result.
gamescope_wsi_expect() {
    local label="$1" expected_values="$2" expected_notice="$3" output
    shift 3
    output="$(gamescope_wsi_probe "$@")" || fail "$label: launch failed: $(cat "$gamescope_wsi_stderr")"
    gamescope_wsi_check "$label" "$output" "$expected_values" "$expected_notice"
}

# Writes a gamescope WSI manifest named $2 at the path $1, shaped like the installed one.
gamescope_wsi_manifest() {
    mkdir -p "$(dirname -- "$1")"
    printf '%s\n' '{' '    "file_format_version" : "1.0.0",' '    "layer" : {' \
        "      \"name\": \"$2\"," '      "type": "GLOBAL",' \
        '      "library_path": "/usr/lib/libVkLayer_FROG_gamescope_wsi.so",' \
        '      "enable_environment": { "ENABLE_GAMESCOPE_WSI": "1" },' \
        '      "disable_environment": { "DISABLE_GAMESCOPE_WSI": "1" }' \
        '    }' '}' > "$1"
}

run_gamescope_wsi_cases() {
    local system_dir="$gamescope_wsi_root/system/vulkan/implicit_layer.d"
    local kept="$system_dir/VkLayer_FROG_gamescope_wsi.x86_64.json"
    local config_dir="$gamescope_wsi_root/config/vulkan/implicit_layer.d"
    local extra_dir="$gamescope_wsi_root/extra/vulkan/implicit_layer.d"
    local unnamed_dir="$gamescope_wsi_root/unnamed/vulkan/implicit_layer.d"
    local mangohud_root="$gamescope_wsi_root/mangohud"
    local config_x8664="$config_dir/VkLayer_FROG_gamescope_wsi.x86_64.json"
    local extra_x86="$extra_dir/VkLayer_FROG_gamescope_wsi.x86.json"
    local retired_link="$gamescope_wsi_retired_dir/VkLayer_FROG_gamescope_wsi.x86_64.json"
    local private_dir="$expected_layer_path"
    local name64=VK_LAYER_FROG_gamescope_wsi_x86_64
    local name32=VK_LAYER_FROG_gamescope_wsi_x86
    local kept_notice="framegen-vulkan: gamescope WSI layer kept: $gamescope_wsi_dir -> $kept"
    local not_found="framegen-vulkan: gamescope WSI layer not found; gamescope HDR and Xwayland bypass unavailable"
    local expected before output blocked_data readonly_data readonly_dir colon_data relative_cwd

    mkdir -p "$system_dir" "$gamescope_wsi_root/xdg-config" "$gamescope_wsi_root/config-empty" \
        "$gamescope_wsi_root/empty"
    gamescope_wsi_manifest "$kept" "$name64"
    # Other system manifests beside the gamescope layer are never admitted.
    touch "$system_dir/VkLayer_MESA_anti_lag.json"
    # The Steam overlay case names the overlay manifests that the fixtures above create.
    mkdir -p "$steam_overlay_dir"
    touch "$steam_overlay_dir/steamoverlay_x86_64.json" "$steam_overlay_dir/steamoverlay_i386.json"

    # H2c left a link in gamescope-wsi/implicit_layer.d. The first launch removes that link and
    # the empty directory, then links the found manifest in explicit_layer.d (mode 0700).
    mkdir -p "$gamescope_wsi_retired_dir"
    ln -sfn -- "$kept" "$retired_link"
    gamescope_wsi_expect "first launch" \
        "$(gamescope_wsi_values "$private_dir" "$gamescope_wsi_dir" "$name64")" "$kept_notice" \
        ENABLE_GAMESCOPE_WSI=1
    if [[ -e "$gamescope_wsi_retired_dir" ]]; then
        fail "the retired gamescope-wsi/implicit_layer.d directory was not removed"
    fi
    expected="VkLayer_FROG_gamescope_wsi.x86_64.json -> $kept"
    if [[ "$(gamescope_wsi_links)" != "$expected" ]]; then
        fail "gamescope WSI directory does not hold exactly the found manifest as a link:\n$(gamescope_wsi_links)"
    fi
    if [[ "$(stat -c %a "$gamescope_wsi_dir")" != 700 ]]; then
        fail "gamescope WSI directory is not mode 0700"
    fi
    # A repeated launch gives the same values, notice and directory contents.
    gamescope_wsi_expect "repeated launch" \
        "$(gamescope_wsi_values "$private_dir" "$gamescope_wsi_dir" "$name64")" "$kept_notice" \
        ENABLE_GAMESCOPE_WSI=1
    if [[ "$(gamescope_wsi_links)" != "$expected" ]]; then
        fail "repeated launch changed the gamescope WSI directory:\n$(gamescope_wsi_links)"
    fi

    # The retired directory is removed only when nothing else is left in it.
    mkdir -p "$gamescope_wsi_retired_dir"
    ln -sfn -- "$kept" "$retired_link"
    touch "$gamescope_wsi_retired_dir/keep.txt"
    gamescope_wsi_expect "retired directory with another file" \
        "$(gamescope_wsi_values "$private_dir" "$gamescope_wsi_dir" "$name64")" "$kept_notice" \
        ENABLE_GAMESCOPE_WSI=1
    if [[ -L "$retired_link" || ! -f "$gamescope_wsi_retired_dir/keep.txt" ]]; then
        fail "retired gamescope WSI link was not removed, or another file in its directory was touched"
    fi
    rm -f -- "$gamescope_wsi_retired_dir/keep.txt"
    rmdir -- "$gamescope_wsi_retired_dir"

    # The config directory is searched first, so its x86_64 manifest shadows the data-directory
    # copy. The 32-bit manifest exists only in the second data directory, and both names are named.
    mkdir -p "$config_dir" "$extra_dir"
    gamescope_wsi_manifest "$config_x8664" "$name64"
    gamescope_wsi_manifest "$extra_x86" "$name32"
    gamescope_wsi_manifest "$extra_dir/VkLayer_FROG_gamescope_wsi.x86_64.json" "$name64"
    gamescope_wsi_expect "two directories" \
        "$(gamescope_wsi_values "$private_dir" "$gamescope_wsi_dir" "$name64:$name32")" \
        "framegen-vulkan: gamescope WSI layer kept: $gamescope_wsi_dir -> $config_x8664 $extra_x86" \
        ENABLE_GAMESCOPE_WSI=1 XDG_CONFIG_HOME="$gamescope_wsi_root/config" \
        XDG_DATA_DIRS="$gamescope_wsi_root/system:$gamescope_wsi_root/extra"
    expected="$(printf 'VkLayer_FROG_gamescope_wsi.x86.json -> %s\nVkLayer_FROG_gamescope_wsi.x86_64.json -> %s' "$extra_x86" "$config_x8664")"
    if [[ "$(gamescope_wsi_links)" != "$expected" ]]; then
        fail "gamescope WSI two-directory links changed:\n$(gamescope_wsi_links)"
    fi

    # With the config copy alone, the 32-bit link is removed and only the config copy is named.
    gamescope_wsi_expect "stale link removed" \
        "$(gamescope_wsi_values "$private_dir" "$gamescope_wsi_dir" "$name64")" \
        "framegen-vulkan: gamescope WSI layer kept: $gamescope_wsi_dir -> $config_x8664" \
        ENABLE_GAMESCOPE_WSI=1 XDG_CONFIG_HOME="$gamescope_wsi_root/config"
    expected="VkLayer_FROG_gamescope_wsi.x86_64.json -> $config_x8664"
    if [[ "$(gamescope_wsi_links)" != "$expected" ]]; then
        fail "stale gamescope WSI link was not removed:\n$(gamescope_wsi_links)"
    fi

    # A manifest with no VK_LAYER_ name is not admitted, and a later copy of the same file name
    # is still found.
    mkdir -p "$unnamed_dir"
    printf '%s\n' '{}' > "$unnamed_dir/VkLayer_FROG_gamescope_wsi.x86_64.json"
    gamescope_wsi_expect "unnamed copy is skipped" \
        "$(gamescope_wsi_values "$private_dir" "$gamescope_wsi_dir" "$name64")" \
        "framegen-vulkan: gamescope WSI layer kept: $gamescope_wsi_dir -> $kept" \
        ENABLE_GAMESCOPE_WSI=1 XDG_CONFIG_HOME="$gamescope_wsi_root/unnamed"
    expected="VkLayer_FROG_gamescope_wsi.x86_64.json -> $kept"
    if [[ "$(gamescope_wsi_links)" != "$expected" ]]; then
        fail "unnamed manifest changed the gamescope WSI links:\n$(gamescope_wsi_links)"
    fi

    # Nothing usable is found: the not-found notice is printed, the caller's names stay as they
    # are, and every stale link is removed. A file that is not a gamescope WSI link is not ours.
    touch "$gamescope_wsi_dir/unrelated.txt"
    gamescope_wsi_expect "nothing found" \
        "$(gamescope_wsi_values "$private_dir" unset VK_LAYER_existing)" "$not_found" \
        ENABLE_GAMESCOPE_WSI=1 XDG_CONFIG_HOME="$gamescope_wsi_root/unnamed" \
        XDG_DATA_DIRS="$gamescope_wsi_root/empty" VK_INSTANCE_LAYERS=VK_LAYER_existing
    if [[ "$(gamescope_wsi_links)" != "unrelated.txt (not a link)" ]]; then
        fail "missing gamescope WSI manifest left a stale link or removed another file:\n$(gamescope_wsi_links)"
    fi
    rm -f -- "$gamescope_wsi_dir/unrelated.txt"

    # Relink the system manifest, then check that no launch without the layer changes the
    # directory or adds any value.
    gamescope_wsi_expect "relink" \
        "$(gamescope_wsi_values "$private_dir" "$gamescope_wsi_dir" "$name64")" "$kept_notice" \
        ENABLE_GAMESCOPE_WSI=1
    before="$(gamescope_wsi_links)"
    gamescope_wsi_expect "ENABLE_GAMESCOPE_WSI unset" \
        "$(gamescope_wsi_values "$private_dir" unset unset)" ""
    gamescope_wsi_expect "ENABLE_GAMESCOPE_WSI=0" \
        "$(gamescope_wsi_values "$private_dir" unset unset)" "" ENABLE_GAMESCOPE_WSI=0
    gamescope_wsi_expect "ENABLE_GAMESCOPE_WSI=true" \
        "$(gamescope_wsi_values "$private_dir" unset unset)" "" ENABLE_GAMESCOPE_WSI=true
    gamescope_wsi_expect "DISABLE_GAMESCOPE_WSI=1 wins" \
        "$(gamescope_wsi_values "$private_dir" unset unset)" "" \
        ENABLE_GAMESCOPE_WSI=1 DISABLE_GAMESCOPE_WSI=1
    if [[ "$(gamescope_wsi_links)" != "$before" ]]; then
        fail "a launch without the layer changed the gamescope WSI directory:\n$(gamescope_wsi_links)"
    fi

    # Case 1: Steam overlay off and framegen-vulkan implicit. The layer is named first.
    gamescope_wsi_expect "overlay off: framegen-vulkan implicit" \
        "$(gamescope_wsi_values "$private_dir" "$gamescope_wsi_dir" "$name64")" "$kept_notice" \
        ENABLE_GAMESCOPE_WSI=1

    # The caller's VK_LAYER_PATH stays after the gamescope directory, and a caller's copy of the
    # layer name is not duplicated.
    gamescope_wsi_expect "caller layer path and names" \
        "$(gamescope_wsi_values "$private_dir" "$gamescope_wsi_dir:/caller/explicit" "$name64:VK_LAYER_existing")" \
        "$kept_notice" ENABLE_GAMESCOPE_WSI=1 VK_LAYER_PATH=/caller/explicit \
        VK_INSTANCE_LAYERS="VK_LAYER_existing:$name64"

    # Case 2: Steam overlay on. framegen-vulkan is explicit, the layer follows it, and the overlay
    # layers come after the layer.
    gamescope_wsi_expect "overlay on: framegen-vulkan explicit" \
        "$(gamescope_wsi_values "$private_dir" "$gamescope_wsi_dir:$steam_overlay_dir" "VK_LAYER_FGVK_render:$name64:VK_LAYER_VALVE_steam_overlay_64:VK_LAYER_VALVE_steam_overlay_32")" \
        "$kept_notice" ENABLE_GAMESCOPE_WSI=1 ENABLE_VK_LAYER_VALVE_steam_overlay_1=1

    # Case 3: MangoHud requested. The 64-bit MangoHud layer stays above framegen-vulkan, and the
    # gamescope layer follows framegen-vulkan.
    if command -v python3 > /dev/null 2>&1; then
        mkdir -p "$mangohud_root/vulkan/implicit_layer.d" "$gamescope_wsi_root/mangohud-lib"
        touch "$gamescope_wsi_root/mangohud-lib/libMangoHud.so"
        printf '%s\n' '{' '    "file_format_version" : "1.0.0",' '    "layer" : {' \
            '      "name": "VK_LAYER_MANGOHUD_overlay_x86_64",' '      "type": "GLOBAL",' \
            "      \"library_path\": \"$gamescope_wsi_root/mangohud-lib/libMangoHud.so\"," \
            '      "implementation_version": "1",' \
            '      "enable_environment": { "MANGOHUD": "1" },' \
            '      "disable_environment": { "DISABLE_MANGOHUD": "1" }' \
            '    }' '}' > "$mangohud_root/vulkan/implicit_layer.d/MangoHud.x86_64.json"
        gamescope_wsi_expect "MangoHud requested" \
            "$(gamescope_wsi_values "$private_dir:$test_root/cache/framegen-vulkan/mangohud" "$gamescope_wsi_dir" "VK_LAYER_MANGOHUD_overlay_x86_64:VK_LAYER_FGVK_render:$name64")" \
            "$kept_notice" ENABLE_GAMESCOPE_WSI=1 MANGOHUD=1 \
            XDG_DATA_DIRS="$mangohud_root:$gamescope_wsi_root/system"
    else
        echo "fgvk-launch test: MangoHud gamescope WSI case skipped; python3 is not installed" >&2
    fi

    # FGVK_GAMESCOPE=0 execs the game before any launcher change, so nothing is added.
    gamescope_wsi_expect "FGVK_GAMESCOPE=0 exits before the layer" \
        "$(gamescope_wsi_values unset unset unset)" \
        "framegen-vulkan: Gamescope session: starting without frame generation (FGVK_GAMESCOPE=0)" \
        ENABLE_GAMESCOPE_WSI=1 GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 WAYLAND_DISPLAY=gamescope-0 \
        FGVK_GAMESCOPE=0

    # DISABLE_FGVK=1 turns off framegen-vulkan, not the gamescope layer. framegen-vulkan is not
    # named, so the layer is first.
    gamescope_wsi_expect "DISABLE_FGVK=1 keeps the layer" \
        "$(gamescope_wsi_values "$private_dir" "$gamescope_wsi_dir" "$name64")" "$kept_notice" \
        ENABLE_GAMESCOPE_WSI=1 DISABLE_FGVK=1

    # A directory that cannot be created (a file sits where it must go) is not kept. The launch
    # continues with every value unchanged.
    blocked_data="$gamescope_wsi_root/blocked-data"
    mkdir -p "$blocked_data"
    touch "$blocked_data/framegen-vulkan"
    gamescope_wsi_expect "directory cannot be created" \
        "$(gamescope_wsi_values "$private_dir" unset unset)" \
        "framegen-vulkan: gamescope WSI layer not kept: cannot create $blocked_data/framegen-vulkan/gamescope-wsi/explicit_layer.d" \
        ENABLE_GAMESCOPE_WSI=1 XDG_DATA_HOME="$blocked_data"

    # A directory that cannot be written is not kept. Root writes anywhere, so the case runs only
    # as another user.
    if [[ "$(id -u)" == 0 ]]; then
        echo "fgvk-launch test: read-only gamescope WSI directory case skipped as root" >&2
    else
        readonly_data="$gamescope_wsi_root/readonly-data"
        readonly_dir="$readonly_data/framegen-vulkan/gamescope-wsi/explicit_layer.d"
        mkdir -p "$readonly_dir"
        chmod 500 "$readonly_dir"
        output="$(gamescope_wsi_probe ENABLE_GAMESCOPE_WSI=1 XDG_DATA_HOME="$readonly_data")" || true
        chmod 700 "$readonly_dir"
        gamescope_wsi_check "directory is read-only" "$output" \
            "$(gamescope_wsi_values "$private_dir" unset unset)" \
            "framegen-vulkan: gamescope WSI layer not kept: $readonly_dir is not a writable directory"
    fi

    # A path with ':' cannot be named in VK_LAYER_PATH, so nothing is created.
    colon_data="$gamescope_wsi_root/colon:data"
    gamescope_wsi_expect "path with a colon" \
        "$(gamescope_wsi_values "$private_dir" unset unset)" \
        "framegen-vulkan: gamescope WSI layer not kept: directory must be an absolute path without ':' or a newline: $colon_data/framegen-vulkan/gamescope-wsi/explicit_layer.d" \
        ENABLE_GAMESCOPE_WSI=1 XDG_DATA_HOME="$colon_data"
    if [[ -e "$colon_data" ]]; then
        fail "a gamescope WSI directory was created under a path with ':'"
    fi

    # A relative data directory would depend on the working directory, so nothing is created.
    relative_cwd="$gamescope_wsi_root/relative-cwd"
    mkdir -p "$relative_cwd"
    output="$(cd "$relative_cwd" && gamescope_wsi_probe ENABLE_GAMESCOPE_WSI=1 XDG_DATA_HOME=relative-data)" || true
    gamescope_wsi_check "relative data directory" "$output" \
        "$(gamescope_wsi_values "$private_dir" unset unset)" \
        "framegen-vulkan: gamescope WSI layer not kept: directory must be an absolute path without ':' or a newline: relative-data/framegen-vulkan/gamescope-wsi/explicit_layer.d"
    if [[ -e "$relative_cwd/relative-data" ]]; then
        fail "a relative gamescope WSI directory was created"
    fi
}

# The loader's /etc/vulkan directory cannot be redirected. A manifest there would change
# every expected path, so the cases run only on a host without one.
if compgen -G "/etc/vulkan/implicit_layer.d/VkLayer_FROG_gamescope_wsi*.json" > /dev/null; then
    echo "fgvk-launch test: gamescope WSI cases skipped; /etc/vulkan/implicit_layer.d holds a gamescope WSI manifest" >&2
else
    run_gamescope_wsi_cases
fi

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

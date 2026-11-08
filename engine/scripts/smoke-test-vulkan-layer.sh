#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="$(tr -d '[:space:]' < "$repo_root/VERSION")"
archive="${1:-$repo_root/out/framegen-vulkan-v$version-linux.tar.xz}"

if [[ "$(uname -s)" != Linux ]]; then
    echo "The Vulkan-layer smoke test requires Linux." >&2
    exit 1
fi
for command in tar vulkaninfo; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required command not found: $command" >&2
        exit 1
    fi
done
if [[ ! -f "$archive" ]]; then
    echo "Renderer archive not found: $archive" >&2
    exit 1
fi

smoke_root="$(mktemp -d "${TMPDIR:-/tmp}/fgvk-vulkan-smoke.XXXXXX")"
cleanup() {
    rm -rf -- "$smoke_root"
}
trap cleanup EXIT

prefix="$smoke_root/prefix"
home_dir="$smoke_root/home"
log_file="$smoke_root/vulkaninfo.log"
inactive_log_file="$smoke_root/vulkaninfo-inactive.log"
swapchain_log="$smoke_root/vkcube.log"
mkdir -p "$prefix" "$home_dir"
tar -xJf "$archive" -C "$prefix"

manifest="$prefix/share/vulkan/implicit_layer.d/VkLayer_FGVK_render.json"
library="$prefix/lib/libfgvk-render.so"
launcher="$prefix/libexec/framegen-vulkan/fgvk-launch"
for required_file in "$manifest" "$library" "$launcher"; do
    if [[ ! -f "$required_file" ]]; then
        echo "Packaged Renderer smoke test is missing: $required_file" >&2
        exit 1
    fi
done

# The manifest is installed in a global discovery directory, but framegen-vulkan must be
# absent from the process unless the caller explicitly opts in. Exercise the
# loader's real enable_environment gate before testing the active path; a
# regression here could affect Gamescope, Steam, and every unrelated Vulkan
# application merely because the package is installed.
if ! env -u ENABLE_FGVK -u DISABLE_FGVK \
        HOME="$home_dir" \
        XDG_CONFIG_HOME="$home_dir/.config" \
        XDG_DATA_HOME="$prefix/share" \
        VK_LOADER_DEBUG=layer \
        vulkaninfo --summary >"$inactive_log_file" 2>&1; then
    cat "$inactive_log_file" >&2
    echo "vulkaninfo failed while testing framegen-vulkan's inactive manifest gate." >&2
    exit 1
fi
if ! grep -Fq 'VkLayer_FGVK_render.json' "$inactive_log_file"; then
    cat "$inactive_log_file" >&2
    echo "The Vulkan loader did not discover framegen-vulkan's inactive manifest." >&2
    exit 1
fi
if grep -Eq 'Insert(ed)? (instance|device) layer "VK_LAYER_FGVK_render"|framegen-vulkan: render layer active' \
        "$inactive_log_file"; then
    cat "$inactive_log_file" >&2
    echo "framegen-vulkan activated without ENABLE_FGVK=1." >&2
    exit 1
fi

if ! env -u DISABLE_FGVK \
        HOME="$home_dir" \
        XDG_CONFIG_HOME="$home_dir/.config" \
        XDG_DATA_HOME="$prefix/share" \
        VK_LOADER_DEBUG=layer \
        "$launcher" vulkaninfo --summary >"$log_file" 2>&1; then
    cat "$log_file" >&2
    echo "vulkaninfo failed while the packaged framegen-vulkan layer was enabled." >&2
    exit 1
fi

activation_marker="framegen-vulkan: render layer active; identity=VK_LAYER_FGVK_render; build=$version"
if ! grep -Fq "$activation_marker" "$log_file"; then
    cat "$log_file" >&2
    echo "The Vulkan loader completed without framegen-vulkan's activation marker." >&2
    exit 1
fi

if ! grep -Fq 'Insert instance layer "VK_LAYER_FGVK_render"' "$log_file"; then
    cat "$log_file" >&2
    echo "The Vulkan loader discovered framegen-vulkan but did not insert it into the instance chain." >&2
    exit 1
fi

if grep -Eq "Failed to find 'vkGet(Instance|Device)ProcAddr'.*fgvk-render|Skipping layer.*fgvk-render|requested layer VK_LAYER_FGVK_render was loaded but was not found" "$log_file"; then
    cat "$log_file" >&2
    echo "The Vulkan loader reported a framegen-vulkan entrypoint or activation failure." >&2
    exit 1
fi

# When the host exposes a graphical compositor, cover the dormant no-profile
# path through real swapchain creation and presentation as well as instance and
# device creation. Headless runs still retain the vulkaninfo activation check.
if command -v vkcube >/dev/null 2>&1 &&
        [[ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]]; then
    if ! env -u DISABLE_FGVK \
            HOME="$home_dir" \
            XDG_CONFIG_HOME="$home_dir/.config" \
            XDG_DATA_HOME="$prefix/share" \
            VK_LOADER_DEBUG=layer \
            "$launcher" vkcube --c 2 --suppress_popups >"$swapchain_log" 2>&1; then
        cat "$swapchain_log" >&2
        echo "vkcube failed while exercising the packaged framegen-vulkan swapchain path." >&2
        exit 1
    fi
    if ! grep -Fq 'Insert instance layer "VK_LAYER_FGVK_render"' "$swapchain_log"; then
        cat "$swapchain_log" >&2
        echo "The swapchain smoke test did not insert framegen-vulkan." >&2
        exit 1
    fi
fi

echo "Packaged framegen-vulkan Vulkan layer activated successfully on real hardware."

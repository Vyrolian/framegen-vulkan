#!/usr/bin/env bash
# Build self-contained Flatpak bundles for framegen-vulkan.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
monorepo_root="$(cd "$repo_root/.." && pwd)"
build_cache_root="${FGVK_BUILD_CACHE_ROOT:-$repo_root/build/cache}"
build_work_root="${FGVK_BUILD_WORK_ROOT:-$repo_root/build/work}"
if [[ "$build_cache_root" != /* ]]; then
    build_cache_root="$repo_root/$build_cache_root"
fi
if [[ "$build_work_root" != /* ]]; then
    build_work_root="$repo_root/$build_work_root"
fi
version="$(tr -d '[:space:]' < "$repo_root/VERSION")"
default_output="$repo_root/out/framegen-vulkan-v$version-flatpak.tar.xz"
output_path="${1:-$default_output}"

if [[ "$output_path" != /* ]]; then
    output_path="$PWD/$output_path"
fi

if [[ -z "$version" ]]; then
    echo "VERSION must contain a release version." >&2
    exit 1
fi

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "Flatpak packaging runs only on Linux; this host is $(uname -s)." >&2
    exit 2
fi
if ! command -v flatpak >/dev/null 2>&1 || ! command -v flatpak-builder >/dev/null 2>&1; then
    echo "Flatpak packaging needs flatpak and flatpak-builder on the host. Install both (for example: sudo pacman -S flatpak flatpak-builder, or sudo apt install flatpak flatpak-builder) and try again." >&2
    exit 1
fi

for command in flatpak flatpak-builder nm python3 strings tar; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required command not found: $command" >&2
        exit 1
    fi
done

python3 "$monorepo_root/scripts/generate-flatpak-runtimes.py" --check
python3 "$repo_root/scripts/generate-flatpak-vulkan-headers.py" --check

verify_elf_class() {
    local path="$1"
    local expected="$2"
    local actual
    actual="$(od -An -t u1 -j 4 -N 1 "$path" | tr -d '[:space:]')"
    if [[ "$actual" != "$expected" ]]; then
        echo "Flatpak packaging failed: $path has unexpected ELF class byte $actual" >&2
        exit 1
    fi
}

work_root="${FGVK_FLATPAK_WORK_ROOT:-${FGVK_FLATPAK_TMP_ROOT:-$build_work_root/flatpak}}"
if [[ "$work_root" != /* ]]; then
    work_root="$repo_root/$work_root"
fi
mkdir -p "$work_root"
build_root="$(mktemp -d "$work_root/fgvk-flatpak-package.XXXXXX")"
cleanup() {
    rm -rf "$build_root"
}
trap cleanup EXIT

# Keep downloaded SDKs outside the disposable staging tree.  This directory is
# ignored by Git and makes repeated native Linux release builds incremental.
cache_root="${FGVK_FLATPAK_CACHE_ROOT:-$build_cache_root/flatpak}"
if [[ "$cache_root" != /* ]]; then
    cache_root="$repo_root/$cache_root"
fi
export HOME="$cache_root/home"
export XDG_CACHE_HOME="$cache_root/cache"
export XDG_CONFIG_HOME="$cache_root/config"
export XDG_DATA_HOME="$cache_root/data"

mkdir -p "$HOME" "$XDG_CACHE_HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$(dirname "$output_path")"
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo

bundle_dir="$build_root/bundles"
repo_dir="$build_root/repo"
mkdir -p "$bundle_dir" "$repo_dir"

manifest_root="$repo_root/dist/flatpak/fgvk-render"
extension_id="org.freedesktop.Platform.VulkanLayer.fgvkrender"
runtime_versions_file="$repo_root/dist/flatpak/fgvk-render/runtime-versions.txt"
runtime_count=0
while IFS= read -r runtime_version || [[ -n "$runtime_version" ]]; do
    if [[ -z "$runtime_version" || "$runtime_version" == \#* ]]; then
        continue
    fi
    # FGVK_FLATPAK_RUNTIMES (space-separated, e.g. "26.08") builds only those runtimes.
    if [[ -n "${FGVK_FLATPAK_RUNTIMES:-}" && " $FGVK_FLATPAK_RUNTIMES " != *" $runtime_version "* ]]; then
        continue
    fi
    runtime_count=$((runtime_count + 1))
    manifest="$manifest_root/$extension_id"_"$runtime_version.yml"
    build_dir="$build_root/build-$runtime_version"
    bundle="$bundle_dir/$extension_id-$runtime_version.flatpak"

    if [[ ! -f "$manifest" ]]; then
        echo "Missing Flatpak manifest: $manifest" >&2
        exit 1
    fi

    echo "Building framegen-vulkan Flatpak runtime extension $runtime_version..."
    flatpak-builder --force-clean --user --install-deps-from=flathub \
        --state-dir="$build_root/state-$runtime_version" \
        --repo="$repo_dir" "$build_dir" "$manifest"

    # Verify both layer architectures before bundling. Flatpak applications may
    # launch either a 64-bit or genuine 32-bit Vulkan process, and each process
    # must find a manifest and library with matching bitness.
    for required_path in \
        "files/share/vulkan/implicit_layer.d/VkLayer_FGVK_render.json" \
        "files/share/vulkan/implicit_layer.d/VkLayer_FGVK_render.x86.json" \
        "files/share/framegen-vulkan/vulkan/implicit_layer.d/VkLayer_FGVK_render.json" \
        "files/share/framegen-vulkan/vulkan/implicit_layer.d/VkLayer_FGVK_render.x86.json" \
        "files/share/doc/framegen-vulkan/ASSET_PROVENANCE.md" \
        "files/share/doc/framegen-vulkan/COPYRIGHT.md" \
        "files/share/doc/framegen-vulkan/LICENSE" \
        "files/share/doc/framegen-vulkan/THIRD_PARTY_NOTICES.md" \
        "files/share/doc/framegen-vulkan/themes/example.toml" \
        "files/lib64/libfgvk-render.so" \
        "files/lib/i386-linux-gnu/libfgvk-render.so"; do
        if [[ ! -f "$build_dir/$required_path" ]]; then
            echo "Flatpak packaging failed: missing $required_path for $runtime_version" >&2
            exit 1
        fi
    done

    verify_elf_class "$build_dir/files/lib64/libfgvk-render.so" 2
    verify_elf_class "$build_dir/files/lib/i386-linux-gnu/libfgvk-render.so" 1

    for layer_binary in \
            "$build_dir/files/lib64/libfgvk-render.so" \
            "$build_dir/files/lib/i386-linux-gnu/libfgvk-render.so"; do
        expected_identity="VK_LAYER_FGVK_render"
        if ! strings "$layer_binary" |
                grep -F "framegen-vulkan: render layer active; identity=$expected_identity; build=$version" >/dev/null; then
            echo "Flatpak packaging failed: layer build identity is missing for $runtime_version" >&2
            exit 1
        fi
        if ! strings "$layer_binary" | grep -F "FGVK_PROFILE_FALLBACK" >/dev/null; then
            echo "Flatpak packaging failed: profile-fallback wrapper protocol is missing for $runtime_version" >&2
            exit 1
        fi
    done

    manifest64="$build_dir/files/share/vulkan/implicit_layer.d/VkLayer_FGVK_render.json"
    manifest32="$build_dir/files/share/vulkan/implicit_layer.d/VkLayer_FGVK_render.x86.json"

    if ! grep -Fq "/usr/lib/extensions/vulkan/fgvkrender/lib64/libfgvk-render.so" \
        "$manifest64"; then
        echo "Flatpak packaging failed: 64-bit manifest path is incorrect for $runtime_version" >&2
        exit 1
    fi
    if ! grep -Fq '"library_arch": "64"' \
        "$manifest64"; then
        echo "Flatpak packaging failed: 64-bit manifest architecture is incorrect for $runtime_version" >&2
        exit 1
    fi
    if ! grep -Fq "/usr/lib/extensions/vulkan/fgvkrender/lib/i386-linux-gnu/libfgvk-render.so" \
        "$manifest32"; then
        echo "Flatpak packaging failed: 32-bit manifest path is incorrect for $runtime_version" >&2
        exit 1
    fi
    if ! grep -Fq '"library_arch": "32"' \
        "$manifest32"; then
        echo "Flatpak packaging failed: 32-bit manifest architecture is incorrect for $runtime_version" >&2
        exit 1
    fi
    for manifest in "$manifest64" "$manifest32"; do
        if ! grep -Fq '"name": "VK_LAYER_FGVK_render"' "$manifest" ||
                ! grep -Fq '"ENABLE_FGVK": "1"' "$manifest" ||
                ! grep -Fq '"DISABLE_FGVK": "1"' "$manifest"; then
            echo "Flatpak packaging failed: framegen-vulkan layer gating is incorrect for $runtime_version" >&2
            exit 1
        fi
    done
    flatpak build-bundle "$repo_dir" "$bundle" "$extension_id" "$runtime_version" --runtime

    if [[ ! -s "$bundle" ]]; then
        echo "Flatpak packaging failed: missing $bundle" >&2
        exit 1
    fi

    # A runtime extension is normalized from `lib` to `lib64` when Flatpak
    # deploys it. Verify the *installed bundle*, not only the build staging
    # directory, so the manifest cannot point at a path that is absent at run
    # time.
    flatpak install --user --noninteractive "$bundle" >/dev/null
    deployed_dir="$(flatpak info --user --show-location "$extension_id//$runtime_version")"
    deployed_manifest64="$deployed_dir/files/share/vulkan/implicit_layer.d/VkLayer_FGVK_render.json"
    deployed_manifest32="$deployed_dir/files/share/vulkan/implicit_layer.d/VkLayer_FGVK_render.x86.json"

    if [[ ! -f "$deployed_dir/files/lib64/libfgvk-render.so" ]]; then
        echo "Flatpak packaging failed: deployed 64-bit library is missing for $runtime_version" >&2
        exit 1
    fi
    if [[ ! -f "$deployed_dir/files/lib/i386-linux-gnu/libfgvk-render.so" ]]; then
        echo "Flatpak packaging failed: deployed 32-bit library is missing for $runtime_version" >&2
        exit 1
    fi
    for legal_file in ASSET_PROVENANCE.md COPYRIGHT.md LICENSE THIRD_PARTY_NOTICES.md; do
        if [[ ! -f "$deployed_dir/files/share/doc/framegen-vulkan/$legal_file" ]]; then
            echo "Flatpak packaging failed: deployed legal file $legal_file is missing for $runtime_version" >&2
            exit 1
        fi
    done

    verify_elf_class "$deployed_dir/files/lib64/libfgvk-render.so" 2
    verify_elf_class "$deployed_dir/files/lib/i386-linux-gnu/libfgvk-render.so" 1

    for layer_binary in \
            "$deployed_dir/files/lib64/libfgvk-render.so" \
            "$deployed_dir/files/lib/i386-linux-gnu/libfgvk-render.so"; do
        expected_identity="VK_LAYER_FGVK_render"
        if ! strings "$layer_binary" |
                grep -F "framegen-vulkan: render layer active; identity=$expected_identity; build=$version" >/dev/null; then
            echo "Flatpak packaging failed: deployed layer build identity is missing for $runtime_version" >&2
            exit 1
        fi
        if ! strings "$layer_binary" | grep -F "FGVK_PROFILE_FALLBACK" >/dev/null; then
            echo "Flatpak packaging failed: deployed profile-fallback wrapper protocol is missing for $runtime_version" >&2
            exit 1
        fi
    done

    if ! grep -Fq "/usr/lib/extensions/vulkan/fgvkrender/lib64/libfgvk-render.so" \
        "$deployed_manifest64"; then
        echo "Flatpak packaging failed: deployed 64-bit manifest path is incorrect for $runtime_version" >&2
        exit 1
    fi
    if ! grep -Fq '"library_arch": "64"' "$deployed_manifest64"; then
        echo "Flatpak packaging failed: deployed 64-bit manifest architecture is incorrect for $runtime_version" >&2
        exit 1
    fi
    if ! grep -Fq "/usr/lib/extensions/vulkan/fgvkrender/lib/i386-linux-gnu/libfgvk-render.so" \
        "$deployed_manifest32"; then
        echo "Flatpak packaging failed: deployed 32-bit manifest path is incorrect for $runtime_version" >&2
        exit 1
    fi
    if ! grep -Fq '"library_arch": "32"' "$deployed_manifest32"; then
        echo "Flatpak packaging failed: deployed 32-bit manifest architecture is incorrect for $runtime_version" >&2
        exit 1
    fi
done < "$runtime_versions_file"

if ((runtime_count == 0)); then
    echo "Flatpak runtime matrix is empty: $runtime_versions_file" >&2
    exit 1
fi

# Keep the Flatpak bundles self-contained for users who prefer a graphical
# installation. The script selects one matching runtime and performs the
# normal user-scoped Flatpak installation; per-application access remains a
# separate explicit configuration step.
cp "$repo_root/scripts/fgvk-install-flatpaks" "$bundle_dir/Install framegen-vulkan Flatpak Extensions"
chmod 0755 "$bundle_dir/Install framegen-vulkan Flatpak Extensions"
if ! bash -n "$bundle_dir/Install framegen-vulkan Flatpak Extensions"; then
    echo "Flatpak packaging failed: graphical installer has invalid shell syntax" >&2
    exit 1
fi

# The source directory is mounted into Flatpak-builder as a local source. Some
# builder versions remove ignored output directories while cleaning the source
# staging area, so recreate the destination immediately before writing it.
mkdir -p "$(dirname "$output_path")"
tar -C "$bundle_dir" -cJf "$output_path" .
echo "Created and verified: $output_path"
echo "Version: $version"

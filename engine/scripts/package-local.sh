#!/usr/bin/env bash
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
local_vkbasalt_archive="${FGVK_VKBASALT_LOCAL_ARCHIVE:-}"
if [[ -n "$local_vkbasalt_archive" ]]; then
    if [[ "$local_vkbasalt_archive" == /* || "$local_vkbasalt_archive" == *..* || ! -f "$monorepo_root/$local_vkbasalt_archive" ]]; then
        echo "FGVK_VKBASALT_LOCAL_ARCHIVE must be an existing repository-relative archive." >&2
        exit 1
    fi
fi
version="$(tr -d '[:space:]' < "$repo_root/VERSION")"
default_output="$repo_root/out/framegen-vulkan-v$version-linux.tar.xz"
output_path=""
build_32_bit=true

usage() {
    cat <<'EOF'
Usage: scripts/package-local.sh [--64-bit-only] [output-path]

Build and verify a local Linux engine archive. --64-bit-only omits the 32-bit
layer and manifest for faster native 64-bit Deck/Steam Machine test builds.
EOF
}

while (($#)); do
    case "$1" in
        --64-bit-only)
            build_32_bit=false
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --*)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            if [[ -n "$output_path" ]]; then
                echo "Only one output path may be specified" >&2
                exit 2
            fi
            output_path="$1"
            ;;
    esac
    shift
done
output_path="${output_path:-$default_output}"

if [[ "$output_path" != /* ]]; then
    output_path="$PWD/$output_path"
fi

if [[ -z "$version" ]]; then
    echo "VERSION must contain a release version." >&2
    exit 1
fi

vulkan_headers_revision="$(< "$repo_root/vulkan-headers-revision.txt")"
if [[ ! "$vulkan_headers_revision" =~ ^(vulkan-sdk-|v)[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "vulkan-headers-revision.txt must contain a Vulkan-Headers SDK branch (vulkan-sdk-X.Y.Z) or release tag (vX.Y.Z)." >&2
    exit 1
fi

portable_container="${FGVK_PORTABLE_PACKAGE:-0}"
containerized_build="${FGVK_PACKAGE_CONTAINERIZED:-0}"
if [[ "$containerized_build" != "1" && ( "$(uname -s)" != "Linux" || "$portable_container" == "1" ) ]]; then
    container_runtime=""
    if command -v docker >/dev/null 2>&1; then
        container_runtime="docker"
    elif command -v podman >/dev/null 2>&1; then
        container_runtime="podman"
    else
        echo "Portable packaging needs Docker or Podman. Install one and try again." >&2
        exit 1
    fi

    case "$output_path" in
        "$repo_root"/*)
            output_relative="${output_path#"$repo_root"/}"
            ;;
        *)
            echo "For a containerized build, the output path must be inside this repository." >&2
            exit 1
            ;;
    esac

    echo "Using local linux/amd64 $container_runtime packaging environment..."
    docker_64_only=0
    if [[ "$build_32_bit" == false ]]; then
        docker_64_only=1
    fi
    exec "$container_runtime" run --rm --platform linux/amd64 \
        -e FGVK_PACKAGE_64_ONLY="$docker_64_only" \
        -e FGVK_PACKAGE_CONTAINERIZED=1 \
        -e FGVK_VULKAN_HEADERS_REVISION="$vulkan_headers_revision" \
        -e FGVK_RELEASE_SKIP_TESTS="${FGVK_RELEASE_SKIP_TESTS:-0}" \
        -e FGVK_VKBASALT_LOCAL_ARCHIVE="$local_vkbasalt_archive" \
        -v "$monorepo_root:/workspace" \
        -w /workspace/engine \
        ubuntu:22.04 \
        bash -lc '
            set -euo pipefail
            export DEBIAN_FRONTEND=noninteractive
            sed -i "s|http://|https://|g" /etc/apt/sources.list
            # Minimal Ubuntu images do not contain a CA bundle. APT still
            # verifies signed Ubuntu repository metadata during this one-time
            # TLS bootstrap; subsequent downloads use normal certificate checks.
            if [[ ! -s /etc/ssl/certs/ca-certificates.crt ]]; then
                apt-get -o Acquire::https::Verify-Peer=false update -qq
                apt-get -o Acquire::https::Verify-Peer=false install -y -qq ca-certificates
            fi
            apt-get update -qq
            apt-get install -y -qq \
                git curl nodejs python3 llvm clang cmake ninja-build pkg-config g++-multilib \
                libvulkan-dev mesa-common-dev
            git clone --depth=1 -b "$FGVK_VULKAN_HEADERS_REVISION" \
                https://github.com/KhronosGroup/Vulkan-Headers /tmp/vkh
            rm -rf /usr/include/vulkan /usr/include/vk_video
            cp -a /tmp/vkh/include/vulkan /tmp/vkh/include/vk_video /usr/include/
            package_args=()
            if [[ "${FGVK_PACKAGE_64_ONLY:-0}" == "1" ]]; then
                package_args+=(--64-bit-only)
            fi
            scripts/package-local.sh "${package_args[@]}" "/workspace/engine/'"$output_relative"'"
        '
fi

for command in cmake ninja clang++ nm python3 readelf strings tar sha256sum; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required command not found: $command" >&2
        exit 1
    fi
done


mkdir -p "$build_work_root"
build_root="$(mktemp -d "$build_work_root/fgvk-package.XXXXXX")"
cleanup() {
    rm -rf "$build_root"
}
trap cleanup EXIT

build64_dir="$build_root/build64"
build32_dir="$build_root/build32"
install_dir="$build_root/target"
mkdir -p "$(dirname "$output_path")"

build_testing=ON
if [[ "${FGVK_RELEASE_SKIP_TESTS:-0}" == "1" ]]; then
    build_testing=OFF
    echo "Automated tests skipped by maintainer request; package verification remains enabled."
fi
cmake -S "$repo_root" -B "$build64_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$install_dir" \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DBUILD_TESTING="$build_testing" \
    -DFGVK_REQUIRE_NATIVE_PACKAGE_HEADERS=ON \
    -DFGVK_BUILD_VK_LAYER=ON \
    -DFGVK_BUILD_CLI=ON \
    -DFGVK_LAYER_LIBRARY_PATH="../../../lib/libfgvk-render.so" \
    -DFGVK_SCALING_LAYER_LIBRARY_PATH="../../../lib/libfgvk-render-scaling.so"

# Build the complete configured tree before CTest so CMake remains the single
# authority for every registered test executable and production dependency.
cmake --build "$build64_dir"
if [[ "${FGVK_RELEASE_SKIP_TESTS:-0}" != "1" ]]; then
    ctest --test-dir "$build64_dir" --output-on-failure
fi
# Release archives do not need local symbol tables. CMake's install-time strip
# preserves the dynamic entrypoints required by the Vulkan loader while
# keeping both the installed payload and compressed archive smaller.
cmake --install "$build64_dir" --strip

if [[ "$build_32_bit" == true ]]; then
    # A Vulkan layer is loaded into the application's process. Release archives
    # retain the second copy for genuine 32-bit games; local 64-bit-only builds
    # skip it to shorten the edit/test cycle.
    cmake -S "$repo_root" -B "$build32_dir" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$install_dir" \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_CXX_FLAGS=-m32 \
        -DCMAKE_SHARED_LINKER_FLAGS=-m32 \
        -DCMAKE_INSTALL_LIBDIR=lib32 \
        -DBUILD_TESTING=OFF \
        -DFGVK_REQUIRE_NATIVE_PACKAGE_HEADERS=ON \
        -DFGVK_BUILD_VK_LAYER=ON \
        -DFGVK_BUILD_CLI=OFF \
        -DFGVK_LAYER_MANIFEST_SUFFIX=.x86 \
        -DFGVK_LAYER_LIBRARY_PATH="../../../lib32/libfgvk-render.so" \
        -DFGVK_SCALING_LAYER_LIBRARY_PATH="../../../lib32/libfgvk-render-scaling.so"

    cmake --build "$build32_dir" --target fgvk-render fgvk-render-scaling
    cmake --install "$build32_dir" --strip
fi

vkbasalt_stage_args=(
    --stage-native "$install_dir"
    --cache-dir "$build_cache_root/vkbasalt"
)
if [[ "$build_32_bit" == false ]]; then
    vkbasalt_stage_args+=(--64-bit-only)
fi
if [[ -n "$local_vkbasalt_archive" ]]; then
    vkbasalt_stage_args+=(--local-archive "$monorepo_root/$local_vkbasalt_archive")
fi
python3 "$repo_root/scripts/manage-vkbasalt-release.py" \
    "${vkbasalt_stage_args[@]}"

required_paths=(
    "bin/fgvk-cli" \
    "bin/fgvk-diagnostics" \
    "bin/fgvk-installer" \
    "bin/fgvk-launch" \
    "bin/fgvk-remote-play" \
    "share/framegen-vulkan/fgvk_remote_play/libfgvk-remote-play-sdr.so" \
    "share/framegen-vulkan/fgvk_remote_play/__init__.py" \
    "share/framegen-vulkan/fgvk_remote_play/remote_play_core.py" \
    "share/framegen-vulkan/fgvk_remote_play/remote_play_launch.py" \
    "share/framegen-vulkan/fgvk_remote_play/managed_files.py" \
    "bin/fgvk-vrr-lease" \
    "lib/libfgvk-render.so" \
    "lib/libfgvk-render-scaling.so" \
    "lib/vkbasalt/libvkbasalt.so" \
    "share/doc/framegen-vulkan/ASSET_PROVENANCE.md" \
    "share/doc/framegen-vulkan/LICENSE.md" \
    "share/doc/framegen-vulkan/THIRD_PARTY_NOTICES.md" \
    "share/framegen-vulkan/vulkan/implicit_layer.d/VkLayer_FGVK_render.json" \
    "share/framegen-vulkan/vulkan/spatial_scaling.d/VkLayer_FGVK_spatial_scaling.json" \
    "share/framegen-vulkan/vulkan/vkbasalt.d/vkBasalt.json" \
    "share/framegen-vulkan/vkbasalt-shaders/Clarity.fx" \
    "share/framegen-vulkan/vkbasalt-shaders/CropStretch.fx" \
    "share/framegen-vulkan/vkbasalt-shaders/LevelsPlus.fx" \
    "share/framegen-vulkan/vkbasalt-shaders/SOURCE.md" \
    "share/framegen-vulkan/vkbasalt-shaders/Vibrance.fx" \
    "share/framegen-vulkan/vkbasalt-shaders/LICENSE-SweetFX" \
    "share/doc/framegen-vulkan/vkbasalt/LICENSE" \
    "share/doc/framegen-vulkan/vkbasalt/RESHade-LICENSE.md" \
    "share/doc/framegen-vulkan/vkbasalt/SOURCE" \
    "share/doc/framegen-vulkan/vkbasalt/framegen-vulkan-PIN.json" \
    "share/vulkan/implicit_layer.d/VkLayer_FGVK_spatial_scaling.json" \
    "share/vulkan/implicit_layer.d/VkLayer_FGVK_render.json"
)
if [[ "$build_32_bit" == true ]]; then
    required_paths+=(
        "lib32/libfgvk-render.so"
        "lib32/libfgvk-render-scaling.so"
        "lib32/vkbasalt/libvkbasalt.so"
        "share/framegen-vulkan/vulkan/implicit_layer.d/VkLayer_FGVK_render.x86.json"
        "share/vulkan/implicit_layer.d/VkLayer_FGVK_render.x86.json"
        "share/framegen-vulkan/vulkan/spatial_scaling.d/VkLayer_FGVK_spatial_scaling.x86.json"
        "share/vulkan/implicit_layer.d/VkLayer_FGVK_spatial_scaling.x86.json"
        "share/framegen-vulkan/vulkan/vkbasalt.d/vkBasalt.x86.json"
    )
fi
for required_path in "${required_paths[@]}"; do
    if [[ ! -e "$install_dir/$required_path" ]]; then
        echo "Packaging failed: missing $required_path" >&2
        exit 1
    fi
done

# The archive-root launcher is intentionally separate from the installed
# `fgvk-installer` command. Users extract the archive, double-click this file,
# and the installer copies only the verified payload into their user-local
# prefix. The generated manifest gives upgrades and uninstalls an exact,
# checksummed ownership record without treating the entire ~/.local tree as
# disposable.
cp "$repo_root/scripts/fgvk-installer" "$install_dir/Install framegen-vulkan"
chmod 0755 "$install_dir/Install framegen-vulkan"
standalone_readme="$repo_root/dist/framegen-vulkan-README.txt"
if [[ ! -s "$standalone_readme" ]]; then
    echo "Packaging failed: missing standalone Renderer README: $standalone_readme" >&2
    exit 1
fi
cp "$standalone_readme" "$install_dir/README.txt"
if ! grep -Fq '~/.local/bin/fgvk-launch %command%' "$install_dir/README.txt" ||
        ! grep -Fq 'ENABLE_VKBASALT=1' "$install_dir/README.txt" ||
        ! grep -Fq 'Uninstall framegen-vulkan' "$install_dir/README.txt" ||
        ! grep -Fq 'profiles, settings, and diagnostics' "$install_dir/README.txt"; then
    echo "Packaging failed: standalone Renderer README is incomplete" >&2
    exit 1
fi
printf '%s\n' "$version" > "$install_dir/framegen-vulkan-version.txt"
manifest_roots=(bin lib share)
if [[ -d "$install_dir/lib32" ]]; then
    manifest_roots+=(lib32)
fi
(
    cd "$install_dir"
    find "${manifest_roots[@]}" -type f -print | LC_ALL=C sort | xargs sha256sum
) > "$install_dir/framegen-vulkan-install-manifest.txt"
if [[ ! -s "$install_dir/framegen-vulkan-install-manifest.txt" ]]; then
    echo "Packaging failed: standalone installer manifest is empty" >&2
    exit 1
fi
if ! (cd "$install_dir" && sha256sum --check --status framegen-vulkan-install-manifest.txt); then
    echo "Packaging failed: standalone installer manifest does not verify" >&2
    exit 1
fi

if [[ ! -x "$install_dir/bin/fgvk-launch" ]]; then
    echo "Packaging failed: bin/fgvk-launch is not executable" >&2
    exit 1
fi
if [[ ! -x "$install_dir/bin/fgvk-installer" ]]; then
    echo "Packaging failed: bin/fgvk-installer is not executable" >&2
    exit 1
fi
bash -n "$install_dir/bin/fgvk-launch"
if [[ "${FGVK_RELEASE_SKIP_TESTS:-0}" != "1" ]]; then
    bash "$repo_root/scripts/test-fgvk-launch.sh" "$install_dir/bin/fgvk-launch"
fi

verify_elf_class() {
    local path="$1"
    local expected="$2"
    local actual
    actual="$(od -An -t u1 -j 4 -N 1 "$path" | tr -d '[:space:]')"
    if [[ "$actual" != "$expected" ]]; then
        echo "Packaging failed: $path has unexpected ELF class byte $actual" >&2
        exit 1
    fi
}

verify_elf_class "$install_dir/lib/libfgvk-render.so" 2
verify_elf_class "$install_dir/lib/libfgvk-render-scaling.so" 2
verify_elf_class "$install_dir/lib/vkbasalt/libvkbasalt.so" 2
if [[ "$build_32_bit" == true ]]; then
    verify_elf_class "$install_dir/lib32/libfgvk-render.so" 1
    verify_elf_class "$install_dir/lib32/libfgvk-render-scaling.so" 1
    verify_elf_class "$install_dir/lib32/vkbasalt/libvkbasalt.so" 1
fi
for vkbasalt_library in "$install_dir/lib/vkbasalt/libvkbasalt.so"; do
    if ! strings "$vkbasalt_library" |
            grep -Fx "vkBasalt_GetInstanceProcAddr" >/dev/null ||
            ! strings "$vkbasalt_library" |
                grep -Fx "vkBasalt_GetDeviceProcAddr" >/dev/null; then
        echo "Packaging failed: invalid bundled vkBasalt entry point in $vkbasalt_library" >&2
        exit 1
    fi
done
if [[ "$build_32_bit" == true ]] &&
        { ! strings "$install_dir/lib32/vkbasalt/libvkbasalt.so" |
            grep -Fx "vkBasalt_GetInstanceProcAddr" >/dev/null ||
          ! strings "$install_dir/lib32/vkbasalt/libvkbasalt.so" |
            grep -Fx "vkBasalt_GetDeviceProcAddr" >/dev/null; }; then
    echo "Packaging failed: invalid bundled 32-bit vkBasalt entry point" >&2
    exit 1
fi

vkbasalt_manifest64="$install_dir/share/framegen-vulkan/vulkan/vkbasalt.d/vkBasalt.json"
if ! grep -Fq '"name": "VK_LAYER_VKBASALT_post_processing"' "$vkbasalt_manifest64" ||
        ! grep -Fq '"library_arch": "64"' "$vkbasalt_manifest64" ||
        ! grep -Fq '../../../../lib/vkbasalt/libvkbasalt.so' "$vkbasalt_manifest64" ||
        ! grep -Fq '"ENABLE_VKBASALT": "1"' "$vkbasalt_manifest64" ||
        ! grep -Fq '"DISABLE_VKBASALT": "1"' "$vkbasalt_manifest64"; then
    echo "Packaging failed: bundled 64-bit vkBasalt manifest is incorrect" >&2
    exit 1
fi
if [[ "$build_32_bit" == true ]]; then
    vkbasalt_manifest32="$install_dir/share/framegen-vulkan/vulkan/vkbasalt.d/vkBasalt.x86.json"
    if ! grep -Fq '"library_arch": "32"' "$vkbasalt_manifest32" ||
            ! grep -Fq '../../../../lib32/vkbasalt/libvkbasalt.so' "$vkbasalt_manifest32"; then
        echo "Packaging failed: bundled 32-bit vkBasalt manifest is incorrect" >&2
        exit 1
    fi
fi

scaling_manifest64="$install_dir/share/vulkan/implicit_layer.d/VkLayer_FGVK_spatial_scaling.json"
private_scaling_manifest64="$install_dir/share/framegen-vulkan/vulkan/spatial_scaling.d/VkLayer_FGVK_spatial_scaling.json"
if ! grep -Fq '"library_arch": "64"' "$scaling_manifest64" ||
        ! grep -Fq '../../../lib/libfgvk-render-scaling.so' "$scaling_manifest64" ||
        ! grep -Fq '"name": "VK_LAYER_FGVK_spatial_scaling"' "$scaling_manifest64" ||
        ! grep -Fq '"ENABLE_FGVK_SPATIAL_SCALING": "1"' "$scaling_manifest64" ||
        ! grep -Fq '"DISABLE_FGVK_SPATIAL_SCALING": "1"' "$scaling_manifest64"; then
    echo "Packaging failed: 64-bit spatial Vulkan manifest is incorrect" >&2
    exit 1
fi
if ! grep -Fq '"library_arch": "64"' "$private_scaling_manifest64" ||
        ! grep -Fq '../../../../lib/libfgvk-render-scaling.so' "$private_scaling_manifest64" ||
        ! grep -Fq '"name": "VK_LAYER_FGVK_spatial_scaling"' "$private_scaling_manifest64"; then
    echo "Packaging failed: private 64-bit spatial Vulkan manifest is incorrect" >&2
    exit 1
fi

manifest64="$install_dir/share/vulkan/implicit_layer.d/VkLayer_FGVK_render.json"
private_manifest64="$install_dir/share/framegen-vulkan/vulkan/implicit_layer.d/VkLayer_FGVK_render.json"
if ! grep -Fq '"library_arch": "64"' "$manifest64" ||
        ! grep -Fq '../../../lib/libfgvk-render.so' "$manifest64" ||
        ! grep -Fq '"name": "VK_LAYER_FGVK_render"' "$manifest64" ||
        ! grep -Fq '"ENABLE_FGVK": "1"' "$manifest64" ||
        ! grep -Fq '"DISABLE_FGVK": "1"' "$manifest64"; then
    echo "Packaging failed: 64-bit Vulkan manifest is incorrect" >&2
    exit 1
fi
if ! grep -Fq '"library_arch": "64"' "$private_manifest64" ||
        ! grep -Fq '../../../../lib/libfgvk-render.so' "$private_manifest64" ||
        ! grep -Fq '"name": "VK_LAYER_FGVK_render"' "$private_manifest64"; then
    echo "Packaging failed: private 64-bit Vulkan manifest is incorrect" >&2
    exit 1
fi
if [[ "$build_32_bit" == true ]]; then
    manifest32="$install_dir/share/vulkan/implicit_layer.d/VkLayer_FGVK_render.x86.json"
    private_manifest32="$install_dir/share/framegen-vulkan/vulkan/implicit_layer.d/VkLayer_FGVK_render.x86.json"
    scaling_manifest32="$install_dir/share/vulkan/implicit_layer.d/VkLayer_FGVK_spatial_scaling.x86.json"
    private_scaling_manifest32="$install_dir/share/framegen-vulkan/vulkan/spatial_scaling.d/VkLayer_FGVK_spatial_scaling.x86.json"
    if ! grep -Fq '"library_arch": "32"' "$manifest32" ||
            ! grep -Fq '../../../lib32/libfgvk-render.so' "$manifest32" ||
            ! grep -Fq '"name": "VK_LAYER_FGVK_render"' "$manifest32" ||
            ! grep -Fq '"ENABLE_FGVK": "1"' "$manifest32" ||
            ! grep -Fq '"DISABLE_FGVK": "1"' "$manifest32"; then
        echo "Packaging failed: 32-bit Vulkan manifest is incorrect" >&2
        exit 1
    fi
    if ! grep -Fq '"library_arch": "32"' "$private_manifest32" ||
            ! grep -Fq '../../../../lib32/libfgvk-render.so' "$private_manifest32" ||
            ! grep -Fq '"name": "VK_LAYER_FGVK_render"' "$private_manifest32"; then
        echo "Packaging failed: private 32-bit Vulkan manifest is incorrect" >&2
        exit 1
    fi
    if ! grep -Fq '"library_arch": "32"' "$scaling_manifest32" ||
            ! grep -Fq '../../../lib32/libfgvk-render-scaling.so' "$scaling_manifest32" ||
            ! grep -Fq '"name": "VK_LAYER_FGVK_spatial_scaling"' "$scaling_manifest32" ||
            ! grep -Fq '"ENABLE_FGVK_SPATIAL_SCALING": "1"' "$scaling_manifest32" ||
            ! grep -Fq '"DISABLE_FGVK_SPATIAL_SCALING": "1"' "$scaling_manifest32"; then
        echo "Packaging failed: 32-bit spatial Vulkan manifest is incorrect" >&2
        exit 1
    fi
    if ! grep -Fq '"library_arch": "32"' "$private_scaling_manifest32" ||
            ! grep -Fq '../../../../lib32/libfgvk-render-scaling.so' "$private_scaling_manifest32" ||
            ! grep -Fq '"name": "VK_LAYER_FGVK_spatial_scaling"' "$private_scaling_manifest32"; then
        echo "Packaging failed: private 32-bit spatial Vulkan manifest is incorrect" >&2
        exit 1
    fi
fi

layer_binaries=("$install_dir/lib/libfgvk-render.so")
layer_binaries+=("$install_dir/lib/libfgvk-render-scaling.so")
if [[ "$build_32_bit" == true ]]; then
    layer_binaries+=("$install_dir/lib32/libfgvk-render.so")
    layer_binaries+=("$install_dir/lib32/libfgvk-render-scaling.so")
fi
relay_contract_symbol="fgvkSpatialScalingLookupFixedContract"
for layer_binary in "${layer_binaries[@]}"; do
    dynamic_dependencies="$(readelf -d "$layer_binary")"
    if ! grep -Fq 'Shared library: [libstdc++.so.6]' <<< "$dynamic_dependencies"; then
        echo "Packaging failed: $layer_binary must use SteamOS's dynamic C++ runtime" >&2
        echo "Static libstdc++ inside a Vulkan layer is not supported." >&2
        exit 1
    fi

    exported_symbols="$(nm -D --defined-only "$layer_binary" | awk '{print $3}')"
    for required_entrypoint in \
            vkNegotiateLoaderLayerInterfaceVersion \
            vkGetInstanceProcAddr \
            vkGetDeviceProcAddr; do
        if ! grep -Fxq "$required_entrypoint" <<< "$exported_symbols"; then
            echo "Packaging failed: Vulkan entrypoint $required_entrypoint is not exported by $layer_binary" >&2
            exit 1
        fi
    done
    expected_identity="VK_LAYER_FGVK_render"
    if [[ "$layer_binary" == *libfgvk-render-scaling.so ]]; then
        expected_identity="VK_LAYER_FGVK_spatial_scaling"
        if ! grep -Fxq "$relay_contract_symbol" <<< "$exported_symbols"; then
            echo "Packaging failed: spatial capability relay entrypoint is missing from $layer_binary" >&2
            exit 1
        fi
    elif grep -Fxq "$relay_contract_symbol" <<< "$exported_symbols"; then
        echo "Packaging failed: spatial capability relay entrypoint leaked into $layer_binary" >&2
        exit 1
    fi
    if ! strings "$layer_binary" |
            grep -F "framegen-vulkan: render layer active; identity=$expected_identity; build=$version" >/dev/null; then
        echo "Packaging failed: layer build identity diagnostic is missing from $layer_binary" >&2
        exit 1
    fi
    if ! strings "$layer_binary" | grep -F "FGVK_PROFILE_FALLBACK" >/dev/null; then
        echo "Packaging failed: profile-fallback wrapper protocol is missing from $layer_binary" >&2
        exit 1
    fi
done

tar -C "$install_dir" -cJf "$output_path" .

echo "Created and verified: $output_path"
echo "Version: $version"
echo "Architectures: $([[ "$build_32_bit" == true ]] && printf '64,32' || printf '64')"

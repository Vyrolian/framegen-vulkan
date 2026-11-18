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
version="$(tr -d '[:space:]' < "$repo_root/VERSION")"
default_output="$repo_root/out/framegen-vulkan-v$version-linux.tar.xz"
output_path=""
build_32_bit=true

usage() {
    cat <<'EOF'
Usage: scripts/package-local.sh [--64-bit-only] [output-path]

Build and verify a local Linux engine archive. --64-bit-only omits the 32-bit
layer and manifest for faster native 64-bit Deck/Steam Machine test builds.
The archive is reproducible for a given commit: SOURCE_DATE_EPOCH (default: the
commit time of HEAD) sets every archive timestamp. The build is native only and
runs on Linux; any other host exits with status 2.
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
if [[ "$(uname -s)" != "Linux" ]]; then
    echo "Native packaging runs only on Linux; this host is $(uname -s)." >&2
    exit 2
fi

# Archive timestamps come from the source commit, never from the wall clock, so two builds of one
# commit match byte for byte. Sources without git history have no commit time: they warn and use
# the current time, because nothing else identifies the source.
source_date_epoch="${SOURCE_DATE_EPOCH:-}"
if [[ -z "$source_date_epoch" ]]; then
    source_date_epoch="$(git -C "$repo_root" log -1 --format=%ct HEAD 2>/dev/null || true)"
fi
if [[ -z "$source_date_epoch" ]]; then
    echo "framegen-vulkan: no git history and no SOURCE_DATE_EPOCH; archive timestamps will vary." >&2
    source_date_epoch="$(date +%s)"
fi
if [[ ! "$source_date_epoch" =~ ^[0-9]+$ ]]; then
    echo "SOURCE_DATE_EPOCH must be a Unix time in seconds: $source_date_epoch" >&2
    exit 2
fi
export SOURCE_DATE_EPOCH="$source_date_epoch"

vulkan_headers_revision="$(< "$repo_root/vulkan-headers-revision.txt")"
if [[ ! "$vulkan_headers_revision" =~ ^(vulkan-sdk-|v)[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "vulkan-headers-revision.txt must contain a Vulkan-Headers SDK branch (vulkan-sdk-X.Y.Z) or release tag (vX.Y.Z)." >&2
    exit 1
fi

# fgvk (the settings window and `fgvk <command>` entry point) is built with the host toolchain.
fgvk_ui_binary=""
build_fgvk_ui() {
    if ! command -v cargo >/dev/null 2>&1; then
        echo "Packaging needs cargo to build fgvk. Install the Rust toolchain (rustup, or your distribution's cargo package) and try again." >&2
        exit 1
    fi
    fgvk_ui_target_dir="$build_work_root/fgvk-ui-target"
    mkdir -p "$fgvk_ui_target_dir"
    echo "Building fgvk with host cargo..."
    # Rust panic locations, dependency paths and the standard-library source paths embedded from the
    # toolchain sysroot must not name this checkout, the Cargo home or a rustup directory. Cargo
    # reads CARGO_ENCODED_RUSTFLAGS as 0x1f-separated arguments, so checkout paths with spaces stay
    # whole. RUSTFLAGS already set in the environment is kept ahead of the remaps.
    cargo_home="${CARGO_HOME:-${HOME:-}/.cargo}"
    rust_sysroot="$(rustc --print sysroot)"
    rust_flags=()
    if [[ -n "${RUSTFLAGS:-}" ]]; then
        read -r -a rust_flags <<< "$RUSTFLAGS"
    fi
    rust_flags+=(
        "--remap-path-prefix=$monorepo_root=."
        "--remap-path-prefix=$cargo_home=cargo-home"
        "--remap-path-prefix=$rust_sysroot=rust-sysroot"
    )
    encoded_rustflags=""
    for rust_flag in "${rust_flags[@]}"; do
        encoded_rustflags+="$rust_flag"$'\x1f'
    done
    CARGO_ENCODED_RUSTFLAGS="${encoded_rustflags%$'\x1f'}" \
        cargo build --release --locked \
        --manifest-path "$repo_root/fgvk-ui/Cargo.toml" \
        --target-dir "$fgvk_ui_target_dir"
    fgvk_ui_binary="$fgvk_ui_target_dir/release/fgvk"
    if [[ ! -x "$fgvk_ui_binary" ]]; then
        echo "Packaging failed: cargo did not produce $fgvk_ui_binary" >&2
        exit 1
    fi
}
build_fgvk_ui

for command in cmake ninja clang++ nm python3 readelf strings strip tar xz sha256sum; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required command not found: $command" >&2
        exit 1
    fi
done

# Builds compile against one explicit Vulkan-Headers include directory, so the pinned revision
# (not the host SDK) is what the CMake header check and every compile see.
native_header_args=()
if [[ -n "${FGVK_VULKAN_HEADERS_INCLUDE_DIR:-}" ]]; then
    native_vulkan_headers_include_dir="$FGVK_VULKAN_HEADERS_INCLUDE_DIR"
    if [[ "$native_vulkan_headers_include_dir" != /* ]]; then
        native_vulkan_headers_include_dir="$PWD/$native_vulkan_headers_include_dir"
    fi
    header_source="FGVK_VULKAN_HEADERS_INCLUDE_DIR"
else
    headers_parent="$repo_root/build/vulkan-headers"
    headers_root="$headers_parent/$vulkan_headers_revision"
    if [[ ! -f "$headers_root/include/vulkan/vulkan_core.h" ]]; then
        if ! command -v git >/dev/null 2>&1; then
            echo "Native packaging needs git to fetch Vulkan-Headers $vulkan_headers_revision. Install git or set FGVK_VULKAN_HEADERS_INCLUDE_DIR." >&2
            exit 1
        fi
        echo "Fetching Vulkan-Headers $vulkan_headers_revision into $headers_root..."
        mkdir -p "$headers_parent"
        # Clone beside the final directory and rename only after the clone succeeds, so an
        # interrupted fetch never leaves a directory that looks complete.
        clone_dir="$(mktemp -d "$headers_parent/.$vulkan_headers_revision.partial.XXXXXX")"
        if ! git clone --depth=1 -b "$vulkan_headers_revision" \
                https://github.com/KhronosGroup/Vulkan-Headers "$clone_dir"; then
            rm -rf "$clone_dir"
            echo "Failed to fetch Vulkan-Headers $vulkan_headers_revision from GitHub." >&2
            exit 1
        fi
        rm -rf "$headers_root"
        mv "$clone_dir" "$headers_root"
    fi
    native_vulkan_headers_include_dir="$headers_root/include"
    header_source="pinned $vulkan_headers_revision"
fi
if [[ ! -f "$native_vulkan_headers_include_dir/vulkan/vulkan_core.h" ]]; then
    echo "Vulkan-Headers include directory does not contain vulkan/vulkan_core.h: $native_vulkan_headers_include_dir" >&2
    exit 1
fi
echo "Vulkan-Headers include directory ($header_source): $native_vulkan_headers_include_dir"
native_header_args+=("-DFGVK_VULKAN_HEADERS_INCLUDE_DIR=$native_vulkan_headers_include_dir")


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
# The C++ compile maps the checkout root to "." so __FILE__, assertions and any debug paths in the
# binaries name no host directory. The project is C++ only, so no C flags are needed.
source_prefix_map_flag="-ffile-prefix-map=$monorepo_root=."
cmake -S "$repo_root" -B "$build64_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$install_dir" \
    -DCMAKE_CXX_COMPILER=clang++ \
    "-DCMAKE_CXX_FLAGS=$source_prefix_map_flag" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DBUILD_TESTING="$build_testing" \
    -DFGVK_REQUIRE_NATIVE_PACKAGE_HEADERS=ON \
    "${native_header_args[@]}" \
    -DFGVK_BUILD_VK_LAYER=ON \
    -DFGVK_BUILD_CLI=ON \
    -DFGVK_LAYER_LIBRARY_PATH="../../../lib/libfgvk-render.so"

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
        "-DCMAKE_CXX_FLAGS=-m32 $source_prefix_map_flag" \
        -DCMAKE_SHARED_LINKER_FLAGS=-m32 \
        -DCMAKE_INSTALL_LIBDIR=lib32 \
        -DBUILD_TESTING=OFF \
        -DFGVK_REQUIRE_NATIVE_PACKAGE_HEADERS=ON \
        "${native_header_args[@]}" \
        -DFGVK_BUILD_VK_LAYER=ON \
        -DFGVK_BUILD_CLI=OFF \
        -DFGVK_LAYER_MANIFEST_SUFFIX=.x86 \
        -DFGVK_LAYER_LIBRARY_PATH="../../../lib32/libfgvk-render.so"

    cmake --build "$build32_dir" --target fgvk-render
    cmake --install "$build32_dir" --strip
fi

# The settings window and `fgvk <command>` entry point. Built above with host cargo; the
# staged copy is stripped like the other native tools in this archive.
install -Dm755 "$fgvk_ui_binary" "$install_dir/bin/fgvk"
strip --strip-unneeded "$install_dir/bin/fgvk"
# The host-built fgvk inherits the build host's glibc symbol versions. Warn when it
# will not start on the Ubuntu 22.04 (glibc 2.35) baseline of the release-build workflow.
fgvk_max_glibc="$(readelf -V "$install_dir/bin/fgvk" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n 1 || true)"
if [[ -n "$fgvk_max_glibc" && "$(printf '%s\n' "${fgvk_max_glibc#GLIBC_}" 2.35 | sort -V | tail -n 1)" != "2.35" ]]; then
    echo "Warning: bin/fgvk requires $fgvk_max_glibc from the build host; it will not start on systems with an older glibc (Ubuntu 22.04 provides 2.35)." >&2
fi

required_paths=(
    "bin/fgvk" \
    "libexec/framegen-vulkan/fgvk-cli" \
    "libexec/framegen-vulkan/fgvk-installer" \
    "libexec/framegen-vulkan/fgvk-launch" \
    "lib/libfgvk-render.so" \
    "share/doc/framegen-vulkan/ASSET_PROVENANCE.md" \
    "share/doc/framegen-vulkan/COPYRIGHT.md" \
    "share/doc/framegen-vulkan/LICENSE" \
    "share/doc/framegen-vulkan/THIRD_PARTY_NOTICES.md" \
    "share/doc/framegen-vulkan/themes/example.toml" \
    "share/framegen-vulkan/vulkan/implicit_layer.d/VkLayer_FGVK_render.json" \
    "share/vulkan/implicit_layer.d/VkLayer_FGVK_render.json"
)
if [[ "$build_32_bit" == true ]]; then
    required_paths+=(
        "lib32/libfgvk-render.so"
        "share/framegen-vulkan/vulkan/implicit_layer.d/VkLayer_FGVK_render.x86.json"
        "share/vulkan/implicit_layer.d/VkLayer_FGVK_render.x86.json"
    )
fi
for required_path in "${required_paths[@]}"; do
    if [[ ! -e "$install_dir/$required_path" ]]; then
        echo "Packaging failed: missing $required_path" >&2
        exit 1
    fi
done

# Users extract the archive and run ./install. It copies the verified payload into their user-local
# prefix. The generated manifest gives upgrades and uninstalls an exact,
# checksummed ownership record without treating the entire ~/.local tree as
# disposable.
install -m755 "$repo_root/scripts/fgvk-installer" "$install_dir/install"
standalone_readme="$repo_root/dist/framegen-vulkan-README.txt"
if [[ ! -s "$standalone_readme" ]]; then
    echo "Packaging failed: missing standalone Renderer README: $standalone_readme" >&2
    exit 1
fi
cp "$standalone_readme" "$install_dir/README.txt"
if ! grep -Fq '~/.local/bin/fgvk %command%' "$install_dir/README.txt" ||
        ! grep -Fq '~/.local/bin/fgvk uninstall' "$install_dir/README.txt" ||
        ! grep -Fq 'profiles, settings, and diagnostics' "$install_dir/README.txt"; then
    echo "Packaging failed: standalone Renderer README is incomplete" >&2
    exit 1
fi
printf '%s\n' "$version" > "$install_dir/framegen-vulkan-version.txt"
manifest_roots=(bin libexec lib share)
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

if [[ ! -x "$install_dir/bin/fgvk" ]]; then
    echo "Packaging failed: bin/fgvk is not executable" >&2
    exit 1
fi
if [[ "$(find "$install_dir/bin" -mindepth 1 -maxdepth 1 -printf '%f\n')" != "fgvk" ]]; then
    echo "Packaging failed: bin must contain only fgvk" >&2
    exit 1
fi
if [[ ! -x "$install_dir/libexec/framegen-vulkan/fgvk-cli" ]]; then
    echo "Packaging failed: libexec/framegen-vulkan/fgvk-cli is not executable" >&2
    exit 1
fi
if [[ ! -x "$install_dir/libexec/framegen-vulkan/fgvk-launch" ]]; then
    echo "Packaging failed: libexec/framegen-vulkan/fgvk-launch is not executable" >&2
    exit 1
fi
if [[ ! -x "$install_dir/libexec/framegen-vulkan/fgvk-installer" ]]; then
    echo "Packaging failed: libexec/framegen-vulkan/fgvk-installer is not executable" >&2
    exit 1
fi
bash -n "$install_dir/libexec/framegen-vulkan/fgvk-launch"
if [[ "${FGVK_RELEASE_SKIP_TESTS:-0}" != "1" ]]; then
    bash "$repo_root/scripts/test-fgvk-launch.sh" "$install_dir/libexec/framegen-vulkan/fgvk-launch"
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

verify_elf_class "$install_dir/bin/fgvk" 2
verify_elf_class "$install_dir/libexec/framegen-vulkan/fgvk-cli" 2
verify_elf_class "$install_dir/lib/libfgvk-render.so" 2
if [[ "$build_32_bit" == true ]]; then
    verify_elf_class "$install_dir/lib32/libfgvk-render.so" 1
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
fi

layer_binaries=("$install_dir/lib/libfgvk-render.so")
if [[ "$build_32_bit" == true ]]; then
    layer_binaries+=("$install_dir/lib32/libfgvk-render.so")
fi
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

# Modes follow the builder's umask unless normalized, so normalize them before archiving:
# directories and executables are 0755, every other file is 0644.
find "$install_dir" -type d -exec chmod 0755 {} +
find "$install_dir" -type f -perm -u+x -exec chmod 0755 {} +
find "$install_dir" -type f ! -perm -u+x -exec chmod 0644 {} +
# Archive bytes depend only on the payload: sorted members, one timestamp, root ownership, no
# access or change times in member headers, and one xz thread for a stable compressed stream.
tar --sort=name --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 --numeric-owner \
    --pax-option='exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime' \
    -C "$install_dir" -cf - . | xz -T1 -c > "$output_path"

echo "Created and verified: $output_path"
echo "Version: $version"
echo "Architectures: $([[ "$build_32_bit" == true ]] && printf '64,32' || printf '64')"
echo "SHA-256: $(sha256sum "$output_path" | awk '{print $1}')"

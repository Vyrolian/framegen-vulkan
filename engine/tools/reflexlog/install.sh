#!/usr/bin/env bash
# framegen-vulkan reflexlog: build and install the testing-only capture layer.
# Installs only into ~/.local. Not packaged. Use --uninstall to remove it.
set -euo pipefail

usage() {
    cat <<'EOF'
usage: install.sh [--uninstall]

Builds reflexlog in engine/build/reflexlog and installs:
  ~/.local/lib/libVkLayer_reflexlog.so
  ~/.local/share/vulkan/implicit_layer.d/VkLayer_reflexlog.json
  ~/.local/bin/reflexlog

--uninstall removes those three files and leaves the build directory alone.
EOF
}

mode=install
if [ "$#" -gt 1 ]; then
    usage >&2
    exit 2
fi
case "${1:-}" in
    "") mode=install ;;
    --uninstall) mode=uninstall ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
engine_dir="$(cd "$script_dir/../.." && pwd)"
build_dir="$engine_dir/build/reflexlog"

lib_dest="$HOME/.local/lib/libVkLayer_reflexlog.so"
manifest_dest="$HOME/.local/share/vulkan/implicit_layer.d/VkLayer_reflexlog.json"
wrapper_dest="$HOME/.local/bin/reflexlog"

if [ "$mode" = uninstall ]; then
    for installed in "$lib_dest" "$manifest_dest" "$wrapper_dest"; do
        if [ -e "$installed" ] || [ -L "$installed" ]; then
            rm -f -- "$installed"
            echo "framegen-vulkan: reflexlog: removed $installed"
        else
            echo "framegen-vulkan: reflexlog: not installed: $installed"
        fi
    done
    exit 0
fi

cmake -S "$script_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" -j"$(nproc)"

install -d "$(dirname "$lib_dest")" "$(dirname "$manifest_dest")" "$(dirname "$wrapper_dest")"
install -m 0755 "$build_dir/libVkLayer_reflexlog.so" "$lib_dest"
install -m 0644 "$script_dir/VkLayer_reflexlog.json" "$manifest_dest"
install -m 0755 "$script_dir/reflexlog" "$wrapper_dest"

echo "framegen-vulkan: reflexlog: installed $lib_dest"
echo "framegen-vulkan: reflexlog: installed $manifest_dest"
echo "framegen-vulkan: reflexlog: installed $wrapper_dest"
echo "framegen-vulkan: reflexlog: Steam launch option: reflexlog %command%"

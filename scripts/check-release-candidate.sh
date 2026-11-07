#!/usr/bin/env bash
# Verify already-built complete local MAKO Renderer archives before publication.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'EOF'
Usage: scripts/check-release-candidate.sh PATH-TO-HOST-ARCHIVE [PATH-TO-FLATPAK-ARCHIVE]

Checks complete, portable local MAKO Renderer archives from the current clean,
pushed commit. The host archive must be MAKO-Renderer-vX.Y.Z-linux.tar.xz for
engine/VERSION. The optional Flatpak archive must contain one bundle for every
runtime in the shared Flatpak catalogue. This does not build, install, deploy,
publish, or run MAKO Gym.
EOF
}

if (($# == 1 || $# == 2)) && [[ "$1" == --help || "$1" == -h ]]; then
  usage
  exit 0
fi
if (($# < 1 || $# > 2)); then
  usage >&2
  exit 2
fi

for command in git realpath sha256sum tar; do
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "Required command not found: $command" >&2
    exit 1
  fi
done

host_archive="$(realpath -e -- "$1")"
if [[ ! -f "$host_archive" ]]; then
  echo "MAKO Renderer candidate is not a file: $host_archive" >&2
  exit 1
fi
flatpak_archive=""
if (($# == 2)); then
  flatpak_archive="$(realpath -e -- "$2")"
  if [[ ! -f "$flatpak_archive" ]]; then
    echo "MAKO Renderer Flatpak candidate is not a file: $flatpak_archive" >&2
    exit 1
  fi
fi

version="$(tr -d '[:space:]' < "$repo_root/engine/VERSION")"
if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "engine/VERSION must use X.Y.Z format: $version" >&2
  exit 1
fi
expected_host_name="MAKO-Renderer-v$version-linux.tar.xz"
if [[ "$(basename "$host_archive")" != "$expected_host_name" ]]; then
  echo "The host candidate must be $expected_host_name, received $(basename "$host_archive")." >&2
  exit 1
fi

cd "$repo_root"
if [[ -n "$(git status --porcelain --untracked-files=normal)" ]]; then
  echo "Release-candidate checks require a clean worktree." >&2
  exit 1
fi
branch="$(git branch --show-current)"
if [[ -z "$branch" ]] || ! git check-ref-format --branch "$branch" >/dev/null 2>&1; then
  echo "Release-candidate checks require a named branch." >&2
  exit 1
fi
git fetch --quiet origin "refs/heads/$branch:refs/remotes/origin/$branch"
source_commit="$(git rev-parse HEAD)"
engine_commit="$(git -C "$repo_root/engine" log -1 --format=%H -- .)"
if [[ "$source_commit" != "$(git rev-parse "refs/remotes/origin/$branch")" ]]; then
  echo "The candidate branch must match origin/$branch." >&2
  exit 1
fi
engine_short_commit="${engine_commit:0:12}"

work_root="$(mktemp -d "${TMPDIR:-/tmp}/mako-release-candidate.XXXXXX")"
trap 'rm -rf -- "$work_root"' EXIT

# Extract the host archive and verify its declared identity, checksums, layers,
# and build fingerprint. The fingerprint is compiled from the clean engine commit.
mkdir -p "$work_root/host"
tar -tJf "$host_archive" > "$work_root/host-files.txt"
tar -xJf "$host_archive" -C "$work_root/host"
package_root="$work_root/host"
if [[ "$(tr -d '[:space:]' < "$package_root/MAKO-Renderer-version.txt")" != "$version" ]]; then
  echo "The host archive does not contain MAKO Renderer $version." >&2
  exit 1
fi
(
  cd "$package_root"
  sha256sum --check --status MAKO-Renderer-install-manifest.txt
) || {
  echo "The host archive does not match its install manifest." >&2
  exit 1
}
for renderer_file in ./bin/mako-launch ./bin/mako-cli ./lib/libmako-render.so \
  ./lib32/libmako-render.so ./share/vulkan/implicit_layer.d/VkLayer_MAKO_render.json; do
  if ! grep -Fxq "$renderer_file" "$work_root/host-files.txt"; then
    echo "The host archive is missing $renderer_file." >&2
    exit 1
  fi
done
for layer in "$package_root/lib/libmako-render.so" "$package_root/lib32/libmako-render.so"; do
  if ! grep -aFq -- "$engine_short_commit" "$layer"; then
    echo "${layer#"$package_root"/} was not built from the current clean engine commit." >&2
    exit 1
  fi
  if grep -aFq -- "$engine_short_commit.dirty." "$layer"; then
    echo "${layer#"$package_root"/} was built from a dirty engine worktree." >&2
    exit 1
  fi
done

if [[ -n "$flatpak_archive" ]]; then
  if [[ "$(basename "$flatpak_archive")" != "MAKO-Renderer-v$version-flatpaks.tar.xz" ]]; then
    echo "The Flatpak candidate must be MAKO-Renderer-v$version-flatpaks.tar.xz." >&2
    exit 1
  fi
  tar -tJf "$flatpak_archive" | sed 's|^\./||' > "$work_root/flatpak-files.txt"
  mkdir -p "$work_root/flatpaks"
  tar -xJf "$flatpak_archive" -C "$work_root/flatpaks"
  runtime_count=0
  while IFS= read -r runtime_version || [[ -n "$runtime_version" ]]; do
    [[ -n "$runtime_version" && "$runtime_version" != \#* ]] || continue
    runtime_count=$((runtime_count + 1))
    bundle="org.freedesktop.Platform.VulkanLayer.makorender-$runtime_version.flatpak"
    if ! grep -Fxq "$bundle" "$work_root/flatpak-files.txt" ||
        [[ ! -s "$work_root/flatpaks/$bundle" ]]; then
      echo "The complete candidate is missing Flatpak bundle $bundle." >&2
      exit 1
    fi
  done < "$repo_root/engine/dist/flatpak/mako-render/runtime-versions.txt"
  if ((runtime_count == 0)); then
    echo "The Flatpak runtime catalogue contains no runtimes." >&2
    exit 1
  fi
fi

echo "Verified complete MAKO Renderer release candidate: $host_archive"
if [[ -n "$flatpak_archive" ]]; then
  echo "Verified Flatpak extensions: $flatpak_archive"
fi
echo "Source commit: $source_commit"
sha256sum "$host_archive"
if [[ -n "$flatpak_archive" ]]; then
  sha256sum "$flatpak_archive"
fi

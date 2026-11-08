#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_root="$(mktemp -d "${TMPDIR:-/tmp}/fgvk-release-candidate-test.XXXXXX")"
trap 'rm -rf -- "$test_root"' EXIT
fixture_repo="$test_root/repo"
mkdir -p "$fixture_repo/scripts" "$fixture_repo/engine/dist/flatpak/fgvk-render"
cp "$repo_root/scripts/check-release-candidate.sh" "$fixture_repo/scripts/"
printf '0.0.0\n' > "$fixture_repo/engine/VERSION"
printf '%s\n' runtime-a runtime-b > "$fixture_repo/engine/dist/flatpak/fgvk-render/runtime-versions.txt"
printf 'engine source\n' > "$fixture_repo/engine/source.txt"

git -C "$fixture_repo" init -q -b main
git -C "$fixture_repo" config user.name 'Release test'
git -C "$fixture_repo" config user.email 'release-test@example.invalid'
git -C "$fixture_repo" add .
git -C "$fixture_repo" commit -qm 'Create candidate check fixture'
git init -q --bare "$test_root/origin.git"
git -C "$fixture_repo" remote add origin "$test_root/origin.git"
git -C "$fixture_repo" push -q -u origin main
source_commit="$(git -C "$fixture_repo" rev-parse HEAD)"
engine_commit="$(git -C "$fixture_repo" log -1 --format=%H -- engine)"
engine_short="${engine_commit:0:12}"
other_short="0123456789ab"

# make_host DIRECTORY FINGERPRINT [MODE]: writes a complete host payload tree.
make_host() {
  local directory="$1" fingerprint="$2" mode="${3:-complete}"
  rm -rf -- "$directory"
  mkdir -p "$directory/bin" "$directory/libexec/framegen-vulkan" "$directory/lib" "$directory/lib32" \
    "$directory/share/vulkan/implicit_layer.d"
  printf '0.0.0\n' > "$directory/framegen-vulkan-version.txt"
  printf '#!/bin/sh\n' > "$directory/bin/fgvk"
  printf '#!/bin/sh\n' > "$directory/libexec/framegen-vulkan/fgvk-launch"
  printf '#!/bin/sh\n' > "$directory/bin/fgvk-cli"
  printf 'layer %s\n' "$fingerprint" > "$directory/lib/libfgvk-render.so"
  printf 'layer32 %s\n' "$fingerprint" > "$directory/lib32/libfgvk-render.so"
  printf '{}\n' > "$directory/share/vulkan/implicit_layer.d/VkLayer_FGVK_render.json"
  if [[ "$mode" == complete ]]; then
    (
      cd "$directory"
      find bin libexec lib lib32 share -type f -print | LC_ALL=C sort | xargs sha256sum
    ) > "$directory/framegen-vulkan-install-manifest.txt"
  else
    : > "$directory/framegen-vulkan-install-manifest.txt"
  fi
}

# make_host_archive ARCHIVE FINGERPRINT [MODE]
make_host_archive() {
  local archive="$1" fingerprint="$2" mode="${3:-complete}"
  local directory="$test_root/host-tree"
  make_host "$directory" "$fingerprint" "$mode"
  tar -cJf "$archive" -C "$directory" .
}

# make_flatpak_archive ARCHIVE [MISSING_RUNTIME]
make_flatpak_archive() {
  local archive="$1" missing="${2:-}"
  local directory="$test_root/flatpak-tree"
  rm -rf -- "$directory"
  mkdir -p "$directory"
  for runtime in runtime-a runtime-b; do
    if [[ "$runtime" != "$missing" ]]; then
      printf 'bundle %s\n' "$runtime" > \
        "$directory/org.freedesktop.Platform.VulkanLayer.fgvkrender-$runtime.flatpak"
    fi
  done
  tar -cJf "$archive" -C "$directory" .
}

host_name="framegen-vulkan-v0.0.0-linux.tar.xz"
flatpak_name="framegen-vulkan-v0.0.0-flatpaks.tar.xz"
mkdir -p "$test_root/candidates"

make_host_archive "$test_root/candidates/$host_name" "$engine_short"
make_flatpak_archive "$test_root/candidates/$flatpak_name"
"$fixture_repo/scripts/check-release-candidate.sh" \
  "$test_root/candidates/$host_name" "$test_root/candidates/$flatpak_name" > /dev/null
"$fixture_repo/scripts/check-release-candidate.sh" "$test_root/candidates/$host_name" > /dev/null

# A later commit outside engine/ does not stale the engine build fingerprint.
printf 'Release guide changed.\n' > "$fixture_repo/HOW_TO_RELEASE.md"
git -C "$fixture_repo" add HOW_TO_RELEASE.md
git -C "$fixture_repo" commit -qm 'Update release guide only'
git -C "$fixture_repo" push -q origin main
"$fixture_repo/scripts/check-release-candidate.sh" \
  "$test_root/candidates/$host_name" "$test_root/candidates/$flatpak_name" > /dev/null

expect_rejection() {
  local description="$1"
  shift
  if "$fixture_repo/scripts/check-release-candidate.sh" "$@" > /dev/null 2>&1; then
    echo "Release-candidate check accepted $description." >&2
    exit 1
  fi
}

mkdir -p "$test_root/candidates/stale"
make_host_archive "$test_root/candidates/stale/$host_name" "$other_short"
expect_rejection "a host built from another source commit" "$test_root/candidates/stale/$host_name"

mkdir -p "$test_root/candidates/dirty"
make_host_archive "$test_root/candidates/dirty/$host_name" "$engine_short.dirty.12345678"
expect_rejection "a host built from a dirty engine worktree" "$test_root/candidates/dirty/$host_name"

mkdir -p "$test_root/candidates/unchecked"
make_host_archive "$test_root/candidates/unchecked/$host_name" "$engine_short" no-manifest
expect_rejection "a host without an install manifest" "$test_root/candidates/unchecked/$host_name"

mkdir -p "$test_root/candidates/renamed"
make_host_archive "$test_root/candidates/renamed/framegen-vulkan-v9.9.9-linux.tar.xz" "$engine_short"
expect_rejection "a host with the wrong version name" "$test_root/candidates/renamed/framegen-vulkan-v9.9.9-linux.tar.xz"

mkdir -p "$test_root/candidates/incomplete-flatpak"
make_flatpak_archive "$test_root/candidates/incomplete-flatpak/$flatpak_name" runtime-b
expect_rejection "a Flatpak archive without every runtime" \
  "$test_root/candidates/$host_name" "$test_root/candidates/incomplete-flatpak/$flatpak_name"

echo 'Release-candidate archive checks passed.'

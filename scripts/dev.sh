#!/usr/bin/env bash
# framegen-vulkan development entry point: one subcommand per build, check and test gate.
# Run `scripts/dev.sh help` for the list. Subcommands run from the repository root whatever the
# caller's directory. Builds and tests use every core unless FGVK_JOBS is set.
set -euo pipefail

script_directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repository_root="$(cd "$script_directory/.." && pwd)"
invocation_directory="$PWD"
cd "$repository_root"

jobs=1
subcommand=help

die() {
  printf 'framegen-vulkan dev: %s\n' "$*" >&2
  exit 2
}

usage() {
  cat <<'EOF'
framegen-vulkan development entry point. Run it from any directory in the repository.

Usage: scripts/dev.sh <subcommand> [argument]

Builds and tests use every core. FGVK_JOBS=<n> sets the count, for example FGVK_JOBS=2 while the user may be gaming.

Subcommands:
  help                               List these subcommands.
  build                              Configure and build framegen-vulkan in release mode.
  build-engine                       Same as build.
  fgvk-ui                            Build the fgvk settings window and entry point in release mode.
  test                               Protected inputs, release candidate, fgvk-ui and the full Renderer CTest suite.
  test-all                           Protected inputs, release candidate, fgvk-ui and the portable Renderer policy tests.
  test-protected-inputs              Reject licensed inputs and binary payloads in Git.
  test-release-candidate             Verify the local release-candidate checker with complete and incomplete ZIP fixtures.
  test-fgvk-ui                       Run the fgvk crate tests: argument routing, config, launcher state and the process picker.
  test-engine                        Build and run the full framegen-vulkan CTest suite.
  test-engine-portable               Run the portable Renderer policy tests with Vulkan headers and no Vulkan device.
  test-engine-sanitized              Run the portable Renderer policy tests under ASan and UBSan.
  package-engine                     Build the native framegen-vulkan release archive.
  package-flatpaks                   Build the framegen-vulkan Flatpak extension archives.
  package-renderer-local-release     Build the complete release-shaped artifact set without publishing.
  check-release-candidate <archive>  Check built complete archives against the pushed source commit (host, optional Flatpak).
  check-launcher-exclusions          Check the launcher exclusion bindings without rewriting them.
  generate-launcher-exclusions       Generate the launcher exclusion bindings from the shared registry.
  check-flatpak-runtimes             Check the generated Flatpak runtime manifests without rewriting them.
  generate-flatpak-runtimes          Generate the Flatpak runtime manifests from the shared catalogue.
  check-flatpak-headers              Reject a stale Flatpak Vulkan-Headers module without rewriting it.
  generate-flatpak-headers           Generate the Flatpak Vulkan-Headers module from the shared pin.
  sync-arch-package                  Sync the Arch package recipe to the checksum-pinned release.
  check-arch-package                 Check the Arch release pin and the Arch package contract.
  check-color-conversion-spirv       Reject stale embedded colour-conversion SPIR-V without invoking glslangValidator.
  install-hooks                      Enable the commit hook that rejects licensed inputs (Lossless.dll) and binary payloads.
  update-contributors                Update CONTRIBUTORS.md from this branch's history.
  check-contributors                 Check that CONTRIBUTORS.md matches this branch's history.
  check-links                        Check every relative Markdown link and #anchor in the tracked Markdown files.
EOF
}

no_arguments() {
  (($# == 0)) || die "$subcommand takes no argument"
}

# FGVK_JOBS overrides the core count. Invalid values stop before any build starts.
resolve_jobs() {
  if [[ -n "${FGVK_JOBS:-}" ]]; then
    [[ "$FGVK_JOBS" =~ ^[1-9][0-9]*$ ]] || die "FGVK_JOBS must be a positive integer, got '$FGVK_JOBS'"
    jobs="$FGVK_JOBS"
  else
    jobs="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
  fi
}

build_engine() {
  resolve_jobs
  cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Release
  cmake --build engine/build --parallel "$jobs"
}

fgvk_ui() {
  resolve_jobs
  cargo build --release --locked --manifest-path engine/fgvk-ui/Cargo.toml --jobs "$jobs"
}

test_fgvk_ui() {
  resolve_jobs
  cargo test --locked --manifest-path engine/fgvk-ui/Cargo.toml --jobs "$jobs"
}

test_engine() {
  build_engine
  ctest --test-dir engine/build --output-on-failure -j "$jobs"
}

# The portable policy script calls cmake --build without a job count; CMAKE_BUILD_PARALLEL_LEVEL
# gives that step every core without changing the script.
test_engine_portable() {
  resolve_jobs
  (cd engine && CMAKE_BUILD_PARALLEL_LEVEL="$jobs" ./scripts/test-adaptive-scheduler.sh)
}

test_engine_sanitized() {
  resolve_jobs
  (cd engine && CMAKE_BUILD_PARALLEL_LEVEL="$jobs" FGVK_ENABLE_SANITIZERS=ON ./scripts/test-adaptive-scheduler.sh)
}

test_protected_inputs() {
  ./scripts/test-protected-inputs.sh
}

test_release_candidate() {
  ./scripts/test-release-candidate.sh
}

check_arch_package() {
  ./engine/dist/arch/check-release-pin.sh
  ./engine/dist/arch/test-package-contract.sh
}

main() {
  subcommand="${1:-help}"
  if (($# > 0)); then
    shift
  fi

  case "$subcommand" in
    help) no_arguments "$@"; usage ;;
    build) no_arguments "$@"; build_engine ;;
    build-engine) no_arguments "$@"; build_engine ;;
    fgvk-ui) no_arguments "$@"; fgvk_ui ;;
    test)
      no_arguments "$@"
      test_protected_inputs
      test_release_candidate
      test_fgvk_ui
      test_engine
      ;;
    test-all)
      no_arguments "$@"
      test_protected_inputs
      test_release_candidate
      test_fgvk_ui
      test_engine_portable
      ;;
    test-protected-inputs) no_arguments "$@"; test_protected_inputs ;;
    test-release-candidate) no_arguments "$@"; test_release_candidate ;;
    test-fgvk-ui) no_arguments "$@"; test_fgvk_ui ;;
    test-engine) no_arguments "$@"; test_engine ;;
    test-engine-portable) no_arguments "$@"; test_engine_portable ;;
    test-engine-sanitized) no_arguments "$@"; test_engine_sanitized ;;
    package-engine) no_arguments "$@"; ./engine/scripts/package-local.sh ;;
    package-flatpaks) no_arguments "$@"; ./engine/scripts/package-flatpaks.sh ;;
    package-renderer-local-release) no_arguments "$@"; ./engine/scripts/package-local-release.sh ;;
    check-release-candidate)
      (($# >= 1 && $# <= 2)) || die "check-release-candidate takes a host archive and an optional Flatpak archive"
      # Archive paths are relative to the caller's directory, not the repository root.
      candidate_arguments=()
      for argument in "$@"; do
        if [[ "$argument" == -* || "$argument" == /* ]]; then
          candidate_arguments+=("$argument")
        else
          candidate_arguments+=("$invocation_directory/${argument#./}")
        fi
      done
      ./scripts/check-release-candidate.sh "${candidate_arguments[@]}"
      ;;
    check-launcher-exclusions) no_arguments "$@"; python3 scripts/generate-launcher-exclusions.py --check ;;
    generate-launcher-exclusions) no_arguments "$@"; python3 scripts/generate-launcher-exclusions.py ;;
    check-flatpak-runtimes) no_arguments "$@"; python3 scripts/generate-flatpak-runtimes.py --check ;;
    generate-flatpak-runtimes) no_arguments "$@"; python3 scripts/generate-flatpak-runtimes.py ;;
    check-flatpak-headers) no_arguments "$@"; python3 engine/scripts/generate-flatpak-vulkan-headers.py --check ;;
    generate-flatpak-headers) no_arguments "$@"; python3 engine/scripts/generate-flatpak-vulkan-headers.py ;;
    sync-arch-package)
      no_arguments "$@"
      python3 engine/dist/arch/sync-release-pin.py engine/renderer-release.json engine/dist/arch/PKGBUILD
      ;;
    check-arch-package) no_arguments "$@"; check_arch_package ;;
    check-color-conversion-spirv) no_arguments "$@"; python3 engine/scripts/generate-color-conversion-spirv.py --check ;;
    install-hooks) no_arguments "$@"; ./scripts/install-git-hooks.sh ;;
    update-contributors) no_arguments "$@"; python3 scripts/update-contributors.py ;;
    check-contributors) no_arguments "$@"; python3 scripts/update-contributors.py --check ;;
    check-links) no_arguments "$@"; python3 scripts/check-markdown-links.py ;;
    *) die "unknown subcommand '$subcommand'; run scripts/dev.sh help" ;;
  esac
}

main "$@"

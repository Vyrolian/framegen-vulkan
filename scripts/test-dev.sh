#!/usr/bin/env bash
# Checks scripts/dev.sh help, dispatch and argument handling without running the builds,
# tests or checks it wraps.
set -euo pipefail

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dev="$repository_root/scripts/dev.sh"
scratch_root="$(mktemp -d "${TMPDIR:-/tmp}/fgvk-dev-test.XXXXXX")"
failures=0

cleanup() {
  rm -rf -- "$scratch_root"
}
trap cleanup EXIT

fail() {
  printf 'test-dev: FAIL: %s\n' "$*" >&2
  failures=$((failures + 1))
}

# Sets last_status and last_output without ending the test on a nonzero exit.
run_capture() {
  last_status=0
  last_output="$("$@" 2>&1)" || last_status=$?
}

if [[ ! -x "$dev" ]]; then
  fail "scripts/dev.sh is not executable"
fi

run_capture "$dev" help
if ((last_status != 0)); then
  fail "help exited $last_status, expected 0"
fi
help_output="$last_output"

run_capture "$dev"
if ((last_status != 0)) || [[ "$last_output" != "$help_output" ]]; then
  fail "running scripts/dev.sh without a subcommand must print the help and exit 0"
fi

# Every dispatched subcommand is listed by help, and help lists nothing that is not dispatched.
listed="$(printf '%s\n' "$help_output" | sed -n 's/^  \([a-z][a-z0-9-]*\)[[:space:]].*/\1/p' | sort || true)"
dispatched="$(grep -E '^    [a-z][a-z0-9-]*\)' "$dev" | sed -E 's/^ +([a-z0-9-]+)\).*/\1/' | sort || true)"
if [[ -z "$dispatched" ]]; then
  fail "no subcommands found in the dispatch of scripts/dev.sh"
elif [[ "$listed" != "$dispatched" ]]; then
  fail "help does not list exactly the subcommands that scripts/dev.sh dispatches"
  diff <(printf '%s\n' "$listed") <(printf '%s\n' "$dispatched") >&2 || true
fi

run_capture "$dev" no-such-subcommand
if ((last_status != 2)); then
  fail "an unknown subcommand exited $last_status, expected 2"
fi

run_capture "$dev" help extra-argument
if ((last_status != 2)); then
  fail "help with an argument exited $last_status, expected 2"
fi

run_capture "$dev" check-release-candidate
if ((last_status != 2)); then
  fail "check-release-candidate without an archive exited $last_status, expected 2"
fi

# An invalid FGVK_JOBS must stop before cmake configures anything.
run_capture env FGVK_JOBS=0 "$dev" build-engine
if ((last_status != 2)) || [[ "$last_output" != *FGVK_JOBS* ]]; then
  fail "FGVK_JOBS=0 must exit 2 with a message naming FGVK_JOBS, got status $last_status"
fi

if ! (cd "$scratch_root" && "$dev" help >/dev/null 2>&1); then
  fail "help must work from a directory outside the repository"
fi

# A relative archive path is resolved against the caller's directory, then the script fails on
# the missing file, and its error names the absolute path.
caller_directory="$scratch_root/caller"
mkdir -p "$caller_directory"
run_capture bash -c "cd '$caller_directory' && '$dev' check-release-candidate ./missing-archive.tar.xz"
if ((last_status == 0)) || [[ "$last_output" != *"$caller_directory/missing-archive.tar.xz"* ]]; then
  fail "a relative archive path must resolve against the caller's directory"
fi

if ((failures > 0)); then
  printf 'test-dev: %d failure(s)\n' "$failures" >&2
  exit 1
fi
printf 'test-dev: scripts/dev.sh help, dispatch and argument checks passed.\n'

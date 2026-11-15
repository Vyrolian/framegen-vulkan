set shell := ["bash", "-eu", "-o", "pipefail", "-c"]

# List the monorepo commands.
default:
    @just --list

# Install the Markdown formatting and commit-hook tools from the lockfile.
install-tools:
    npm ci

# Enable the tracked Markdown pre-commit hook for this checkout.
install-hooks:
    ./scripts/install-git-hooks.sh

# Format all Markdown without artificial prose wrapping.
format-markdown:
    npm run format:markdown

# Check repository Markdown formatting without changing files.
check-markdown-format:
    npm run format:markdown:check

# Update the README gallery from this branch's commit and co-author history.
update-contributors:
    python3 scripts/update-contributors.py

# Check that the README gallery matches this branch's contributor history.
check-contributors:
    python3 scripts/update-contributors.py --check

# Generate the Renderer launcher list from the documented shared registry.
generate-launcher-exclusions:
    python3 scripts/generate-launcher-exclusions.py

# Check launcher exclusion bindings without changing files.
check-launcher-exclusions:
    python3 scripts/generate-launcher-exclusions.py --check

# Generate Renderer Flatpak manifests from the shared runtime catalogue.
generate-flatpak-runtimes:
    python3 scripts/generate-flatpak-runtimes.py

# Check the generated Renderer Flatpak runtime contract without rewriting it.
check-flatpak-runtimes:
    python3 scripts/generate-flatpak-runtimes.py --check

# Generate Flatpak's build-only Vulkan-Headers module from the shared pin.
generate-flatpak-headers:
    python3 engine/scripts/generate-flatpak-vulkan-headers.py

# Reject a stale Flatpak header dependency without rewriting it.
check-flatpak-headers:
    python3 engine/scripts/generate-flatpak-vulkan-headers.py --check

# Sync the Arch package recipe to the checksum-pinned Renderer release.
sync-arch-package:
    python3 engine/dist/arch/sync-release-pin.py engine/renderer-release.json engine/dist/arch/PKGBUILD

# Reject a stale or unsafe Arch package recipe and lifecycle script.
check-arch-package:
    engine/dist/arch/check-release-pin.sh
    engine/dist/arch/test-package-contract.sh

# Build framegen-vulkan.
build: build-engine

# Run the protected-input, release-candidate, settings-launcher and engine tests.
test: test-protected-inputs test-release-candidate test-fgvk-ui test-engine

# Reject licensed inputs and disguised binary/model/archive payloads from Git.
test-protected-inputs:
    ./scripts/test-protected-inputs.sh

# Verify the local release-candidate checker with complete and incomplete ZIP fixtures.
test-release-candidate:
    ./scripts/test-release-candidate.sh

# Configure and build framegen-vulkan in release mode.
build-engine:
    cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Release
    cmake --build engine/build

# Build the fgvk settings window and `fgvk <command>` entry point in release mode.
fgvk-ui:
    cargo build --release --locked --manifest-path engine/fgvk-ui/Cargo.toml

# Run the fgvk crate tests: argument routing, config, launcher state and the process picker.
test-fgvk-ui:
    cargo test --locked --manifest-path engine/fgvk-ui/Cargo.toml

# Build and run the framegen-vulkan tests.
test-engine: build-engine
    ctest --test-dir engine/build --output-on-failure

# Run the portable Renderer policy tests with Vulkan headers but without a Vulkan device.
test-engine-portable:
    cd engine && ./scripts/test-adaptive-scheduler.sh

# Run the portable Renderer policy tests under ASan and UBSan.
test-engine-sanitized:
    cd engine && FGVK_ENABLE_SANITIZERS=ON ./scripts/test-adaptive-scheduler.sh

# Build the native framegen-vulkan release archive.
package-engine:
    ./engine/scripts/package-local.sh

# Build the framegen-vulkan Flatpak extension archive.
package-flatpaks:
    ./engine/scripts/package-flatpaks.sh

# Build the complete release-shaped Renderer artifact set without publishing.
package-renderer-local-release:
    ./engine/scripts/package-local-release.sh

# Check an already-built complete local Renderer host archive against the pushed source commit.
check-release-candidate archive:
    ./scripts/check-release-candidate.sh {{archive}}

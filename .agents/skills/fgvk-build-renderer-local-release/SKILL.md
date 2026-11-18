---
name: fgvk-build-renderer-local-release
description: "Build and verify the complete release-shaped framegen-vulkan artifact set locally: host archive, Flatpak bundle, Arch package, and checksums. Use to simulate Renderer packaging without publishing."
---

# Build a local framegen-vulkan release set

Use the canonical aggregate command rather than assembling release formats independently.

Execution boundary: the host runs the aggregate script, the native Renderer and Flatpak builds, and the checksums. It needs Linux with the native build tools, `cargo`, `flatpak` and `flatpak-builder`, and `makepkg` with `fakeroot`; a missing tool stops the build with an install hint. Artifacts are written under `engine/out/`, and the Flatpak cache lives under `engine/build/cache/`.

## Workflow

1. Locate the framegen-vulkan repository root and confirm it contains `scripts/dev.sh` and `engine/scripts/package-local-release.sh`. Read `engine/docs/BUILDING-FROM-SOURCE.md` and the local `AGENTS.md` when available.
2. Inspect the branch and worktree and preserve all user changes. Tell the user the native build is starting and may take several minutes.
3. Run the build on the actual host (`exec_command` with `sandbox_permissions: "require_escalated"` in Codex); the default isolated shell may not access the host build tools or the network. From the repository root run:

   ```bash
   scripts/dev.sh package-renderer-local-release
   ```

   The subcommand invokes `./engine/scripts/package-local-release.sh`, the canonical aggregate script, which owns the host archive build, every declared Flatpak runtime, the disposable locally synchronized Arch recipe, and the checksum manifest.
4. Do not set `FGVK_RELEASE_SKIP_TESTS` unless the user explicitly requests the documented maintainer exception. Request execution approval when network access requires it.
5. On success, run `sha256sum --check engine/out/SHA256SUMS`. Inspect the Arch package's `.PKGINFO` with `bsdtar`, confirm the expected dual-bitness host contents and declared Flatpak bundles, and report sizes plus clickable absolute links for all artifacts and `SHA256SUMS`.

If the canonical command fails, preserve completed output and diagnose the owning script. Do not silently manufacture an ad hoc package that bypasses its checks. Repair the canonical workflow only when the request includes getting the build operational, then rerun or resume through that workflow and disclose the repair.

This is release-shaped local packaging, not a release. Do not commit, push, change tracked release pins, tag, upload, or publish.

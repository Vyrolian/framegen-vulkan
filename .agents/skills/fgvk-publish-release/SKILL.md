---
name: fgvk-publish-release
description: Publish and verify a public framegen-vulkan release. Use for authorized publication; use fgvk-prepare-release for notes, artwork, and flavor-text preparation only.
---

# Publish framegen-vulkan

Locate the framegen-vulkan checkout and read its current `AGENTS.md` and [DEVELOPING.md](../../../DEVELOPING.md#release). The guide and owning scripts are authoritative; follow their current instructions rather than a copied procedure or a commit SHA stored in this skill.

Execution boundary: the host runs Git, release orchestration, the native Renderer and Flatpak builds, uploads, and published-asset checks. The host needs `cargo`, `flatpak-builder`, `makepkg`, and `fakeroot` in addition to the native build tools; a missing tool stops the build with an install hint.

Run host-dependent preflight, native release builds, published-asset installation checks, and publication commands in the actual host context (`exec_command` with `sandbox_permissions: "require_escalated"` in Codex). The default isolated shell cannot establish the host's toolchain, running-game state, or installed-package state and may not access the network. Do not treat a sandbox-only failure as evidence that SteamOS needs packages installed.

For each new release, choose one new `X.Y.Z` that advances framegen-vulkan. Reuse a version only when resuming that exact incomplete release. Prepare and commit `engine/RELEASE_NOTES.md` with the release changes. Push the candidate `main`, confirm the worktree is clean and `HEAD` matches `origin/main`, and record that commit's SHA. Follow the guide's complete local candidate archive check and applicable game matrix. Run fgvk Gym only when the changed boundary calls for targeted hardware evidence, and record what was not tested.

When the user explicitly authorizes publication, run the guide's top-level `./scripts/publish-release.sh X.Y.Z`. Let it publish the versioned Renderer host archive, Flatpak bundles, and verified Arch package, and record the immutable checksums and source commit in `engine/renderer-release.json`. Use component commands only for a documented interrupted-release resume. Keep the pinned Vulkan-Headers check, release tests, and package verification active unless the guide's explicit maintainer exception applies. Never move a published tag, replace an asset, or manually edit script-owned pins and links.

Complete the guide's public-asset installation check. Verify Renderer release assets, pinned hashes, and README links before reporting the release complete. Report the published version, source and release commits, tags, artifact identities, validation evidence, and any unfinished checks. For a request limited to preparing release notes, artwork, and flavor text, use [Prepare Release](../fgvk-prepare-release/SKILL.md) instead. Its local materials can feed this workflow once publication is authorized.

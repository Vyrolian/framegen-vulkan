# MAKO repository guide for coding agents

This is the only repository-wide instruction source for coding agents. Component READMEs, guides, and scripts add detail without overriding it. Do not create nested `AGENTS.md` copies unless a genuinely scoped exception is required. When a workflow changes, update its owning script or guide first, then keep this index aligned.

## Engineering principles

Before changing code, read the applicable repository, component, and boundary guides to find the canonical owner, generated outputs, compatibility requirements, and validation path. MAKO runs on resource-constrained devices, so protect CPU/GPU cost, memory, synchronization, startup, and hot paths. Extend existing owners and abstractions instead of creating parallel implementations. Keep each change small and align its code, tests, schemas or generators, evidence, and documentation.

Keep Markdown prose unwrapped: each paragraph and list item stays on one source line. Preserve line breaks only when they carry Markdown structure, such as blank paragraph boundaries, headings, tables, blockquotes or admonitions, and fenced or indented code.

## Product names and compatibility identifiers

Use these names in user-facing prose, UI text, workflow labels, package output, and new logs:

- **MAKO** for the overall project;
- **MAKO Renderer** for the Vulkan layer, backend, and CLI;
- **MAKO Gym** for the private real-hardware QA companion; and
- **MAKO Traces** for the private comparative-evidence archive.

The repository and checkout slugs `MAKO-Gym` and `MAKO-Traces` are stable technical identifiers. Preserve them in paths, URLs, schema identifiers, commands, and other literal references while using MAKO Gym and MAKO Traces in prose.

Renderer log records use the stable `MAKO Renderer:` prefix. Do not introduce `mako:`, `mako-render:`, or `Mako Renderer:` as new public prefixes.

These stable compatibility identifiers are not branding errors:

- commands and files such as `mako-launch`, `mako-cli`, `mako-diagnostics`, `mako-remote-play`, `libmako-render.so`, and `mako-render/conf.toml`;
- source paths, namespaces, modules, application IDs, package-manager names, and existing environment variables such as `VK_LAYER_MAKO_render`, `ENABLE_MAKO`, and `DISABLE_MAKO`; and
- release tags such as `render-vX.Y.Z`, plus already-published legacy archive names. Packaging scripts own current archive names.

The diagnostics helper deliberately recognizes historical lowercase renderer prefixes so reports from older installations remain readable. Preserve that input compatibility while requiring current source to emit the branded prefix. Do not rename any stable identifier without an explicit migration and backward-compatibility plan.

## Agent working flow

1. Inspect the current branch and worktree before acting, preserve unrelated changes, and read this guide plus the owning component README and boundary-specific document.
2. Trace the existing behavior to its canonical owner, generated outputs, compatibility obligations, and contract tests before editing. Extend that owner rather than creating a parallel path.
3. Make the smallest behavior-appropriate change. Update an owning schema, generator, migration ledger, focused test, and documentation in the same change whenever their contract moves; never patch a generated output independently. Update `MAINTENANCE.md` when adding or changing an upstream dependency, compatibility assumption, version owner, or ongoing validation responsibility. Keep it a dependency checklist; put dated findings and test results in the owning compatibility record or QA evidence. Keep build pins, source reviews, protocol versions, and hardware evidence distinct; update the owning version/compatibility record in the same change.
4. Run the read-only freshness gates and the portable tests required by `TESTING.md`, then add package, sanitizer, Vulkan, hardware, or game-matrix evidence in proportion to the boundary affected.
5. Review the complete diff and final worktree state for duplication, stale documentation, accidental generated artifacts, and unrelated edits. Report what passed, what was not testable, and the exact branch/staged/committed/pushed state without claiming unavailable hardware evidence.

## Project map

| Area | Responsibility | Primary locations |
| --- | --- | --- |
| Repository root | Product overview, cross-component orchestration, shared diagnostics, compatibility cleanup, and releases | `README.md`, `justfile`, `TESTING.md`, `CLEANUPS.md`, `HOW_TO_RELEASE.md`, `scripts/` |
| Repository tooling | Markdown formatting, the pre-commit hook, and the diagnostics helper's tests | `package.json`, `lint-staged.config.mjs`, `.prettierrc.json`, `.githooks/`, `scripts/tests/` |
| MAKO Renderer layer | Vulkan interception, native spatial reconstruction, deterministic Adaptive scheduling, generated-frame planning, swapchain handling, presentation, recovery, Gamescope integration | `engine/mako-render/` |
| Renderer backend | Lossless Scaling resource extraction and private compute pipeline | `engine/mako-backend/` |
| Renderer common code | Configuration, Vulkan wrappers, device features, and image-quality utilities | `engine/mako-common/` |
| Renderer tools | Validation, benchmarking, debugging, and GPU quality regression | `engine/mako-cli/` |
| Renderer distribution | Host manifests, Flatpak definitions, packaging, Remote Play helper, and build scripts | `engine/dist/`, `engine/scripts/` |
| Product website | Public MAKO product story, direct component downloads, social metadata, static GitHub Pages output, and Pages deployment | `website/`, `.github/workflows/pages.yml` |
| Automation | Portable CI, release contracts, and website deployment | `.github/workflows/` |
| Private real-hardware QA | Licensed-model feature, quality, performance, synchronization, recovery, Gamescope/Desktop, Flatpak, and Proton matrices with local evidence | Sibling `MAKO-Gym` checkout; MAKO owns only `engine/scripts/run-mako-gym.sh` and its portable bridge contract |

Start with the root `README.md`, then read `engine/README.md`. The boundary owners are:

| Boundary | Authoritative guide |
| --- | --- |
| Renderer code ownership and presentation modules | `engine/docs/RENDERER-ARCHITECTURE.md` |
| Native installation, atomic file replacement, and rollback | `INSTALLATION-TRANSACTIONS.md` |
| Native Steam Remote Play shared owner | `engine/scripts/mako_remote_play/` |
| Renderer configuration | `engine/docs/CONFIGURATION.md` for profile setup; `engine/docs/CONFIGURATION-REFERENCE.md` for fields, defaults, and environment variables |
| Launcher executable exclusions | `engine/docs/CONFIGURATION-REFERENCE.md`, `engine/mako-common/launcher_exclusions.json` |
| Renderer allocation, ownership, and memory accounting | `engine/docs/MEMORY-MANAGEMENT.md` |
| Live and deferred setting lifetimes | `engine/docs/RUNTIME-TRANSITIONS.md` |
| Spatial scaling | `engine/docs/SCALING.md` |
| Adaptive scheduling and frame plans | `engine/docs/ADAPTIVE-VALIDATION.md` |
| HDR | `engine/docs/HDR-PIPELINE.md` |
| Implicit-layer and Gamescope presentation | `engine/docs/WSI-ISOLATION.md` |
| Optional Vulkan-layer exceptions | `engine/docs/LAYER-CHAINING.md` |

## Directory and module placement

- Keep repository orchestration and release tooling at the root and Renderer work under `engine/`. Do not create root-level utilities, constants, generated files, or tests for component-owned behavior.
- Keep real-hardware scenarios, licensed-DLL orchestration, and run artifacts in the private sibling `MAKO-Gym` repository. This repository owns only the bridge and release-gate integration.
- Renderer interception, scaling, scheduling, presentation, and recovery live in `engine/mako-render/`; shared configuration and Vulkan/quality helpers in `engine/mako-common/`; backend inference and shaders in `engine/mako-backend/`; tools in `engine/mako-cli/`; distribution files in `engine/dist/`; reusable workflows in `engine/scripts/`; and the bundled vkBasalt shader sources in `engine/vkbasalt-shaders/`. Keep focused tests with their owning CMake component.
- Repository-level Python tests belong in `scripts/tests/`, and Markdown formatting configuration stays at the root.
- The public website lives under `website/`: `website/app/` owns page code and styling, `website/public/` owns browser assets, `website/vite.pages.config.ts` owns the Pages build, and `.github/workflows/pages.yml` is the only publishing entry point.
- Renderer tests belong with their CMake component. Independently deployed components must not import each other at runtime; when they cannot share one owner, keep native declarations and add a focused contract test instead of duplicating utility or serialization layers.
- Keep swapchain construction, resource replacement, profile application, presentation, and status assembly together under `engine/mako-render/src/swapchain/`, with one `Swapchain` state owner.

## Authoritative workflow index

| Task | Read first | Executable entry points |
| --- | --- | --- |
| Understand validation coverage | `TESTING.md` | `justfile`, `.github/workflows/tests.yml` |
| Review ongoing dependencies, compatibility responsibilities, and checked versions | `MAINTENANCE.md` | Canonical version owners listed there; `engine/gamescope-compatibility.json`, `just check-gamescope-compatibility` |
| Format Markdown or enable the commit hook | `AGENTS.md` | `just install-tools`, `just format-markdown`, `just check-markdown-format`, `just install-hooks` |
| Review non-skill Markdown for clarity, duplication, and accuracy | `AGENTS.md` | `.agents/skills/mako-refactor-docs/SKILL.md` (`$mako-refactor-docs`) |
| Review and reclaim disposable MAKO storage after approval | `AGENTS.md` | `.agents/skills/mako-cleanup/SKILL.md` (`$mako-cleanup`) |
| Update the README contributor gallery | `CONTRIBUTING.md` | `just update-contributors`, `just check-contributors` |
| Change native installation or rollback | `INSTALLATION-TRANSACTIONS.md` | `engine/scripts/mako-installer`, `engine/scripts/test-mako-installer.sh` |
| Build Renderer from source or restore SteamOS build tools | `engine/docs/BUILDING-FROM-SOURCE.md` | `engine/CMakeLists.txt`, `engine/scripts/install-steamos-build-tools.sh`, `engine/scripts/build-steamos-dev.sh` |
| Run portable Renderer tests | `TESTING.md` | `engine/scripts/test-adaptive-scheduler.sh` |
| Change native spatial scaling | `engine/docs/SCALING.md`, `engine/docs/WSI-ISOLATION.md`, `engine/docs/HDR-PIPELINE.md` | `engine/mako-render/src/spatial_scaler.*`, `engine/mako-render/src/spatial_scaling_policy.hpp`, `engine/scripts/generate-spatial-scaling-spirv.py` |
| Change runtime configuration transitions or setting lifetimes | `engine/docs/RUNTIME-TRANSITIONS.md`, `engine/docs/CONFIGURATION-REFERENCE.md` | `engine/mako-render/src/profile_update.hpp`, `engine/mako-render/src/instance.cpp`, `engine/mako-render/src/swapchain/profile.cpp`, `engine/mako-render/src/swapchain/resources.cpp` |
| Change Adaptive scheduling or generated-frame plans | `engine/docs/ADAPTIVE-VALIDATION.md` | `engine/mako-render/src/adaptive_scheduler.*`, `engine/mako-render/src/generated_frame_plan.hpp`, `engine/mako-render/src/generated_frame_delivery.hpp` |
| Build host or complete local Renderer artifacts | `engine/docs/BUILDING-FROM-SOURCE.md` | `engine/scripts/package-local.sh`, `engine/scripts/package-local-release.sh` |
| Change or verify the Arch binary package | `engine/dist/arch/README.md` | `just sync-arch-package`, `just check-arch-package`, `engine/dist/arch/verify-release-package.sh` |
| Add, retire, or build Flatpak runtime extensions | `engine/docs/BUILDING-FROM-SOURCE.md#flatpak-extension-builds` | `engine/dist/flatpak/mako-render/runtime-versions.json`, `just generate-flatpak-runtimes`, `just check-flatpak-runtimes`, `engine/scripts/package-flatpaks.sh` |
| Test AMD image quality | `engine/docs/IMAGE-QUALITY-REGRESSION.md`, sibling `MAKO-Gym/docs/AMD-QUALITY-REGRESSION.md` | `engine/mako-cli/src/tools/quality.cpp`, `engine/scripts/run-mako-gym.sh --suite quality` |
| Test licensed-backend throughput or deterministic output | Sibling `MAKO-Gym/docs/RENDER-PERFORMANCE.md`, sibling `MAKO-Gym/docs/QUALITY-REPEATABILITY.md` | `engine/scripts/run-mako-gym.sh --suite performance`, `engine/scripts/run-mako-gym.sh --suite repeatability` |
| Test spatial GPU cost, live runtime overhead, or synchronization correctness | `TESTING.md`, sibling `MAKO-Gym/docs/` | `engine/scripts/run-mako-gym.sh --suite spatial-performance`, `engine/scripts/run-mako-gym.sh --suite runtime-overhead`, `engine/scripts/run-mako-gym.sh --suite sync-validation` |
| Test repeated private-resource memory or sustained temperature/performance health | `TESTING.md`, sibling `MAKO-Gym/docs/SUSTAINED-HEALTH.md` | `engine/scripts/run-mako-gym.sh --suite sustained-health` |
| Change HDR colour handling | `engine/docs/HDR-PIPELINE.md` | `engine/mako-render/src/color_pipeline.cpp`, `engine/mako-backend/src/mako.cpp` |
| Change WSI/layer isolation | `engine/docs/WSI-ISOLATION.md` | `engine/mako-render/src/presentation_policy.hpp`, `engine/scripts/mako-launch` |
| Add or change an optional Vulkan layer chain | `engine/docs/LAYER-CHAINING.md`, `engine/docs/WSI-ISOLATION.md` | Owning launcher, manifest, package, and focused loader/runtime validation |
| Run native Vulkan feature, recovery, compositor, Proton translation, or multi-runtime compatibility matrices | `TESTING.md`, `engine/docs/SCALING.md`, `engine/docs/ADAPTIVE-VALIDATION.md`, sibling `MAKO-Gym/docs/` | `engine/scripts/run-mako-gym.sh`; sibling Gym owns all manifests and hardware runners |
| Test native or Steam Runtime execution outside Gamescope | `TESTING.md`, sibling `MAKO-Gym/docs/DIRECT-DESKTOP-END-TO-END.md` | `engine/scripts/run-mako-gym.sh --suite direct-desktop-e2e` |
| Check a complete local release candidate | `TESTING.md`, `HOW_TO_RELEASE.md` | `engine/scripts/package-local-release.sh`, `scripts/check-release-candidate.sh` |
| Validate real SteamOS hardware | `TESTING.md` | `engine/scripts/run-mako-gym.sh`; sibling Gym owns hardware runners |
| Exercise the game/runtime matrix | `engine/docs/ADAPTIVE-VALIDATION.md` | Manual DXVK, VKD3D-Proton, native Vulkan, Gamescope, and supported desktop scenarios |
| Change or exercise comparative game capture | `TRACES.md` | `scripts/capture-trace.sh`, `scripts/test-capture-trace.sh`, sibling private `MAKO-Traces` checkout |
| Build or publish the product website | `website/README.md` | `website/package.json`, `.github/workflows/pages.yml` |
| Add or document a launcher executable exclusion | `engine/docs/CONFIGURATION-REFERENCE.md`, `CLEANUPS.md` | `engine/mako-common/launcher_exclusions.json`, `scripts/generate-launcher-exclusions.py`, `just generate-launcher-exclusions`, `just check-launcher-exclusions` |
| Add or remove transitional compatibility | `CLEANUPS.md` | Owning migration/generator and its focused regression tests |
| Change native Steam Remote Play override | `engine/docs/CONFIGURATION.md#native-steam-remote-play`, `INSTALLATION-TRANSACTIONS.md` | `engine/scripts/mako_remote_play/`, `engine/scripts/mako-remote-play`, and focused Remote Play tests |
| Collect diagnostics | `COLLECT_DIAGNOSTICS.md` | `scripts/mako-diagnostics`, `scripts/tests/test_diagnostics_helper.py` |
| Compare MAKO and NVIDIA Smooth Motion overhead without Nsight | `engine/docs/COLLECT_DIAGNOSTICS.md#compare-mako-and-nvidia-smooth-motion-without-nsight` | `engine/scripts/profile-framegen.py capture`, `engine/scripts/profile-framegen.py compare` |
| Prepare release notes, artwork, and flavor text | `HOW_TO_RELEASE.md#notes-identity-and-artwork` | `.agents/skills/mako-prepare-release/SKILL.md` (`$mako-prepare-release`) |
| Publish the Renderer release | `HOW_TO_RELEASE.md` | `scripts/publish-release.sh` |
| Resume one Renderer publish | `HOW_TO_RELEASE.md` | `engine/scripts/publish-package.sh` |

`just --list` exposes the common non-publishing entry points. Component scripts remain the source of truth for their arguments, prerequisites, cache behavior, and output paths.

## Test selection and evidence

Use `TESTING.md` to choose proportionate evidence. Renderer C++ changes need portable/full CTest; scheduling and presentation work also needs sanitizers and the applicable Adaptive matrix. Choose targeted MAKO Gym evidence for Vulkan, scaling, synchronization, shader, quality, or resource-lifetime boundaries when the change needs hardware qualification, including FP32 and FP16 for AMD paths; report untested hardware rows explicitly. Repository tooling changes need `just test-diagnostics-helper` and `just check-markdown-format`, website changes need both documented builds, and package-boundary changes need package verification.

Before publication, check the complete local Renderer archives built from the clean pushed candidate with `scripts/check-release-candidate.sh`; reuse those archives rather than rebuilding them in CI. MAKO Gym suites are targeted, optional hardware evidence chosen for the changed boundary, and untested rows must be reported. Publication rebuilds after release-owned metadata commits, so the published-asset install check is separate. Run every Gym suite only for a genuinely cross-cutting change or explicit maintainer request. Native AArch64 support remains unavailable until it has source-built packages and real-hardware evidence, not an opaque binary, second detector, or global FEX mutation.

When claiming complete Renderer hardware qualification, include the bounded MAKO Gym resource-constraint suite against the relevant packaged CLI and launcher. Its case/profile mapping remains owned by Gym.

A skipped GPU test is not evidence, and the automated AMD scene does not replace the game/runtime matrix. State which hardware, driver, architecture, Flatpak runtime, and rows were not tested.

Read `TRACES.md` before archiving a completed session. MAKO owns trace extraction, sanitization, metadata, initial checksums, and producer tests; MAKO Traces owns stored evidence and archive validation. MAKO Gym owns hardware manifests, orchestration, assertions, and local evidence; MAKO owns only its bridge contract and release integration. Update both repositories when either cross-repository contract changes. Never copy private inventories, licensed inputs, or generated evidence into MAKO, and never make portable workflows depend on them.

## Diagnostics map

- `COLLECT_DIAGNOSTICS.md` routes users to the standalone Renderer collection guide.
- `engine/docs/COLLECT_DIAGNOSTICS.md` owns Renderer collection.
- `scripts/mako-diagnostics` filters current and legacy Renderer logs into a focused report; its deterministic coverage is in `scripts/tests/test_diagnostics_helper.py`. `engine/scripts/analyze-pacing-profile.py` summarizes versioned CPU pacing captures; its tests live with the Renderer.
- `engine/mako-render/src/present_diagnostics.cpp` and `.hpp` own structured presentation records. Related state is emitted from `instance.cpp` and the owning files under `swapchain/`; `swapchain/status.cpp` assembles live status without applying transitions.
- `engine/scripts/wayland_pacing_protocol.py` owns optional source-checkout capture of selected Wayland protocol messages and exact feedback-to-commit correlation for `analyze-pacing-profile.py`; it does not change the Renderer or saved launch configuration.
- `engine/mako-render/src/frame_content_capture.*` owns optional explicit-trigger real/generated picture readback through existing copy commands and completion fences; `engine/scripts/analyze-frame-content.py` decodes and compares its separate private image files. Timing-only collectors do not automatically include those pictures.
- `engine/mako-render/src/wayland_presentation_feedback.*` owns optional `MAKO_PACING_PROFILE_WAYLAND=1` compositor presentation/discard observation for the game's borrowed native Wayland surface. Keep its private event queue, bounded proxies/records, surface retirement and timestamp domains separate from pacing policy; XCB probes do not qualify Wine Wayland.

Keep diagnostic operation names and fields machine-filterable. If a current log format changes, update the collector, its tests, and both user guides together.

## Generated files, artifacts, and protected inputs

- Treat `engine/{build,out}/`, `node_modules/`, `website/{dist,dist-pages,.vinext,.next,node_modules}/`, package-manager stores, `__pycache__/`, and sibling `MAKO-Gym/out/` as generated local data. Do not hand-edit or commit them.
- Renderer SPIR-V headers and hash manifests are generated from adjacent GLSL by `engine/scripts/generate-color-conversion-spirv.py` and `engine/scripts/generate-spatial-scaling-spirv.py`. Portable CTest runs their read-only `--check` modes; regeneration requires `glslangValidator`. Never edit embedded arrays or hashes independently.
- `engine/mako-common/launcher_exclusions.json` owns the documented launcher executable exclusions for Renderer activation. `scripts/generate-launcher-exclusions.py` generates the component-local C++ list; use `just generate-launcher-exclusions` after source edits and `just check-launcher-exclusions` for read-only validation. Never edit the generated lists independently or add a runtime dependency between the components.
- `engine/scripts/mako_remote_play/` owns Remote Play file safety, the self-contained launch template, and managed-file primitives. The native command uses its installed private package.
- `engine/vulkan-headers-revision.txt` also owns the generated `engine/dist/flatpak/mako-render/vulkan-headers.json` build-only dependency. Use `just generate-flatpak-headers` after changing the pin and `just check-flatpak-headers` for read-only validation; never edit that module independently.
- `engine/gamescope-compatibility.json` owns Gamescope protocol provenance, supported wire versions, scoped upstream source review, and recorded hardware validation summaries. Keep the adapter, protocol fixtures, third-party notice, and WSI guide aligned; `just check-gamescope-compatibility` checks local version/provenance agreement without network access. Follow `engine/docs/WSI-ISOLATION.md#maintaining-the-compatibility-record` when reviewing upstream or entering evidence. A source review or protocol fixture never establishes a tested Gamescope release.
- `engine/vkbasalt-release.json` owns the immutable vkBasalt archive, Vulkan-Headers provenance, and reviewed API declaration. `engine/scripts/manage-vkbasalt-release.py --update-from-archive` derives the pin from a verified public archive; `just generate-vkbasalt-release` generates its Flatpak dependency and both architecture manifests. The corresponding read-only gate rejects stale outputs, a header revision different from the Renderer pin, or an API declaration different from the Renderer manifests.
- `engine/dist/flatpak/mako-render/runtime-versions.json` owns the ordered Flatpak runtime catalogue and LLVM SDK versions; the adjacent `org.freedesktop.Platform.VulkanLayer.makorender.yml.in` owns the common manifest. `scripts/generate-flatpak-runtimes.py` generates Renderer manifests and `runtime-versions.txt`. Use `just generate-flatpak-runtimes` to regenerate these, then `just check-flatpak-runtimes` for read-only validation; never edit outputs independently. Renderer packaging and Renderer CTest reject stale outputs.
- Published Renderer URLs, checksums, and pins in `engine/renderer-release.json` are maintained by the release/pinning scripts. `engine/dist/arch/sync-release-pin.py` derives the Arch recipe's version and checksum from that verified pin; use `just check-arch-package` instead of editing those fields independently. Local packaging writes local identity into the archive without changing the tracked release pin.
- `Lossless.dll` is a user-supplied licensed input. Never add it to the repository, package it, upload it, or treat its presence as portable CI data.
- MAKO Renderer descends from the GPL-3.0-or-later lsfg-vk version 2 history identified in `LICENSE.md`. The replacement lsfg-vk repository announced on 27 August 2026 is licensed under CC BY-NC-ND 4.0 and is not a source for MAKO; never copy its post-reset code, documentation, or assets into this repository.

## Compatibility cleanup contract

`CLEANUPS.md` tracks temporary migrations, legacy readers, deny-lists, and their removal gates. Update it when adding or removing a compatibility path. Generated wrappers are replaceable cache: keep one current format and regenerate old ones from canonical state. Preserve small idempotent migrations for values stored only in retired locations; skipped-version upgrades mean age alone never makes them safe to remove.

## Portable packaging contract

Published host archives are built with the portable Ubuntu 22.04 builder: a fixed Clang and libc baseline plus the pinned Vulkan headers, so the compiled presentation contract does not depend on the host SDK. The container image's toolchain pins are a packaging requirement and must keep those properties.

Native publication always selects that portable builder, matching the complete local release-candidate build. [`engine/vulkan-headers-revision.txt`](engine/vulkan-headers-revision.txt) owns the shared native/Flatpak Vulkan-Headers ref and minimum version; the Renderer packager, fast SteamOS development builder, main Renderer and sanitizer CI builds, CMake, and generated Flatpak module consume it. Native and Flatpak packaging also require `VK_KHR_present_id2` and `VK_EXT_present_timing` for both architectures, independently of test selection. The fast SteamOS builder enforces that same header check while retaining the host compiler and incremental build tree; manual CMake builds may still use host headers. Flatpak builds retain each runtime SDK compiler and libraries. Layer manifest API versions remain separate from the build-header pin. Follow [tester/release build alignment](HOW_TO_RELEASE.md#keep-tester-and-release-builds-aligned): retain artifact hashes and resolved toolchain identities, invalidate stale native package caches when the builder changes, and verify the public asset rather than assuming identical source means identical compiled support.

## Packaging, deployment, and release boundaries

Local packaging does not publish. Engine replacement requires games using the layer to be closed. Run installation actions only when the task calls for changing the installed environment.

Publishing is a separate, explicitly authorized workflow. Follow `HOW_TO_RELEASE.md` from a clean `main` checkout, update `engine/RELEASE_NOTES.md` as the only manual release copy, check the complete local candidate archives, and let the scripts manage versions, pins, checksums, tags, assets, and README release links. Choose MAKO Gym hardware checks as needed for the changed boundary; no GitHub hardware runner is part of the release workflow. Never move a published tag or replace a published asset.

## Source-control boundaries

Inspect `git status`, preserve unrelated changes, and stay on the selected branch. Inspection, implementation, tests, builds, and packages do not authorize switching branches, pulling, rebasing, staging, committing, pushing, tagging, publishing, or deploying. Perform those mutations only when explicitly requested, then report the final branch and staged, committed, and pushed state.

# Agent guide

Personal fork of a Vulkan frame-generation layer (lsfg-vk v2 descendant), now named framegen-vulkan / `fgvk`. Current work and specs: `engine/docs/DEVELOPMENT-HANDOFF.md`. Read the owning doc below before touching a boundary; keep changes small and aligned (code, tests, generators, docs).

## Layout

| Path | Owns |
| --- | --- |
| `engine/fgvk-render/` | Vulkan layer: interception, scaling, Adaptive scheduling, swapchain/presentation (`src/swapchain/`, one `Swapchain` owner), recovery |
| `engine/fgvk-backend/` | Lossless Scaling resource extraction and compute pipeline |
| `engine/fgvk-common/` | Config, Vulkan wrappers, shared helpers |
| `engine/fgvk-cli/` | Validation, benchmark and quality tools |
| `engine/fgvk-ui/` | egui settings launcher `fgvk` (Rust) |
| `engine/dist/`, `engine/scripts/` | Packaging, Flatpak, Arch, installer, launch wrapper, build scripts |
| `scripts/` | Diagnostics helper, release and trace scripts (tests in `scripts/tests/`) |
| `website/` | Static product site |

Boundary docs (`engine/docs/`): `RENDERER-ARCHITECTURE`, `CONFIGURATION` / `CONFIGURATION-REFERENCE`, `RUNTIME-TRANSITIONS`, `MEMORY-MANAGEMENT`, `SCALING`, `ADAPTIVE-VALIDATION`, `HDR-PIPELINE`, `WSI-ISOLATION`, `LAYER-CHAINING`, `BUILDING-FROM-SOURCE`. Root: `TESTING.md`, `MAINTENANCE.md` (dependencies and version owners), `CLEANUPS.md` (compatibility paths), `HOW_TO_RELEASE.md`, `INSTALLATION-TRANSACTIONS.md`.

## Build and test

- `just --list` shows entry points. C++ changes: `just test-engine-portable` (CTest); scheduling/presentation also `just test-engine-sanitized`. Launcher: `cargo test --locked` in `engine/fgvk-ui`. Tooling: `just test-diagnostics-helper`.
- The device is resource-constrained: watch CPU/GPU cost, memory, sync and hot paths. Keep at most two build jobs while the user may be gaming.
- Report what passed and what was not tested (hardware, AMD/FP16, Flatpak, Gamescope). A skipped GPU test is not evidence.

## Generated files and protected inputs

- Never hand-edit generated output; change the source and run its generator, then the `just check-*` gate: SPIR-V headers (`engine/scripts/generate-*-spirv.py`), launcher exclusions (`just generate-launcher-exclusions`), Flatpak runtimes/headers (`just generate-flatpak-runtimes`, `just generate-flatpak-headers`), vkBasalt release (`just generate-vkbasalt-release`), Arch pin (`just sync-arch-package`).
- Build dirs (`engine/build/`, `engine/out/`, `target/`, `node_modules/`, `website/dist*`) are local data; never commit them.
- `Lossless.dll` is a licensed user input: never commit, package or upload it.
- The lsfg-vk repository relaunched on 27 August 2026 is CC BY-NC-ND 4.0: never copy its code, docs or assets here. Keep copyright and licence lines intact.

## Style and git

- Markdown prose unwrapped: one line per paragraph or list item.
- Log records use the `framegen-vulkan:` prefix.
- Do not switch branches, pull, rebase, push, tag, publish or install without an explicit request. Local commits on the current work branch are allowed when the user approves the change. Report final branch and commit state.

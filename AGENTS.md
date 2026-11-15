# Agent guide

Personal fork of a Vulkan frame-generation layer (lsfg-vk v2 descendant), now named framegen-vulkan / `fgvk`. Current work and specs: `engine/docs/DEVELOPMENT-HANDOFF.md`. Read the owning doc below before touching a boundary; keep changes small and aligned (code, tests, generators, docs).

## Layout

| Path | Owns |
| --- | --- |
| `engine/fgvk-render/` | Vulkan layer: interception, Fixed-multiplier scheduling, swapchain/presentation (`src/swapchain/`, one `Swapchain` owner), recovery |
| `engine/fgvk-backend/` | Lossless Scaling resource extraction and compute pipeline |
| `engine/fgvk-common/` | Config, Vulkan wrappers, shared helpers |
| `engine/fgvk-cli/` | Validation and DLL inspection tools |
| `engine/fgvk-ui/` | Settings window and `fgvk <command>` entry point (Rust, egui); one crate, one binary |
| `engine/dist/`, `engine/scripts/` | Packaging, Flatpak, Arch, installer, launch wrapper, build scripts |
| `scripts/` | Release scripts and `scanout-watch.sh` |

Placement rules for `engine/fgvk-ui/`: the crate owns only the settings window and the argument routing in `src/launch.rs`. It must not load the renderer or Vulkan, and launch behaviour stays in `engine/scripts/fgvk-launch` (installed privately under `libexec/framegen-vulkan/`, never on `PATH`). Keep it self-contained: `processes.rs` embeds `../fgvk-common/launcher_exclusions.json`, so the crate builds only inside this repository.

Boundary docs (`engine/docs/`): `RENDERER-ARCHITECTURE`, `CONFIGURATION` / `CONFIGURATION-REFERENCE`, `RUNTIME-TRANSITIONS`, `MEMORY-MANAGEMENT`, `WSI-ISOLATION`, `LAYER-CHAINING`, `BUILDING-FROM-SOURCE`. Root: `DEVELOPING.md` (build and test, dependencies and version owners, release), `CLEANUPS.md` (compatibility paths), `INSTALLATION-TRANSACTIONS.md`.

## Build and test

- `scripts/dev.sh help` lists the entry points. C++ changes: `scripts/dev.sh test-engine-portable` (CTest); scheduling/presentation also `scripts/dev.sh test-engine-sanitized`. Settings window and entry point: `scripts/dev.sh test-fgvk-ui` (`cargo test --locked` in `engine/fgvk-ui`).
- The device is resource-constrained: watch CPU/GPU cost, memory, sync and hot paths. Keep at most two build jobs while the user may be gaming (`FGVK_JOBS=2 scripts/dev.sh <subcommand>`; builds use all cores otherwise).
- Markdown links and `#anchors` in tracked files: `scripts/dev.sh check-links` (exit 1 lists each broken link).
- Report what passed and what was not tested (hardware, AMD/FP16, Flatpak). A skipped GPU test is not evidence.

## Generated files and protected inputs

- Never hand-edit generated output; change the source and run its generator, then the `scripts/dev.sh check-*` gate: SPIR-V headers (`engine/scripts/generate-*-spirv.py`), launcher exclusions (`scripts/dev.sh generate-launcher-exclusions`), Flatpak runtimes/headers (`scripts/dev.sh generate-flatpak-runtimes`, `scripts/dev.sh generate-flatpak-headers`), Arch pin (`scripts/dev.sh sync-arch-package`).
- Build dirs (`engine/build/`, `engine/out/`, `target/`) are local data; never commit them.
- `Lossless.dll` is a licensed user input: never commit, package or upload it.
- The lsfg-vk repository relaunched on 27 August 2026 is CC BY-NC-ND 4.0: never copy its code, docs or assets here. Keep copyright and licence lines intact.

## Style and git

- Markdown prose unwrapped: one line per paragraph or list item.
- Log records use the `framegen-vulkan:` prefix.
- Do not switch branches, pull, rebase, push, tag, publish or install without an explicit request. Local commits on the current work branch are allowed when the user approves the change. Report final branch and commit state.

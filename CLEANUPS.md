# Compatibility cleanup ledger

This document owns MAKO's transitional compatibility and cleanup policy. It is not a general backlog. Add an entry when a change temporarily accepts, imports, rewrites, or rejects an older persisted representation and remove the entry when the compatibility code is removed.

GPU Headroom adds optional `gpu_headroom_percent` (default `0`) to version-2 profiles and schema-5 runtime profile snapshots, and `gpu_headroom_state` to schema-5 context records. Readers must accept older records with reserve `0` and state `unavailable`, and must not infer support from a saved toggle. Keep these absent-field defaults until supported older Renderer installations can no longer produce schema-5 records. Configuration round-trip and runtime-reader tests cover the compatibility boundary. No persisted-value migration is needed.

The persisted `allow_fp16` boolean remains the canonical global precision choice; upgrades preserve both true and false values. The corrected runtime applies false even under Ultra Performance and across LSFG and MAKO Scaler, and reports unavailable true requests rather than silently selecting FP32. LS1 always uses FP32 independently of this setting. No stored-value rewrite is needed or performed.

Optional AC/battery profile tables extend Renderer configuration version 2 without rewriting existing profiles. Profiles without these tables retain their previous behavior; opting in clones the existing native settings and opting out restores the retained base. No legacy-value migration is required. Older editors may discard these optional tables, so the configuration guides require matching current components.

The optional `adaptive_target_refresh_rate` boolean defaults to false for existing configuration and for older schema-5 runtime records. No saved-target migration is required: display feedback affects only the applied target, while `target_fps` remains the requested manual fallback. Native configuration and runtime-status regression tests preserve these defaults and distinguish requested and applied targets.

The shader editor adds optional `mako-shader-tool` payload discovery while published older Renderer archives remain installable. Older archives cannot open the new editor; installing one removes a stale private helper, with rollback preserving it on failure. Remove this optional-package branch only when all supported install/rollback targets include the matching helper and effect-local parameter runtime. Legacy bare-name shader overrides are persisted user data and remain readable until a tested migration preserves their values for every effect.

## Policy

The unreleased HDR precision toggle has been replaced by `hdr_quality` without a saved-value migration or legacy-field reader. Older runtime records without this field decode as `quality`, matching their full-precision behavior; retain this missing-field default until all supported Renderers report HDR quality. Older shader binaries without the optional precision hook retain full precision.

Generated files and user data have different lifecycles:

- Launch wrappers are disposable cache. Regenerate any stale, incomplete, or contaminated wrapper from canonical data instead of adding format-specific transforms.
- Persisted values that exist nowhere else are user data. Keep an idempotent migration until MAKO ends the direct-upgrade path from the affected release.
- Deny-lists and fail-closed host guards remain current safety rules while stale artifacts could reactivate unsupported behavior.

The optional `disable_hdr_exposure` boolean in standalone `launcher.conf` version 1 defaults to true.

Standalone `launcher.conf` version 1 gains the optional `disable_steam_overlay` boolean, with true as the absent-field default. Use a matching launcher; older strict readers cannot consume the new field.

MAKO supports direct upgrades from MAKO 2.0.0, with no maximum skipped-version gap, but does not import state from differently named experimental predecessors or pre-public file formats. A data migration can be removed only after MAKO declares a newer minimum direct-upgrade baseline than its source release or provides an equivalent state-preserving replacement.

When adding transitional compatibility, update this ledger in the same change with its owner, reason, removal gate, and regression test. Prefer one generic contract test over one test per historical version when the implementation does not branch on version.

## Current obligations

| Compatibility path | Owner | Why it remains and removal gate | Regression evidence |
| --- | --- | --- | --- |
| Frame-profile schema-1 evidence | Sibling MAKO Gym `scripts/renderer_profile.py` | The Renderer now emits schema 2 with encoding and GPU substages. Retained schema-1 profiles remain readable as SDR aggregate evidence only; they cannot satisfy HDR or substage claims. Removal gate: historical profile archives no longer require this reader, or an explicit lossless archive reader replaces it. | Gym profile tests reject schema-1 HDR use, mismatched encoding and inconsistent schema-2 substage sums. |
| Native Remote Play experimental override guard | `engine/scripts/mako_remote_play/remote_play_core.py` | Older proof-of-concept wrappers/backups and the experimental Vulkan meta-layer are not managed state and must be removed with their original tools before installation. Do not infer their integrity or replace them automatically. Removal gate: experimental installations can no longer encounter the managed installer, or an explicit verified importer replaces this guard. | Native Remote Play tests refuse preexisting backups, sidecars, symlinks, and the experimental manifest while preserving the Steam entry point. |
| Private bridge presentation-wait fallback removal qualification | `engine/mako-render/src/presentation_policy.hpp` and application-facing feature/device entrypoints | Feature masking is removed and replaced by bounded compositor-feedback readiness shared by SDR scaling and HDR bridge output. Full qualification still requires correct real-image completion, throughput and bounded latency in fixed- and variable-refresh game comparisons, pause/resume, and serialized/asynchronous Gym controls. Independent compositor completion must distinguish repeated timing predictions from actual output loss; fixed-refresh success does not close physical-VRR or input-latency qualification. | Policy tests cover completion-paced source feedback, GPU-cost changes, queued spacing, absent/stale feedback, ID wrap and FG-off; hardware and game qualification must establish the remaining gate. |
| Relocated Lossless Scaling model tables | `engine/mako-backend/src/extraction/model_resources.cpp` | Compatible DLL updates can move resource IDs while retaining the supported complete LS1, Vulkan LSFG, or DirectX LSFG table. Keep the bounded, unique-table fallback until those model layouts are retired or replaced by an equally validated discovery contract. | Synthetic DLL inspection tests cover positive/negative relocation, canonical precedence, selected-model isolation, ambiguous/partial tables, reflection and precision mismatches, one-table LSFG registry selection, and exact reflected DirectX stages before the explicit FP32 translation fallback; LSFG FP16 requests cannot silently use FP32; the separately selected experimental build may convert a supported graph to explicit half arithmetic, with unqualified image quality. |
| Pre-`VK_EXT_present_timing` build headers | `engine/mako-render/src/pnext_chain.hpp` | Supported build SDKs can predate an application/driver's `VkPresentTimingsInfoEXT`; filtering a later present-mode or damage-region node must still preserve the complete timing prefix. The local layout copies only the outer node and keeps its timing payload opaque. Removal gate: Every supported host and Flatpak build SDK exposes `VK_EXT_present_timing`; replace the local layout and numeric structure identifier with the SDK declarations while retaining runtime support. | `pnext-chain` regressions pass with old and new headers; new SDKs statically verify the structure identifier, size, alignment, and every field offset. |
| Historical diagnostic prefixes and operation IDs | `scripts/mako-diagnostics` | Older reports remain readable after log branding and operation names changed. Remove only after a declared diagnostics support window expires. | `test_diagnostics_helper.py` covers current input plus `resume-generated-frames`, `generated-image-recovered`, and `swapchain-recreation-suppressed`. |
| CPU pacing-profile version 1 captures and version-1 output observations | `engine/scripts/analyze-pacing-profile.py` | Local preview captures contain aggregate source/output timing without version 2's correlated hitch fields, and version-1 individual outputs omit detailed worker readiness/queue-lock timing. Keep both readable without inventing missing per-source, worker or completed-GPU attribution. Remove only after those retained preview captures are no longer needed or an archive reader replaces this path. | `test_pacing_profile_report.py` requires explicit unavailable attribution for version 1 and rejects malformed or unsupported records. |
| Runtime-status filenames without process start ticks | `engine/mako-render/src/runtime_status.cpp` startup pruning | Abnormal exits from older releases can leave `<pid>-<role>-<context>.json`. Remove after the upgrade baseline cannot retain one. | Runtime-status tests remove only stale current and legacy records while preserving active locks and unrelated or unsafe entries. |
| Split-layer mode `1` | `engine/mako-render/src/layer_role.hpp` | Old launchers may export `MAKO_SPLIT_LAYER_CHAIN=1`; mode `2` keeps the same loader order but changes reconstruction ownership. Remove after every supported launcher regenerates before launch and no standalone integration exports mode `1`. | Layer-role tests cover both modes, exact mode `2` order, placement, and extent; MAKO Gym covers the real Gamescope WSI path. |

Native Remote Play uses ordinary saved profiles without dedicated-profile creation or override-version migration. The optional root `current_profile` TOML key persists the editor selection for streams.

## Completed cleanups

| Cleanup | Result | Completed |
| --- | --- | --- |
| Separate plugin integration and Qt configuration UI removal | Removed the separate plugin integration, its generators and bindings, the Qt configuration application, and the plugin-specific Remote Play adapters. The Renderer reads only `~/.config/mako-render/conf.toml`; the earlier integration's selection comment and its `mako-run` Remote Play launcher are no longer read. Older installations need a one-time migration. | 2026-11-07 |
| Numbered Flatpak filename imports | Removed unused internal aliases for individual branches; all bundle filenames and extension identities use the generated runtime catalogue. Retiring a branch no longer breaks generated constants, and published filenames remain unchanged. | 2026-10-08 |
| Pre-Steady-default Adaptive profile override | Removed the missing-field exception so every profile without an explicit choice adopts the current Steady Base Cap default. Profiles that explicitly saved Fractional remain unchanged. | 2026-08-21 |

## Removal checklist

Before deleting a current obligation:

1. Confirm whether the old representation contains unique user data or is only generated output.
2. Identify the published release that first wrote the canonical replacement and compare it with the declared minimum direct-upgrade baseline.
3. Verify that install, startup, rollback, and skipped-version upgrades cannot encounter the removed path within the supported contract.
4. Remove the compatibility code, its ledger entry, and only the tests that no longer express a current invariant.
5. Run the boundary-specific suite in `TESTING.md`; package/install changes also require local archive verification.
6. Mention user-visible compatibility removals in the component release notes.

Diagnostic input compatibility, stable package identifiers, and published archive names have their own contracts in `AGENTS.md`; do not treat them as generated-wrapper cleanup.

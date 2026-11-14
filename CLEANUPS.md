# Compatibility cleanup ledger

This document owns framegen-vulkan's transitional compatibility and cleanup policy. It is not a general backlog. Add an entry when a change temporarily accepts, imports, rewrites, or rejects an older persisted representation and remove the entry when the compatibility code is removed.

The persisted `allow_fp16` boolean remains the canonical global precision choice; upgrades preserve both true and false values. The corrected runtime applies false even under Ultra Performance and across LSFG and framegen-vulkan Scaler, and reports unavailable true requests rather than silently selecting FP32. No stored-value rewrite is needed or performed.

AC/battery profile tables (`[profile.handheld]` and `[profile.docked]`) are retired, accepted and ignored.

GPU Headroom is retired: old files accept and ignore `gpu_headroom_percent`, `FGVK_GPU_HEADROOM_PERCENT` is no longer read, and framegen-vulkan no longer writes the key or the schema-5 `gpu_headroom_state` and profile `gpu_headroom_percent` fields; `schema_version` stays 5 because nothing in the repository reads the removed fields.

The optional `adaptive_target_refresh_rate` boolean defaults to false for existing configuration and for older schema-5 runtime records. No saved-target migration is required: display feedback affects only the applied target, while `target_fps` remains the requested manual fallback. Native configuration and runtime-status regression tests preserve these defaults and distinguish requested and applied targets.

The shader editor adds optional `fgvk-shader-tool` payload discovery while published older Renderer archives remain installable. Older archives cannot open the new editor; installing one removes a stale private helper, with rollback preserving it on failure. Remove this optional-package branch only when all supported install/rollback targets include the matching helper and effect-local parameter runtime. Legacy bare-name shader overrides are persisted user data and remain readable until a tested migration preserves their values for every effect.

## Policy


Generated files and user data have different lifecycles:

- Launch wrappers are disposable cache. Regenerate any stale, incomplete, or contaminated wrapper from canonical data instead of adding format-specific transforms.
- Persisted values that exist nowhere else are user data. Keep an idempotent migration until framegen-vulkan ends the direct-upgrade path from the affected release.
- Deny-lists and fail-closed host guards remain current safety rules while stale artifacts could reactivate unsupported behavior.

The optional `disable_hdr_exposure` boolean in standalone `launcher.conf` version 1 is still accepted (0 or 1, no duplicates) and ignored, so old files stay valid.

Standalone `launcher.conf` version 1 gains the optional `disable_steam_overlay` boolean, with true as the absent-field default. Use a matching launcher; older strict readers cannot consume the new field.

Standalone `launcher.conf` version 1 gains the optional logging booleans `log_pacing`, `log_trace`, `log_wayland`, `log_gpu_cost` and `log_present`, with 0 as the absent-field default. Use a matching launcher; older strict readers cannot consume the new fields.

framegen-vulkan supports direct upgrades from framegen-vulkan 2.0.0, with no maximum skipped-version gap, but does not import state from differently named experimental predecessors or pre-public file formats. A data migration can be removed only after framegen-vulkan declares a newer minimum direct-upgrade baseline than its source release or provides an equivalent state-preserving replacement.

When adding transitional compatibility, update this ledger in the same change with its owner, reason, removal gate, and regression test. Prefer one generic contract test over one test per historical version when the implementation does not branch on version.

## Current obligations

| Compatibility path | Owner | Why it remains and removal gate | Regression evidence |
| --- | --- | --- | --- |
| Frame-profile schema-1 evidence | Sibling fgvk Gym `scripts/renderer_profile.py` | The Renderer now emits schema 2 with encoding and GPU substages. Retained schema-1 profiles remain readable as SDR aggregate evidence only; they cannot satisfy HDR or substage claims. Removal gate: historical profile archives no longer require this reader, or an explicit lossless archive reader replaces it. | Gym profile tests reject schema-1 HDR use, mismatched encoding and inconsistent schema-2 substage sums. |
| Private bridge presentation-wait fallback removal qualification | `engine/fgvk-render/src/presentation_policy.hpp` and application-facing feature/device entrypoints | Feature masking is removed and replaced by bounded compositor-feedback readiness shared by SDR scaling and HDR bridge output. Full qualification still requires correct real-image completion, throughput and bounded latency in fixed- and variable-refresh game comparisons, pause/resume, and serialized/asynchronous Gym controls. Independent compositor completion must distinguish repeated timing predictions from actual output loss; fixed-refresh success does not close physical-VRR or input-latency qualification. | Policy tests cover completion-paced source feedback, GPU-cost changes, queued spacing, absent/stale feedback, ID wrap and FG-off; hardware and game qualification must establish the remaining gate. |
| Relocated Lossless Scaling model tables | `engine/fgvk-backend/src/extraction/model_resources.cpp` | Compatible DLL updates can move resource IDs while retaining the supported complete Vulkan LSFG or DirectX LSFG table. Keep the bounded, unique-table fallback until those model layouts are retired or replaced by an equally validated discovery contract. | Synthetic DLL inspection tests cover positive/negative relocation, canonical precedence, selected-model isolation, ambiguous/partial tables, reflection and precision mismatches, one-table LSFG registry selection, and exact reflected DirectX stages before the explicit FP32 translation fallback; LSFG FP16 requests cannot silently use FP32; the separately selected experimental build may convert a supported graph to explicit half arithmetic, with unqualified image quality. |
| Pre-`VK_EXT_present_timing` build headers | `engine/fgvk-render/src/pnext_chain.hpp` | Supported build SDKs can predate an application/driver's `VkPresentTimingsInfoEXT`; filtering a later present-mode or damage-region node must still preserve the complete timing prefix. The local layout copies only the outer node and keeps its timing payload opaque. Removal gate: Every supported host and Flatpak build SDK exposes `VK_EXT_present_timing`; replace the local layout and numeric structure identifier with the SDK declarations while retaining runtime support. | `pnext-chain` regressions pass with old and new headers; new SDKs statically verify the structure identifier, size, alignment, and every field offset. |
| Historical diagnostic prefixes and operation IDs | `scripts/fgvk-diagnostics` | Older reports remain readable after log branding and operation names changed. Remove only after a declared diagnostics support window expires. | `test_diagnostics_helper.py` covers current input plus `resume-generated-frames`, `generated-image-recovered`, and `swapchain-recreation-suppressed`. |
| CPU pacing-profile version 1 captures and version-1 output observations | `engine/scripts/analyze-pacing-profile.py` | Local preview captures contain aggregate source/output timing without version 2's correlated hitch fields, and version-1 individual outputs omit detailed worker readiness/queue-lock timing. Keep both readable without inventing missing per-source, worker or completed-GPU attribution. Remove only after those retained preview captures are no longer needed or an archive reader replaces this path. | `test_pacing_profile_report.py` requires explicit unavailable attribution for version 1 and rejects malformed or unsupported records. |
| Runtime-status filenames without process start ticks | `engine/fgvk-render/src/runtime_status.cpp` startup pruning | Abnormal exits from older releases can leave `<pid>-<role>-<context>.json`. Remove after the upgrade baseline cannot retain one. | Runtime-status tests remove only stale current and legacy records while preserving active locks and unrelated or unsafe entries. |

## Completed cleanups

| Cleanup | Result | Completed |
| --- | --- | --- |
| Native Steam Remote Play removal | Removed the native Steam Remote Play override (`fgvk-remote-play`, `fgvk_remote_play/`, its SDR helper) and the stream-only `current_profile` / `FGVK_FOLLOW_CURRENT_PROFILE` selection. The root `current_profile` key is ignored, so existing files still load and match by `active_in`. Older builds may have left `~/.config/framegen-vulkan/native-remote-play.json` or a `streaming_client.fgvk-original` backup; restore them with that older build's `fgvk-remote-play restore-before-uninstall` before upgrading. This build neither restores nor deletes them and never touches Steam files. | 2026-11-13 |
| Separate plugin integration and Qt configuration UI removal | Removed the separate plugin integration, its generators and bindings, and the Qt configuration application. The Renderer reads only `~/.config/framegen-vulkan/conf.toml`. Older installations need a one-time migration. | 2026-11-07 |
| Numbered Flatpak filename imports | Removed unused internal aliases for individual branches; all bundle filenames and extension identities use the generated runtime catalogue. Retiring a branch no longer breaks generated constants, and published filenames remain unchanged. | 2026-10-08 |
| Pre-Steady-default Adaptive profile override | Removed the missing-field exception so every profile without an explicit choice adopts the current Steady Base Cap default. Profiles that explicitly saved Fractional remain unchanged. | 2026-08-21 |
| Spatial-scaling layer and `FGVK_SPLIT_LAYER_CHAIN` | Removed the second `libfgvk-render-scaling.so` copy, its `VkLayer_FGVK_spatial_scaling` manifests, `ENABLE_FGVK_SPATIAL_SCALING` and `DISABLE_FGVK_SPATIAL_SCALING`, the same-process capability and create relay, and the `FGVK_SPLIT_LAYER_CHAIN` reader. One `libfgvk-render.so` and one manifest remain; an old launcher that still exports `FGVK_SPLIT_LAYER_CHAIN` is ignored. | 2026-11-14 |
| Gamescope support | Removed the Gamescope feedback reader, the `fgvk-vrr-lease` helper, the compatibility record and checker, the Gamescope-only swapchain and pacing branches, the `gamescope_vrr_mode` and `frame_generation_refresh_threshold` profile keys (accepted, ignored and dropped on save), and the `FGVK_GAMESCOPE_VRR_MODE` and `FGVK_FRAME_GENERATION_REFRESH_THRESHOLD` overrides. The launcher exits at start in a Gamescope session without frame generation. | 2026-11-14 |
| Retired scaling profile keys | `scaling_enabled`, `scaling_method`, `scaling_factor`, `scaling_supersampling`, and `scaling_sharpness`, plus their `FGVK_SCALING_*` environment variables, were removed with the spatial scaler. Old `conf.toml` files still load because these keys are accepted and ignored, and the writer never emits them. | 2026-11-14 |
| Development tool removal | Removed the present measurer (`engine/fgvk-measure`) and its analyser, the `fgvk-cli` `benchmark`, `debug`, `quality-regression` and `synchronization-validation-canary` commands with their image-quality module, the latency and framegen comparison scripts, the latency probe and the fgvk Gym bridge script. `validate`, `inspect-dll` and `mark` are unchanged. | 2026-11-15 |

## Removal checklist

Before deleting a current obligation:

1. Confirm whether the old representation contains unique user data or is only generated output.
2. Identify the published release that first wrote the canonical replacement and compare it with the declared minimum direct-upgrade baseline.
3. Verify that install, startup, rollback, and skipped-version upgrades cannot encounter the removed path within the supported contract.
4. Remove the compatibility code, its ledger entry, and only the tests that no longer express a current invariant.
5. Run the boundary-specific suite in `TESTING.md`; package/install changes also require local archive verification.
6. Mention user-visible compatibility removals in the component release notes.

Diagnostic input compatibility, stable package identifiers, and published archive names have their own contracts in `AGENTS.md`; do not treat them as generated-wrapper cleanup.

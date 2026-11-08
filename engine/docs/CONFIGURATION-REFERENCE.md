# Advanced configuration reference

For normal setup, use the [configuration guide](CONFIGURATION.md). This page covers every `conf.toml` field and environment variable. The file is `~/.config/framegen-vulkan/conf.toml`.

## Format compatibility

Only `version = 2` is supported. Unknown keys are ignored. Launcher compatibility options belong in `launcher.conf`, shader selection in the vkBasalt configuration, and HDR buffer precision is a native profile field.

## Profiles

`active_in` matches executable names, process names, or executable-path suffixes. `FGVK_PROFILE` selects an exact profile and overrides automatic matching. If nothing matches, framegen-vulkan stays inactive. A minimal example:

```toml
version = 2

[global]
allow_fp16 = true

[[profile]]
name = "My game"
active_in = ["Game.exe"]
frame_generation_provisioned = true
frame_generation_enabled = true
multiplier = 2

scaling_enabled = false
scaling_method = "ls1"
scaling_factor = 1.5
scaling_supersampling = false
scaling_sharpness = 0.8
```

### AC and battery settings

Both optional `handheld` and `docked` tables must exist when enabled. They inherit omitted Renderer values from the preceding profile and cannot override its name or matches. DLL, FP16, Shaders, and launcher settings stay shared.

```toml
[[profile]]
name = "My game"
active_in = ["Game.exe"]
adaptive = true
target_fps = 90

[profile.handheld]
target_fps = 60

[profile.docked]
target_fps = 120
```

Battery selects Handheld; confirmed AC selects Docked. Unavailable startup detection uses Base; failures during play retain the last confirmed source. Use current matching editors, as older versions may discard these tables.

### Launcher exclusions

Known launchers and helpers stay inactive while their child games can match. The [shared registry](../fgvk-common/launcher_exclusions.json) owns the exact exclusions; generic `Launcher.exe` is not globally excluded. If only a launcher is detected, wait for gameplay and match the game executable instead. Maintainers can find registry changes in [the compatibility ledger](../../CLEANUPS.md).

## Global settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `dll` | Automatic | Optional absolute path to `Lossless.dll`. LSFG and LS1 require it; framegen-vulkan Scaler and Native Resolution do not. |
| `allow_fp16` | `true` | Selects FP16 for supported LSFG models and framegen-vulkan Scaler; `false` selects FP32. If a model lacks native FP16, select FP32. LS1 always uses FP32. Requires restart; Ultra Performance respects this choice. [Experimental source builds](BUILDING-FROM-SOURCE.md#experimental-forced-lsfg-fp16) have separate limits. |

## Profile settings

### Frame Generation

| Setting | Default | Meaning |
| --- | --- | --- |
| `frame_generation_provisioned` | `true` | Prepares Frame Generation at process start. Set `false` for Scaling-only use. Requires restart. |
| `frame_generation_enabled` | `true` | Live execution state. `false` pauses generation. |
| `multiplier` | `2` | Fixed total output multiplier, from 2–5. |
| `adaptive` | `false` | Enables Adaptive instead of Fixed generation. |
| `target_fps` | `120` | Adaptive generation target, or saved fallback when refresh matching is on. Fractional Automatic permits sustained native FPS above this target when the manual base cap is off or at least the target; the cap remains enforced. |
| `adaptive_target_refresh_rate` | `false` | Matches Adaptive's output target to confirmed Gamescope display refresh, clamped to the Renderer’s 10–1000 FPS range. Applies live; unavailable or zero refresh uses the saved `target_fps`. Fixed ignores this choice. |
| `adaptive_max_multiplier` | `3` | Adaptive multiplier ceiling, from 2–5. |
| `adaptive_auto_base_fps_cap` | `false` | Enables Steady Base Cap behavior. |
| `adaptive_fractional_real_frame_priority` | `auto` | Fractional real-frame preference: `auto`, `low`, `medium`, `high`, or `very-high`. |
| `adaptive_stable_cadence` | `true` | Favours a stable validated cadence when available. |
| `gamescope_vrr_mode` | `follow-steam` | Per-profile live Gamescope VRR choice: `follow-steam`, `on`, or `off`. On and Off take effect only when Gamescope reports that the active display supports VRR; otherwise they are saved but leave the live setting untouched. |
| `base_fps_cap` | `0` | Real-frame cap; `0` disables it. |
| `frame_generation_refresh_threshold` | `0` | Pauses generation at or below a confirmed Gamescope refresh; `0` disables it. |
| `dynamic_cadence_recovery` | `false` | Rechecks native cadence for games that switch rates between gameplay and menus. Temporarily reduces generated work while sampling; checks can lower output FPS or cause hitches under sustained GPU load. |
| `dynamic_cadence_probe_interval_seconds` | `2.0` | Recovery interval from 0.1–3 seconds. |

See [Adaptive validation](ADAPTIVE-VALIDATION.md) for scheduling details.

### Scaling

| Setting | Default | Meaning |
| --- | --- | --- |
| `scaling_enabled` | `false` | Enables scaling at process start. Requires restart. |
| `scaling_method` | `ls1` | `native`, `fgvk`, `ls1`, or `ls1-performance`. LS1 failures fall back to framegen-vulkan Scaler. |
| `scaling_factor` | `1.5` | Target output-to-source ratio per dimension from 1.0–2.0; the surface, display target, and resource limits may reduce the effective ratio. |
| `scaling_supersampling` | `false` | Allows supported variable Gamescope surfaces to render beyond the display target before downsampling. |
| `scaling_sharpness` | `0.8` | Sharpening strength from 0.0–1.0. |
| `hdr_quality` | `'performance'` | Live HDR quality: `'quality'` retains full precision; `'balanced'` uses compact shader/scaling buffers with full motion analysis and linear-light generation; `'performance'` also uses approximate HDR10 interpolation and halves motion Flow Scale, bounded at 0.25. Output resolution and saved Flow Scale are preserved. SDR ignores it. See [HDR quality](HDR-PIPELINE.md#hdr-quality). |
| `swapchain_image_count_compatibility` | `false` | Preserves the game's requested swapchain image minimum. Use only for games that otherwise fail to start. Requires restart. |

### Performance and device selection

| Setting | Default | Meaning |
| --- | --- | --- |
| `gpu_headroom_percent` | `0` | GPU Headroom reserve: `0` (Off), `5`, `10`, `15`, or `20` percent. Live, Frame Generation only; see below. |
| `flow_scale` | `0.8` | LSFG motion-estimation resolution from 0.25–1.0. Lower values reduce GPU cost and may reduce quality. |
| `performance_mode` | `false` | Uses the lighter LSFG model. |
| `ultra_performance` | `false` | Restart-bound lighter preset using 70% Flow Scale, the lighter model and LS1 Performance when scaling is enabled. Respects the global precision choice. |
| `gpu` | Automatic | Optional GPU name, vendor/device ID, or PCI bus ID. Cross-GPU Frame Generation is unsupported. |
| `pacing` | `none` | Compatibility field; `none` is the only supported value. |

### GPU Headroom

GPU Headroom is a soft utilization target for the application's Vulkan GPU. A 10% reserve aims for roughly 90% utilization. It requires readable AMD `gpu_busy_percent` telemetry for that exact DRM render node; unsupported devices, missing permissions and stale readings retain normal pacing. Gamescope is not required. Readings cover the whole GPU, including the compositor and other applications, so this is neither a game-only meter nor a power or temperature limit.

Changes apply live while Frame Generation is active. Off stops Headroom sampling and removes only its additional cap; existing manual and Adaptive caps still apply. Brief spikes may reach 100%. Loading pauses and unavailable telemetry release the extra cap so stale measurements cannot keep throttling a new scene.

Fixed retains its selected multiplier, so reserving capacity may lower both real and output FPS. Releasing Headroom does not force a native-frame probe: ordinary Fixed/FIFO pacing can retain that generated workload after the scene becomes lighter, including with Headroom Off. Use Adaptive when restoring a higher real-frame rate after load changes is important. Adaptive can use more generated frames within its configured ceiling and existing qualification rules. Neither policy changes model quality, Flow Scale or clocks. The controller never deliberately caps below 20 real FPS; Adaptive additionally respects `Target FPS / available maximum multiplier`. For example, a 90 FPS target with a 2× ceiling cannot trade away real frames below 45 FPS. A higher permitted multiplier allows more room but can increase interpolation artifacts and input latency. The utilization target is not guaranteed when generation is expensive, another process consumes the GPU, or the floor is reached.

GPU Headroom is dormant with Frame Generation disabled or refresh-paused, during confirmed Steam-menu suspension, and during ordered-acquire recovery. It does not independently control scaling or shaders. Dynamic Cadence Recovery is mutually exclusive: enabling Headroom disables Recovery; enabling Recovery clears Headroom. Profile and separate power-mode settings preserve the reserve. Live Status distinguishes unavailable telemetry and a reached real-frame floor; diagnostics record decisions independently of the saved setting.

See [Adaptive validation](ADAPTIVE-VALIDATION.md#gpu-headroom) for controller sampling and recovery, and [Runtime transitions](RUNTIME-TRANSITIONS.md) for when edits take effect.

## Explicit launch selection

```text
FGVK_CONFIG="$HOME/.config/framegen-vulkan/conf.toml" FGVK_PROFILE="My game" ~/.local/bin/fgvk %command%
```

`DISABLE_FGVK=1` bypasses framegen-vulkan. Native Remote Play uses `FGVK_FOLLOW_CURRENT_PROFILE=1` to follow a valid root `current_profile` selection on reload, falling back to `FGVK_PROFILE`. Ordinary game launchers use normal matching.

## Environment-only configuration

With `FGVK_ENV=1`, `FGVK_GPU_HEADROOM_PERCENT=0|5|10|15|20` selects the same reserve. Other values are rejected.

Set `FGVK_ENV=1` to build one profile from environment variables instead of TOML:

- global: `FGVK_DLL_PATH`, `FGVK_NO_FP16`;
- Fixed and identity: `FGVK_GPU`, `FGVK_MULTIPLIER`, `FGVK_FRAME_GENERATION_PROVISIONED`, `FGVK_FRAME_GENERATION_ENABLED`, `FGVK_FRAME_GENERATION_REFRESH_THRESHOLD`, `FGVK_BASE_FPS_CAP`;
- Adaptive: `FGVK_ADAPTIVE`, `FGVK_ADAPTIVE_AUTO_BASE_FPS_CAP`, `FGVK_ADAPTIVE_FRACTIONAL_REAL_FRAME_PRIORITY`, `FGVK_TARGET_FPS`, `FGVK_ADAPTIVE_TARGET_REFRESH_RATE`, `FGVK_ADAPTIVE_MAX_MULTIPLIER`, `FGVK_ADAPTIVE_STABLE_CADENCE`, `FGVK_DYNAMIC_CADENCE_RECOVERY`, `FGVK_DYNAMIC_CADENCE_PROBE_INTERVAL_SECONDS`;
- Gamescope VRR: `FGVK_GAMESCOPE_VRR_MODE` (`follow-steam`, `on`, or `off`);
- HDR quality: `FGVK_HDR_QUALITY` (`quality`, `balanced`, or `performance`; defaults to `performance`);
- Scaling: `FGVK_SCALING_ENABLED`, `FGVK_SCALING_METHOD`, `FGVK_SCALING_FACTOR`, `FGVK_SCALING_SUPERSAMPLING`, `FGVK_SCALING_SHARPNESS`, `FGVK_SWAPCHAIN_IMAGE_COUNT_COMPATIBILITY`; and
- resources: `FGVK_ULTRA_PERFORMANCE`, `FGVK_FLOW_SCALE`, `FGVK_PERFORMANCE_MODE`, `FGVK_PACING`.

`FGVK_DISABLE_HDR_EXPOSURE=1` disables HDR exposure by default. The restart-only HDR control exports `FGVK_DISABLE_HDR_EXPOSURE=0` and `DXVK_HDR=1` while retaining `DISABLE_GAMESCOPE_WSI=1`; see the [HDR launch contract](HDR-PIPELINE.md#toggle-and-launch-contract). Environment-only settings are process-start settings, not live profile controls.

## Pacing profiler

`FGVK_PACING_PROFILE_TRACE=1` enables per-source CPU phases/settings, individual outer present/acquire/wait calls, worker readiness/queue-lock timing, exact source-tagged completed backend GPU batches and native Wayland feedback. It implies CPU/GPU/Wayland profiling, retains mapped ELF build and process/profile identity, and preserves the beginning of a private 64 MiB capture before stopping explicitly. It adds per-call file publication and observer overhead without changing saved settings, introducing host GPU waits or querying fences before presentation. The trace does not select a different presentation path; generated copies stay on the deferred worker. Its observers still add CPU and logging cost. See the [detailed frame trace](COLLECT_DIAGNOSTICS.md#detailed-game-frame-trace) for raw capture retention, exact/missing joins, private JSON export and the optional protocol companion.

`FGVK_PACING_PROFILE_COPY=1` implies the trace and adds native copy timestamp queries in the existing copy command buffers, plus optional supported KHR/EXT device-to-host clock calibration; its reports retain calibration uncertainty and separate estimated copy-end timing from WSI semaphore signals and physical scanout. It moves generated copies onto the application's present thread with fence-based render-slot admission, so its pacing is not the product's.

`FGVK_PACING_PROFILE=1` enables independent process-start CPU pacing/phase summaries and application acquire/present observations without broad presentation diagnostics. The default is a private, bounded timing-only capture under `${TMPDIR:-/tmp}`; `FGVK_PACING_PROFILE_LOG` selects a new absolute file or `stderr` for console-only output. See the [pacing capture guide](COLLECT_DIAGNOSTICS.md#lightweight-minecraft-pacing-capture). `FGVK_PACING_PROFILE_GPU=1` additionally enables backend GPU timestamp collection and implies the CPU capture; it reads completed batches at the existing backend fence boundary, with no new wait. It is diagnostic measurement overhead and excludes game rendering, application-device scaling/copies and scanout. Profiling changes no saved profile or pacing policy.

`FGVK_PACING_PROFILE_WAYLAND=1` implies CPU capture and adds native Wayland `wp_presentation` feedback on the application's borrowed surface. It records compositor-reported presentation timestamps/discards separately from lower-call timing, without GPU queries, socket reads, commits, target-time changes or an event-reader worker. It uses a private event queue serviced by the application's existing display reader. The [capture guide](COLLECT_DIAGNOSTICS.md#lightweight-minecraft-pacing-capture) describes clock domains, bounded storage, protocol/callback gaps and ambiguous commit association. This process-start observer adds protocol and dispatch overhead; remove it after the capture.

`FGVK_PACING_PROFILE_CONTENT=1` implies CPU capture and arms an explicit-trigger image sequence capture for Fixed 2x SDR without spatial scaling. It records at most 128 small real/generated picture pairs plus source/history/image/queue identities, using the existing copy commands and completed render-fence boundary. No capture GPU resources or readback are created before the named trigger exists. Pictures are stored separately from timing-only logs and add transfer/readback/I/O overhead. See the same [capture guide](COLLECT_DIAGNOSTICS.md#lightweight-minecraft-pacing-capture) for warm-scene triggering, bounds and offline analysis.

## Presentation acquisition timeout

`FGVK_PRESENT_ACQUIRE_TIMEOUT_MS` limits how long framegen-vulkan waits for generated-image availability before native recovery. Adaptive VRR delivery and constrained Fixed image pools enforce a 50 ms limit when no explicit value is set. For a focused stall reproduction, try `25` and include the log. See [presentation recovery](LIFECYCLE.md) for deadline and fallback rules.

# Advanced configuration reference

For normal setup, use the [configuration guide](CONFIGURATION.md). This page covers every `conf.toml` field and environment variable. The file is `~/.config/framegen-vulkan/conf.toml`.

## Format compatibility

Only `version = 2` is supported. Unknown keys are ignored. Launcher compatibility options belong in `launcher.conf`.

## Profiles

`active_in` matches executable names, process names, or executable-path suffixes. `FGVK_PROFILE` selects an exact profile and overrides automatic matching. If nothing matches, framegen-vulkan stays inactive. A minimal example:

```toml
version = 2

[global]
allow_fp16 = true
pacing_correction = true

[[profile]]
name = "My game"
active_in = ["Game.exe"]
frame_generation_provisioned = true
frame_generation_enabled = true
multiplier = 2
```

### Launcher exclusions

Known launchers and helpers stay inactive while their child games can match. The [shared registry](../fgvk-common/launcher_exclusions.json) owns the exact exclusions; generic `Launcher.exe` is not globally excluded. If only a launcher is detected, wait for gameplay and match the game executable instead. Maintainers can find registry changes in [the compatibility ledger](../../CLEANUPS.md).

## Global settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `dll` | Automatic | Optional absolute path to `Lossless.dll`. Frame Generation requires it. |
| `allow_fp16` | `true` | Selects FP16 for supported LSFG models; `false` selects FP32. If a model lacks native FP16, select FP32. Requires restart; Ultra Performance respects this choice. [Experimental source builds](BUILDING-FROM-SOURCE.md#experimental-forced-lsfg-fp16) have separate limits. |
| `pacing_correction` | `true` | Fixed Frame Generation with internal images: a GPU timestamp after each output's present measures when the private present queue next runs, and each generated frame is moved (at most ±1 ms) onto its own ideal point between its two reals, at every multiplier. Real frames are never moved. No target times, no waits. Requires restart. `FGVK_REAL_OFFSET_US` accepts only `auto`, which turns it on; any other value, such as `0` or a number, turns it off. |

## Profile settings

### Frame Generation

| Setting | Default | Meaning |
| --- | --- | --- |
| `frame_generation_provisioned` | `true` | Prepares Frame Generation at process start. Set `false` to skip Frame Generation interop. Requires restart. |
| `frame_generation_enabled` | `true` | Live execution state. `false` pauses generation. |
| `multiplier` | `2` | Fixed total output multiplier, from 2–5. |
| `adaptive_stable_cadence` | `true` | Favours a stable validated cadence when available. |
| `low_latency` | `true` | Just-in-time release at a cap. With Frame Generation on and `base_fps_cap` set, each capped source present moves its release point later by a delay (at most three cap intervals): the delay grows while the source is more than 2 ms ahead of its cap deadline and halves when it is late. The game then starts its next frame later instead of queueing up to three frames ahead. The cap clock and output timing do not change. An application that waits on no release token is held at its acquire instead. It also holds the game's next frame until the GPU is about to finish the previous one, like Reflex, so a GPU-bound game does not queue frames behind the GPU. With the game's Reflex on, Reflex holds the game; low latency only starts frame generation just in time. `false` keeps every release at the cap deadline. With it on, each released output batch also starts about 2 ms after its frames are ready, phase-locked to the batch cadence, so readiness jitter does not show as uneven output spacing. Applies live. The latency effect is not yet measured in a game run. |
| `base_fps_cap` | `0` | Real-frame cap; `0` disables it. With Frame Generation on, the minimum frame interval a Reflex-enabled game requests is raised to this cap's interval, so the game holds the cap itself. |

See [Fixed validation](ADAPTIVE-VALIDATION.md) for pacing details.

### Swapchain

| Setting | Default | Meaning |
| --- | --- | --- |
| `swapchain_image_count_compatibility` | `false` | Preserves the game's requested swapchain image minimum. Use only for games that otherwise fail to start. Requires restart. |

### Retired scaling keys

The spatial scaler was removed. `scaling_enabled`, `scaling_method`, `scaling_factor`, `scaling_supersampling`, and `scaling_sharpness` (and their `FGVK_SCALING_*` environment variables) are retired. They are accepted and ignored, so old files still load, and framegen-vulkan does not write them.

The `gamescope_vrr_mode` and `frame_generation_refresh_threshold` keys and the `FGVK_GAMESCOPE_VRR_MODE` and `FGVK_FRAME_GENERATION_REFRESH_THRESHOLD` overrides were removed with Gamescope support. Existing files still load; the keys and overrides are ignored, and the keys are omitted when the file is next written.

`[profile.handheld]` and `[profile.docked]` tables in old files are accepted and ignored.

`gpu_headroom_percent` and its `FGVK_GPU_HEADROOM_PERCENT` override were removed with GPU Headroom. Old files still load; the key is accepted and ignored, and framegen-vulkan does not write it.

The Adaptive keys `adaptive`, `adaptive_max_multiplier`, `adaptive_auto_base_fps_cap`, `adaptive_fractional_real_frame_priority`, `adaptive_target_refresh_rate`, `target_fps`, `dynamic_cadence_recovery`, and `dynamic_cadence_probe_interval_seconds` were removed with Adaptive generation. Old files still load; these keys are accepted with any value and ignored, a profile with `adaptive = true` runs Fixed with its `multiplier`, and framegen-vulkan does not write them. Their `FGVK_ADAPTIVE*`, `FGVK_TARGET_FPS`, and `FGVK_DYNAMIC_CADENCE_*` overrides are no longer read.

### Performance and device selection

| Setting | Default | Meaning |
| --- | --- | --- |
| `flow_scale` | `0.8` | LSFG motion-estimation resolution from 0.25–1.0. Lower values reduce GPU cost and may reduce quality. |
| `performance_mode` | `false` | Uses the lighter LSFG model. |
| `ultra_performance` | `false` | Restart-bound lighter preset using 70% Flow Scale and the lighter model. Respects the global precision choice. |
| `gpu` | Automatic | Optional GPU name, vendor/device ID, or PCI bus ID. Cross-GPU Frame Generation is unsupported. |
| `pacing` | `none` | Compatibility field; `none` is the only supported value. |

## Explicit launch selection

```text
FGVK_CONFIG="$HOME/.config/framegen-vulkan/conf.toml" FGVK_PROFILE="My game" ~/.local/bin/fgvk %command%
```

`DISABLE_FGVK=1` bypasses framegen-vulkan. Ordinary game launchers use normal matching.

## Launcher settings

These keys live in `${XDG_CONFIG_HOME:-$HOME/.config}/framegen-vulkan/launcher.conf`, or in `FGVK_LAUNCH_CONFIG` when that is set. The `fgvk` launcher reads them before the game starts. Boolean keys take only `0` or `1`. A duplicate or unknown key makes the whole file invalid, and the launcher then applies no settings from it.

| Setting | Default | Meaning |
| --- | --- | --- |
| `version` | required | Must be `1`. |
| `disable_steam_overlay` | `1` | Turns off Steam's in-game overlay and FPS counter before the game starts, which may reduce stutter and may affect Steam Input. |
| `enable_zink` | `0` | Runs OpenGL through Vulkan. |
| `force_alsa_audio` | `0` | Changes the audio path for compatibility problems. |
| `disable_hdr_exposure` | Not written | Retired. `0` or `1` is still accepted and ignored. |
| `log_pacing` | `0` | Exports `FGVK_PACING_PROFILE=1`: light pacing summaries. |
| `log_trace` | `0` | Exports `FGVK_PACING_PROFILE_TRACE=1`: every present, acquire and wait. Larger file. |
| `log_wayland` | `0` | Exports `FGVK_PACING_PROFILE_WAYLAND=1`: compositor presentation feedback. |
| `log_gpu_cost` | `0` | Exports `FGVK_PACING_PROFILE_GPU_SPLIT=1` and `FGVK_PACING_PROFILE=1`: frame-generation GPU time. Does not change pacing. |
| `log_present` | `0` | Exports `FGVK_PRESENT_DIAGNOSTICS=1`: verbose renderer messages on stderr. |

A logging key exports its variable only when the caller has not already set that variable, so a value in the launch options always wins, including `=0`. The Logging section of the `fgvk` settings window writes the five `log_*` keys.

## Environment-only configuration

Set `FGVK_ENV=1` to build one profile from environment variables instead of TOML:

- global: `FGVK_DLL_PATH`, `FGVK_NO_FP16`;
- Fixed and identity: `FGVK_GPU`, `FGVK_MULTIPLIER`, `FGVK_FRAME_GENERATION_PROVISIONED`, `FGVK_FRAME_GENERATION_ENABLED`, `FGVK_BASE_FPS_CAP`;
- Smooth Cadence: `FGVK_ADAPTIVE_STABLE_CADENCE`;
- Compatibility: `FGVK_SWAPCHAIN_IMAGE_COUNT_COMPATIBILITY`; and
- resources: `FGVK_ULTRA_PERFORMANCE`, `FGVK_FLOW_SCALE`, `FGVK_PERFORMANCE_MODE`, `FGVK_PACING`.


## Pacing profiler

`FGVK_PACING_PROFILE_TRACE=1` enables per-source CPU phases/settings, individual outer present/acquire/wait calls, worker readiness/queue-lock timing, and native Wayland feedback. It implies CPU and Wayland profiling, but not backend GPU timing (add `FGVK_PACING_PROFILE_GPU=1` for source-tagged GPU batches, which changes pacing as described below), retains mapped ELF build and process/profile identity, and preserves the beginning of a private 512 MiB capture under `${TMPDIR:-/tmp}` (often RAM-backed) before stopping explicitly. It adds per-call file publication and observer overhead without changing saved settings, introducing host GPU waits or querying fences before presentation. The trace does not select a different presentation path; generated copies stay on the deferred worker. Its observers still add CPU and logging cost. See the [detailed frame trace](COLLECT_DIAGNOSTICS.md#detailed-game-frame-trace) for raw capture retention, exact/missing joins, private JSON export and the optional protocol companion.

`FGVK_PACING_PROFILE=1` enables independent process-start CPU pacing/phase summaries and application acquire/present observations without broad presentation diagnostics. The default is a private, bounded timing-only capture under `${TMPDIR:-/tmp}`; `FGVK_PACING_PROFILE_LOG` selects a new absolute file or `stderr` for console-only output. See the [pacing capture guide](COLLECT_DIAGNOSTICS.md#lightweight-minecraft-pacing-capture). `FGVK_PACING_PROFILE_GPU=1` additionally enables backend GPU timestamp collection and implies the CPU capture; to read completed queries the backend waits for every in-flight batch on each source, on the application's present thread, so the game's present can be held for most of a GPU frame and its frame rate drops: do not use it to measure pacing. It is diagnostic measurement overhead and excludes game rendering, application-device copies and scanout. Profiling changes no saved profile or pacing policy.

`FGVK_PACING_PROFILE_GPU_SPLIT=1` adds non-draining GPU boundaries to a `FGVK_PACING_PROFILE=1` capture. It has no effect without that switch and does not imply `FGVK_PACING_PROFILE_GPU=1`. It never drains or waits on in-flight work, changes no wait, path or schedule, and does not make the drained GPU profile active. The backend reads each batch's boundaries only after the existing completion fence has signalled, and a query that is not ready yet is skipped. Each completed generated backend batch emits one `pacing-profile-gpu-split` version 1 record, correlated to its source with `source`, `backend_frame`, the application copy's `game_done_ns` and `source_copy_end_ns`, and the backend's `fg_start_ns`, `prepass_end_ns`, `generated_end_ns` and `batch_end_ns`. Every time is CLOCK_MONOTONIC nanoseconds, the same base as `started_ns`; zero means unknown. The render half stays zero unless the application device enabled calibrated timestamps. The backend half needs VK_KHR_calibrated_timestamps (or its EXT form) with the device and CLOCK_MONOTONIC domains; without it the backend logs `framegen-vulkan: GPU split profile unavailable; reason=calibration` once and stays off. Each generated output the deferred worker copies at its deadline also emits one `pacing-profile-output-gpu` version 1 record, with `source`, `available` and the copy's `copy_start_ns` and `copy_end_ns` on the GPU. It is read after the render slot's fence, so it can appear a few batches after its output, and a skipped output reports `available=0`. The boundaries are GPU ordering points, so bottom-of-pipe timestamps can still cost some overlap on some drivers. Compare with the switch off before reading small differences as pacing.

`FGVK_PACING_PROFILE_WAYLAND=1` implies CPU capture and adds native Wayland `wp_presentation` feedback on the application's borrowed surface. It records compositor-reported presentation timestamps/discards separately from lower-call timing, without GPU queries, socket reads, commits, target-time changes or an event-reader worker. It uses a private event queue serviced by the application's existing display reader. The [capture guide](COLLECT_DIAGNOSTICS.md#lightweight-minecraft-pacing-capture) describes clock domains, bounded storage, protocol/callback gaps and ambiguous commit association. This process-start observer adds protocol and dispatch overhead; remove it after the capture.

## Presentation acquisition timeout

`FGVK_PRESENT_ACQUIRE_TIMEOUT_MS` limits how long framegen-vulkan waits for generated-image availability before native recovery. Constrained Fixed image pools enforce a 50 ms limit when no explicit value is set. For a focused stall reproduction, try `25` and include the log. See [presentation recovery](LIFECYCLE.md) for deadline and fallback rules.

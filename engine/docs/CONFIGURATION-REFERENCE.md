# Advanced configuration reference

For normal setup, use the [configuration guide](CONFIGURATION.md). This page covers every `conf.toml` field and environment variable. The file is `~/.config/framegen-vulkan/conf.toml`.

## Format compatibility

Only `version = 2` is supported. Unknown keys are ignored. Launcher compatibility options belong in `launcher.conf`.

## Profiles

`active_in` matches executable names, process names, or executable-path suffixes. `FGVK_PROFILE` selects an exact profile and overrides automatic matching. An unmatched game launched through `fgvk` uses the configured default profile, if any. A minimal example:

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
| `default_profile` | Unset | Exact profile name to use for unmatched games started through `fgvk`. The settings window updates this name on rename and clears it on deletion. |
| `allow_fp16` | `true` | Selects FP16 for supported LSFG models; `false` selects FP32. If a model lacks native FP16, select FP32. Requires restart; Ultra Performance respects this choice. [Experimental source builds](BUILDING-FROM-SOURCE.md#experimental-forced-lsfg-fp16) have separate limits. |
| `pacing_correction` | `true` | Fixed Frame Generation with internal images: a GPU timestamp after each output's present measures when the private present queue next runs, and each generated frame is moved (at most ±2.5 ms) onto its own ideal point between its two reals, at every multiplier. Real frames are never moved. No target times or waits. Changes apply to the next worker batch after configuration reload. Enabling it starts with fresh timing samples. `FGVK_REAL_OFFSET_US` accepts only `auto`, which turns it on; any other value, such as `0` or a number, turns it off. The screen time of each output (Xwayland, X11, Gamescope or native Wayland feedback) drives the correction when it is available, with or without the pacing profile; `FGVK_ONSCREEN_CORRECTION=0` keeps the GPU timing only. See [diagnostics](COLLECT_DIAGNOSTICS.md#on-screen-correction-os1). |
| `real_present_lead` | `true` | A real output that follows a generated output presents earlier by the GPU wait its markers measured, at most 2.5 ms, so the GPU spacing of the pair stays even. The real is not otherwise moved. While the lead is in effect, the GPU-marker part of `pacing_correction` takes no samples and stays at zero; screen-mode correction still runs. It needs the GPU markers: any `FGVK_REAL_OFFSET_US` value other than `auto` turns them off, and then the lead stays off. Changes apply to the next worker batch after configuration reload. `FGVK_REAL_PRESENT_LEAD` overrides it. |

## Profile settings

### Frame Generation

| Setting | Default | Meaning |
| --- | --- | --- |
| `frame_generation_provisioned` | `true` | Prepares Frame Generation at process start. Set `false` to skip Frame Generation interop. Requires restart. |
| `frame_generation_enabled` | `true` | Live execution state. `false` pauses generation. |
| `multiplier` | `2` | Fixed total output multiplier, from 2–5. |
| `low_latency` | `true` | Just-in-time release at a cap. With Frame Generation on and `base_fps_cap` set, each capped source present moves its release point later by a delay (at most three cap intervals): the delay grows while the source is more than 2 ms ahead of its cap deadline and halves when it is late. The game then starts its next frame later instead of queueing up to three frames ahead. The cap clock and output timing do not change. An application that waits on no release token is held at its acquire instead. It also holds the game's next frame until the GPU is about to finish the previous one, like Reflex, so a GPU-bound game does not queue frames behind the GPU. With the game's Reflex on, Reflex holds the game; low latency only starts frame generation just in time. `false` keeps every release at the cap deadline. How each output batch starts is set by `batch_hold`. Applies live. The latency effect is not yet measured in a game run. |
| `batch_hold` | `true` | Each released output batch starts about 2 ms after its frames are ready, phase-locked to the batch cadence, so GPU timing wobble does not show as uneven output spacing. Applies with `low_latency` on and off. With `low_latency` off, a batch starts at its schedule or its held readiness, whichever is later. `false` starts each batch at its readiness (or at its schedule, when that is later): up to about 2 ms less latency, slightly less even spacing. Applies live. |
| `base_fps_cap` | `0` | Real-frame cap; `0` disables it. With Frame Generation on, the minimum frame interval a Reflex-enabled game requests is raised to this cap's interval, so the game holds the cap itself. |

HDR10/PQ swapchains (`A2B10G10R10_UNORM_PACK32` or `A2R10G10B10_UNORM_PACK32` with `HDR10_ST2084_EXT`) and linear scRGB swapchains (`R16G16B16A16_SFLOAT` with `EXTENDED_SRGB_LINEAR_EXT`) get frame generation in linear light, with no setting to choose it. Other HDR colour spaces (HLG, Dolby Vision, extended sRGB nonlinear) pass through without frame generation. Under Wine on native Wayland, the same two HDR pairs arrive as `PASS_THROUGH` and get frame generation too.

See [Fixed pacing](WSI-ISOLATION.md#fixed-pacing-paths) for pacing details.

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
| `log_present` | Not written | Retired. `0` or `1` is still accepted and ignored. |

A logging key exports its variable only when the caller has not already set that variable, so a value in the launch options always wins, including `=0`. The Logging section of the `fgvk` settings window writes the four `log_*` keys.

## Environment-only configuration

Set `FGVK_ENV=1` to build one profile from environment variables instead of TOML:

- global: `FGVK_DLL_PATH`, `FGVK_NO_FP16`;
- Fixed and identity: `FGVK_GPU`, `FGVK_MULTIPLIER`, `FGVK_FRAME_GENERATION_PROVISIONED`, `FGVK_FRAME_GENERATION_ENABLED`, `FGVK_BASE_FPS_CAP`;
- Compatibility: `FGVK_SWAPCHAIN_IMAGE_COUNT_COMPATIBILITY`; and
- resources: `FGVK_ULTRA_PERFORMANCE`, `FGVK_FLOW_SCALE`, `FGVK_PERFORMANCE_MODE`, `FGVK_PACING`.


## Pacing profiler

`FGVK_PACING_PROFILE_TRACE=1` enables per-source CPU phases/settings, individual outer present/acquire/wait calls, worker readiness/queue-lock timing, and native Wayland feedback. It implies CPU and Wayland profiling, but not backend GPU timing (add `FGVK_PACING_PROFILE_GPU=1` for source-tagged GPU batches, which changes pacing as described below), retains mapped ELF build and process/profile identity, and preserves the beginning of a private 512 MiB capture in the capture directory before stopping explicitly. It adds per-call file publication and observer overhead without changing saved settings, introducing host GPU waits or querying fences before presentation. The trace does not select a different presentation path; generated copies stay on the deferred worker. Its observers still add CPU and logging cost. See the [detailed frame trace](COLLECT_DIAGNOSTICS.md#detailed-game-frame-trace) for raw capture retention, exact/missing joins, private JSON export and the optional protocol companion.

`FGVK_PACING_PROFILE=1` enables independent process-start CPU pacing/phase summaries and application acquire/present observations without broad presentation diagnostics. The default is a private, bounded timing-only capture under `~/fgvk-logs`, or `${TMPDIR:-/tmp}` if the home directory is unavailable or the log directory cannot be used. `FGVK_PACING_PROFILE_LOG` selects a new absolute file or `stderr` for console-only output. See the [pacing capture guide](COLLECT_DIAGNOSTICS.md#lightweight-minecraft-pacing-capture). `FGVK_PACING_PROFILE_GPU=1` additionally enables backend GPU timestamp collection and implies the CPU capture. To read completed queries the backend waits for every in-flight batch on each source, on the application's present thread, so the game's present can be held for most of a GPU frame and its frame rate drops. Use this switch to measure GPU work. That measurement excludes game rendering, application-device copies and scanout. Profiling changes no saved profile or pacing policy.

`FGVK_PACING_PROFILE_GPU_SPLIT=1` adds non-draining GPU boundaries to a `FGVK_PACING_PROFILE=1` capture. It has no effect without that switch and does not imply `FGVK_PACING_PROFILE_GPU=1`. It never drains or waits on in-flight work, changes no wait, path or schedule, and does not make the drained GPU profile active. The backend reads each batch's boundaries only after the existing completion fence has signalled, and a query that is not ready yet is skipped. Each completed generated backend batch emits one `pacing-profile-gpu-split` version 1 record, correlated to its source with `source`, `backend_frame`, the application copy's `game_done_ns` and `source_copy_end_ns`, and the backend's `fg_start_ns`, `prepass_end_ns`, `generated_end_ns` and `batch_end_ns`. Every time is CLOCK_MONOTONIC nanoseconds, the same base as `started_ns`; zero means unknown. The render half stays zero unless the application device enabled calibrated timestamps. The backend half needs VK_KHR_calibrated_timestamps (or its EXT form) with the device and CLOCK_MONOTONIC domains; without it the backend logs `framegen-vulkan: GPU split profile unavailable; reason=calibration` once and stays off. The first deferred generated output per source has a `pacing-profile-output-gpu` version 1 record with `source` and `available`. On the game queue, `staging_start_ns` and `staging_end_ns` bracket the staging blit. On the worker queue, `copy_start_ns` and `copy_end_ns` bracket the copy into the acquired lower image. Older captures omit the staging fields. It is read after the render slot's fence, so it can appear a few batches after its output, and a skipped output reports `available=0`. The boundaries are GPU ordering points, so bottom-of-pipe timestamps can still cost some overlap on some drivers. Compare with the switch off before reading small differences as pacing.

`FGVK_PACING_PROFILE_WAYLAND=1` implies CPU capture and adds native Wayland `wp_presentation` feedback on the application's borrowed surface. It records compositor-reported presentation timestamps/discards separately from lower-call timing, without GPU queries, socket reads, commits, target-time changes or an event-reader worker. It uses a private event queue serviced by the application's existing display reader. The [capture guide](COLLECT_DIAGNOSTICS.md#lightweight-minecraft-pacing-capture) describes clock domains, bounded storage, protocol/callback gaps and ambiguous commit association. This process-start observer adds protocol and dispatch overhead; remove it after the capture.

`FGVK_BACKEND_QUEUE_PRIORITY=<value>` sets the priority of fgvk-backend's own device queue, from 0 to 1 (default 1), for an A/B against the game's queue. A value that is not a finite number in [0, 1] logs one line and uses 1; an accepted value is logged at device creation.

`FGVK_REAL_PRESENT_LEAD=1` turns the real present lead on (see `real_present_lead`), and `FGVK_REAL_PRESENT_LEAD=0` turns it off. Any other value, or unset, keeps the `[global] real_present_lead` setting. The variable is read once.

Unset marks fgvk's private present queue as the Reflex out-of-band present queue (`vkQueueNotifyOutOfBandNV`), and so does `FGVK_REFLEX_OUT_OF_BAND=present` or any other value that is not listed here. `FGVK_REFLEX_OUT_OF_BAND=render` marks the render queue, and `FGVK_REFLEX_OUT_OF_BAND=off` marks nothing. Read once. It applies only to a device whose application enabled `VK_NV_low_latency2`, and never to the application's own queues. The per-second pacing summary prints `reflex_oob=` (0 none, 1 present, 2 render) with `backend_queue_priority=`, so a capture names its arm.

## Experimental early generated copy

`FGVK_EARLY_GENERATED_COPY=1` lets the worker try a lower image and submit the generated frame's final copy before its host readiness wait. It applies to overlapping internal-image 2× batches on native Wayland FIFO and IMMEDIATE. The copy still waits for the staging and image-acquire semaphores. The worker keeps its existing present deadline calculation. If the lower image is unavailable, it retries on the normal path. With `FGVK_PACING_PROFILE=1`, `copy_submit_ns` records when the worker called the final-copy submit on both paths. Compare it with `ready_ns` and with `copy_start_ns` and `copy_end_ns` from the GPU split profile.

`FGVK_SAME_QUEUE_GENERATED_COPY=1` tests staging on the same private graphics queue as the final copy for overlapping internal-image 2× batches on native Wayland IMMEDIATE. It uses the game queue while an older batch still needs the private queue. The next source copy waits on the staging submission's GPU timeline value so the backend can safely reuse the generated image. Use with `FGVK_EARLY_GENERATED_COPY=1` and GPU split profiling. Join the `same-queue generated copy` stderr records to the GPU split trace by context and source, then compare both the stage-to-copy gap and generation-to-presentation time.

`FGVK_DIRECT_GENERATED_COPY=1` tries an early lower-image acquire with timeout zero for the same native Wayland IMMEDIATE 2× path. On success, a single copy on the game's graphics queue writes directly to the acquired image and the worker presents it on its normal schedule. Image pressure or lock contention retains staging. The trial reserves at most one early image and requires two existing lower images above the surface minimum. An available GPU split output with zero staging timestamps identifies the direct copy.

## Experimental transfer-queue generated copy

`FGVK_TRANSFER_GENERATED_COPY=1` records the final generated copy, from the staging image into the lower swapchain image, on a dedicated transfer-family queue of the device. It applies to worker-recorded generated copies of presents that use fgvk's internal application images, at any multiplier, and it needs the device's private present queue and frame-generation interop. Every active swapchain of such a device has lower images that are `VK_SHARING_MODE_CONCURRENT` across the graphics and transfer families. The private queue still presents, and `FGVK_DIRECT_GENERATED_COPY` and `FGVK_SAME_QUEUE_GENERATED_COPY` do not apply to the transfer-recorded batches. The per-second `pacing-profile` line reports `transfer_copy=1` for a context whose generated copies ran on that queue, and the device log prints `framegen-vulkan: transfer generated copy:`. Any value other than `1`, or unset, changes nothing. Read once.

## Presentation acquisition timeout

`FGVK_PRESENT_ACQUIRE_TIMEOUT_MS` limits how long framegen-vulkan waits for generated-image availability before native recovery. Constrained Fixed image pools enforce a 50 ms limit when no explicit value is set. For a focused stall reproduction, try `25` and include the log. See [presentation recovery](LIFECYCLE.md) for deadline and fallback rules.

## Gamescope tick lock

`FGVK_GAMESCOPE_TICK_LOCK=1` turns on the experimental gamescope tick lock; it is off by default. The lock runs only inside Gamescope, when the screen source is the on-screen watcher and the automatic real offset is on. Any other value leaves it off, and the variable is read once per process. Under those conditions each generated output starts a lead before one of Gamescope's ticks, and no two consecutive outputs share a tick. Every other path schedules exactly as before, and the refresh and multiplier cap is unchanged. See the [diagnostics](COLLECT_DIAGNOSTICS.md#gamescope-tick-lock-gt1) for the `tick_*` fields.

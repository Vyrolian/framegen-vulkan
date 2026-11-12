# Configuration

framegen-vulkan reads its profiles from `~/.config/framegen-vulkan/conf.toml`. Edit the file in any text editor. framegen-vulkan reads it when a game starts, so most changes apply to the next launch; settings marked live in [Applying changes](#applying-changes) can change while the game runs. Check each edit with `~/.local/bin/fgvk-cli validate`. The [configuration reference](CONFIGURATION-REFERENCE.md) lists every field, default, and environment variable.

## Quick start

1. Create `~/.config/framegen-vulkan/conf.toml` from the example below.
2. Set `active_in` to the game's executable name, such as `Game.exe`. Match the game's process, not its launcher, Steam title, or ROM filename.
3. Enable Frame Generation and/or Scaling for that profile.
4. Add `~/.local/bin/fgvk %command%` to the game's Steam launch options, then start the game.
5. Run `~/.local/bin/fgvk-cli validate` after each change.

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

scaling_enabled = false
scaling_method = "ls1"
scaling_factor = 1.5
scaling_supersampling = false
scaling_sharpness = 0.8
```

framegen-vulkan activates on the next launch of a matched process. If nothing matches, framegen-vulkan stays inactive. `FGVK_PROFILE` selects an exact profile and overrides automatic matching.

### Lossless Scaling availability

**framegen-vulkan Scaler and Shaders work without Lossless Scaling.** Frame Generation and LS1 require it. If framegen-vulkan reports it missing, install Lossless Scaling or set `dll` to the correct `Lossless.dll` path, then restart the game.

## Settings window

Run `~/.local/bin/fgvk` with no arguments to open the settings window. It edits the same `~/.config/framegen-vulkan/conf.toml` you can edit by hand, keeps its comments, and writes the file atomically. If the file is invalid, the window shows the problem and leaves the file unchanged. Changes made in a text editor appear in the window while it is open.

The window has these controls:

| Control | What it sets |
| --- | --- |
| Profile | Chooses the profile to edit. New, Rename and Delete manage profiles; each profile needs a unique name. |
| Games | Lists the `active_in` entries. Type an executable name and press Add, or choose Pick running… to add a program that is running now. The × button removes an entry. |
| Frame generation | `Off`, a fixed `2x` to `5x` multiplier, or `Adaptive`. Adaptive shows the target FPS (10 to 1000) and the maximum multiplier (2x to 5x). |
| Quality (flow scale) | `flow_scale` from 25% to 100%. Higher values track motion more precisely and cost more GPU time. |
| FPS limit | Optional real-FPS cap, stored as `base_fps_cap`. Turn it off to remove the manual cap. |
| Performance mode | `performance_mode` for the selected profile. |
| FP16 (faster) | `[global] allow_fp16`. Restart the game to apply. |
| Pacing correction | `[global] pacing_correction`. Restart the game to apply. |
| Lossless.dll | The `dll` path for Frame Generation and LS1 scaling. Leave it empty to remove the setting. |

Typed values apply when you press Enter or leave the field. Sliders apply when you release them, and buttons apply at once. Live settings change a running game; restart-only settings apply the next time the game starts. See [Applying changes](#applying-changes).

## Profiles

Each `[[profile]]` has a unique `name`. `active_in` lists executable names, process names, or executable-path suffixes. Copy a profile block to create a variant for another game.

### AC and battery settings

Optional `[profile.handheld]` and `[profile.docked]` tables override selected values while the device is on battery or charging. Battery selects handheld; confirmed AC power selects docked. If startup power detection fails, the base profile applies. DLL, FP16, shader, and launcher settings stay shared. Both tables must exist when enabled.

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

### Launcher exclusions

Known launchers and helpers stay inactive while their child games can match. The [shared registry](../fgvk-common/launcher_exclusions.json) owns the exact exclusions; the generic `Launcher.exe` name is not excluded globally. If only a launcher is detected, wait for gameplay and try again.

## Frame Generation

Enable `frame_generation_provisioned` before launching to allow Frame Generation in that process. `frame_generation_enabled` is live: `false` pauses generation, and `true` resumes it.

| Setting | What it does |
| --- | --- |
| `multiplier` | Fixed output multiplier, 2–5. Start at 2. |
| `adaptive = true` | Varies generation toward `target_fps` (10–1000), up to `adaptive_max_multiplier` (2–5). |
| `adaptive_target_refresh_rate` | Uses Gamescope's refresh rate as the Adaptive target; `target_fps` applies when no feedback is available. |
| `adaptive_auto_base_fps_cap` | Steady Base Cap: initially caps real FPS at half the target for even cadence; may reduce responsiveness. |
| `adaptive_fractional_real_frame_priority` | Real-frame priority for Fractional Adaptive, from low to very high. Automatic adds no priority cap. |
| `adaptive_stable_cadence` | Favours consistent delivery; compare on and off for responsiveness. |
| `base_fps_cap` | Limits real FPS. Off adds no manual cap; Steady or explicit Fractional caps take precedence. |
| `gamescope_vrr_mode` | `follow-steam`, `on`, or `off` for Gamescope VRR during play, when the display supports it. |

With Fractional Adaptive and automatic real-frame priority, sustained native FPS can exceed `target_fps` without generation when `base_fps_cap` is off or at least the target. For example, a 60 FPS target on a 90 Hz display allows native 75 FPS gameplay with the cap off or set to 90; a manual cap still limits faster sources.

For games that switch between 30 FPS cutscenes and 60 FPS gameplay on a 60 Hz display, try Fractional Adaptive with a 2x ceiling, a 60 FPS target, and `dynamic_cadence_recovery = true`. Recovery clears base caps and selects automatic real-frame priority; keep those settings to let gameplay reach its native rate. Its checks can briefly lower output FPS or hitch. `frame_generation_refresh_threshold` checks display Hz, not the game's real FPS.

## Spatial scaling

Enable `scaling_enabled` before launching. Set Steam's game resolution to the display maximum, then choose a lower in-game resolution. Gamescope is recommended; try different display modes if needed.

| `scaling_method` | Use it for |
| --- | --- |
| `native` | Simple model-free reconstruction |
| `fgvk` | Open framegen-vulkan Scaler with anti-ringing and sharpening |
| `ls1` | Highest-quality LS1 scaling; requires Lossless Scaling |
| `ls1-performance` | Lower-cost LS1 scaling; requires Lossless Scaling |

`scaling_factor` targets 1.0–2.0× output dimensions, within display limits; `scaling_sharpness` adjusts sharpening except with `native`. `scaling_supersampling` can improve quality on supported Gamescope surfaces, with higher GPU and memory use. LS1 failures fall back to the framegen-vulkan Scaler.

For 1920×1080 output at 1.5×, aim for a 1280×720 input. Raising the factor without lowering the in-game resolution increases GPU and memory cost. Check source and presentation sizes in [diagnostics](COLLECT_DIAGNOSTICS.md); see [Scaling](SCALING.md) for supported display setups.

## Performance and device selection

| Setting | What it does |
| --- | --- |
| `ultra_performance = true` (restart) | Uses 70% Flow Scale, the lighter model, and `ls1-performance` when scaling is enabled. |
| `flow_scale` | 25–100% motion-estimation resolution; lower saves GPU work at a quality cost. Ultra Performance fixes it at 70%. |
| `gpu_headroom_percent` | Live, Frame Generation-only reserve that reduces real FPS to keep spare GPU capacity; see below. Off by default. |
| `performance_mode = true` | Uses the lighter Frame Generation model, which reduces GPU work with potentially more artifacts. |
| `allow_fp16` (restart, `[global]`) | FP16 for supported Frame Generation models and the framegen-vulkan Scaler; `false` uses FP32. LS1 always uses FP32. |
| `pacing_correction` (restart, `[global]`) | On by default. Measures when each output gets the GPU after its present and moves real frames so generated frames sit midway between them. `false` keeps the uncorrected timing. |
| `dll` (restart) | Lossless.dll path override; leave unset for automatic detection. |
| `gpu` (restart) | Optional GPU name, vendor/device ID, or PCI bus ID. Multi-GPU Frame Generation is unsupported. |

### GPU Headroom

Set `gpu_headroom_percent` to 5, 10, 15, or 20 to reserve that share of GPU capacity. The reserve targets roughly 90% utilization at 10%, including the compositor and other applications. It applies live while Frame Generation is active. Setting it to 0 removes the additional cap and stops utilization sampling.

framegen-vulkan smooths readings and gradually reduces real FPS when usage stays high. Adaptive can use more generated frames within its qualified multiplier ceiling; Fixed keeps its multiplier, so output FPS can fall too. Adaptive is preferable when you want to regain real FPS as scenes become lighter. Brief spikes can still reach 100%, and the reserve is not a power or temperature limit.

Supported AMD telemetry must be readable for the game's GPU; Gamescope is optional. Missing readings restore ordinary pacing. Existing caps and a minimum real-frame rate limit how much load framegen-vulkan can remove. GPU Headroom does not independently control scaling or shaders, and it is mutually exclusive with `dynamic_cadence_recovery`. See the [setting reference](CONFIGURATION-REFERENCE.md#gpu-headroom) for floors and compatibility limits.

## Shaders

Shaders use bundled vkBasalt. Add `ENABLE_VKBASALT=1` to the launch option, then select effects in a vkBasalt configuration file referenced by `VKBASALT_CONFIG_FILE`. Effects run in the order listed in `effects`, separated by colons; more effects cost more GPU time. The bundled effect sources are in `engine/vkbasalt-shaders/`, and their provenance is in [third-party notices](../../THIRD_PARTY_NOTICES.md).

```text
ENABLE_VKBASALT=1 VKBASALT_CONFIG_FILE="$HOME/.config/vkBasalt/game-name.conf" ~/.local/bin/fgvk %command%
```

```ini
effects = makoClarity:smaa
```

- **Sharpening:** list `cas` or `dls` in `effects`, then set `casSharpness` or `dlsSharpness`. `dlsDenoise` limits sharpening of grain and noise.
- **Anti-aliasing:** list `fxaa` or `smaa` in `effects`.
- **HDR Look (SDR):** `makoHDRLook` changes appearance without enabling HDR.
- **Crop & Stretch:** `makoCropStretch` has four crop parameters, each limited to 49%, declared in [`CropStretch.fx`](../vkbasalt-shaders/CropStretch.fx). Cropping affects the whole game image, including HUD elements and subtitles. Place it before other effects when those effects should use the stretched image.

Only framegen-vulkan's private bundled vkBasalt is used; a system installation is ignored. Adding a custom effect requires a vkBasalt-compatible ReShade `.fx` file and its includes and textures reachable by the game; see [optional graphics integrations](LAYER-CHAINING.md) for the supported setup and limits. Restart the game after changing shader activation or configuration.

## Compatibility

| Setting | What it does |
| --- | --- |
| `frame_generation_refresh_threshold` | Pauses generation and Adaptive caps at or below the chosen Gamescope refresh; a manual `base_fps_cap` remains. |
| `dynamic_cadence_recovery` | Off by default. Rechecks games that change native FPS by briefly reducing generated work; these checks can lower output FPS or cause hitches under sustained GPU load. Clears base caps and GPU Headroom, and resets real-frame priority. |
| `dynamic_cadence_probe_interval_seconds` | 0.1–3 seconds between recovery checks; default 2. Shorter intervals react sooner but can hitch more often. |
| `swapchain_image_count_compatibility` (restart) | May help games that fail to start, with less generated-frame headroom. |

Keep the defaults unless a game needs a change.

## Applying changes

| Timing | Common settings |
| --- | --- |
| Restart | Enabling `frame_generation_provisioned` or `scaling_enabled`; `dll`; `allow_fp16`; `pacing_correction`; `gpu`; `ultra_performance`; `swapchain_image_count_compatibility`; launcher compatibility settings |
| Live | `frame_generation_enabled`; most Frame Generation timing and cap settings; `scaling_method` and `scaling_sharpness`; `flow_scale`; `performance_mode`; `gpu_headroom_percent`; `hdr_quality` |
| May wait for resolution change or restart | `scaling_factor`, `scaling_supersampling`, and changes needing more Frame Generation capacity |

Some live changes rebuild resources and may briefly hitch. See [Runtime transitions](RUNTIME-TRANSITIONS.md) for details.

## Launch settings

For native Steam and Proton games, use the launch option below. Flatpak applications need [Flatpak preparation](FLATPAK-GUIDE.md). The Arch package uses `/usr/bin/fgvk`. For a terminal launch, replace `%command%` with the executable and arguments.

```text
~/.local/bin/fgvk %command%
```

Standalone launch compatibility settings live in `~/.config/framegen-vulkan/launcher.conf`, and apply to every `fgvk` game:

```ini
version = 1
disable_steam_overlay = 1
disable_hdr_exposure = 0
enable_zink = 0
force_alsa_audio = 0
```

- `disable_steam_overlay`: on by default; it may reduce stutter but disables Steam's in-game overlay and FPS counter and may affect Steam Input.
- `enable_zink`: runs OpenGL through Vulkan.
- `force_alsa_audio`: changes the audio path for compatibility problems.

To use a shared MangoHud layout, create `~/.config/framegen-vulkan/mangohud.conf` and enable MangoHud for a game with `MANGOHUD=1` in its launch option. Shaders and MangoHud are mutually exclusive; a manual command requesting both is rejected. Install host MangoHud and Python 3 first, and check availability with `fgvk --check-mangohud`. Flatpak games are unsupported. See the [shared MangoHud layout](LAYER-CHAINING.md#shared-mangohud-layout).

## HDR

**HDR is experimental and disabled by default.** It works with Scaling, Frame Generation and Shaders in Gaming Mode on Steam Deck OLED or an HDR-capable external display, with HDR allowed in Gamescope. The Steam Deck LCD's built-in display is SDR.

Set `disable_hdr_exposure = 0` in `launcher.conf`, restart the game, then enable HDR in its own settings. Restart after changing HDR exposure or switching between HDR and SDR displays.

`hdr_quality` defaults to `'performance'`. `'quality'` retains full precision and full motion analysis; `'balanced'` uses compact shader and scaling buffers while retaining linear-light Frame Generation and full motion analysis; `'performance'` also uses approximate HDR10 generation and coarser motion analysis. Output resolution stays unchanged. Changes apply live with a brief hitch; lower precision may add banding or motion artifacts. See the [HDR pipeline](HDR-PIPELINE.md) for details.

## Native Steam Remote Play

Native Remote Play is an opt-in override for the receiving device. Use `fgvk-remote-play status` to inspect it and `fgvk-remote-play restore-before-uninstall` before removing the Renderer. See [Renderer usage](../README.md#usage) for the complete command set and limits.

## Advanced configuration

For every TOML field, its default, and the environment variables, see the [configuration reference](CONFIGURATION-REFERENCE.md). For problems, see [troubleshooting](TROUBLESHOOTING.md) or [collect diagnostics](COLLECT_DIAGNOSTICS.md).

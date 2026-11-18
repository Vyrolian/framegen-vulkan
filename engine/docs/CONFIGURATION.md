# Configuration

framegen-vulkan reads its profiles from `~/.config/framegen-vulkan/conf.toml`. Edit the file in any text editor. framegen-vulkan reads it when a game starts, so most changes apply to the next launch; settings marked live in [Applying changes](#applying-changes) can change while the game runs. Check each edit with `~/.local/bin/fgvk-cli validate`. The [configuration reference](CONFIGURATION-REFERENCE.md) lists every field, default, and environment variable.

## Quick start

1. Create `~/.config/framegen-vulkan/conf.toml` from the example below.
2. Set `active_in` to the game's executable name, such as `Game.exe`. Match the game's process, not its launcher, Steam title, or ROM filename.
3. Enable Frame Generation for that profile.
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

```

framegen-vulkan activates on the next launch of a matched process. If nothing matches, framegen-vulkan stays inactive. `FGVK_PROFILE` selects an exact profile and overrides automatic matching.

### Lossless Scaling availability

**Frame Generation requires Lossless Scaling's `Lossless.dll`.** If framegen-vulkan reports it missing, install Lossless Scaling or set `dll` to the correct `Lossless.dll` path, then restart the game.

## Settings window

Run `~/.local/bin/fgvk` with no arguments to open the settings window. A second settings invocation exits if the window is already open. You can still launch games with `fgvk <command>`. The window edits `~/.config/framegen-vulkan/conf.toml`, keeps its comments, and writes the file atomically. If the file is invalid, the window shows the problem and leaves the file unchanged. Changes made in a text editor appear in the window while it is open.

The window has these controls:

| Control | What it sets |
| --- | --- |
| Profile | Chooses the profile to edit. New, Rename and Delete manage profiles; each profile needs a unique name. |
| Games | Lists the `active_in` entries. Type an executable name and press Add, or choose Pick running… to add a program that is running now. The × button removes an entry. |
| Frame generation | `Off` or a fixed `2x` to `5x` multiplier. |
| Quality (flow scale) | `flow_scale` from 25% to 100%. Higher values track motion more precisely and cost more GPU time. |
| FPS limit | Optional real-FPS cap, stored as `base_fps_cap`. Turn it off to remove the manual cap. |
| Low latency | `low_latency` for the selected profile. On by default; starts each capped frame just in time. Applies live. |
| Batch hold | `batch_hold` for the selected profile. On by default; each output batch starts about 2 ms after its frames are ready, with Low latency on or off. Applies live. |
| Performance mode | `performance_mode` for the selected profile. |
| FP16 (faster) | `[global] allow_fp16`. Restart the game to apply. |
| Pacing correction | `[global] pacing_correction`. Restart the game to apply. |
| Lossless.dll | The `dll` path for Frame Generation. Leave it empty to remove the setting. |

Typed values apply when you press Enter or leave the field. Sliders apply when you release them, and buttons apply at once. Live settings change a running game; restart-only settings apply the next time the game starts. See [Applying changes](#applying-changes).

## Themes

The Appearance section at the bottom of the settings window chooses the colours. A change applies at once and is saved to `~/.config/framegen-vulkan/ui.toml` as `theme = "…"` and `catppuccin_accent = "…"`. That file is separate from `conf.toml`, which the layer reads, so the theme choice never reaches the layer. Built-in themes are the four Catppuccin flavours (Mocha is the default; Latte, Frappé and Macchiato are the others), Dracula, Nord, Gruvbox Dark, Tokyo Night, Solarized Dark and Solarized Light. The built-in palettes follow each theme's public colour specification.

The Accent list appears while a Catppuccin theme is selected and its file does not set an accent. It offers the fourteen Catppuccin accents, and the default is Mauve. A theme file's `catppuccin_accent` or `accent` key takes precedence over the list. Error and success colours stay red and green whatever accent is chosen.

Custom themes are `*.toml` files in `~/.config/framegen-vulkan/themes/`, or in `$XDG_CONFIG_HOME/framegen-vulkan/themes/` when that variable is set. Press Reload themes after adding or editing one. The example in `~/.local/share/doc/framegen-vulkan/themes/example.toml` (or `/usr/share/doc/framegen-vulkan-bin/themes/example.toml` from the Arch package) lists every key. `name` is required. `base` names the built-in theme whose keys the file inherits, and defaults to Catppuccin Mocha. Colours are `"#rrggbb"` or `"#rrggbbaa"`. The key `background` sets the window background, which the palette calls `base`. A file that cannot be read or parsed is skipped, and the window shows a note that names the file and the line. An unknown key is ignored with a note.

## Profiles

Each `[[profile]]` has a unique `name`. `active_in` lists executable names, process names, or executable-path suffixes. Copy a profile block to create a variant for another game.

### Launcher exclusions

Known launchers and helpers stay inactive while their child games can match. The [shared registry](../fgvk-common/launcher_exclusions.json) owns the exact exclusions; the generic `Launcher.exe` name is not excluded globally. If only a launcher is detected, wait for gameplay and try again.

## Frame Generation

Enable `frame_generation_provisioned` before launching to allow Frame Generation in that process. `frame_generation_enabled` is live: `false` pauses generation, and `true` resumes it.

| Setting | What it does |
| --- | --- |
| `multiplier` | Fixed output multiplier, 2–5. Start at 2. |
| `base_fps_cap` | Limits real FPS. Off adds no manual cap. |
| `low_latency` | On by default. With frame generation at a cap, the game starts each frame later, just in time, instead of queueing up to three frames ahead. It also holds the game's next frame until the GPU is about to finish the previous one, like Reflex. Off keeps the release at the cap deadline. How each output batch starts is set by `batch_hold`. With the game's Reflex on, Reflex holds the game; low latency only starts frame generation just in time. Live. The latency effect is not yet measured in a game run. |
| `batch_hold` | On by default. Each output batch starts about 2 ms after its frames are ready, phase-locked to the batch cadence, so GPU timing wobble does not show as uneven frames. With low latency off, a batch starts at its schedule or its held readiness, whichever is later. Off starts each batch at its readiness (or at its schedule, when that is later): up to about 2 ms less latency, slightly less even. Live. |

Older files can still contain `adaptive`, `adaptive_max_multiplier`, `adaptive_auto_base_fps_cap`, `adaptive_fractional_real_frame_priority`, `adaptive_target_refresh_rate`, `target_fps`, `dynamic_cadence_recovery`, and `dynamic_cadence_probe_interval_seconds`. These keys are accepted and ignored: a profile with `adaptive = true` runs Fixed with its `multiplier`, and framegen-vulkan does not write them.

## Performance and device selection

| Setting | What it does |
| --- | --- |
| `ultra_performance = true` (restart) | Uses 70% Flow Scale and the lighter model. |
| `flow_scale` | 25–100% motion-estimation resolution; lower saves GPU work at a quality cost. Ultra Performance fixes it at 70%. |
| `performance_mode = true` | Uses the lighter Frame Generation model, which reduces GPU work with potentially more artifacts. |
| `allow_fp16` (restart, `[global]`) | FP16 for supported Frame Generation models; `false` uses FP32. |
| `pacing_correction` (restart, `[global]`) | On by default. Measures when each output gets the GPU after its present and moves each generated frame onto its own ideal point between its two reals, at every multiplier. Real frames are never moved. `FGVK_REAL_OFFSET_US=auto` turns it on; any other value turns it off. `false` keeps the uncorrected timing. |
| `dll` (restart) | Lossless.dll path override; leave unset for automatic detection. |
| `gpu` (restart) | Optional GPU name, vendor/device ID, or PCI bus ID. Multi-GPU Frame Generation is unsupported. |

## Compatibility

| Setting | What it does |
| --- | --- |
| `swapchain_image_count_compatibility` (restart) | May help games that fail to start, with less generated-frame headroom. |

Keep the defaults unless a game needs a change.

HDR10/PQ swapchains (A2B10G10R10 or A2R10G10B10) and linear scRGB swapchains (R16G16B16A16_SFLOAT) get frame generation in linear light; other HDR transfer functions pass through without it. Under Wine on native Wayland, Wine reports the same HDR pairs as `PASS_THROUGH` and sets the HDR description on the surface itself, so those swapchains get the same frame generation. `disable_hdr_exposure` in old launcher files is ignored.

`[profile.handheld]` and `[profile.docked]` tables in old files are accepted and ignored.

`gpu_headroom_percent` in old files is accepted and ignored.

## Applying changes

| Timing | Common settings |
| --- | --- |
| Restart | Enabling `frame_generation_provisioned`; `dll`; `allow_fp16`; `pacing_correction`; `gpu`; `ultra_performance`; `swapchain_image_count_compatibility`; launcher compatibility settings |
| Live | `frame_generation_enabled`; most Frame Generation timing and cap settings; `flow_scale`; `performance_mode`; `low_latency` |
| May wait for resolution change or restart | Changes needing more Frame Generation capacity |

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
enable_zink = 0
force_alsa_audio = 0
log_pacing = 0
log_trace = 0
log_wayland = 0
log_gpu_cost = 0
```

- `disable_steam_overlay`: on by default; it may reduce stutter but disables Steam's in-game overlay and FPS counter and may affect Steam Input.
- `enable_zink`: runs OpenGL through Vulkan.
- `force_alsa_audio`: changes the audio path for compatibility problems.
- `log_pacing`: light pacing summaries; exports `FGVK_PACING_PROFILE=1`.
- `log_trace`: detailed trace of every present, acquire and wait, with a larger file; exports `FGVK_PACING_PROFILE_TRACE=1`.
- `log_wayland`: compositor presentation feedback; exports `FGVK_PACING_PROFILE_WAYLAND=1`.
- `log_gpu_cost`: frame-generation GPU time, which does not change pacing; exports `FGVK_PACING_PROFILE_GPU_SPLIT=1` and `FGVK_PACING_PROFILE=1`.

The logging switches default to 0 and take only 0 or 1. The Logging section of the `fgvk` settings window writes them. A launcher exports its variable only when the launch options do not already set that variable, so a value typed there wins, including `=0`. Logs are written under `${TMPDIR:-/tmp}` and apply the next time a game starts.

The retired `log_present` key from earlier builds is still accepted (0 or 1, no duplicates) and ignored; framegen-vulkan does not write it.

Enable host MangoHud with `MANGOHUD=1` in the launch option after installing MangoHud and Python 3. MangoHud reads its own configuration. See [layer ordering](LAYER-CHAINING.md#standalone-framegen-vulkan-with-mangohud).

## Advanced configuration

For every TOML field, its default, and the environment variables, see the [configuration reference](CONFIGURATION-REFERENCE.md). For problems, see [troubleshooting](TROUBLESHOOTING.md) or [collect diagnostics](COLLECT_DIAGNOSTICS.md).

# Optional graphics integrations

Use the launch options below. Restart the game after changing layer activation.

## Standalone framegen-vulkan with vkBasalt

vkBasalt is bundled; no separate installation is needed. Set the effects in a vkBasalt configuration file as described in [Shaders](CONFIGURATION.md#shaders).

For native or Proton activation, add this to the game's Steam launch options:

```text
ENABLE_VKBASALT=1 ~/.local/bin/fgvk-launch %command%
```

To choose an advanced vkBasalt configuration:

```text
ENABLE_VKBASALT=1 VKBASALT_CONFIG_FILE="$HOME/.config/vkBasalt/game-name.conf" ~/.local/bin/fgvk-launch %command%
```

Flatpak apps require [Flatpak shader setup](FLATPAK-GUIDE.md#optional-private-vkbasalt-chain). To disable Shaders, remove `ENABLE_VKBASALT=1` from the launch option:

```text
~/.local/bin/fgvk-launch %command%
```

## Standalone framegen-vulkan with MangoHud

Install host MangoHud and Python 3 first. For native or Proton activation, add `MANGOHUD=1` to the launch option:

```text
MANGOHUD=1 ~/.local/bin/fgvk-launch %command%
```

MangoHud and Shaders are mutually exclusive: a command requesting both is rejected. Host MangoHud is not enabled inside Flatpak or for a direct `flatpak run` command.

`fgvk-launch --check-mangohud` reports availability as JSON without writing files. Native launch checks only the named `MangoHud.x86_64.json` and `MangoHud.x86.json` manifests in the standard XDG configuration/data locations. It verifies each layer's identity, architecture, library path and activation gates, resolves relative library paths, and stages only validated manifests under `$XDG_CACHE_HOME/framegen-vulkan/mangohud` (normally `~/.cache/framegen-vulkan/mangohud`). The directory is private to the user; concurrent launches are serialized during staging and unchanged files are not rewritten. Missing or invalid installations leave the game running without MangoHud and produce a diagnostic. The launcher never adds the host's full implicit-layer directory.

## Shared MangoHud layout

Create the configuration file manually; enabling MangoHud does not create it.

To use one custom MangoHud layout across your games, create `~/.config/framegen-vulkan/mangohud.conf` with ordinary MangoHud options, for example:

```ini
fps
frametime
position=top-right
```

Add `MANGOHUD=1` to the launch option of each game that should use it. The launcher respects `XDG_CONFIG_HOME` for the shared file location. On each supported host launch, framegen-vulkan selects this file if it is a readable regular file. Creating or removing it takes effect on the next game launch without resaving profiles. Missing, unreadable, or non-file paths leave the inherited configuration unchanged. framegen-vulkan does not create, edit, or execute the file.

The shared file replaces any inherited `MANGOHUD_CONFIGFILE`, including Gaming Mode's Gamescope preset file. This changes only the launched game's MangoHud configuration; Steam's separate performance overlay remains under Steam's control. Existing `MANGOHUD_CONFIG` values retain MangoHud's precedence: without `read_cfg`, that variable can bypass file loading; with `read_cfg`, its options override file values. For a per-game file exception, apply the override after framegen-vulkan's wrapper:

```text
MANGOHUD=1 ~/.local/bin/fgvk-launch env MANGOHUD_CONFIGFILE=/absolute/path/to/game.conf %command%
```

Use this form for each game that needs its own file. Quote paths containing spaces. The shared file does not enable MangoHud by itself or add Flatpak or simultaneous Shader support. Choosing a layout adds no framegen-vulkan per-frame work. MangoHud's own cost depends on its metrics, and its FPS counter is not a guaranteed measurement of pre-generation game FPS; that depends on the active layer chain.

## Support boundaries

| Integration | Support |
| --- | --- |
| Steam Vulkan FPS overlay | Desktop Mode native/Proton launches with **Disable Steam Overlay** off |
| MangoHud | Host games; cannot run with framegen-vulkan Shaders |
| vkBasalt | Bundled for native/Proton and prepared Flatpak apps |
| XR Gaming / Breezy | Compatibility unvalidated |
| ReShade / OptiScaler through Proton | Game-specific; disable their Frame Generation |
| RenderDoc | Developer diagnostics only |
| OBS Vulkan Capture / other Vulkan layers | Unsupported |
| Other Frame Generation layers | Do not combine with framegen-vulkan |

## XR Gaming / Breezy

Gamescope effects and Vulkan-only mode use different paths; framegen-vulkan's WSI toggle does not select between them. Compatibility with glasses is unvalidated.

For Anchor/Follow lag, compare framegen-vulkan alone, XR alone, and both. Record the XR effect off for 30 seconds, on for 30 seconds, then off again, keeping the scene and settings unchanged. Send the actual Breezy runtime archive with [framegen-vulkan diagnostics](../../COLLECT_DIAGNOSTICS.md).

## Game-local Proton integrations

For Windows ReShade or OptiScaler, keep the integration's documented DLL overrides and use your normal framegen-vulkan launch command. Disable its Frame Generation while framegen-vulkan handles generation. Compatibility varies by game.

## Troubleshooting and rollback

Disable the optional integration and restart if startup, effects, or pacing worsen. If the problem remains, follow [troubleshooting](TROUBLESHOOTING.md).

For implementation details, see [WSI isolation](WSI-ISOLATION.md).

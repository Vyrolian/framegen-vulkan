# Optional graphics integrations

Use the launch options below. Restart the game after changing layer activation.

## vkBasalt

fgvk blocks a system vkBasalt in its launches.

## Standalone framegen-vulkan with MangoHud

Install host MangoHud and Python 3 first. For native or Proton activation, add `MANGOHUD=1` to the launch option:

```text
MANGOHUD=1 ~/.local/bin/fgvk %command%
```

The launcher places MangoHud above framegen-vulkan, so it sees only the application's own presents: its FPS counter and frame-time graph show source frames, not generated output, and its overlay is part of the image framegen-vulkan interpolates. Below framegen-vulkan, MangoHud drew on every output inside the private present queue lock, which delayed outputs by about 1.3 ms and lengthened the game's acquires on a busy GPU. Host MangoHud is not enabled inside Flatpak or for a direct `flatpak run` command.

Native launch checks only the named `MangoHud.x86_64.json` and `MangoHud.x86.json` manifests in the standard XDG configuration/data locations. It verifies each layer's identity, architecture, library path and activation gates, resolves relative library paths, and stages only validated manifests under `$XDG_CACHE_HOME/framegen-vulkan/mangohud` (normally `~/.cache/framegen-vulkan/mangohud`). The directory is private to the user; concurrent launches are serialized during staging and unchanged files are not rewritten. Missing or invalid installations leave the game running without MangoHud and produce a diagnostic. The launcher never adds the host's full implicit-layer directory.

MangoHud reads its own configuration, normally `~/.config/MangoHud/MangoHud.conf`. Inherited `MANGOHUD_CONFIGFILE` and `MANGOHUD_CONFIG` values pass through unchanged.

## Support boundaries

| Integration | Support |
| --- | --- |
| Steam Vulkan FPS overlay | Desktop Mode native/Proton launches with **Disable Steam Overlay** off |
| MangoHud | Host games |
| XR Gaming / Breezy | Compatibility unvalidated |
| ReShade / OptiScaler through Proton | Game-specific; disable their Frame Generation |
| RenderDoc | Developer diagnostics only |
| OBS Vulkan Capture / other Vulkan layers | Unsupported |
| Other Frame Generation layers | Do not combine with framegen-vulkan |

## XR Gaming / Breezy

Gamescope effects and Vulkan-only mode use different paths. framegen-vulkan runs in a Gamescope session by default as an experimental path, and `FGVK_GAMESCOPE=0` turns it off. Compatibility with glasses is unvalidated.

For Anchor/Follow lag, compare framegen-vulkan alone, XR alone, and both. Record the XR effect off for 30 seconds, on for 30 seconds, then off again, keeping the scene and settings unchanged. Send the actual Breezy runtime archive with [framegen-vulkan diagnostics](../../COLLECT_DIAGNOSTICS.md).

## Game-local Proton integrations

For Windows ReShade or OptiScaler, keep the integration's documented DLL overrides and use your normal framegen-vulkan launch command. Disable its Frame Generation while framegen-vulkan handles generation. Compatibility varies by game.

## Troubleshooting and rollback

Disable the optional integration and restart if startup, effects, or pacing worsen. If the problem remains, follow [troubleshooting](TROUBLESHOOTING.md).

For implementation details, see [WSI isolation](WSI-ISOLATION.md).

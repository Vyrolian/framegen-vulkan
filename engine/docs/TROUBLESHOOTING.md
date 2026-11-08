# Troubleshooting

## framegen-vulkan does not load

1. Use Vulkan, including DXVK/VKD3D-Proton for Windows games. Select Vulkan in emulators; OpenGL games may need [Zink](CONFIGURATION.md#launch-settings).
2. Copy the complete launch option from **framegen-vulkan Configuration** into Steam and restart. The basic native/Proton option is:

    ```text
    ~/.local/bin/fgvk-launch %command%
    ```

3. For terminal launches, replace `%command%` with the executable and arguments. Flatpak apps need [Flatpak preparation](FLATPAK-GUIDE.md).
4. Check that `active_in` in the profile matches the game's executable. A saved profile alone does not activate framegen-vulkan.

For native loader problems, run:

```bash
~/.local/bin/fgvk-launch vulkaninfo | grep -i VK_LAYER_FGVK_render
```

Install Vulkan-tools if `vulkaninfo` is missing. If no layer appears, check the installation and launch environment. For 32-bit games, ensure the matching 32-bit layer and manifest are installed.

## The layer loads but a feature does not start

- Enable the feature before launching and restart for options marked **Restart**.
- For Frame Generation or LS1, install Lossless Scaling's default Steam version and check **Lossless.dll Path**. framegen-vulkan Scaler and Shaders work without it.
- Clear an unnecessary **GPU** override; Frame Generation must use the game's GPU.
- For scaling, lower the in-game resolution and check the input/output sizes in diagnostics.
- Compare V-Sync on/off and test features separately.

To check configuration errors:

```bash
fgvk-cli validate --config ~/.config/framegen-vulkan/conf.toml
```

## Gamescope WSI, overlays, and HDR

Use [supported graphics integrations](LAYER-CHAINING.md). HDR is experimental and disabled by default. To use it on a supported Gamescope display, set `disable_hdr_exposure = 0` in `launcher.conf`, restart the game, then enable HDR in the game. Keep full Gamescope WSI off; see [HDR setup](CONFIGURATION.md#hdr).

## Collect diagnostics

If the problem remains, [collect a diagnostics report](COLLECT_DIAGNOSTICS.md) and submit it privately through the linked form. Include the game, runtime, settings, and what went wrong; keep logs out of public issues.

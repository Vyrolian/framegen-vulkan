# Troubleshooting

## framegen-vulkan does not load

1. Use Vulkan, including DXVK/VKD3D-Proton for Windows games. Select Vulkan in emulators; OpenGL games may need [Zink](CONFIGURATION.md#launch-settings).
2. Copy the launch option into Steam and restart. The basic native/Proton option is:

    ```text
    ~/.local/bin/fgvk %command%
    ```

3. For terminal launches, replace `%command%` with the executable and arguments. Flatpak apps need [Flatpak preparation](FLATPAK-GUIDE.md).
4. Check that `active_in` in the profile matches the game's executable. A saved profile alone does not activate framegen-vulkan.

For native loader problems, run:

```bash
~/.local/bin/fgvk vulkaninfo | grep -i VK_LAYER_FGVK_render
```

Install Vulkan-tools if `vulkaninfo` is missing. If no layer appears, check the installation and launch environment. For 32-bit games, ensure the matching 32-bit layer and manifest are installed.

## `fgvk` cannot start the game

`fgvk <command>` replaces itself with the private launch helper at `libexec/framegen-vulkan/fgvk-launch`, next to the `fgvk` executable. If it prints `launch helper not found` and exits with status 127, that install is missing the helper: reinstall framegen-vulkan. For a build outside an install, set `FGVK_LAUNCH_HELPER` to the helper path, for example `engine/scripts/fgvk-launch`. Exit status 126 means the helper exists but is not executable.

If the loader reports `GLIBC_2.43 not found` for `fgvk`, the archive was built against a newer glibc than this system has. Use the Arch package or a build from source on that system. `package-local.sh` warns when its `fgvk` needs a newer glibc than Ubuntu 22.04 provides.

## The layer loads but a feature does not start

- Enable the feature before launching and restart for options marked **Restart**.
- For Frame Generation, install Lossless Scaling's default Steam version and check **Lossless.dll Path**.
- Clear an unnecessary **GPU** override; Frame Generation must use the game's GPU.
- Compare V-Sync on/off and test features separately.
- Zink (`enable_zink`, OpenGL through Vulkan) can run frame generation on its 8-bit swapchains, logged as `mode=passthrough-sdr8`.

To check configuration errors:

```bash
fgvk cli validate --config ~/.config/framegen-vulkan/conf.toml
```

## Gamescope sessions

Gamescope sessions run frame generation by default. This path is experimental and not a supported setup. HDR under Gamescope is not recovered yet: Gamescope's WSI gives framegen-vulkan an sRGB swapchain, so HDR frames are blended as SDR. `FGVK_GAMESCOPE=0` starts the game without frame generation. Use [supported graphics integrations](LAYER-CHAINING.md) for other setups.

## Collect diagnostics

If the problem remains, [collect a diagnostics report](COLLECT_DIAGNOSTICS.md) and submit it privately through the linked form. Include the game, runtime, settings, and what went wrong; keep logs out of public issues.

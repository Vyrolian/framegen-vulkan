## What's new in framegen-vulkan v1.3.0

Frame generation now follows the game's own present timing (VK_EXT_present_timing), which vkd3d-proton uses for VSync at a fraction of the refresh rate (for example VSync 60 on a 240 Hz screen).

- **Even generated frames under partial VSync:** when the game asks for each frame to stay up several refreshes, framegen-vulkan now splits that time across the generated frames instead of showing them at once and holding the real frame. VSync 60 at 2× on 240 Hz now shows 2 refreshes per frame instead of a 1/3 split. With VRR this is clean; on a fixed refresh rate some uneven frames remain, because the NVIDIA driver on Wayland rounds the game's own timestamps up by a refresh. That also happens without framegen-vulkan.
- **Present timing feedback with frame generation:** vkd3d-proton's timing feedback no longer freezes when frame generation is on, so its own pacing and catch-up after stalls keep working.
- **Pacing captures:** captures now record the timing target sent with each frame and the driver's reported presentation times.

### Install

Download `framegen-vulkan-v1.3.0.zip` from this release and extract it.

For native Steam and other Linux apps, install or upgrade from the `native` folder:

```bash
cd framegen-vulkan-v1.3.0/native
./install
```

For Flatpak apps, run the extension installer from the `flatpak` folder, then configure each app using the [Flatpak setup guide](https://github.com/Vyrolian/framegen-vulkan/blob/v1.3.0/engine/docs/FLATPAK-GUIDE.md):

```bash
cd framegen-vulkan-v1.3.0/flatpak
./install-flatpak
```

### Uninstall

Native: close games using framegen-vulkan and run:

```bash
~/.local/bin/fgvk uninstall
```

The uninstaller asks separately whether to delete your profiles, settings, and diagnostics.

Flatpak: to uninstall the extension for runtime 26.08, close the apps using it and run:

```bash
flatpak uninstall --user org.freedesktop.Platform.VulkanLayer.fgvkrender//26.08
```

Repeat for each installed runtime branch. App overrides and profiles remain. [Flatpak removal and override cleanup](https://github.com/Vyrolian/framegen-vulkan/blob/v1.3.0/engine/docs/FLATPAK-GUIDE.md#uninstall-the-flatpak-extension)

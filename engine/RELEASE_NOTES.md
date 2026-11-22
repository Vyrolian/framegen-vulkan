## What's new in framegen-vulkan v1.2.1

A bug-fix release.

- **No hang when closing a game:** games that close their window while the compositor holds on to frames (for example Minecraft on Zink) no longer hang at exit. framegen-vulkan now waits at most one second for those frames, then releases the swapchain itself.

### Install

Download `framegen-vulkan-v1.2.1.zip` from this release and extract it.

For native Steam and other Linux apps, install or upgrade from the `native` folder:

```bash
cd framegen-vulkan-v1.2.1/native
./install
```

For Flatpak apps, run the extension installer from the `flatpak` folder, then configure each app using the [Flatpak setup guide](https://github.com/Vyrolian/framegen-vulkan/blob/v1.2.1/engine/docs/FLATPAK-GUIDE.md):

```bash
cd framegen-vulkan-v1.2.1/flatpak
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

Repeat for each installed runtime branch. App overrides and profiles remain. [Flatpak removal and override cleanup](https://github.com/Vyrolian/framegen-vulkan/blob/v1.2.1/engine/docs/FLATPAK-GUIDE.md#uninstall-the-flatpak-extension)

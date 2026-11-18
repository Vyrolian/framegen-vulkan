## What's new in framegen-vulkan v1.1.0

`fgvk` opens Settings, launches games, and runs the diagnostic commands through `fgvk cli`. Upgrading removes the old `fgvk-cli` and `fgvk-installer` commands when they match files owned by the previous package.

### Native Linux

Download and extract the Linux archive into its own folder. To install or upgrade, run from that folder:

```bash
./install
```

To uninstall, close games using framegen-vulkan and run:

```bash
~/.local/bin/fgvk uninstall
```

The uninstaller asks separately whether to delete your profiles, settings, and diagnostics.

### Flatpak

Download and extract the Flatpak archive into its own folder, then run:

```bash
./install-flatpak
```

Configure each app using the [Flatpak setup guide](https://github.com/Vyrolian/framegen-vulkan/blob/v1.1.0/engine/docs/FLATPAK-GUIDE.md).

To uninstall the extension for runtime 26.08, close the apps using it and run:

```bash
flatpak uninstall --user org.freedesktop.Platform.VulkanLayer.fgvkrender//26.08
```

Repeat for each installed runtime branch. App overrides and profiles remain. [Flatpak removal and override cleanup](https://github.com/Vyrolian/framegen-vulkan/blob/v1.1.0/engine/docs/FLATPAK-GUIDE.md#uninstall-the-flatpak-extension)

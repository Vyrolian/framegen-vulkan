## What's new in framegen-vulkan v1.4.0

The settings window now owns the Lossless.dll path and the default profile, and framegen-vulkan no longer searches for the DLL when a game starts.

- **Lossless.dll is chosen, not searched:** games use only the configured `dll` path. Without one, frame generation stays off and the log says that no path is set. Existing configurations with a path are unchanged.
- **Browse and Auto-locate:** next to the Lossless.dll field, Browse opens the desktop file picker and Auto-locate searches the Steam libraries once, including libraries on other drives listed in `libraryfolders.vdf`.
- **System clipboard:** copy and paste in the settings window now use the desktop clipboard.
- **Always one default profile:** a new configuration starts with **Main profile** as the default. **Set as default** moves the default to another profile, the default is marked `(default)` in the profile list, and it cannot be deleted until another profile is the default.

### Install

Download `framegen-vulkan-v1.4.0.zip` from this release and extract it.

For native Steam and other Linux apps, install or upgrade from the `native` folder:

```bash
cd framegen-vulkan-v1.4.0/native
./install
```

For Flatpak apps, run the extension installer from the `flatpak` folder, then configure each app using the [Flatpak setup guide](https://github.com/Vyrolian/framegen-vulkan/blob/v1.4.0/engine/docs/FLATPAK-GUIDE.md):

```bash
cd framegen-vulkan-v1.4.0/flatpak
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

Repeat for each installed runtime branch. App overrides and profiles remain. [Flatpak removal and override cleanup](https://github.com/Vyrolian/framegen-vulkan/blob/v1.4.0/engine/docs/FLATPAK-GUIDE.md#uninstall-the-flatpak-extension)

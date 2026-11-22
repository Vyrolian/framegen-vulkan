## What's new in framegen-vulkan v1.2.0

Frame generation now runs on the game's own GPU queue and hands frames to the screen inside the game's present call, which raises the base frame rate at 2× to 4× and keeps pacing tight on Wayland, X11 (Xwayland) and gamescope.

- **One download:** each release is a single `framegen-vulkan-v1.2.0.zip` with a `native` folder and a `flatpak` folder, each with its own installer.
- **Higher base frame rate:** generated and real frames are copied on the game's queue at its present call. The slower worker-thread path is now a rare fallback (about 10–16% of frames at 2× on X11 and gamescope, down from about 30%; about 6% at 3× on Wayland).
- **X11 FIFO black screen fixed:** when the driver refuses fgvk's swapchain request, fgvk retries with simpler requests instead of leaving the game on a black screen.
- **Reflex:** sleep-mode resets from the game are honored, fgvk's own present work no longer counts against the game's frame in the driver, and the real frame's present lead is on by default.
- **AMD Anti-Lag:** the game's Anti-Lag is limited to the frame-generation cap.
- **Lower latency by default:** holding real frames to align generated ones (batch hold) is now off by default.
- **Pacing correction:** shown as its own setting in the settings window and applied live while the game runs.
- **Pacing captures:** `fgvk` saves pacing captures and prints their timings as readable tables.

### Install

Download `framegen-vulkan-v1.2.0.zip` from this release and extract it.

For native Steam and other Linux apps, install or upgrade from the `native` folder:

```bash
cd framegen-vulkan-v1.2.0/native
./install
```

For Flatpak apps, run the extension installer from the `flatpak` folder, then configure each app using the [Flatpak setup guide](https://github.com/Vyrolian/framegen-vulkan/blob/v1.2.0/engine/docs/FLATPAK-GUIDE.md):

```bash
cd framegen-vulkan-v1.2.0/flatpak
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

Repeat for each installed runtime branch. App overrides and profiles remain. [Flatpak removal and override cleanup](https://github.com/Vyrolian/framegen-vulkan/blob/v1.2.0/engine/docs/FLATPAK-GUIDE.md#uninstall-the-flatpak-extension)

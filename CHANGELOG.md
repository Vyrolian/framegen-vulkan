# Changelog

Each release of framegen-vulkan, newest first. The full notes of each release are on its GitHub release page; [`engine/RELEASE_NOTES.md`](engine/RELEASE_NOTES.md) holds the notes of the current release.

## 1.4.0

- **Lossless.dll is chosen, not searched:** games use only the configured `dll` path; without one, frame generation stays off and the log says that no path is set.
- **Browse and Auto-locate:** the settings window can pick the DLL with the desktop file picker or search the Steam libraries once, including libraries listed in `libraryfolders.vdf`.
- **System clipboard:** copy and paste in the settings window use the desktop clipboard.
- **Always one default profile:** a new configuration starts with **Main profile** as the default; **Set as default** moves it, and the default profile cannot be deleted.

## 1.3.0 (2026-11-22)

- **Even generated frames under partial VSync:** frame generation follows the game's own present timing (`VK_EXT_present_timing`), so VSync at a fraction of the refresh rate (for example VSync 60 on 240 Hz) splits each frame's time across the generated frames.
- **Present timing feedback with frame generation:** vkd3d-proton's timing feedback keeps working with frame generation on.
- **Pacing captures:** captures record each frame's timing target and the driver's reported presentation times.

## 1.2.1 (2026-11-22)

- **No hang when closing a game:** games that close their window while the compositor holds frames (for example Minecraft on Zink) no longer hang at exit.

## 1.2.0 (2026-11-22)

- **One download:** each release is a single zip with a `native` and a `flatpak` folder, each with its own installer.
- **Higher base frame rate:** frame generation runs on the game's own GPU queue and presents inside the game's present call; the worker-thread path is a rare fallback.
- **X11 FIFO black screen fixed:** a refused swapchain request is retried with simpler requests.
- **Reflex and AMD Anti-Lag:** Reflex sleep-mode resets are honored and the real frame's present lead is on by default; Anti-Lag is limited to the frame-generation cap.
- **Lower latency by default:** batch hold is off by default.
- **Pacing correction and captures:** pacing correction is its own setting and applies live; `fgvk` saves pacing captures and prints their timings as tables.

## 1.1.0 (2026-11-19)

- **One `fgvk` command:** `fgvk` opens Settings, launches games and runs the diagnostic commands through `fgvk cli`. Upgrading removes the old `fgvk-cli` and `fgvk-installer` commands.

## 1.0.0 (2026-11-18)

- **First framegen-vulkan release:** Fixed frame generation only; spatial scaling, Adaptive frame generation, GPU Headroom and the AC/battery profile tables are removed, and their old `conf.toml` keys are accepted and ignored.
- **Pacing:** generated frames are paced at every multiplier from 2× to 5× and moved onto their ideal points by the on-screen correction; batches start just in time after their frames are ready, and `low_latency` holds the game until the GPU is about to finish when the GPU is the limit.
- **Reflex:** with the game's Reflex on, framegen-vulkan leaves its own latency holds off so Reflex holds the game.
- **Displays and colour:** native Wayland, Xwayland, X11 and experimental Gamescope sessions; HDR10/PQ and scRGB, including Wine's HDR on native Wayland, get frame generation in linear light; Zink is supported.
- **Settings window:** an Appearance section with built-in and custom themes.
- **Builds:** release archives are built natively on Ubuntu 22.04 and are reproducible from one commit.

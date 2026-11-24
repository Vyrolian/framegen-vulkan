# framegen-vulkan

framegen-vulkan (`fgvk`) is a Vulkan layer that brings Lossless Scaling frame generation to Linux, with a focus on even frame pacing and low latency. It works with native Vulkan games, Zink, and with Proton games through DXVK and VKD3D-Proton.

## Features

- Fixed frame generation multipliers from 2× to 5×
- NVIDIA Reflex support in games that offer it
- Configurable FPS cap and an automatic cap based on refresh rate and multiplier
- Wayland, X11/Xwayland, and gamescope support
- Variable refresh rate (VRR) support
- HDR10 and scRGB frame generation

## Installation

### Requirements

- Linux on x86_64 with a Vulkan 1.2 driver. Testing has focused on NVIDIA. AMD and Intel remain unverified.
- A purchased copy of Lossless Scaling installed through Steam on the default public branch. `fgvk` reads `Lossless.dll` from that installation at runtime. [Lossless Scaling on Steam](https://store.steampowered.com/app/993090/Lossless_Scaling/)

### Install a release

Download `framegen-vulkan-vX.Y.Z.zip` from [GitHub Releases](https://github.com/Vyrolian/framegen-vulkan/releases) and extract it. It has two folders with their own installers: `native` for Steam and other Linux apps (built on Ubuntu 22.04, 64-bit and 32-bit Vulkan layers), and `flatpak` for Flatpak apps.

For native apps, run from the extracted folder. The installer uses `~/.local` and keeps existing profiles.

```bash
cd framegen-vulkan-vX.Y.Z/native
./install
```

For Flatpak apps, run the extension installer and follow the guide to configure each app. [Flatpak setup guide](engine/docs/FLATPAK-GUIDE.md)

```bash
cd framegen-vulkan-vX.Y.Z/flatpak
./install-flatpak
```

Releases up to v1.1.0 ship separate `-linux.tar.xz` and `-flatpak.tar.xz` archives. The original v1.0.0 archives use `"./Install framegen-vulkan"` and `"./Install framegen-vulkan Flatpak Extensions"`.

### Uninstall

Close the games and apps using framegen-vulkan. For a native installation, run:

```bash
~/.local/bin/fgvk uninstall
```

If you still have the original v1.0.0 native package installed, run `~/.local/bin/fgvk-installer --uninstall`. Confirm removal. The native uninstaller asks separately whether to delete your profiles, settings, and diagnostics.

For the Flatpak extension on runtime 26.08, run and confirm:

```bash
flatpak uninstall --user org.freedesktop.Platform.VulkanLayer.fgvkrender//26.08
```

Repeat for each installed runtime branch. Your profiles and app overrides remain. [Flatpak removal and override cleanup](engine/docs/FLATPAK-GUIDE.md#uninstall-the-flatpak-extension)

### Build dependencies

For source builds, use Clang with C++20 support, CMake 3.15 or newer, Ninja, Python 3, Git, Vulkan, X11 and XCB development files, and Rust 1.78 or newer with Cargo.

**Arch Linux and SteamOS with a writable root**

```bash
sudo pacman -S --needed base-devel clang cmake ninja git python vulkan-headers vulkan-icd-loader libx11 libxcb xorgproto rust
```

**Debian and Ubuntu**

```bash
sudo apt install build-essential clang cmake ninja-build git python3 libvulkan-dev libx11-dev libxcb1-dev cargo
```

**Fedora**

```bash
sudo dnf install gcc-c++ clang cmake ninja-build git python3 vulkan-loader-devel vulkan-headers libX11-devel libxcb-devel cargo
```

**openSUSE**

```bash
sudo zypper install gcc-c++ clang cmake ninja git python3 vulkan-devel vulkan-headers libX11-devel libxcb-devel cargo
```

The maintainer has tested the Arch Linux command. If your distribution ships Rust older than 1.78, use rustup to install a current toolchain. The build fetches its pinned Vulkan-Headers revision automatically. [rustup](https://rustup.rs/)

### Build and install

Run these commands from the repository root.

```bash
cd engine
./scripts/package-local.sh --64-bit-only build/package/framegen-vulkan.tar.xz
mkdir -p build/package/x && tar -xf build/package/framegen-vulkan.tar.xz -C build/package/x
cd build/package/x && ./install
```

The installer puts everything under `~/.local`, keeps existing profiles, and prints the launch option. Close games using `fgvk` before updating. To update, run `./install` from the newer extracted package.

The original v1.0.0 package uses `~/.local/bin/fgvk-cli validate` for configuration checks.

To also build the 32-bit layer, omit `--64-bit-only` and install multilib support. [Building from source](engine/docs/BUILDING-FROM-SOURCE.md)

## Launch a game

1. Run `~/.local/bin/fgvk` to open Settings and create a game profile. You can also edit `~/.config/framegen-vulkan/conf.toml` directly and check it with `~/.local/bin/fgvk cli validate`.
2. Start the game through `fgvk` using the command for your launcher.

| Launcher | How |
| --- | --- |
| Steam (native or Proton) | Set `~/.local/bin/fgvk %command%` in Launch Options |
| Heroic or Lutris (native) | Use the full path to `~/.local/bin/fgvk` as the wrapper or command prefix |
| Heroic and other Flatpak apps | [Flatpak setup guide](engine/docs/FLATPAK-GUIDE.md) |
| gamescope (Steam) | `gamescope -- ~/.local/bin/fgvk %command%` |
| A desktop program | `~/.local/bin/fgvk /path/to/program` |

You can shorten `~/.local/bin/fgvk` to `fgvk` when `~/.local/bin` is on the `PATH` of the shell or launcher that starts the game.

`fgvk` uses the matching game profile or the default profile, marked `(default)` in the settings window. The layer stays inactive when neither applies.

For HDR in Proton on Wayland, add `PROTON_ENABLE_WAYLAND=1 DXVK_HDR=1` to the game’s launch options. To disable frame generation inside gamescope, set `FGVK_GAMESCOPE=0`.

## Documentation

- [Configuration](engine/docs/CONFIGURATION.md) and [all settings](engine/docs/CONFIGURATION-REFERENCE.md)
- [Troubleshooting](engine/docs/TROUBLESHOOTING.md) and [collecting diagnostics](engine/docs/COLLECT_DIAGNOSTICS.md)
- [Building from source](engine/docs/BUILDING-FROM-SOURCE.md) for SteamOS, 32-bit layers, Flatpak and Arch packages
- [Renderer architecture](engine/docs/RENDERER-ARCHITECTURE.md)

## Credits

The original project was **lsfg-vk**. **MAKO** forked its GPL-licensed v2 development tree, and **framegen-vulkan** forked MAKO. Each project has since taken its own direction. [lsfg-vk archive](https://git.lsfg-vk.dev/lsfg-vk-archive) · [MAKO](https://github.com/eugeniosegala/MAKO)

[Contributors](CONTRIBUTORS.md)

## Licence

framegen-vulkan is free software under the GNU General Public License version 3 or later. [Licence text](LICENSE)

The copyright and third-party notices document the copyright holders and the lsfg-vk lineage. [Copyright](COPYRIGHT.md) · [Third-party notices](THIRD_PARTY_NOTICES.md)

framegen-vulkan and its maintainer have no affiliation with Lossless Scaling.

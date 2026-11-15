# framegen-vulkan

framegen-vulkan (`fgvk`) is a Vulkan layer that adds frame generation to games on Linux. It runs the Lossless Scaling frame-generation model (LSFG) at a fixed multiplier (2x to 5x) for native Vulkan games and for Proton games through DXVK and VKD3D-Proton, on Wayland, X11/Xwayland and gamescope (experimental). HDR10 and scRGB games get frame generation in linear light.

framegen-vulkan does not include Lossless Scaling. You need a lawful copy of [Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) installed through Steam; framegen-vulkan reads its `Lossless.dll` at runtime and never copies, changes or uploads it.

## Requirements

- Linux on x86_64 with a Vulkan 1.2 GPU driver (tested on NVIDIA; AMD and Intel are not regularly tested).
- Lossless Scaling installed through Steam (the default public version).
- To build: a C++20 compiler, CMake 3.15 or newer, Ninja, Python 3, git, Vulkan, X11 and XCB development files, and Rust 1.78 or newer with `cargo`.

## Build dependencies

Arch Linux (and SteamOS with a writable root):

```bash
sudo pacman -S --needed base-devel cmake ninja git python vulkan-headers vulkan-icd-loader libx11 libxcb xorgproto rust
```

Debian and Ubuntu:

```bash
sudo apt install build-essential cmake ninja-build git python3 libvulkan-dev libx11-dev libxcb1-dev cargo
```

Fedora:

```bash
sudo dnf install gcc-c++ cmake ninja-build git python3 vulkan-loader-devel vulkan-headers libX11-devel libxcb-devel cargo
```

openSUSE:

```bash
sudo zypper install gcc-c++ cmake ninja git python3 vulkan-devel vulkan-headers libX11-devel libxcb-devel cargo
```

Only the Arch line is tested by the maintainer. If your distribution's Rust is older than 1.78 (Ubuntu 22.04, for example), install Rust with [rustup](https://rustup.rs/) instead of the `cargo` package. The build fetches the pinned Vulkan-Headers revision itself.

## Build and install

```bash
git clone <repository-url> framegen-vulkan
cd framegen-vulkan/engine
./scripts/package-local.sh --64-bit-only build/package/framegen-vulkan.tar.xz
mkdir -p build/package/x && tar -xf build/package/framegen-vulkan.tar.xz -C build/package/x
cd build/package/x && "./Install framegen-vulkan"
```

The installer puts everything under `~/.local`, keeps existing profiles and prints the launch option. Run it again from a newer package to update; `~/.local/bin/fgvk-installer --uninstall` removes it. Close games that use framegen-vulkan first. Leave out `--64-bit-only` to also build the 32-bit layer (needs a multilib compiler, for example `g++-multilib` on Debian/Ubuntu or `lib32-glibc` on Arch).

## Use

1. Run `~/.local/bin/fgvk` to open the settings window and create a profile for your game (or edit `~/.config/framegen-vulkan/conf.toml`; check it with `~/.local/bin/fgvk-cli validate`).
2. Start the game through `fgvk`:

| Launcher | How |
| --- | --- |
| Steam (native or Proton) | Launch options: `~/.local/bin/fgvk %command%` |
| Heroic or Lutris (native) | Set the absolute `~/.local/bin/fgvk` path as the wrapper or command prefix |
| Flatpak Heroic or other Flatpak apps | Follow the [Flatpak guide](engine/docs/FLATPAK-GUIDE.md) |
| gamescope | `gamescope -- ~/.local/bin/fgvk %command%`; set `FGVK_GAMESCOPE=0` to turn frame generation off there |
| A desktop program | `~/.local/bin/fgvk /path/to/program` |

A profile alone does nothing: the game has to start through `fgvk`, and if no profile matches, the layer stays inactive. For HDR in Proton on Wayland, start the game with `PROTON_ENABLE_WAYLAND=1 DXVK_HDR=1`.

## Documentation

- [Configuration](engine/docs/CONFIGURATION.md) and [all settings](engine/docs/CONFIGURATION-REFERENCE.md)
- [Troubleshooting](engine/docs/TROUBLESHOOTING.md) and [collecting diagnostics](engine/docs/COLLECT_DIAGNOSTICS.md)
- [Building from source](engine/docs/BUILDING-FROM-SOURCE.md) for SteamOS, 32-bit layers, Flatpak and Arch packages
- [Developing](DEVELOPING.md) for tests, dependency owners and releases
- [Renderer architecture](engine/docs/RENDERER-ARCHITECTURE.md)

## Contributors

<!-- fgvk-contributors:start -->

<a href="https://github.com/eugeniosegala"><img src="https://github.com/eugeniosegala.png?size=48&amp;v=20261004" width="48" height="48" alt="@eugeniosegala" /></a> <a href="https://github.com/Hu2ki3"><img src="https://github.com/Hu2ki3.png?size=48&amp;v=20261004" width="48" height="48" alt="@Hu2ki3" /></a> <a href="https://github.com/hugouchoasborges"><img src="https://github.com/hugouchoasborges.png?size=48&amp;v=20261004" width="48" height="48" alt="@hugouchoasborges" /></a> <a href="https://github.com/lordkaus"><img src="https://github.com/lordkaus.png?size=48&amp;v=20261004" width="48" height="48" alt="@lordkaus" /></a> <a href="https://github.com/PJ-568"><img src="https://github.com/PJ-568.png?size=48&amp;v=20261004" width="48" height="48" alt="@PJ-568" /></a> <a href="https://github.com/rosakodu"><img src="https://github.com/rosakodu.png?size=48&amp;v=20261004" width="48" height="48" alt="@rosakodu" /></a> <a href="https://github.com/Tak-attack"><img src="https://github.com/Tak-attack.png?size=48&amp;v=20261004" width="48" height="48" alt="@Tak-attack" /></a> <a href="https://github.com/Vyrolian"><img src="https://github.com/Vyrolian.png?size=48&amp;v=20261004" width="48" height="48" alt="@Vyrolian" /></a> <a href="https://github.com/w169q169"><img src="https://github.com/w169q169.png?size=48&amp;v=20261004" width="48" height="48" alt="@w169q169" /></a> <a href="https://github.com/WowOne987"><img src="https://github.com/WowOne987.png?size=48&amp;v=20261004" width="48" height="48" alt="@WowOne987" /></a>
<!-- fgvk-contributors:end -->

## Licence

framegen-vulkan is free software under the GNU General Public License version 3 or later; see [LICENSE](LICENSE). Copyright holders, the lsfg-vk lineage and third-party notices are in [COPYRIGHT.md](COPYRIGHT.md) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Lossless Scaling and LSFG are the property of their owner; framegen-vulkan is not affiliated with or endorsed by Lossless Scaling.

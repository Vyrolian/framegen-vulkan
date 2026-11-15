# Arch Linux packaging

Install `framegen-vulkan-bin` for a system-wide framegen-vulkan on writable x86_64 Arch Linux. SteamOS users should use the [user-local archive](../../../README.md#install-and-use).

## Contents

The package includes the `fgvk` settings window and `fgvk <command>` entry point, the CLI, the private launch helper, and 64-bit/32-bit Vulkan layers under `/usr`. Use pacman to manage these files; the user-local archive installer is not included. Maintainers can find the [payload layout and checks](../../docs/BUILDING-FROM-SOURCE.md#arch-package-maintenance) in the build guide.

## Install the release package

An `x86_64` Arch Linux system with the multilib repository enabled is required because one package carries both the 64-bit and 32-bit Vulkan layers and their runtime dependencies. Download `framegen-vulkan-bin-X.Y.Z-1-x86_64.pkg.tar.zst` from the matching framegen-vulkan GitHub release and leave it unextracted. Open a terminal in the folder containing the downloaded file, then run the following command, replacing the example filename with the exact downloaded filename:

```bash
sudo pacman -U ./framegen-vulkan-bin-X.Y.Z-1-x86_64.pkg.tar.zst
```

The `./` prefix means the terminal's current folder; this command does not download the package. You can instead use the file's full path. For example, if you saved it in `Downloads`, replace the example filename below with the downloaded filename:

```bash
sudo pacman -U "$HOME/Downloads/framegen-vulkan-bin-X.Y.Z-1-x86_64.pkg.tar.zst"
```

If pacman reports `could not find or read package`, check that the file exists at the path you supplied and that its filename matches exactly. Download it again if it was deleted or the download did not finish. Reinstalling after removal uses the same steps.

Pacman verifies the package database transaction, installs the declared dependencies, owns every installed path, and runs the non-mutating coexistence report. The GitHub release records the package's SHA-256 in its release notes and the repository's release metadata.

### Configure and launch

Installing the package does not activate framegen-vulkan for every game. Create a game profile in `~/.config/framegen-vulkan/conf.toml` and configure Frame Generation. Check the file with:

```bash
fgvk-cli validate
```
 Frame Generation requires a lawful user-supplied `Lossless.dll`. DirectX-only LSFG models additionally need the optional `vkd3d` dependency, plus `lib32-vkd3d` for 32-bit games. Native SPIR-V LSFG models do not need the translator. For a native Steam or Proton game, add this under **Steam Properties > General > Launch Options**:

```text
/usr/bin/fgvk %command%
```

Use the absolute `/usr/bin` path so a separate `~/.local/bin/fgvk` cannot shadow the pacman-owned entry point. Flatpak games additionally require the matching framegen-vulkan Flatpak runtime extension and application preparation described in the [Flatpak guide](../../docs/FLATPAK-GUIDE.md).

## Build from the tracked recipe

To reproduce the package from the official archive instead, build and install from this directory:

```bash
cd engine/dist/arch
makepkg -si
```

`makepkg` downloads the release archive, verifies `sha256sums`, and repackages the extracted payload. The package does not run a compiler.

## Build the complete local Renderer artifact set

From the repository root, build release-shaped host, Flatpak, and Arch artifacts from the current checkout with:

```bash
scripts/dev.sh package-renderer-local-release
```

The equivalent component command is `engine/scripts/package-local-release.sh`. It writes all three artifacts and `SHA256SUMS` under `engine/out/`, uses the same portable builders as publication, and verifies the pacman package against the newly built host archive. It synchronizes a disposable copy of this recipe, so the tracked public archive checksum, Renderer release pin, tags, and releases remain unchanged.

## Upgrade

This package is not currently published through an official pacman repository or the AUR, so `pacman -Syu` alone cannot discover a new framegen-vulkan version. Download the newer `.pkg.tar.zst` from the matching GitHub release and run `sudo pacman -U` again. Pacman replaces all package-owned files under `/usr`. User profiles and configuration data are not package-owned and are never touched during an upgrade. The `pre_upgrade` hook re-reports any user-local framegen-vulkan install it detects.

## Remove

```bash
sudo pacman -R framegen-vulkan-bin
```

Pacman removes only the files the package owns under `/usr`. Profiles and configuration under `~/.config/framegen-vulkan` are preserved and must be removed manually if they are no longer wanted. A user-local framegen-vulkan install is left untouched.

Do not run the upstream installer or extract the archive with `FGVK_INSTALL_PREFIX=/usr`. That writes the same `/usr/bin` and `/usr/share` paths this package owns: pacman then refuses the next install or upgrade with `exists in filesystem`, and the upstream uninstaller would delete files pacman still believes it owns. If pacman reports a file conflict, use `pacman -Qo` to check whether the file is unowned, remove it, and install again; do not reach for `--overwrite`.

## Coexistence with user-local installs

The pacman package manages `/usr`; the standalone archive installs a separate native copy under `~/.local`. Pacman reports user-local copies but does not change or remove them.

Use `/usr/bin/fgvk` explicitly to select this package. Do not export `ENABLE_FGVK=1` globally: use the launcher to select the matching layers. Flatpak runtime extensions are installed and updated separately.

## Profiles and configuration data

Profiles and settings live in `~/.config/framegen-vulkan` (`conf.toml` and `launcher.conf`). They belong to the user and are never installed, replaced, or removed by this package.

Because both delivery paths share that directory, do not run two different framegen-vulkan versions against one `~/.config/framegen-vulkan`: a user-local install and this package would migrate the same profiles with different engine versions. Keep one owner per user.

## Updating for a new release

The publisher synchronizes the recipe with the verified Renderer archive. See [Arch package maintenance](../../docs/BUILDING-FROM-SOURCE.md#arch-package-maintenance) for pin updates and packaging-only fixes.

## AUR publication

The release workflow publishes packages on GitHub, not the AUR. AUR publication is a separate maintainer workflow; see [the build guide](../../docs/BUILDING-FROM-SOURCE.md#aur-publication).

## Verification

The package is checked against the host archive's checksums, required files, architectures, dependencies and Vulkan manifests. It may contain no ELF file under `/usr/share`. The installed checksum manifest is `/usr/share/doc/framegen-vulkan-bin/framegen-vulkan-install-manifest.txt`. See [maintainer verification](../../docs/BUILDING-FROM-SOURCE.md#verification) for the complete contract.

## Links

- [Upstream README](../../../README.md)
- [Engine README](../../README.md)
- [Flatpak guide](../../docs/FLATPAK-GUIDE.md)
- [Troubleshooting](../../docs/TROUBLESHOOTING.md)
- [Collect diagnostics](../../docs/COLLECT_DIAGNOSTICS.md)
- [Installation transactions](../../../INSTALLATION-TRANSACTIONS.md)
- [Cleanups](../../../CLEANUPS.md)

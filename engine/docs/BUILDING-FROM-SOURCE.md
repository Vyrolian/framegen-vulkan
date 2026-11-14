# Building framegen-vulkan from source

This guide covers local development, source installation, and distributable Renderer packages.

Once cloned, run build and packaging commands from `engine/` unless a section says otherwise. Paths in the build-path table are relative to the repository root.

<!-- prettier-ignore -->
> [!IMPORTANT]
> If you are planning on compiling framegen-vulkan on SteamOS, check the direct-build prerequisites from the repository root, then run the installer if anything is missing:
>
> ```bash
> ./engine/scripts/install-steamos-build-tools.sh --check
> ./engine/scripts/install-steamos-build-tools.sh --install
> ```
>
> The installer restores the original SteamOS read-only state, initializes the official Arch and Holo keyrings, and reinstalls the native build packages without `--needed`. Some SteamOS images preserve Pacman's installed-package records while removing development files or the local keyring. The script prompts before the Pacman transaction and does not build or deploy framegen-vulkan.

SteamOS image updates can also leave X11, XCB, Wayland, and Vulkan loader packages registered with Pacman while their development headers or metadata are absent. The installer restores these alongside the compiler, 32-bit headers, shader tools, and Flatpak development tools; its `--check` mode reports missing commands and headers without changing the host.

Already writable roots remain writable. Initially read-only roots are restored after the package transaction, including when it fails. SteamOS 3.9 provides `steamos-readonly` through its `steamos-alias` package, forwarding to `holo-readonly`; the installer accepts the command's `disabled` result with exit status 1.

## Prerequisites

Install:

- Build tools such as `git` and `curl`
- A C++ compiler that supports C++20 or later
- CMake 3.15 or newer for the commands below, which use [`cmake --install`](https://cmake.org/cmake/help/v3.15/release/3.15.html#command-line); the optional forced-FP16 build requires 3.22.1 or newer
- Ninja (recommended; other CMake generators may work)
- Vulkan headers and loader development files
- X11 and XCB headers (`libx11`, `libxcb`, and `xorgproto` on Arch/SteamOS)
- Python 3 when building the registered tests
- A multilib C++ toolchain when building the 32-bit Vulkan layer
- A current stable Rust toolchain with `cargo` for the `engine/fgvk-ui` settings window and `fgvk <command>` entry point (`just fgvk-ui` builds it, `just test-fgvk-ui` tests it); the packagers need it too

Package names vary by distribution. These commands cover common Debian/Ubuntu and Arch installations:

```bash
# On Debian/Ubuntu, use:
sudo apt-get install -y \
    git curl python3 \
    llvm clang clang-tools clang-tidy \
    cmake ninja-build pkg-config g++-multilib \
    libvulkan-dev libx11-dev libxcb1-dev \
    mesa-common-dev

# On writable Arch Linux, use (SteamOS users should run the installer above):
sudo pacman -S --needed \
    git curl python \
    llvm clang ccache lib32-glibc \
    cmake ninja \
    vulkan-headers vulkan-icd-loader libx11 libxcb xorgproto
```

The release packager builds the 64-bit CLI, launcher, and both Renderer roles, then builds both roles again with `-m32`. It installs the libraries in `lib` and `lib32` with architecture-tagged manifests. A direct CMake build targets only the selected compiler architecture.

## Choose the right build path

| Need | Entry point | Result |
| --- | --- | --- |
| Incremental native Renderer work on SteamOS | `engine/scripts/build-steamos-dev.sh` | Reuses a development tree and builds the 64-bit layer and CLI; no distributable archive |
| Standalone Renderer archive | `engine/scripts/package-local.sh` | Tests and packages the host Renderer payload; does not publish |
| Complete local Renderer release-shaped set | `engine/scripts/package-local-release.sh` | Builds the portable host archive, Flatpak archive, verified Arch package, and `SHA256SUMS` without changing release pins or publishing |
| Complete local release candidate | `engine/scripts/package-local-release.sh`, then `scripts/check-release-candidate.sh` with the host and Flatpak archives from `engine/out/` | Builds once from a clean pushed commit and checks the existing complete archives without a CI rebuild or installation |
| Public release | `scripts/publish-release.sh X.Y.Z` from the repository root | Publishes framegen-vulkan, records its checksums in `engine/renderer-release.json`, and updates release links |

`package-local-release.sh` writes under `engine/out/` and synchronizes a disposable Arch recipe. The complete local archives serve as release candidates; publication separately applies version and pin commits and rebuilds public assets. [How to release framegen-vulkan](../../HOW_TO_RELEASE.md) owns qualification, build alignment, and publication.

Local native Linux packaging compiles against the pinned Vulkan-Headers revision (see [Vulkan-Headers](#vulkan-headers)); set `FGVK_PORTABLE_PACKAGE=1` only to use the portable Ubuntu 22.04 baseline with Docker or Podman. Non-Linux packaging and all public native releases require one of those runtimes.

Every packaging mode builds `bin/fgvk` with the host `cargo` before any container starts, so a missing `cargo` stops packaging at once. The portable Ubuntu 22.04 container only receives the finished binary. A host-built `fgvk` needs the build host's glibc symbol versions: with the current toolchain it requires `GLIBC_2.43`, so it will not start on Ubuntu 22.04 (glibc 2.35). `package-local.sh` prints a warning when the requirement exceeds 2.35.

For an explicitly requested maintainer exception, `FGVK_RELEASE_SKIP_TESTS=1` omits test compilation, CTest, and launcher tests while retaining build, ABI, archive, and checksum verification. See the [hotfix exception](../../HOW_TO_RELEASE.md#maintainer-directed-hotfix-without-automated-validation). Publication always uses the portable builders: Ubuntu 22.04, Clang 14, and the pinned Vulkan headers for the native archive, plus Ubuntu 24.04 driving each declared Flatpak runtime SDK. Use `FGVK_PORTABLE_PACKAGE=1` for complete tester packages too; the scripts select Docker or Podman automatically. Without that flag, Flatpak packaging uses host `flatpak-builder` when available and falls back to the container builder when it is absent. Follow the [build-alignment and evidence requirements](../../HOW_TO_RELEASE.md#keep-tester-and-release-builds-aligned) when comparing tester and release artifacts.

## Package dependency pins

### Vulkan-Headers

Native and Flatpak archive builds and `scripts/build-steamos-dev.sh` enable `FGVK_REQUIRE_NATIVE_PACKAGE_HEADERS=ON` for both applicable architectures; the option retains its historical name for compatibility. CMake compiles the actual presentation-filter header and rejects headers older than the pinned revision or missing `VK_KHR_present_id2` or `VK_EXT_present_timing`, including when automated tests are disabled. The timing-node ABI checks therefore compile against the official `VkPresentTimingsInfoEXT` declaration. This prevents SDK selection from silently removing presentation compatibility. It is a build-time requirement, not a higher Vulkan driver/API requirement. Manual CMake builds retain their default host-header compatibility unless this option is enabled. Flatpak builds use the shared pinned headers with each runtime's existing compiler and libraries and still need runtime-specific evidence.

[`engine/vulkan-headers-revision.txt`](../vulkan-headers-revision.txt) is the single owner of the native and Flatpak Vulkan-Headers build pin and minimum header version. It contains one upstream SDK branch (`vulkan-sdk-X.Y.Z`) or release tag (`vX.Y.Z`). The portable packager, fast SteamOS development builder, and main Renderer CI builds fetch that ref, and CMake derives its minimum version from the same file; native `package-local.sh` builds fetch it once into `engine/build/vulkan-headers/<revision>/` and compile against it, unless `FGVK_VULKAN_HEADERS_INCLUDE_DIR` names another include directory containing `vulkan/vulkan_core.h`.

The fast builder caches a release tag under `engine/build/cache/vulkan-headers/`, resolves a movable SDK branch on each run, and logs the selected commit. `scripts/generate-flatpak-vulkan-headers.py` derives the shared Flatpak module from the pin; regenerate after pin changes, then run its read-only `--check` gate. Every generated Flatpak manifest includes that generated module before its two Renderer builds. The headers are removed during Flatpak cleanup and are not a runtime dependency. An SDK branch can advance, so retain the resolved header commit with the build evidence.

The sanitizer CI job also fetches this pin and compiles its policy tests against the same headers; manual portable-policy runs can still exercise distribution headers for compatibility coverage. To update the baseline, change this file and qualify the resulting packages under the tester/release build-alignment requirements above.

The Vulkan layer manifests' `api_version` describes the layer's supported API and is maintained separately; it must not automatically follow header updates.

Distributable archives and Flatpak extensions include the project license, third-party notices, and asset-provenance record. Packaging fails if those files or another required payload entry is missing.

## Fast SteamOS development build

For native Steam-game iteration, use the persistent incremental build instead of the release packager:

```bash
./scripts/build-steamos-dev.sh
```

This native host build uses the shared Vulkan-Headers pin and package presentation check while retaining its incremental CMake tree. Its first run needs network access to cache the pinned headers; later runs reuse a pinned release tag offline. It still needs the host C++ toolchain and X11/XCB development headers. Use the portable package builder when the output itself must match the release toolchain and packaging checks.

It builds both 64-bit Renderer roles and the CLI, retaining `build/steamos-dev` between runs. For real AMD validation with a sibling fgvk Gym checkout, run `./scripts/run-fgvk-gym.sh --suite quality --cli build/steamos-dev/fgvk-cli/fgvk-cli`. To retain a second tree for genuine 32-bit games, run:

```bash
./scripts/build-steamos-dev.sh --with-32-bit
```

The 32-bit tree defaults to `build/steamos-dev-32`. This development path skips Flatpak extensions, tests, archives, and hardware QA. It uses `ccache` under `build/cache/ccache` when available.

### Experimental forced LSFG FP16

For visual testing of an FP32-only LSFG model, build a separate native candidate with the existing FP16 setting controlling conversion:

```bash
./scripts/build-steamos-dev.sh --experimental-lsfg-fp16 --with-32-bit \
    --build-dir build/lsfg-fp16-experimental \
    --build-32-dir build/lsfg-fp16-experimental-32 --jobs 4
```

This build requires CMake 3.22.1 or newer. `scripts/prepare-lsfg-fp16-tools.sh` downloads checksum-pinned official SPIRV-Tools `33e02568181e3312f49a3cf33df470bf96ef293a` and its matching SPIRV-Headers `2a611a970fdbc41ac2e3e328802aed9985352dca` into `build/cache/lsfg-fp16-tools/`. The optimizer is linked statically for each architecture, with no additional runtime library or executable requirement. Direct CMake builds can use `-DFGVK_EXPERIMENTAL_LSFG_FP16=ON -DFGVK_LSFG_FP16_TOOLS_SOURCE=PATH` with the prepared source directory. Ordinary development and release builds leave conversion disabled by default; the build fingerprint identifies experimental binaries explicitly.

There is no additional runtime toggle. With FP16 on, native FP16 takes priority; otherwise a supported native FP32 or translated DirectX graph is converted once and cached in memory. With FP16 off, the ordinary FP32 path remains selected. Conversion requires valid SPIR-V, preserved descriptor contracts, and actual half arithmetic. It cannot manufacture support for an unknown model layout or an unsupported GPU. Shader inputs, converted modules, and model resources never leave process memory.

Forced conversion remains unqualified for game image quality and reports `experimental_fp16=forced; quality=unqualified`. Earlier blanket conversion failed moving-edge checks; precision protection corrected the tested procedural failures. Compare brightness, motion edges, ghosting, flicker, and fine detail against FP32 and real-frame passthrough in actual games. Compilation, procedural checks, and Vulkan validation do not establish game quality. Building does not update installed components; use the deployment workflow only when requested.

The forced path keeps shared mipmap preparation (logical stage 255) and final image reconstruction (logical stage 256) at their original FP32 precision and converts eligible feature arithmetic to FP16. Sampling coordinates, LODs, offsets, and bit-pattern calculations retain FP32, including dependencies passed through shader memory; rounding them before expanding back to FP32 corrupts motion estimates. Protection is conservative across writes to one variable and stops at sampled feature values, so some feature stages remain entirely FP32. The tested beta graph contains 28 stages with actual half arithmetic; this is reported from each converted graph rather than assumed from its size. This mixed precision avoids the observed beta brightness loss and procedural motion errors without modifying a DLL's native FP16 model. Compare generated-frame brightness and motion with FP32 as well as real-frame passthrough; do not hide a conversion error with a brightness multiplier.

## Flatpak extension builds

From the monorepo root:

```bash
cd engine
./scripts/package-flatpaks.sh
```

The script verifies both Renderer roles for 64-bit and 32-bit processes on every supported runtime and writes the archive under `engine/out/`.

[`runtime-versions.json`](../dist/flatpak/fgvk-render/runtime-versions.json) owns the ordered Freedesktop branches and LLVM SDK versions; the adjacent `org.freedesktop.Platform.VulkanLayer.fgvkrender.yml.in` owns the common manifest. After adding or retiring a branch, run from the monorepo root:

```bash
just generate-flatpak-runtimes
just check-flatpak-runtimes
```

Without `just`, run `python3 scripts/generate-flatpak-runtimes.py`; validate with `python3 scripts/generate-flatpak-runtimes.py --check`.

Generation updates Renderer manifests and the shell matrix, and removes retired generated manifests. Never edit generated outputs independently. Packaging and Renderer CTest reject stale outputs without rewriting them.

The same catalogue drives the bundles and extension refresh. When it changes, build a complete current-source Flatpak archive; older archives may lack new branches. Installed components share no source-checkout dependency. Builds use the shared [Vulkan-Headers pin](#vulkan-headers) with each runtime SDK's compiler and libraries.

## SteamOS Flatpak development cache

Reusable package data lives under `build/cache`; disposable staging lives under `build/work`. Inspect both without deleting anything:

```bash
./scripts/prune-build-cache.sh
```

Add `--confirm` to remove those two trees. Native incremental builds, release artifacts, installed framegen-vulkan files, and profiles are not removed.

To inspect only the Flatpak cache and staging area, run:

```bash
./scripts/prune-steamos-flatpak-cache.sh
```

Add `--confirm` to remove only those Flatpak directories:

```bash
./scripts/prune-steamos-flatpak-cache.sh --confirm
```

Both pruners target only the default repository-local paths. They do not remove custom locations selected through environment overrides.

Set `FGVK_BUILD_JOBS=4` to limit parallelism on a memory-constrained Deck, or `FGVK_BUILD_DIR=/path/to/build` to relocate the incremental development tree.

## Build and install framegen-vulkan

1. **Clone the repository**

Clone the framegen-vulkan monorepo and enter the engine package:

```bash
git clone https://github.com/eugeniosegala/MAKO.git
cd framegen-vulkan/engine
```

To build a specific Renderer release, check out its tag:

```bash
git checkout tags/render-vX.Y.Z
```

2. **Configure with CMake**

```bash
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DCMAKE_CXX_COMPILER=clang++
```

Useful CMake options:

- `CMAKE_BUILD_TYPE`: Set to `Release` for optimized builds or `Debug` for debugging builds.
- `CMAKE_INSTALL_PREFIX`: Specify the installation directory (default is `/usr/local`).
- `FGVK_BUILD_VK_LAYER`: Set to `On` to build the Vulkan layer (default is `On`).
- `FGVK_BUILD_CLI`: Set to `On` to build the command-line interface (default is `On`).
- `FGVK_INSTALL_DEVELOP`: Set to `On` to install development files like headers and libraries (default is `Off`).
- `FGVK_REQUIRE_NATIVE_PACKAGE_HEADERS`: Require the shared native/Flatpak Vulkan header baseline (default is `Off` for direct CMake builds; all package builds set it to `On`).
- `FGVK_VULKAN_HEADERS_INCLUDE_DIR`: Prefer an explicit Vulkan-Headers `include/` directory for compilation and the required-header check; the fast SteamOS builder sets this to its cache of the shared pin.
- `FGVK_LAYER_LIBRARY_PATH`: Override the frame-generation role library path stored in its manifest.
- `FGVK_LAYER_MANIFEST_SUFFIX`: Add a suffix to the installed manifest filename when packaging multiple architectures.

For a non-system prefix, set the path relative to its installed manifest, for example `../../../lib/libfgvk-render.so`.

3. **Build**

```bash
cmake --build build
```

The default build produces the single framegen-vulkan Vulkan layer library, `libfgvk-render.so`.

The default and Vulkan-layer builds check `fgvk-render/src/**/*.cpp` against the explicit production source lists in `fgvk-render/CMakeLists.txt`. When adding or moving an implementation file, assign it to its owning target there. The inventory check runs on incremental builds too, so an unlisted new file fails the build instead of silently disappearing from the host, 32-bit, Flatpak, and Arch artifacts. CTest runs the same check as `renderer-source-inventory` in the portable policy set.

4. **Install**

```bash
sudo cmake --install build
```

Start a native game with `fgvk <command>`. The helper selects the install prefix's private manifests, activates framegen-vulkan for that child, excludes competing frame generation layers, and defaults to SDR. Read [WSI isolation](WSI-ISOLATION.md) and [Optional graphics integrations](LAYER-CHAINING.md) before changing manifests or launch variables.

## Arch package maintenance

User installation, updates and removal are in the [Arch guide](../dist/arch/README.md). `engine/dist/arch/PKGBUILD` repackages the verified host archive; it provides and conflicts with `fgvk-renderer`, the identity reserved for a source-built alternative. Run recipe scripts from `engine/dist/arch/` unless a command specifies the repository root.

### Payload layout

The package installs:

- `fgvk` (settings window and `fgvk <command>` entry point), `fgvk-cli`, and `fgvk-diagnostics` into `/usr/bin`.
- the private launch helper `fgvk-launch` into `/usr/libexec/framegen-vulkan`, off `PATH`.
- the 64-bit Vulkan layers into `/usr/lib` and the 32-bit layers into `/usr/lib32`.
- the public implicit-layer manifest into `/usr/share/vulkan/implicit_layer.d`, gated by `ENABLE_FGVK`.
- the private manifest used by `fgvk` into `/usr/share/framegen-vulkan/vulkan/implicit_layer.d`.
- license texts into `/usr/share/licenses/framegen-vulkan-bin`.
- `ASSET_PROVENANCE.md`, the version file, and the upstream checksum manifest into `/usr/share/doc/framegen-vulkan-bin`.

The package deliberately excludes:

- `bin/fgvk-installer` and the `Install framegen-vulkan` launcher, which form the user-local installer and target `~/.local`.
- the archive's `README.txt`, which documents the user-local installer flow that this package does not provide.

Two details differ from the archive layout: the archived `share/doc/framegen-vulkan/*` files are installed under `/usr/share/licenses/framegen-vulkan-bin/` and `/usr/share/doc/framegen-vulkan-bin/`, and shared libraries are installed as mode `0755` instead of the archive's `0644`, following Arch convention. Content hashes are unaffected, so the checksum manifest still matches.

### Release pins

The publisher pins the verified host archive in `engine/renderer-release.json`, runs `sync-release-pin.py` (resetting `pkgrel` to 1 for a new version), and checks it with `check-release-pin.sh`. `verify-release-package.sh` packages that exact local archive and retains the verified `.pkg.tar.zst` for publication and checksum recording. The archive manifest owns payload membership; do not maintain a second list.

The verifier checks every ELF's architecture and direct `DT_NEEDED` dependencies against `.PKGINFO`; a new shared-library dependency requires a reviewed package dependency and soname mapping. Dynamically opened libraries still need Arch launch testing.

For a packaging-only fix, leave `pkgver` and `sha256sums` unchanged and increment `pkgrel`. Run `just check-arch-package` from the repository root before committing. `check-release-pin.sh` also runs in portable Renderer CTest and fails when `pkgver`, the source URL, or `sha256sums` drifts from `engine/renderer-release.json`.

### AUR publication

This repository owns the reviewed `PKGBUILD` and publishes its verified binary package on GitHub, but the release publisher does not push to the AUR. An AUR package needs its own package repository, generated `.SRCINFO`, and maintainer credentials; those are separate publication and trust boundaries. If it is published later, generate `.SRCINFO` from this exact recipe after each synchronized Renderer release instead of maintaining a second independent pin.

### Verification

- `makepkg` verifies the release archive against `sha256sums` before packaging.
- Renderer publication runs `verify-release-package.sh` after synchronizing the recipe, uploads that exact verified package, records its checksum in release metadata, and refuses to consider the release complete if the asset is missing or differs from the recorded checksum.
- The build wrapper rejects a stale or renamed package unless its filename, `.PKGINFO` name/version/architecture, embedded `framegen-vulkan-version.txt`, synchronized recipe, and source archive checksum all agree.
- The build wrapper rejects a new or undeclared ELF dependency in the CLI or either Renderer role. It also rejects an ELF with the wrong 32-bit or 64-bit class for its package path.
- Every payload file this package installs has the same SHA-256 as its entry in `framegen-vulkan-install-manifest.txt`. The recipe verifies the complete archive manifest, packages every supported entry instead of maintaining a second payload allowlist, and fails on unsafe or unsupported paths. The manifest is installed unchanged at `/usr/share/doc/framegen-vulkan-bin/framegen-vulkan-install-manifest.txt` so the claim can be re-checked. The only manifest entries without a matching installed file are `bin/fgvk-installer` and desktop entries, both excluded by design.
- A Vulkan loader run with `VK_IMPLICIT_LAYER_PATH` pointed at each packaged manifest directory discovers and activates `VK_LAYER_FGVK_render` for the public set, and the matching layer for each private directory, with the relative `library_path` values resolving to `/usr/lib` and `/usr/lib32`.
- `bash -n PKGBUILD`, `sh -n framegen-vulkan-bin.install`, and `./check-release-pin.sh` all pass.

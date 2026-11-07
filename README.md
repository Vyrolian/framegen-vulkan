# MAKO - Frame Generation, Scaling, and Shaders on SteamOS/Linux

<p align="center">
  <img src="assets/mako-logo.webp" width="256" alt="MAKO logo" />
</p>

<p align="center">
  <a href="https://trendshift.io/repositories/171701?utm_source=trendshift-badge&amp;utm_medium=badge&amp;utm_campaign=badge-trendshift-171701" target="_blank" rel="noopener noreferrer"><img src="https://trendshift.io/api/badge/trendshift/repositories/171701/daily?language=C%2B%2B" alt="eugeniosegala/MAKO | Trendshift" width="250" height="55" /></a>
</p>

<p align="center">
  <a href="https://discord.gg/NAVkyCq7Rc" target="_blank" rel="noopener noreferrer"><img src="https://img.shields.io/badge/Discord-join-5865F2?style=flat-square&amp;logo=discord&amp;logoColor=white" alt="Join the MAKO Discord community" /></a>
  <a href="https://github.com/eugeniosegala/MAKO/actions/workflows/tests.yml" target="_blank" rel="noopener noreferrer"><img src="https://img.shields.io/github/actions/workflow/status/eugeniosegala/MAKO/tests.yml?branch=main&amp;style=flat-square&amp;label=tests" alt="Tests status" /></a>
  <a href="LICENSE.md" target="_blank" rel="noopener noreferrer"><img src="https://img.shields.io/badge/license-GPL--3.0--or--later-0f766e?style=flat-square" alt="GPL-3.0-or-later license" /></a>
  <br />
  <a href="https://github.com/eugeniosegala/MAKO/releases?q=render-v" target="_blank" rel="noopener noreferrer"><img src="https://img.shields.io/github/v/release/eugeniosegala/MAKO?filter=render-%2A&amp;display_name=tag&amp;sort=semver&amp;style=flat-square&amp;label=Renderer&amp;color=1d4ed8" alt="Latest MAKO Renderer release" /></a>
  <img src="https://img.shields.io/badge/platform-SteamOS%20%7C%20Linux-6b8e23?style=flat-square" alt="SteamOS and Linux" />
</p>

<!-- prettier-ignore -->
> **Independent project:** MAKO is not an official Lossless Scaling, lsfg-vk, vkBasalt, or ReShade release. MAKO does not contain or distribute Lossless Scaling, `Lossless.dll`, or extracted proprietary model payloads. LSFG frame generation and LS1 scaling read selected resources at runtime from a lawful, user-supplied <a href="https://store.steampowered.com/app/993090/Lossless_Scaling/" target="_blank" rel="noopener noreferrer">Lossless Scaling</a> installation; the open MAKO Scaler and bundled shaders do not require it. MAKO does not alter the user's DLL file, and translated resources remain process-local. Users are responsible for complying with the terms applicable to their copy. See <a href="THIRD_PARTY_NOTICES.md" target="_blank" rel="noopener noreferrer">Third-party notices</a>.

## Downloads

<p align="center">
  <a href="https://ko-fi.com/F0Q429XXET"><img src="assets/kofi-button.svg" align="middle" hspace="4" alt="Support MAKO on Ko-fi" /></a>
  <a href="https://github.com/eugeniosegala/MAKO/releases" target="_blank" rel="noopener noreferrer"><img src="https://img.shields.io/github/downloads/eugeniosegala/MAKO/total?style=flat&amp;label=total%20downloads&amp;color=0f766e" width="201" height="30" align="middle" hspace="4" alt="Total downloads across all MAKO Renderer release assets" /></a>
</p>

| Component | Recommended for | Releases |
| --- | --- | --- |
| **MAKO Renderer** | SteamOS, Steam Machine, and desktop Linux (host archive, Arch package, and Flatpak extensions) | <a href="https://github.com/eugeniosegala/MAKO/releases/tag/render-v4.6.0" target="_blank" rel="noopener noreferrer">Latest MAKO Renderer release (Linux archive under Assets)</a> |

## Community

Join the official <a href="https://discord.gg/NAVkyCq7Rc" target="_blank" rel="noopener noreferrer">MAKO Discord</a> for discussion, testing, development, showcases, and live troubleshooting. GitHub remains the source of truth for <a href="https://github.com/eugeniosegala/MAKO/issues/new/choose" target="_blank" rel="noopener noreferrer">bug reports and feature requests</a>.

<!-- prettier-ignore -->
> [!TIP]
> **Want update alerts?** At the top-right of the <a href="https://github.com/eugeniosegala/MAKO" target="_blank" rel="noopener noreferrer">MAKO GitHub repository</a> page, click **Watch** > **Custom**, select **Releases**, then click **Apply**. GitHub will notify you when a new release is published, subject to your GitHub notification settings.

Published Renderer packages currently target x86_64 Linux hosts and include layers for both 64-bit and 32-bit x86 game processes. Native AArch64/Armada packages are not included in this release.

## ✨ Feature Highlights

<p align="center">
  <br />
  <a href="engine/docs/CONFIGURATION.md#frame-generation"><img src="assets/features/adaptive-frame-generation.webp" width="150" align="middle" alt="Adaptive Frame Generation" /></a>
  <a href="engine/docs/CONFIGURATION.md#spatial-scaling"><img src="assets/features/scaling.webp" width="150" align="middle" alt="Scaling" /></a>
  <a href="engine/docs/CONFIGURATION.md#shaders"><img src="assets/features/shaders.webp" width="150" align="middle" alt="Shaders" /></a>
  <a href="engine/docs/CONFIGURATION.md#hdr"><img src="assets/features/hdr.webp" width="150" align="middle" alt="Experimental HDR" /></a>
  <br /><br />
</p>

- **Full-quality frame generation:** Lossless Scaling models from your licensed installation, with per-game quality and performance controls.
- **Reduced ghosting:** Full-quality v2 with Lighter FG Model off can reduce ghosting; supported AMD GPUs gain extra safeguards. Results vary by game.
- **Spatial scaling:** LS1 Quality, LS1 Performance, or the open MAKO Scaler, used alone or alongside frame generation.
- **Per-game shaders:** Live sharpening, anti-aliasing, colour, cinematic, and retro effects through bundled vkBasalt.
- **Adaptive Frame Generation:** Target 30–240 FPS with a selectable 2x–5x generation ceiling.
- **Display-aware pacing:** Adapts frame delivery to the presentation plan and Gamescope VRR state.
- **Experimental HDR:** HDR support for Scaling, Frame Generation, and Shaders through Gamescope.
- **64-bit and 32-bit x86 support:** Matching Vulkan layers for native and Flatpak games.
- **Gamescope recovery:** Preserves real frames during presentation pressure and Steam-menu transitions.
- **Per-game profiles:** Saves renderer and compatibility settings in one TOML file, matched by executable or process name.
- **Native Steam Remote Play:** Opt-in frame generation, scaling, and shaders on the receiving device, controlled with `mako-remote-play`.

## What MAKO is

MAKO (**Motion-Adaptive Kernel Orchestration**) brings LSFG frame generation, spatial scaling, and bundled shader effects to Linux gaming. **MAKO Scaler and shaders work without Lossless Scaling; only Frame Generation and LS1 require it.** Scaling can run alone or alongside Fixed or Adaptive Frame Generation.

MAKO Renderer is the Vulkan layer that provides the graphics pipeline for frame generation and spatial scaling. Its packages also include optional bundled shader effects, a command-line tool, and a launcher for native Steam and Proton games.

## In-game considerations

<!-- prettier-ignore -->
> [!TIP]
> **Try the game's V-Sync setting both on and off.** Neither setting is universally best: the result depends on the game, its FPS cap, VRR, and the compositor. Keep whichever option feels smoother and more responsive for that game.

Every game and display behaves differently, so compare one setting at a time. If Frame Generation, Scaling, or Shaders do not work, try the game's Fullscreen, Borderless Fullscreen, and Windowed modes; none is universally best. For Scaling, also try different in-game resolutions and check that the input is smaller than the display. Changing display mode can change the resolution MAKO processes and its GPU cost. See the <a href="engine/docs/CONFIGURATION.md" target="_blank" rel="noopener noreferrer">configuration guide</a> for which settings apply live and which require a restart.

## Install and use

1. Install the default public version of <a href="https://store.steampowered.com/app/993090/Lossless_Scaling/" target="_blank" rel="noopener noreferrer">Lossless Scaling</a> from Steam if you will use frame generation or LS1 scaling. MAKO can use beta branches, but they are not validated; the default public branch is recommended. MAKO reads its licensed `Lossless.dll`; MAKO Scaler and Shaders work without it.
2. Download and extract `MAKO-Renderer-v<version>-linux.tar.xz` from the <a href="https://github.com/eugeniosegala/MAKO/releases/tag/render-v4.6.0" target="_blank" rel="noopener noreferrer">latest MAKO Renderer release</a> into a new folder. Run **Install MAKO Renderer** from that folder. The installer shows the launch option when it finishes.
3. Add a profile to `~/.config/mako-render/conf.toml`, naming the game's executable in `active_in`. The [configuration guide](engine/docs/CONFIGURATION.md) has a complete example, and `~/.local/bin/mako-cli validate` checks the file.
4. For a native Steam or Proton game, add this under **Steam Properties > General > Launch Options**, then use Steam's **Play** button normally:

    ```text
    ~/.local/bin/mako-launch %command%
    ```

5. Experiment with the settings to find what works best for each game. Try Fixed or Adaptive Frame Generation, scaling, and shader effects independently.
6. For Flatpak games, launchers, or emulators, follow the [standalone Flatpak guide](engine/docs/FLATPAK-GUIDE.md) to install the matching extension and prepare each app, then launch it normally. The host `mako-launch` command cannot replace sandbox setup.

Run the installer again to update. See the <a href="engine/README.md#direct-linux-installation" target="_blank" rel="noopener noreferrer">MAKO Renderer guide</a> for manual installation and removal, and the Arch package guide for pacman installations.

### Optional graphics integrations

Shaders use bundled vkBasalt; no separate installation is needed. MAKO also supports host-installed MangoHud and a per-profile **Gamescope WSI** compatibility option, independent of Scaling. See <a href="engine/docs/LAYER-CHAINING.md" target="_blank" rel="noopener noreferrer">optional graphics integrations</a> for ordering and limits.

### Diagnostics

For a step-by-step guide that creates a shareable diagnostics report, see <a href="COLLECT_DIAGNOSTICS.md" target="_blank" rel="noopener noreferrer">Collect MAKO diagnostics</a>.

## Documentation

These guides describe the selected branch. For a published version, select its `render-vX.Y.Z` tag on GitHub; a development branch may document features that are not in the latest download.

- <a href="engine/docs/CONFIGURATION.md" target="_blank" rel="noopener noreferrer">MAKO Renderer configuration</a>: profiles, game launch setup, and runtime behaviour.
- <a href="engine/docs/CONFIGURATION-REFERENCE.md" target="_blank" rel="noopener noreferrer">Configuration reference</a>: every `conf.toml` field, default, and environment variable.
- <a href="engine/README.md" target="_blank" rel="noopener noreferrer">MAKO Renderer</a>: direct installation, usage, builds, and architecture guides.
- <a href="COLLECT_DIAGNOSTICS.md" target="_blank" rel="noopener noreferrer">Diagnostics</a>: activation, presentation problems, and private reports.
- <a href="TESTING.md" target="_blank" rel="noopener noreferrer">Testing</a> and <a href="HOW_TO_RELEASE.md" target="_blank" rel="noopener noreferrer">releases</a>: contributor validation and publication.
- <a href="MAINTENANCE.md" target="_blank" rel="noopener noreferrer">Ongoing maintenance</a>: dependencies, version owners, upstream review triggers, and required checks.

## Credits and project lineage

MAKO builds on and integrates work from these open-source projects and their communities:

- **<a href="https://github.com/PancakeTAS/lsfg-vk" target="_blank" rel="noopener noreferrer">PancakeTAS</a>** and the **lsfg-vk contributors** created the GPL-3.0-or-later version 2 Vulkan layer and Linux integration from which MAKO Renderer descends. MAKO's direct upstream baseline is <a href="https://github.com/PancakeTAS/lsfg-vk/commit/8b0da2661c6f3473a7fccc8ba643880050e71642" target="_blank" rel="noopener noreferrer"><code>8b0da266</code></a>; the exact lineage and migration commits are recorded in <a href="LICENSE.md#lsfg-vk-renderer-lineage" target="_blank" rel="noopener noreferrer">LICENSE.md</a>.
- **<a href="https://github.com/DadSchoorse/vkBasalt" target="_blank" rel="noopener noreferrer">Georg Lehmann / DadSchoorse</a>** and the **vkBasalt contributors** created the Vulkan post-processing layer that MAKO bundles through its maintained fork. Selected shader effects come from <a href="https://github.com/CeeJayDK/SweetFX" target="_blank" rel="noopener noreferrer">CeeJayDK/SweetFX</a> and <a href="https://github.com/crosire/reshade-shaders" target="_blank" rel="noopener noreferrer">crosire/reshade-shaders</a>; exact source and license details are in <a href="THIRD_PARTY_NOTICES.md" target="_blank" rel="noopener noreferrer">Third-party notices</a>.

MAKO also thanks the **Lossless Scaling developers** for the LS1 and LSFG models accessed through each user's licensed installation, the **Wine/vkd3d developers** whose shader translator enables the Vulkan LS1 path, and the community contributors, testers, guide authors, and creators who helped make the project possible. The open MAKO Scaler is independently implemented in this repository.

The original copyright and license notices are preserved in <a href="LICENSE.md" target="_blank" rel="noopener noreferrer">LICENSE.md</a>. MAKO is an independent community project and is not affiliated with or endorsed by Lossless Scaling or the credited upstream projects.

## License

MAKO is distributed under <a href="LICENSE.md" target="_blank" rel="noopener noreferrer">GPL-3.0-or-later</a>. Required upstream notices and the user-supplied proprietary-component boundary are recorded in <a href="THIRD_PARTY_NOTICES.md" target="_blank" rel="noopener noreferrer">Third-party notices</a>, and visual sources are recorded in <a href="ASSET_PROVENANCE.md" target="_blank" rel="noopener noreferrer">Asset provenance</a>. Contributions are accepted under the policy in <a href="CONTRIBUTING.md" target="_blank" rel="noopener noreferrer">CONTRIBUTING.md</a>.

## AI-assisted development

MAKO uses coding agents as part of an evidence-driven engineering workflow while keeping architecture, review, validation, and release decisions under human ownership. See <a href="AI_USE.md" target="_blank" rel="noopener noreferrer">AI use in MAKO</a> for the full approach.

## Contributors

MAKO thanks everyone whose commits or co-authored work is in this repository. This gallery is generated from the current branch's Git history; upstream projects and other forms of help are credited above.

<!-- mako-contributors:start -->

<a href="https://github.com/eugeniosegala"><img src="https://github.com/eugeniosegala.png?size=48&amp;v=20261004" width="48" height="48" alt="@eugeniosegala" /></a> <a href="https://github.com/Hu2ki3"><img src="https://github.com/Hu2ki3.png?size=48&amp;v=20261004" width="48" height="48" alt="@Hu2ki3" /></a> <a href="https://github.com/hugouchoasborges"><img src="https://github.com/hugouchoasborges.png?size=48&amp;v=20261004" width="48" height="48" alt="@hugouchoasborges" /></a> <a href="https://github.com/lordkaus"><img src="https://github.com/lordkaus.png?size=48&amp;v=20261004" width="48" height="48" alt="@lordkaus" /></a> <a href="https://github.com/PJ-568"><img src="https://github.com/PJ-568.png?size=48&amp;v=20261004" width="48" height="48" alt="@PJ-568" /></a> <a href="https://github.com/rosakodu"><img src="https://github.com/rosakodu.png?size=48&amp;v=20261004" width="48" height="48" alt="@rosakodu" /></a> <a href="https://github.com/Tak-attack"><img src="https://github.com/Tak-attack.png?size=48&amp;v=20261004" width="48" height="48" alt="@Tak-attack" /></a> <a href="https://github.com/w169q169"><img src="https://github.com/w169q169.png?size=48&amp;v=20261004" width="48" height="48" alt="@w169q169" /></a> <a href="https://github.com/WowOne987"><img src="https://github.com/WowOne987.png?size=48&amp;v=20261004" width="48" height="48" alt="@WowOne987" /></a>
<!-- mako-contributors:end -->

See <a href="CONTRIBUTING.md">Contributing</a> to get involved.

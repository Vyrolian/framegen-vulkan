framegen-vulkan standalone archive
================================

framegen-vulkan provides Frame Generation and spatial scaling for Vulkan games on SteamOS and Linux. The archive includes matching 64-bit and 32-bit Vulkan layers, the command-line tools, and the managed installer.

Frame Generation and LS1 scaling require a user-owned installation of the default public version of Lossless Scaling from Steam. framegen-vulkan can use beta branches, but they are not validated; the default public branch is recommended. The open framegen-vulkan Scaler does not require Lossless.dll.

Install or update
-----------------

1. Close every game currently using framegen-vulkan.
2. Extract the entire archive into a new folder. Do not run the installer from an archive preview.
3. Double-click "Install framegen-vulkan" and choose "Execute" if your file manager asks.
4. Confirm the installation location. The installer verifies every managed payload file, safely updates an existing installation, and preserves your profiles.
5. When installation finishes, the installer shows the Steam launch option. Profiles live in ~/.config/framegen-vulkan/conf.toml; check them with ~/.local/bin/fgvk-cli validate.

Prepare and launch a game
-------------------------

Installing framegen-vulkan does not activate it for every game. A standalone native or Proton game needs both a matching profile and the launch command.

1. Add a profile to ~/.config/framegen-vulkan/conf.toml. Set active_in to the game's executable, for example Game.exe. Use the game's process, not its launcher or Steam display title. A minimal example:

    version = 2

    [global]
    allow_fp16 = true

    [[profile]]
    name = "My game"
    active_in = ["Game.exe"]
    frame_generation_provisioned = true
    frame_generation_enabled = true
    multiplier = 2

    scaling_enabled = false

2. Enable Frame Generation and/or scaling in the profile. Set dll to your Lossless.dll path if automatic discovery fails. To use the open framegen-vulkan Scaler without the DLL, set frame_generation_enabled = false, scaling_enabled = true, and scaling_method = "fgvk". Settings that need a restart take effect the next time the game starts.
3. Check the file with ~/.local/bin/fgvk-cli validate.

Run ~/.local/bin/fgvk with no arguments to open the settings window. It edits the same conf.toml file.

The full configuration guide is at https://github.com/eugeniosegala/MAKO/blob/main/engine/docs/CONFIGURATION.md.

For a native Steam or Proton game, add this under Steam Properties > General > Launch Options:

    ~/.local/bin/fgvk %command%

Keep %command% exactly as written. Set this once per game, then use Steam's Play button normally. If you selected a custom installation prefix, use the fgvk path shown by the installer's completion message instead.

For a direct desktop command, replace %command% with the actual game executable and arguments:

    ~/.local/bin/fgvk "/path/to/your-game"

Optional private vkBasalt post-processing
-----------------------------------------

The archive includes framegen-vulkan's pinned 64-bit and 32-bit vkBasalt payload. To add it after framegen-vulkan for one native Steam or Proton game, use this launch option:

    ENABLE_VKBASALT=1 ~/.local/bin/fgvk %command%

For the full vkBasalt option suite, create a standard config file and select it for that game:

    ENABLE_VKBASALT=1 VKBASALT_CONFIG_FILE="$HOME/.config/vkBasalt/game-name.conf" ~/.local/bin/fgvk %command%

The config path is optional. Renderer process matching does not select a vkBasalt file; assign a distinct VKBASALT_CONFIG_FILE in each game's launch option for per-game settings. Only framegen-vulkan's private bundled vkBasalt is used; a system installation is ignored. If the complete 64-bit/32-bit bundle or selected config is unavailable, the launcher reports it and safely continues with framegen-vulkan alone. Restart the game after changing activation or configuration. Flatpak applications require the separate setup guide below.

Flatpak games, launchers, and emulators require the matching framegen-vulkan runtime extension and preparation for each application. Installing the extension does not prepare Flatpaks. "Install framegen-vulkan Flatpak Extensions" installs extensions only. Follow the Flatpak setup link below for runtime selection, configuration/DLL access, and sandbox environment, then restart and launch the app normally. The host fgvk command does not replace that preparation.

Update
------

Download and fully extract the newer framegen-vulkan archive, close games using framegen-vulkan, and run its "Install framegen-vulkan" file. The installer replaces only the managed payload and preserves your profiles and settings.

The complete payload is verified and staged before any installed file is replaced. If a later update step fails, the installer restores the previous files and selected Renderer version. Check destination permissions and available disk space before retrying; do not run the user-local installer with sudo. If restoration also fails, the installer reports where it retained recovery backups.

Uninstall
---------

Run:

    ~/.local/bin/fgvk-installer --uninstall

The first confirmation removes the managed native Renderer while preserving modified files and your profiles. Shared Flatpak runtime extensions remain installed.

The uninstaller then separately asks whether to remove profiles, settings, and diagnostics from ~/.config/framegen-vulkan. Declining keeps them for a later installation. Accepting permanently removes that directory and its contents.

Documentation and support
-------------------------

Direct installation and usage:
https://github.com/eugeniosegala/MAKO/blob/main/engine/README.md

Flatpak setup:
https://github.com/eugeniosegala/MAKO/blob/main/engine/docs/FLATPAK-GUIDE.md

Troubleshooting:
https://github.com/eugeniosegala/MAKO/blob/main/engine/docs/TROUBLESHOOTING.md

Diagnostics:
https://github.com/eugeniosegala/MAKO/blob/main/engine/docs/COLLECT_DIAGNOSTICS.md

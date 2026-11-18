framegen-vulkan standalone archive
================================

framegen-vulkan provides Frame Generation for Vulkan games on SteamOS and Linux. The archive includes matching 64-bit and 32-bit Vulkan layers, the command-line tools, and the managed installer.

Frame Generation requires a user-owned installation of the default public version of Lossless Scaling from Steam. framegen-vulkan can use beta branches, but they are not validated; the default public branch is recommended.

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

2. Enable Frame Generation in the profile. Set dll to your Lossless.dll path if automatic discovery fails. Settings that need a restart take effect the next time the game starts.
3. Check the file with ~/.local/bin/fgvk-cli validate.

Run ~/.local/bin/fgvk with no arguments to open the settings window. It edits the same conf.toml file.

For a native Steam or Proton game, add this under Steam Properties > General > Launch Options:

    ~/.local/bin/fgvk %command%

Keep %command% exactly as written. Set this once per game, then use Steam's Play button normally. If you selected a custom installation prefix, use the fgvk path shown by the installer's completion message instead.

For a direct desktop command, replace %command% with the actual game executable and arguments:

    ~/.local/bin/fgvk "/path/to/your-game"

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

In the framegen-vulkan source repository, see:

  README.md                           Installation and launch
  engine/docs/CONFIGURATION.md         Profiles and settings
  engine/docs/FLATPAK-GUIDE.md         Flatpak setup
  engine/docs/TROUBLESHOOTING.md       Troubleshooting
  engine/docs/COLLECT_DIAGNOSTICS.md   Diagnostics

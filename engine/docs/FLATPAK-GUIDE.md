# Flatpak guide

Complete the steps below for each app. Installing the native Renderer or creating a profile alone does not prepare a Flatpak.

## Prepare a Renderer profile

In `~/.config/framegen-vulkan/conf.toml`, set `active_in` to the game's executable, such as `Game.exe` or `dolphin-emu`, and enable the features you want. Match the executable, not the Flatpak application ID.

Frame Generation requires Lossless Scaling's default Steam version: set `dll` to its absolute `Lossless.dll` path. Without it, turn Frame Generation off.

## Identify the application and runtime

List apps:

```bash
flatpak list --app --columns=application,name,runtime
```

Replace Dolphin's ID below with your app's ID. Keep this terminal open for the remaining commands:

```bash
appid=org.DolphinEmu.dolphin-emu
flatpak info --show-runtime "$appid"
```

framegen-vulkan provides a Freedesktop **26.08** extension on x86_64 with 64-bit and 32-bit game layers.

For KDE/GNOME runtimes, inspect the metadata:

```bash
fgvk_app_runtime=$(flatpak info --show-runtime "$appid")
flatpak info --show-metadata "$fgvk_app_runtime"
```

Check for `26.08` in the `version=` or `versions=` entry under `[Extension org.freedesktop.Platform.VulkanLayer]`.

## Packaged extensions

Download and extract `framegen-vulkan-v<version>.zip` from the matching release and open its `flatpak` folder (releases up to v1.1.0: `framegen-vulkan-v<version>-flatpak.tar.xz`). You can also [build the bundle locally](BUILDING-FROM-SOURCE.md#choose-the-right-build-path). Run the installer from the extracted folder, then prepare each app below.

```bash
./install-flatpak
```

The original v1.0.0 archive has the suffix `-flatpaks.tar.xz` and uses `"./Install framegen-vulkan Flatpak Extensions"`.

To install the 26.08 bundle directly, run the following from the extracted folder.

```bash
flatpak install --user ./org.freedesktop.Platform.VulkanLayer.fgvkrender-26.08.flatpak
```

## Manual application override

Fully close the app and its games. Grant configuration access:

```bash
flatpak override --user --filesystem="$HOME/.config/framegen-vulkan:rw" "$appid"
flatpak override --user --env=FGVK_CONFIG="$HOME/.config/framegen-vulkan/conf.toml" "$appid"
```

For Frame Generation, replace the example with the folder containing your configured `Lossless.dll`. Otherwise, skip this block:

```bash
fgvk_dll_directory="/absolute/path/to/steamapps/common/Lossless Scaling"
flatpak override --user --filesystem="$fgvk_dll_directory:ro" "$appid"
```

Apply the activation settings:

```bash
flatpak override --user --env=ENABLE_FGVK=1 "$appid"
flatpak override --user --env=DISABLE_LSFG=1 "$appid"
flatpak override --user --env=DISABLE_LSFGVK=1 "$appid"
flatpak override --user --env=DISABLE_VKBASALT=1 "$appid"
```

Do not override `VK_IMPLICIT_LAYER_PATH`: the runtime already finds the extension's layer, and replacing the path also hides the NVIDIA driver's own layers. These settings persist for this app. Repeat with another `appid` for each additional app.

- **Heroic/Lutris:** prepare the launcher and match each game's executable. Avoid an app-wide `FGVK_PROFILE`; no host wrapper is needed.
- **EmuDeck:** prepare each Flatpak emulator, select Vulkan, and keep its Steam shortcut and ROM arguments. Native/AppImage emulators use [native launch setup](../../README.md#launch-a-game).

## Launch and verify

Restart the app and launch normally, or run:

```bash
flatpak run "$appid"
```

Use Vulkan, including DXVK/VKD3D for compatible Windows games.

If framegen-vulkan does not activate, inspect the overrides and sandbox access:

```bash
flatpak override --user --show "$appid"
```

```bash
flatpak run --command=sh "$appid" -c '
    ls /usr/lib/extensions/vulkan/fgvkrender/share/vulkan/implicit_layer.d &&
    test -r "$FGVK_CONFIG" && echo "framegen-vulkan configuration is readable"
'
```

Missing manifests usually mean the extension or runtime branch is wrong. An unreadable configuration needs its path or access grant corrected. Also check executable matching, enabled features, and DLL access. For a game report, [collect diagnostics](COLLECT_DIAGNOSTICS.md).

## Updates and disabling framegen-vulkan

Native Renderer updates do not update Flatpak extensions. Close apps and rerun the newer Flatpak archive's installer for each branch you use. Recheck the branch if an app changes runtime.

Bypass framegen-vulkan for one launch of a closed app:

```bash
flatpak run --env=DISABLE_FGVK=1 "$appid"
```

Keep framegen-vulkan disabled:

```bash
flatpak override --user --env=DISABLE_FGVK=1 "$appid"
```

Re-enable and restart:

```bash
flatpak override --user --unset-env=DISABLE_FGVK "$appid"
```

Avoid `flatpak override --reset`: it also removes unrelated app settings.

## Uninstall the Flatpak extension

Close the Flatpak apps and games using framegen-vulkan. For each installed runtime branch, run the uninstall command and confirm removal. This command removes branch 26.08:

```bash
flatpak uninstall --user org.freedesktop.Platform.VulkanLayer.fgvkrender//26.08
```

Use `flatpak list --user --runtime --columns=application,branch` to find the installed branches. Removing the extension keeps your framegen-vulkan profiles and each app's overrides. Check `flatpak override --user --show "$appid"` and undo the settings you added for framegen-vulkan, including any variables that disabled other layers.

## Building extensions

See [Flatpak extension builds](BUILDING-FROM-SOURCE.md#flatpak-extension-builds) for source builds and runtime catalogue maintenance.

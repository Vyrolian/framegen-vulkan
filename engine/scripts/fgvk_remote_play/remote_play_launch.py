#!/usr/bin/python3
# FGVK_NATIVE_REMOTE_PLAY_OVERRIDE_V3
"""Self-contained Steam entry point; installed independently of the profile editor."""

import fcntl
import hashlib
import json
import os
import re
import stat
from pathlib import Path
import sys
from typing import Callable, Mapping, TypedDict


class LaunchPaths(TypedDict):
    original: Path
    checksum: Path
    state: Path
    config: Path
    runner: Path
    lock: Path
    sdr_helper: Path


class LaunchSettings(TypedDict):
    FGVK_LAUNCH_RENDERER_REQUIRED: str
    FGVK_LAUNCH_SPATIAL_REQUIRED: str
    FGVK_LAUNCH_WSI_REQUIRED: str
    FGVK_LAUNCH_EXTERNAL_LAYER: str


# Native streaming uses Vulkan. Never inherit a launcher's OpenGL/Zink override.
REMOVED_ENVIRONMENT = (
    "GALLIUM_DRIVER", "MESA_LOADER_DRIVER_OVERRIDE", "__GLX_VENDOR_LIBRARY_NAME",
    "ENABLE_FGVK", "FGVK_PROFILE", "FGVK_PROFILE_FALLBACK", "FGVK_CONFIG",
    "FGVK_FOLLOW_CURRENT_PROFILE", "FGVK_ENV",
    "ENABLE_VKBASALT", "VKBASALT_CONFIG_FILE", "FGVK_PRESENT_DIAGNOSTICS",
    "ENABLE_FGVK_SPATIAL_SCALING", "DISABLE_FGVK_SPATIAL_SCALING",
    "ENABLE_GAMESCOPE_WSI", "FGVK_SPLIT_LAYER_CHAIN", "MANGOHUD",
    "VKBASALT_CONFIG_RELOAD", "FGVK_EXTERNAL_VULKAN_LAYER",
    "FGVK_REMOTE_PLAY_SDR",
)
SDR_HELPER_RELATIVE_PATH = 'share/framegen-vulkan/fgvk_remote_play/libfgvk-remote-play-sdr.so'
SDR_HELPER_MARKER = b'FGVK_REMOTE_PLAY_SDR_HELPER_V1'
MANAGED_LAYERS = (
    "VK_LAYER_FGVK_render", "VK_LAYER_FGVK_spatial_scaling",
    "VK_LAYER_FROG_gamescope_wsi_x86_64", "VK_LAYER_VKBASALT_post_processing",
    "VK_LAYER_MANGOHUD_overlay_x86_64", "VK_LAYER_MANGOHUD_overlay_x86",
)
PASSTHROUGH_DISABLE_ENVIRONMENT = (
    "DISABLE_FGVK", "DISABLE_FGVK_SPATIAL_SCALING", "DISABLE_GAMESCOPE_WSI",
    "DISABLE_VKBASALT", "DISABLE_MANGOHUD",
)


def open_regular(path: Path):
    """Reject redirected/special launch inputs without waiting on a FIFO."""
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    stream = os.fdopen(descriptor, "rb")
    if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
        stream.close()
        raise ValueError("launch input is not a regular file")
    return stream


def read_record(path: Path) -> str:
    with open_regular(path) as stream:
        content = stream.read(4097)
        if len(content) > 4096:
            raise ValueError("launch record is too large")
        return content.decode("utf-8")


def check_sdr_helper(path: Path) -> None:
    """Reject missing, redirected, wrong-architecture or stale helper payloads."""
    if not path.is_absolute() or re.search(r'[\s:]', str(path)):
        raise ValueError('Remote Play SDR helper path cannot contain loader separators')
    with open_regular(path) as stream:
        payload = stream.read(1024 * 1024 + 1)
    if (len(payload) > 1024 * 1024 or len(payload) < 20 or
            payload[:6] != b'\x7fELF\x02\x01' or payload[16:20] != b'\x03\0\x3e\0' or
            SDR_HELPER_MARKER not in payload):
        raise ValueError('Install the current framegen-vulkan Remote Play SDR helper')


def without_sdr_preload(value: str) -> str:
    """Remove only framegen-vulkan's helper, preserving Steam and user preload entries."""
    return ' '.join(entry for entry in re.split(r'[\s:]+', value) if entry and
                    Path(entry).name != Path(SDR_HELPER_RELATIVE_PATH).name)


def selected_profile(config: Path) -> dict:
    """Resolve the saved editor selection once, before launching a stream."""
    # Disabled status and activation's interpreter check also run on Python 3.10.
    try:
        import tomllib
    except ImportError as error:
        raise ValueError("Remote Play requires a working system Python 3.11 or newer") from error
    with open_regular(config) as stream:
        content = stream.read().decode("utf-8")
    data = tomllib.loads(content)
    if data.get("version") != 2:
        raise ValueError("unsupported configuration version")
    profiles = data.get("profile", [])
    if not isinstance(profiles, list) or not profiles or any(
        not isinstance(profile, dict) or not isinstance(profile.get("name"), str)
        or not profile["name"] for profile in profiles
    ):
        raise ValueError("selected profile unavailable")
    name = data.get("current_profile")
    return next((profile for profile in profiles if profile["name"] == name),
                next((profile for profile in profiles if profile["name"] == "fgvk"), profiles[0]))


def launch_settings(profile: dict, config: Path) -> LaunchSettings:
    """Read only process-start discovery inputs; Renderer owns actual settings."""
    modes = [profile]
    for mode in ('handheld', 'docked'):
        if mode in profile:
            if not isinstance(profile[mode], dict):
                raise ValueError('invalid power settings')
            modes.append(profile[mode])
    def required(field: str, default: bool) -> bool:
        values = [mode.get(field, profile.get(field, default)) for mode in modes]
        if any(type(value) is not bool for value in values):
            raise ValueError('invalid layer discovery setting')
        return any(values)
    sidecar = config.parent / 'profile-wrapper-settings.json'
    settings = {}
    if sidecar.exists() or sidecar.is_symlink():
        with open_regular(sidecar) as stream:
            root = json.load(stream)
        if not isinstance(root, dict) or root.get('version') != 1 or not isinstance(root.get('profiles'), dict):
            raise ValueError('invalid wrapper settings')
        settings = root['profiles'].get(profile['name'], {})
        if not isinstance(settings, dict):
            raise ValueError('invalid profile wrapper settings')
    external = settings.get('external_vulkan_layer', '')
    if external == 'vkbasalt':
        # vkBasalt is no longer bundled; an old saved choice means no external layer.
        external = ''
    if external not in ('', 'mangohud'):
        raise ValueError('unsupported external layer')
    wsi = settings.get('gamescope_wsi_compatibility', False)
    if type(wsi) is not bool:
        raise ValueError('invalid WSI setting')
    scaling = required('scaling_enabled', False)
    return LaunchSettings(
        FGVK_LAUNCH_RENDERER_REQUIRED='1' if required('frame_generation_provisioned', True) or scaling else '0',
        FGVK_LAUNCH_SPATIAL_REQUIRED='1' if scaling and wsi else '0',
        FGVK_LAUNCH_WSI_REQUIRED='1' if wsi else '0',
        FGVK_LAUNCH_EXTERNAL_LAYER=external,
    )


def launch(paths: LaunchPaths, arguments: list[str], execute: Callable,
           environment: Mapping[str, str] | None = None, *,
           allow_override: bool = True) -> int:
    """Validate the original, then delegate feature/power setup to the framegen-vulkan launcher."""
    clean = dict(os.environ if environment is None else environment)
    for key in REMOVED_ENVIRONMENT:
        clean.pop(key, None)
    if 'LD_PRELOAD' in clean:
        preload = without_sdr_preload(clean['LD_PRELOAD'])
        if preload:
            clean['LD_PRELOAD'] = preload
        else:
            clean.pop('LD_PRELOAD')
    clean.update({key: "1" for key in PASSTHROUGH_DISABLE_ENVIRONMENT})
    if "VK_INSTANCE_LAYERS" in clean:
        clean["VK_INSTANCE_LAYERS"] = ":".join(
            layer for layer in clean["VK_INSTANCE_LAYERS"].split(":")
            if layer not in MANAGED_LAYERS
        )
    for key in ('FGVK_LAUNCH_RENDERER_REQUIRED', 'FGVK_LAUNCH_SPATIAL_REQUIRED', 'FGVK_LAUNCH_WSI_REQUIRED',
                'FGVK_LAUNCH_EXTERNAL_LAYER'):
        clean.pop(key, None)
    original = paths["original"]
    try:
        if original.is_symlink() or not original.is_file() or not os.access(original, os.X_OK):
            raise ValueError("original client unavailable")
        with open_regular(original) as stream:
            header = stream.read(20)
            if len(header) < 20 or header[:6] != b"\x7fELF\x02\x01" or header[18:20] != b"\x3e\0":
                raise ValueError("original client is not native x86_64 ELF")
            stream.seek(0)
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if digest != read_record(paths["checksum"]).strip():
            raise ValueError("original client checksum mismatch")
    except (OSError, ValueError) as error:
        print(f"framegen-vulkan: Remote Play original validation failed: {error}", file=sys.stderr)
        return 126

    try:
        if not allow_override:
            raise ValueError("override launch lock unavailable")
        state = json.loads(read_record(paths["state"]))
        if not isinstance(state, dict) or type(state.get('version')) is not int or state.get("version") != 3 or state.get("enabled") is not True:
            raise ValueError("override disabled")
        profile = selected_profile(paths["config"])
        profile_name = profile["name"]
        if not paths["runner"].is_file() or not os.access(paths["runner"], os.X_OK):
            raise ValueError("framegen-vulkan launcher unavailable")
        check_sdr_helper(paths['sdr_helper'])
        settings = launch_settings(profile, paths['config'])
        active = dict(clean)
        active.pop("DISABLE_FGVK", None)
        active.pop("DISABLE_FGVK_SPATIAL_SCALING", None)
        active["FGVK_PROFILE"] = profile_name
        active["FGVK_FOLLOW_CURRENT_PROFILE"] = '1'
        active['FGVK_CONFIG'] = str(paths['config'])
        if paths['runner'].name != 'fgvk-launch':
            active.update(settings)
        # Existing launchers own layer setup and disabled-feature passthrough;
        # the Renderer owns settings, model availability, and power selection.
        command = [str(paths["runner"]), "/usr/bin/env",
                   "-u", "GALLIUM_DRIVER", "-u", "MESA_LOADER_DRIVER_OVERRIDE",
                   "-u", "__GLX_VENDOR_LIBRARY_NAME", "FGVK_REMOTE_PLAY_SDR=1",
                   'LD_PRELOAD=' + ' '.join(filter(None, (str(paths['sdr_helper']), clean.get('LD_PRELOAD')))),
                   str(original), *arguments]
        execute(command[0], command, active)
        return 0
    except (OSError, ValueError, TypeError, KeyError) as error:
        print(f"framegen-vulkan: Remote Play passthrough: {error}", file=sys.stderr)
    try:
        execute(str(original), [str(original), *arguments], clean)
        return 0
    except OSError as error:
        print(f"framegen-vulkan: Remote Play original launch failed: {error}", file=sys.stderr)
        return 126


def main() -> int:
    root = Path(__file__).resolve().parent
    home = Path('__FGVK_USER_HOME__')
    config_dir = home / ".config/framegen-vulkan"
    paths = LaunchPaths(
        original=root / "streaming_client.fgvk-original",
        checksum=root / "streaming_client.fgvk-original.sha256",
        state=config_dir / "native-remote-play.json",
        config=Path('__FGVK_CONFIG_PATH__'),
        runner=Path('__FGVK_RUNNER_PATH__'),
        lock=config_dir / "native-remote-play.lock",
        sdr_helper=Path('__FGVK_SDR_HELPER_PATH__'),
    )
    # Keep the shared lease through the exec chain and the stream lifetime.
    # This also protects the handoff while fgvk-launch is still preparing its environment.
    try:
        descriptor = os.open(paths["lock"], os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK)
        with os.fdopen(descriptor, "r") as lock:
            identity = os.fstat(lock.fileno())
            if not stat.S_ISREG(identity.st_mode) or identity.st_nlink != 1:
                raise OSError("launch lock is not a private regular file")
            os.set_inheritable(descriptor, True)
            fcntl.flock(lock, fcntl.LOCK_SH)
            return launch(paths, sys.argv[1:], os.execve)
    except OSError as error:
        print(f"framegen-vulkan: Remote Play launch lock unavailable: {error}", file=sys.stderr)
        return launch(paths, sys.argv[1:], os.execve, allow_override=False)


if __name__ == "__main__":
    sys.exit(main())

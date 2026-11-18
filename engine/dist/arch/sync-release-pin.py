#!/usr/bin/env python3
"""Synchronize the Arch recipe with the verified framegen-vulkan release pin."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


def _replace_exactly_once(
    contents: str,
    pattern: str,
    replacement: str,
    field: str,
    pkgbuild_path: Path,
) -> str:
    updated, replacements = re.subn(
        pattern, replacement, contents, flags=re.MULTILINE
    )
    if replacements != 1:
        raise ValueError(
            f"Expected exactly one {field} assignment in {pkgbuild_path}"
        )
    return updated


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("pin", type=Path)
    parser.add_argument("pkgbuild", type=Path)
    args = parser.parse_args()
    pin_path, pkgbuild_path = args.pin, args.pkgbuild
    manifest = json.loads(pin_path.read_text(encoding="utf-8"))
    binaries = manifest.get("remote_binary")
    if not isinstance(binaries, list) or len(binaries) > 1:
        raise ValueError(f"{pin_path} must contain at most one remote_binary entry")

    original = pkgbuild_path.read_text(encoding="utf-8")
    if binaries:
        binary = binaries[0]
        if not isinstance(binary, dict):
            raise ValueError(f"Invalid remote_binary entry in {pin_path}")
        updated = apply_pin(original, binary, pin_path, pkgbuild_path)
        status = f"framegen-vulkan {binary['version']}"
    else:
        updated = original
        for field, line in (
            ("url", 'url=""'),
            ("source", "source=()"),
            ("sha256sums", "sha256sums=()"),
        ):
            updated = _replace_exactly_once(updated, rf"^{field}=.*$", line, field, pkgbuild_path)
        status = "no public release"

    if updated != original:
        if args.check:
            raise SystemExit(
                f"{pkgbuild_path} does not match {pin_path}. Run scripts/dev.sh sync-arch-package."
            )
        pkgbuild_path.write_text(updated, encoding="utf-8")
    print(f"Arch package pin: {status}.")
    return 0


def apply_pin(contents: str, binary: dict, pin_path: Path, pkgbuild_path: Path) -> str:
    version = binary.get("version")
    checksum = binary.get("sha256hash")
    if not isinstance(version, str) or not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ValueError(f"Invalid Renderer version in {pin_path}: {version!r}")
    if not isinstance(checksum, str) or not re.fullmatch(
        r"[0-9a-fA-F]{64}", checksum
    ):
        raise ValueError(f"Invalid Renderer SHA-256 in {pin_path}")

    repository = binary.get("source_repository")
    if (
        not isinstance(repository, str)
        or not re.fullmatch(r"https://github\.com/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+", repository)
        or repository.lower() == "https://github.com/eugeniosegala/mako"
    ):
        raise ValueError(f"Invalid framegen-vulkan repository in {pin_path}")
    expected_name = f"framegen-vulkan-v{version}-linux.tar.xz"
    expected_tag = f"render-v{version}"
    expected_url = f"{repository}/releases/download/{expected_tag}/{expected_name}"
    if (
        binary.get("name") != expected_name
        or binary.get("release_tag") != expected_tag
        or binary.get("url") != expected_url
    ):
        raise ValueError(
            f"{pin_path} does not describe the canonical {expected_name} release asset"
        )

    current_version_match = re.search(r"^pkgver=(.+)$", contents, re.MULTILINE)
    if current_version_match is None:
        raise ValueError(f"Could not find pkgver in {pkgbuild_path}")

    updated = _replace_exactly_once(
        contents,
        r"^pkgver=.*$",
        f"pkgver={version}",
        "pkgver",
        pkgbuild_path,
    )
    if current_version_match.group(1) != version:
        updated = _replace_exactly_once(
            updated,
            r"^pkgrel=.*$",
            "pkgrel=1",
            "pkgrel",
            pkgbuild_path,
        )
    source = f'{repository}/releases/download/render-v${{pkgver}}/framegen-vulkan-v${{pkgver}}-linux.tar.xz'
    for field, line in (
        ("url", f'url="{repository}"'),
        ("source", f'source=("{source}")'),
        ("sha256sums", f"sha256sums=('{checksum.lower()}')"),
    ):
        updated = _replace_exactly_once(updated, rf"^{field}=.*$", line, field, pkgbuild_path)
    return updated


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Inspect or compare creation requests from framegen-vulkan's opt-in full frame trace.

This compares enabled setup at a framegen-vulkan layer boundary, not extension use, pixels,
driver capability, frame cadence, or successful reproduction of a game fault.
"""

import argparse
from collections import defaultdict
import json
from pathlib import Path
import re


PREFIX = "framegen-vulkan: present diagnostics: operation=pacing-profile-vulkan-setup "
FEATURES = ("presentId", "presentWait", "presentId2", "presentWait2",
            "swapchainMaintenance1", "descriptorHeap", "presentMetering")


def read_setup(path):
    groups = defaultdict(list)
    invalid = 0
    gaps = 0
    with Path(path).open(encoding="utf-8", errors="replace") as stream:
        for ordinal, line in enumerate(stream):
            if "operation=pacing-profile-capture-gap " in line or "operation=pacing-profile-log-wrap " in line:
                gaps += 1
            if not line.startswith(PREFIX):
                continue
            try:
                fields = dict(token.split("=", 1) for token in line[len(PREFIX):].split())
                if fields["version"] != "1" or fields["kind"] not in ("instance", "device", "swapchain", "surface"):
                    raise ValueError("unsupported setup record")
                if fields["boundary"] not in ("incoming", "forwarded") or fields["role"] not in ("frame-generation", "spatial-scaling", "combined"):
                    raise ValueError("unsupported boundary")
                for key in ("pid", "call", "result"):
                    fields[key] = int(fields[key])
                if fields["pid"] <= 0 or fields["call"] <= 0:
                    raise ValueError("invalid identity")
                if not re.fullmatch(r"(?:0x[0-9a-fA-F]+|0)", fields["handle"]):
                    raise ValueError("invalid handle")
                if fields["kind"] in ("instance", "device"):
                    count = int(fields["extension_count"])
                    names = [] if fields["extensions"] == "-" else fields["extensions"].split(",")
                    if count < 0 or fields["extensions_truncated"] not in ("0", "1") or any(
                            not re.fullmatch(r"VK_[a-zA-Z0-9_]+", name) for name in names):
                        raise ValueError("invalid extensions")
                    if fields["extensions_truncated"] == "0" and count != len(names):
                        raise ValueError("incomplete extension list")
                if fields["kind"] != "surface":
                    if fields["pnext_truncated"] not in ("0", "1"):
                        raise ValueError("invalid chain state")
                    if fields["pnext"] != "-":
                        for stype in fields["pnext"].split(","):
                            int(stype)
                if any(fields[key] not in ("0", "1") for key in FEATURES if key in fields):
                    raise ValueError("invalid feature value")
                fields["ordinal"] = ordinal
                groups[(fields["pid"], fields["role"], fields["call"])].append(fields)
            except (KeyError, ValueError):
                invalid += 1
    return groups, invalid, gaps


def snapshot(path, pid=None, role="frame-generation"):
    groups, invalid, gaps = read_setup(path)
    pids = sorted({key[0] for key in groups if key[1] == role})
    if pid is None:
        if len(pids) > 1:
            raise ValueError(f"multiple PIDs for {role}: {pids}; select --pid / --other-pid")
        pid = pids[0] if pids else None
    records = []
    incomplete = 0
    for (owner, layer, _), rows in groups.items():
        if owner != pid or layer != role:
            continue
        boundaries = [r["boundary"] for r in rows]
        surface = len(rows) == 1 and rows[0]["kind"] == "surface" and boundaries == ["forwarded"]
        if not surface and (sorted(boundaries) != ["forwarded", "incoming"] or
                len({(r["kind"], r["handle"], r["result"]) for r in rows}) != 1):
            incomplete += 1
            continue
        records.extend(rows)
    records.sort(key=lambda r: r["ordinal"])
    swapchains = [r for r in records if r["kind"] == "swapchain" and r["boundary"] == "forwarded" and r["result"] == 0]
    selected = swapchains[-1] if swapchains else None
    device = next((r for r in reversed(records) if selected and r["kind"] == "device" and
                   r["boundary"] == "forwarded" and r["result"] == 0 and
                   r["handle"] == selected.get("device") and r["ordinal"] < selected["ordinal"]), None)
    chosen = {}
    for r in records:
        if (selected and r["call"] == selected["call"]) or (device and r["call"] == device["call"]):
            chosen[r["kind"] + "." + r["boundary"]] = r
    return dict(path=str(path), pid=pid, role=role, available=bool(selected and device),
                selection="last successful lower swapchain and its preceding device; verify this is the gameplay surface",
                malformed_records=invalid, incomplete_calls=incomplete, capture_gaps=gaps,
                successful_swapchain_creates=len(swapchains), selected=chosen,
                surfaces=[r for r in records if r["kind"] == "surface"],
                instances=[r for r in records if r["kind"] == "instance"],
                unavailable=[] if selected and device else ["swapchain/device setup missing; old captures cannot recover it"])


def fields_for_comparison(record):
    if not record:
        return None
    keys = ("flags", "pnext", "pnext_truncated", "queues", "queues_truncated", "extension_count",
            "extensions_truncated", "min_images", "format", "color_space", "width", "height",
            "layers", "usage", "sharing", "transform", "alpha", "present_mode", "clipped") + FEATURES
    result = {key: record[key] for key in keys if key in record}
    if "extensions" in record:
        result["extensions"] = ("unavailable (truncated)" if record.get("extensions_truncated") != "0" else
                                sorted(record["extensions"].split(",")) if record["extensions"] != "-" else [])
    return result


def compare(left, right):
    differences = []
    for boundary in ("device.incoming", "device.forwarded", "swapchain.incoming", "swapchain.forwarded"):
        a = fields_for_comparison(left["selected"].get(boundary))
        b = fields_for_comparison(right["selected"].get(boundary))
        if a is None or b is None:
            differences.append(dict(boundary=boundary, evidence="unavailable", left=a, right=b))
            continue
        for field, flag in (("pnext", "pnext_truncated"), ("extensions", "extensions_truncated"), ("queues", "queues_truncated")):
            if a.get(flag) == "1" or b.get(flag) == "1":
                differences.append(dict(boundary=boundary, field=field, evidence="unavailable (truncated)"))
        for key in sorted(a.keys() | b.keys()):
            if a.get(key) != b.get(key):
                differences.append(dict(boundary=boundary, field=key,
                                        left=a.get(key, "unavailable"), right=b.get(key, "unavailable")))
    return differences


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--compare", type=Path)
    parser.add_argument("--pid", type=int)
    parser.add_argument("--other-pid", type=int)
    parser.add_argument("--role", default="frame-generation")
    args = parser.parse_args()
    try:
        left = snapshot(args.log, args.pid, args.role)
        output = dict(left=left, limitation=__doc__.strip())
        if args.compare:
            right = snapshot(args.compare, args.other_pid, args.role)
            output.update(right=right, differences=compare(left, right))
        print(json.dumps(output, indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, f"Cannot inspect setup: {error}\n")


if __name__ == "__main__":
    main()

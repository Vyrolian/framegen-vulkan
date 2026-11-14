#!/usr/bin/env python3
"""Check the recorded Gamescope protocol boundary without network access or writes."""

from __future__ import annotations

import argparse
from datetime import date
import hashlib
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


ENGINE = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def check(engine: Path, upstream_protocol: Path | None) -> None:
    record = json.loads((engine / "gamescope-compatibility.json").read_text())
    require(record["schema_version"] == 1, "unsupported compatibility record schema")
    require(record["repository"] == "ValveSoftware/gamescope", "unexpected upstream repository")
    protocol = record["protocol"]
    require(protocol["path"] == "protocol/gamescope-swapchain.xml", "unexpected protocol path")
    require(re.fullmatch(r"[0-9a-f]{40}", protocol["source_commit"]) is not None,
            "protocol provenance needs a full commit")
    require(re.fullmatch(r"[0-9a-f]{64}", protocol["sha256"]) is not None,
            "protocol provenance needs a SHA-256")
    versions = protocol["supported_versions"]
    require(isinstance(versions, list) and bool(versions) and
            all(type(value) is int and value > 0 for value in versions),
            "supported_versions must contain positive integers")
    require(versions == list(range(1, max(versions) + 1)),
            "supported_versions must record the contiguous legacy-to-current range")
    maximum = max(versions)
    review = record["last_source_review"]
    date.fromisoformat(review["date"])
    require(re.fullmatch(r"[0-9a-f]{40}", review["commit"]) is not None,
            "source review needs an immutable commit")
    require(protocol["path"] in review["files"] and bool(review["scope"]) and bool(review["result"]),
            "source review needs its files, scope, and result")
    require(isinstance(record["hardware_validations"], list), "hardware_validations must be a list")

    notices = (engine.parent / "THIRD_PARTY_NOTICES.md").read_text()
    provenance = f'https://github.com/{record["repository"]}/blob/{protocol["source_commit"]}/{protocol["path"]}'
    require(provenance in notices, "third-party protocol provenance differs from the compatibility record")

    if upstream_protocol is not None:
        payload = upstream_protocol.read_bytes()
        require(hashlib.sha256(payload).hexdigest() == protocol["sha256"],
                "upstream XML differs from the recorded protocol SHA-256")
        root = ET.fromstring(payload)
        for key in ("factory_interface", "swapchain_interface"):
            interface = root.find(f'interface[@name="{protocol[key]}"]')
            require(interface is not None and int(interface.attrib["version"]) == maximum,
                    f"upstream {key} version differs from the record")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream-protocol", type=Path,
                        help="also verify a locally downloaded XML from the recorded protocol source commit")
    args = parser.parse_args()
    try:
        check(ENGINE, args.upstream_protocol)
    except (OSError, ValueError, KeyError, TypeError, ET.ParseError) as error:
        print(f"Gamescope compatibility check failed: {error}", file=sys.stderr)
        return 1
    print("Gamescope compatibility record is consistent" +
          (" and the supplied upstream protocol." if args.upstream_protocol else "."))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

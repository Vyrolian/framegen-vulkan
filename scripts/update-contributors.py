#!/usr/bin/env python3
"""Generate CONTRIBUTORS.md from commit authors and co-author trailers."""

import argparse
import json
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTRIBUTORS = ROOT / "CONTRIBUTORS.md"
CONFIG = ROOT / "scripts/contributors.json"
COAUTHOR = re.compile(r"^Co-authored-by:\s*([^\n<>]+?)\s*<[^\n<>]+>\s*$", re.IGNORECASE | re.MULTILINE)
LOGIN = re.compile(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,37}[A-Za-z0-9])?")


def git_names(mako_base):
    result = subprocess.run(
        ["git", "log", "--no-merges", "--format=%an%x1f%B%x1e", f"{mako_base}..HEAD"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    names = set()
    for record in result.stdout.split("\x1e"):
        if "\x1f" not in record:
            continue
        author_name, body = record.split("\x1f", 1)
        names.add(author_name.strip().casefold())
        names.update(match.group(1).strip().casefold() for match in COAUTHOR.finditer(body))
    return names


def render():
    config = json.loads(CONFIG.read_text(encoding="utf-8"))
    name_to_login = {}
    for login, aliases in config["login_to_names"].items():
        for alias in aliases:
            name = alias.casefold()
            if name in name_to_login and name_to_login[name] != login:
                raise ValueError(f"Commit name {alias!r} maps to multiple GitHub logins")
            name_to_login[name] = login
    # Exclude AI co-author trailers from contributor credits.
    ignored = tuple(prefix.casefold() for prefix in config.get("ignored_name_prefixes", []))
    names = {name for name in git_names(config["mako_base"]) if not (ignored and name.startswith(ignored))}
    missing = names - name_to_login.keys()
    if missing:
        raise ValueError(
            "Map these commit author/co-author names in scripts/contributors.json: "
            + ", ".join(sorted(missing))
        )
    logins = set(name_to_login[name] for name in names)
    logins.update(config.get("additional_logins", []))
    maintainer = config["maintainer_login"]
    upstream = config["upstream_credits"]
    for login in logins | {maintainer} | upstream.keys():
        if not LOGIN.fullmatch(login):
            raise ValueError(f"Invalid GitHub login in scripts/contributors.json: {login!r}")

    lines = ["# Contributors", ""]
    lines.append(f"- framegen-vulkan maintainer. [@{maintainer}](https://github.com/{maintainer})")
    lines.extend(
        f"- [@{login}](https://github.com/{login})"
        for login in sorted(logins - {maintainer}, key=str.casefold)
    )
    lines.extend(["", "## Upstream credits", ""])
    lines.extend(
        f"- {credit} [@{login}](https://github.com/{login})"
        for login, credit in upstream.items()
    )
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if CONTRIBUTORS.md is missing or stale")
    args = parser.parse_args()
    current = CONTRIBUTORS.read_text(encoding="utf-8") if CONTRIBUTORS.exists() else None
    updated = render()
    if args.check:
        if updated != current:
            parser.exit(1, "CONTRIBUTORS.md is missing or stale. Run python3 scripts/update-contributors.py.\n")
        print("CONTRIBUTORS.md is current")
    elif updated != current:
        CONTRIBUTORS.write_text(updated, encoding="utf-8")
        print("Updated CONTRIBUTORS.md")
    else:
        print("CONTRIBUTORS.md is already current")


if __name__ == "__main__":
    main()

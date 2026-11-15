#!/usr/bin/env python3
"""Check relative Markdown links and #anchors in the tracked Markdown files.

Usage: scripts/check-markdown-links.py [--root DIRECTORY]

Every relative link target must name a tracked file or directory. Every #fragment that points into a
Markdown file must match a heading (GitHub slug rules, repeated headings numbered -1, -2, ...) or an
explicit id/name anchor. External URLs and mailto links are skipped. Working handoff notes
(`*-HANDOFF.md`) are not checked as sources, but their targets are still checked when other files
link to them. Links inside fenced or inline code are ignored. Exits 1 and lists each broken link.
"""

from __future__ import annotations

import argparse
import posixpath
import re
import subprocess
import sys
import urllib.parse
from collections.abc import Iterator
from pathlib import Path

FENCE = re.compile(r"^ {0,3}(`{3,}|~{3,})")
CODE_SPAN = re.compile(r"(`+)(.+?)\1")
INLINE_LINK = re.compile(
    r"\]\(\s*(<[^<>\n]*>|[^\s()]*(?:\([^\s()]*\)[^\s()]*)*)(?:\s+(?:\"[^\"\n]*\"|'[^'\n]*'))?\s*\)"
)
REFERENCE_DEFINITION = re.compile(r"^ {0,3}\[[^\]\n]+\]:\s*(<[^<>\n]*>|\S+)")
HTML_ATTRIBUTE = re.compile(r"""\b(?:href|src)\s*=\s*(?:"([^"\n]*)"|'([^'\n]*)')""", re.IGNORECASE)
HTML_ANCHOR = re.compile(r"""<a\s[^>]*?\b(?:id|name)\s*=\s*(?:"([^"\n]*)"|'([^'\n]*)')""", re.IGNORECASE)
ATX_HEADING = re.compile(r"^ {0,3}#{1,6}(?:[ \t]+(.*?))?(?:[ \t]+#+)?[ \t]*$")
SETEXT_UNDERLINE = re.compile(r"^ {0,3}(?:=+|-+)[ \t]*$")
NOT_SETEXT_TEXT = re.compile(r"^\s*(?:[-*+>|]\s|\d+[.)]\s|$)")
EXTERNAL = re.compile(r"^(?:[A-Za-z][A-Za-z0-9+.-]*:|//)")


def git_listing(root: Path, *arguments: str) -> list[str]:
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", *arguments],
        check=True,
        capture_output=True,
    )
    return [name for name in result.stdout.decode("utf-8", "surrogateescape").split("\0") if name]


def prose_lines(text: str) -> Iterator[tuple[int, str]]:
    """Yield (line number, line) for every line outside fenced code blocks."""
    open_fence: str | None = None
    for number, line in enumerate(text.splitlines(), start=1):
        match = FENCE.match(line)
        if open_fence is None:
            if match:
                open_fence = match.group(1)
                continue
            yield number, line
        elif match and match.group(1)[0] == open_fence[0] and line.strip() == match.group(1):
            open_fence = None


def github_slug(heading: str) -> str:
    """Approximate GitHub's heading id: drop markup and punctuation, lower-case, spaces to hyphens."""
    text = re.sub(r"<[^>]*>", "", heading)
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"`+([^`]*)`+", r"\1", text)
    text = text.replace("*", "")
    text = re.sub(r"[^\w\- ]", "", text.strip().lower())
    return text.replace(" ", "-")


def markdown_anchors(text: str) -> set[str]:
    anchors: set[str] = set()
    counts: dict[str, int] = {}

    def add_heading(raw: str) -> None:
        base = github_slug(raw)
        seen = counts.get(base, 0)
        counts[base] = seen + 1
        anchors.add(base if seen == 0 else f"{base}-{seen}")

    previous: str | None = None
    for _, line in prose_lines(text):
        for match in HTML_ANCHOR.finditer(line):
            anchors.add(match.group(1) if match.group(1) is not None else match.group(2))
        heading = ATX_HEADING.match(line)
        if heading:
            add_heading(heading.group(1) or "")
            previous = None
        elif previous is not None and SETEXT_UNDERLINE.match(line):
            add_heading(previous)
            previous = None
        else:
            previous = line if line.strip() and not NOT_SETEXT_TEXT.match(line) else None
    return anchors


def link_targets(line: str) -> Iterator[str]:
    scrubbed = CODE_SPAN.sub(lambda match: " " * len(match.group(0)), line)
    for match in INLINE_LINK.finditer(scrubbed):
        yield match.group(1)
    definition = REFERENCE_DEFINITION.match(scrubbed)
    if definition:
        yield definition.group(1)
    for match in HTML_ATTRIBUTE.finditer(scrubbed):
        yield match.group(1) if match.group(1) is not None else match.group(2)


class LinkChecker:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.files = set(git_listing(root))
        self.directories = {"."}
        for name in self.files:
            parent = posixpath.dirname(name)
            while parent and parent not in self.directories:
                self.directories.add(parent)
                parent = posixpath.dirname(parent)
        self.anchor_cache: dict[str, set[str]] = {}
        self.checked = 0
        self.broken: list[str] = []

    def anchors_of(self, name: str) -> set[str] | None:
        if name not in self.anchor_cache:
            path = self.root / name
            if not path.is_file():
                return None
            text = path.read_text(encoding="utf-8", errors="replace")
            self.anchor_cache[name] = markdown_anchors(text)
        return self.anchor_cache[name]

    def report(self, source: str, number: int, target: str, reason: str) -> None:
        self.broken.append(f"{source}:{number}: broken link '{target}': {reason}")

    def check_target(self, source: str, number: int, raw_target: str) -> None:
        target = raw_target[1:-1] if raw_target.startswith("<") and raw_target.endswith(">") else raw_target
        if not target or EXTERNAL.match(target):
            return
        path_part, _, fragment = target.partition("#")
        path_part = urllib.parse.unquote(path_part.partition("?")[0])
        fragment = urllib.parse.unquote(fragment)
        self.checked += 1

        if path_part == "":
            destination = source
        else:
            if path_part.startswith("/"):
                joined = path_part.lstrip("/")
            else:
                joined = posixpath.join(posixpath.dirname(source), path_part)
            destination = posixpath.normpath(joined)
            if destination == ".." or destination.startswith("../"):
                self.report(source, number, raw_target, "the link leaves the repository")
                return
            if destination in self.directories:
                return
            if destination not in self.files:
                self.report(source, number, raw_target, "no tracked file or directory with that name")
                return

        if not fragment or not destination.lower().endswith(".md"):
            return
        anchors = self.anchors_of(destination)
        if anchors is None:
            self.report(source, number, raw_target, "tracked file is missing from the working tree")
        elif fragment not in anchors:
            self.report(source, number, raw_target, f"no heading or anchor '#{fragment}' in {destination}")

    def run(self) -> int:
        markdown = sorted(name for name in self.files if name.lower().endswith(".md"))
        for source in markdown:
            if source.endswith("-HANDOFF.md"):
                continue
            path = self.root / source
            if not path.is_file():
                self.broken.append(f"{source}: tracked file is missing from the working tree")
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for number, line in prose_lines(text):
                for target in link_targets(line):
                    self.check_target(source, number, target)

        if self.broken:
            for entry in self.broken:
                print(entry, file=sys.stderr)
            print(
                f"check-markdown-links: {len(self.broken)} broken link(s) among {self.checked} "
                f"relative link(s) in {len(markdown)} Markdown file(s).",
                file=sys.stderr,
            )
            return 1
        print(
            f"check-markdown-links: {self.checked} relative link(s) and anchor(s) in "
            f"{len(markdown)} Markdown file(s) resolve."
        )
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="repository to check (default: the repository containing this script)",
    )
    arguments = parser.parse_args()
    try:
        return LinkChecker(arguments.root.resolve()).run()
    except subprocess.CalledProcessError as error:
        print(f"check-markdown-links: git ls-files failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

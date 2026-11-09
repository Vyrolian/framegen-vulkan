#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarise fgvk-pt-probe logs.

Each log is one run. For a run this prints n and the p5, median, p95 and p99 of presented minus target,
the share of presents more than 1 ms from their target, and the same quantiles for the absolute error of
each presented step against 1/rate. Every metric is repeated for each wp_presentation flags value. Two
logs, normally a targeted run and a --no-target control, print side by side.

Usage: python3 -I engine/fgvk-measure/probe/analyze_pt_probe.py [--rate HZ] [--label NAME] LOG [LOG]

The rate comes from the session line of each log unless --rate overrides it. Standard library only.
"""

import argparse
import math
import re
import sys
from pathlib import Path

PROBE_PREFIX = "framegen-vulkan: pt-probe "
PRESENT_RE = re.compile(
    r"^framegen-vulkan: pt-probe present_id=(\d+) target_ns=(\d+) submit_ns=(\d+) call_ns=(\d+) "
    r"stage_ns=(\d+) presented_ns=(\d+) flags=(\d+)\s*$"
)
SESSION_RE = re.compile(r"^framegen-vulkan: pt-probe session (.*?)\s*$")
SHARE_LIMIT_MS = 1.0
DEFAULT_RATE_HZ = 150.0
CLOCK_MONOTONIC = 1
LABEL_WIDTH = 46
COLUMN_WIDTH = 26


def parse_log(lines):
    """Returns (session fields, present records sorted by present_id, count of unreadable probe lines)."""
    session = {}
    presents = []
    unreadable = 0
    for line in lines:
        if not line.startswith(PROBE_PREFIX):
            continue
        match = PRESENT_RE.match(line)
        if match:
            presents.append({
                "present_id": int(match.group(1)),
                "target_ns": int(match.group(2)),
                "submit_ns": int(match.group(3)),
                "call_ns": int(match.group(4)),
                "stage_ns": int(match.group(5)),
                "presented_ns": int(match.group(6)),
                "flags": int(match.group(7)),
            })
            continue
        match = SESSION_RE.match(line)
        if match:
            for field in match.group(1).split():
                key, separator, value = field.partition("=")
                if separator:
                    session[key] = value
            continue
        unreadable += 1
    presents.sort(key=lambda record: record["present_id"])
    return session, presents, unreadable


def quantile(ordered, fraction):
    """Linear interpolation between the closest ranks of an already sorted, non-empty list."""
    position = fraction * (len(ordered) - 1)
    low = math.floor(position)
    high = math.ceil(position)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def summarise(values):
    """n, p5, median, p95, p99 and the share of absolute values above SHARE_LIMIT_MS."""
    ordered = sorted(values)
    if not ordered:
        return {"n": 0, "p5": None, "median": None, "p95": None, "p99": None, "share_over_1ms": None}
    over = sum(1 for value in values if abs(value) > SHARE_LIMIT_MS)
    return {
        "n": len(ordered),
        "p5": quantile(ordered, 0.05),
        "median": quantile(ordered, 0.5),
        "p95": quantile(ordered, 0.95),
        "p99": quantile(ordered, 0.99),
        "share_over_1ms": over / len(ordered),
    }


def analyse_lines(lines, rate_override=None, label=None):
    """Analyses one run. Returns a report dict that render() turns into text."""
    session, presents, unreadable = parse_log(lines)
    warnings = []
    if rate_override is not None:
        rate_hz = rate_override
    elif "rate_hz" in session:
        rate_hz = float(session["rate_hz"])
    else:
        rate_hz = DEFAULT_RATE_HZ
        warnings.append("no session line, so the rate is 150 Hz unless --rate is given")
    period_ms = 1000.0 / rate_hz
    clock = session.get("wp_clock_id", "unknown")
    if clock != str(CLOCK_MONOTONIC):
        warnings.append(f"wp_presentation clock_id is {clock}, not CLOCK_MONOTONIC: presented times do not "
                        "compare with the targets")

    deviations = []  # (flags, presented - target in ms)
    steps = []       # (flags of the later present, |presented step - 1/rate| in ms)
    previous = None
    for record in presents:
        if record["presented_ns"] and record["target_ns"]:
            deviation_ms = (record["presented_ns"] - record["target_ns"]) / 1e6
            deviations.append((record["flags"], deviation_ms))
        if (previous is not None and previous["present_id"] + 1 == record["present_id"]
                and previous["presented_ns"] and record["presented_ns"]):
            step_ms = (record["presented_ns"] - previous["presented_ns"]) / 1e6
            steps.append((record["flags"], abs(step_ms - period_ms)))
        previous = record

    flag_values = sorted({flags for flags, _ in deviations} | {flags for flags, _ in steps})
    by_flags = {}
    for flags in flag_values:
        by_flags[flags] = {
            "deviation": summarise([value for f, value in deviations if f == flags]),
            "step_error": summarise([value for f, value in steps if f == flags]),
        }

    default_label = "no-target" if session.get("no_target") == "1" else "targeted"
    return {
        "label": label or default_label,
        "session": session,
        "rate_hz": rate_hz,
        "period_ms": period_ms,
        "counts": {
            "presents": len(presents),
            "presented": sum(1 for record in presents if record["presented_ns"]),
            "staged": sum(1 for record in presents if record["stage_ns"]),
            "unreadable": unreadable,
        },
        "deviation": summarise([value for _, value in deviations]),
        "step_error": summarise([value for _, value in steps]),
        "by_flags": by_flags,
        "warnings": warnings,
    }


def fmt_ms(value):
    return "-" if value is None else f"{value:.3f}"


def fmt_share(value):
    return "-" if value is None else f"{100.0 * value:.1f}%"


def table_line(text, cells):
    return f"{text:<{LABEL_WIDTH}}" + "".join(f"{cell:>{COLUMN_WIDTH}}" for cell in cells)


def render(reports):
    """Lays the reports out as one column per run."""
    lines = [table_line("run", [report["label"] for report in reports])]

    def add(text, cells):
        lines.append(table_line(text, cells))

    for title, key in (("mode", "mode"), ("lead_ms", "lead_ms"), ("uneven", "uneven"),
                       ("no_target", "no_target"), ("stage", "stage"), ("refresh_ns", "refresh_ns"),
                       ("timing_queue_full", "timing_queue_full"), ("suboptimal", "suboptimal"),
                       ("feedback_request_failures", "feedback_request_failures")):
        add(title, [report["session"].get(key, "-") for report in reports])
    add("rate_hz used", [f"{report['rate_hz']:g}" for report in reports])
    add("period 1/rate (ms)", [f"{report['period_ms']:.3f}" for report in reports])
    add("presents", [str(report["counts"]["presents"]) for report in reports])
    add("presented (wp_presentation)", [str(report["counts"]["presented"]) for report in reports])
    add("staged (past presentation timing)", [str(report["counts"]["staged"]) for report in reports])
    add("unreadable probe lines", [str(report["counts"]["unreadable"]) for report in reports])

    def stat_rows(title, summaries, share=False):
        lines.append(title)
        add("  n", [str(summary["n"]) for summary in summaries])
        for name, key in (("  p5", "p5"), ("  median", "median"), ("  p95", "p95"), ("  p99", "p99")):
            add(name, [fmt_ms(summary[key]) for summary in summaries])
        if share:
            add("  share with |presented - target| > 1 ms",
                [fmt_share(summary["share_over_1ms"]) for summary in summaries])

    empty = summarise([])
    stat_rows("presented - target (ms)", [report["deviation"] for report in reports], share=True)
    stat_rows("|presented step - 1/rate| (ms)", [report["step_error"] for report in reports])
    all_flags = sorted({flags for report in reports for flags in report["by_flags"]})
    for flags in all_flags:
        stat_rows(f"flags={flags}: presented - target (ms)",
                  [report["by_flags"].get(flags, {}).get("deviation", empty) for report in reports], share=True)
        stat_rows(f"flags={flags}: |presented step - 1/rate| (ms)",
                  [report["by_flags"].get(flags, {}).get("step_error", empty) for report in reports])
    for report in reports:
        for warning in report["warnings"]:
            lines.append(f"warning ({report['label']}): {warning}")
    return "\n".join(lines)


def unique_labels(reports):
    seen = {}
    for report in reports:
        base = report["label"]
        seen[base] = seen.get(base, 0) + 1
        if seen[base] > 1:
            report["label"] = f"{base} #{seen[base]}"


def main(argv=None):
    parser = argparse.ArgumentParser(description="Summarise fgvk-pt-probe logs, one run per log.")
    parser.add_argument("logs", nargs="+", type=Path, help="one or two probe logs, targeted first")
    parser.add_argument("--rate", type=float, help="rate in Hz (default: the session line, else 150)")
    parser.add_argument("--label", action="append", default=[], help="column label, once per log")
    args = parser.parse_args(argv)
    if len(args.logs) > 2:
        parser.error("give one or two logs")
    if args.label and len(args.label) != len(args.logs):
        parser.error("give one --label per log")
    reports = []
    for index, path in enumerate(args.logs):
        text = path.read_text(encoding="utf-8", errors="replace")
        label = args.label[index] if args.label else None
        reports.append(analyse_lines(text.splitlines(), args.rate, label))
    unique_labels(reports)
    print(render(reports))
    return 0


if __name__ == "__main__":
    sys.exit(main())

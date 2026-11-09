#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Join a framegen-vulkan present measurer log by sequence and report the game's own present path.

Input is the log written by the standalone layer VK_LAYER_FGVK_measure (engine/fgvk-measure) to
/tmp/fgvk-measure-<uid>-<pid>.log. Times are reported in milliseconds. Standard library only.
"""

import argparse
import math
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

PREFIX = "framegen-vulkan: measure: "
NS_PER_MS = 1_000_000
NS_PER_S = 1_000_000_000
SHARE_THRESHOLD_MS = 3.0
GROUP_ALL = "all"
GROUP_UNPRESENTED = "no-presented"
METRICS = (
    ("call_to_gpu_end", "call -> gpu end"),
    ("gpu_end_to_presented", "gpu end -> presented"),
    ("call_to_presented", "call -> presented"),
    ("presented_step", "presented step"),
)


@dataclass
class Present:
    seq: int
    swapchain: int
    image: int
    waits: int
    call_start_ns: int
    call_end_ns: int
    result: int
    measured: bool


@dataclass
class Gpu:
    seq: int
    available: bool
    start_ns: int
    end_ns: int


@dataclass
class Presented:
    seq: int
    presented_ns: int
    refresh_ns: int
    flags: int
    msc: int


@dataclass
class Log:
    presents: list = field(default_factory=list)
    present_by_seq: dict = field(default_factory=dict)
    gpu: dict = field(default_factory=dict)
    presented: dict = field(default_factory=dict)
    discarded: set = field(default_factory=set)
    sessions: list = field(default_factory=list)
    clock_ids: list = field(default_factory=list)
    summaries: list = field(default_factory=list)
    duplicates: Counter = field(default_factory=Counter)
    malformed: int = 0
    ignored: int = 0


def parse_fields(text):
    fields = {}
    for token in text.split():
        name, separator, value = token.partition("=")
        if separator:
            fields[name] = value
    return fields


def parse_line(line):
    """Returns (kind, fields) for a measure line, or None for any other line."""
    index = line.find(PREFIX)
    if index < 0:
        return None
    body = line[index + len(PREFIX):].rstrip("\r\n")
    kind, _, rest = body.partition(" ")
    if kind == "session":
        # The device name is free text and may contain spaces and '='.
        head, _, tail = rest.partition(" device=")
        device, _, calibrated = tail.rpartition(" calibrated=")
        return kind, {"pid": head.partition("=")[2], "device": device, "calibrated": calibrated}
    return kind, parse_fields(rest)


def load(lines):
    log = Log()
    for line in lines:
        parsed = parse_line(line)
        if parsed is None:
            log.ignored += 1
            continue
        kind, fields = parsed
        try:
            if kind == "present":
                present = Present(
                    seq=int(fields["seq"]),
                    swapchain=int(fields["swapchain"]),
                    image=int(fields["image"]),
                    waits=int(fields["waits"]),
                    call_start_ns=int(fields["call_start_ns"]),
                    call_end_ns=int(fields["call_end_ns"]),
                    result=int(fields["result"]),
                    measured=fields["measured"] == "1",
                )
                if present.seq in log.present_by_seq:
                    log.duplicates["present"] += 1
                    continue
                log.present_by_seq[present.seq] = present
                log.presents.append(present)
            elif kind == "gpu":
                gpu = Gpu(
                    seq=int(fields["seq"]),
                    available=fields["available"] == "1",
                    start_ns=int(fields["gpu_start_ns"]),
                    end_ns=int(fields["gpu_end_ns"]),
                )
                if gpu.seq in log.gpu:
                    log.duplicates["gpu"] += 1
                else:
                    log.gpu[gpu.seq] = gpu
            elif kind == "presented":
                presented = Presented(
                    seq=int(fields["seq"]),
                    presented_ns=int(fields["presented_ns"]),
                    refresh_ns=int(fields["refresh_ns"]),
                    flags=int(fields["flags"]),
                    msc=int(fields["msc"]),
                )
                if presented.seq in log.presented:
                    log.duplicates["presented"] += 1
                else:
                    log.presented[presented.seq] = presented
            elif kind == "discarded":
                log.discarded.add(int(fields["seq"]))
            elif kind == "clock":
                log.clock_ids.append(int(fields["clock_id"]))
            elif kind == "session":
                log.sessions.append(fields)
            elif kind == "summary":
                log.summaries.append({key: int(value) for key, value in fields.items()})
            else:
                log.ignored += 1
        except (KeyError, ValueError):
            log.malformed += 1
    return log


def percentile(sorted_values, percent):
    """Linear interpolation between closest ranks (the common 'linear' method)."""
    if len(sorted_values) == 1:
        return sorted_values[0]
    rank = (len(sorted_values) - 1) * percent / 100.0
    low = math.floor(rank)
    high = math.ceil(rank)
    return sorted_values[low] + (sorted_values[high] - sorted_values[low]) * (rank - low)


def summarize(values_ns, share_over_threshold=False):
    values_ms = sorted(value / NS_PER_MS for value in values_ns)
    summary = {"n": len(values_ms)}
    if not values_ms:
        return summary
    summary["p5"] = percentile(values_ms, 5)
    summary["median"] = percentile(values_ms, 50)
    summary["p95"] = percentile(values_ms, 95)
    summary["p99"] = percentile(values_ms, 99)
    if share_over_threshold:
        over = sum(1 for value in values_ms if value > SHARE_THRESHOLD_MS)
        summary["over_3ms_share"] = 100.0 * over / len(values_ms)
    return summary


def group_key(flags):
    return str(flags)


def analyze(log, start_s=0.0, end_s=None):
    if not log.presents:
        raise ValueError("the log has no present records")
    # The window is measured from the first present in the log, as the analyser documents.
    origin = min(present.call_start_ns for present in log.presents)
    start_ns = round(start_s * NS_PER_S)
    end_ns = None if end_s is None else round(end_s * NS_PER_S)

    def in_window(present):
        offset = present.call_start_ns - origin
        return offset >= start_ns and (end_ns is None or offset < end_ns)

    window = [present for present in log.presents if in_window(present)]
    window_seqs = {present.seq for present in window}
    valid_presented = {seq: rec for seq, rec in log.presented.items() if rec.presented_ns > 0}

    counts = {
        "presents": len(window),
        "measured": sum(1 for present in window if present.measured),
        "gpu_lines": sum(1 for seq in window_seqs if seq in log.gpu),
        "unavailable": sum(1 for seq in window_seqs if seq in log.gpu and not log.gpu[seq].available),
        "presented": sum(1 for seq in window_seqs if seq in valid_presented),
        "discarded": sum(1 for seq in window_seqs if seq in log.discarded),
        "invalid_presented": sum(
            1 for seq in window_seqs if seq in log.presented and log.presented[seq].presented_ns <= 0),
    }
    counts["unmeasured"] = counts["presents"] - counts["measured"]

    unjoined = {
        "present_without_outcome": sum(
            1 for present in window
            if present.seq not in log.presented and present.seq not in log.discarded),
        "measured_without_gpu": sum(
            1 for present in window if present.measured and present.seq not in log.gpu),
        "gpu_without_present": sum(1 for seq in log.gpu if seq not in log.present_by_seq),
        "presented_without_present": sum(1 for seq in log.presented if seq not in log.present_by_seq),
        "discarded_without_present": sum(1 for seq in log.discarded if seq not in log.present_by_seq),
    }
    unjoined["total"] = sum(unjoined.values())

    values = {metric: {GROUP_ALL: []} for metric, _ in METRICS}
    group_sizes = Counter()

    def add(metric, group, value):
        values[metric].setdefault(group, []).append(value)
        values[metric][GROUP_ALL].append(value)

    for present in window:
        presented = valid_presented.get(present.seq)
        group = group_key(presented.flags) if presented else GROUP_UNPRESENTED
        group_sizes[group] += 1
        gpu = log.gpu.get(present.seq)
        gpu_ok = gpu is not None and gpu.available
        if gpu_ok:
            add("call_to_gpu_end", group, gpu.end_ns - present.call_start_ns)
        if presented:
            add("call_to_presented", group, presented.presented_ns - present.call_start_ns)
        if presented and gpu_ok:
            add("gpu_end_to_presented", group, presented.presented_ns - gpu.end_ns)
    group_sizes[GROUP_ALL] = len(window)

    # Presented step: consecutive presented_ns of the window's presents, grouped by the later flags.
    in_window_presented = sorted(
        (rec for seq, rec in valid_presented.items() if seq in window_seqs),
        key=lambda rec: rec.presented_ns)
    for earlier, later in zip(in_window_presented, in_window_presented[1:]):
        add("presented_step", group_key(later.flags), later.presented_ns - earlier.presented_ns)

    groups = sorted(group_sizes, key=lambda name: (
        name != GROUP_ALL, name == GROUP_UNPRESENTED, int(name) if name.isdigit() else 0))
    report_groups = []
    for group in groups:
        entry = {"group": group, "presents": group_sizes[group], "metrics": {}}
        for metric, _ in METRICS:
            entry["metrics"][metric] = summarize(
                values[metric].get(group, []), share_over_threshold=metric == "gpu_end_to_presented")
        report_groups.append(entry)

    return {
        "origin_ns": origin,
        "start_s": start_s,
        "end_s": end_s,
        "counts": counts,
        "unjoined": unjoined,
        "groups": report_groups,
    }


def render(report):
    lines = []
    window_end = "end" if report["end_s"] is None else f"{report['end_s']:g}s"
    lines.append(f"framegen-vulkan: measure analysis: window {report['start_s']:g}s to {window_end} "
                 "from the first present; times in ms")
    lines.append("counts: " + " ".join(f"{key}={value}" for key, value in report["counts"].items()))
    lines.append("unjoined: " + " ".join(f"{key}={value}" for key, value in report["unjoined"].items()))
    for entry in report["groups"]:
        lines.append(f"group {entry['group']} presents={entry['presents']}")
        for metric, label in METRICS:
            summary = entry["metrics"][metric]
            row = f"  {label:<22} n={summary['n']}"
            if summary["n"]:
                row += (f" p5={summary['p5']:.3f} median={summary['median']:.3f}"
                        f" p95={summary['p95']:.3f} p99={summary['p99']:.3f}")
            if "over_3ms_share" in summary:
                row += f" over_{SHARE_THRESHOLD_MS:g}ms={summary['over_3ms_share']:.1f}%"
            lines.append(row)
    return "\n".join(lines) + "\n"


def main(argv=None, out=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("log", type=Path,
                        help="measure log, /tmp/fgvk-measure-<uid>-<pid>.log")
    parser.add_argument("--from", dest="start", type=float, default=0.0, metavar="S",
                        help="window start in seconds from the first present (default 0)")
    parser.add_argument("--to", dest="end", type=float, default=None, metavar="S",
                        help="window end in seconds from the first present, exclusive (default: end of log)")
    args = parser.parse_args(argv)
    if args.start < 0 or (args.end is not None and args.end <= args.start):
        parser.error("--from must be >= 0 and --to must be greater than --from")
    out = out if out is not None else sys.stdout
    with args.log.open(encoding="utf-8", errors="replace") as handle:
        log = load(handle)
    if not log.presents:
        print("no present records in the log", file=sys.stderr)
        return 2
    out.write(render(analyze(log, args.start, args.end)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

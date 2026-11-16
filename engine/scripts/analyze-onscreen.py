#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reads the GP1 on-screen profile of a pacing log.

Joins each pacing-profile-output record to its pacing-profile-onscreen record (per context, by the
output's present_id and the record's lower_present_id). Times are steady-clock nanoseconds in the log;
reported times are seconds since the first output of the log.

Usage: python3 -I analyze-onscreen.py <log> [--from SECONDS] [--to SECONDS]

Reports coverage, present to on-screen completion, the on-screen interval between consecutive outputs
(per transition), generated outputs against their ideal place between two reals, same-wake pairs,
timeouts and errors, then one line per 2 s window. Completion means the driver's WSI saw the lower
present; offsets are close to constant, so read the spacing, not the absolute values.
"""

import argparse
import math
import re
import sys
from collections import defaultdict

TOKEN = re.compile(r"(\w+)=(\S+)")
OUTPUT_OPERATION = "operation=pacing-profile-output version="
ONSCREEN_OPERATION = "operation=pacing-profile-onscreen version="
VK_SUCCESS = 0
VK_TIMEOUT = 2
VK_SUBOPTIMAL_KHR = 1000001003
SAME_WAKE_MS = 0.25
WINDOW_SECONDS = 2.0


def parse_records(path):
    """Returns (outputs, onscreen): output dicts in log order, and onscreen dicts by (context, lower id)."""
    outputs = []
    onscreen = {}
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if OUTPUT_OPERATION in line:
                fields = dict(TOKEN.findall(line))
                record = integers(fields, ("context", "present_id", "generated", "started_ns",
                                           "finished_ns", "scheduled_ns", "result"))
                if record is not None:
                    outputs.append(record)
            elif ONSCREEN_OPERATION in line:
                fields = dict(TOKEN.findall(line))
                record = integers(fields, ("context", "lower_present_id", "result", "done_ns"))
                if record is not None:
                    onscreen[(record["context"], record["lower_present_id"])] = record
    return outputs, onscreen


def integers(fields, names):
    record = {}
    for name in names:
        try:
            record[name] = int(fields[name])
        except (KeyError, ValueError):
            return None
    return record


def percentile(values, fraction):
    """Linear interpolation between the closest ranks; None for an empty list."""
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    low = math.floor(position)
    high = math.ceil(position)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def spread(values):
    if not values:
        return "n=0"
    return "n=%d p50=%.3f p95=%.3f p99=%.3f" % (
        len(values), percentile(values, 0.50), percentile(values, 0.95), percentile(values, 0.99))


def completed(result):
    return result in (VK_SUCCESS, VK_SUBOPTIMAL_KHR)


def join(outputs, onscreen):
    """The outputs that completed on screen, each with its completion time (done_ns). A timed-out
    or failed wait has no completion, so its output is not counted as covered."""
    covered = []
    for output in outputs:
        match = onscreen.get((output["context"], output["present_id"])) if output["present_id"] else None
        if match is None or not completed(match["result"]):
            continue
        output["done_ns"] = match["done_ns"]
        covered.append(output)
    return covered


def ms(nanoseconds):
    return nanoseconds / 1e6


def analyze(path, start=None, stop=None):
    outputs, onscreen = parse_records(path)
    if not outputs:
        print("no pacing-profile-output records in %s" % path)
        return 1
    origin = min(output["started_ns"] for output in outputs)

    def seconds(nanoseconds):
        return (nanoseconds - origin) / 1e9

    covered = join(outputs, onscreen)
    print("outputs: %d, with an on-screen completion: %d (%.1f%%)" % (
        len(outputs), len(covered), 100.0 * len(covered) / len(outputs)))
    selected = [output for output in covered
                if (start is None or seconds(output["done_ns"]) >= start)
                and (stop is None or seconds(output["done_ns"]) <= stop)]
    if not selected:
        print("no covered outputs in the selected range")
        return 1

    present_to_done = [ms(output["done_ns"] - output["finished_ns"]) for output in selected]
    print("present -> done ms: " + spread(present_to_done))

    by_context = defaultdict(list)
    for output in selected:
        by_context[output["context"]].append(output)
    intervals = []
    transitions = defaultdict(list)
    same_wake = []
    gaps = []
    for context_outputs in by_context.values():
        context_outputs.sort(key=lambda output: output["done_ns"])
        for earlier, later in zip(context_outputs, context_outputs[1:]):
            gap = ms(later["done_ns"] - earlier["done_ns"])
            intervals.append((later["done_ns"], gap))
            kind = ("generated" if earlier["generated"] else "real") + "->" + (
                "generated" if later["generated"] else "real")
            transitions[kind].append(gap)
            if gap < SAME_WAKE_MS:
                same_wake.append(later["done_ns"])
        reals = [index for index, output in enumerate(context_outputs) if not output["generated"]]
        for first, second in zip(reals, reals[1:]):
            between = context_outputs[first + 1:second]
            if not between:
                continue
            count = len(between) + 1
            start_done = context_outputs[first]["done_ns"]
            end_done = context_outputs[second]["done_ns"]
            for position, output in enumerate(between, start=1):
                ideal = start_done + (end_done - start_done) * position / count
                error = ms(output["done_ns"] - ideal)
                gaps.append((output["done_ns"], abs(error)))

    print("on-screen interval ms: " + spread([gap for _, gap in intervals]))
    for kind in ("real->generated", "generated->real", "generated->generated", "real->real"):
        if transitions[kind]:
            print("  %s: %s" % (kind, spread(transitions[kind])))
    print("generated vs ideal |error| ms: " + spread([error for _, error in gaps]))
    print("same-wake pairs (under %.2f ms, the earlier one was likely replaced): %d" % (
        SAME_WAKE_MS, len(same_wake)))
    timeouts = sum(1 for record in onscreen.values() if record["result"] == VK_TIMEOUT)
    errors = sum(1 for record in onscreen.values() if record["result"] < 0)
    print("timeouts: %d, errors: %d" % (timeouts, errors))

    print("windows of %.0f s (time from the first output):" % WINDOW_SECONDS)
    windows = defaultdict(lambda: {"reals": 0, "outputs": 0, "intervals": [], "errors": [], "same": 0})
    for output in selected:
        window = int(seconds(output["done_ns"]) // WINDOW_SECONDS)
        windows[window]["outputs"] += 1
        windows[window]["reals"] += 0 if output["generated"] else 1
    for done, gap in intervals:
        windows[int(seconds(done) // WINDOW_SECONDS)]["intervals"].append(gap)
    for done in same_wake:
        windows[int(seconds(done) // WINDOW_SECONDS)]["same"] += 1
    for done, error in gaps:
        windows[int(seconds(done) // WINDOW_SECONDS)]["errors"].append(error)
    for window in sorted(windows):
        entry = windows[window]
        p99_interval = percentile(entry["intervals"], 0.99)
        p99_error = percentile(entry["errors"], 0.99)
        print("  t=%5.0f-%5.0f reals/s=%.1f outputs/s=%.1f interval_p99=%s error_p99=%s same_wake=%d" % (
            window * WINDOW_SECONDS, (window + 1) * WINDOW_SECONDS,
            entry["reals"] / WINDOW_SECONDS, entry["outputs"] / WINDOW_SECONDS,
            "%.3f" % p99_interval if p99_interval is not None else "-",
            "%.3f" % p99_error if p99_error is not None else "-",
            entry["same"]))
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("log")
    parser.add_argument("--from", dest="start", type=float, default=None,
                        help="first second to analyse, counted from the first output")
    parser.add_argument("--to", dest="stop", type=float, default=None,
                        help="last second to analyse, counted from the first output")
    arguments = parser.parse_args(argv)
    return analyze(arguments.log, arguments.start, arguments.stop)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

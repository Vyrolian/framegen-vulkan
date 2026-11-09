#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Explain output gaps in a framegen-vulkan pacing capture.

Usage: analyze-hitches.py LOG [--marks MARKS] [--from SECONDS] [--to SECONDS] [--min-gap MS]

LOG is a FGVK_PACING_PROFILE_LOG capture. MARKS is the file `fgvk-cli mark` appends to. Standard
library only. Times in the log are CLOCK_MONOTONIC nanoseconds; the report shows seconds from the
first output. A generated output of the pair (s-1, s) carries source s.
"""

import argparse
import math
import re
import statistics
import sys
from collections import Counter, defaultdict

RECORD = re.compile(r"^framegen-vulkan: present diagnostics: operation=(\S+)(?: (.*))?$")
MARK = re.compile(r"^framegen-vulkan: mark monotonic_ns=(-?\d+) note=(.*)$")
NS_PER_MS = 1_000_000.0
NS_PER_S = 1_000_000_000.0
FG_LATE_MS = 3.0
COPY_LATE_MS = 1.0


def fields_of(text):
    result = {}
    for token in (text or "").split():
        key, separator, value = token.partition("=")
        if separator and key not in result:
            result[key] = value
    return result


def number(fields, key, default=0.0):
    try:
        return float(fields[key])
    except (KeyError, ValueError):
        return default


def integer(fields, key, default=0):
    # Nanosecond fields stay exact integers; float() would round past 2^53.
    try:
        return int(fields[key])
    except (KeyError, ValueError):
        return default


def percentile(ordered, q):
    # Linear interpolation between the two nearest ranks of an ascending list.
    position = (len(ordered) - 1) * q
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def correlation(xs, ys):
    # Pearson r. Sums are exact integers, so a constant series has exactly zero variance.
    n = len(xs)
    if n < 2:
        return None
    sum_x, sum_y = sum(xs), sum(ys)
    var_x = n * sum(x * x for x in xs) - sum_x * sum_x
    var_y = n * sum(y * y for y in ys) - sum_y * sum_y
    if var_x <= 0 or var_y <= 0:
        return None
    cov = n * sum(x * y for x, y in zip(xs, ys)) - sum_x * sum_y
    return cov / math.sqrt(var_x * var_y)


def load(path, records, marks):
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.rstrip("\n")
            mark = MARK.match(line)
            if mark:
                marks.append((float(mark.group(1)), mark.group(2)))
                continue
            record = RECORD.match(line)
            if record:
                records[record.group(1)].append(fields_of(record.group(2)))


def overlapping(rows, start, end):
    return [row for row in rows if number(row, "enter_ns") < end and number(row, "exit_ns") > start]


def describe_output(output):
    label = "gen" if output.get("generated") == "1" else "real"
    return f"{label} src{int(number(output, 'source'))}"


def after_call_samples(outputs, output_gpu, presented, presented_flags, generated_outputs=True):
    # Joins each available copy record to its output (generated or real) by (context, source),
    # then to the output's on-screen time by feedback. Each sample is
    # (flags, call_ns, copy_start_ns, copy_end_ns, presented_ns).
    generated = defaultdict(list)
    wanted = "1" if generated_outputs else "0"
    for output in outputs:
        if output.get("generated", "0") == wanted:
            generated[(integer(output, "context"), integer(output, "source"))].append(output)
    samples = []
    unavailable = unjoined = ambiguous = unshown = 0
    for record in output_gpu:
        if not integer(record, "available"):
            unavailable += 1
            continue
        matches = generated.get((integer(record, "context"), integer(record, "source")), [])
        if not matches:
            unjoined += 1
            continue
        if len(matches) > 1:
            ambiguous += 1
            continue
        feedback = integer(matches[0], "feedback")
        if feedback not in presented:
            unshown += 1
            continue
        samples.append((presented_flags.get(feedback, 0), integer(matches[0], "started_ns"),
                        integer(record, "copy_start_ns"), integer(record, "copy_end_ns"),
                        int(presented[feedback])))
    return samples, unavailable, unjoined, ambiguous, unshown


def print_present_marker(outputs, output_gpu, markers, presented, presented_flags, in_window):
    # T4: the private queue marker submitted right after each lower present. Joined to its output
    # by (context, feedback) and to the on-screen time by feedback. Its start is when the private
    # queue got past the present's queue-side work.
    by_feedback = {}
    for output in outputs:
        by_feedback[(integer(output, "context"), integer(output, "feedback"))] = output
    copy_end = {}
    for record in output_gpu:
        if integer(record, "available"):
            copy_end[(integer(record, "context"), integer(record, "source"))] = integer(record, "copy_end_ns")
    unavailable = unjoined = unshown = 0
    groups = defaultdict(list)
    for marker in markers:
        if not integer(marker, "available"):
            unavailable += 1
            continue
        output = by_feedback.get((integer(marker, "context"), integer(marker, "feedback")))
        if output is None:
            unjoined += 1
            continue
        feedback = integer(marker, "feedback")
        if feedback not in presented:
            unshown += 1
            continue
        call = integer(output, "started_ns")
        if not in_window(call, call):
            continue
        kind = "generated" if output.get("generated") == "1" else "real"
        end = copy_end.get((integer(output, "context"), integer(output, "source"))) if kind == "generated" else None
        groups[(kind, presented_flags.get(feedback, 0))].append(
            (call, integer(marker, "marker_start_ns"), int(presented[feedback]), end))
    total = sum(len(rows) for rows in groups.values())
    print(f"present marker split (private queue after each lower present): {total} joined;"
          f" {unavailable} unavailable, {unjoined} unjoined, {unshown} not presented (whole capture)")
    for (kind, flags) in sorted(groups):
        rows = groups[(kind, flags)]
        metrics = [
            ("call -> marker start", [(r[1] - r[0]) / NS_PER_MS for r in rows]),
            ("marker start -> presented", [(r[2] - r[1]) / NS_PER_MS for r in rows]),
            ("call -> presented", [(r[2] - r[0]) / NS_PER_MS for r in rows]),
        ]
        ends = [r for r in rows if r[3] is not None]
        if ends:
            metrics.append(("copy end -> marker start", [(r[1] - r[3]) / NS_PER_MS for r in ends]))
        print(f"  {kind} flags {flags}: n {len(rows)}")
        for name, values in metrics:
            ordered = sorted(values)
            print(f"    {name}: p5 {percentile(ordered, .05):.2f} median {percentile(ordered, .5):.2f}"
                  f" p95 {percentile(ordered, .95):.2f} p99 {percentile(ordered, .99):.2f} ms")


def print_after_call(samples, unavailable, unjoined, ambiguous, unshown, label="generated outputs"):
    print(f"after-call split ({label}, available copies): {len(samples)} joined;"
          f" {unavailable} unavailable and {unjoined} unjoined (counted over the whole capture);"
          f" {ambiguous} ambiguous; {unshown} not presented")
    if not samples:
        return
    groups = defaultdict(list)
    for sample in samples:
        groups[sample[0]].append(sample)

    def report(label, rows):
        metrics = (
            ("call -> copy start", [(r[2] - r[1]) / NS_PER_MS for r in rows]),
            ("copy duration", [(r[3] - r[2]) / NS_PER_MS for r in rows]),
            ("copy end -> presented", [(r[4] - r[3]) / NS_PER_MS for r in rows]),
            ("call -> presented", [(r[4] - r[1]) / NS_PER_MS for r in rows]),
        )
        print(f"  {label}: n {len(rows)}")
        for name, values in metrics:
            ordered = sorted(values)
            print(f"    {name}: p5 {percentile(ordered, .05):.2f} median {percentile(ordered, .5):.2f}"
                  f" p95 {percentile(ordered, .95):.2f} p99 {percentile(ordered, .99):.2f} ms")
        late = sum((r[3] - r[1]) / NS_PER_MS > COPY_LATE_MS for r in rows)
        print(f"    copy end later than call by > {COPY_LATE_MS:.0f} ms: {late} of {len(rows)}"
              f" ({100.0 * late / len(rows):.0f}%)")
        coefficient = correlation([r[3] - r[1] for r in rows], [r[4] - r[1] for r in rows])
        text = "n/a" if coefficient is None else f"{coefficient:.2f}"
        print(f"    correlation of (copy end - call) with (presented - call): {text}")

    for flags in sorted(groups):
        report(f"flags {flags}", groups[flags])
    report("overall", samples)


def main(argv):
    parser = argparse.ArgumentParser(description="Explain output gaps in a framegen-vulkan capture.")
    parser.add_argument("log")
    parser.add_argument("--marks", help="marks file written by fgvk-cli mark")
    parser.add_argument("--from", dest="start", type=float, help="window start, seconds from the first output")
    parser.add_argument("--to", dest="end", type=float, help="window end, seconds from the first output")
    parser.add_argument("--min-gap", type=float, help="report gaps of at least this many ms")
    args = parser.parse_args(argv)

    records = defaultdict(list)
    marks = []
    load(args.log, records, marks)
    if args.marks:
        load(args.marks, records, marks)

    outputs = sorted((o for o in records["pacing-profile-output"] if number(o, "started_ns") > 0),
                     key=lambda o: number(o, "started_ns"))
    if not outputs:
        print("no output records in the capture")
        return 1
    t0 = number(outputs[0], "started_ns")

    def seconds(ns):
        return (ns - t0) / NS_PER_S

    intervals = [number(b, "started_ns") - number(a, "started_ns") for a, b in zip(outputs, outputs[1:])]
    if args.min_gap is not None:
        min_gap_ms = args.min_gap
    elif intervals:
        min_gap_ms = 2.0 * statistics.median(intervals) / NS_PER_MS
    else:
        min_gap_ms = 0.0

    game_done = {}
    for split in records["pacing-profile-gpu-split"]:
        game_done.setdefault(int(number(split, "source")), number(split, "game_done_ns"))
    presented = {}
    presented_flags = {}
    for sample in records["pacing-profile-wayland"]:
        if sample.get("event") == "presented" and number(sample, "feedback") and "presented_ns" in sample:
            presented[int(number(sample, "feedback"))] = number(sample, "presented_ns")
            presented_flags[int(number(sample, "feedback"))] = integer(sample, "flags")

    frames = records["pacing-profile-app-frame"]
    calls = records["pacing-profile-call"]
    sleeps = records["pacing-profile-reflex-sleep"]
    acquires = records["pacing-profile-app-acquire"]

    def shown(output):
        return presented.get(int(number(output, "feedback")))

    def in_window(start, end):
        if args.start is not None and seconds(end) < args.start:
            return False
        if args.end is not None and seconds(start) > args.end:
            return False
        return True

    # A gap counts when the call-time step or the on-screen (presented_ns) step reaches min-gap.
    gaps = []
    display_only = 0
    for a, b in zip(outputs, outputs[1:]):
        start, end = number(a, "started_ns"), number(b, "started_ns")
        call_ms = (end - start) / NS_PER_MS
        shown_a, shown_b = shown(a), shown(b)
        shown_ms = (shown_b - shown_a) / NS_PER_MS if shown_a and shown_b else None
        if call_ms < min_gap_ms and (shown_ms is None or shown_ms < min_gap_ms):
            continue
        if not in_window(start, end):
            continue
        if call_ms < min_gap_ms:
            display_only += 1
        gaps.append((start, end, a, b))

    worker_wait = []
    for row in records["pacing-profile-thread"]:
        if row.get("role") == "worker" and number(row, "now_ns"):
            worker_wait.append((number(row, "now_ns"), number(row, "wait_ms"), number(row, "involuntary")))
    recorder_rows = [(number(r, "now_ns"), r) for r in records["pacing-profile-recorder"] if number(r, "now_ns")]

    def second_of(rows, at):
        # The per-second row whose window [now - 1 s, now] contains `at`.
        for now, row in rows:
            if now - NS_PER_S <= at <= now:
                return row
        return None

    def late_generated(output):
        # A generated output presented more than FG_LATE_MS after its source's game_done step.
        if output.get("generated") != "1":
            return False
        source = int(number(output, "source"))
        if source not in game_done:
            return False
        return (number(output, "started_ns") - game_done[source]) / NS_PER_MS > FG_LATE_MS

    classes = Counter()
    fg_late = 0
    print(f"capture: {len(outputs)} outputs, first at t=0, min gap {min_gap_ms:.1f} ms")
    for start, end, a, b in gaps:
        gap_ms = (end - start) / NS_PER_MS
        # App presents whose between-present interval [previous_exit, enter] overlaps the gap:
        # time inside layer calls on that thread against the whole interval.
        frames_in = [f for f in frames if number(f, "previous_exit_ns") > 0
                     and number(f, "previous_exit_ns") < end and number(f, "enter_ns") > start]
        frame_total = sum(number(f, "enter_ns") - number(f, "previous_exit_ns") for f in frames_in)
        inside = sum(number(f, "inside_layer_ns") for f in frames_in)
        if not frames_in:
            cls = "no app-frame"
        elif inside * 2 >= frame_total:
            cls = "inside-layer dominated"
        else:
            cls = "outside-layer dominated"
        classes[cls] += 1
        step = None
        source_a, source_b = int(number(a, "source")), int(number(b, "source"))
        if source_a in game_done and source_b in game_done:
            step = (game_done[source_b] - game_done[source_a]) / NS_PER_MS
        late = late_generated(a) or late_generated(b)
        if late:
            fg_late += 1
        step_text = f"{step:.1f} ms" if step is not None else "n/a"
        shown_a, shown_b = shown(a), shown(b)
        shown_text = f"{(shown_b - shown_a) / NS_PER_MS:.1f} ms" if shown_a and shown_b else "n/a"
        print(f"t={seconds(start):9.3f}s gap {gap_ms:7.1f} ms on-screen {shown_text}"
              f"  {describe_output(a)} -> {describe_output(b)}"
              f"  game_done step {step_text}  app-frame {cls}"
              f"  between app presents {frame_total / NS_PER_MS:.1f} ms, inside layer {inside / NS_PER_MS:.1f}"
              + ("  FG late > 3 ms after game_done" if late else ""))
        for call in sorted(overlapping(calls, start, end),
                           key=lambda c: number(c, "exit_ns") - number(c, "enter_ns"), reverse=True)[:3]:
            duration = (number(call, "exit_ns") - number(call, "enter_ns")) / NS_PER_MS
            print(f"    call {call.get('call', '?')} tid {call.get('tid', '?')} {duration:.2f} ms "
                  f"result {call.get('result', '?')}")
        for sleep in overlapping(sleeps, start, end):
            duration = (number(sleep, "exit_ns") - number(sleep, "enter_ns")) / NS_PER_MS
            print(f"    reflex-sleep {duration:.2f} ms result {sleep.get('result', '?')}")
        for acquire in overlapping(acquires, start, end):
            print(f"    acquire path={acquire.get('path', '?')} lock_wait_ms="
                  f"{number(acquire, 'lock_wait_ns') / NS_PER_MS:.2f} tail_wait_value="
                  f"{int(number(acquire, 'tail_wait_value'))} tail_seen_s={seconds(number(acquire, 'tail_seen_ns')):.3f}")

        worker = next(((w, i) for now, w, i in worker_wait if now - NS_PER_S <= end <= now), None)
        if worker:
            print(f"    worker run-queue wait {worker[0]:.2f} ms/s, involuntary switches {int(worker[1])}")
        health = second_of(recorder_rows, end)
        if health and (number(health, "dropped") or number(health, "window_slot_drops")):
            print(f"    RECORDER DROPS in this second: {int(number(health, 'dropped'))} queue,"
                  f" {int(number(health, 'window_slot_drops'))} window slot")

    for mark_ns, note in sorted(marks):
        print(f"mark t={seconds(mark_ns):.3f}s note={note}")
        for start, end, a, b in gaps:
            if start <= mark_ns + NS_PER_S and end >= mark_ns - NS_PER_S:
                print(f"    gap {(end - start) / NS_PER_MS:.1f} ms at t={seconds(start):.3f}s")

    reals = defaultdict(list)
    generated = []
    for output in outputs:
        feedback = int(number(output, "feedback"))
        if feedback not in presented:
            continue
        source = int(number(output, "source"))
        if output.get("generated") == "1":
            generated.append((source, presented[feedback]))
        else:
            reals[source].append(presented[feedback])
    violations = 0
    if not presented:
        print("order: no presented joins (the Wayland feedback is off)")
    else:
        for source in reals:
            reals[source].sort()
        for source, shown in sorted(generated):
            previous = reals.get(source - 1, [None])[0]
            current = reals.get(source, [None])[0]
            problems = []
            if previous is not None and shown < previous:
                problems.append("before real src" + str(source - 1))
            if current is not None and shown > current:
                problems.append("after real src" + str(source))
            if problems:
                violations += 1
                print(f"order violation: generated src{source} presented {'; '.join(problems)}")
        misplaced = []
        for source, at in generated:
            previous = reals.get(source - 1, [None])[0]
            current = reals.get(source, [None])[0]
            if previous is None or current is None or current <= previous:
                continue
            span = current - previous
            misplaced.append((abs((at - previous) / span - 0.5) * span / NS_PER_MS, (at - previous) / span))
        if misplaced:
            errors = sorted(m for m, _ in misplaced)
            positions = sorted(p for _, p in misplaced)
            pick = lambda v, q: v[min(len(v) - 1, int(len(v) * q))]
            print(f"generated on screen between its reals: position p5 {pick(positions, .05):.2f} median"
                  f" {pick(positions, .5):.2f} p95 {pick(positions, .95):.2f}; misplacement median"
                  f" {pick(errors, .5):.2f} p99 {pick(errors, .99):.2f} max {errors[-1]:.2f} ms;"
                  f" > 2 ms: {sum(e > 2 for e in errors)} of {len(errors)}")
        duplicates = sum(len(v) - 1 for v in reals.values() if len(v) > 1)
        print(f"order: {len(generated)} generated outputs presented, {violations} order violations,"
              f" {duplicates} duplicate reals")

    # Reflex: simulation start (marker 0) per present ID, matched to the app present by its
    # PRESENT_START marker (4) near an app-frame enter; then sim step against on-screen step of the reals.
    sim_start, present_start = {}, {}
    for marker in records["pacing-profile-reflex-marker"]:
        pid, kind = int(number(marker, "present_id")), int(number(marker, "marker"))
        if kind == 0:
            sim_start.setdefault(pid, number(marker, "ns"))
        elif kind == 4:
            present_start.setdefault(pid, number(marker, "ns"))
    if sim_start:
        enters = sorted((number(f, "enter_ns"), int(number(f, "source"))) for f in frames if number(f, "source"))
        import bisect
        keys = [e for e, _ in enters]
        real_shown = {src: min(v) for src, v in reals.items()} if presented else {}
        real_call = {}
        for output in outputs:
            if output.get("generated") != "1":
                real_call.setdefault(int(number(output, "source")), number(output, "started_ns"))
        pairs = []
        for pid, at in present_start.items():
            if pid not in sim_start or not keys:
                continue
            i = bisect.bisect_left(keys, at)
            best = min((j for j in (i - 1, i) if 0 <= j < len(keys)), key=lambda j: abs(keys[j] - at))
            if abs(keys[best] - at) <= 2 * NS_PER_MS:
                pairs.append((enters[best][1], sim_start[pid]))
        pairs.sort()
        errors = []
        for (s0, sim0), (s1, sim1) in zip(pairs, pairs[1:]):
            if s1 != s0 + 1:
                continue
            display = real_shown if s0 in real_shown and s1 in real_shown else real_call
            if s0 in display and s1 in display:
                errors.append(((display[s1] - display[s0]) - (sim1 - sim0)) / NS_PER_MS)
        if errors:
            absolute = sorted(abs(e) for e in errors)
            pick = lambda v, q: v[min(len(v) - 1, int(len(v) * q))]
            print(f"animation error (real display step - game simulation step, {len(errors)} pairs,"
                  f" {'on-screen' if real_shown else 'call time'}): median {pick(absolute, .5):.2f}"
                  f" p95 {pick(absolute, .95):.2f} p99 {pick(absolute, .99):.2f} max {absolute[-1]:.2f} ms;"
                  f" > 4 ms: {sum(a > 4 for a in absolute)}")
        else:
            print("animation error: Reflex markers present but none matched an app present")
    else:
        print("animation error: no Reflex simulation markers (Reflex off or not captured)")
    sleep_total = sum(number(r, "exit_ns") - number(r, "enter_ns") for r in sleeps) / NS_PER_MS
    if sleeps:
        longest = max(number(r, "exit_ns") - number(r, "enter_ns") for r in sleeps) / NS_PER_MS
        print(f"reflex sleeps: {len(sleeps)}, total {sleep_total:.0f} ms, longest {longest:.2f} ms")

    skips = Counter(skip.get("reason", "?") for skip in records["pacing-profile-skip"])
    for reason, count in sorted(skips.items()):
        print(f"skip reason {reason}: {count}")

    # Worker copy timestamps of generated outputs (FGVK_PACING_PROFILE_GPU_SPLIT=1 only). A sample
    # counts when its call time falls in --from/--to; unavailable and unjoined rows carry no call time.
    if records["pacing-profile-output-gpu"]:
        samples, unavailable, unjoined, ambiguous, unshown = after_call_samples(
            outputs, records["pacing-profile-output-gpu"], presented, presented_flags)
        samples = [sample for sample in samples if in_window(sample[1], sample[1])]
        print_after_call(samples, unavailable, unjoined, ambiguous, unshown)
    else:
        print("after-call split: no pacing-profile-output-gpu records"
              " (FGVK_PACING_PROFILE_GPU_SPLIT unset, or no generated copy was timed)")
    # T3b: the private -> lower copy of real outputs (internal images), FG on or off. With FG
    # off the copy waits the game's present semaphores, so its start is the game's GPU frame end.
    if records["pacing-profile-real-copy-gpu"]:
        samples, unavailable, unjoined, ambiguous, unshown = after_call_samples(
            outputs, records["pacing-profile-real-copy-gpu"], presented, presented_flags, False)
        samples = [sample for sample in samples if in_window(sample[1], sample[1])]
        print_after_call(samples, unavailable, unjoined, ambiguous, unshown, "real outputs")

    if records["pacing-profile-present-marker-gpu"]:
        print_present_marker(outputs, records["pacing-profile-output-gpu"],
            records["pacing-profile-present-marker-gpu"], presented, presented_flags, in_window)

    recorder_drops = sum(int(number(r, "dropped")) for r in records["pacing-profile-recorder"])
    slot_drops = sum(int(number(r, "window_slot_drops")) for r in records["pacing-profile-recorder"])
    print(f"recorder drops: {recorder_drops} queue, {slot_drops} window slot"
          " (per second rows carry now_ns; drops are printed under the gap they fall in)")
    print(f"totals: gaps {len(gaps)}; inside-layer dominated {classes['inside-layer dominated']};"
          f" outside-layer dominated {classes['outside-layer dominated']};"
          f" no app-frame {classes['no app-frame']}; FG late > 3 ms after game_done {fg_late};"
          f" order violations {violations}; on-screen gap with call step < min-gap {display_only}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

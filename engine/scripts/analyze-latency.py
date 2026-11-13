#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read the FG latency budget of a pacing-profile capture without changing runtime behaviour.

Input: a log written with FGVK_PACING_PROFILE=1 FGVK_PACING_PROFILE_GPU_SPLIT=1 (Wayland
presentation feedback is on by default with the profile). All record timestamps are steady-clock
(CLOCK_MONOTONIC) nanoseconds; Wayland samples on another clock are not subtracted.

Game present call: `pacing-profile-app-frame` `enter_ns`, taken by ApplicationPresentScope at the
application's vkQueuePresentKHR entry, before the layer's pre-present drain. The output records'
`arrival_ns` is the later limiter arrival inside Swapchain::present and is used only when no
app-frame record exists for that source (counted as `arrival_fallbacks`).

Reflex: markers recorded by the pass-through (`pacing-profile-reflex-marker`, the game's own
present IDs). The Vulkan present call is matched to the out-of-band present-start marker that
precedes it on the same thread (VKD3D sets OUT_OF_BAND_PRESENT_START/END around its
vkQueuePresentKHR); that marker's ID gives the frame's simulation and D3D present markers.

Joins use process/context/source and Wayland request surface/feedback identity. Only consecutive
game GPU steps of 10-18 ms qualify. At higher multipliers, generated slot means the first
generated output.
"""

import argparse
import bisect
from collections import Counter, defaultdict
import json
from pathlib import Path


STAGES = (
    "game present call -> game GPU done",
    "game GPU done -> generated ready",
    "generated ready -> generated slot",
    "generated slot -> real slot",
    "real slot -> on screen",
    "game present call -> on screen",
)

REFLEX_STAGES = (
    "simulation start -> simulation end",
    "simulation start -> D3D present start",
    "D3D present start -> game present call",
    "game present call -> game GPU done",
    "game GPU done -> on screen",
    "simulation start -> on screen",
)

# VkLatencyMarkerNV values.
SIMULATION_START, SIMULATION_END, PRESENT_START = 0, 1, 4
OUT_OF_BAND_PRESENT_START, OUT_OF_BAND_PRESENT_END = 10, 11


def stats(values):
    values = sorted(values)
    if not values:
        return {"n": 0, "p5": None, "median": None, "p95": None}
    return {"n": len(values), **{
        name: values[min(len(values) - 1, int(p * len(values)))]
        for name, p in (("p5", .05), ("median", .5), ("p95", .95))}}


def analyze(lines, flags=None):
    outputs, gpu, requests, presented = defaultdict(list), {}, {}, {}
    app_entries, app_duplicates = {}, set()
    markers = defaultdict(dict)
    oob = defaultdict(list)
    modes, sleeps = Counter(), []
    for line in lines:
        if not line.startswith("framegen-vulkan:"):
            continue
        fields = dict(word.split("=", 1) for word in line.split() if "=" in word)
        op = fields.get("operation")
        pid = fields.get("pid", "unknown")
        try:
            if op == "pacing-profile-output":
                if int(fields["result"]) < 0:
                    continue
                key = pid, fields["context"], int(fields["source"])
                outputs[key].append({k: int(fields.get(k, 0)) for k in (
                    "ordinal", "generated", "scheduled_ns", "started_ns", "arrival_ns", "feedback",
                    "batch_interval_ns", "batch_outputs")})
            elif op == "pacing-profile-gpu-split":
                key = pid, fields["context"], int(fields["source"])
                gpu[key] = int(fields["game_done_ns"]), int(fields["generated_end_ns"])
            elif op == "pacing-profile-wayland":
                identity = pid, fields["surface_token"], int(fields["feedback"])
                if fields["event"] == "request":
                    requests[identity] = (pid, fields["context"], int(fields["source"]),
                                          int(fields["generated"]))
                elif fields["event"] == "presented" and int(fields["clock_id"]) == 1:
                    presented[identity] = int(fields["presented_ns"]), int(fields["flags"])
            elif op == "pacing-profile-app-frame":
                key = pid, int(fields["source"])
                entry = int(fields["enter_ns"]), fields["tid"]
                if key in app_entries and app_entries[key] != entry:
                    app_duplicates.add(key)  # Two contexts in one process: source is ambiguous.
                app_entries[key] = entry
            elif op == "pacing-profile-reflex-marker":
                marker, present_id, at = int(fields["marker"]), int(fields["present_id"]), int(fields["ns"])
                markers[pid, present_id].setdefault(marker, at)
                if marker == OUT_OF_BAND_PRESENT_START:
                    oob[pid, fields["tid"]].append((at, present_id))
            elif op == "pacing-profile-reflex-sleep":
                sleeps.append((int(fields["exit_ns"]) - int(fields["enter_ns"])) / 1e6)
            elif op == "pacing-profile-reflex-mode":
                # Version 1 records carry no driver result.
                modes[(int(fields["low_latency"]), int(fields["boost"]),
                       int(fields["min_interval_us"]), fields.get("result", "?"))] += 1
        except (KeyError, ValueError):
            continue  # Truncated/unavailable records cannot supply evidence.
    for key in app_duplicates:
        del app_entries[key]
    for values in oob.values():
        values.sort()

    def reflex_frame(pid, entry, tid):
        """The Reflex markers of the frame whose Vulkan present call entered at `entry`."""
        starts = oob.get((pid, tid))
        if not starts:
            return None
        index = bisect.bisect_right(starts, (entry, float("inf"))) - 1
        if index < 0:
            return None
        start, present_id = starts[index]
        frame = markers[pid, present_id]
        end = frame.get(OUT_OF_BAND_PRESENT_END)
        if end is None or end < entry:
            return None  # The present call is not inside that out-of-band present.
        return frame

    display = defaultdict(list)
    for identity, value in presented.items():
        if identity in requests:
            display[requests[identity]].append((identity[2], *value))
    budgets = {stage: [] for stage in STAGES}
    reflex = {stage: [] for stage in REFLEX_STAGES}
    source_steps, period_errors, worker_period_errors = [], [], []
    flags_seen = Counter()
    arrival_fallbacks = 0
    for key, batch in outputs.items():
        previous = key[0], key[1], key[2] - 1
        if key not in gpu or previous not in gpu:
            continue
        done, ready = gpu[key]
        previous_done = gpu[previous][0]
        if not done or not ready or not previous_done:
            continue
        source_step = done - previous_done
        if not 10_000_000 <= source_step <= 18_000_000:
            continue
        generated = sorted((o for o in batch if o["generated"]), key=lambda o: o["ordinal"])
        real = [o for o in batch if not o["generated"]]
        if not generated or len(real) != 1:
            continue
        gen, real = generated[0], real[0]
        app = app_entries.get((key[0], key[2]))
        arrival = app[0] if app else gen["arrival_ns"]
        gen_slot, real_slot = gen["scheduled_ns"], real["scheduled_ns"]
        if not arrival or not gen_slot or not real_slot or not arrival <= done <= ready:
            continue
        if not app:
            arrival_fallbacks += 1
        # Negative ready->slot values are retained: they expose a late GPU batch.
        source_steps.append(source_step / 1e6)
        for stage, value in zip(STAGES[:4], (done-arrival, ready-done, gen_slot-ready, real_slot-gen_slot)):
            budgets[stage].append(value / 1e6)
        # Approximate the used batch period from scheduled spacing. Compression
        # or a real-output adjustment can affect this; it is not an estimator replay.
        if gen["batch_interval_ns"] > 0 and gen["batch_outputs"] > 0:
            worker_period_errors.append((gen["batch_interval_ns"] * gen["batch_outputs"] - source_step) / 1e6)
        interval = (real_slot - gen_slot) / len(generated)
        period_errors.append((interval * (len(generated) + 1) - source_step) / 1e6)
        matches = [p for p in display[(*key, 0)] if p[0] == real["feedback"]]
        if len(matches) != 1:
            continue
        _, on_screen, scanout_flags = matches[0]
        if flags is not None and scanout_flags != flags:
            continue
        if on_screen < real_slot:
            continue
        flags_seen[scanout_flags] += 1
        budgets[STAGES[4]].append((on_screen - real_slot) / 1e6)
        budgets[STAGES[5]].append((on_screen - arrival) / 1e6)
        frame = reflex_frame(key[0], *app) if app else None
        if not frame:
            continue
        sim_start, sim_end = frame.get(SIMULATION_START), frame.get(SIMULATION_END)
        d3d_present = frame.get(PRESENT_START)
        if sim_start is None or sim_end is None or d3d_present is None:
            continue
        if not (sim_start <= sim_end and sim_start <= d3d_present <= arrival):
            continue
        for stage, value in zip(REFLEX_STAGES, (sim_end - sim_start, d3d_present - sim_start,
                arrival - d3d_present, done - arrival, on_screen - done, on_screen - sim_start)):
            reflex[stage].append(value / 1e6)
    return {
        "arrival_fallbacks": arrival_fallbacks,
        "source_step_ms": stats(source_steps),
        "worker_batch_period_minus_gpu_step_ms": stats(worker_period_errors),
        "scheduled_batch_period_minus_gpu_step_ms": stats(period_errors),
        "display_flags": dict(flags_seen),
        "budget_ms": {stage: stats(values) for stage, values in budgets.items()},
        "reflex_modes": [{"low_latency": m[0], "boost": m[1], "min_interval_us": m[2], "result": m[3],
                          "count": n}
                         for m, n in sorted(modes.items())],
        "reflex_sleep_ms": {**stats(sleeps), "max": max(sleeps) if sleeps else None},
        "reflex_budget_ms": {stage: stats(values) for stage, values in reflex.items()},
    }


def print_table(title, budget):
    print(f"  {title:44} {'n':>7} {'p5':>9} {'median':>9} {'p95':>9}")
    for stage, sample in budget.items():
        cells = [f"{sample[k]:9.3f}" if sample[k] is not None else f"{'missing':>9}"
                 for k in ("p5", "median", "p95")]
        print(f"  {stage:44} {sample['n']:7} {' '.join(cells)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logs", type=Path, nargs="+")
    parser.add_argument("--flags", type=int, help="restrict display joins, e.g. 14 for torn direct scanout")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    for path in args.logs:
        with path.open(errors="replace") as stream:
            result = analyze(stream, args.flags)
        if args.json:
            print(json.dumps({"path": str(path), **result}, sort_keys=True))
            continue
        print(f"{path}:")
        print(f"  gameplay sources: {result['source_step_ms']['n']}; display flags: {result['display_flags']};"
              f" arrival fallbacks: {result['arrival_fallbacks']}")
        print_table("stage (ms)", result["budget_ms"])
        print("  worker batch period minus GPU step (ms):", result["worker_batch_period_minus_gpu_step_ms"])
        print("  scheduled batch period minus GPU step (approximate, ms):",
              result["scheduled_batch_period_minus_gpu_step_ms"])
        modes = ", ".join(f"low_latency={m['low_latency']} boost={m['boost']} "
                          f"min_interval_us={m['min_interval_us']} result={m['result']} x{m['count']}"
                          for m in result["reflex_modes"]) or "none recorded"
        sleep = result["reflex_sleep_ms"]
        print(f"  Reflex mode: {modes}")
        if sleep["n"]:
            print(f"  Reflex sleeps: {sleep['n']}, median {sleep['median']:.3f} ms,"
                  f" p95 {sleep['p95']:.3f} ms, max {sleep['max']:.3f} ms")
        else:
            print("  Reflex sleeps: none recorded")
        print_table("Reflex stage (ms)", result["reflex_budget_ms"])


if __name__ == "__main__":
    main()

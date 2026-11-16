#!/usr/bin/env python3
"""Exact on-screen analyzer for framegen-vulkan native Wayland pacing logs.

The layer writes the keys this reader joins on: pacing-profile-output (pid, context, feedback)
matches pacing-profile-wayland request (pid, context, feedback); the request's surface_token and
feedback match its presented or discarded event (pid, surface_token, feedback). Feedback ids are
per surface_token, so the surface is part of every outcome key.

Presented times are compositor reports (wp_presentation), not pixel measurements. Stdlib only.
The log is streamed line by line and only the numeric fields below are kept.

Usage: nice -n 19 python3 -I analyze-wayland-onscreen.py LOG [--from S] [--to S] [--context ID]
       [--json] [--compare-legacy]
"""

import argparse
import json
import math
import re
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path
from typing import NamedTuple, Optional

FIELD = re.compile(r"\b([a-z0-9_]+)=([^\s]+)")
WINDOW_NS = 2_000_000_000
SAME_TIME_NS = 250_000
CLOCK_TOLERANCE_NS = 1_000_000
MONOTONIC_CLOCK = 1
# VK_SUCCESS and VK_SUBOPTIMAL_KHR: the results the legacy join accepted for a lower call.
SUCCESS_RESULTS = (0, 1000001003)
FLAG_BITS = ((1, "vsync"), (2, "hw_clock"), (4, "hw_completion"), (8, "zero_copy"))
OUTPUT_FIELDS = ("pid", "context", "ordinal", "source", "generated", "image_index", "started_ns", "finished_ns", "feedback")
REQUEST_FIELDS = ("context", "source", "image_index", "generated", "requested_ns", "clock_id", "presentation_now_ns")
PRESENTED_FIELDS = ("received_ns", "clock_id", "presented_ns", "sequence", "refresh_ns", "flags", "output_id")
UNAVAILABLE_FIELDS = ("context", "source", "image_index", "generated", "requested_ns", "clock_id", "reason")
WAYLAND_EVENTS = ("request", "call", "presented", "discarded", "abandoned", "unavailable")


class Output(NamedTuple):
    ordinal: int
    source: int
    generated: int
    image: int
    started: int
    finished: int


class Request(NamedTuple):
    surface: int
    context: int
    source: int
    generated: int
    image: int
    requested: int
    clock: int
    now: int


class Outcome(NamedTuple):
    event: str
    received: int
    clock: Optional[int]
    presented: int
    refresh: int
    flags: int


class Call(NamedTuple):
    started: int
    finished: int
    result: int


class Row(NamedTuple):
    pid: int
    context: int
    feedback: int
    request: Request
    output: Optional[Output]
    outcome: Optional[Outcome]
    adjusted: Optional[int]  # presented time in layer clock; None when the event is not timed


def request_key(pid: int, context: int, feedback: int) -> tuple:
    return (pid, context, feedback)


def surface_key(pid: int, surface: int, feedback: int) -> tuple:
    return (pid, surface, feedback)


class Log:
    """Parsed numeric fields of one log. Lists per key, so duplicates are visible and never picked."""

    def __init__(self, keep_calls: bool):
        self.keep_calls = keep_calls
        self.counts = Counter()
        self.invalid = 0
        self.first_output_ns: Optional[int] = None
        self.outputs = defaultdict(list)
        self.requests = defaultdict(list)
        self.request_surfaces = defaultdict(list)
        self.outcomes = defaultdict(list)
        self.calls = defaultdict(list)
        self.clock_offsets: list[int] = []

    def add_output(self, body: str) -> None:
        fields = dict(FIELD.findall(body))
        try:
            if fields.get("version") not in ("1", "2"):
                raise ValueError("output version")
            values = {name: int(fields[name]) for name in OUTPUT_FIELDS}
            if min(values.values()) < 0 or values["generated"] not in (0, 1) or values["finished_ns"] < values["started_ns"]:
                raise ValueError("output values")
        except (KeyError, ValueError):
            self.invalid += 1
            return
        self.counts["outputs"] += 1
        if self.first_output_ns is None or values["started_ns"] < self.first_output_ns:
            self.first_output_ns = values["started_ns"]
        if values["feedback"] == 0:
            self.counts["outputs_without_feedback"] += 1
            return
        self.outputs[request_key(values["pid"], values["context"], values["feedback"])].append(
            Output(values["ordinal"], values["source"], values["generated"], values["image_index"],
                   values["started_ns"], values["finished_ns"]))

    def add_wayland(self, body: str) -> None:
        fields = dict(FIELD.findall(body))
        event = fields.get("event")
        try:
            if fields.get("version") != "2" or event not in WAYLAND_EVENTS:
                raise ValueError("wayland event")
            pid, surface, feedback = (int(fields[name]) for name in ("pid", "surface_token", "feedback"))
            if min(pid, surface) < 0 or feedback <= 0:
                raise ValueError("wayland identity")
            if event == "request":
                values = {name: int(fields[name]) for name in REQUEST_FIELDS}
                if min(values.values()) < 0 or values["generated"] not in (0, 1):
                    raise ValueError("request values")
                request = Request(surface, values["context"], values["source"], values["generated"],
                                  values["image_index"], values["requested_ns"], values["clock_id"],
                                  values["presentation_now_ns"])
                self.counts["requests"] += 1
                self.requests[request_key(pid, request.context, feedback)].append(request)
                self.request_surfaces[surface_key(pid, surface, feedback)].append(request)
                self.clock_offsets.append(values["presentation_now_ns"] - values["requested_ns"])
            elif event == "call":
                started, finished, result = int(fields["started_ns"]), int(fields["finished_ns"]), int(fields["result"])
                if started < 0 or finished < started:
                    raise ValueError("call times")
                self.counts["calls"] += 1
                if self.keep_calls:
                    self.calls[surface_key(pid, surface, feedback)].append(Call(started, finished, result))
            elif event == "presented":
                values = {name: int(fields[name]) for name in PRESENTED_FIELDS}
                if min(values.values()) < 0:
                    raise ValueError("presented values")
                self.counts["presented"] += 1
                if values["clock_id"] != MONOTONIC_CLOCK:
                    self.counts["presented_clock_not_monotonic"] += 1
                if values["presented_ns"] == 0:
                    self.counts["presented_without_timestamp"] += 1
                self.outcomes[surface_key(pid, surface, feedback)].append(Outcome(
                    event, values["received_ns"], values["clock_id"], values["presented_ns"],
                    values["refresh_ns"], values["flags"]))
            elif event in ("discarded", "abandoned"):
                received = int(fields["received_ns"])
                if received < 0:
                    raise ValueError("received time")
                self.counts[event] += 1
                self.outcomes[surface_key(pid, surface, feedback)].append(Outcome(event, received, None, 0, 0, 0))
            else:
                if min(int(fields[name]) for name in UNAVAILABLE_FIELDS) < 0:
                    raise ValueError("unavailable values")
                self.counts["unavailable"] += 1
        except (KeyError, ValueError):
            self.invalid += 1

    def add_gap(self, body: str) -> None:
        fields = dict(FIELD.findall(body))
        try:
            if fields.get("version") != "1":
                raise ValueError("gap version")
            lost = int(fields["lost_records"])
            if min(int(fields["pid"]), int(fields["surface_token"]), lost) < 0:
                raise ValueError("gap values")
        except (KeyError, ValueError):
            self.invalid += 1
            return
        self.counts["wayland_gap_records"] += 1
        self.counts["wayland_gap_lost"] += lost


def read_log(path: Path, keep_calls: bool) -> Log:
    log = Log(keep_calls)
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if "operation=pacing-profile-" not in line:
                continue
            _, _, rest = line.partition("operation=")
            operation, _, body = rest.partition(" ")
            if operation == "pacing-profile-output":
                log.add_output(body)
            elif operation == "pacing-profile-wayland":
                log.add_wayland(body)
            elif operation == "pacing-profile-wayland-gap":
                log.add_gap(body)
    return log


def clock_offset(offsets: list[int]) -> int:
    """Compositor clock minus layer clock, applied only when it is larger than the tolerance."""
    if not offsets:
        return 0
    median = statistics.median(offsets)
    return round(median) if abs(median) > CLOCK_TOLERANCE_NS else 0


def build_rows(log: Log, offset: int) -> list[Row]:
    rows = []
    for key, requests in log.requests.items():
        pid, _, feedback = key
        if len(requests) != 1:
            continue
        request = requests[0]
        if len(log.request_surfaces[surface_key(pid, request.surface, feedback)]) != 1:
            continue
        outputs = log.outputs.get(key, [])
        outcomes = log.outcomes.get(surface_key(pid, request.surface, feedback), [])
        output = outputs[0] if len(outputs) == 1 else None
        outcome = outcomes[0] if len(outcomes) == 1 else None
        timed = (outcome is not None and outcome.event == "presented" and outcome.clock == MONOTONIC_CLOCK
                 and outcome.presented > 0)
        rows.append(Row(pid, request.context, feedback, request, output, outcome,
                        outcome.presented - offset if timed else None))
    return rows


def role(row: Row) -> str:
    return "generated" if row.request.generated else "real"


def stats(values_ns) -> dict:
    """Nearest-rank percentiles in milliseconds; values are nanoseconds."""
    ordered = sorted(values_ns)
    if not ordered:
        return {"n": 0, "p50": None, "p95": None, "p99": None, "max": None}

    def at(fraction: float) -> float:
        return ordered[round((len(ordered) - 1) * fraction)] / 1e6
    return {"n": len(ordered), "p50": at(.5), "p95": at(.95), "p99": at(.99), "max": ordered[-1] / 1e6}


def ideal_groups(selected: list[Row]) -> dict:
    """Generated frames of source s sit between the reals of source s-1 and s (same context)."""
    buckets = defaultdict(lambda: {"reals": {}, "generated": defaultdict(list), "ambiguous": set()})
    for row in selected:
        bucket = buckets[row.pid, row.context]
        source = row.request.source
        if row.request.generated:
            bucket["generated"][source].append(row)
        elif source in bucket["reals"]:
            bucket["ambiguous"].add(source)
        else:
            bucket["reals"][source] = row
    result = {"m": Counter(), "skipped": Counter(), "errors": [], "outside": 0, "complete": 0, "orphan_generated": 0}
    for bucket in buckets.values():
        reals, generated, ambiguous = bucket["reals"], bucket["generated"], bucket["ambiguous"]
        result["orphan_generated"] += sum(len(rows) for source, rows in generated.items() if source not in reals)
        sources = sorted(reals)
        for s0, s1 in zip(sources, sources[1:]):
            gens = sorted(generated.get(s1, []), key=lambda r: r.request.requested)
            result["m"][len(gens) + 1] += 1
            if s0 in ambiguous or s1 in ambiguous:
                result["skipped"]["ambiguous source"] += 1
                continue
            if s1 - s0 != 1:
                result["skipped"]["source gap"] += 1
                continue
            before, after = reals[s0], reals[s1]
            if before.adjusted is None or after.adjusted is None:
                result["skipped"]["real not shown"] += 1
                continue
            if any(g.adjusted is None for g in gens):
                result["skipped"]["generated not shown"] += 1
                continue
            r0, r1 = before.adjusted, after.adjusted
            if r1 <= r0:
                result["skipped"]["real order inversion"] += 1
                continue
            result["complete"] += 1
            m = len(gens) + 1
            for k, gen in enumerate(gens, start=1):
                if not r0 < gen.adjusted < r1:
                    result["outside"] += 1
                ideal = r0 + (r1 - r0) * k / m
                result["errors"].append((k, gen.adjusted - ideal, gen.request.requested))
    return result


def ordinal_inversions(rows: list[Row]) -> Counter:
    """Presented-time order against issue (ordinal) order within each context; ordinals restart per context."""
    per_context = defaultdict(list)
    for row in rows:
        if row.output is not None and row.adjusted is not None:
            per_context[row.pid, row.context].append(row)
    inversions = Counter()
    for members in per_context.values():
        ordered = sorted(members, key=lambda r: r.output.ordinal)
        for a, b in zip(ordered, ordered[1:]):
            if b.adjusted < a.adjusted:
                inversions[f"{role(a)}->{role(b)}"] += 1
    return inversions


def legacy_join(selected: list[Row], log: Log) -> Counter:
    """The old join: generated and image_index within pid/context/source, unique lower-call containment."""
    owners = defaultdict(list)
    for row in selected:
        if row.output is not None:
            owners[row.pid, row.context, row.output.source, row.output.generated, row.output.image].append(row)
    partners = defaultdict(list)
    for row in selected:
        calls = log.calls.get(surface_key(row.pid, row.request.surface, row.feedback), [])
        if len(calls) != 1 or calls[0].result not in SUCCESS_RESULTS:
            continue
        call = calls[0]
        request = row.request
        candidates = [o for o in owners.get((row.pid, row.context, request.source, request.generated, request.image), [])
                      if o.output.started <= call.started and call.finished <= o.output.finished]
        if len(candidates) == 1:
            partners[row.pid, row.context, candidates[0].feedback].append(row.feedback)
    counts = Counter()
    for row in selected:
        if row.output is None:
            continue
        mapped = partners.get((row.pid, row.context, row.feedback), [])
        counts["outputs"] += 1
        if not mapped:
            counts["unmatched"] += 1
        elif len(mapped) > 1:
            counts["ambiguous"] += 1
        else:
            counts["matched"] += 1
            if mapped[0] != row.feedback:
                counts["different"] += 1
    return counts


def analyze(log: Log, context: Optional[int], from_s: float, to_s: Optional[float], compare_legacy: bool) -> dict:
    offset = clock_offset(log.clock_offsets)
    rows = build_rows(log, offset)
    t0 = log.first_output_ns
    if t0 is None:
        t0 = min((r.request.requested for r in rows), default=0)

    joins = Counter()
    for row in rows:
        if row.output is None:
            joins["requests_without_output"] += 1
            joins["requests_without_output_" + (row.outcome.event if row.outcome else "no_outcome")] += 1
    joins["outputs_without_request"] = sum(len(v) for k, v in log.outputs.items() if k not in log.requests)
    joins["outcomes_without_request"] = sum(len(v) for k, v in log.outcomes.items() if k not in log.request_surfaces)
    joins["presented_without_request"] = sum(1 for k, v in log.outcomes.items()
                                              if k not in log.request_surfaces for e in v if e.event == "presented")
    joins["duplicate_requests"] = sum(len(v) > 1 for v in log.requests.values())
    joins["duplicate_request_surfaces"] = sum(len(v) > 1 for v in log.request_surfaces.values())
    joins["duplicate_outputs"] = sum(len(v) > 1 for v in log.outputs.values())
    joins["duplicate_outcomes"] = sum(len(v) > 1 for v in log.outcomes.values())
    joins["outputs_without_feedback"] = log.counts["outputs_without_feedback"]
    joins["presented_clock_not_monotonic"] = log.counts["presented_clock_not_monotonic"]
    joins["presented_without_timestamp"] = log.counts["presented_without_timestamp"]
    joins["ordinal_inversions"] = dict(ordinal_inversions(rows))

    from_ns = round(from_s * 1e9)
    to_ns = math.inf if to_s is None else round(to_s * 1e9)
    selected = [r for r in rows if (context is None or r.context == context)
                and from_ns <= r.request.requested - t0 < to_ns]

    shown = sorted((r for r in selected if r.adjusted is not None), key=lambda r: (r.adjusted, r.feedback))
    pairs = [(b.adjusted - a.adjusted, f"{role(a)}->{role(b)}", b.request.requested, b.outcome.refresh)
             for a, b in zip(shown, shown[1:])]
    p2s = [(r.request.requested, r.adjusted - r.output.finished) for r in shown if r.output is not None]
    ideal = ideal_groups(selected)
    presented_here = [r.outcome for r in selected if r.outcome is not None and r.outcome.event == "presented"]

    windows = defaultdict(lambda: {"reals": 0, "outputs": 0, "shown": 0, "discarded": 0,
                                   "intervals": [], "errors": [], "present_to_screen": []})
    for r in selected:
        bucket = windows[(r.request.requested - t0) // WINDOW_NS]
        bucket["outputs"] += 1
        bucket["reals"] += r.request.generated == 0
        bucket["shown"] += r.adjusted is not None
        bucket["discarded"] += r.outcome is not None and r.outcome.event == "discarded"
    for diff, _, when, _ in pairs:
        windows[(when - t0) // WINDOW_NS]["intervals"].append(diff)
    for _, error, when in ideal["errors"]:
        windows[(when - t0) // WINDOW_NS]["errors"].append(error)
    for when, value in p2s:
        windows[(when - t0) // WINDOW_NS]["present_to_screen"].append(value)

    transitions = {}
    for name in ("real->real", "real->generated", "generated->real", "generated->generated"):
        transitions[name] = stats(diff for diff, label, _, _ in pairs if label == name)
    periods = Counter()
    for diff, _, _, refresh in pairs:
        if refresh > 0:
            periods[math.floor(diff / refresh + 0.5)] += 1
    flags = Counter(o.flags for o in presented_here)

    result = {
        "log": None,
        "t0_ns": t0,
        "selection": {"context": context, "from_s": from_s, "to_s": to_s},
        "clock": {"requests": len(log.clock_offsets),
                  "median_ns": statistics.median(log.clock_offsets) if log.clock_offsets else None,
                  "offset_applied_ns": offset},
        "totals": {name: log.counts[name] for name in ("outputs", "requests", "presented", "discarded",
                                                       "abandoned", "unavailable", "calls",
                                                       "wayland_gap_records", "wayland_gap_lost")}
                  | {"invalid_records": log.invalid},
        "joins": dict(joins),
        "selected": {"requests": len(selected),
                     "reals": sum(r.request.generated == 0 for r in selected),
                     "generated": sum(r.request.generated == 1 for r in selected),
                     "presented": sum(r.outcome is not None and r.outcome.event == "presented" for r in selected),
                     "discarded_real": sum(r.outcome is not None and r.outcome.event == "discarded" and r.request.generated == 0 for r in selected),
                     "discarded_generated": sum(r.outcome is not None and r.outcome.event == "discarded" and r.request.generated == 1 for r in selected)},
        "present_to_screen_ms": stats(v for _, v in p2s) | {"negative": sum(v < 0 for _, v in p2s)},
        "intervals_ms": stats(diff for diff, _, _, _ in pairs)
                        | {"same_time_equal": sum(diff == 0 for diff, _, _, _ in pairs),
                           "same_time_under_0_25_ms": sum(diff < SAME_TIME_NS for diff, _, _, _ in pairs)},
        "transitions_ms": transitions,
        "ideal": {"complete": ideal["complete"], "m_histogram": {str(m): n for m, n in sorted(ideal["m"].items())},
                  "skipped": dict(ideal["skipped"]), "generated_outside_reals": ideal["outside"],
                  "orphan_generated": ideal["orphan_generated"],
                  "by_k": {}},
        "refresh_ns": {str(v): n for v, n in sorted(Counter(o.refresh for o in presented_here).items())},
        "refresh_periods": {str(k): n for k, n in sorted(periods.items())},
        "flags": {str(v): n for v, n in sorted(flags.items())},
        "windows": [],
        "legacy": None,
    }
    by_k = defaultdict(list)
    for k, error, _ in ideal["errors"]:
        by_k[k].append(error)
    for k in sorted(by_k):
        values = by_k[k]
        result["ideal"]["by_k"][str(k)] = {"abs_error_ms": stats(abs(v) for v in values),
                                           "signed_p50_ms": stats(values)["p50"]}
    for index in sorted(windows):
        bucket = windows[index]
        start = index * WINDOW_NS / 1e9
        result["windows"].append({
            "start_s": start, "end_s": start + WINDOW_NS / 1e9,
            "reals_per_s": bucket["reals"] / (WINDOW_NS / 1e9), "outputs_per_s": bucket["outputs"] / (WINDOW_NS / 1e9),
            "shown": bucket["shown"], "discarded": bucket["discarded"],
            "interval_ms": stats(bucket["intervals"]), "error_ms": stats(abs(v) for v in bucket["errors"]),
            "present_to_screen_ms": stats(bucket["present_to_screen"]),
        })
    if compare_legacy:
        result["legacy"] = dict(legacy_join(selected, log))
    return result


def fmt(value) -> str:
    return "-" if value is None else f"{value:.3f}"


def fmt_stats(s: dict) -> str:
    return f"n={s['n']} p50={fmt(s['p50'])} p95={fmt(s['p95'])} p99={fmt(s['p99'])} max={fmt(s['max'])} ms"


def render_text(result: dict, path: Path) -> str:
    sel = result["selection"]
    lines = [f"Wayland on-screen report: {path}",
             "Presented times are compositor reports (wp_presentation), not pixel measurements.",
             f"t0 (first output started_ns) = {result['t0_ns']}; selection: context={sel['context'] if sel['context'] is not None else 'all'}, "
             f"from={sel['from_s']:g} s, to={'end' if sel['to_s'] is None else format(sel['to_s'], 'g') + ' s'} (seconds since t0)."]
    t = result["totals"]
    lines.append(f"Log totals (whole log): outputs={t['outputs']} requests={t['requests']} presented={t['presented']} "
                 f"discarded={t['discarded']} abandoned={t['abandoned']} unavailable={t['unavailable']} calls={t['calls']} "
                 f"invalid_records={t['invalid_records']} wayland_gap_records={t['wayland_gap_records']} "
                 f"wayland_gap_lost={t['wayland_gap_lost']}.")
    j = result["joins"]
    lines.append(f"Joins (whole log): outputs with feedback 0={j.get('outputs_without_feedback', 0)}; "
                 f"requests without output={j.get('requests_without_output', 0)} "
                 f"(presented {j.get('requests_without_output_presented', 0)}, discarded {j.get('requests_without_output_discarded', 0)}, "
                 f"abandoned {j.get('requests_without_output_abandoned', 0)}, no outcome {j.get('requests_without_output_no_outcome', 0)}); "
                 f"outputs without request={j.get('outputs_without_request', 0)}; "
                 f"outcomes without request={j.get('outcomes_without_request', 0)} (presented {j.get('presented_without_request', 0)}).")
    lines.append(f"  presented with clock_id != 1 (dropped from timing)={j.get('presented_clock_not_monotonic', 0)}; "
                 f"presented without timestamp={j.get('presented_without_timestamp', 0)}; "
                 f"duplicate keys: requests={j.get('duplicate_requests', 0)}, request surfaces={j.get('duplicate_request_surfaces', 0)}, "
                 f"outputs={j.get('duplicate_outputs', 0)}, outcomes={j.get('duplicate_outcomes', 0)} (duplicated keys are excluded).")
    inversions = j.get("ordinal_inversions", {})
    lines.append("  presented time against issue (ordinal) order, timed outputs: inversions="
                 f"{sum(inversions.values())}" + (" (" + ", ".join(f"{k} {v}" for k, v in sorted(inversions.items())) + ")" if inversions else "") + ".")
    c = result["clock"]
    median = "-" if c["median_ns"] is None else f"{c['median_ns']:g}"
    lines.append(f"Clock: median(presentation_now_ns - requested_ns) = {median} ns over {c['requests']} requests; "
                 f"offset applied to presented times = {c['offset_applied_ns']} ns.")
    s = result["selected"]
    lines.append(f"Selection: requests={s['requests']} reals={s['reals']} generated={s['generated']} presented={s['presented']} "
                 f"discarded real={s['discarded_real']} generated={s['discarded_generated']}.")
    lines.append("present -> screen (presented - finished): " + fmt_stats(result["present_to_screen_ms"])
                 + f"; negative={result['present_to_screen_ms']['negative']}.")
    iv = result["intervals_ms"]
    lines.append("on-screen interval (consecutive shown outputs): " + fmt_stats(iv)
                 + f"; same-time pairs equal={iv['same_time_equal']}, under 0.25 ms={iv['same_time_under_0_25_ms']}.")
    for name, value in result["transitions_ms"].items():
        lines.append(f"  {name}: {fmt_stats(value)}")
    ideal = result["ideal"]
    lines.append(f"generated vs ideal: complete real pairs={ideal['complete']}; multiplier histogram (m per real pair)="
                 + json.dumps(ideal["m_histogram"], sort_keys=True) + f"; generated outside (R0, R1)={ideal['generated_outside_reals']}; "
                 f"orphan generated={ideal['orphan_generated']}; skipped={json.dumps(ideal['skipped'], sort_keys=True)}.")
    for k, value in ideal["by_k"].items():
        a = value["abs_error_ms"]
        lines.append(f"  k={k}: |error| n={a['n']} p50={fmt(a['p50'])} p95={fmt(a['p95'])} p99={fmt(a['p99'])} ms; "
                     f"signed p50={fmt(value['signed_p50_ms'])} ms (positive = later than ideal)")
    lines.append(f"discarded: real={s['discarded_real']} generated={s['discarded_generated']}.")
    lines.append("refresh_ns seen (presented events): " + (", ".join(f"{k} x{v}" for k, v in result["refresh_ns"].items()) or "none")
                 + "; intervals in refresh periods (rounded, informational; sub-period values are expected under tearing): "
                 + (", ".join(f"{k}: {v}" for k, v in result["refresh_periods"].items()) or "none") + ".")
    flag_names = []
    for value, _ in sorted(result["flags"].items(), key=lambda kv: int(kv[0])):
        bits = [name for bit, name in FLAG_BITS if int(value) & bit]
        flag_names.append(f"{value} ({', '.join(bits) or 'none'}) x{result['flags'][value]}")
    lines.append("flags: " + (", ".join(flag_names) or "none") + ".")
    lines.append("  flags are not proof of tearing or of no tearing.")
    lines.append("windows (2 s from t0; outputs = lower presents with a request):")
    for w in result["windows"]:
        lines.append(f"  +{w['start_s']:g}-{w['end_s']:g} s: reals/s {w['reals_per_s']:.1f}, outputs/s {w['outputs_per_s']:.1f}, "
                     f"shown {w['shown']}, discarded {w['discarded']}, interval p99 {fmt(w['interval_ms']['p99'])} ms "
                     f"(n {w['interval_ms']['n']}), error p99 {fmt(w['error_ms']['p99'])} ms (n {w['error_ms']['n']}), "
                     f"present -> screen p50 {fmt(w['present_to_screen_ms']['p50'])} ms")
    if result["legacy"] is not None:
        lg = result["legacy"]
        lines.append("legacy join (generated + image_index within pid/context/source, unique lower-call containment): "
                     f"outputs={lg.get('outputs', 0)} matched={lg.get('matched', 0)} "
                     f"matched a different request than the exact join={lg.get('different', 0)} "
                     f"unmatched={lg.get('unmatched', 0)} ambiguous={lg.get('ambiguous', 0)}.")
    return "\n".join(lines)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("log", type=Path)
    parser.add_argument("--from", dest="from_s", type=float, default=0.0, help="window start, seconds since t0")
    parser.add_argument("--to", dest="to_s", type=float, default=None, help="window end, seconds since t0")
    parser.add_argument("--context", type=int, default=None, help="only this context")
    parser.add_argument("--json", action="store_true", help="print JSON instead of text")
    parser.add_argument("--compare-legacy", action="store_true", help="also run the old generated+image_index join")
    args = parser.parse_args(argv)
    if not math.isfinite(args.from_s) or args.from_s < 0:
        parser.error("--from must be a finite number >= 0")
    if args.to_s is not None and (not math.isfinite(args.to_s) or args.to_s <= args.from_s):
        parser.error("--to must be finite and greater than --from")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        log = read_log(args.log, keep_calls=args.compare_legacy)
    except OSError as exc:
        print(f"framegen-vulkan: cannot read {args.log}: {exc}", file=sys.stderr)
        return 2
    result = analyze(log, args.context, args.from_s, args.to_s, args.compare_legacy)
    result["log"] = str(args.log)
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print(render_text(result, args.log))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""Summarize framegen-vulkan CPU/GPU pacing and opt-in native Wayland presentation feedback."""

import argparse
import json
import os
from collections import defaultdict
import math
from pathlib import Path
import re
from typing import NamedTuple


class Window(NamedTuple):
    pid: int
    context: int
    version: int
    values: dict[str, float]
    swapchain: str = ""
    wsi: str = "unknown"


def read_windows(path: Path) -> tuple[list[Window], int]:
    windows = []
    invalid = 0
    marker = "framegen-vulkan: present diagnostics: operation=pacing-profile "
    required = ("window_ms", "sources", "output_attempts", "source_interval_samples",
                "source_interval_mean_ms", "source_interval_max_ms")
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if marker not in line:
                continue
            fields = dict(re.findall(r"\b([a-z0-9_]+)=([^\s]+)", line.split(marker, 1)[1]))
            try:
                pid, context, version = (int(fields[key]) for key in ("pid", "context", "version"))
                if min(pid, context) < 0 or version not in (1, 2):
                    raise ValueError("unsupported identity or version")
                values = {key: float(value) for key, value in fields.items()
                          if key not in ("pid", "context", "version", "swapchain", "wsi")}
                if not all(math.isfinite(value) and value >= 0 for value in values.values()):
                    raise ValueError("invalid measurement")
                if not all(key in values for key in required) or values["window_ms"] <= 0:
                    raise ValueError("incomplete window")
                wsi = fields.get("wsi", "unknown")
                if wsi not in ("unknown", "wayland", "xcb", "xlib"):
                    raise ValueError("unknown WSI provenance")
                windows.append(Window(pid, context, version, values, fields.get("swapchain", ""), wsi))
            except (KeyError, ValueError, OverflowError):
                invalid += 1
    return windows, invalid


def read_observations(path: Path) -> tuple[dict[str, list[dict]], int]:
    observations = {"outputs": [], "waits": [], "presents": [], "wayland": [], "wayland_gaps": [], "limits": [], "wraps": [], "sources": [], "gpu_frames": [], "application_calls": [], "wait_calls": [], "capture_gaps": [], "sessions": [], "acquire_driver": [], "copies": [], "copy_gpu": []}
    invalid = 0
    prefix = "framegen-vulkan: present diagnostics: operation="
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if prefix not in line:
                continue
            fields = dict(re.findall(r"\b([a-z0-9_]+)=([^\s]+)", line.split(prefix, 1)[1]))
            operation = line.split(prefix, 1)[1].split(" ", 1)[0]
            try:
                if operation in ("pacing-profile-source", "pacing-profile-gpu", "pacing-profile-application", "pacing-profile-wait"):
                    required = {
                        "pacing-profile-source": ("pid", "tid", "context", "source", "started_ns", "finished_ns", "backend_scheduled", "backend_epoch", "backend_frame", "frame_generation_enabled", "adaptive", "multiplier", "smooth_cadence", "effective_cap_fps", "configured_cap_fps", "cap_wait_ns", "readiness_wait_ns", "output_wait_ns", "private_total_ns", "render_fence_ns", "schedule_ns", "source_copy_ns", "acquire_ns", "generated_submit_ns", "generated_present_ns", "original_present_ns", "unattributed_ns"),
                        "pacing-profile-gpu": ("pid", "context", "source", "backend_epoch", "backend_frame", "previous_wait_us", "prepass_submit_us", "generation_submit_us", "prepass_us", "span_us", "readback_us", "outputs"),
                        "pacing-profile-application": ("pid", "tid", "stream", "ordinal", "context", "source", "image_index", "started_ns", "finished_ns", "result_known", "result", "timeout_known", "timeout_ns", "join_known", "join_ns", "configuration_known", "configuration_ns"),
                        "pacing-profile-wait": ("pid", "tid", "present_id", "timeout_ns", "result", "started_ns", "finished_ns"),
                    }[operation]
                    if int(fields["version"]) != 1:
                        raise ValueError("unsupported frame trace")
                    numeric = lambda key: float(fields[key]) if key.endswith(("_us", "_fps")) else int(fields[key])
                    record = {key: numeric(key) for key in required}
                    if any(not math.isfinite(value) or (value < 0 and key != "result") for key, value in record.items()):
                        raise ValueError("invalid frame trace value")
                    if "finished_ns" in record and record["finished_ns"] < record["started_ns"]:
                        raise ValueError("backwards frame trace")
                    for key in ("backend_scheduled", "frame_generation_enabled", "adaptive", "smooth_cadence", "result_known", "timeout_known", "join_known", "configuration_known"):
                        if key in record and record[key] not in (0, 1):
                            raise ValueError("invalid trace boolean")
                    if operation == "pacing-profile-gpu":
                        if not 1 <= record["outputs"] <= 4:
                            raise ValueError("invalid GPU output count")
                        record["output_us"] = [float(fields[f"output_{i}_us"]) for i in range(record["outputs"])]
                        if any(not math.isfinite(value) or value < 0 for value in record["output_us"]):
                            raise ValueError("invalid GPU output duration")
                    else:
                        record["swapchain"] = fields["swapchain"]
                    if operation in ("pacing-profile-application", "pacing-profile-wait"):
                        record["api"] = fields["api"]
                        if record["api"] not in ("present", "acquire", "acquire2", "KHR", "KHR2"):
                            raise ValueError("unknown application API")
                    if operation == "pacing-profile-application":
                        if "caller" in fields:
                            record["caller"] = fields["caller"]
                        if "call_id" in fields:
                            record.update({key: int(fields[key]) for key in ("call_id", "lower_calls", "lower_ns")})
                            if record["call_id"] <= 0 or min(record["lower_calls"], record["lower_ns"]) < 0 or record["lower_ns"] > record["finished_ns"] - record["started_ns"]:
                                raise ValueError("invalid acquisition boundaries")
                    bucket = {"pacing-profile-source": "sources", "pacing-profile-gpu": "gpu_frames", "pacing-profile-application": "application_calls", "pacing-profile-wait": "wait_calls"}[operation]
                    observations[bucket].append(record)
                elif operation == "pacing-profile-copy-gpu":
                    keys = ("pid", "context", "source", "generation", "slot", "generated", "image_index", "query_result", "available",
                            "begin_ticks", "end_ticks", "valid_bits", "collected_ns", "read_started_ns", "read_finished_ns")
                    record = {key: int(fields[key]) for key in keys}
                    record["period_ns"] = float(fields["period_ns"])
                    for calibration_name in ("before", "after"):
                        for suffix in ("result", "device_ticks", "host_ns", "deviation_ns", "started_ns", "finished_ns"):
                            key = calibration_name+"_"+suffix
                            record[key] = int(fields[key])
                        if record[calibration_name+"_finished_ns"] < record[calibration_name+"_started_ns"]:
                            raise ValueError("backwards calibration")
                    if (int(fields["version"]) != 1 or any(value < 0 for key, value in record.items() if not key.endswith("result"))
                            or not math.isfinite(record["period_ns"]) or record["period_ns"] <= 0
                            or not 1 <= record["valid_bits"] <= 64 or not 0 <= record["slot"] < 5
                            or record["generated"] not in (0, 1) or record["available"] not in (0, 1)
                            or bool(record["slot"]) != bool(record["generated"])
                            or record["available"] and record["query_result"] not in (0, 1)
                            or not record["collected_ns"] <= record["read_started_ns"] <= record["read_finished_ns"]):
                        raise ValueError("invalid native GPU trace")
                    for key in ("swapchain", "device", "queue", "command"):
                        record[key] = fields[key]
                    observations["copy_gpu"].append(record)
                elif operation in ("pacing-profile-acquire-driver", "pacing-profile-copy"):
                    if int(fields["version"]) != 1:
                        raise ValueError("unsupported native boundary trace")
                    keys = (("pid", "tid", "call_id", "attempt", "timeout_ns", "started_ns", "finished_ns", "result", "image_index")
                            if operation == "pacing-profile-acquire-driver" else
                            ("pid", "tid", "context", "source", "generated", "image_index", "pass", "started_ns", "finished_ns", "submitted", "wait_value", "signal_value", "wait_count", "wait_recorded", "signal_count", "signal_recorded"))
                    record = {key: int(fields[key]) for key in keys}
                    if any(value < 0 for key, value in record.items() if key != "result") or record["finished_ns"] < record["started_ns"]:
                        raise ValueError("invalid native boundary trace")
                    if operation == "pacing-profile-acquire-driver":
                        if not record["call_id"] or not record["attempt"] or (record["result"] not in (0, 1000001003) and record["image_index"] != 0xffffffff):
                            raise ValueError("invalid lower acquire result")
                        observations["acquire_driver"].append(record)
                    else:
                        if record["generated"] not in (0, 1) or record["submitted"] not in (0, 1):
                            raise ValueError("invalid copy boolean")
                        for key in ("swapchain", "device", "queue", "command", "timeline_wait", "timeline_signal", "fence"):
                            record[key] = fields[key]
                        for name in ("wait", "signal"):
                            if record[name+"_recorded"] != min(record[name+"_count"], 16):
                                raise ValueError("invalid bounded semaphore list")
                            record[name+"s"] = [fields[f"{name}_{i}"] for i in range(record[name+"_recorded"])]
                        observations["copies"].append(record)
                elif operation == "pacing-profile-capture-gap":
                    count = int(fields["lost_records"])
                    if count < 0: raise ValueError("invalid capture gap")
                    observations["capture_gaps"].append({"lost_records": count})
                elif operation == "pacing-profile-session":
                    observations["sessions"].append({key: fields[key] for key in ("pid", "build", "fingerprint") if key in fields} |
                        {key: fields[key] for key in ("renderer_build_id", "frame_trace") if key in fields})
                elif operation == "pacing-profile-wayland":
                    keys = ("version", "pid", "surface_token", "feedback", "context", "source", "image_index", "generated",
                            "requested_ns", "received_ns", "finished_ns", "presentation_now_ns", "calibration_span_ns",
                            "clock_id", "presented_ns", "sequence", "refresh_ns", "flags", "output_id", "reason", "result")
                    version = int(fields["version"])
                    event = fields["event"]
                    required = {
                        "request": ("context", "source", "image_index", "generated", "requested_ns", "clock_id", "presentation_now_ns", "calibration_span_ns"),
                        "unavailable": ("context", "source", "image_index", "generated", "requested_ns", "clock_id", "reason"),
                        "call": ("started_ns", "finished_ns", "result"),
                        "presented": ("received_ns", "clock_id", "presented_ns", "sequence", "refresh_ns", "flags", "output_id"),
                        "discarded": ("received_ns",), "abandoned": ("received_ns",),
                    }
                    if event not in required or version not in (1, 2):
                        raise ValueError("unknown Wayland event/version")
                    if version == 2:
                        for key in ("pid", "surface_token", "feedback", *required[event]):
                            int(fields[key])
                    record = {key: int(fields[key]) if version == 1 or key in fields else
                              (0xffffffff if key == "clock_id" else 0) for key in keys}
                    if version == 2 and event == "call":
                        record["requested_ns"] = int(fields["started_ns"])
                    if (any(record[key] < 0 for key in keys if key != "result")
                            or record["generated"] not in (0, 1) or record["clock_id"] > 0xffffffff):
                        raise ValueError("invalid Wayland record")
                    record["event"] = event
                    if record["event"] == "call" and record["finished_ns"] < record["requested_ns"]:
                        raise ValueError("backwards Wayland call")
                    observations["wayland"].append(record)
                elif operation == "pacing-profile-wayland-gap":
                    record = {key: int(fields[key]) for key in ("version", "pid", "surface_token", "lost_records")}
                    if record["version"] != 1 or min(record.values()) < 0:
                        raise ValueError("invalid Wayland gap")
                    observations["wayland_gaps"].append(record)
                elif operation == "pacing-profile-capture-limit":
                    record = {key: int(fields[key]) for key in ("version", "pid", "limit_bytes", "captured_bytes", "rejected_bytes")}
                    if record["version"] != 1 or min(record.values()) < 0 or record["captured_bytes"] > record["limit_bytes"]:
                        raise ValueError("invalid capture limit")
                    observations["limits"].append(record)
                elif operation == "pacing-profile-log-wrap":
                    observations["wraps"].append({})
                elif operation == "pacing-profile-output":
                    keys = ("version", "pid", "context", "ordinal", "source", "generated", "deferred",
                            "image_index", "scheduled_ns", "started_ns", "finished_ns", "result", "copy_checked", "copy_status", "copy_sampled_ns")
                    record = {key: int(fields[key]) for key in keys}
                    if (record["version"] not in (1, 2) or record["finished_ns"] < record["started_ns"] or
                            any(record[key] < 0 for key in keys if key not in ("result", "copy_status")) or
                            (record["copy_checked"] and record["copy_sampled_ns"] < record["finished_ns"]) or
                            any(record[key] not in (0, 1) for key in ("generated", "deferred", "copy_checked"))):
                        raise ValueError("invalid output trace")
                    if record["version"] == 2:
                        extra = ("enqueued_ns", "queue_lock_started_ns", "queue_lock_acquired_ns", "ready_checked", "ready_started_ns", "ready_finished_ns", "ready_deadline_ns", "ready_result")
                        record.update({key: int(fields[key]) for key in extra})
                        if (any(record[key] < 0 for key in extra if key != "ready_result") or record["ready_checked"] not in (0, 1)
                                or record["queue_lock_acquired_ns"] < record["queue_lock_started_ns"]
                                or record["ready_finished_ns"] < record["ready_started_ns"]):
                            raise ValueError("invalid worker trace")
                        for key in ("queue", "present_wait"):
                            if key in fields: record[key] = fields[key]
                    observations["outputs"].append(record)
                elif operation == "application-present-wait":
                    record = {key: int(fields[key]) for key in
                              ("pid", "sample_end_monotonic_ms", "calls", "successful", "timeouts", "errors", "polls")}
                    record.update({key: float(fields[key]) for key in ("duration_mean_ms", "duration_max_ms")})
                    if not all(math.isfinite(value) and value >= 0 for value in record.values()):
                        raise ValueError("invalid wait window")
                    if fields["api"] not in ("KHR", "KHR2"):
                        raise ValueError("unknown wait API")
                    record.update(swapchain=fields["swapchain"], api=fields["api"])
                    observations["waits"].append(record)
                elif operation == "application-queue-present":
                    record = {key: int(fields[key]) for key in ("pid", "sample_end_monotonic_ms", "calls")}
                    record.update({key: float(fields[key]) for key in ("duration_mean_ms", "duration_max_ms")})
                    for key in ("deferred_join_samples", "deferred_join_mean_ms", "deferred_join_max_ms"):
                        if key in fields:
                            record[key] = float(fields[key])
                    if not all(math.isfinite(value) and value >= 0 for value in record.values()):
                        raise ValueError("invalid application present window")
                    record["swapchain"] = fields["swapchain"]
                    observations["presents"].append(record)
            except (KeyError, ValueError, OverflowError):
                invalid += 1
    requests = defaultdict(list)
    for record in observations["wayland"]:
        if record["event"] == "request":
            requests[record["pid"], record["surface_token"], record["feedback"]].append(record)
    for record in observations["wayland"]:
        if record["version"] != 2 or record["event"] in ("request", "unavailable"):
            continue
        matched = requests[record["pid"], record["surface_token"], record["feedback"]]
        if len(matched) == 1:
            for key in ("context", "source", "image_index", "generated", "presentation_now_ns", "calibration_span_ns"):
                record[key] = matched[0][key]
            if record["clock_id"] == 0xffffffff:
                record["clock_id"] = matched[0]["clock_id"]
    observations["invalid_records"] = invalid
    return observations, invalid


def acquisition_links(observations: dict) -> dict:
    """Consume image acquisitions once, within the exact swapchain lifetime.

    Never recover an identity through a truncated/omitted stream, duplicate
    call, failed acquire or overlapping reuse. Old captures lack lifetime IDs.
    """
    if observations.get("capture_gaps") or observations.get("wraps") or observations.get("invalid_records"):
        return {}
    events = []
    identities = defaultdict(int)
    for call in observations.get("application_calls", []):
        identities[call["pid"], call["stream"], call["ordinal"]] += 1
    for call in observations.get("application_calls", []):
        if not call["context"] or call["image_index"] == 0xffffffff:
            continue
        identity = call["pid"], call["stream"], call["ordinal"]
        is_present = call["api"] == "present"
        events.append((call["started_ns"] if is_present else call["finished_ns"], is_present, identity, call))
    pending, links = {}, {}
    for _, is_present, identity, call in sorted(events, key=lambda e: (e[0], e[1], e[2])):
        key = call["pid"], call["context"], call["swapchain"], call["image_index"]
        if identities[identity] != 1:
            pending[key] = None
        elif not is_present:
            if call["result_known"] and call["result"] in (0, 1000001003):
                pending[key] = call if key not in pending else None
        else:
            acquire = pending.pop(key, None)
            if acquire and acquire["finished_ns"] <= call["started_ns"]:
                links[identity] = acquire
    return links


def native_copy_clock(sample: dict, submission: dict) -> dict:
    """Raw GPU evidence plus a conservative, explicitly estimated clock join.

    Keep independent before/after calibrations, their error and disagreement.
    Narrow timestamp counters and clock/epoch inconsistencies stay unmapped.
    This does not measure the later semaphore signal or physical scanout.
    """
    result = {"sample": sample, "host_estimate": None}
    if not sample["available"]:
        result["unavailable"] = "query-result-unavailable"
        return result
    mask = (1 << sample["valid_bits"]) - 1
    elapsed = ((sample["end_ticks"] - sample["begin_ticks"]) & mask) * sample["period_ns"]
    result["duration_ns"] = elapsed
    if sample["valid_bits"] != 64:
        result["unavailable"] = "narrow-device-clock"
        return result
    estimates = []
    for name in ("before", "after"):
        start, end, host, deviation = (sample[name+"_"+suffix] for suffix in ("started_ns", "finished_ns", "host_ns", "deviation_ns"))
        if sample[name+"_result"] != 0 or not deviation or not start - deviation <= host <= end + deviation:
            result["unavailable"] = "calibration-unavailable-or-wrong-host-clock"
            return result
        mapped = []
        for field in ("begin_ticks", "end_ticks"):
            delta = (sample[field] - sample[name+"_device_ticks"] + (1 << 63)) % (1 << 64) - (1 << 63)
            mapped.append(host + delta * sample["period_ns"])
        estimates.append((mapped, deviation))
    begin_bounds = (min(v[0][0]-v[1] for v in estimates), max(v[0][0]+v[1] for v in estimates))
    end_bounds = (min(v[0][1]-v[1] for v in estimates), max(v[0][1]+v[1] for v in estimates))
    # Require genuinely bracketing calibrations and a bounded captured batch.
    # Do not extrapolate a stale calibration across suspension or a clock reset.
    if (sample["before_finished_ns"] > submission["started_ns"] or sample["after_started_ns"] < sample["read_finished_ns"]
            or sample["after_finished_ns"] - sample["before_started_ns"] > 1_000_000_000
            or end_bounds[1] < begin_bounds[0] or end_bounds[0] > sample["collected_ns"]
            or end_bounds[1] < submission["started_ns"] or begin_bounds[1] < submission["started_ns"]
            or begin_bounds[0] > sample["collected_ns"] or elapsed > 1_000_000_000):
        result["unavailable"] = "calibration-does-not-bracket-copy"
        return result
    result["host_estimate"] = {"begin_low_ns": begin_bounds[0], "begin_high_ns": begin_bounds[1],
                               "end_low_ns": end_bounds[0], "end_high_ns": end_bounds[1]}
    return result


def frame_timeline(observations: dict) -> dict:
    """Join exact identities only; preserve missing/ambiguous evidence in each row."""
    grouped = defaultdict(list)
    for source in observations.get("sources", []):
        grouped[source["pid"], source["context"], source["source"]].append(source)
    applications, gpu, outputs, feedback = (defaultdict(list) for _ in range(4))
    for call in observations.get("application_calls", []):
        if call["api"] == "present" and call["context"] and call["source"]:
            applications[call["pid"], call["context"], call["source"]].append(call)
    for sample in observations.get("gpu_frames", []):
        gpu[sample["pid"], sample["context"], sample["source"]].append(sample)
    for sample in observations.get("outputs", []):
        outputs[sample["pid"], sample["context"], sample["source"]].append(sample)
    for sample in observations.get("wayland", []):
        if sample["context"] and sample["source"]:
            feedback[sample["pid"], sample["context"], sample["source"]].append(sample)
    copies, driver, copy_gpu = defaultdict(list), defaultdict(list), defaultdict(list)
    for sample in observations.get("copies", []):
        copies[sample["pid"], sample["context"], sample["source"]].append(sample)
    for sample in observations.get("acquire_driver", []):
        driver[sample["pid"], sample["call_id"]].append(sample)
    for sample in observations.get("copy_gpu", []):
        copy_gpu[sample["pid"], sample["context"], sample["source"]].append(sample)
    acquire_links = acquisition_links(observations)
    rows = []
    previous = {}
    for key, matches in sorted(grouped.items()):
        if len(matches) != 1:
            continue  # A duplicate source identity cannot be silently selected.
        source = matches[0]
        row = {"source": source, "application": None, "gpu": None,
               "outputs": sorted(outputs[key], key=lambda r: r["ordinal"]), "wayland": feedback[key], "missing": [],
               "acquisition": None, "acquire_driver": [], "copy_gpu": [],
               "copies": sorted((r for r in copies[key] if r["swapchain"] == source["swapchain"] and source["started_ns"] <= r["started_ns"] <= r["finished_ns"] <= source["finished_ns"]), key=lambda r: r["started_ns"])}
        for sample in copy_gpu[key]:
            matches = [c for c in row["copies"] if c["submitted"] and sample["generation"] == source["backend_epoch"] and
                       all(c[field] == sample[field] for field in ("swapchain", "device", "queue", "command", "generated", "image_index")) and
                       sample["slot"] == (c["pass"] + 1 if c["generated"] else 0)]
            duplicates = [s for s in copy_gpu[key] if s["slot"] == sample["slot"]]
            if len(matches) != 1 or len(duplicates) != 1:
                continue
            joined = native_copy_clock(sample, matches[0])
            presented = [o for o in row["outputs"] if o["generated"] == sample["generated"] and o["image_index"] == sample["image_index"]]
            if len(presented) == 1 and joined["host_estimate"]:
                output = presented[0]
                bounds = joined["host_estimate"]
                # Output scopes can include queue locking and observer setup.
                # Prefer the existing exact Wayland lower-call boundary; in
                # its absence retain the entire scope as entry uncertainty.
                calls = [c for c in row["wayland"] if c["event"] == "call" and
                         c["generated"] == sample["generated"] and c["image_index"] == sample["image_index"] and
                         output["started_ns"] <= c["requested_ns"] <= c["finished_ns"] <= output["finished_ns"]]
                entry_low, entry_high = output["started_ns"], output["finished_ns"]
                joined["lower_entry_boundary"] = "output-scope-bounds"
                if len(calls) == 1:
                    entry_low = entry_high = calls[0]["requested_ns"]
                    joined["lower_entry_boundary"] = "wayland-lower-call"
                joined["lower_entry_low_ns"], joined["lower_entry_high_ns"] = entry_low, entry_high
                joined["copy_end_before_entry_estimate"] = ("before" if bounds["end_high_ns"] < entry_low else
                    "after" if bounds["end_low_ns"] > entry_high else "ambiguous")
                joined["copy_end_to_entry_low_ns"] = entry_low - bounds["end_high_ns"]
                joined["copy_end_to_entry_high_ns"] = entry_high - bounds["end_low_ns"]
            row["copy_gpu"].append(joined)
        app = applications[key]
        if len(app) == 1 and app[0]["swapchain"] == source["swapchain"] and app[0]["started_ns"] <= source["started_ns"] <= source["finished_ns"] <= app[0]["finished_ns"]:
            row["application"] = app[0]
            row["acquisition"] = acquire_links.get((app[0]["pid"], app[0]["stream"], app[0]["ordinal"]))
            acquire = row["acquisition"]
            if acquire:
                row["acquire_to_present_ns"] = app[0]["started_ns"] - acquire["finished_ns"]
                attempts = sorted(driver[key[0], acquire.get("call_id", 0)], key=lambda r: r["attempt"])
                if ([r["attempt"] for r in attempts] == list(range(1, acquire.get("lower_calls", 0) + 1))
                        and all(acquire["started_ns"] <= r["started_ns"] <= r["finished_ns"] <= acquire["finished_ns"] for r in attempts)
                        and all(r["tid"] == acquire["tid"] for r in attempts)
                        and all(a["finished_ns"] <= b["started_ns"] for a, b in zip(attempts, attempts[1:]))
                        and sum(r["finished_ns"] - r["started_ns"] for r in attempts) == acquire.get("lower_ns")):
                    row["acquire_driver"] = attempts
        else:
            row["missing"].append("application-boundary-missing-or-ambiguous")
        samples = [r for r in gpu[key] if source["backend_scheduled"] and r["backend_epoch"] == source["backend_epoch"] and r["backend_frame"] == source["backend_frame"]]
        if len(samples) == 1:
            row["gpu"] = samples[0]
        elif source["backend_scheduled"]:
            row["missing"].append("completed-gpu-sample-missing-or-ambiguous")
        older = previous.get(key[:2])
        if older and older["source"]["source"] + 1 == source["source"] and source["started_ns"] >= older["source"]["finished_ns"]:
            row["source_gap_ns"] = source["started_ns"] - older["source"]["started_ns"]
            row["outside_inner_ns"] = source["started_ns"] - older["source"]["finished_ns"]
            a, b = older["application"], row["application"]
            if a and b and b["started_ns"] >= a["finished_ns"]:
                row["application_gap_ns"] = b["started_ns"] - a["started_ns"]
                row["outside_application_ns"] = b["started_ns"] - a["finished_ns"]
        previous[key[:2]] = row
        rows.append(row)
    acquired_ids = {id(r["acquisition"]) for r in rows if r["acquisition"]}
    application_ids = {id(r["application"]) for r in rows if r["application"]}
    driver_ids = {id(c) for r in rows for c in r["acquire_driver"]}
    copy_ids = {id(c) for r in rows for c in r["copies"]}
    copy_gpu_ids = {id(c["sample"]) for r in rows for c in r["copy_gpu"]}
    return {"schema": 1, "sessions": observations.get("sessions", []), "frames": rows,
            "unmatched_acquisitions": [c for c in observations.get("application_calls", []) if c["api"] != "present" and id(c) not in acquired_ids],
            "unmatched_application_presents": [c for c in observations.get("application_calls", []) if c["api"] == "present" and id(c) not in application_ids],
            "unmatched_acquire_driver": [c for c in observations.get("acquire_driver", []) if id(c) not in driver_ids],
            "unmatched_copies": [c for c in observations.get("copies", []) if id(c) not in copy_ids],
            "unmatched_copy_gpu": [c for c in observations.get("copy_gpu", []) if id(c) not in copy_gpu_ids],
            "invalid_records": observations.get("invalid_records", 0),
            "duplicate_source_identities": sum(len(values) != 1 for values in grouped.values()),
            "capture_gaps": observations.get("capture_gaps", []), "capture_limits": observations.get("limits", []),
            "capture_wraps": len(observations.get("wraps", [])),
            "unmeasured": ["game simulation/input timestamps", "source-render GPU completion in host clock", "WSI semaphore signal completion", "physical scanout pixels"] +
                ([] if any(c["host_estimate"] for r in rows for c in r["copy_gpu"]) else ["per-image GPU copy completion in host clock"])}


def frame_trace_report(observations: dict, pid: int, context: int, top: int) -> list[str]:
    timeline = frame_timeline(observations)
    rows = [r for r in timeline["frames"] if r["source"]["pid"] == pid and r["source"]["context"] == context]
    if not rows:
        return ["  Per-source causal trace unavailable in this capture; enable FGVK_PACING_PROFILE_TRACE=1."]
    lines = [f"  Per-source trace: {len(rows)} sources, {sum(r['application'] is not None for r in rows)} exact application joins, {sum(r['gpu'] is not None for r in rows)} exact completed GPU joins."]
    acquired = [r for r in rows if r["acquisition"]]
    if acquired:
        lines.append(f"  Image ownership: {len(acquired)} exact acquisition-to-present joins; driver-call attempts are separate from framegen-vulkan/observer time.")
        for row in sorted(acquired, key=lambda r: r["acquisition"]["finished_ns"] - r["acquisition"]["started_ns"], reverse=True)[:top]:
            acquire = row["acquisition"]
            total = acquire["finished_ns"] - acquire["started_ns"]
            lower = acquire.get("lower_ns") if len(row["acquire_driver"]) == acquire.get("lower_calls") else None
            lines.append(f"    Source {row['source']['source']}: acquire {total/1e6:.3f} ms; " +
                         (f"driver {lower/1e6:.3f} ms, framegen-vulkan/observer {(total-lower)/1e6:.3f} ms; " if lower is not None else "driver partition unavailable; ") +
                         f"acquire return -> outer present {row['acquire_to_present_ns']/1e6:.3f} ms.")
    else:
        lines.append("  Acquisition-to-present ownership unavailable (requires recorded matching image/context lifetimes).")
    copies = [c for r in rows for c in r["copies"]]
    if copies:
        lines.append(f"  Native FG copy submissions: source={sum(not c['generated'] for c in copies)}, generated={sum(c['generated'] for c in copies)}, submission exceptions={sum(not c['submitted'] for c in copies)}; queue/semaphore dependencies retained in JSON; GPU completion requires separate native timestamp records.")
    native_gpu = [c for r in rows for c in r["copy_gpu"]]
    if native_gpu:
        lines.append(f"  Native copy GPU timestamps: {len(native_gpu)} exact joins, {sum(bool(c['host_estimate']) for c in native_gpu)} calibrated host estimates; {len(copies)-len(native_gpu)} copy submissions lack a unique completed sample. Raw calibrations and uncertainty retained.")
        for generated in (True, False):
            role = [c for c in native_gpu if bool(c["sample"]["generated"]) == generated]
            lines.append(f"    {'Generated' if generated else 'Real/source'} copy end relative to lower entry (calibrated estimates): " +
                ", ".join(f"{label}={sum(c.get('copy_end_before_entry_estimate') == label for c in role)}" for label in ("before", "after", "ambiguous")) +
                f", unavailable={sum('copy_end_before_entry_estimate' not in c for c in role)}.")
        for copy in sorted((c for c in native_gpu if "copy_end_to_entry_low_ns" in c), key=lambda c: c["copy_end_to_entry_low_ns"])[:top]:
            lines.append(f"    Source {copy['sample']['source']} {'generated' if copy['sample']['generated'] else 'real/source'}: estimated copy-end -> lower-entry interval " +
                         f"[{copy['copy_end_to_entry_low_ns']/1e6:.3f}, {copy['copy_end_to_entry_high_ns']/1e6:.3f}] ms; " +
                         f"GPU copy span {copy['duration_ns']/1e6:.3f} ms.")
        lines.append("  Calibrated copy-end estimates do not establish the later WSI semaphore signal, game simulation time or physical scanout.")
        batches = {(c["sample"]["source"], c["sample"]["generation"]): c["sample"] for c in native_gpu}
        costs = sorted((s["read_finished_ns"]-s["read_started_ns"] +
                        s["before_finished_ns"]-s["before_started_ns"] +
                        s["after_finished_ns"]-s["after_started_ns"])/1000 for s in batches.values())
        lines.append("  Native timestamp read/calibration CPU cost p50/p99/max (us): " +
                     "/".join(f"{costs[round((len(costs)-1)*p)]:.3f}" for p in (.5,.99,1)) +
                     "; excludes command recording/publication and gameplay overhead.")
    elif copies:
        lines.append("  Native copy GPU timestamps unavailable in this capture; submission records alone cannot establish copy completion at lower entry.")
    if timeline["duplicate_source_identities"]:
        lines.append(f"  Ambiguous source identities excluded: {timeline['duplicate_source_identities']}.")
    if timeline["capture_gaps"]:
        lines.append("  Capture has omitted records; missing boundaries are unavailable evidence.")
    for row in sorted((r for r in rows if "source_gap_ns" in r), key=lambda r: r["source_gap_ns"], reverse=True)[:top]:
        source, app, gpu = row["source"], row["application"], row["gpu"]
        lines.append(f"    Source {source['source']}: inner entry gap {row['source_gap_ns']/1e6:.3f} ms; " +
                     (f"outer entry gap {row['application_gap_ns']/1e6:.3f} ms, outside outer {row['outside_application_ns']/1e6:.3f} ms; " if "application_gap_ns" in row else "outer interval unavailable; ") +
                     (f"current pre-config join {app['join_ns']/1e6:.3f} ms; " if app and app["join_known"] else "current join unavailable; ") +
                     f"current waits cap/ready/output {source['cap_wait_ns']/1e6:.3f}/{source['readiness_wait_ns']/1e6:.3f}/{source['output_wait_ns']/1e6:.3f} ms; " +
                     (f"GPU span {gpu['span_us']/1000:.3f} ms." if gpu else "GPU sample unavailable."))
    worker = [o for r in rows for o in r["outputs"] if o.get("ready_checked")]
    if worker:
        lines.append(f"  Actual worker readiness waits: {len(worker)}, success={sum(o['ready_result'] == 0 for o in worker)}, timeout={sum(o['ready_result'] == 2 for o in worker)}, errors={sum(o['ready_result'] < 0 for o in worker)}.")
        for o in sorted(worker, key=lambda o: o["ready_finished_ns"] - o["ready_started_ns"], reverse=True)[:top]:
            lines.append(f"    Source {o['source']}: readiness {(o['ready_finished_ns']-o['ready_started_ns'])/1e6:.3f} ms, result={o['ready_result']}; queue-lock wait {(o['queue_lock_acquired_ns']-o['queue_lock_started_ns'])/1e6:.3f} ms; actual lower entry {o['started_ns']} ns.")
    lines.append("  Backend GPU spans alone are elapsed durations; native copy timestamps require their separate calibrated clock joins. Game simulation and physical scanout remain unmeasured.")
    return lines


def output_report(samples: list[dict], top: int) -> list[str]:
    if not samples:
        return ["  Per-output trace unavailable; averages cannot identify individual output bursts."]
    # Never measure across a missing/duplicated record or a backwards clock.
    pairs = [(a, b) for a, b in zip(samples, samples[1:])
             if b["ordinal"] == a["ordinal"] + 1 and b["started_ns"] >= a["started_ns"]]
    gaps = sorted((b["started_ns"] - a["started_ns"]) / 1e6 for a, b in pairs)
    checked = [r for r in samples if r["copy_checked"]]
    lines = [f"  Per-output trace: {len(samples)} records, {len(samples) - 1 - len(pairs)} discontinuous pairs excluded."]
    if gaps:
        percentiles = [gaps[round((len(gaps) - 1) * p)] for p in (.5, .95, .99)]
        lines.append("  CPU output gaps min/p50/p95/p99/max (ms): " +
                     "/".join(f"{value:.3f}" for value in [gaps[0], *percentiles, gaps[-1]]))
    lines.append("  Final-copy fence sampled after lower-present return: " +
                 f"ready={sum(r['copy_status'] == 0 for r in checked)}, " +
                 f"pending={sum(r['copy_status'] == 1 for r in checked)}, " +
                 f"errors={sum(r['copy_status'] not in (0, 1) for r in checked)}, " +
                 f"unchecked={len(samples) - len(checked)} (not per-image completion or display timing).")
    if checked:
        delays = [(r["copy_sampled_ns"] - r["finished_ns"]) / 1000 for r in checked]
        lines.append(f"  Copy status observation after return: mean/max {sum(delays) / len(delays):.3f}/{max(delays):.3f} us.")
    for a, b in sorted(pairs, key=lambda pair: pair[1]["started_ns"] - pair[0]["started_ns"], reverse=True)[:top]:
        gap = (b["started_ns"] - a["started_ns"]) / 1e6
        before = "generated" if a["generated"] else "real"
        after = "generated" if b["generated"] else "real"
        lateness = f"{(b['started_ns'] - b['scheduled_ns']) / 1e6:.3f} ms" if b["scheduled_ns"] else "unavailable"
        lines.append(f"    Gap {gap:.3f} ms: source {a['source']} {before} image {a['image_index']} -> " +
                     f"source {b['source']} {after} image {b['image_index']}; " +
                     f"previous lower call {(a['finished_ns'] - a['started_ns']) / 1e6:.3f} ms; " +
                     f"next CPU deadline lateness {lateness}.")
    return lines


def wayland_report(records: list[dict], pid: int, context: int, start_ns: int, end_ns: int, top: int) -> list[str]:
    requests = [r for r in records if r["pid"] == pid and r["context"] == context
                and r["event"] in ("request", "unavailable") and start_ns <= r["requested_ns"] <= end_ns]
    if not requests:
        return ["  Wayland presentation feedback unavailable in this interval (not evidence of zero discards)."]
    lines = []
    admitted = [r for r in requests if r["event"] == "request"]
    unavailable = defaultdict(int)
    for r in requests:
        if r["event"] == "unavailable":
            unavailable[r["reason"]] += 1
    if unavailable:
        names = {1: "discovery pending", 2: "protocol unavailable", 3: "clock pending", 4: "capacity reached", 5: "proxy failure"}
        lines.append("  Wayland observation gaps: " + ", ".join(f"{names.get(k, f'reason {k}')}={v}" for k, v in sorted(unavailable.items())))
    def key(r):
        return r["surface_token"], r["feedback"]
    terminals = defaultdict(list)
    calls = defaultdict(list)
    for r in records:
        if r["pid"] != pid:
            continue
        if r["event"] in ("presented", "discarded", "abandoned"):
            terminals[key(r)].append(r)
        elif r["event"] == "call":
            calls[key(r)].append(r)
    outcomes = [terminals[key(r)][0] for r in admitted if len(terminals[key(r)]) == 1]
    ambiguous = sum(len(terminals[key(r)]) > 1 for r in admitted)
    presented = [r for r in outcomes if r["event"] == "presented"]
    lines.append(f"  Wayland feedback: requests={len(admitted)}, presented={len(presented)}, "
                 f"discarded={sum(r['event'] == 'discarded' for r in outcomes)}, "
                 f"abandoned={sum(r['event'] == 'abandoned' for r in outcomes)}, "
                 f"pending={sum(not terminals[key(r)] for r in admitted)}, ambiguous={ambiguous}.")
    for generated in (0, 1):
        chosen = [r for r in outcomes if r["generated"] == generated]
        lines.append(f"    {'Generated' if generated else 'Real'} outcomes: " + ", ".join(
            f"{name}={sum(r['event'] == name for r in chosen)}" for name in ("presented", "discarded", "abandoned")))
    failed = sum(len(calls[key(r)]) == 1 and calls[key(r)][0]["result"] < 0 for r in admitted)
    lines.append(f"  Associated lower-present failures: {failed}; missing/duplicate call records: "
                 f"{sum(len(calls[key(r)]) != 1 for r in admitted)}.")
    groups = defaultdict(list)
    for r in presented:
        # Never subtract timestamps from different clocks, surfaces or outputs.
        groups[r["surface_token"], r["clock_id"], r["output_id"]].append(r)
    for (surface, clock, output), group in groups.items():
        group.sort(key=lambda r: r["feedback"])
        valid = [r for r in group if r["presented_ns"] > 0]
        same = defaultdict(list)
        for r in valid:
            same[r["presented_ns"], r["sequence"]].append(r)
        duplicates = sum(len(items) - 1 for items in same.values())
        inversions = sum(b["presented_ns"] < a["presented_ns"] for a, b in zip(valid, valid[1:]))
        flags = defaultdict(int)
        for r in group:
            flags[r["flags"]] += 1
        lines.append(f"  Compositor presentation clock={clock}, surface={surface}, output proxy={output}: "
                     f"{len(valid)} valid timestamps, duplicate timestamp/sequence observations={duplicates}, "
                     f"request-order timestamp inversions={inversions}, flags=" + ",".join(f"{k:#x}:{v}" for k, v in sorted(flags.items())) + ".")
        if duplicates:
            lines.append("    Multiple feedback requests may belong to the same surface commit; those outputs cannot be assigned distinct visible frames.")
        unique = sorted((items[0] for items in same.values()), key=lambda r: r["presented_ns"])
        pairs = [(a, b) for a, b in zip(unique, unique[1:]) if b["presented_ns"] > a["presented_ns"]]
        gaps = sorted((b["presented_ns"] - a["presented_ns"]) / 1e6 for a, b in pairs)
        if gaps:
            values = [gaps[0], *[gaps[round((len(gaps) - 1) * p)] for p in (.5, .95, .99)], gaps[-1]]
            lines.append("    Distinct compositor presentation gaps min/p50/p95/p99/max (ms): " + "/".join(f"{v:.3f}" for v in values))
        by_role = defaultdict(list)
        for a, b in pairs:
            if len(same[a["presented_ns"], a["sequence"]]) == len(same[b["presented_ns"], b["sequence"]]) == 1:
                by_role[a["generated"], b["generated"]].append((b["presented_ns"] - a["presented_ns"]) / 1e6)
        for roles, values in sorted(by_role.items()):
            values.sort()
            names = ["generated" if role else "real" for role in roles]
            lines.append(f"    Unambiguous {' -> '.join(names)} compositor gaps p50/p99/max (ms): " +
                         "/".join(f"{v:.3f}" for v in (values[(len(values)-1)//2], values[round((len(values)-1)*.99)], values[-1])) +
                         f"; pairs={len(values)}.")
        for a, b in sorted(pairs, key=lambda pair: pair[1]["presented_ns"] - pair[0]["presented_ns"], reverse=True)[:top]:
            role = lambda r: "generated" if r["generated"] else "real"
            lines.append(f"    Compositor gap {(b['presented_ns'] - a['presented_ns']) / 1e6:.3f} ms: "
                         f"source {a['source']} {role(a)} -> source {b['source']} {role(b)}; feedback {a['feedback']}->{b['feedback']}.")
    lines.append("  Wayland feedback describes the compositor's reported commit outcome; it does not inspect pixels or measure input latency. Requests precede lower calls; commit association must be checked.")
    return lines


def presentation_window_report(timeline: dict, intervals: list[tuple[float, float]], top: int) -> str:
    """Compare exact output/call/feedback joins without treating requests as commits."""
    groups = defaultdict(list)
    events = defaultdict(lambda: defaultdict(list))
    outputs = defaultdict(list)
    copies = defaultdict(list)
    for row in timeline["frames"]:
        source = row["source"]
        owner = source["pid"], source["context"], source["source"]
        for output in row["outputs"]:
            outputs[owner].append(output)
        for sample in row["copy_gpu"]:
            copies[owner].append(sample)
        for event in row["wayland"]:
            events[event["pid"], event["surface_token"], event["feedback"]][event["event"]].append(event)
    for identity, observed in events.items():
        if len(observed["request"]) != 1 or len(observed["presented"]) != 1:
            continue
        request, presented = observed["request"][0], observed["presented"][0]
        if not presented["presented_ns"] or observed["discarded"] or observed["abandoned"]:
            continue
        group = (identity[0], request["context"], identity[1], presented["clock_id"], presented["output_id"])
        owner = request["pid"], request["context"], request["source"]
        calls = observed["call"]
        call = calls[0] if len(calls) == 1 and calls[0]["result"] in (0, 1000001003) else None
        matches = [o for o in outputs[owner] if call and o["result"] in (0, 1000001003) and
                   all(o[key] == request[key] for key in ("generated", "image_index")) and
                   o["started_ns"] <= call["requested_ns"] <= call["finished_ns"] <= o["finished_ns"]]
        output = matches[0] if len(matches) == 1 else None
        estimates = [c for c in copies[owner] if output and c.get("lower_entry_boundary") == "wayland-lower-call" and
                     c.get("lower_entry_low_ns") == call["requested_ns"] and
                     all(c["sample"][key] == request[key] for key in ("generated", "image_index"))]
        groups[group].append(dict(request=request, presented=presented, call=call, output=output,
                                  copy=estimates[0] if len(estimates) == 1 else None))
    lines = ["Presentation window comparison (offsets from the first presented feedback in each process/surface/clock/output; contexts remain separate)."]
    if not groups:
        return "\n".join(lines + ["  No uniquely requested/presented feedback is available."])
    if timeline.get("capture_gaps") or timeline.get("capture_wraps") or timeline.get("invalid_records"):
        lines.append("  Capture has omitted, wrapped or malformed records; comparisons retain only locally consecutive identities.")
    origins = {}
    for (pid, context, surface, clock, output_id), samples in groups.items():
        key = pid, surface, clock, output_id
        first = min(s["presented"]["presented_ns"] for s in samples)
        origins[key] = min(origins.get(key, first), first)
    def distribution(values):
        values = sorted(values)
        return "/".join(f"{values[round((len(values)-1)*p)] / 1e6:.3f}" for p in (0, .5, .95, .99, 1)) if values else "unavailable"
    for (pid, context, surface, clock, output_id), samples in sorted(groups.items()):
        samples.sort(key=lambda s: s["request"]["feedback"])
        origin = origins[pid, surface, clock, output_id]
        if not any(start * 1e9 <= s["presented"]["presented_ns"] - origin < end * 1e9
                   for s in samples for start, end in intervals):
            continue
        timestamps = defaultdict(int)
        ordinals = defaultdict(int)
        for sample in samples:
            timestamps[sample["presented"]["presented_ns"]] += 1
            if sample["output"]:
                ordinals[sample["output"]["ordinal"]] += 1
        lines.append(f"PID {pid}, context {context}, surface {surface}, clock {clock}, output proxy {output_id}; origin={origin} ns")
        for start, end in intervals:
            selected = [s for s in samples if start * 1e9 <= s["presented"]["presented_ns"] - origin < end * 1e9]
            paired = []
            for a, b in zip(selected, selected[1:]):
                if (not a["output"] or not b["output"] or b["request"]["feedback"] != a["request"]["feedback"] + 1 or
                        b["output"]["ordinal"] != a["output"]["ordinal"] + 1 or
                        any(timestamps[s["presented"]["presented_ns"]] != 1 or ordinals[s["output"]["ordinal"]] != 1 for s in (a, b)) or
                        b["presented"]["presented_ns"] <= a["presented"]["presented_ns"] or
                        b["call"]["requested_ns"] < a["call"]["finished_ns"]):
                    continue
                paired.append((a, b))
            lines.append(f"  +{start:g}–{end:g} s: {len(selected)} presented observations, {sum(s['output'] is not None for s in selected)} exact lower-output joins, "
                         f"{len(paired)} consecutive pairs; {max(0, len(selected)-1)-len(paired)} pairs excluded.")
            for label, field, key in (("Lower-call entry", "call", "requested_ns"), ("Lower-call return", "call", "finished_ns"),
                                      ("Compositor-reported presentation", "presented", "presented_ns")):
                lines.append(f"    {label} gaps min/p50/p95/p99/max (ms): " + distribution([b[field][key]-a[field][key] for a,b in paired]))
            for generated in (1, 0):
                role_pairs = [(a,b) for a,b in paired if b["request"]["generated"] == generated]
                lines.append(f"    Gaps ending at {'generated' if generated else 'real'} outputs ({len(role_pairs)} pairs), presentation min/p50/p95/p99/max (ms): " +
                             distribution([b["presented"]["presented_ns"]-a["presented"]["presented_ns"] for a,b in role_pairs]))
            counts = defaultdict(int)
            for s in selected:
                counts[s["copy"].get("copy_end_before_entry_estimate", "unavailable") if s["copy"] else "unavailable"] += 1
            lines.append("    Calibrated copy end vs exact lower entry: " + ", ".join(f"{name}={counts[name]}" for name in ("before", "after", "ambiguous", "unavailable")))
            steps = [b["output"]["scheduled_ns"]-a["output"]["scheduled_ns"] for a,b in paired
                     if a["output"]["scheduled_ns"] and b["output"]["scheduled_ns"]]
            lines.append("    Requested CPU deadline steps min/p50/p95/p99/max (ms): " + distribution(steps))
            refresh_steps = [b["output"]["scheduled_ns"]-a["output"]["scheduled_ns"] -
                             round((b["output"]["scheduled_ns"]-a["output"]["scheduled_ns"]) / a["presented"]["refresh_ns"]) * a["presented"]["refresh_ns"]
                             for a,b in paired if a["output"]["scheduled_ns"] and b["output"]["scheduled_ns"] and
                             a["presented"]["refresh_ns"] > 0 and a["presented"]["refresh_ns"] == b["presented"]["refresh_ns"]]
            if refresh_steps:
                lines.append(f"    Median CPU deadline step minus nearest reported refresh multiple: {sorted(refresh_steps)[len(refresh_steps)//2]} ns "
                             "(nominal drift only; no measured vblank phase or causal attribution).")
            for a,b in sorted(paired, key=lambda pair: abs((pair[1]["presented"]["presented_ns"]-pair[0]["presented"]["presented_ns"]) -
                                                         (pair[1]["call"]["requested_ns"]-pair[0]["call"]["requested_ns"])), reverse=True)[:top]:
                def describe(s):
                    lead = s["copy"].get("copy_end_to_entry_low_ns") if s["copy"] else None
                    lead_text = f"{lead/1e6:.3f}" if lead is not None else "unavailable"
                    return (f"feedback={s['request']['feedback']} source={s['request']['source']} "
                            f"{'generated' if s['request']['generated'] else 'real'} lower_entry_ns={s['call']['requested_ns']} "
                            f"copy_end_lead_ms={lead_text}")
                lines.append(f"    {describe(a)} -> {describe(b)}; lower gap={(b['call']['requested_ns']-a['call']['requested_ns'])/1e6:.3f} ms, "
                             f"presentation gap={(b['presented']['presented_ns']-a['presented']['presented_ns'])/1e6:.3f} ms.")
    lines.append("Feedback-to-output joins identify requests, not verified buffer commits. Copy-end markers precede WSI semaphore signals; physical pixels and game simulation remain unmeasured.")
    return "\n".join(lines)


def presentation_interval(value: str) -> tuple[float, float]:
    try:
        start, end = map(float, value.split(":"))
        if not math.isfinite(start) or not math.isfinite(end) or not 0 <= start < end:
            raise ValueError()
    except ValueError:
        raise argparse.ArgumentTypeError("presentation window must be START:END in finite seconds, with 0 <= START < END")
    return start, end


def report(windows: list[Window], top: int = 5, skip_seconds: float = 0,
           observations: dict[str, list[dict]] | None = None) -> str:
    groups = defaultdict(list)
    for window in windows:
        groups[window.pid, window.context].append(window)
    lines = ["CPU observations: source frames and output submissions; optional Wayland records report compositor presentation separately."]
    if (observations or {}).get("wraps"):
        lines.append("Capture has wrapped: earlier settings/FG portions may have been overwritten; this is not a complete session.")
    for (pid, context), group in groups.items():
        first = min(window.values.get("sample_end_monotonic_ms", 0) for window in group)
        group = [window for window in group
                 if window.values.get("sample_end_monotonic_ms", 0) >= first + skip_seconds * 1000]
        if not group:
            continue
        duration = sum(window.values["window_ms"] for window in group)
        source_rate = sum(window.values["sources"] for window in group) * 1000 / duration
        output_rate = sum(window.values["output_attempts"] for window in group) * 1000 / duration
        lines.append(f"PID {pid}, context {context}: {len(group)} windows, {duration / 1000:.2f} s")
        lines.append("  Recorded WSI: " + ", ".join(sorted({window.wsi for window in group})))
        for limit in (observations or {}).get("limits", []):
            if limit["pid"] == pid:
                lines.append(f"  Capture stopped at its {limit['limit_bytes']} byte limit, preserving the beginning; later activity is unavailable.")
        lines.append(f"  Source: {source_rate:.2f} frames/s; output: {output_rate:.2f} attempts/s")
        segments = []
        for window in group:
            key = tuple(window.values.get(name) for name in ("smooth_cadence", "effective_base_cap_fps", "present_mode",
                "frame_generation_enabled", "configured_multiplier", "configured_base_cap_fps", "adaptive"))
            if not segments or segments[-1][0] != key:
                segments.append((key, []))
            segments[-1][1].append(window)
        if len(segments) > 1:
            lines.append("  Settings segments (transition windows can contain both settings):")
            for (smooth, cap, mode, fg, multiplier, configured_cap, adaptive), segment in segments:
                seconds = sum(w.values["window_ms"] for w in segment) / 1000
                sources = sum(w.values["sources"] for w in segment)
                setting = lambda value: "unknown" if value is None else f"{value:g}"
                lines.append(f"    fg={setting(fg)} multiplier={setting(multiplier)} adaptive={setting(adaptive)} "
                             f"smooth={setting(smooth)} cap={setting(cap)} configured_cap={setting(configured_cap)} mode={setting(mode)}: {seconds:.2f} s, {sources / seconds:.2f} source FPS")
        selected_outputs = [r for r in (observations or {}).get("outputs", []) if r["pid"] == pid and r["context"] == context]
        if skip_seconds and group[0].values.get("sample_end_monotonic_ms"):
            cutoff = (first + skip_seconds * 1000 - group[0].values["window_ms"]) * 1e6
            selected_outputs = [r for r in selected_outputs if r["started_ns"] >= cutoff]
        lines.extend(output_report(selected_outputs, top))
        lines.extend(frame_trace_report(observations or {}, pid, context, top))
        trace_dropped = sum(w.values.get("output_trace_dropped", 0) for w in group)
        if trace_dropped:
            lines.append(f"  Output trace storage limit: {trace_dropped:g} records omitted; gaps across them are unavailable.")
        swapchains = {w.swapchain for w in group if w.swapchain}
        start_ms = min(w.values.get("sample_end_monotonic_ms", 0) - w.values["window_ms"] for w in group)
        end_ms = max(w.values.get("sample_end_monotonic_ms", 0) for w in group)
        wayland = (observations or {}).get("wayland", [])
        if wayland or any(w.values.get("wayland_requested") for w in group):
            for (smooth, cap, mode, fg, multiplier, configured_cap, adaptive), segment in segments:
                segment_start = min(w.values.get("sample_end_monotonic_ms", 0) - w.values["window_ms"] for w in segment)
                segment_end = max(w.values.get("sample_end_monotonic_ms", 0) for w in segment)
                if len(segments) > 1:
                    lines.append(f"  Wayland settings segment: fg={fg} multiplier={multiplier} adaptive={adaptive} smooth={smooth} "
                                 f"cap={cap} configured_cap={configured_cap} mode={mode} (transition windows may mix settings)")
                lines.extend(wayland_report(wayland, pid, context, int(segment_start * 1e6), int(segment_end * 1e6), top))
            omitted = sum(r["lost_records"] for r in (observations or {}).get("wayland_gaps", []) if r["pid"] == pid)
            if omitted:
                lines.append(f"  Wayland trace incomplete: {omitted} omitted records in this process; do not infer full coverage.")
        def boundary_records(kind):
            return [r for r in (observations or {}).get(kind, []) if r["pid"] == pid and r["swapchain"] in swapchains
                    and (not end_ms or start_ms <= r["sample_end_monotonic_ms"] <= end_ms)]
        presents = boundary_records("presents")
        if presents:
            calls = sum(r["calls"] for r in presents)
            mean = sum(r["calls"] * r["duration_mean_ms"] for r in presents) / max(1, calls)
            maximum = max(r["duration_max_ms"] for r in presents)
            lines.append(f"  Outer application present CPU duration mean/max: {mean:.3f}/{maximum:.3f} ms (includes pre-present draining).")
            joins = [r for r in presents if r.get("deferred_join_samples", 0) > 0]
            samples = sum(r["deferred_join_samples"] for r in joins)
            if samples:
                mean = sum(r["deferred_join_samples"] * r["deferred_join_mean_ms"] for r in joins) / samples
                maximum = max(r["deferred_join_max_ms"] for r in joins)
                lines.append(f"  Deferred output join before configuration/present mean/max: {mean:.3f}/{maximum:.3f} ms.")
            else:
                lines.append("  Pre-present deferred join timing unavailable; the inner layer timer excludes it.")
        waits = boundary_records("waits")
        if waits:
            for api in ("KHR", "KHR2"):
                selected = [r for r in waits if r["api"] == api]
                if not selected:
                    continue
                calls = sum(r["calls"] for r in selected)
                mean = sum(r["calls"] * r["duration_mean_ms"] for r in selected) / max(1, calls)
                maximum = max(r["duration_max_ms"] for r in selected)
                lines.append(f"  Actual {api} present waits: calls={calls}, " +
                             ", ".join(f"{key}={sum(r[key] for r in selected)}" for key in ("successful", "timeouts", "errors", "polls")) +
                             f", mean/max {mean:.3f}/{maximum:.3f} ms.")
        elif any(w.values.get("wait_capture") for w in group):
            lines.append("  Actual present waits: no completed wait windows recorded.")
        else:
            lines.append("  Actual present waits unavailable: this capture did not include lightweight wait observations.")
        if any("fifo_caller_timing_sources" in window.values for window in group):
            capabilities = sum(window.values.get("fifo_caller_timing_sources", 0) for window in group)
            lines.append(f"  FIFO source batches with caller timing/wait capabilities: {capabilities:g}")
        timing = [window.values for window in group if "timing_driver_attempts" in window.values]
        if timing:
            counts = {name: sum(value.get(f"timing_{name}_attempts", 0) for value in timing)
                      for name in ("driver", "application", "ext", "google", "bridge")}
            lines.append("  Lower-present timing attempts: " + ", ".join(
                f"{name}={count:g}" for name, count in counts.items()))
            targets = [value for value in timing if value.get("present_target_interval_samples", 0) > 0
                       and "present_target_interval_mean_ms" in value]
            samples = sum(value["present_target_interval_samples"] for value in targets)
            if samples:
                mean = sum(value["present_target_interval_mean_ms"] * value["present_target_interval_samples"]
                           for value in targets) / samples
                lines.append(f"  Requested framegen-vulkan Vulkan target interval: mean {mean:.3f} ms, samples {samples:g} (not display timing).")
            discontinuities = sum(value.get("present_target_discontinuities", 0) for value in timing)
            lines.append(f"  Repeated/backwards requested targets: {discontinuities:g}")
            if len(timing) != len(group):
                lines.append("  Timing-owner coverage is partial: older windows did not record it.")
        else:
            lines.append("  Output timing owner/targets unavailable in this capture; FIFO mode and CPU gaps do not identify them.")
        if any(window.values.get("gpu_requested", 0) for window in group):
            gpu_samples = sum(window.values.get("gpu_span_samples", 0) for window in group)
            if not gpu_samples:
                lines.append("  GPU timing unavailable: no completed backend batch samples (not zero GPU cost).")
            else:
                lines.append("  framegen-vulkan backend GPU spans: excludes game rendering, scaling/copies and scanout.")
                for label, metric in (("GPU prepass", "gpu_prepass"),
                                      ("GPU generated batch", "gpu_generation"),
                                      ("GPU per output", "gpu_output"),
                                      ("GPU whole batch", "gpu_span"),
                                      ("CPU query readback", "gpu_readback")):
                    observed = [window.values for window in group
                                if window.values.get(f"{metric}_samples", 0) > 0 and
                                f"{metric}_mean_ms" in window.values and f"{metric}_max_ms" in window.values]
                    samples = sum(value[f"{metric}_samples"] for value in observed)
                    if samples:
                        mean = sum(value[f"{metric}_mean_ms"] * value[f"{metric}_samples"]
                                   for value in observed) / samples
                        maximum = max(value[f"{metric}_max_ms"] for value in observed)
                        lines.append(f"    {label}: mean {mean:.3f} ms, maximum {maximum:.3f} ms, samples {samples:g}")
                lines.append("    GPU spans can include scheduling/preemption gaps; do not add whole-batch and component spans.")
        for label, key in (("Source interval", "source_interval_max_ms"),
                           ("Profiler publication", "profiler_publish_max_ms"),
                           ("Output wake lateness", "output_wake_lateness_max_ms"),
                           ("Source-cap wake lateness", "source_cap_wake_lateness_max_ms"),
                           ("Source deadline lateness", "source_deadline_lateness_max_ms")):
            available = [window.values[key] for window in group if key in window.values]
            if available:
                lines.append(f"  {label} maximum: {max(available):.3f} ms")
        detailed = [window for window in group if window.version == 2 and
                    window.values.get("worst_source_interval_ms", 0) > 0]
        if not detailed:
            lines.append("  Legacy capture: correlated hitch attribution is unavailable.")
        for window in sorted(detailed, key=lambda item: item.values["worst_source_interval_ms"], reverse=True)[:top]:
            v = window.values
            def metric(name):
                return v.get(f"worst_source_{name}_ms", 0)
            time = (v.get("sample_end_monotonic_ms", first) - first) / 1000
            lines.append(f"  Worst source interval in window ending +{time:.2f}s: {metric('interval'):.3f} ms")
            lines.append(f"    Previous framegen-vulkan present: {metric('layer'):.3f} ms; outside that present: {metric('outside'):.3f} ms")
            lines.append(f"    Cap wait: {metric('cap_wait'):.3f}; readiness wait: {metric('readiness_wait'):.3f}; output sleeps: {metric('output_wait'):.3f} ms")
            lines.append(f"    Profiler publication after that present: {metric('profiler_publish'):.3f} ms (included in outside time)")
            lines.append("    Private CPU phases (ms): " + ", ".join(
                f"{name}={metric(name):.3f}" for name in
                ("render_fence", "schedule", "source_copy", "acquire", "generated_submit", "generated_present", "original_present", "unattributed")))
    lines.append("Outside time can include game/JVM work, acquisition and game limiting. Readiness waits are not GPU execution time.")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--top", type=int, default=5)
    parser.add_argument("--skip-seconds", type=float, default=0)
    parser.add_argument("--wayland-protocol", type=Path, help="correlate a companion Wayland protocol capture with actual commits")
    parser.add_argument("--frames-json", type=Path, help="write a new private file with exact per-source boundary joins and missing evidence")
    parser.add_argument("--presentation-window", type=presentation_interval, action="append", default=[], metavar="START:END",
                        help="compare lower-call/copy/presentation timing at offsets from first presented feedback; repeat for good/bad windows")
    args = parser.parse_args()
    if not 1 <= args.top <= 100 or not math.isfinite(args.skip_seconds) or args.skip_seconds < 0:
        parser.error("--top must be 1–100 and --skip-seconds must be finite and nonnegative")
    try:
        windows, invalid = read_windows(args.log)
        observations, invalid_observations = read_observations(args.log)
    except OSError as error:
        parser.exit(1, f"Cannot read pacing capture: {error.strerror}\n")
    if not windows and not observations.get("sources"):
        parser.exit(1, "No supported pacing-profile windows or source traces found.\n")
    if args.frames_json:
        try:
            fd = os.open(args.frames_json, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            with os.fdopen(fd, "w") as stream:
                json.dump(frame_timeline(observations), stream, separators=(",", ":"))
                stream.write("\n")
        except OSError as error:
            parser.exit(1, f"Cannot write frame timeline: {error.strerror}\n")
    print(report(windows, args.top, args.skip_seconds, observations))
    if args.presentation_window:
        print(presentation_window_report(frame_timeline(observations), args.presentation_window, args.top))
    if not windows:
        for pid, context in sorted({(r["pid"], r["context"]) for r in observations["sources"]}):
            print(f"PID {pid}, context {context}: partial capture (no complete one-second window)")
            print("\n".join(frame_trace_report(observations, pid, context, args.top)))
    if args.wayland_protocol:
        from wayland_pacing_protocol import protocol_report
        try:
            print(protocol_report(args.wayland_protocol, observations, args.top))
        except OSError as error:
            parser.exit(1, f"Cannot read Wayland protocol capture: {error.strerror}\n")
    if invalid_observations:
        print(f"Rejected malformed/unsupported observations: {invalid_observations}")
    if invalid:
        print(f"Rejected malformed/unsupported windows: {invalid}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

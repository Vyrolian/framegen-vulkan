#!/usr/bin/env python3
"""Unattended input-latency comparison: baseline, fgvk at 2x, and NVIDIA Smooth Motion.

The native probe (engine/tools/latency-probe, binary fgvk-latency-probe) samples a
synthetic input schedule at every frame and records when the real present completes.
This script runs the probe matrix for each mode, checks that frame generation is
really active through Wayland buffer attach counts, and reports the differences.

Subcommands:
  run      build a session directory, run the matrix, then analyze the session
  analyze  recompute report.json and report.txt for an existing session
"""

import sys

if sys.version_info < (3, 11):
    raise SystemExit("compare-latency.py requires Python 3.11 or newer")

import argparse
import bisect
import copy
import csv
import hashlib
import io
import json
import math
import os
from pathlib import Path
import random
import re
import signal
import statistics
import subprocess
import time
import tomllib
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any, Iterable, Mapping, Sequence


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_PROBE = REPO_ROOT / "engine/build/latency-probe/fgvk-latency-probe"
DEFAULT_LAYER_BUILD = REPO_ROOT / "engine/build/repro-setup-release"
LAYER_SUBDIR = "fgvk-render"
MANIFEST_NAME = "VkLayer_FGVK_render.json"
LAYER_LIBRARY_NAME = "libfgvk-render.so"
GLOBAL_CONFIG_RELATIVE = Path(".config") / "framegen-vulkan" / "conf.toml"

PROFILE_NAME = "latency probe"
PROBE_PROCESS_NAME = "fgvk-latency-probe"
MODES = ("baseline", "fgvk", "smooth-motion")
FG_MODES = ("fgvk", "smooth-motion")
PRESENT_MODES = ("fifo", "mailbox", "immediate")
REFLEX_CHOICES = ("off", "on")
SCHEMA_VERSION = 1

ACTIVATION_DURATION_S = 6.0
ACTIVE_RATIO = 1.8
BASELINE_RATIO = 1.2
SUSPECT_MARGIN_MS = 0.5
MAX_INCOMPLETE_FRACTION = 0.01
PAUSE_BETWEEN_RUNS_S = 3.0
TIMEOUT_GRACE_S = 20.0
TERMINATE_GRACE_S = 3.0
SUCCESSFUL_PRESENTS = frozenset({"VK_SUCCESS", "VK_SUBOPTIMAL_KHR"})
HIDDEN_WINDOW_EVENTS = ("hidden", "occluded", "minimized")
WAYLAND_ATTACH = re.compile(r"-> wl_surface[#@]\d+\.attach\(")

REMOVED_PREFIXES = ("FGVK_", "NVPRESENT_", "MANGOHUD")
REMOVED_KEYS = frozenset({
    "ENABLE_FGVK",
    "DISABLE_FGVK",
    "DISABLE_LAYER_NV_PRESENT_1",
    "VK_INSTANCE_LAYERS",
    "VK_LAYER_PATH",
    "VK_IMPLICIT_LAYER_PATH",
    "VK_ADD_IMPLICIT_LAYER_PATH",
    "ENABLE_VKBASALT",
    "WAYLAND_DEBUG",
})

DISCLAIMER = (
    "Latency is the synthetic input sample to present-wait completion of the real frame "
    "(driver/compositor report, not photons). Generated frames shown earlier are not counted. "
    "No smoothness claim is made."
)

METRIC_NAMES = ("event_p50_ms", "event_p99_ms", "frame_p50_ms", "source_fps", "present_hold_p99_ms")


class SetupError(RuntimeError):
    """A precondition for the run is missing; nothing has been started."""


@dataclass(frozen=True)
class RunSpec:
    mode: str
    cap: int
    reflex: bool
    repeat: int

    @property
    def reflex_label(self) -> str:
        return "on" if self.reflex else "off"


@dataclass(frozen=True)
class FrameRow:
    frame: int
    present_id: int
    sim_ns: int
    event_count: int
    present_call_ns: int
    present_return_ns: int
    present_result: str
    complete_ns: int
    wait_result: str


@dataclass(frozen=True)
class WindowEvent:
    ns: int
    name: str


@dataclass(frozen=True)
class RunOptions:
    present_mode: str
    duration_s: float
    gpu_load: int
    seed: int


# ---------------------------------------------------------------------------
# Environment, configuration and layer setup
# ---------------------------------------------------------------------------


def mode_overrides(mode: str, session_dir: Path) -> dict[str, str]:
    """Return the variables that define one mode, on top of the sanitized base."""
    if mode == "baseline":
        return {
            "DISABLE_FGVK": "1",
            "NVPRESENT_ENABLE_SMOOTH_MOTION": "0",
            "DISABLE_LAYER_NV_PRESENT_1": "1",
        }
    if mode == "smooth-motion":
        return {
            "DISABLE_FGVK": "1",
            "NVPRESENT_ENABLE_SMOOTH_MOTION": "1",
        }
    if mode == "fgvk":
        return {
            "VK_IMPLICIT_LAYER_PATH": str(session_dir / "manifests"),
            "ENABLE_FGVK": "1",
            "FGVK_CONFIG": str(session_dir / "conf.toml"),
            "NVPRESENT_ENABLE_SMOOTH_MOTION": "0",
            "DISABLE_LAYER_NV_PRESENT_1": "1",
        }
    raise ValueError(f"unknown mode: {mode}")


def build_child_env(base: Mapping[str, str], mode: str, session_dir: Path) -> dict[str, str]:
    """Build the probe environment for one mode. The base mapping is never modified."""
    env = {
        key: value
        for key, value in base.items()
        if not key.startswith(REMOVED_PREFIXES) and key not in REMOVED_KEYS
    }
    env["DISABLE_MANGOHUD"] = "1"
    env["SDL_VIDEO_DRIVER"] = "wayland"
    env.update(mode_overrides(mode, session_dir))
    return env


def toml_string(value: str) -> str:
    """Return a TOML basic string. Non-ASCII text is kept; control characters are escaped."""
    out = ['"']
    for char in value:
        code = ord(char)
        if char in '"\\':
            out.append("\\" + char)
        elif code < 0x20 or code == 0x7F:
            out.append(f"\\u{code:04x}")
        else:
            out.append(char)
    out.append('"')
    return "".join(out)


def render_config(global_dll: str) -> str:
    """Return the session conf.toml text for the single latency-probe profile."""
    lines = [
        "version = 2",
        "",
        "[global]",
        "allow_fp16 = false",
        f"dll = {toml_string(global_dll)}",
        "",
        "[[profile]]",
        f"name = {toml_string(PROFILE_NAME)}",
        f"active_in = [{toml_string(PROBE_PROCESS_NAME)}]",
        "frame_generation_provisioned = true",
        "frame_generation_enabled = true",
        "adaptive = false",
        "multiplier = 2",
        "base_fps_cap = 0",
        "adaptive_stable_cadence = false",
        "dynamic_cadence_recovery = false",
        "gpu_headroom_percent = 0",
        "flow_scale = 1.0",
        "performance_mode = false",
        "",
    ]
    return "\n".join(lines)


def read_global_dll(config_path: Path) -> str:
    """Return [global] dll from the user's fgvk-render config. The DLL itself is never touched."""
    try:
        with config_path.open("rb") as handle:
            data = tomllib.load(handle)
    except FileNotFoundError as exc:
        raise SetupError(f"fgvk-render config not found: {config_path}") from exc
    except tomllib.TOMLDecodeError as exc:
        raise SetupError(f"fgvk-render config is not valid TOML: {config_path}: {exc}") from exc
    global_table = data.get("global")
    dll = global_table.get("dll") if isinstance(global_table, dict) else None
    if not isinstance(dll, str) or not dll:
        raise SetupError(f"{config_path} has no [global] dll value")
    return dll


def rewrite_manifest(manifest: Mapping[str, Any], library_path: str) -> dict[str, Any]:
    """Return a copy of the layer manifest with layer.library_path replaced."""
    rewritten = copy.deepcopy(dict(manifest))
    layer = rewritten.get("layer")
    if not isinstance(layer, dict):
        raise SetupError("layer manifest has no layer object")
    layer["library_path"] = library_path
    return rewritten


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, data: Any) -> None:
    """Write JSON through a temporary file so an interrupted run never leaves a partial file."""
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def write_private_text(path: Path, text: str) -> None:
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as handle:
        handle.write(text)


def create_session_dir(path: Path) -> None:
    """Create the session directory with mode 0700. It must not exist yet."""
    if path.exists() or path.is_symlink():
        raise SetupError(f"--output must not exist: {path}")
    if not path.parent.is_dir():
        raise SetupError(f"parent of --output does not exist: {path.parent}")
    os.mkdir(path, 0o700)
    os.chmod(path, 0o700)
    for name in ("manifests", "runs", "activation"):
        os.mkdir(path / name, 0o700)


def install_session_layer(session_dir: Path, layer_build: Path, global_dll: str) -> tuple[Path, Path]:
    """Write the manifest copy and conf.toml. Returns (manifest, config) paths."""
    layer_dir = layer_build / LAYER_SUBDIR
    source_manifest = layer_dir / MANIFEST_NAME
    library = (layer_dir / LAYER_LIBRARY_NAME).absolute()
    manifest_path = session_dir / "manifests" / MANIFEST_NAME
    manifest = read_json(source_manifest)
    write_json(manifest_path, rewrite_manifest(manifest, str(library)))
    config_path = session_dir / "conf.toml"
    write_private_text(config_path, render_config(global_dll))
    return manifest_path, config_path


# ---------------------------------------------------------------------------
# Running probes
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class ChildResult:
    returncode: int | None
    timed_out: bool
    elapsed_s: float


def _signal_group(pid: int, signum: int) -> None:
    try:
        os.killpg(pid, signum)
    except ProcessLookupError:
        pass


def run_child(argv: Sequence[str], env: Mapping[str, str], timeout_s: float, log_path: Path,
              stderr_path: Path | None = None) -> ChildResult:
    """Run one probe in its own session. Time out, then SIGTERM, then SIGKILL the group."""
    started = time.monotonic()
    timed_out = False
    with log_path.open("wb") as stdout_handle:
        stderr_handle = stdout_handle
        if stderr_path is not None:
            stderr_handle = stderr_path.open("wb")
        try:
            proc = subprocess.Popen(list(argv), env=dict(env), stdin=subprocess.DEVNULL,
                                    stdout=stdout_handle, stderr=stderr_handle, start_new_session=True)
            try:
                try:
                    returncode = proc.wait(timeout=timeout_s)
                except subprocess.TimeoutExpired:
                    timed_out = True
                    _signal_group(proc.pid, signal.SIGTERM)
                    try:
                        returncode = proc.wait(timeout=TERMINATE_GRACE_S)
                    except subprocess.TimeoutExpired:
                        _signal_group(proc.pid, signal.SIGKILL)
                        returncode = proc.wait()
            except BaseException:
                _signal_group(proc.pid, signal.SIGKILL)
                proc.wait()
                raise
        finally:
            if stderr_handle is not stdout_handle:
                stderr_handle.close()
    return ChildResult(returncode=returncode, timed_out=timed_out, elapsed_s=time.monotonic() - started)


def probe_argv(probe: Path, output_dir: Path, *, cap: int, reflex: bool, duration_s: float,
               options: RunOptions, probe_seed: int) -> list[str]:
    return [
        str(probe),
        "--output-dir", str(output_dir),
        "--duration", f"{duration_s:g}",
        "--fps-cap", str(cap),
        "--present-mode", options.present_mode,
        "--reflex", "on" if reflex else "off",
        "--gpu-load", str(options.gpu_load),
        "--seed", str(probe_seed),
    ]


def count_wayland_attaches(log_path: Path) -> int:
    """Count wl_surface.attach requests in a WAYLAND_DEBUG=client log.

    libwayland prints proxies as wl_surface#N or wl_surface@N, so both forms count.
    wl_surface.commit is not counted: it is not a buffer attach.
    """
    if not log_path.is_file():
        return 0
    count = 0
    with log_path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if WAYLAND_ATTACH.search(line):
                count += 1
    return count


def count_successful_presents(frames_csv: Path) -> int:
    if not frames_csv.is_file():
        return 0
    with frames_csv.open(newline="", encoding="utf-8") as handle:
        return sum(1 for row in csv.DictReader(handle) if row.get("present_result") in SUCCESSFUL_PRESENTS)


def activation_verdict(mode: str, attaches: int, presents: int) -> dict[str, Any]:
    """Compare Wayland buffer attaches with successful presents for one activation run."""
    ratio = attaches / presents if presents > 0 else None
    if ratio is None:
        status = "no-presents"
    elif mode == "baseline":
        status = "ok" if ratio <= BASELINE_RATIO else "not-ok"
    else:
        status = "active" if ratio >= ACTIVE_RATIO else "not-active"
    return {"attaches": attaches, "presents": presents, "ratio": ratio, "status": status}


def refresh_activation_record(session_dir: Path, record: Mapping[str, Any]) -> dict[str, Any]:
    """Return an activation record, recomputed from its run directory when the logs are present.

    The verdict is recomputed from activation/<name>/wayland.log and probe/frames.csv. When either
    file is missing, the stored values are kept and the record's source says so.
    """
    refreshed = dict(record)
    rel_dir = record.get("dir")
    if rel_dir:
        run_dir = session_dir / str(rel_dir)
        log_path = run_dir / "wayland.log"
        frames_path = run_dir / "probe" / "frames.csv"
        if log_path.is_file() and frames_path.is_file():
            attaches = count_wayland_attaches(log_path)
            presents = count_successful_presents(frames_path)
            refreshed.update(activation_verdict(str(record["mode"]), attaches, presents))
            refreshed["source"] = "recomputed"
            return refreshed
    refreshed["source"] = "stored"
    return refreshed


class LatencySession:
    """Mutable session state, persisted to session.json after every run."""

    def __init__(self, path: Path, state: dict[str, Any]) -> None:
        self.path = path
        self.state = state
        self._started = 0

    def save(self) -> None:
        write_json(self.path / "session.json", self.state)

    def pause_before_next_run(self) -> None:
        if self._started:
            time.sleep(PAUSE_BETWEEN_RUNS_S)
        self._started += 1


def execute_run(session: LatencySession, *, kind: str, name: str, mode: str, cap: int, reflex: bool,
                repeat: int | None, probe_seed: int, probe: Path, base_env: Mapping[str, str],
                options: RunOptions, duration_s: float, wayland_debug: bool, progress_prefix: str) -> dict[str, Any]:
    """Run one probe into <session>/<kind dir>/<name>/probe and record the outcome."""
    kind_dir = "activation" if kind == "activation" else "runs"
    rel_dir = f"{kind_dir}/{name}"
    run_dir = session.path / rel_dir
    run_dir.mkdir(mode=0o700)
    probe_dir = run_dir / "probe"
    probe_dir.mkdir(mode=0o700)

    env = build_child_env(base_env, mode, session.path)
    if wayland_debug:
        env["WAYLAND_DEBUG"] = "client"
    argv = probe_argv(probe, probe_dir, cap=cap, reflex=reflex, duration_s=duration_s,
                      options=options, probe_seed=probe_seed)

    session.pause_before_next_run()
    if wayland_debug:
        result = run_child(argv, env, duration_s + TIMEOUT_GRACE_S, run_dir / "probe.log",
                           stderr_path=run_dir / "wayland.log")
    else:
        result = run_child(argv, env, duration_s + TIMEOUT_GRACE_S, run_dir / "probe.log")

    record: dict[str, Any] = {
        "kind": kind,
        "name": name,
        "mode": mode,
        "cap": cap,
        "reflex": "on" if reflex else "off",
        "repeat": repeat,
        "probe_seed": probe_seed,
        "present_mode": options.present_mode,
        "gpu_load": options.gpu_load,
        "duration_s": duration_s,
        "returncode": result.returncode,
        "timed_out": result.timed_out,
        "elapsed_s": round(result.elapsed_s, 3),
        "dir": rel_dir,
        "env_overrides": mode_overrides(mode, session.path),
        "argv": argv,
    }
    write_json(run_dir / "run.json", record)
    print(f"{progress_prefix} {name}: exit={result.returncode} "
          f"timeout={'yes' if result.timed_out else 'no'} elapsed={result.elapsed_s:.1f}s", flush=True)
    return record


def run_activation_checks(session: LatencySession, *, targets: Sequence[str], cap: int, probe: Path,
                          base_env: Mapping[str, str], options: RunOptions) -> list[dict[str, Any]]:
    """Confirm frame generation is active with Wayland buffer attach counts, one short run per mode."""
    results: list[dict[str, Any]] = []
    for index, mode in enumerate(targets, start=1):
        name = f"{index}-{mode}-cap{cap}-reflexoff"
        record = execute_run(session, kind="activation", name=name, mode=mode, cap=cap, reflex=False,
                             repeat=None, probe_seed=options.seed, probe=probe, base_env=base_env,
                             options=options, duration_s=ACTIVATION_DURATION_S, wayland_debug=True,
                             progress_prefix=f"[check {index}/{len(targets)}]")
        run_dir = session.path / record["dir"]
        attaches = count_wayland_attaches(run_dir / "wayland.log")
        presents = count_successful_presents(run_dir / "probe" / "frames.csv")
        record.update(activation_verdict(mode, attaches, presents))
        results.append(record)
        session.state["activation"].append(record)
        session.save()
        ratio = "-" if record["ratio"] is None else f"{record['ratio']:.2f}"
        print(f"  {mode}: attaches={attaches} presents={presents} ratio={ratio} status={record['status']}",
              flush=True)
    return results


def build_matrix(modes: Sequence[str], caps: Sequence[int], reflex_values: Sequence[bool], repeats: int,
                 seed: int) -> list[RunSpec]:
    specs = [
        RunSpec(mode=mode, cap=cap, reflex=reflex, repeat=repeat)
        for mode in modes
        for cap in caps
        for reflex in reflex_values
        for repeat in range(1, repeats + 1)
    ]
    random.Random(seed).shuffle(specs)
    return specs


def run_matrix(session: LatencySession, specs: Sequence[RunSpec], *, probe: Path,
               base_env: Mapping[str, str], options: RunOptions, seed: int) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    total = len(specs)
    for index, spec in enumerate(specs, start=1):
        name = f"{index}-{spec.mode}-cap{spec.cap}-reflex{spec.reflex_label}-r{spec.repeat}"
        record = execute_run(session, kind="matrix", name=name, mode=spec.mode, cap=spec.cap,
                             reflex=spec.reflex, repeat=spec.repeat, probe_seed=seed + spec.repeat,
                             probe=probe, base_env=base_env, options=options, duration_s=options.duration_s,
                             wayland_debug=False, progress_prefix=f"[{index}/{total}]")
        results.append(record)
        session.state["runs"].append(record)
        session.save()
    return results


# ---------------------------------------------------------------------------
# Analysis (pure functions where possible)
# ---------------------------------------------------------------------------


def parse_frames_csv(text: str) -> list[FrameRow]:
    rows = [
        FrameRow(
            frame=int(raw["frame"]),
            present_id=int(raw["present_id"]),
            sim_ns=int(raw["sim_ns"]),
            event_count=int(raw["event_count"]),
            present_call_ns=int(raw["present_call_ns"]),
            present_return_ns=int(raw["present_return_ns"]),
            present_result=raw["present_result"],
            complete_ns=int(raw["complete_ns"]),
            wait_result=raw["wait_result"],
        )
        for raw in csv.DictReader(io.StringIO(text))
    ]
    rows.sort(key=lambda row: row.present_id)
    return rows


def parse_events_csv(text: str) -> list[int]:
    rows = list(csv.DictReader(io.StringIO(text)))
    rows.sort(key=lambda raw: int(raw["index"]))
    return [int(raw["event_ns"]) for raw in rows]


def parse_window_csv(text: str) -> list[WindowEvent]:
    return [WindowEvent(ns=int(raw["ns"]), name=raw["event"]) for raw in csv.DictReader(io.StringIO(text))]


def percentile(sorted_values: Sequence[float], pct: float) -> float | None:
    """Linear-interpolation percentile over ascending values, as numpy's default."""
    count = len(sorted_values)
    if count == 0:
        return None
    if count == 1:
        return float(sorted_values[0])
    rank = (count - 1) * pct / 100.0
    lower = math.floor(rank)
    upper = math.ceil(rank)
    fraction = rank - lower
    return float(sorted_values[lower]) + (float(sorted_values[upper]) - float(sorted_values[lower])) * fraction


def distribution(values: Iterable[float]) -> dict[str, Any]:
    data = sorted(float(value) for value in values)
    count = len(data)
    if count == 0:
        return {"n": 0, "mean": None, "p50": None, "p95": None, "p99": None, "max": None}
    return {
        "n": count,
        "mean": sum(data) / count,
        "p50": percentile(data, 50),
        "p95": percentile(data, 95),
        "p99": percentile(data, 99),
        "max": data[-1],
    }


def _empty_run_stats() -> dict[str, Any]:
    return {
        "frames_kept": 0,
        "frames_incomplete": 0,
        "events_measured": 0,
        "events_uncompleted": 0,
        "events_without_frame": 0,
        "event_latency_ms": distribution([]),
        "frame_latency_ms": distribution([]),
        "present_hold_ms": distribution([]),
        "source_fps": None,
        "completion_interval_median_ms": None,
    }


def summarize_run(frames: Sequence[FrameRow], events: Sequence[int], window_events: Sequence[WindowEvent], *,
                  start_ns: int | None, warmup_s: float, exit_code: int | None, timed_out: bool,
                  problems: Sequence[str] = ()) -> dict[str, Any]:
    """Validate one run and compute its latency statistics.

    Frames are kept when sim_ns >= start_ns + warmup. An event maps to the lowest frame whose
    event_count exceeds the event index, so its frame always lies inside the kept window.
    """
    reasons: list[str] = []
    if timed_out:
        reasons.append("timeout")
    if exit_code is None:
        reasons.append("not-run")
    elif exit_code != 0:
        reasons.append(f"probe-exit-{exit_code}")
    reasons.extend(problems)

    ordered = sorted(frames, key=lambda row: row.present_id)
    if start_ns is None:
        reasons.append("missing-meta")
        stats = _empty_run_stats()
        stats["frames_total"] = len(ordered)
        stats["events_total"] = len(events)
        return {"valid": False, "invalid_reasons": reasons, "warmup_s": warmup_s, **stats}

    cutoff = start_ns + round(warmup_s * 1e9)
    for name in HIDDEN_WINDOW_EVENTS:
        if any(event.name == name and event.ns >= cutoff for event in window_events):
            reasons.append(f"window-{name}")

    kept = [row for row in ordered if row.sim_ns >= cutoff]
    if not kept:
        reasons.append("no-kept-frames")
    incomplete = sum(1 for row in kept if row.complete_ns == 0)
    if kept and incomplete / len(kept) > MAX_INCOMPLETE_FRACTION:
        reasons.append("incomplete-frames")

    completed = [row for row in ordered if row.complete_ns != 0]
    if any(row.complete_ns < row.present_return_ns for row in completed):
        reasons.append("completion-before-present-return")
    if any(later.complete_ns < earlier.complete_ns for earlier, later in zip(completed, completed[1:])):
        reasons.append("completion-order")

    counts = [row.event_count for row in ordered]
    event_latencies: list[float] = []
    measured = 0
    uncompleted = 0
    without_frame = 0
    for index, event_ns in enumerate(events):
        if event_ns < cutoff:
            continue
        measured += 1
        position = bisect.bisect_right(counts, index)
        if position >= len(ordered):
            uncompleted += 1
            without_frame += 1
            continue
        frame = ordered[position]
        if frame.complete_ns == 0:
            uncompleted += 1
            continue
        event_latencies.append((frame.complete_ns - event_ns) / 1e6)

    frame_latencies = [(row.complete_ns - row.sim_ns) / 1e6 for row in kept if row.complete_ns != 0]
    holds = [(row.present_return_ns - row.present_call_ns) / 1e6 for row in kept]
    calls = [row.present_call_ns for row in kept]
    source_fps = None
    if len(calls) >= 2 and calls[-1] > calls[0]:
        source_fps = (len(calls) - 1) / ((calls[-1] - calls[0]) / 1e9)
    kept_completed = [row for row in kept if row.complete_ns != 0]
    intervals = [(later.complete_ns - earlier.complete_ns) / 1e6
                 for earlier, later in zip(kept_completed, kept_completed[1:])]

    return {
        "valid": not reasons,
        "invalid_reasons": reasons,
        "warmup_s": warmup_s,
        "frames_total": len(ordered),
        "frames_kept": len(kept),
        "frames_incomplete": incomplete,
        "events_total": len(events),
        "events_measured": measured,
        "events_uncompleted": uncompleted,
        "events_without_frame": without_frame,
        "event_latency_ms": distribution(event_latencies),
        "frame_latency_ms": distribution(frame_latencies),
        "present_hold_ms": distribution(holds),
        "source_fps": source_fps,
        "completion_interval_median_ms": statistics.median(intervals) if intervals else None,
    }


def analyze_run_dir(run_dir: Path, record: Mapping[str, Any], warmup_s: float) -> dict[str, Any]:
    """Read one run directory and summarize it, keeping the orchestrator's identity fields."""
    probe_dir = run_dir / "probe"
    problems: list[str] = []
    start_ns = None
    meta_path = probe_dir / "meta.json"
    if meta_path.is_file():
        try:
            start_ns = int(read_json(meta_path)["start_ns"])
        except (ValueError, KeyError, TypeError):
            problems.append("malformed-meta")

    frames: list[FrameRow] = []
    events: list[int] = []
    window_events: list[WindowEvent] = []
    for name, loader in (("frames.csv", parse_frames_csv), ("events.csv", parse_events_csv),
                         ("window.csv", parse_window_csv)):
        path = probe_dir / name
        if not path.is_file():
            problems.append(f"missing-output-{name}")
            continue
        try:
            parsed = loader(path.read_text(encoding="utf-8"))
        except (ValueError, KeyError):
            problems.append(f"malformed-output-{name}")
            continue
        if name == "frames.csv":
            frames = parsed
        elif name == "events.csv":
            events = parsed
        else:
            window_events = parsed

    exit_code = record.get("returncode")
    summary = summarize_run(frames, events, window_events, start_ns=start_ns, warmup_s=warmup_s,
                            exit_code=exit_code, timed_out=bool(record.get("timed_out")), problems=problems)
    summary.update({
        "name": record["name"],
        "mode": record["mode"],
        "cap": int(record["cap"]),
        "reflex": record["reflex"],
        "repeat": record.get("repeat"),
        "present_mode": record["present_mode"],
        "gpu_load": int(record["gpu_load"]),
        "returncode": exit_code,
        "timed_out": bool(record.get("timed_out")),
        "dir": record["dir"],
    })
    return summary


def aggregate(values: Sequence[float]) -> dict[str, Any] | None:
    if not values:
        return None
    return {"median": statistics.median(values), "min": min(values), "max": max(values), "n": len(values)}


def _metric_value(summary: Mapping[str, Any], name: str) -> float | None:
    if name == "event_p50_ms":
        return summary["event_latency_ms"]["p50"]
    if name == "event_p99_ms":
        return summary["event_latency_ms"]["p99"]
    if name == "frame_p50_ms":
        return summary["frame_latency_ms"]["p50"]
    if name == "source_fps":
        return summary["source_fps"]
    if name == "present_hold_p99_ms":
        return summary["present_hold_ms"]["p99"]
    raise ValueError(name)


def compare_runs(runs: Sequence[Mapping[str, Any]], activation: Mapping[str, Mapping[str, Any]]) -> list[dict[str, Any]]:
    """Group runs by (cap, reflex, present mode, gpu load) and compare each mode with baseline."""
    groups: dict[tuple[int, str, str, int], list[Mapping[str, Any]]] = {}
    for run in runs:
        key = (int(run["cap"]), str(run["reflex"]), str(run["present_mode"]), int(run["gpu_load"]))
        groups.setdefault(key, []).append(run)

    comparison: list[dict[str, Any]] = []
    for key in sorted(groups, key=lambda item: (item[0], item[1] != "off", item[2], item[3])):
        members = groups[key]
        modes: dict[str, dict[str, Any]] = {}
        for mode in MODES:
            mode_runs = [run for run in members if run["mode"] == mode]
            if not mode_runs:
                continue
            valid = [run for run in mode_runs if run["valid"]]
            metrics = {}
            for name in METRIC_NAMES:
                values = [value for value in (_metric_value(run, name) for run in valid) if value is not None]
                metrics[name] = aggregate(values)
            modes[mode] = {
                "mode": mode,
                "runs_total": len(mode_runs),
                "runs_valid": len(valid),
                "invalid_runs": [{"name": run["name"], "reasons": run["invalid_reasons"]}
                                 for run in mode_runs if not run["valid"]],
                "metrics": metrics,
            }

        baseline = modes.get("baseline")
        base_p50 = baseline["metrics"]["event_p50_ms"]["median"] if baseline and baseline["metrics"]["event_p50_ms"] else None
        base_p99 = baseline["metrics"]["event_p99_ms"]["median"] if baseline and baseline["metrics"]["event_p99_ms"] else None
        rows = []
        for mode in MODES:
            row = modes.get(mode)
            if row is None:
                continue
            p50 = row["metrics"]["event_p50_ms"]
            p99 = row["metrics"]["event_p99_ms"]
            row["activation_status"] = activation.get(mode, {}).get("status")
            row["delta_event_p50_ms"] = None
            row["delta_event_p99_ms"] = None
            if mode != "baseline" and p50 is not None and base_p50 is not None:
                row["delta_event_p50_ms"] = p50["median"] - base_p50
            if mode != "baseline" and p99 is not None and base_p99 is not None:
                row["delta_event_p99_ms"] = p99["median"] - base_p99
            flags = []
            if mode in FG_MODES and row["activation_status"] != "active":
                flags.append("activation-unverified")
            if (mode in FG_MODES and p50 is not None and base_p50 is not None
                    and p50["median"] < base_p50 - SUSPECT_MARGIN_MS):
                flags.append("suspect")
            if row["runs_valid"] < 2:
                flags.append("insufficient")
            row["flags"] = flags
            rows.append(row)

        comparison.append({
            "cap": key[0],
            "reflex": key[1],
            "present_mode": key[2],
            "gpu_load": key[3],
            "modes": rows,
        })
    return comparison


def build_report(session: Mapping[str, Any], runs: Sequence[Mapping[str, Any]],
                 activation: Sequence[Mapping[str, Any]], session_dir: Path) -> dict[str, Any]:
    activation_by_mode: dict[str, Mapping[str, Any]] = {}
    for record in activation:
        activation_by_mode.setdefault(str(record["mode"]), record)
    args = session["args"]
    return {
        "schema": SCHEMA_VERSION,
        "session": str(session_dir),
        "disclaimer": DISCLAIMER,
        "args": args,
        "activation": [
            {
                "mode": record["mode"],
                "cap": record.get("cap"),
                "attaches": record.get("attaches"),
                "presents": record.get("presents"),
                "ratio": record.get("ratio"),
                "status": record.get("status"),
                "source": record.get("source", "stored"),
            }
            for record in activation
        ],
        "runs": list(runs),
        "comparison": compare_runs(runs, activation_by_mode),
    }


def _fmt(value: float | None, digits: int = 2) -> str:
    return "-" if value is None else f"{value:.{digits}f}"


def _fmt_range(aggregate_value: Mapping[str, Any] | None, digits: int = 2) -> str:
    if aggregate_value is None:
        return "-"
    return (f"{aggregate_value['median']:.{digits}f} "
            f"[{aggregate_value['min']:.{digits}f}-{aggregate_value['max']:.{digits}f}]")


def _fmt_delta(value: float | None) -> str:
    return "-" if value is None else f"{value:+.2f}"


def format_report_text(report: Mapping[str, Any]) -> str:
    args = report["args"]
    runs = report["runs"]
    valid_count = sum(1 for run in runs if run["valid"])
    lines = [
        f"fgvk input-latency comparison: {report['session']}",
        DISCLAIMER,
        (f"warmup {args['warmup']:g} s excluded; duration {args['duration']:g} s; "
         f"repeats {args['repeats']}; seed {args['seed']}; gpu load {args['gpu_load']}; "
         f"present mode {args['present_mode']}"),
        "",
        "Activation (wl_surface buffer attaches per successful present, reflex off):",
    ]
    if not report["activation"]:
        lines.append("  not checked (--no-activation-check); FG modes are reported as activation-unverified")
    for record in report["activation"]:
        attaches = "-" if record["attaches"] is None else record["attaches"]
        lines.append(f"  {record['mode']:<14} attaches={attaches} presents={record['presents']} "
                     f"ratio={_fmt(record['ratio'])} status={record['status']} ({record['source']})")
    lines += [
        "",
        f"Runs: {len(runs)} total, {valid_count} valid, {len(runs) - valid_count} invalid",
    ]
    for run in runs:
        if not run["valid"]:
            lines.append(f"  invalid {run['name']}: {', '.join(run['invalid_reasons'])}")
    lines += [
        "",
        "Latency in ms: median across valid repeats, [min-max] across repeats.",
    ]
    header = (f"  {'mode':<14} {'valid':>7} {'event p50':>24} {'event p99':>24} {'d p50':>8} {'d p99':>8} "
              f"{'frame p50':>24} {'src fps':>9} {'hold p99':>24}  flags")
    for group in report["comparison"]:
        lines.append("")
        lines.append(f"cap {group['cap']}  reflex {group['reflex']}  present {group['present_mode']}  "
                     f"gpu load {group['gpu_load']}")
        lines.append(header)
        for row in group["modes"]:
            metrics = row["metrics"]
            fps = metrics["source_fps"]
            lines.append(
                f"  {row['mode']:<14} {str(row['runs_valid']) + '/' + str(row['runs_total']):>7} "
                f"{_fmt_range(metrics['event_p50_ms']):>24} {_fmt_range(metrics['event_p99_ms']):>24} "
                f"{_fmt_delta(row['delta_event_p50_ms']):>8} {_fmt_delta(row['delta_event_p99_ms']):>8} "
                f"{_fmt_range(metrics['frame_p50_ms']):>24} "
                f"{(_fmt(fps['median'], 1) if fps else '-'):>9} "
                f"{_fmt_range(metrics['present_hold_p99_ms']):>24}  "
                f"{','.join(row['flags']) or '-'}"
            )
    lines += [
        "",
        "Deltas are mode median minus baseline median for the same group.",
        "suspect: FG event p50 median is more than 0.5 ms below baseline.",
        "insufficient: fewer than 2 valid runs. activation-unverified: no active Wayland check.",
        DISCLAIMER,
    ]
    return "\n".join(lines) + "\n"


def analyze_session(session_dir: Path) -> dict[str, Any]:
    """Recompute the report for a session directory and write report.json and report.txt."""
    session = read_json(session_dir / "session.json")
    warmup_s = float(session["args"]["warmup"])
    runs = [analyze_run_dir(session_dir / record["dir"], record, warmup_s)
            for record in session.get("runs", [])]
    activation = [refresh_activation_record(session_dir, record) for record in session.get("activation", [])]
    report = build_report(session, runs, activation, session_dir)
    text = format_report_text(report)
    write_json(session_dir / "report.json", report)
    (session_dir / "report.txt").write_text(text, encoding="utf-8")
    report["text"] = text
    return report


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------


def _parse_choice_list(text: str, choices: Sequence[str], option: str) -> list[str]:
    values: list[str] = []
    for item in text.split(","):
        value = item.strip()
        if value not in choices:
            raise SetupError(f"{option}: {value!r} is not one of {', '.join(choices)}")
        if value not in values:
            values.append(value)
    if not values:
        raise SetupError(f"{option} must not be empty")
    return values


def _parse_int_list(text: str, option: str, minimum: int) -> list[int]:
    values: list[int] = []
    for item in text.split(","):
        try:
            value = int(item.strip())
        except ValueError as exc:
            raise SetupError(f"{option}: {item.strip()!r} is not an integer") from exc
        if value < minimum:
            raise SetupError(f"{option}: values must be at least {minimum}")
        if value not in values:
            values.append(value)
    return values


def cmd_run(args: argparse.Namespace) -> int:
    modes = _parse_choice_list(args.modes, MODES, "--modes")
    caps = _parse_int_list(args.caps, "--caps", minimum=0)
    reflex_labels = _parse_choice_list(args.reflex, REFLEX_CHOICES, "--reflex")
    reflex_values = [label == "on" for label in reflex_labels]
    if args.repeats < 1:
        raise SetupError("--repeats must be at least 1")
    if args.duration <= 0:
        raise SetupError("--duration must be positive")
    if args.warmup < 0 or args.warmup >= args.duration:
        raise SetupError("--warmup must be >= 0 and smaller than --duration")
    if args.gpu_load < 0:
        raise SetupError("--gpu-load must not be negative")

    probe = Path(args.probe).absolute()
    if not probe.is_file() or not os.access(probe, os.X_OK):
        raise SetupError(f"probe is not an executable file (build it with `just latency-probe`): {probe}")
    layer_build = Path(args.layer_build).absolute()
    source_manifest = layer_build / LAYER_SUBDIR / MANIFEST_NAME
    layer_library = layer_build / LAYER_SUBDIR / LAYER_LIBRARY_NAME
    for path in (source_manifest, layer_library):
        if not path.is_file():
            raise SetupError(f"layer build file missing: {path}")
    global_dll = read_global_dll(Path.home() / GLOBAL_CONFIG_RELATIVE)

    session_dir = Path(os.path.abspath(args.output))
    create_session_dir(session_dir)
    manifest_path, config_path = install_session_layer(session_dir, layer_build, global_dll)

    specs = build_matrix(modes, caps, reflex_values, args.repeats, args.seed)
    options = RunOptions(present_mode=args.present_mode, duration_s=args.duration,
                         gpu_load=args.gpu_load, seed=args.seed)
    state: dict[str, Any] = {
        "schema": SCHEMA_VERSION,
        "created_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "args": {
            "output": str(session_dir),
            "probe": str(probe),
            "layer_build": str(layer_build),
            "modes": modes,
            "caps": caps,
            "reflex": reflex_labels,
            "present_mode": args.present_mode,
            "repeats": args.repeats,
            "duration": args.duration,
            "warmup": args.warmup,
            "gpu_load": args.gpu_load,
            "seed": args.seed,
            "activation_check": not args.no_activation_check,
        },
        "probe": {"path": str(probe), "sha256": sha256_file(probe)},
        "layer_library": {"path": str(layer_library.absolute()), "sha256": sha256_file(layer_library)},
        "manifest": str(manifest_path.relative_to(session_dir)),
        "config": str(config_path.relative_to(session_dir)),
        "python": sys.version.split()[0],
        "matrix": [{"mode": spec.mode, "cap": spec.cap, "reflex": spec.reflex_label, "repeat": spec.repeat}
                   for spec in specs],
        "activation": [],
        "runs": [],
    }
    session = LatencySession(session_dir, state)
    session.save()
    print(f"session: {session_dir}", flush=True)
    print(f"probe sha256 {state['probe']['sha256']}", flush=True)
    print(f"layer library sha256 {state['layer_library']['sha256']}", flush=True)
    print(f"matrix: {len(specs)} runs of {args.duration:g} s", flush=True)

    base_env = dict(os.environ)
    if not args.no_activation_check:
        targets = ["baseline"] + [mode for mode in modes if mode in FG_MODES]
        print(f"activation checks ({ACTIVATION_DURATION_S:g} s each): {', '.join(targets)}", flush=True)
        run_activation_checks(session, targets=targets, cap=caps[0], probe=probe, base_env=base_env,
                              options=options)
    run_matrix(session, specs, probe=probe, base_env=base_env, options=options, seed=args.seed)

    report = analyze_session(session_dir)
    print(report["text"], end="")
    print(f"wrote {session_dir / 'report.json'}")
    print(f"wrote {session_dir / 'report.txt'}")
    return 0


def cmd_analyze(args: argparse.Namespace) -> int:
    session_dir = Path(os.path.abspath(args.session))
    if not (session_dir / "session.json").is_file():
        raise SetupError(f"not a session directory (no session.json): {session_dir}")
    report = analyze_session(session_dir)
    print(report["text"], end="")
    print(f"wrote {session_dir / 'report.json'}")
    print(f"wrote {session_dir / 'report.txt'}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Unattended input-latency comparison: baseline, fgvk 2x and NVIDIA Smooth Motion.",
        epilog=DISCLAIMER,
    )
    sub = parser.add_subparsers(dest="command", required=True)

    run = sub.add_parser("run", help="run the probe matrix and analyze the session")
    run.add_argument("--output", required=True, help="new session directory (must not exist; created 0700)")
    run.add_argument("--probe", default=str(DEFAULT_PROBE),
                     help=f"fgvk-latency-probe binary (default: {DEFAULT_PROBE})")
    run.add_argument("--layer-build", default=str(DEFAULT_LAYER_BUILD),
                     help=f"build dir with {LAYER_SUBDIR}/{MANIFEST_NAME} and {LAYER_LIBRARY_NAME} "
                          f"(default: {DEFAULT_LAYER_BUILD})")
    run.add_argument("--modes", default=",".join(MODES),
                     help="comma list from baseline,fgvk,smooth-motion (default: all)")
    run.add_argument("--caps", default="60", help="comma list of frame caps, 0 uncapped (default: 60)")
    run.add_argument("--reflex", default="off,on", help="comma list of off,on (default: off,on)")
    run.add_argument("--present-mode", default="fifo", choices=PRESENT_MODES,
                     help="present mode for every run (default: fifo)")
    run.add_argument("--repeats", type=int, default=3, help="repeats per (mode, cap, reflex) (default: 3)")
    run.add_argument("--duration", type=float, default=35.0, help="measured seconds per run (default: 35)")
    run.add_argument("--warmup", type=float, default=5.0, help="seconds excluded from statistics (default: 5)")
    run.add_argument("--gpu-load", type=int, default=0, help="fragment-shader loop iterations (default: 0)")
    run.add_argument("--seed", type=int, default=1, help="matrix shuffle and event seed (default: 1)")
    run.add_argument("--no-activation-check", action="store_true",
                     help="skip the Wayland buffer attach activation runs")
    run.set_defaults(handler=cmd_run)

    analyze = sub.add_parser("analyze", help="recompute report.json and report.txt for a session")
    analyze.add_argument("session", help="session directory created by the run subcommand")
    analyze.set_defaults(handler=cmd_analyze)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.handler(args)
    except SetupError as exc:
        print(f"compare-latency: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

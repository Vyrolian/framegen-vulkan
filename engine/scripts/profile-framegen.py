#!/usr/bin/env python3
"""Local, Nsight-free comparison capture for framegen-vulkan and NVIDIA Smooth Motion."""

import argparse
import csv
import io
import json
import math
import os
from pathlib import Path
import selectors
import shutil
import signal
import socket
import statistics
import struct
import subprocess
import sys
import time
from typing import TypedDict


class Distribution(TypedDict):
    samples: int
    mean: float
    p50: float
    p95: float
    p99: float
    maximum: float


class GpuIdentity(TypedDict):
    uuid: str
    name: str
    driver: str


class TelemetrySample(TypedDict):
    seconds: float
    power_w: float | None
    gpu_busy_percent: float | None
    vram_mib: float | None
    graphics_clock_mhz: float | None
    memory_clock_mhz: float | None
    temperature_c: float | None


class TelemetryReport(TypedDict):
    power_w: Distribution | None
    gpu_busy_percent: Distribution | None
    vram_mib: Distribution | None
    graphics_clock_mhz: Distribution | None
    memory_clock_mhz: Distribution | None
    temperature_c: Distribution | None
    samples: int
    rejected_rows: int
    observed_span_seconds: float
    energy_j: float | None
    power_covered_seconds: float
    time_weighted_power_w: float | None


class FrameStreamRequired(TypedDict):
    file: str
    frametime_ms: Distribution | None


class FrameStream(FrameStreamRequired, total=False):
    # Malformed/oversized streams have an error instead of derived statistics.
    error: str
    rejected_rows: int
    log_interval_ms: int
    elapsed_seconds: float | None
    observed_fps: float | None
    frames_over_33ms: int
    frames_over_50ms: int
    interpretation: str


class CaptureMetadata(TypedDict):
    schema: int
    mode: str
    scenario: str
    reflex: str
    frame_domain: str
    warmup_seconds: float
    duration_seconds: float
    frame_log_interval_ms: int
    gpu_selector: str
    sample_interval_ms: int
    command_basename: str
    gpu: GpuIdentity | None
    telemetry_status: str
    capture_elapsed_seconds: float | None
    child_exit_code: int | None
    sampler_exit_code: int | None
    manual: bool
    stop_confirmed: bool
    recorded_csv: str | None


class ComparisonReport(TypedDict):
    schema: int
    mode: str
    scenario: str
    reflex: str
    frame_domain: str
    gpu: GpuIdentity | None
    warmup_seconds: float
    duration_seconds: float
    capture_elapsed_seconds: float | None
    telemetry_status: str
    frame_log_interval_ms: int
    sample_interval_ms: int | None
    telemetry: TelemetryReport
    frame_streams: list[FrameStream]
    activation: str
    input_latency_ms: None
    gpu_execution_ms: None
    limitations: list[str]
    manual: bool
    stop_confirmed: bool


SCHEMA = 1
FIELDS = ("power.draw", "utilization.gpu", "memory.used", "clocks.gr",
          "clocks.mem", "temperature.gpu")
METRICS = ("power_w", "gpu_busy_percent", "vram_mib", "graphics_clock_mhz",
           "memory_clock_mhz", "temperature_c")
LIMIT = 64 * 1024 * 1024


def number(value):
    if isinstance(value, bool):
        return None
    try:
        result = float(value)
        return result if math.isfinite(result) and result >= 0 else None
    except (ValueError, TypeError):
        return None


def distribution(values) -> Distribution | None:
    values = sorted(values)
    if not values:
        return None

    def percentile(fraction):
        position = (len(values) - 1) * fraction
        lo = int(position)
        hi = min(lo + 1, len(values) - 1)
        return values[lo] + (values[hi] - values[lo]) * (position - lo)

    return dict(samples=len(values), mean=statistics.fmean(values),
                p50=percentile(.5), p95=percentile(.95), p99=percentile(.99),
                maximum=values[-1])


def write_json(path, value):
    with Path(path).open("x", encoding="utf-8") as stream:
        os.chmod(path, 0o600)
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")


def environment(original, mode, folder, warmup, duration, interval, control=None):
    """Only the launched child receives mode/logging overrides."""
    env = original.copy()
    for key in ("FGVK_PACING_PROFILE", "FGVK_PACING_PROFILE_GPU",
                "FGVK_PACING_PROFILE_TRACE", "FGVK_PACING_PROFILE_CONTENT",
                "FGVK_PACING_PROFILE_WAYLAND"):
        env[key] = "0"
    env["NVPRESENT_ENABLE_SMOOTH_MOTION"] = "1" if mode == "smooth-motion" else "0"
    if mode == "smooth-motion":
        env.pop("DISABLE_LAYER_NV_PRESENT_1", None)
    else:
        # NVIDIA's disable gate tests presence, not the string's numeric value.
        env["DISABLE_LAYER_NV_PRESENT_1"] = "1"
    env["DISABLE_FGVK"] = "0" if mode == "fgvk" else "1"
    if mode == "fgvk":
        env["ENABLE_FGVK"] = "1"
    else:
        env.pop("ENABLE_FGVK", None)
        env.pop("ENABLE_FGVK_SPATIAL_SCALING", None)
        env["DISABLE_FGVK_SPATIAL_SCALING"] = "1"
        layers = env.get("VK_INSTANCE_LAYERS", "").split(":")
        layers = [name for name in layers if name and name not in
                  ("VK_LAYER_FGVK_render", "VK_LAYER_FGVK_spatial_scaling")]
        if layers:
            env["VK_INSTANCE_LAYERS"] = ":".join(layers)
        else:
            env.pop("VK_INSTANCE_LAYERS", None)
    # Do not insert/reorder layers or replace an existing MangoHud wrapper.
    config = env.get("MANGOHUD_CONFIG") or "read_cfg"
    env["MANGOHUD_CONFIG"] = config + (
        f",output_folder={folder},autostart_log={0 if control else warmup},log_duration={0 if control else duration}"
        f",log_interval={interval},permit_upload=0,upload_logs=0")
    if control:
        env["MANGOHUD_CONFIG"] += f",control={control},toggle_logging=Shift_L+F2"
    return env


class RecordingControl:
    """Observe MangoHud's own recording key; never read keyboard/input devices."""

    def __init__(self, folder, name):
        self.folder = folder
        self.name = name
        self.connection = None
        self.peer_pid = None
        self.buffer = b""
        self.started = None
        self.stopped = None
        self.csv = None
        self.confirmed = False
        self.version_confirmed = False
        self.message = "Waiting for game — Left Shift + F2 starts/stops recording"

    def request(self, active):
        if self.connection is None or not self.version_confirmed:
            raise ValueError("MangoHud is not connected yet")
        self.connection.sendall(b":logging=1;" if active else b":logging=0;")

    def poll(self):
        if self.stopped is not None:
            return
        if self.connection is None:
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            connection.settimeout(.02)
            try:
                connection.connect("\0" + self.name)
                self.peer_pid, uid, _gid = struct.unpack("3i", connection.getsockopt(
                    socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i")))
                if uid != os.getuid():
                    raise ValueError("MangoHud control peer has a different owner")
                self.connection = connection
                self.buffer = b""
                self.version_confirmed = False
                self.message = "Checking MangoHud recording control"
            except (OSError, ValueError) as error:
                connection.close()
                if isinstance(error, ValueError):
                    self.message = str(error)
                    self.stopped = time.monotonic()
                return
        try:
            data = self.connection.recv(4096)
            if not data:
                self.connection.close()
                self.connection = None
                if self.started is not None:
                    self.stopped = time.monotonic()
                self.message = "MangoHud disconnected; recording completion unconfirmed"
                return
            self.buffer = (self.buffer + data)[-8192:]
        except (socket.timeout, BlockingIOError):
            pass
        except OSError:
            self.connection.close()
            self.connection = None
            if self.started is not None:
                self.stopped = time.monotonic()
            self.message = "MangoHud connection failed; recording completion unconfirmed"
            return
        version_prefix = b":MangoHudControlVersion="
        if version_prefix in self.buffer:
            tail = self.buffer.split(version_prefix, 1)[1]
            if b";" in tail:
                if tail.split(b";", 1)[0] != b"1":
                    self.message = "Unsupported MangoHud recording-control version"
                    self.stopped = time.monotonic()
                    return
                self.version_confirmed = True
                if self.started is None:
                    self.message = "Ready — Left Shift + F2 to record"
        if not self.version_confirmed:
            return
        if b":LoggingFinished" in self.buffer:
            self.stopped = time.monotonic()
            self.confirmed = self.started is not None
            self.message = "Recording saved" if self.confirmed else "Recording ended before a frame was observed"
            return
        if self.started is None:
            candidates = [path for path in self.folder.glob("*.csv")
                          if not path.name.endswith("_summary.csv") and not path.is_symlink()
                          and path.is_file() and path.stat().st_size > 0]
            if not candidates:
                return
            # Match the log to this control peer, not an unrelated launcher/HUD.
            try:
                opened = {os.readlink(fd) for fd in Path(f"/proc/{self.peer_pid}/fd").iterdir()}
            except OSError:
                self.message = "Cannot verify the recording process; GPU capture has not started"
                return
            matches = [path for path in candidates if str(path) in opened]
            if len(matches) != 1:
                self.message = "Waiting for one recording from the connected MangoHud process"
                return
            self.csv = matches[0].name
            self.started = time.monotonic()
            self.message = "RECORDING — Left Shift + F2 to stop"

    def close(self):
        if self.connection:
            self.connection.close()


class RecordingButtons:
    """Optional small controller; the in-game key works without a GUI."""

    def __init__(self, control):
        import tkinter as tk
        self.root = tk.Tk()
        self.root.title("framegen-vulkan recording")
        self.control = control
        self.status = tk.StringVar(value=control.message)
        tk.Label(self.root, textvariable=self.status, padx=18, pady=14).pack()
        self.start = tk.Button(self.root, text="Start recording", command=lambda: self.request(True))
        self.start.pack(fill="x", padx=18, pady=4)
        self.stop = tk.Button(self.root, text="Stop recording", command=lambda: self.request(False))
        self.stop.pack(fill="x", padx=18, pady=4)
        tk.Label(self.root, text="In game: Left Shift + F2", padx=18, pady=10).pack()
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def request(self, active):
        try:
            self.control.request(active)
        except (OSError, ValueError) as error:
            self.status.set(str(error))

    def poll(self):
        if self.root is None:
            return
        self.status.set(self.control.message)
        self.start.configure(state="normal" if self.control.version_confirmed and self.control.started is None else "disabled")
        self.stop.configure(state="normal" if self.control.started and self.control.stopped is None else "disabled")
        self.root.update()

    def close(self):
        if self.root:
            self.root.destroy()
            self.root = None


def read_frames(path, interval) -> FrameStream:
    """Find the named metric header; system metadata and summaries are not data."""
    if path.stat().st_size > LIMIT:
        return dict(file=path.name, error="CSV exceeds 64 MiB", frametime_ms=None)
    values = []
    elapsed = []
    rejected = 0
    header = None
    with path.open(encoding="utf-8", errors="replace", newline="") as stream:
        for row in csv.reader(stream):
            row = [cell.strip() for cell in row]
            if "frametime" in row and "elapsed" in row and "fps" in row:
                if header is not None:
                    return dict(file=path.name, error="repeated CSV header", frametime_ms=None)
                header = row
                continue
            if header is None:
                continue
            if len(row) != len(header):
                rejected += 1
                continue
            item = dict(zip(header, row))
            frame, timestamp = number(item["frametime"]), number(item["elapsed"])
            if (frame is None or frame <= 0 or timestamp is None or
                    (elapsed and timestamp <= elapsed[-1])):
                rejected += 1
                continue
            values.append(frame)
            elapsed.append(timestamp)
    stats = distribution(values)
    # Interval logging samples a single frame, not every frame in the interval.
    return dict(file=path.name, frametime_ms=stats, rejected_rows=rejected,
                log_interval_ms=interval, elapsed_seconds=(elapsed[-1] - elapsed[0]) / 1e9
                if len(elapsed) > 1 else None,
                observed_fps=1000 / stats["mean"] if stats and interval == 0 else None,
                frames_over_33ms=sum(value > 1000 / 30 for value in values),
                frames_over_50ms=sum(value > 50 for value in values),
                interpretation="MangoHud-observed calls; source/output domain is not inferred")


def telemetry_report(path, warmup, duration) -> TelemetryReport:
    samples = []
    rejected = 0
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            try:
                item = json.loads(line)
                stamp = number(item.get("seconds"))
                if stamp is None:
                    raise ValueError("missing timestamp")
                if warmup <= stamp < warmup + duration:
                    samples.append(item)
            except (ValueError, TypeError, AttributeError):
                rejected += 1
    result: TelemetryReport = {key: distribution([value for row in samples
                                if (value := number(row.get(key))) is not None])
              for key in METRICS}
    result["samples"] = len(samples)
    result["rejected_rows"] = rejected
    result["observed_span_seconds"] = samples[-1]["seconds"] - samples[0]["seconds"] if len(samples) > 1 else 0
    # Trapezoidal energy only covers adjacent valid power readings. Gaps are not zero.
    energy, covered = 0., 0.
    for first, second in zip(samples, samples[1:]):
        a, b = number(first.get("power_w")), number(second.get("power_w"))
        delta = second["seconds"] - first["seconds"]
        if a is not None and b is not None and 0 < delta <= 2:
            energy += (a + b) / 2 * delta
            covered += delta
    result["energy_j"] = energy if covered else None
    result["power_covered_seconds"] = covered
    result["time_weighted_power_w"] = energy / covered if covered else None
    return result


def summarize(folder, metadata: CaptureMetadata) -> ComparisonReport:
    frame_folder = folder / ("recorded" if metadata.get("manual") else "mangohud")
    frames = [read_frames(path, metadata["frame_log_interval_ms"])
              for path in sorted(frame_folder.glob("*.csv"))
              if not path.name.endswith("_summary.csv") and not path.is_symlink() and path.is_file()
              and (not metadata.get("manual") or path.name == metadata.get("recorded_csv"))]
    return dict(schema=SCHEMA, mode=metadata["mode"], scenario=metadata["scenario"],
                reflex=metadata["reflex"], frame_domain=metadata["frame_domain"],
                gpu=metadata.get("gpu"), warmup_seconds=metadata["warmup_seconds"],
                duration_seconds=metadata["duration_seconds"],
                capture_elapsed_seconds=metadata.get("capture_elapsed_seconds"),
                telemetry_status=metadata.get("telemetry_status", "unknown"),
                frame_log_interval_ms=metadata["frame_log_interval_ms"],
                sample_interval_ms=metadata.get("sample_interval_ms"),
                telemetry=telemetry_report(folder / "telemetry.jsonl", metadata["warmup_seconds"],
                                           metadata["duration_seconds"]),
                frame_streams=frames, activation="requested, not independently verified",
                manual=metadata.get("manual", False), stop_confirmed=metadata.get("stop_confirmed", False),
                input_latency_ms=None, gpu_execution_ms=None,
                limitations=["Board telemetry includes other processes and compositor work.",
                             "Frame time is not input latency or GPU execution duration.",
                             ("Manual boundaries are host observations of the connected MangoHud log and completion event, not exact GPU boundaries."
                              if metadata.get("manual") else "MangoHud logging starts relative to its initialization; telemetry starts at launcher entry. Windows are not exactly aligned."),
                             "Source/output layer position must be verified separately; streams are never merged.",
                             "Per-frame CSV writes and GPU polling add unqualified measurement overhead."])


def format_report(report: ComparisonReport):
    lines = [f"{report['mode']} — {report['scenario']} (Reflex: {report['reflex']})",
             f"Activation: {report['activation']}"]
    elapsed = report["capture_elapsed_seconds"]
    if report.get("manual") and not report.get("stop_confirmed"):
        lines.append("Manual recording incomplete: no confirmed MangoHud stop was observed.")
    elif not report.get("manual") and elapsed is not None and elapsed < report["warmup_seconds"] + report["duration_seconds"]:
        lines.append("Partial capture: launcher exited before the requested GPU window completed.")
    for key in METRICS:
        stats = report["telemetry"][key]
        lines.append(f"{key}: mean {stats['mean']:.3f}, p95 {stats['p95']:.3f} ({stats['samples']} samples)"
                     if stats else f"{key}: unavailable")
    for item in report["frame_streams"]:
        stats = item["frametime_ms"]
        if not stats:
            lines.append(f"{item['file']}: no usable frame data")
            continue
        fps = item["observed_fps"]
        lines.append(f"{item['file']}: {stats['samples']} samples, frame ms p50/p95/p99 "
                     f"{stats['p50']:.3f}/{stats['p95']:.3f}/{stats['p99']:.3f}, "
                     + (f"observed FPS {fps:.2f}" if fps else "interval-sampled; FPS unavailable"))
    if not report["frame_streams"]:
        lines.append("Frame data unavailable: no MangoHud CSV; preserve/enable your MangoHud wrapper.")
    lines += ["Input latency: unavailable. GPU execution time: unavailable.", *report["limitations"]]
    return "\n".join(lines) + "\n"


def save_report(folder, metadata):
    report = summarize(folder, metadata)
    write_json(folder / "report.json", report)
    with (folder / "report.txt").open("x") as stream:
        os.chmod(stream.name, 0o600)
        stream.write(format_report(report))
    print(format_report(report), flush=True)


def capture(args):
    if args.mode != "fgvk" and any(Path(word).name in ("fgvk", "fgvk-launch") for word in args.command):
        raise ValueError("baseline/Smooth Motion need the game command with MangoHud directly; "
                         "framegen-vulkan launchers isolate layers and can suppress MangoHud/NVIDIA Smooth Motion")
    if args.mode == "smooth-motion" and os.environ.get("VK_IMPLICIT_LAYER_PATH"):
        raise ValueError("VK_IMPLICIT_LAYER_PATH overrides system layer discovery; launch this capture "
                         "outside the framegen-vulkan launcher so NVIDIA Smooth Motion remains discoverable")
    folder = args.output.expanduser().absolute()
    if any(character in str(folder) for character in (",", "\n", "\r", ":")):
        raise ValueError("output path cannot contain MangoHud option separators")
    if args.manual:
        if folder.is_symlink():
            raise ValueError("capture directory cannot be a symlink")
        folder.mkdir(mode=0o700, parents=True, exist_ok=True)
        folder = folder / (time.strftime("session-%Y%m%d-%H%M%S-") + str(time.monotonic_ns()))
    folder.mkdir(mode=0o700, parents=True, exist_ok=False)
    (folder / "mangohud").mkdir(mode=0o700)
    metadata: CaptureMetadata = dict(schema=SCHEMA, mode=args.mode, scenario=args.scenario,
                    reflex=args.reflex, frame_domain=args.frame_domain,
                    warmup_seconds=args.warmup, duration_seconds=args.duration,
                    frame_log_interval_ms=args.frame_log_interval, gpu_selector=args.gpu,
                    sample_interval_ms=args.sample_interval,
                    command_basename=Path(args.command[0]).name,
                    # Arguments/environment may contain secrets; do not store them.
                    gpu=None, telemetry_status="not started", capture_elapsed_seconds=None,
                    child_exit_code=None, sampler_exit_code=None, manual=args.manual,
                    stop_confirmed=False, recorded_csv=None)
    control = RecordingControl(folder / "mangohud", f"fgvk-fg-{os.getpid()}-{time.monotonic_ns()}") if args.manual else None
    env = environment(os.environ, args.mode, folder / "mangohud", args.warmup,
                      args.duration, args.frame_log_interval, control.name if control else None)
    started = time.monotonic()
    sampler = None
    child = None
    previous_handlers = {}
    selector = selectors.DefaultSelector()
    buttons = None
    report_saved = False
    if args.buttons:
        try:
            buttons = RecordingButtons(control)
        except Exception as error:
            print(f"framegen-vulkan: recording window unavailable ({error}); use Left Shift + F2 in game.", file=sys.stderr)
    print(f"framegen-vulkan: comparison capture: {folder}", file=sys.stderr, flush=True)
    print("framegen-vulkan: Left Shift + F2 starts/stops recording. No countdown." if control else
          "framegen-vulkan: GPU sampling stops after warmup + duration; the game is not stopped.", file=sys.stderr)
    try:
        with (folder / "telemetry.jsonl").open("x") as raw, (folder / "sampler-errors.log").open("x") as errors:
            os.chmod(raw.name, 0o600)
            os.chmod(errors.name, 0o600)
            binary = shutil.which("nvidia-smi")

            def launch_sampler():
                nonlocal sampler
                if not binary:
                    metadata["telemetry_status"] = "nvidia-smi not found"
                    return
                try:
                    sampler = subprocess.Popen([binary, "-i", args.gpu,
                        "--query-gpu=uuid,name,driver_version," + ",".join(FIELDS),
                        "--format=csv,noheader,nounits", f"--loop-ms={args.sample_interval}"],
                        stdout=subprocess.PIPE, stderr=errors, start_new_session=True)
                    os.set_blocking(sampler.stdout.fileno(), False)
                    selector.register(sampler.stdout, selectors.EVENT_READ)
                    metadata["telemetry_status"] = "started"
                except OSError as error:
                    metadata["telemetry_status"] = str(error)
            if not control:
                launch_sampler()
            child = subprocess.Popen(args.command, env=env, start_new_session=True)

            def forward(signum, _frame):
                if child.poll() is None:
                    try:
                        os.killpg(child.pid, signum)
                    except ProcessLookupError:
                        pass

            for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
                previous_handlers[signum] = signal.signal(signum, forward)
            pending = b""
            manual_started = False
            stop_requested = None
            previous_message = None
            while child.poll() is None:
                if control:
                    control.poll()
                    if control.started is not None and not manual_started:
                        manual_started = True
                        metadata["warmup_seconds"] = control.started - started
                        metadata["recorded_csv"] = control.csv
                        launch_sampler()
                    if buttons:
                        buttons.poll()
                    if control.message != previous_message:
                        print(f"framegen-vulkan: {control.message}", file=sys.stderr, flush=True)
                        previous_message = control.message
                    if control.stopped is not None:
                        break
                    if control.started and time.monotonic() - control.started >= 600:
                        if stop_requested is None:
                            try:
                                control.request(False)
                            except OSError:
                                pass
                            stop_requested = time.monotonic()
                        elif time.monotonic() - stop_requested > 3:
                            control.stopped = time.monotonic()
                            break
                elif time.monotonic() - started >= args.warmup + args.duration:
                    break
                if not selector.get_map():
                    time.sleep(.1)
                for key, _ in selector.select(.1):
                    chunk = os.read(key.fd, 65536)
                    if not chunk:
                        selector.unregister(key.fileobj)
                        continue
                    pending += chunk
                    if len(pending) > 65536:
                        raise ValueError("unbounded nvidia-smi output")
                    while b"\n" in pending:
                        line, pending = pending.split(b"\n", 1)
                        row = next(csv.reader(io.StringIO(line.decode("utf-8", errors="replace"))), [])
                        if len(row) != 3 + len(METRICS):
                            if errors.tell() < 65536:
                                errors.write(line.decode("utf-8", errors="replace") + "\n")
                            continue
                        identity: GpuIdentity = dict(zip(("uuid", "name", "driver"), (cell.strip() for cell in row[:3])))
                        if metadata["gpu"] is not None and identity != metadata["gpu"]:
                            raise ValueError("GPU selector returned multiple devices; select one GPU")
                        metadata["gpu"] = identity
                        item: TelemetrySample = dict(seconds=time.monotonic() - started,
                                    **dict(zip(METRICS, map(number, row[3:]))))
                        raw.write(json.dumps(item, allow_nan=False) + "\n")
            metadata["capture_elapsed_seconds"] = time.monotonic() - started
            if control:
                metadata["duration_seconds"] = max(0, (control.stopped or time.monotonic()) - control.started) if control.started else 0
                metadata["stop_confirmed"] = control.confirmed
            if sampler:
                if sampler.poll() is None:
                    sampler.terminate()
                try:
                    metadata["sampler_exit_code"] = sampler.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    sampler.kill()
                    metadata["sampler_exit_code"] = sampler.wait()
            if metadata["gpu"] is None:
                metadata["telemetry_status"] = "unavailable; see sampler-errors.log"
            if control:
                control.close()
                if buttons:
                    buttons.close()
                if control.csv and control.confirmed:
                    source = folder / "mangohud" / control.csv
                    if source.is_file() and not source.is_symlink() and source.stat().st_size <= LIMIT:
                        snapshot = folder / "recorded"
                        snapshot.mkdir(mode=0o700)
                        # Freeze the stopped log before any later hotkey recording.
                        with source.open("rb") as stream, (snapshot / control.csv).open("xb") as target:
                            os.chmod(target.name, 0o600)
                            shutil.copyfileobj(stream, target)
                raw.flush()
                save_report(folder, metadata)
                report_saved = True
            print("framegen-vulkan: recording report saved; game stays open." if control else
                  "framegen-vulkan: GPU capture finished; exit the game to finalize its CSV/report.", file=sys.stderr, flush=True)
            metadata["child_exit_code"] = child.wait()
    finally:
        if control:
            control.close()
        if buttons:
            buttons.close()
        selector.close()
        if sampler:
            if sampler.poll() is None:
                sampler.kill()
                sampler.wait()
            sampler.stdout.close()
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)
        write_json(folder / "metadata.json", metadata)
    if not report_saved:
        save_report(folder, metadata)
    result = metadata["child_exit_code"]
    return result if result >= 0 else 128 - result


def compare(paths):
    reports = []
    for path in paths:
        sessions = sorted(item for item in path.glob("session-*/report.json") if not item.is_symlink())
        selected = sessions[-1] if sessions else path / "report.json"
        print(f"Report: {selected}")
        with selected.open() as stream:
            report = json.load(stream)
        if report.get("schema") != SCHEMA:
            raise ValueError("unsupported comparison report schema")
        reports.append(report)
    print("Run | Board power W | GPU busy % | VRAM MiB | Frame domain")
    for report in reports:
        def mean(key):
            stats = report["telemetry"][key]
            return f"{stats['mean']:.2f}" if stats else "unavailable"
        print(f"{report['mode']} | {mean('power_w')} | {mean('gpu_busy_percent')} | "
              f"{mean('vram_mib')} | {report['frame_domain']}")
    matched = True
    keys = ["scenario", "reflex", "gpu", "frame_log_interval_ms", "sample_interval_ms"]
    if not all(report.get("manual") for report in reports):
        keys += ["warmup_seconds", "duration_seconds"]
    for key in keys:
        if any(report[key] != reports[0][key] for report in reports[1:]):
            matched = False
            print(f"WARNING: {key} differs; this is not a matched comparison.")
    if any(report["frame_domain"] == "unknown" for report in reports):
        print("WARNING: frame domain unverified; FPS cannot establish source-throughput loss.")
    elif any(report["frame_domain"] != reports[0]["frame_domain"] for report in reports[1:]):
        print("WARNING: source/output frame domains differ; FPS values are not directly comparable.")
    if any(report["gpu"] is None for report in reports):
        matched = False
        print("WARNING: GPU identity/telemetry is unavailable in at least one run.")
    for report in reports:
        if report.get("manual") and not report.get("stop_confirmed"):
            matched = False
            print(f"WARNING: {report['mode']} recording has no confirmed stop; deltas withheld.")
        span = report["telemetry"]["observed_span_seconds"]
        if span < .8 * report["duration_seconds"]:
            matched = False
            print(f"WARNING: {report['mode']} has less than 80% telemetry coverage; deltas withheld.")
    baselines = [report for report in reports if report["mode"] == "baseline"]
    if matched and len(baselines) == 1:
        baseline = baselines[0]
        for report in reports:
            if report is baseline:
                continue
            for key in ("power_w", "gpu_busy_percent", "vram_mib"):
                first, second = baseline["telemetry"][key], report["telemetry"][key]
                if first and second:
                    print(f"{report['mode']} {key} delta vs baseline: {second['mean'] - first['mean']:+.3f} "
                          "(board observation, not isolated FG cost)")
    print("\nFrame streams (never combined or assumed to measure source FPS):")
    for report in reports:
        for item in report["frame_streams"]:
            stats = item["frametime_ms"]
            if stats:
                print(f"{report['mode']} / {item['file']}: p99 {stats['p99']:.3f} ms; "
                      f"observed FPS {item['observed_fps']}; domain {report['frame_domain']}")
    print("\nNo input-latency, GPU-ms or FG-only cost claim is possible from these measurements.")
    print("Activation is requested only. Verify FG, the same scene/settings and layer position in each run.")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    record = commands.add_parser("capture", help="prefix your existing game launch command")
    record.add_argument("--mode", choices=("baseline", "fgvk", "smooth-motion"), required=True)
    record.add_argument("--output", type=Path, required=True, help="private output directory; --manual adds a new session beneath it")
    record.add_argument("--manual", action="store_true", help="Left Shift + F2 starts/stops recording; no countdown")
    record.add_argument("--buttons", action="store_true", help="also show a Start/Stop window (requires --manual and tkinter)")
    record.add_argument("--scenario", required=True, help="same scene/settings label across runs")
    record.add_argument("--reflex", choices=("on", "off", "unknown"), default="unknown", help="recorded label, not a setting override")
    record.add_argument("--frame-domain", choices=("source", "output", "unknown"), default="unknown", help="only label source/output after independently verifying layer position")
    record.add_argument("--gpu", default="0", help="one nvidia-smi device index or UUID")
    record.add_argument("--warmup", type=int, default=30)
    record.add_argument("--duration", type=int, default=60)
    record.add_argument("--sample-interval", type=int, default=500, help="GPU polling interval in ms")
    record.add_argument("--frame-log-interval", type=int, default=0, help="0 logs every observed frame; positive ms samples frame times")
    record.add_argument("command", nargs=argparse.REMAINDER)
    comparison = commands.add_parser("compare", help="compare existing capture directories")
    comparison.add_argument("runs", nargs="+", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "compare":
            return compare(args.runs)
        if args.command[:1] == ["--"]:
            args.command = args.command[1:]
        if not args.command:
            parser.error("a launch command after -- is required")
        if args.buttons and not args.manual:
            parser.error("--buttons requires --manual")
        if not args.gpu.strip() or "," in args.gpu:
            parser.error("--gpu must select one index or UUID, not a list")
        if not (1 <= args.warmup <= 600 and 1 <= args.duration <= 600 and
                100 <= args.sample_interval <= 5000 and 0 <= args.frame_log_interval <= 1000):
            parser.error("warmup/duration: 1–600 s; GPU interval: 100–5000 ms; frame interval: 0–1000 ms")
        return capture(args)
    except (OSError, ValueError, KeyError) as error:
        print(f"framegen-vulkan: comparison profiler: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

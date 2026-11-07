#!/usr/bin/env python3
"""Capture selected Wayland messages and correlate commits with MAKO feedback."""

import argparse
from collections import defaultdict
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time


LIMIT = 64 * 1024 * 1024
ANSI = re.compile(rb"\x1b\[[0-9;]*m")
MESSAGE = re.compile(
    r"^\[([0-9:. ]+)\]\s+.*?(->\s*)?([a-z][a-z0-9_]+)[@#](\d+)\.([a-z0-9_]+)\((.*)\)$")
ALLOWED = {
    "wl_surface": {"attach", "commit"},
    "wl_buffer": {"release"},
    "wp_presentation": {"feedback", "clock_id"},
    "wp_presentation_feedback": {"sync_output", "presented", "discarded"},
    "wp_linux_drm_syncobj_surface_v1": {"set_acquire_point", "set_release_point"},
    "zwp_linux_surface_synchronization_v1": {"set_acquire_fence"},
    "wp_commit_timer_v1": {"set_timestamp"},
    "wp_fifo_v1": {"set_barrier", "wait_barrier"},
}


def message(line):
    """Ignore all unrelated stderr, input events, titles and protocol strings."""
    clean = ANSI.sub(b"", line).decode("ascii", errors="replace").strip()
    match = MESSAGE.fullmatch(clean)
    if not match or match[5] not in ALLOWED.get(match[3], set()):
        return None
    # These messages contain only numeric/object arguments. Do not retain an
    # arbitrary quoted string even if a malformed line uses an allowed name.
    if not re.fullmatch(r"[a-zA-Z0-9_@#., :+\-]*", match[6]):
        return None
    return clean, match


def stamp_ns(stamp):
    if ":" in stamp:
        hours, minutes, seconds = stamp.split(":")
        whole, fraction = seconds.split(".")
        return ((int(hours) * 3600 + int(minutes) * 60 + int(whole)) * 10**9
                + int(fraction.ljust(9, "0"))), 86400 * 10**9
    whole, fraction = stamp.strip().split(".")
    return int(whole) * 10**6 + int(fraction.ljust(6, "0")), 2**32 * 1000


def read_protocol(path):
    """Pair request lifetimes with the next commit, then its actual feedback.

    No cross-clock absolute subtraction: commit/call/presentation intervals
    each stay in their own clock. Multiple feedback objects on one commit are
    deduplicated; conflicting associations are unavailable evidence.
    """
    pending = {}
    waiting = defaultdict(list)
    buffers = {}
    serials = defaultdict(int)
    attachments = defaultdict(int)
    committed_attachments = defaultdict(int)
    clocks = {}
    presented = defaultdict(list)
    invalid = 0
    epoch = 0
    previous = None
    offsets = []
    limited = False
    with Path(path).open("rb") as stream:
        for raw in stream:
            if raw.startswith(b"MAKO Renderer: wayland protocol clock "):
                fields = dict(re.findall(rb"([a-z_]+)=(\d+)", raw))
                try:
                    before, after, realtime = (int(fields[k]) for k in
                        (b"monotonic_before_ns", b"monotonic_after_ns", b"realtime_ns"))
                    if after < before:
                        raise ValueError("backwards calibration")
                    offsets.append((realtime - after, realtime - before))
                except (KeyError, ValueError):
                    invalid += 1
                continue
            if raw.startswith(b"MAKO Renderer: wayland protocol capture-limit"):
                limited = True
            parsed = message(raw)
            if not parsed:
                continue
            _, m = parsed
            try:
                stamp, modulus = stamp_ns(m[1])
                if previous is not None and stamp + epoch < previous - modulus // 2:
                    epoch += modulus
                timestamp = stamp + epoch
                # Concurrent stderr writers can print timestamps out of order.
                # Preserve file order, but reject backwards per-pair intervals.
                previous = max(previous or 0, timestamp)
                sent, interface, proxy, method, args = bool(m[2]), m[3], int(m[4]), m[5], m[6]
                if interface == "wp_presentation" and method == "clock_id" and not sent:
                    clocks[proxy] = int(args)
                elif interface == "wp_presentation" and method == "feedback" and sent:
                    objects = re.fullmatch(r"wl_surface[@#](\d+),\s*new id wp_presentation_feedback[@#](\d+)", args)
                    if not objects:
                        raise ValueError("malformed feedback request")
                    surface, feedback = map(int, objects.groups())
                    if feedback in pending:
                        # Several displays/processes share an unlabelled stderr
                        # stream, or a required terminal record is missing.
                        pending[feedback]["conflict"] = True
                        invalid += 1
                        continue
                    item = dict(surface=surface, clock=clocks.get(proxy), commit=None, output=0, conflict=False)
                    pending[feedback] = item
                    waiting[surface].append(item)
                elif interface == "wl_surface" and method == "attach" and sent:
                    buffer = re.match(r"(?:wl_buffer[@#](\d+)|nil|null)(?:,|$)", args)
                    if not buffer:
                        raise ValueError("malformed buffer attachment")
                    buffers[proxy] = int(buffer[1] or 0)
                    attachments[proxy] += 1
                elif interface == "wl_surface" and method == "commit" and sent:
                    serials[proxy] += 1
                    commit = dict(surface=proxy, serial=serials[proxy], timestamp=timestamp, buffer=buffers.get(proxy, 0),
                                  attachment_serial=attachments[proxy],
                                  new_attachment=attachments[proxy] != committed_attachments[proxy])
                    committed_attachments[proxy] = attachments[proxy]
                    for item in waiting.pop(proxy, []):
                        item["commit"] = commit
                elif interface == "wp_presentation_feedback" and not sent:
                    item = pending.get(proxy)
                    if not item:
                        continue
                    if method == "sync_output":
                        output = re.fullmatch(r"wl_output[@#](\d+)", args)
                        if not output:
                            raise ValueError("malformed sync output")
                        item["output"] = int(output[1])
                    elif method in ("presented", "discarded"):
                        pending.pop(proxy)
                        if method == "presented" and not item["conflict"] and item["commit"]:
                            values = [int(v.strip()) for v in args.split(",")]
                            if len(values) != 7 or any(v < 0 or v > 0xffffffff for v in values) or values[2] >= 10**9:
                                raise ValueError("malformed presentation timestamp")
                            hi, lo, ns, refresh, seqhi, seqlo, flags = values
                            key = (item["clock"], ((hi << 32) | lo) * 10**9 + ns,
                                   (seqhi << 32) | seqlo, refresh, flags, item["output"])
                            presented[key].append(item["commit"])
            except (ValueError, OverflowError):
                invalid += 1
    clock_step = len(offsets) > 1 and max(lo for lo, _ in offsets) - min(hi for _, hi in offsets) > 1_000_000
    return presented, invalid, clock_step, limited


def protocol_report(path, observations, top=5):
    presented, invalid, clock_step, limited = read_protocol(path)
    lines = ["Wayland commit correlation (protocol marshal timestamps, not socket delivery or pixels):"]
    if invalid:
        lines.append(f"  Rejected malformed/conflicting protocol records: {invalid}; coverage is partial.")
    if limited:
        lines.append("  Protocol capture reached its size limit; later commits are unavailable.")
    if clock_step:
        lines.append("  Realtime clock changed during capture; commit interval comparisons are unavailable.")
        return "\n".join(lines)
    requests, calls, terminals = defaultdict(list), defaultdict(list), defaultdict(list)
    for r in observations.get("wayland", []):
        key = (r["pid"], r["surface_token"], r["feedback"])
        if r["event"] == "request":
            requests[key].append(r)
        elif r["event"] == "call":
            calls[key].append(r)
        elif r["event"] in ("presented", "discarded", "abandoned"):
            terminals[key].append(r)
    groups = defaultdict(list)
    missing = ambiguous = 0
    for key, rr in requests.items():
        tt, cc = terminals[key], calls[key]
        if len(rr) != 1 or len(tt) != 1 or len(cc) != 1 or tt[0]["event"] != "presented":
            missing += 1
            continue
        r, t, c = rr[0], tt[0], cc[0]
        candidates = presented.get(tuple(t[k] for k in
            ("clock_id", "presented_ns", "sequence", "refresh_ns", "flags", "output_id")), [])
        unique = {(x["surface"], x["serial"]): x for x in candidates}
        if len(unique) != 1:
            if unique:
                ambiguous += 1
            else:
                missing += 1
            continue
        commit = next(iter(unique.values()))
        groups[key[:2], t["clock_id"], t["output_id"], commit["surface"]].append((r, c, t, commit))
    lines.append(f"  Matched outputs={sum(map(len, groups.values()))}, unavailable={missing}, ambiguous={ambiguous}.")
    for group, records in groups.items():
        records.sort(key=lambda x: x[0]["feedback"])
        pairs = []
        attachment_pairs = []
        for a, b in zip(records, records[1:]):
            if b[0]["feedback"] != a[0]["feedback"] + 1:
                continue
            gaps = ((b[1]["requested_ns"] - a[1]["requested_ns"]) / 1e6,
                    (b[3]["timestamp"] - a[3]["timestamp"]) / 1e6,
                    (b[2]["presented_ns"] - a[2]["presented_ns"]) / 1e6)
            if min(gaps) < 0:
                continue
            if b[3]["serial"] == a[3]["serial"] + 1:
                pairs.append((gaps, a, b))
            # Some WSI drivers commit FIFO state again after the content commit.
            # Keep exact feedback-to-commit joins and require consecutive actual
            # attach requests, including reattachments of the same buffer ID.
            # Any intervening attachment (even null), conflict, or source-context
            # transition prevents this supplementary comparison.
            if (not invalid and a[3]["new_attachment"] and b[3]["new_attachment"] and
                    a[3]["buffer"] and b[3]["buffer"] and
                    b[3]["serial"] > a[3]["serial"] and
                    b[3]["attachment_serial"] == a[3]["attachment_serial"] + 1 and
                    a[0].get("context", 0) != 0 and
                    a[0].get("context") == b[0].get("context")):
                attachment_pairs.append((gaps, a, b))
        lines.append(f"  PID/surface/clock/output/protocol-surface={group}: {len(pairs)} adjacent matched pairs.")
        for index, name in enumerate(("Lower-call", "Wayland commit", "Compositor presentation")):
            values = sorted(p[0][index] for p in pairs)
            if values:
                selected = [values[round((len(values) - 1) * p)] for p in (0, .5, .95, .99, 1)]
                lines.append(f"    {name} gaps min/p50/p95/p99/max (ms): " + "/".join(f"{x:.3f}" for x in selected))
        rank = lambda p: max(abs(p[0][1] - p[0][0]), abs(p[0][2] - p[0][1]))
        for gaps, a, b in sorted(pairs, key=rank, reverse=True)[:top]:
            role = lambda x: "generated" if x[0]["generated"] else "real"
            lines.append(f"    Feedback {a[0]['feedback']}->{b[0]['feedback']} ({role(a)}->{role(b)}), "
                         f"buffers {a[3]['buffer']}->{b[3]['buffer']}: call/commit/presentation gaps "
                         + "/".join(f"{x:.3f}" for x in gaps) + " ms.")
        intervening = sum(b[3]["serial"] > a[3]["serial"] + 1 for _, a, b in attachment_pairs)
        lines.append(f"    Consecutive buffer-attachment matched pairs: {len(attachment_pairs)}; "
                     f"with intervening commits without a new attachment: {intervening}.")
        for index, name in enumerate(("Lower-call", "Buffer commit", "Compositor presentation")):
            roles = []
            for generated, label in ((1, "generated"), (0, "real")):
                values = sorted(gaps[index] for gaps, _, b in attachment_pairs if b[0]["generated"] == generated)
                if values:
                    middle = len(values)//2
                    median = (values[middle] + values[(len(values)-1)//2])/2
                    roles.append(f"before {label}={median:.3f} ms (n={len(values)})")
            if roles:
                lines.append(f"    {name} attachment-pair median: " + "; ".join(roles))
        for gaps, a, b in sorted(attachment_pairs, key=rank, reverse=True)[:top]:
            lines.append(f"    Attachment feedback {a[0]['feedback']}->{b[0]['feedback']}, "
                         f"commit serials {a[3]['serial']}->{b[3]['serial']}: call/commit/presentation gaps "
                         + "/".join(f"{x:.3f}" for x in gaps) + " ms.")
    lines.append("  Equal presentation outcomes on several feedback objects qualify one commit only. "
                 "Strict adjacent-commit pairs never bridge extra commits; the separate attachment comparison "
                 "permits only commits without a new attachment between exactly consecutive attachment requests. "
                 "Protocol logging can perturb timing; image progression remains unmeasured.")
    return "\n".join(lines)


def clock_record():
    before = time.monotonic_ns()
    realtime = time.time_ns()
    after = time.monotonic_ns()
    return (f"MAKO Renderer: wayland protocol clock monotonic_before_ns={before} "
            f"realtime_ns={realtime} monotonic_after_ns={after}\n").encode("ascii")


def capture(command, path=None, limit=LIMIT, frame_trace=False, runtime_log=None):
    if path is None:
        fd, name = tempfile.mkstemp(prefix=f"mako-wayland-protocol-{os.getuid()}-", suffix=".log")
    else:
        name = str(Path(path).absolute())
        fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    written = 0
    stopped = False
    runtime_fd = None
    runtime_bytes = 0
    runtime_stopped = False
    def save_runtime(data):
        nonlocal runtime_bytes, runtime_stopped
        if runtime_fd is None or runtime_stopped:
            return
        marker = b"MAKO Renderer: runtime capture-limit; beginning preserved\n"
        if runtime_bytes + len(data) + len(marker) > 4 * 1024 * 1024:
            data = marker
            runtime_stopped = True
        try:
            remaining = memoryview(data)
            while remaining:
                count = os.write(runtime_fd, remaining)
                if count <= 0:
                    raise OSError("zero-length runtime capture write")
                runtime_bytes += count
                remaining = remaining[count:]
        except OSError as error:
            runtime_stopped = True
            print(f"MAKO Renderer: runtime capture stopped: {error}", file=sys.stderr, flush=True)
    def save(data):
        nonlocal written, stopped
        if stopped:
            return
        marker = b"MAKO Renderer: wayland protocol capture-limit; beginning preserved\n"
        if written + len(data) + len(marker) > limit:
            data = marker
            stopped = True
        if written + len(data) > limit:
            return
        remaining = memoryview(data)
        try:
            while remaining:
                count = os.write(fd, remaining)
                if count <= 0:
                    raise OSError("zero-length protocol capture write")
                written += count
                remaining = remaining[count:]
        except OSError as error:
            # Diagnostic disk failures must not stop draining the child's
            # stderr pipe or terminate the running game.
            stopped = True
            print(f"MAKO Renderer: Wayland protocol capture stopped: {error}", file=sys.stderr, flush=True)
    try:
        print(f"MAKO Renderer: Wayland protocol capture: {name}", file=sys.stderr, flush=True)
        save(b"MAKO Renderer: wayland protocol session version=1 retention=stop\n" + clock_record())
        env = os.environ.copy()
        env.update(WAYLAND_DEBUG="client", MAKO_PACING_PROFILE_WAYLAND="1")
        if frame_trace:
            env["MAKO_PACING_PROFILE_TRACE"] = "1"
        if runtime_log is not None:
            runtime_fd = os.open(runtime_log, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
            env.update(VKD3D_DEBUG="info", DXVK_LOG_LEVEL="info")
            keys = ("SteamAppId", "SteamGameId", "VKD3D_CONFIG", "VKD3D_DISABLE_EXTENSIONS",
                    "VKD3D_SWAPCHAIN_PRESENT_MODE", "VKD3D_FRAME_RATE", "PROTON_ENABLE_WAYLAND",
                    "PROTON_USE_WAYLAND", "MAKO_PROFILE", "MANGOHUD", "MANGOHUD_CONFIG",
                    "VKD3D_LOG_FILE", "DXVK_LOG_PATH")
            save_runtime(("MAKO Renderer: runtime capture environment=" + json.dumps(
                {key: env[key] for key in keys if key in env}, ensure_ascii=True) + "\n").encode())
            print(f"MAKO Renderer: runtime capture: {runtime_log}", file=sys.stderr, flush=True)
        child = subprocess.Popen(command, env=env, stderr=subprocess.PIPE)
        handlers = {}
        def forward(signum, _frame):
            try:
                child.send_signal(signum)
            except ProcessLookupError:
                pass
        for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            handlers[signum] = signal.signal(signum, forward)
        try:
            calibrated = time.monotonic()
            with child.stderr:
                for raw in child.stderr:
                    now = time.monotonic()
                    if now - calibrated >= 1:
                        save(clock_record())
                        calibrated = now
                    parsed = message(raw)
                    if parsed:
                        save(parsed[0].encode("ascii") + b"\n")
                    elif MESSAGE.fullmatch(ANSI.sub(b"", raw).decode("ascii", errors="replace").strip()):
                        continue  # Drop unselected Wayland traffic, including input.
                    else:
                        # Retain runtime startup diagnostics, never unrelated game
                        # stderr, Wayland input, titles, or shader/picture payloads.
                        clean = ANSI.sub(b"", raw)
                        if (clean.startswith((b"MAKO Renderer:", b"info:", b"warn:", b"err:")) or
                                re.match(rb"[0-9.]+:[0-9a-f]+:[0-9a-f]+:(?:info|warn|err):vkd3d-proton:", clean)):
                            save_runtime(clean)
                        sys.stderr.buffer.write(raw)
                        sys.stderr.buffer.flush()
            result = child.wait()
            save(clock_record())
            return result if result >= 0 else 128 - result
        finally:
            for signum, handler in handlers.items():
                signal.signal(signum, handler)
    finally:
        os.close(fd)
        if runtime_fd is not None:
            os.close(runtime_fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="new private file; default: /tmp/mako-wayland-protocol-UID-*.log")
    parser.add_argument("--frame-trace", action="store_true", help="enable correlated per-source CPU/GPU/application tracing in the child")
    parser.add_argument("--runtime-log", type=Path, help="new private bounded runtime log; enables VKD3D/DXVK info logging in the child")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="-- followed by the existing game launch command")
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("supply -- followed by the existing game launch command")
    try:
        return capture(command, args.output, frame_trace=args.frame_trace, runtime_log=args.runtime_log)
    except OSError as error:
        parser.exit(1, f"Cannot capture Wayland protocol: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())

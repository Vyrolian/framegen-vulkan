# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic joins and missing evidence for the latency budget analyser."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/analyze-latency.py"
SPEC = importlib.util.spec_from_file_location("analyze_latency", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def record(operation, **fields):
    return "framegen-vulkan: operation=" + operation + " " + " ".join(
        f"{key}={value}" for key, value in fields.items()) + "\n"


def capture(context=7, pid=10, surface=99, flags=14, clock=1, app_entry=True, reflex=True):
    # Frame 2: 14 ms game step; budget stages 20, 2, 1, 7, 1 ms; total 31 ms.
    # Reflex frame 3 (the game's ID, one ahead of the source): simulation 80-88 ms,
    # D3D present 90 ms, out-of-band present 93.9-94.5 ms around the 94 ms present call.
    lines = []
    if app_entry:
        lines.append(record("pacing-profile-app-frame", pid=pid, tid=5, source=2,
                            enter_ns=94_000_000, exit_ns=94_400_000))
    if reflex:
        lines.append(record("pacing-profile-reflex-mode", pid=pid, low_latency=1, boost=0,
                            min_interval_us=0, result=0))
        lines.append(record("pacing-profile-reflex-sleep", pid=pid, tid=4,
                            enter_ns=79_000_000, exit_ns=80_000_000, result=0))
        for marker, tid, ns in ((0, 4, 80_000_000), (1, 4, 88_000_000), (4, 6, 90_000_000),
                                (10, 5, 93_900_000), (11, 5, 94_500_000)):
            lines.append(record("pacing-profile-reflex-marker", pid=pid, tid=tid,
                                present_id=3, marker=marker, ns=ns))
        # An earlier frame's out-of-band present on the same thread must not match.
        lines.append(record("pacing-profile-reflex-marker", pid=pid, tid=5,
                            present_id=2, marker=10, ns=80_000_000))
        lines.append(record("pacing-profile-reflex-marker", pid=pid, tid=5,
                            present_id=2, marker=11, ns=80_500_000))
    for source, done in ((1, 100_000_000), (2, 114_000_000)):
        lines.append(record("pacing-profile-gpu-split", pid=pid, context=context,
                            source=source, game_done_ns=done, generated_end_ns=done+2_000_000))
    for ordinal, generated, slot, feedback in ((3, 1, 117_000_000, 11), (4, 0, 124_000_000, 12)):
        lines.append(record("pacing-profile-output", pid=pid, context=context, source=2,
                            ordinal=ordinal, generated=generated, scheduled_ns=slot,
                            started_ns=slot, arrival_ns=95_000_000 if generated else 0,
                            result=0, feedback=feedback, batch_interval_ns=6_900_000 if generated else 0,
                            batch_outputs=2 if generated else 0))
    lines.append(record("pacing-profile-wayland", pid=pid, surface_token=surface,
                        event="request", context=context, source=2, generated=0, feedback=12))
    lines.append(record("pacing-profile-wayland", pid=pid, surface_token=surface,
                        event="presented", feedback=12, clock_id=clock,
                        presented_ns=125_000_000, flags=flags))
    return lines


class LatencyTests(unittest.TestCase):
    def test_budget(self):
        result = MODULE.analyze(capture())
        for stage, value in zip(MODULE.STAGES, (20, 2, 1, 7, 1, 31)):
            self.assertEqual(result["budget_ms"][stage], dict(n=1, p5=value, median=value, p95=value))
        self.assertEqual(result["scheduled_batch_period_minus_gpu_step_ms"]["median"], 0)
        self.assertAlmostEqual(result["worker_batch_period_minus_gpu_step_ms"]["median"], -.2)

    def test_identity_and_record_order(self):
        # Same source/output/feedback IDs in other processes and surfaces do not collide.
        lines = capture() + capture(context=8, surface=100) + capture(pid=20)
        result = MODULE.analyze(reversed(lines))
        self.assertEqual(result["budget_ms"][MODULE.STAGES[-1]]["n"], 3)

    def test_missing_display_and_clock(self):
        for lines in (capture()[:-1], capture(clock=0)):
            result = MODULE.analyze(lines)
            self.assertEqual(result["budget_ms"][MODULE.STAGES[0]]["n"], 1)
            self.assertEqual(result["budget_ms"][MODULE.STAGES[-1]]["n"], 0)

    def test_no_cross_surface_join(self):
        lines = capture()
        lines[-1] = lines[-1].replace("surface_token=99", "surface_token=100")
        self.assertEqual(MODULE.analyze(lines)["budget_ms"][MODULE.STAGES[-1]]["n"], 0)

    def test_display_flags_filter(self):
        result = MODULE.analyze(capture(flags=6), flags=14)
        self.assertEqual(result["budget_ms"][MODULE.STAGES[0]]["n"], 1)
        self.assertEqual(result["budget_ms"][MODULE.STAGES[-1]]["n"], 0)

    def test_bad_and_missing_gpu(self):
        lines = capture()
        no_gpu = [line for line in lines if "gpu-split" not in line]
        for lines in ([], ["partial operation=pacing-profile-output"], no_gpu):
            result = MODULE.analyze(lines)
            self.assertEqual(result["budget_ms"][MODULE.STAGES[0]]["n"], 0)

    def test_limiter_arrival_fallback(self):
        # Without an app-frame record the later limiter arrival (95 ms) is used and counted.
        result = MODULE.analyze(capture(app_entry=False))
        self.assertEqual(result["arrival_fallbacks"], 1)
        self.assertEqual(result["budget_ms"][MODULE.STAGES[0]]["median"], 19)
        self.assertEqual(result["reflex_budget_ms"][MODULE.REFLEX_STAGES[-1]]["n"], 0)

    def test_reflex_budget(self):
        result = MODULE.analyze(capture())
        self.assertEqual(result["arrival_fallbacks"], 0)
        for stage, value in zip(MODULE.REFLEX_STAGES, (8, 10, 4, 20, 11, 45)):
            self.assertEqual(result["reflex_budget_ms"][stage]["median"], value, stage)
        self.assertEqual(result["reflex_modes"],
                         [dict(low_latency=1, boost=0, min_interval_us=0, result="0", count=1)])
        self.assertEqual(result["reflex_sleep_ms"]["max"], 1)

    def test_reflex_requires_present_inside_out_of_band(self):
        lines = [line.replace("marker=11 ns=94500000", "marker=11 ns=93950000") for line in capture()]
        result = MODULE.analyze(lines)
        self.assertEqual(result["budget_ms"][MODULE.STAGES[-1]]["n"], 1)
        self.assertEqual(result["reflex_budget_ms"][MODULE.REFLEX_STAGES[-1]]["n"], 0)

    def test_without_reflex(self):
        result = MODULE.analyze(capture(reflex=False))
        self.assertEqual(result["reflex_modes"], [])
        self.assertEqual(result["reflex_sleep_ms"]["n"], 0)
        self.assertEqual(result["reflex_budget_ms"][MODULE.REFLEX_STAGES[0]]["n"], 0)

    def test_empty_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "empty.log"
            path.write_text("")
            result = subprocess.run([sys.executable, "-I", str(SCRIPT), str(path)],
                                    check=True, text=True, capture_output=True)
            self.assertIn("missing", result.stdout)

    def test_higher_multiplier_uses_first_generated(self):
        lines = capture()
        lines.insert(4, record("pacing-profile-output", pid=10, context=7, source=2,
                              ordinal=2, generated=1, scheduled_ns=110_000_000,
                              started_ns=110_000_000, arrival_ns=94_000_000, result=0, feedback=10))
        result = MODULE.analyze(lines)
        self.assertEqual(result["budget_ms"][MODULE.STAGES[3]]["median"], 14)


if __name__ == "__main__":
    unittest.main()

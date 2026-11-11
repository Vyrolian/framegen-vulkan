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


def capture(context=7, pid=10, surface=99, flags=14, clock=1):
    # Frame 2: 14 ms game step; budget stages 20, 2, 1, 7, 1 ms; total 31 ms.
    lines = [record("pacing-profile", pid=pid, context=context,
                    arrival_basis="application_present_entry")]
    for source, done in ((1, 100_000_000), (2, 114_000_000)):
        lines.append(record("pacing-profile-gpu-split", pid=pid, context=context,
                            source=source, game_done_ns=done, generated_end_ns=done+2_000_000))
    for ordinal, generated, slot, feedback in ((3, 1, 117_000_000, 11), (4, 0, 124_000_000, 12)):
        lines.append(record("pacing-profile-output", pid=pid, context=context, source=2,
                            ordinal=ordinal, generated=generated, scheduled_ns=slot,
                            started_ns=slot, arrival_ns=94_000_000 if generated else 0,
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
        for lines in ([], ["partial operation=pacing-profile-output"], capture()[2:]):
            result = MODULE.analyze(lines)
            self.assertEqual(result["budget_ms"][MODULE.STAGES[0]]["n"], 0)

    def test_old_arrival_basis_and_empty_cli(self):
        self.assertEqual(MODULE.analyze([record("pacing-profile")])["arrival_basis"],
                         ["limiter_arrival_approximation"])
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

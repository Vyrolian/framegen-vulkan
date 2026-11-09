#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check analyze-measure.py on a synthetic measure log: the join by seq, the window and the medians.

Every expected value below was worked out by hand from the synthetic timestamps. Run with
`python3 -I engine/fgvk-measure/tests/test_analyze_measure.py`.
"""

import contextlib
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path

ANALYZER = Path(__file__).resolve().parents[2] / "scripts" / "analyze-measure.py"
spec = importlib.util.spec_from_file_location("analyze_measure", ANALYZER)
analyze_measure = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = analyze_measure
spec.loader.exec_module(analyze_measure)

PREFIX = "framegen-vulkan: measure: "
T0 = 1_000_000_000

# seq, call_start_ns, measured
PRESENTS = [
    (1, 1_000_000_000, True),
    (2, 1_016_000_000, True),
    (3, 1_033_000_000, True),
    (4, 1_050_000_000, True),
    (5, 1_066_000_000, True),
    (6, 1_083_000_000, False),  # unmeasured: no GPU marker
    (7, 1_100_000_000, True),   # discarded
    (8, 1_116_000_000, True),   # measured but no gpu line
    (9, 1_133_000_000, True),   # gpu unavailable
    (10, 1_150_000_000, True),  # no presented and no discarded record
    (11, 1_166_000_000, True),
]
# seq, gpu_end_ns, available
GPU = [
    (1, 999_000_000, True),        # call -> gpu end = -1.0 ms
    (2, 1_018_000_000, True),      # +2.0 ms
    (3, 1_036_000_000, True),      # +3.0 ms
    (4, 1_054_000_000, True),      # +4.0 ms
    (5, 1_071_000_000, True),      # +5.0 ms
    (7, 1_106_000_000, True),      # +6.0 ms, discarded
    (9, 0, False),                 # unavailable
    (10, 1_157_000_000, True),     # +7.0 ms
    (11, 1_167_500_000, True),     # +1.5 ms
    (98, 2_000_000_000, True),     # no present for this seq
]
# seq, presented_ns, flags
PRESENTED = [
    (1, 1_001_000_000, 14),  # gpu end -> presented 2.0, call -> presented 1.0
    (2, 1_023_000_000, 14),  # 5.0, 7.0
    (3, 1_037_000_000, 14),  # 1.0, 4.0
    (4, 1_057_500_000, 14),  # 3.5, 7.5
    (5, 1_075_000_000, 14),  # 4.0, 9.0
    (6, 1_095_000_000, 6),   # no gpu, 12.0
    (8, 1_120_000_000, 6),   # no gpu, 4.0
    (9, 1_141_000_000, 14),  # no gpu, 8.0
    (11, 1_170_000_000, 6),  # 2.5, 4.0
    (99, 1_200_000_000, 6),  # no present for this seq
]
DISCARDED = [7]


def build_lines():
    lines = [
        "framegen-vulkan: measure: session pid=4242 device=AMD Radeon Graphics (RADV GFX1100) calibrated=1\n",
        "framegen-vulkan: measure: clock clock_id=1\n",
        "not a measure line\n",
    ]
    for seq, call_ns, measured in PRESENTS:
        lines.append(f"{PREFIX}present seq={seq} swapchain=1 image=0 waits=1 call_start_ns={call_ns} "
                     f"call_end_ns={call_ns + 200_000} result=0 measured={1 if measured else 0}\n")
    for seq, end_ns, available in GPU:
        if available:
            lines.append(f"{PREFIX}gpu seq={seq} available=1 gpu_start_ns={end_ns - 1000} gpu_end_ns={end_ns}\n")
        else:
            lines.append(f"{PREFIX}gpu seq={seq} available=0 gpu_start_ns=0 gpu_end_ns=0\n")
    for seq, presented_ns, flags in PRESENTED:
        lines.append(f"{PREFIX}presented seq={seq} presented_ns={presented_ns} refresh_ns=16666667 "
                     f"flags={flags} msc={seq}\n")
    for seq in DISCARDED:
        lines.append(f"{PREFIX}discarded seq={seq}\n")
    lines.append(f"{PREFIX}summary presents=11 measured=10 unmeasured=1 feedbacks=10 presented=9 discarded=1\n")
    return lines


def analyze(start=0.0, end=None):
    log = analyze_measure.load(build_lines())
    return analyze_measure.analyze(log, start, end)


def group(report, name):
    for entry in report["groups"]:
        if entry["group"] == name:
            return entry
    raise AssertionError(f"no group {name!r} in {[g['group'] for g in report['groups']]}")


def metric_row(text, group_name, label):
    """The rendered row for one metric inside one group, found by its label."""
    lines = text.splitlines()
    header = next((i for i, line in enumerate(lines) if line.startswith(f"group {group_name} ")), None)
    if header is None:
        raise AssertionError(f"no group {group_name!r} in the rendered report")
    for line in lines[header + 1:]:
        if line.startswith("group "):
            break
        if line.strip().startswith(label):
            return line
    raise AssertionError(f"no row {label!r} in group {group_name!r}")


class JoinAndCountTests(unittest.TestCase):
    def test_counts(self):
        counts = analyze()["counts"]
        self.assertEqual(counts["presents"], 11)
        self.assertEqual(counts["measured"], 10)
        self.assertEqual(counts["unmeasured"], 1)
        self.assertEqual(counts["gpu_lines"], 9)
        self.assertEqual(counts["unavailable"], 1)
        self.assertEqual(counts["presented"], 9)
        self.assertEqual(counts["discarded"], 1)
        self.assertEqual(counts["invalid_presented"], 0)

    def test_unjoined(self):
        unjoined = analyze()["unjoined"]
        self.assertEqual(unjoined["present_without_outcome"], 1)   # seq 10
        self.assertEqual(unjoined["measured_without_gpu"], 1)      # seq 8
        self.assertEqual(unjoined["gpu_without_present"], 1)       # seq 98
        self.assertEqual(unjoined["presented_without_present"], 1)  # seq 99
        self.assertEqual(unjoined["discarded_without_present"], 0)
        self.assertEqual(unjoined["total"], 4)


class MedianTests(unittest.TestCase):
    def assertMedian(self, entry, metric, expected, n):
        summary = entry["metrics"][metric]
        self.assertEqual(summary["n"], n, metric)
        self.assertAlmostEqual(summary["median"], expected, places=9, msg=metric)

    def test_overall_medians(self):
        overall = group(analyze(), "all")
        self.assertMedian(overall, "call_to_gpu_end", 3.5, 8)
        self.assertMedian(overall, "gpu_end_to_presented", 3.0, 6)
        self.assertMedian(overall, "call_to_presented", 7.0, 9)
        self.assertMedian(overall, "presented_step", 20.75, 8)
        self.assertAlmostEqual(overall["metrics"]["gpu_end_to_presented"]["over_3ms_share"], 50.0)

    def test_per_flags_medians(self):
        report = analyze()
        flags14 = group(report, "14")
        self.assertMedian(flags14, "call_to_gpu_end", 3.0, 5)
        self.assertMedian(flags14, "gpu_end_to_presented", 3.5, 5)
        self.assertMedian(flags14, "call_to_presented", 7.25, 6)
        self.assertMedian(flags14, "presented_step", 20.5, 5)
        self.assertAlmostEqual(flags14["metrics"]["gpu_end_to_presented"]["over_3ms_share"], 60.0)

        flags6 = group(report, "6")
        self.assertMedian(flags6, "call_to_gpu_end", 1.5, 1)
        self.assertMedian(flags6, "gpu_end_to_presented", 2.5, 1)
        self.assertMedian(flags6, "call_to_presented", 4.0, 3)
        self.assertMedian(flags6, "presented_step", 25.0, 3)

        unpresented = group(report, analyze_measure.GROUP_UNPRESENTED)
        self.assertMedian(unpresented, "call_to_gpu_end", 6.5, 2)
        self.assertEqual(unpresented["presents"], 2)

    def test_render_prints_the_medians(self):
        text = analyze_measure.render(analyze())
        self.assertIn("group 14 presents=6", text)
        self.assertIn("group all presents=11", text)

        row = metric_row(text, "all", "call -> gpu end")
        self.assertIn("n=8", row)
        self.assertIn("median=3.500", row)

        row = metric_row(text, "all", "gpu end -> presented")
        self.assertIn("n=6", row)
        self.assertIn("median=3.000", row)
        self.assertIn("over_3ms=50.0%", row)

        self.assertIn("median=7.000", metric_row(text, "all", "call -> presented"))
        self.assertIn("median=7.250", metric_row(text, "14", "call -> presented"))
        self.assertIn("median=20.500", metric_row(text, "14", "presented step"))
        self.assertIn("median=20.750", metric_row(text, "all", "presented step"))
        self.assertIn("median=25.000", metric_row(text, "6", "presented step"))


class WindowTests(unittest.TestCase):
    def test_from_starts_the_window_after_the_first_present(self):
        report = analyze(start=0.05)
        self.assertEqual(report["counts"]["presents"], 8)  # seq 4..11
        flags14 = group(report, "14")
        self.assertEqual(flags14["metrics"]["call_to_gpu_end"]["n"], 2)  # seq 4 and 5
        self.assertAlmostEqual(flags14["metrics"]["call_to_gpu_end"]["median"], 4.5, places=9)

    def test_to_is_exclusive(self):
        report = analyze(start=0.0, end=0.02)
        self.assertEqual(report["counts"]["presents"], 2)  # seq 1 and 2
        flags14 = group(report, "14")
        self.assertAlmostEqual(flags14["metrics"]["call_to_gpu_end"]["median"], 0.5, places=9)


class ParsingTests(unittest.TestCase):
    def test_session_device_name_keeps_spaces(self):
        kind, fields = analyze_measure.parse_line(
            "framegen-vulkan: measure: session pid=7 device=Some GPU (x=y) calibrated=0\n")
        self.assertEqual(kind, "session")
        self.assertEqual(fields["pid"], "7")
        self.assertEqual(fields["device"], "Some GPU (x=y)")
        self.assertEqual(fields["calibrated"], "0")

    def test_percentile_is_linear(self):
        values = [1.0, 2.0, 3.0, 4.0, 5.0]
        self.assertAlmostEqual(analyze_measure.percentile(values, 50), 3.0)
        self.assertAlmostEqual(analyze_measure.percentile(values, 5), 1.2)
        self.assertAlmostEqual(analyze_measure.percentile(values, 95), 4.8)
        self.assertAlmostEqual(analyze_measure.percentile([2.5], 99), 2.5)

    def test_main_prints_and_exit_codes(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "fgvk-measure-test.log"
            log.write_text("".join(build_lines()), encoding="utf-8")
            out = io.StringIO()
            self.assertEqual(analyze_measure.main([str(log)], out), 0)
            self.assertIn("median=3.500", out.getvalue())

            empty = Path(directory) / "empty.log"
            empty.write_text("nothing here\n", encoding="utf-8")
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(analyze_measure.main([str(empty)], io.StringIO()), 2)
                with self.assertRaises(SystemExit):
                    analyze_measure.main([str(log), "--from", "2", "--to", "1"], io.StringIO())


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check analyze_pt_probe.py on a synthetic probe log: the quantiles, the flags split, the step error,
the rate override and the side-by-side layout.

Every expected value below was worked out by hand from the synthetic timestamps. Run with
`python3 -I engine/fgvk-measure/tests/test_analyze_pt_probe.py`, or through CTest as measure-pt-probe-analyzer.
"""

import contextlib
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path

ANALYZER = Path(__file__).resolve().parents[1] / "probe" / "analyze_pt_probe.py"
spec = importlib.util.spec_from_file_location("analyze_pt_probe", ANALYZER)
analyze_pt_probe = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = analyze_pt_probe
spec.loader.exec_module(analyze_pt_probe)

PREFIX = "framegen-vulkan: pt-probe "
T0 = 1_000_000_000
PERIOD_NS = 10_000_000  # rate 100 Hz

# present_id -> (presented_ns, flags). Present k has target T0 + (k - 1) * PERIOD_NS.
PRESENTED = {
    1: (1_000_500_000, 14),  # +0.5 ms
    2: (1_011_000_000, 14),  # +1.0 ms (not over the 1 ms share limit)
    3: (1_023_000_000, 6),   # +3.0 ms
    4: (0, 0),               # not presented
    5: (1_039_000_000, 6),   # -1.0 ms (not over the share limit)
}
# Present 4 reached a stage but was never presented, so its stage_ns is set separately.
STAGE_NS = {4: 1_030_800_000}


def session_line(rate=100, no_target=0, clock=1):
    return (f"{PREFIX}session mode=fifo rate_hz={rate} lead_ms=6 uneven=0 no_target={no_target} "
            "seconds=0.05 presents=5 presented=4 staged=5 discarded=0 stage=IMAGE_FIRST_PIXEL_OUT "
            f"time_domain=CLOCK_MONOTONIC time_domain_id=1 wp_clock_id={clock} refresh_ns=0 "
            "refresh_interval_ns=0 extent=2560x1440 images=4 timing_queue=64 seed=7 timing_queue_full=0 "
            "suboptimal=0 feedback_request_failures=0 implicit_layers=disabled")


def present_line(k, presented_ns, flags):
    target = T0 + (k - 1) * PERIOD_NS
    stage_ns = STAGE_NS.get(k, presented_ns)
    return (f"{PREFIX}present_id={k} target_ns={target} submit_ns={target - 6_000_000} "
            f"call_ns={target - 5_900_000} stage_ns={stage_ns} presented_ns={presented_ns} flags={flags}")


def build_lines(rate=100, no_target=0, clock=1):
    lines = [
        "unrelated line, ignored",
        session_line(rate=rate, no_target=no_target, clock=clock),
        f"{PREFIX}garbage that is not a record",
    ]
    for k in sorted(PRESENTED):
        presented_ns, flags = PRESENTED[k]
        lines.append(present_line(k, presented_ns, flags))
    return lines


def close(actual, expected):
    return actual is not None and abs(actual - expected) < 1e-9


class ParseAndCountTests(unittest.TestCase):
    def test_counts(self):
        counts = analyze_pt_probe.analyse_lines(build_lines())["counts"]
        self.assertEqual(counts["presents"], 5)
        self.assertEqual(counts["presented"], 4)
        self.assertEqual(counts["staged"], 5)
        self.assertEqual(counts["unreadable"], 1)

    def test_run_label_from_session(self):
        self.assertEqual(analyze_pt_probe.analyse_lines(build_lines())["label"], "targeted")
        self.assertEqual(analyze_pt_probe.analyse_lines(build_lines(no_target=1))["label"], "no-target")


class DeviationTests(unittest.TestCase):
    def test_overall_quantiles(self):
        # Sorted deviations: -1.0, 0.5, 1.0, 3.0 (ms). Linear interpolation over n - 1 = 3 gaps.
        deviation = analyze_pt_probe.analyse_lines(build_lines())["deviation"]
        self.assertEqual(deviation["n"], 4)
        self.assertTrue(close(deviation["p5"], -0.775))
        self.assertTrue(close(deviation["median"], 0.75))
        self.assertTrue(close(deviation["p95"], 2.7))
        self.assertTrue(close(deviation["p99"], 2.94))
        self.assertTrue(close(deviation["share_over_1ms"], 0.25))  # only 3.0 is more than 1 ms away

    def test_flags_split(self):
        by_flags = analyze_pt_probe.analyse_lines(build_lines())["by_flags"]
        self.assertEqual(sorted(by_flags), [6, 14])
        deviation14 = by_flags[14]["deviation"]  # deviations 0.5, 1.0
        self.assertEqual(deviation14["n"], 2)
        self.assertTrue(close(deviation14["median"], 0.75))
        self.assertTrue(close(deviation14["share_over_1ms"], 0.0))
        deviation6 = by_flags[6]["deviation"]  # deviations -1.0, 3.0
        self.assertEqual(deviation6["n"], 2)
        self.assertTrue(close(deviation6["p5"], -0.8))
        self.assertTrue(close(deviation6["median"], 1.0))
        self.assertTrue(close(deviation6["p95"], 2.8))
        self.assertTrue(close(deviation6["p99"], 2.96))
        self.assertTrue(close(deviation6["share_over_1ms"], 0.5))


class StepTests(unittest.TestCase):
    def test_step_error_at_100_hz(self):
        # Steps between consecutive presented ids: 2 -> 1 is 10.5 ms, 3 -> 2 is 12.0 ms. Each is compared
        # with the 10 ms period, giving errors 0.5 and 2.0 ms (flags of the later present).
        report = analyze_pt_probe.analyse_lines(build_lines())
        step = report["step_error"]
        self.assertEqual(step["n"], 2)
        self.assertTrue(close(step["p5"], 0.575))
        self.assertTrue(close(step["median"], 1.25))
        self.assertTrue(close(step["p95"], 1.925))
        self.assertTrue(close(step["p99"], 1.985))
        self.assertTrue(close(report["by_flags"][14]["step_error"]["median"], 0.5))
        self.assertTrue(close(report["by_flags"][6]["step_error"]["median"], 2.0))

    def test_rate_override_changes_the_period(self):
        # At 200 Hz the period is 5 ms, so the same steps give errors 5.5 and 7.0 ms.
        step = analyze_pt_probe.analyse_lines(build_lines(), rate_override=200.0)["step_error"]
        self.assertEqual(step["n"], 2)
        self.assertTrue(close(step["median"], 6.25))


class LayoutTests(unittest.TestCase):
    def test_quantile_interpolation(self):
        self.assertTrue(close(analyze_pt_probe.quantile([1.0, 2.0, 3.0, 4.0], 0.5), 2.5))
        self.assertTrue(close(analyze_pt_probe.quantile([0.0, 10.0], 0.25), 2.5))

    def test_empty_run(self):
        report = analyze_pt_probe.analyse_lines([])
        self.assertEqual(report["deviation"]["n"], 0)
        self.assertIsNone(report["deviation"]["p5"])
        text = analyze_pt_probe.render([report])
        self.assertIn("no session line", text)
        row = next(line for line in text.splitlines() if line.startswith("presented - target"))
        self.assertEqual(row, "presented - target (ms)")
        self.assertIn("-", next(line for line in text.splitlines() if line.startswith("  p5")))

    def test_clock_warning(self):
        report = analyze_pt_probe.analyse_lines(build_lines(clock=2))
        self.assertTrue(any("clock_id is 2" in warning for warning in report["warnings"]))

    def test_side_by_side(self):
        targeted = analyze_pt_probe.analyse_lines(build_lines())
        control = analyze_pt_probe.analyse_lines(build_lines(no_target=1))
        text = analyze_pt_probe.render([targeted, control])
        header = text.splitlines()[0].split()
        self.assertEqual(header, ["run", "targeted", "no-target"])
        presents_row = next(line for line in text.splitlines() if line.startswith("presents"))
        self.assertEqual(presents_row.split(), ["presents", "5", "5"])

    def test_cli_writes_both_logs_and_numbers_duplicate_labels(self):
        with tempfile.TemporaryDirectory() as directory:
            first = Path(directory) / "first.log"
            second = Path(directory) / "second.log"
            first.write_text("\n".join(build_lines()) + "\n", encoding="utf-8")
            second.write_text("\n".join(build_lines()) + "\n", encoding="utf-8")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                status = analyze_pt_probe.main([str(first), str(second)])
        self.assertEqual(status, 0)
        header = out.getvalue().splitlines()[0]
        self.assertEqual(header.split(), ["run", "targeted", "targeted", "#2"])
        self.assertIn("targeted #2", header)
        self.assertIn("share with |presented - target| > 1 ms", out.getvalue())


if __name__ == "__main__":
    unittest.main()

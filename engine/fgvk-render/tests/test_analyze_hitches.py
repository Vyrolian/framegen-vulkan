#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic-log test for engine/scripts/analyze-hitches.py. Standard library only."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ANALYSER = Path(__file__).resolve().parents[2] / "scripts" / "analyze-hitches.py"
BASE = 1_000_000_000_000  # CLOCK_MONOTONIC nanoseconds; the synthetic capture starts here
MS = 1_000_000


def ms(value):
    return BASE + int(value * MS)


def output(ordinal, source, generated, started, feedback):
    return (f"framegen-vulkan: present diagnostics: operation=pacing-profile-output version=2 context=1 pid=1 "
            f"swapchain=0x1 ordinal={ordinal} source={source} generated={generated} deferred=1 image_index=0 "
            f"scheduled_ns={started} started_ns={started} finished_ns={started + MS} result=0 "
            f"copy_checked=0 copy_status=1 copy_sampled_ns=0 arrival_ns=0 feedback={feedback} "
            f"backend_frame=0 phase=0 present_id=0")


def output_gpu(source, available, copy_start, copy_end):
    return (f"framegen-vulkan: present diagnostics: operation=pacing-profile-output-gpu version=1 pid=1 "
            f"context=1 source={source} available={available} copy_start_ns={copy_start} copy_end_ns={copy_end}")


def presented(feedback, shown, flags):
    return (f"framegen-vulkan: present diagnostics: operation=pacing-profile-wayland version=2 pid=1 "
            f"surface_token=1 event=presented feedback={feedback} received_ns={ms(shown)} clock_id=1 "
            f"presented_ns={ms(shown)} sequence={feedback} refresh_ns=8000000 flags={flags} output_id=0")


def after_call_lines():
    # Generated sources 4 to 6 (feedback 6 to 8), called at 200, 300 and 400 ms.
    # Source 4 copies from 1 ms to 3 ms after its call and shows 12 ms after it, on flags 7.
    # Source 5 copies from 0.5 to 1.5 ms after its call and shows 16 ms after it, on flags 7.
    # Source 6 copies from 0 to 0.2 ms after its call and shows 8 ms after it, on flags 15.
    # Source 7 has no copy (unavailable). Source 99 has no generated output (unjoined).
    return [
        output(6, 4, 1, ms(200), 6),
        output(7, 5, 1, ms(300), 7),
        output(8, 6, 1, ms(400), 8),
        output_gpu(4, 1, ms(201), ms(203)),
        output_gpu(5, 1, ms(300.5), ms(301.5)),
        output_gpu(6, 1, ms(400), ms(400.2)),
        output_gpu(7, 0, 0, 0),
        output_gpu(99, 1, ms(500), ms(500.5)),
        presented(6, 212, 7),
        presented(7, 316, 7),
        presented(8, 408, 15),
    ]


def capture_lines():
    lines = [
        output(1, 1, 0, ms(0), 1),
        output(2, 2, 1, ms(10), 2),
        output(3, 2, 0, ms(60), 3),
        output(4, 3, 1, ms(70), 4),
        output(5, 3, 0, ms(110), 5),
        # Game step: source 3's game_done is 68 ms, so the generated output of source 3 is 2 ms after it.
        f"framegen-vulkan: present diagnostics: operation=pacing-profile-gpu-split version=1 pid=1 "
        f"context=1 source=1 backend_frame=0 game_done_ns={ms(0)} source_copy_end_ns=0 fg_start_ns=0 "
        f"prepass_end_ns=0 generated_end_ns=0 batch_end_ns=0",
        f"framegen-vulkan: present diagnostics: operation=pacing-profile-gpu-split version=1 pid=1 "
        f"context=1 source=2 backend_frame=0 game_done_ns={ms(0)} source_copy_end_ns=0 fg_start_ns=0 "
        f"prepass_end_ns=0 generated_end_ns=0 batch_end_ns=0",
        f"framegen-vulkan: present diagnostics: operation=pacing-profile-gpu-split version=1 pid=1 "
        f"context=1 source=3 backend_frame=0 game_done_ns={ms(68)} source_copy_end_ns=0 fg_start_ns=0 "
        f"prepass_end_ns=0 generated_end_ns=0 batch_end_ns=0",
    ]
    # Wayland feedback: generated src3 (feedback 4) is presented after real src3 (feedback 5).
    for feedback, presented in ((1, 10), (2, 20), (3, 30), (4, 80), (5, 70)):
        lines.append(f"framegen-vulkan: present diagnostics: operation=pacing-profile-wayland version=2 pid=1 "
                     f"surface_token=1 event=presented feedback={feedback} received_ns={ms(presented)} "
                     f"clock_id=1 presented_ns={ms(presented)} sequence={feedback} refresh_ns=8000000 "
                     f"flags=7 output_id=0")
    lines += [
        f"framegen-vulkan: present diagnostics: operation=pacing-profile-app-frame version=1 pid=1 tid=5 "
        f"source=2 enter_ns={ms(60)} exit_ns={ms(62)} previous_exit_ns={ms(10)} inside_layer_ns={45 * MS} "
        f"inside_submit_ns={45 * MS} inside_wait_ns=0 inside_acquire_ns=0",
        f"framegen-vulkan: present diagnostics: operation=pacing-profile-call version=1 pid=1 tid=5 "
        f"call=queue-present enter_ns={ms(30)} exit_ns={ms(55)} result=0 present_id=0 timeout_ns=0",
        f"framegen-vulkan: present diagnostics: operation=pacing-profile-app-acquire version=1 pid=1 tid=5 "
        f"enter_ns={ms(12)} exit_ns={ms(14)} path=host lock_wait_ns=0 tail_wait_value=7 "
        f"tail_seen_ns={ms(13)}",
        f"framegen-vulkan: present diagnostics: operation=pacing-profile-reflex-sleep version=1 pid=1 tid=5 "
        f"enter_ns={ms(40)} exit_ns={ms(41)} result=0",
        "framegen-vulkan: present diagnostics: operation=pacing-profile-skip version=1 pid=1 context=1 "
        f"source=2 reason=readiness scheduled_ns={ms(1)} now_ns={ms(2)}",
        "framegen-vulkan: present diagnostics: operation=pacing-profile-recorder version=1 pid=1 records=10 "
        "dropped=3 window_slot_drops=0 enqueue_max_us=5 enqueue_sum_us=9 writer_lag_max_ms=2 "
        "writer_cycle_max_ms=1 bytes=100",
    ]
    return lines


class AnalyseHitchesTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.log = Path(self.directory.name) / "capture.log"
        self.marks = Path(self.directory.name) / "marks.log"
        self.log.write_text("\n".join(capture_lines()) + "\n", encoding="utf-8")
        self.marks.write_text(f"framegen-vulkan: mark monotonic_ns={ms(55)} note=stutter here\n",
                              encoding="utf-8")

    def tearDown(self):
        self.directory.cleanup()

    def run_analyser(self, *extra):
        result = subprocess.run([sys.executable, "-I", str(ANALYSER), str(self.log), *extra],
                                capture_output=True, text=True, timeout=60,
                                env={"PATH": os.environ.get("PATH", "")})
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_reports_gap_class_calls_acquire_and_reflex(self):
        out = self.run_analyser("--marks", str(self.marks), "--min-gap", "20")
        self.assertIn("gap    50.0 ms on-screen 10.0 ms  gen src2 -> real src2", out, out)
        self.assertIn("inside-layer dominated", out, out)
        self.assertIn("call queue-present tid 5 25.00 ms result 0", out, out)
        self.assertIn("reflex-sleep 1.00 ms result 0", out, out)
        self.assertIn("acquire path=host", out, out)
        self.assertIn("tail_wait_value=7", out, out)

    def test_order_violation_and_joins(self):
        out = self.run_analyser("--min-gap", "20")
        self.assertIn("order violation: generated src3 presented after real src3", out, out)
        self.assertIn("order: 2 generated outputs presented, 1 order violations", out, out)

    def test_skip_recorder_and_totals(self):
        out = self.run_analyser("--min-gap", "20")
        self.assertIn("skip reason readiness: 1", out, out)
        self.assertIn("recorder drops: 3 queue, 0 window slot", out, out)
        # Gaps: 10->60 ms by call time (inside the layer), 60->70 on screen only (30->80 ms), 70->110 by call time.
        self.assertIn("totals: gaps 3; inside-layer dominated 1; outside-layer dominated 0;"
                      " no app-frame 2; FG late > 3 ms after game_done 1; order violations 1;"
                      " on-screen gap with call step < min-gap 1", out, out)

    def test_generated_position_on_screen(self):
        out = self.run_analyser("--min-gap", "20")
        # src2 generated shown at 20 between reals at 10 and 30: centred. src3 generated shown at 80
        # between 30 and 70: position 1.25, 30 ms off the midpoint.
        self.assertIn("misplacement median 30.00 p99 30.00 max 30.00 ms; > 2 ms: 1 of 2", out, out)

    def test_animation_error_from_reflex_markers(self):
        extra = []
        # App presents of sources 1..3 at 0, 50, 100 ms; simulation starts 10 ms apart, then 40 ms.
        for source, (enter, sim) in enumerate(((0, -20), (50, -10), (100, 30)), start=1):
            extra.append(f"framegen-vulkan: present diagnostics: operation=pacing-profile-app-frame version=1 "
                         f"pid=1 tid=5 source={source} enter_ns={ms(enter)} exit_ns={ms(enter + 1)} "
                         f"previous_exit_ns=0 inside_layer_ns=0 inside_submit_ns=0 inside_wait_ns=0 inside_acquire_ns=0")
            extra.append(f"framegen-vulkan: present diagnostics: operation=pacing-profile-reflex-marker version=1 "
                         f"pid=1 tid=5 present_id={source} marker=0 ns={ms(sim)}")
            extra.append(f"framegen-vulkan: present diagnostics: operation=pacing-profile-reflex-marker version=1 "
                         f"pid=1 tid=5 present_id={source} marker=4 ns={ms(enter)}")
        with self.log.open("a", encoding="utf-8") as handle:
            handle.write("\n".join(extra) + "\n")
        out = self.run_analyser("--min-gap", "20")
        # On-screen reals: src1 10, src2 30, src3 70 ms. Display steps 20, 40; simulation steps 10, 40.
        self.assertIn("animation error (real display step - game simulation step, 2 pairs, on-screen):"
                      " median 10.00", out, out)

    def test_mark_window_reports_the_note_and_its_gaps(self):
        out = self.run_analyser("--marks", str(self.marks), "--min-gap", "20")
        self.assertIn("mark t=0.055s note=stutter here", out, out)
        self.assertIn("gap 50.0 ms at t=0.010s", out, out)

    def test_window_excludes_gaps_outside_it(self):
        out = self.run_analyser("--min-gap", "20", "--from", "0.2")
        self.assertIn("totals: gaps 0;", out, out)

    def append_lines(self, lines):
        with self.log.open("a", encoding="utf-8") as handle:
            handle.write("\n".join(lines) + "\n")

    def test_after_call_split_joins_copy_times_to_presented_outputs(self):
        self.append_lines(after_call_lines())
        out = self.run_analyser("--min-gap", "20")
        self.assertIn("after-call split (generated outputs, available copies): 3 joined; 1 unavailable"
                      " and 1 unjoined (counted over the whole capture); 0 ambiguous; 0 not presented", out, out)
        # Flags 7 holds sources 4 and 5 (copy starts 1.0 and 0.5 ms after the call, shown 12 and 16 ms after it).
        self.assertIn("  flags 7: n 2", out, out)
        self.assertIn("median 0.75 ", section_line(out, "  flags 7: n 2", "call -> copy start:"), out)
        self.assertIn("median 1.50 ", section_line(out, "  flags 7: n 2", "copy duration:"), out)
        self.assertIn("median 11.75 ", section_line(out, "  flags 7: n 2", "copy end -> presented:"), out)
        self.assertIn("median 14.00 ", section_line(out, "  flags 7: n 2", "call -> presented:"), out)
        self.assertIn("2 of 2 (100%)", section_line(out, "  flags 7: n 2", "copy end later"), out)
        # Two points on opposite slopes: copy end after the call falls as the on-screen time rises.
        self.assertIn("-1.00", section_line(out, "  flags 7: n 2", "correlation"), out)
        # One sample has no correlation.
        self.assertIn("  flags 15: n 1", out, out)
        self.assertTrue(section_line(out, "  flags 15: n 1", "correlation").endswith("n/a"), out)
        # All three: call to copy start median 0.50, call to presented median 12.00, copy end to presented 9.00.
        self.assertIn("median 0.50 ", section_line(out, "  overall: n 3", "call -> copy start:"), out)
        self.assertIn("median 12.00 ", section_line(out, "  overall: n 3", "call -> presented:"), out)
        self.assertIn("median 9.00 ", section_line(out, "  overall: n 3", "copy end -> presented:"), out)
        self.assertIn("2 of 3 (67%)", section_line(out, "  overall: n 3", "copy end later"), out)

    def test_after_call_split_honours_window(self):
        self.append_lines(after_call_lines())
        # Call times 200, 300 and 400 ms are 0.2, 0.3 and 0.4 s from the first output.
        out = self.run_analyser("--min-gap", "20", "--from", "0.35")
        self.assertIn("  overall: n 1", out, out)
        self.assertIn("  flags 15: n 1", out, out)
        self.assertNotIn("  flags 7:", out, out)
        out = self.run_analyser("--min-gap", "20", "--to", "0.25")
        self.assertIn("  overall: n 1", out, out)
        self.assertIn("  flags 7: n 1", out, out)
        self.assertNotIn("  flags 15:", out, out)

    def test_real_copy_split_joins_real_outputs(self):
        # T3b: a real output (generated=0) with its private -> lower copy timed; the generated
        # output of the same source must not be joined to the real-copy record.
        call = ms(500)
        self.append_lines([
            output(90, 50, 0, call, 900), presented(900, 506, 14),
            output(91, 50, 1, call + 2 * MS, 901), presented(901, 509, 14),
            "framegen-vulkan: present diagnostics: operation=pacing-profile-real-copy-gpu version=1 pid=1 "
            f"context=1 source=50 available=1 copy_start_ns={call + 2 * MS} copy_end_ns={call + 2 * MS + MS // 10}",
        ])
        out = self.run_analyser("--min-gap", "20")
        self.assertIn("after-call split (real outputs, available copies): 1 joined; 0 unavailable", out, out)
        real = out[out.index("after-call split (real outputs"):]
        self.assertIn("median 2.00 ", section_line(real, "  overall: n 1", "call -> copy start:"), out)
        self.assertIn("median 6.00 ", section_line(real, "  overall: n 1", "call -> presented:"), out)

    def test_present_marker_split_joins_by_feedback(self):
        # T4: the marker after a generated lower present joins its output by feedback; the
        # generated copy end gives copy end -> marker start.
        call = ms(600)
        self.append_lines([
            output(92, 60, 1, call, 902), presented(902, 605, 14),
            "framegen-vulkan: present diagnostics: operation=pacing-profile-output-gpu version=1 pid=1 "
            f"context=1 source=60 available=1 copy_start_ns={call + MS // 2} copy_end_ns={call + MS}",
            "framegen-vulkan: present diagnostics: operation=pacing-profile-present-marker-gpu version=1 pid=1 "
            f"context=1 source=60 feedback=902 generated=1 available=1 marker_start_ns={call + 3 * MS} "
            f"marker_end_ns={call + 3 * MS + 1000}",
            "framegen-vulkan: present diagnostics: operation=pacing-profile-present-marker-gpu version=1 pid=1 "
            "context=1 source=61 feedback=903 generated=0 available=0 marker_start_ns=0 marker_end_ns=0",
        ])
        out = self.run_analyser("--min-gap", "20")
        self.assertIn("present marker split (private queue after each lower present): 1 joined; 1 unavailable", out, out)
        marker = out[out.index("present marker split"):]
        self.assertIn("median 3.00 ", section_line(marker, "  generated flags 14: n 1", "call -> marker start:"), out)
        self.assertIn("median 2.00 ", section_line(marker, "  generated flags 14: n 1", "marker start -> presented:"), out)
        self.assertIn("median 2.00 ", section_line(marker, "  generated flags 14: n 1", "copy end -> marker start:"), out)

    def test_after_call_split_absent_without_gpu_split_records(self):
        out = self.run_analyser("--min-gap", "20")
        self.assertIn("after-call split: no pacing-profile-output-gpu records", out, out)


def section_line(out, header, metric):
    # The metric line under one group header of the after-call split, without its indentation.
    lines = out.splitlines()
    start = next(i for i, line in enumerate(lines) if line == header)
    for line in lines[start + 1:]:
        if not line.startswith("    "):
            break
        if line.strip().startswith(metric):
            return line.strip()
    raise AssertionError(f"no '{metric}' line under '{header}'\n{out}")


if __name__ == "__main__":
    unittest.main()

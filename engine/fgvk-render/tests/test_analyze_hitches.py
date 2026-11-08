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


if __name__ == "__main__":
    unittest.main()

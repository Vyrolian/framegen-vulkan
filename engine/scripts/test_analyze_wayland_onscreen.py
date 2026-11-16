#!/usr/bin/env python3
"""Synthetic-log tests for analyze-wayland-onscreen.py (stdlib unittest, no GPU or compositor)."""

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "analyze-wayland-onscreen.py"
sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("analyze_wayland_onscreen", SCRIPT)
analyzer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analyzer)

PID = 77
MS = 1_000_000
T = 1_000_000_000
REFRESH = 4_000_000
CTX_A, CTX_B = 111, 222
SURF_A, SURF_B = 3001, 3002


def output_line(context, feedback, ordinal, source, generated, image, started, finished):
    return ("framegen-vulkan: present diagnostics: operation=pacing-profile-output version=1 "
            f"context={context} pid={PID} swapchain=0x1 ordinal={ordinal} source={source} generated={generated} "
            f"deferred=0 image_index={image} scheduled_ns=0 started_ns={started} finished_ns={finished} "
            f"result=0 copy_checked=0 copy_status=1 copy_sampled_ns=0 arrival_ns=0 feedback={feedback} "
            "backend_frame=0 phase=0 present_id=0 ready_ns=0 slack_ns=0 offset_ns=0\n")


def wayland_line(surface, event, feedback, **fields):
    body = " ".join(f"{key}={value}" for key, value in fields.items())
    return (f"framegen-vulkan: present diagnostics: operation=pacing-profile-wayland version=2 pid={PID} "
            f"surface_token={surface} event={event} feedback={feedback} {body}\n")


def request_line(surface, feedback, context, source, generated, image, requested, now_shift=150, clock=1):
    return wayland_line(surface, "request", feedback, context=context, source=source, image_index=image,
                        generated=generated, requested_ns=requested, clock_id=clock,
                        presentation_now_ns=requested + now_shift, calibration_span_ns=100)


def presented_line(surface, feedback, presented, clock=1):
    return wayland_line(surface, "presented", feedback, received_ns=presented + 500_000, clock_id=clock,
                        presented_ns=presented, sequence=0, refresh_ns=REFRESH, flags=14, output_id=111)


def discarded_line(surface, feedback, when):
    return wayland_line(surface, "discarded", feedback, received_ns=when + 500_000)


def emit(lines, surface, context, feedback, ordinal, source, generated, image, presented, *,
         clock=1, output=True, discarded=False, requested=None, shift=0, now_shift=150):
    """One lower present: output record, request, and its outcome. Output scope ends 4 ms before the
    layer-clock presentation; `shift` moves only the compositor's reported time (clock offset tests)."""
    rq = requested if requested is not None else presented - (10 if not generated else 6) * MS
    finished = presented - 4 * MS
    if output:
        lines.append(output_line(context, feedback, ordinal, source, generated, image, rq - 1000, finished))
    lines.append(request_line(surface, feedback, context, source, generated, image, rq, now_shift=now_shift))
    if discarded:
        lines.append(discarded_line(surface, feedback, presented + shift))
    else:
        lines.append(presented_line(surface, feedback, presented + shift, clock=clock))
    return lines


def main_lines():
    """Surface A is 2x (one generated per source), surface B is 3x. Feedback ids overlap on purpose."""
    lines = []
    period_a = 16 * MS
    real_a = {s: T + s * period_a + 10 * MS for s in range(1, 7)}
    for s in range(1, 7):
        gen = real_a[1] - 8 * MS if s == 1 else (real_a[s - 1] + real_a[s]) // 2 + 1 * MS
        ordinal = 2 * s - 1
        emit(lines, SURF_A, CTX_A, 2 * s - 1, ordinal, s, 1, (ordinal - 1) % 4, gen,
             output=(s != 3), requested=gen - 6 * MS)
        ordinal = 2 * s
        emit(lines, SURF_A, CTX_A, 2 * s, ordinal, s, 0, (ordinal - 1) % 4, real_a[s],
             clock=(2 if s == 4 else 1))
        if s == 5:  # duplicate output record for one feedback id: the request keeps no unique output
            lines.append(output_line(CTX_A, 10, 10, 5, 0, 1, real_a[5] - 10 * MS - 1000, real_a[5] - 4 * MS))
    period_b = 24 * MS
    real_b = {s: T + 1_000_000_000 + s * period_b + 10 * MS for s in range(1, 6)}
    for s in range(1, 6):
        if s == 1:
            gen1, gen2 = real_b[1] - 8 * MS, real_b[1] - 4 * MS
        else:
            gap = real_b[s] - real_b[s - 1]
            gen1 = real_b[s - 1] + gap // 3 + MS // 2
            gen2 = real_b[s - 1] + 2 * gap // 3 - MS // 4
        base = 3 * s - 2
        emit(lines, SURF_B, CTX_B, base, base, s, 1, 0, gen1, requested=gen1 - 6 * MS)
        emit(lines, SURF_B, CTX_B, base + 1, base + 1, s, 1, 1, gen2, requested=gen2 - 6 * MS,
             discarded=(s == 3))
        emit(lines, SURF_B, CTX_B, base + 2, base + 2, s, 0, 2, real_b[s], requested=real_b[s] - 10 * MS)
    # One record the parser must reject (no refresh_ns), one unavailable event, one empty gap record.
    lines.append(wayland_line(SURF_A, "presented", 99, received_ns=T, clock_id=1, presented_ns=T,
                              sequence=0, flags=14, output_id=111))
    lines.append(wayland_line(SURF_A, "unavailable", 500, context=333, source=1, image_index=0,
                              generated=1, requested_ns=T, clock_id=4294967295, reason=3))
    lines.append(f"framegen-vulkan: present diagnostics: operation=pacing-profile-wayland-gap version=1 pid={PID} "
                 "surface_token=3001 lost_records=0\n")
    lines.append("framegen-vulkan: present diagnostics: operation=pacing-profile-recorder version=1 pid=77 records=0\n")
    return lines


def write_log(directory: Path, lines) -> Path:
    path = directory / "pacing.log"
    path.write_text("".join(lines), encoding="utf-8")
    return path


def run_cli(*args):
    return subprocess.run([sys.executable, "-I", str(SCRIPT), *map(str, args)], capture_output=True, text=True, check=False)


def analyze_file(path: Path, context=None, from_s=0.0, to_s=None, legacy=False):
    log = analyzer.read_log(path, keep_calls=legacy)
    return analyzer.analyze(log, context, from_s, to_s, legacy)


class MainLogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.path = write_log(Path(cls.directory.name), main_lines())

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_totals_and_parse_rejects_bad_record(self):
        result = analyze_file(self.path)
        self.assertEqual(result["totals"]["outputs"], 27)
        self.assertEqual(result["totals"]["requests"], 27)
        self.assertEqual(result["totals"]["presented"], 26)
        self.assertEqual(result["totals"]["discarded"], 1)
        self.assertEqual(result["totals"]["unavailable"], 1)
        self.assertEqual(result["totals"]["invalid_records"], 1)
        self.assertEqual(result["totals"]["wayland_gap_records"], 1)

    def test_joins_count_missing_output_duplicates_and_clock_dropped(self):
        joins = analyze_file(self.path)["joins"]
        self.assertEqual(joins["requests_without_output"], 2)
        self.assertEqual(joins["requests_without_output_presented"], 2)
        self.assertEqual(joins["duplicate_outputs"], 1)
        self.assertEqual(joins["duplicate_requests"], 0)
        self.assertEqual(joins["duplicate_outcomes"], 0)
        self.assertEqual(joins["presented_clock_not_monotonic"], 1)
        self.assertEqual(joins["outputs_without_request"], 0)
        self.assertEqual(joins["outcomes_without_request"], 0)
        self.assertEqual(joins["ordinal_inversions"], {})

    def test_present_to_screen_is_exact_per_joined_output(self):
        ms = analyze_file(self.path)["present_to_screen_ms"]
        self.assertEqual(ms["n"], 23)
        self.assertAlmostEqual(ms["p50"], 4.0)
        self.assertAlmostEqual(ms["max"], 4.0)
        self.assertEqual(ms["negative"], 0)

    def test_intervals_skip_untimed_and_span_both_surfaces(self):
        result = analyze_file(self.path)
        self.assertEqual(result["intervals_ms"]["n"], 24)
        self.assertEqual(result["intervals_ms"]["same_time_equal"], 0)
        self.assertEqual(sum(v["n"] for v in result["transitions_ms"].values()), 24)

    def test_ideal_groups_whole_log(self):
        ideal = analyze_file(self.path)["ideal"]
        self.assertEqual(ideal["complete"], 6)
        self.assertEqual(ideal["m_histogram"], {"2": 5, "3": 4})
        self.assertEqual(ideal["skipped"], {"real not shown": 2, "generated not shown": 1})
        self.assertEqual(ideal["generated_outside_reals"], 0)

    def test_2x_error_is_exact_on_surface_a(self):
        by_k = analyze_file(self.path, context=CTX_A)["ideal"]["by_k"]
        self.assertEqual(set(by_k), {"1"})
        self.assertEqual(by_k["1"]["abs_error_ms"]["n"], 3)
        self.assertAlmostEqual(by_k["1"]["abs_error_ms"]["p50"], 1.0)
        self.assertAlmostEqual(by_k["1"]["signed_p50_ms"], 1.0)

    def test_3x_errors_and_discarded_generated_on_surface_b(self):
        result = analyze_file(self.path, context=CTX_B)
        by_k = result["ideal"]["by_k"]
        self.assertEqual(result["ideal"]["m_histogram"], {"3": 4})
        self.assertEqual(result["ideal"]["skipped"], {"generated not shown": 1})
        self.assertEqual(by_k["1"]["abs_error_ms"]["n"], 3)
        self.assertAlmostEqual(by_k["1"]["abs_error_ms"]["p50"], 0.5)
        self.assertAlmostEqual(by_k["1"]["signed_p50_ms"], 0.5)
        self.assertEqual(by_k["2"]["abs_error_ms"]["n"], 3)
        self.assertAlmostEqual(by_k["2"]["abs_error_ms"]["p50"], 0.25)
        self.assertAlmostEqual(by_k["2"]["signed_p50_ms"], -0.25)
        self.assertEqual(result["selected"]["discarded_generated"], 1)
        self.assertEqual(result["selected"]["discarded_real"], 0)

    def test_context_filter_counts(self):
        a = analyze_file(self.path, context=CTX_A)
        self.assertEqual((a["selected"]["requests"], a["selected"]["reals"], a["selected"]["generated"]), (12, 6, 6))
        self.assertEqual(a["intervals_ms"]["n"], 10)
        b = analyze_file(self.path, context=CTX_B)
        self.assertEqual((b["selected"]["requests"], b["selected"]["reals"], b["selected"]["generated"]), (15, 5, 10))
        self.assertEqual(b["intervals_ms"]["n"], 13)

    def test_window_filter_selects_surface_b_only(self):
        result = analyze_file(self.path, from_s=0.9, to_s=1.5)
        self.assertEqual(result["selected"]["requests"], 15)
        self.assertEqual(len(result["windows"]), 1)
        window = result["windows"][0]
        self.assertAlmostEqual(window["outputs_per_s"], 7.5)
        self.assertAlmostEqual(window["reals_per_s"], 2.5)
        self.assertEqual(window["shown"], 14)
        self.assertEqual(window["discarded"], 1)

    def test_text_report_has_required_sections_and_flag_note(self):
        text = analyzer.render_text(analyze_file(self.path), self.path)
        for section in ("Log totals (whole log)", "Joins (whole log)", "present -> screen", "on-screen interval",
                        "generated vs ideal", "discarded:", "refresh_ns seen", "flags:", "windows (2 s"):
            self.assertIn(section, text)
        self.assertIn("flags are not proof of tearing or of no tearing.", text)

    def test_json_output_and_cli_round_trip(self):
        completed = run_cli(self.path, "--json", "--context", CTX_B)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        result = json.loads(completed.stdout)
        self.assertEqual(result["selection"]["context"], CTX_B)
        self.assertEqual(result["selected"]["requests"], 15)

    def test_invalid_window_is_rejected(self):
        completed = run_cli(self.path, "--from", "2", "--to", "1")
        self.assertEqual(completed.returncode, 2)
        self.assertIn("--to must be", completed.stderr)


class ClockAndDuplicateTests(unittest.TestCase):
    def write(self, lines):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        return write_log(Path(directory.name), lines)

    def reals(self, now_shift, clock_shift, count=3):
        lines = []
        for s in range(1, count + 1):
            rq = T + s * 16 * MS
            emit(lines, 4001, 444, s, s, s, 0, 0, rq + 10 * MS, requested=rq, shift=clock_shift, now_shift=now_shift)
        return lines

    def test_clock_offset_above_tolerance_is_applied(self):
        result = analyze_file(self.write(self.reals(now_shift=5 * MS, clock_shift=5 * MS)))
        self.assertEqual(result["clock"]["offset_applied_ns"], 5 * MS)
        self.assertAlmostEqual(result["present_to_screen_ms"]["p50"], 4.0)

    def test_clock_offset_within_tolerance_is_not_applied(self):
        result = analyze_file(self.write(self.reals(now_shift=MS // 2, clock_shift=MS // 2)))
        self.assertEqual(result["clock"]["offset_applied_ns"], 0)
        self.assertAlmostEqual(result["present_to_screen_ms"]["p50"], 4.5)

    def test_duplicate_request_and_outcome_keys_are_excluded(self):
        lines = []
        emit(lines, 5001, 555, 1, 1, 1, 0, 0, T + 16 * MS, requested=T + 6 * MS)
        emit(lines, 5001, 555, 2, 2, 2, 0, 1, T + 32 * MS, requested=T + 22 * MS)
        lines.append(request_line(5001, 2, 555, 2, 0, 1, T + 22 * MS))  # second request, same key
        emit(lines, 5001, 555, 3, 3, 3, 0, 2, T + 48 * MS, requested=T + 38 * MS)
        lines.append(presented_line(5001, 3, T + 48 * MS))  # second outcome, same key
        result = analyze_file(self.write(lines))
        self.assertEqual(result["joins"]["duplicate_requests"], 1)
        self.assertEqual(result["joins"]["duplicate_outcomes"], 1)
        self.assertEqual(result["selected"]["requests"], 2)
        self.assertEqual(result["selected"]["presented"], 1)
        self.assertEqual(result["intervals_ms"]["n"], 0)


class LegacyJoinTests(unittest.TestCase):
    def test_legacy_join_reports_a_different_request_and_an_unmatched_output(self):
        lines = [
            output_line(555, 10, 1, 1, 0, 0, 100_000, 200_000),
            output_line(555, 11, 2, 1, 0, 0, 300_000, 400_000),
            request_line(5001, 10, 555, 1, 0, 0, 150_000),
            request_line(5001, 11, 555, 1, 0, 0, 350_000),
            wayland_line(5001, "call", 10, started_ns=500_000, finished_ns=510_000, result=0),
            wayland_line(5001, "call", 11, started_ns=150_000, finished_ns=160_000, result=0),
            presented_line(5001, 10, 1_000_000),
            presented_line(5001, 11, 2_000_000),
        ]
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        result = analyze_file(write_log(Path(directory.name), lines), legacy=True)
        self.assertEqual(result["legacy"].get("outputs"), 2)
        self.assertEqual(result["legacy"].get("matched"), 1)
        self.assertEqual(result["legacy"].get("different"), 1)
        self.assertEqual(result["legacy"].get("unmatched"), 1)
        self.assertEqual(result["legacy"].get("ambiguous", 0), 0)


if __name__ == "__main__":
    unittest.main()

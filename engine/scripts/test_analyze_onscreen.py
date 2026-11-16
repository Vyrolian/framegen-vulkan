#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic check of analyze-onscreen.py (stdlib only, run as python3 -I).

A 2x run (real, generated, ...) with one replaced generated frame (its completion lands 0.1 ms after
the previous one) and one timed-out wait, and a 3x run with perfect spacing.
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ANALYZER = os.path.join(HERE, "analyze-onscreen.py")
BASE_NS = 1_000_000_000_000
FRAME_NS = 16_666_667
VK_SUCCESS = 0
VK_TIMEOUT = 2
DONE_LATENCY_NS = 3_000_000


def output_line(context, ordinal, lower_id, generated, started_ns):
    return ("framegen-vulkan: present diagnostics: operation=pacing-profile-output version=1 "
            "context=%d pid=1 swapchain=0x3000 ordinal=%d source=%d generated=%d deferred=1 "
            "image_index=0 scheduled_ns=%d started_ns=%d finished_ns=%d result=0 copy_checked=0 "
            "copy_status=0 copy_sampled_ns=0 arrival_ns=0 feedback=0 backend_frame=0 phase=0 "
            "present_id=%d ready_ns=0 slack_ns=0 offset_ns=0 app_present_id=0\n") % (
        context, ordinal, ordinal // 2, int(generated), started_ns, started_ns, started_ns + 200_000,
        lower_id)


def onscreen_line(context, lower_id, result, done_ns):
    return ("framegen-vulkan: present diagnostics: operation=pacing-profile-onscreen version=1 "
            "context=%d pid=1 lower_present_id=%d result=%d wait_entered_ns=%d done_ns=%d dropped=0\n") % (
        context, lower_id, result, done_ns - DONE_LATENCY_NS, done_ns)


def two_x_log():
    """Twenty outputs at 60 fps x 2. Output 5 is replaced (done 0.1 ms after output 4); output 9 times out."""
    lines = []
    done = {}
    for ordinal in range(20):
        started = BASE_NS + ordinal * FRAME_NS
        lower_id = 10 + ordinal
        lines.append(output_line(5, ordinal, lower_id, ordinal % 2 == 1, started))
        done[lower_id] = started + DONE_LATENCY_NS
    done[10 + 5] = done[10 + 4] + 100_000
    for ordinal in range(20):
        lower_id = 10 + ordinal
        if ordinal == 9:
            lines.append(onscreen_line(5, lower_id, VK_TIMEOUT, done[lower_id]))
        else:
            lines.append(onscreen_line(5, lower_id, VK_SUCCESS, done[lower_id]))
    return lines


def three_x_log():
    """Twelve outputs at 60 fps x 3, perfectly spaced: a real every third output."""
    lines = []
    for ordinal in range(12):
        started = BASE_NS + ordinal * FRAME_NS
        lower_id = 40 + ordinal
        lines.append(output_line(6, ordinal, lower_id, ordinal % 3 != 0, started))
        lines.append(onscreen_line(6, lower_id, VK_SUCCESS, started + DONE_LATENCY_NS))
    return lines


def run_analyzer(lines, extra=()):
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "profile.log")
        with open(path, "w", encoding="utf-8") as handle:
            handle.writelines(lines)
        completed = subprocess.run([sys.executable, "-I", ANALYZER, path, *extra],
                                   capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        raise SystemExit("analyzer failed (%d): %s%s" % (completed.returncode, completed.stdout,
                                                         completed.stderr))
    return completed.stdout


def expect(condition, message):
    if not condition:
        sys.stderr.write("FAIL: %s\n" % message)
        raise SystemExit(1)


def line_with(text, prefix):
    for line in text.splitlines():
        if line.startswith(prefix):
            return line
    return ""


def check_two_x():
    text = run_analyzer(two_x_log())
    expect("outputs: 20, with an on-screen completion: 19 (95.0%)" in text,
           "the timed-out wait is not a completion")
    expect("timeouts: 1, errors: 0" in text, "the timeout is counted once")
    expect("same-wake pairs (under 0.25 ms, the earlier one was likely replaced): 1" in text,
           "the replaced frame is one same-wake pair")
    error_line = line_with(text, "generated vs ideal |error| ms:")
    expect("n=8" in error_line, "eight generated outputs sit between two reals with a completion")
    expect("p50=0.000" in error_line, "the perfectly placed generated outputs have zero error at the median")
    expect("real->generated" in text and "generated->real" in text, "both transitions are reported")
    expect("windows of 2 s" in text and "reals/s=" in text, "the 2 s windows are printed")
    windowed = run_analyzer(two_x_log(), ("--from", "0.0", "--to", "0.1"))
    expect("with an on-screen completion: 19" in windowed, "coverage is over all outputs, not the window")
    selected = int(re.search(r"n=(\d+)", line_with(windowed, "present -> done ms:")).group(1))
    expect(0 < selected < 19, "the range filter restricts the selected outputs")


def check_three_x():
    text = run_analyzer(three_x_log())
    expect("outputs: 12, with an on-screen completion: 12 (100.0%)" in text, "every 3x output completes")
    expect("timeouts: 0, errors: 0" in text, "a clean 3x run has no timeouts or errors")
    expect("same-wake pairs (under 0.25 ms, the earlier one was likely replaced): 0" in text,
           "a clean 3x run has no same-wake pairs")
    error_line = line_with(text, "generated vs ideal |error| ms:")
    expect("n=6" in error_line and "p99=0.000" in error_line,
           "two generated outputs between each pair of reals are exactly on their ideal places")


def main():
    check_two_x()
    check_three_x()
    print("analyze-onscreen synthetic checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

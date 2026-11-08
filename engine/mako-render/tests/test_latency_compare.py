"""Portable contracts for the unattended input-latency comparison. No GPU and no probe launch."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import tomllib
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "scripts/compare-latency.py"
SPEC = importlib.util.spec_from_file_location("compare_latency", SCRIPT)
compare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(compare)

MS = 1_000_000
START_NS = 5_000_000_000
FRAME_HEADER = ("frame,present_id,sim_ns,event_count,present_call_ns,present_return_ns,"
                "present_result,complete_ns,wait_result\n")


def frame(present_id, sim_ns, event_count, *, hold_ns=0, complete_after_ns=10 * MS,
          result="VK_SUCCESS", complete=True):
    call = sim_ns
    returned = call + hold_ns
    complete_ns = returned + complete_after_ns if complete else 0
    return compare.FrameRow(
        frame=present_id - 1,
        present_id=present_id,
        sim_ns=sim_ns,
        event_count=event_count,
        present_call_ns=call,
        present_return_ns=returned,
        present_result=result,
        complete_ns=complete_ns,
        wait_result="VK_SUCCESS" if complete else "skipped",
    )


def synthetic_run(latency_ms, *, frames=60, spacing_ms=10):
    """Frames every spacing_ms; one event 5 ms before each frame after the first.

    Every event then maps to the next frame, so its latency is latency_ms + 5 ms exactly.
    """
    rows = []
    events = [START_NS + 5 * MS + k * spacing_ms * MS for k in range(frames - 1)]
    for index in range(frames):
        sim = START_NS + index * spacing_ms * MS
        count = sum(1 for event in events if event <= sim)
        rows.append(frame(index + 1, sim, count, complete_after_ns=latency_ms * MS))
    return rows, events


def summary_stub(mode, p50, p99, *, valid=True, cap=60, reflex="off", present_mode="fifo", gpu_load=0,
                 name=None, reasons=(), fps=60.0):
    return {
        "name": name or f"{mode}-{p50}-{valid}",
        "mode": mode,
        "cap": cap,
        "reflex": reflex,
        "present_mode": present_mode,
        "gpu_load": gpu_load,
        "valid": valid,
        "invalid_reasons": list(reasons),
        "event_latency_ms": {"n": 10, "mean": p50, "p50": p50, "p95": p99, "p99": p99, "max": p99},
        "frame_latency_ms": {"n": 10, "mean": p50, "p50": p50 - 1.0, "p95": p99, "p99": p99, "max": p99},
        "present_hold_ms": {"n": 10, "mean": 0.5, "p50": 0.5, "p95": 0.8, "p99": 0.9, "max": 1.0},
        "source_fps": fps,
    }


class EnvironmentTests(unittest.TestCase):
    INHERITED = {
        "MAKO_CONFIG": "/user/conf.toml",
        "MAKO_PACING_PROFILE_TRACE": "1",
        "NVPRESENT_QUEUE_FAMILY": "1",
        "MANGOHUD": "1",
        "MANGOHUD_CONFIG": "fps_limit=60",
        "ENABLE_MAKO": "1",
        "DISABLE_MAKO": "1",
        "ENABLE_MAKO_SPATIAL_SCALING": "1",
        "DISABLE_MAKO_SPATIAL_SCALING": "1",
        "DISABLE_LAYER_NV_PRESENT_1": "1",
        "VK_INSTANCE_LAYERS": "VK_LAYER_KHRONOS_validation",
        "VK_LAYER_PATH": "/layers",
        "VK_IMPLICIT_LAYER_PATH": "/implicit",
        "VK_ADD_IMPLICIT_LAYER_PATH": "/add",
        "ENABLE_VKBASALT": "1",
        "WAYLAND_DEBUG": "client",
        "PATH": "/usr/bin",
        "KEEP_ME": "yes",
    }

    def test_inherited_keys_are_removed_and_base_is_not_modified(self):
        base = dict(self.INHERITED)
        session = Path("/session")
        for mode in compare.MODES:
            env = compare.build_child_env(base, mode, session)
            expected = compare.mode_overrides(mode, session)
            for key in self.INHERITED:
                if key in ("PATH", "KEEP_ME"):
                    self.assertEqual(env[key], self.INHERITED[key], mode)
                    continue
                self.assertEqual(env.get(key), expected.get(key), f"{mode}: {key}")
        self.assertEqual(base, self.INHERITED)

    def test_fixed_keys_in_every_mode(self):
        for mode in compare.MODES:
            env = compare.build_child_env({}, mode, Path("/session"))
            self.assertEqual(env["DISABLE_MANGOHUD"], "1", mode)
            self.assertEqual(env["SDL_VIDEO_DRIVER"], "wayland", mode)

    def test_baseline_environment(self):
        env = compare.build_child_env({}, "baseline", Path("/session"))
        self.assertEqual(env["DISABLE_MAKO"], "1")
        self.assertEqual(env["DISABLE_MAKO_SPATIAL_SCALING"], "1")
        self.assertEqual(env["NVPRESENT_ENABLE_SMOOTH_MOTION"], "0")
        self.assertEqual(env["DISABLE_LAYER_NV_PRESENT_1"], "1")
        self.assertNotIn("ENABLE_MAKO", env)
        self.assertNotIn("VK_IMPLICIT_LAYER_PATH", env)
        self.assertNotIn("MAKO_CONFIG", env)

    def test_smooth_motion_environment(self):
        env = compare.build_child_env({}, "smooth-motion", Path("/session"))
        self.assertEqual(env["NVPRESENT_ENABLE_SMOOTH_MOTION"], "1")
        self.assertEqual(env["DISABLE_MAKO"], "1")
        self.assertEqual(env["DISABLE_MAKO_SPATIAL_SCALING"], "1")
        self.assertNotIn("DISABLE_LAYER_NV_PRESENT_1", env)
        self.assertNotIn("ENABLE_MAKO", env)

    def test_fgvk_environment_points_at_session_files(self):
        session = Path("/tmp/session-under-test")
        env = compare.build_child_env({}, "fgvk", session)
        self.assertEqual(env["ENABLE_MAKO"], "1")
        self.assertEqual(env["VK_IMPLICIT_LAYER_PATH"], str(session / "manifests"))
        self.assertEqual(env["MAKO_CONFIG"], str(session / "conf.toml"))
        self.assertEqual(env["NVPRESENT_ENABLE_SMOOTH_MOTION"], "0")
        self.assertEqual(env["DISABLE_LAYER_NV_PRESENT_1"], "1")
        self.assertNotIn("DISABLE_MAKO", env)

    def test_enable_mako_only_in_fgvk_mode(self):
        for mode in compare.MODES:
            env = compare.build_child_env({}, mode, Path("/session"))
            if mode == "fgvk":
                self.assertEqual(env["ENABLE_MAKO"], "1")
            else:
                self.assertNotIn("ENABLE_MAKO", env, mode)

    def test_smooth_motion_only_in_its_mode(self):
        values = {mode: compare.build_child_env({}, mode, Path("/session")).get("NVPRESENT_ENABLE_SMOOTH_MOTION")
                  for mode in compare.MODES}
        self.assertEqual(values, {"baseline": "0", "fgvk": "0", "smooth-motion": "1"})

    def test_unknown_mode_is_rejected(self):
        with self.assertRaises(ValueError):
            compare.build_child_env({}, "turbo", Path("/session"))


class ConfigAndManifestTests(unittest.TestCase):
    def test_config_parses_with_exact_values(self):
        dll = 'C:\\Games "x"/Lossless.dll'
        data = tomllib.loads(compare.render_config(dll))
        expected = {
            "version": 2,
            "current_profile": "latency probe",
            "global": {"allow_fp16": False, "dll": dll},
            "profile": [{
                "name": "latency probe",
                "active_in": ["fgvk-latency-probe"],
                "frame_generation_provisioned": True,
                "frame_generation_enabled": True,
                "adaptive": False,
                "multiplier": 2,
                "base_fps_cap": 0,
                "adaptive_stable_cadence": False,
                "dynamic_cadence_recovery": False,
                "gpu_headroom_percent": 0,
                "flow_scale": 1.0,
                "performance_mode": False,
            }],
        }
        self.assertEqual(data, expected)
        profile = data["profile"][0]
        self.assertIs(profile["frame_generation_provisioned"], True)
        self.assertIs(profile["frame_generation_enabled"], True)
        self.assertIs(profile["adaptive"], False)
        self.assertIs(profile["adaptive_stable_cadence"], False)
        self.assertIs(profile["dynamic_cadence_recovery"], False)
        self.assertIs(profile["performance_mode"], False)
        self.assertIs(data["global"]["allow_fp16"], False)
        self.assertIsInstance(profile["multiplier"], int)
        self.assertIsInstance(profile["flow_scale"], float)

    def test_toml_string_round_trips_control_characters(self):
        value = 'line\nbreak "quoted" \\ tab\t and del\x7f'
        self.assertEqual(tomllib.loads(f"v = {compare.toml_string(value)}")["v"], value)

    def test_read_global_dll(self):
        with tempfile.TemporaryDirectory() as tmp:
            config = Path(tmp) / "conf.toml"
            config.write_text('[global]\ndll = "/games/Lossless.dll"\n', encoding="utf-8")
            self.assertEqual(compare.read_global_dll(config), "/games/Lossless.dll")
            config.write_text("[global]\n", encoding="utf-8")
            with self.assertRaises(compare.SetupError):
                compare.read_global_dll(config)
            with self.assertRaises(compare.SetupError):
                compare.read_global_dll(Path(tmp) / "missing.toml")

    def test_rewrite_manifest_sets_library_path_only(self):
        manifest = {
            "file_format_version": "1.2.1",
            "layer": {"name": "VK_LAYER_MAKO_render", "library_path": "/old/libmako-render.so",
                      "enable_environment": {"ENABLE_MAKO": "1"}},
        }
        rewritten = compare.rewrite_manifest(manifest, "/new/libmako-render.so")
        self.assertEqual(rewritten["layer"]["library_path"], "/new/libmako-render.so")
        self.assertEqual(rewritten["layer"]["name"], "VK_LAYER_MAKO_render")
        self.assertEqual(rewritten["layer"]["enable_environment"], {"ENABLE_MAKO": "1"})
        self.assertEqual(manifest["layer"]["library_path"], "/old/libmako-render.so")
        with self.assertRaises(compare.SetupError):
            compare.rewrite_manifest({"layer": []}, "/x")

    def test_install_session_layer_writes_manifest_and_private_config(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            layer_build = root / "build"
            (layer_build / "mako-render").mkdir(parents=True)
            (layer_build / "mako-render" / "VkLayer_MAKO_render.json").write_text(
                json.dumps({"file_format_version": "1.2.1", "layer": {"library_path": "@X@"}}),
                encoding="utf-8")
            (layer_build / "mako-render" / "libmako-render.so").write_bytes(b"")
            session = root / "session"
            compare.create_session_dir(session)
            manifest_path, config_path = compare.install_session_layer(session, layer_build, "/games/Lossless.dll")
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual(manifest["layer"]["library_path"],
                             str((layer_build / "mako-render" / "libmako-render.so").absolute()))
            self.assertEqual(manifest_path, session / "manifests" / "VkLayer_MAKO_render.json")
            self.assertEqual(tomllib.loads(config_path.read_text(encoding="utf-8"))["global"]["dll"],
                             "/games/Lossless.dll")
            if os.name == "posix":
                self.assertEqual(stat.S_IMODE(config_path.stat().st_mode), 0o600)
                self.assertEqual(stat.S_IMODE(session.stat().st_mode), 0o700)

    def test_create_session_dir_refuses_existing_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(compare.SetupError):
                compare.create_session_dir(Path(tmp))


class MatrixAndActivationTests(unittest.TestCase):
    def test_matrix_is_complete_and_seeded(self):
        modes = ["baseline", "fgvk"]
        specs = compare.build_matrix(modes, [60, 0], [False, True], 3, seed=7)
        self.assertEqual(len(specs), 2 * 2 * 2 * 3)
        self.assertEqual(len(set(specs)), len(specs))
        self.assertEqual(specs, compare.build_matrix(modes, [60, 0], [False, True], 3, seed=7))
        self.assertEqual(sorted(specs, key=repr), sorted(compare.build_matrix(modes, [60, 0], [False, True],
                                                                              3, seed=8), key=repr))

    def test_activation_thresholds(self):
        self.assertEqual(compare.activation_verdict("fgvk", 200, 100)["status"], "active")
        self.assertEqual(compare.activation_verdict("smooth-motion", 150, 100)["status"], "not-active")
        self.assertEqual(compare.activation_verdict("baseline", 100, 100)["status"], "ok")
        self.assertEqual(compare.activation_verdict("baseline", 180, 100)["status"], "not-ok")
        self.assertEqual(compare.activation_verdict("fgvk", 5, 0)["status"], "no-presents")
        self.assertEqual(compare.activation_verdict("fgvk", 200, 100)["ratio"], 2.0)

    def test_wayland_attach_counting_on_synthetic_log(self):
        # libwayland prints proxies as wl_surface#N (real logs) or wl_surface@N; both count.
        # Commit lines never count, and attachx or non-request lines do not match.
        log = "\n".join([
            "[1.001] -> wl_surface#3.attach(wl_buffer#9, 0, 0)",
            "[1.002] -> wl_surface#3.commit()",
            "[1.003] -> wl_surface#3.damage_buffer(0, 0, 1280, 720)",
            "[1.004] -> wl_surface@12.attach(wl_buffer@4, 0, 0)",
            "[1.005] -> wl_surface@12.commit()",
            "[1.006] -> wl_surface#7.attach(wl_buffer#2, 0, 0)",
            "[1.007] <- wl_callback@9.done(12)",
            "[1.008] -> wl_surface#3.attachx(wl_buffer#9, 0, 0)",
            "[1.009] -> wl_surface@12.attach(",
            "[1.010] -> wl_surface@7.commit(",
        ])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "wayland.log"
            path.write_text(log, encoding="utf-8")
            self.assertEqual(compare.count_wayland_attaches(path), 4)
            self.assertEqual(compare.count_wayland_attaches(Path(tmp) / "absent.log"), 0)

    def test_commit_only_log_counts_no_attaches(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "wayland.log"
            path.write_text("[1.0] -> wl_surface#3.commit()\n[1.1] -> wl_surface@3.commit()\n",
                            encoding="utf-8")
            self.assertEqual(compare.count_wayland_attaches(path), 0)

    def test_analyze_recomputes_activation_from_logs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "session"
            (root / "activation" / "1-fgvk-cap60-reflexoff" / "probe").mkdir(parents=True)
            (root / "activation" / "1-fgvk-cap60-reflexoff" / "wayland.log").write_text(
                "".join("[0.1] -> wl_surface#3.attach(wl_buffer#9, 0, 0)\n" for _ in range(195))
                + "".join("[0.2] -> wl_surface#3.commit()\n" for _ in range(195)),
                encoding="utf-8")
            frame_rows = FRAME_HEADER + "".join(
                f"{i},{i + 1},{i},0,{i},{i},VK_SUCCESS,0,skipped\n" for i in range(100))
            (root / "activation" / "1-fgvk-cap60-reflexoff" / "probe" / "frames.csv").write_text(
                frame_rows, encoding="utf-8")
            stored_wrong = {"mode": "fgvk", "cap": 60, "dir": "activation/1-fgvk-cap60-reflexoff",
                            "attaches": 1, "presents": 1, "ratio": 1.0, "status": "not-active"}
            stored_only = {"mode": "smooth-motion", "cap": 60, "dir": "activation/missing",
                           "attaches": 5, "presents": 5, "ratio": 1.0, "status": "not-active"}
            session = {"args": {"warmup": 0.0, "duration": 0.6, "repeats": 1, "seed": 1, "gpu_load": 0,
                                "present_mode": "fifo"},
                       "activation": [stored_wrong, stored_only], "runs": []}
            (root / "session.json").write_text(json.dumps(session), encoding="utf-8")

            report = compare.analyze_session(root)

            by_mode = {entry["mode"]: entry for entry in report["activation"]}
            self.assertEqual(by_mode["fgvk"]["attaches"], 195)
            self.assertEqual(by_mode["fgvk"]["presents"], 100)
            self.assertAlmostEqual(by_mode["fgvk"]["ratio"], 1.95)
            self.assertEqual(by_mode["fgvk"]["status"], "active")
            self.assertEqual(by_mode["fgvk"]["source"], "recomputed")
            self.assertEqual(by_mode["smooth-motion"]["source"], "stored")
            self.assertEqual(by_mode["smooth-motion"]["ratio"], 1.0)
            self.assertIn("attaches=195", report["text"])
            self.assertIn("buffer attaches per successful present", report["text"])


class PercentileTests(unittest.TestCase):
    def test_linear_interpolation(self):
        self.assertEqual(compare.percentile([1, 2, 3, 4], 50), 2.5)
        self.assertAlmostEqual(compare.percentile(list(range(1, 11)), 95), 9.55)
        self.assertAlmostEqual(compare.percentile(list(range(1, 11)), 99), 9.91)
        self.assertEqual(compare.percentile([5], 99), 5.0)
        self.assertIsNone(compare.percentile([], 50))

    def test_distribution_fields(self):
        stats = compare.distribution([4.0, 1.0, 3.0, 2.0])
        self.assertEqual(stats["n"], 4)
        self.assertEqual(stats["mean"], 2.5)
        self.assertEqual(stats["p50"], 2.5)
        self.assertEqual(stats["max"], 4.0)
        empty = compare.distribution([])
        self.assertEqual(empty["n"], 0)
        self.assertIsNone(empty["p99"])


class RunSummaryTests(unittest.TestCase):
    def test_event_maps_to_lowest_frame_with_higher_count(self):
        frames = [frame(1, START_NS + 10 * MS, 1), frame(2, START_NS + 20 * MS, 2),
                  frame(3, START_NS + 30 * MS, 3)]
        events = [START_NS + 5 * MS, START_NS + 15 * MS, START_NS + 25 * MS]
        summary = compare.summarize_run(frames, events, [], start_ns=START_NS, warmup_s=0.0,
                                        exit_code=0, timed_out=False)
        self.assertTrue(summary["valid"], summary["invalid_reasons"])
        self.assertEqual(summary["event_latency_ms"]["n"], 3)
        self.assertEqual(summary["event_latency_ms"]["mean"], 15.0)
        self.assertEqual(summary["events_uncompleted"], 0)

    def test_warmup_excludes_early_frames_and_events(self):
        frames = [frame(1, START_NS + 10 * MS, 1), frame(2, START_NS + 20 * MS, 2),
                  frame(3, START_NS + 30 * MS, 3)]
        events = [START_NS + 5 * MS, START_NS + 15 * MS, START_NS + 25 * MS]
        summary = compare.summarize_run(frames, events, [], start_ns=START_NS, warmup_s=0.015,
                                        exit_code=0, timed_out=False)
        self.assertEqual(summary["frames_kept"], 2)
        self.assertEqual(summary["events_measured"], 2)
        self.assertEqual(summary["event_latency_ms"]["n"], 2)
        self.assertEqual(summary["event_latency_ms"]["p50"], 15.0)

    def test_event_after_last_frame_is_uncompleted_without_frame(self):
        frames = [frame(1, START_NS + 10 * MS, 1)]
        events = [START_NS + 5 * MS, START_NS + 50 * MS]
        summary = compare.summarize_run(frames, events, [], start_ns=START_NS, warmup_s=0.0,
                                        exit_code=0, timed_out=False)
        self.assertEqual(summary["event_latency_ms"]["n"], 1)
        self.assertEqual(summary["events_uncompleted"], 1)
        self.assertEqual(summary["events_without_frame"], 1)

    def test_event_whose_frame_did_not_complete_is_counted(self):
        frames = [frame(1, START_NS + 10 * MS, 1, complete=False), frame(2, START_NS + 20 * MS, 2)]
        events = [START_NS + 5 * MS]
        summary = compare.summarize_run(frames, events, [], start_ns=START_NS, warmup_s=0.0,
                                        exit_code=0, timed_out=False)
        self.assertEqual(summary["event_latency_ms"]["n"], 0)
        self.assertEqual(summary["events_uncompleted"], 1)
        self.assertEqual(summary["events_without_frame"], 0)

    def test_invalid_reasons(self):
        good = [frame(i + 1, START_NS + i * MS, 0) for i in range(50)]

        timeout = compare.summarize_run(good, [], [], start_ns=START_NS, warmup_s=0.0, exit_code=-15,
                                        timed_out=True)
        self.assertIn("timeout", timeout["invalid_reasons"])
        self.assertFalse(timeout["valid"])

        exited = compare.summarize_run(good, [], [], start_ns=START_NS, warmup_s=0.0, exit_code=2,
                                       timed_out=False)
        self.assertIn("probe-exit-2", exited["invalid_reasons"])

        problem = compare.summarize_run(good, [], [], start_ns=START_NS, warmup_s=0.0, exit_code=0,
                                        timed_out=False, problems=["missing-output-window.csv"])
        self.assertIn("missing-output-window.csv", problem["invalid_reasons"])

        missing_meta = compare.summarize_run(good, [], [], start_ns=None, warmup_s=0.0, exit_code=0,
                                             timed_out=False)
        self.assertEqual(missing_meta["invalid_reasons"], ["missing-meta"])

        empty = compare.summarize_run([], [], [], start_ns=START_NS, warmup_s=0.0, exit_code=0,
                                      timed_out=False)
        self.assertIn("no-kept-frames", empty["invalid_reasons"])

    def test_window_hidden_states_after_warmup_are_invalid(self):
        good = [frame(i + 1, START_NS + i * MS, 0) for i in range(50)]
        for name in ("hidden", "occluded", "minimized"):
            early = [compare.WindowEvent(START_NS + 2 * MS, name)]
            late = [compare.WindowEvent(START_NS + 20 * MS, name)]
            before = compare.summarize_run(good, [], early, start_ns=START_NS, warmup_s=0.01, exit_code=0,
                                           timed_out=False)
            after = compare.summarize_run(good, [], late, start_ns=START_NS, warmup_s=0.01, exit_code=0,
                                          timed_out=False)
            self.assertTrue(before["valid"], name)
            self.assertIn(f"window-{name}", after["invalid_reasons"])
        focus = compare.summarize_run(good, [], [compare.WindowEvent(START_NS + 20 * MS, "focus_lost")],
                                      start_ns=START_NS, warmup_s=0.0, exit_code=0, timed_out=False)
        self.assertTrue(focus["valid"])

    def test_incomplete_frame_threshold_is_one_percent(self):
        def run_with_incomplete(count, total):
            rows = [frame(i + 1, START_NS + i * MS, 0, complete=(i >= count)) for i in range(total)]
            return compare.summarize_run(rows, [], [], start_ns=START_NS, warmup_s=0.0, exit_code=0,
                                         timed_out=False)

        self.assertTrue(run_with_incomplete(1, 200)["valid"])
        self.assertIn("incomplete-frames", run_with_incomplete(2, 100)["invalid_reasons"])

    def test_completion_before_present_return_is_invalid(self):
        rows = [frame(1, START_NS, 0), frame(2, START_NS + MS, 0)]
        rows[1] = compare.FrameRow(**{**rows[1].__dict__, "present_return_ns": START_NS + 9 * MS,
                                      "complete_ns": START_NS + 3 * MS})
        summary = compare.summarize_run(rows, [], [], start_ns=START_NS, warmup_s=0.0, exit_code=0,
                                        timed_out=False)
        self.assertIn("completion-before-present-return", summary["invalid_reasons"])

    def test_completions_must_not_decrease_by_present_id(self):
        rows = [frame(1, START_NS, 0, complete_after_ns=50 * MS),
                frame(2, START_NS + MS, 0, complete_after_ns=10 * MS)]
        summary = compare.summarize_run(rows, [], [], start_ns=START_NS, warmup_s=0.0, exit_code=0,
                                        timed_out=False)
        self.assertIn("completion-order", summary["invalid_reasons"])

    def test_source_fps_intervals_and_present_hold(self):
        spacing = round(1_000_000_000 / 60)
        rows = []
        for index in range(61):
            sim = START_NS + index * spacing
            rows.append(compare.FrameRow(frame=index, present_id=index + 1, sim_ns=sim, event_count=0,
                                         present_call_ns=sim, present_return_ns=sim + 2 * MS,
                                         present_result="VK_SUCCESS", complete_ns=sim + 20 * MS,
                                         wait_result="VK_SUCCESS"))
        summary = compare.summarize_run(rows, [], [], start_ns=START_NS, warmup_s=0.0, exit_code=0,
                                        timed_out=False)
        self.assertAlmostEqual(summary["source_fps"], 60.0, places=3)
        self.assertAlmostEqual(summary["completion_interval_median_ms"], spacing / MS, places=6)
        self.assertEqual(summary["present_hold_ms"]["p50"], 2.0)

    def test_present_hold_covers_every_present_call(self):
        rows = [frame(1, START_NS, 0, hold_ns=3 * MS),
                frame(2, START_NS + MS, 0, hold_ns=9 * MS, result="VK_ERROR_OUT_OF_DATE_KHR", complete=False)]
        summary = compare.summarize_run(rows, [], [], start_ns=START_NS, warmup_s=0.0, exit_code=0,
                                        timed_out=False)
        self.assertEqual(summary["present_hold_ms"]["n"], 2)
        self.assertEqual(summary["present_hold_ms"]["p50"], 6.0)
        self.assertEqual(summary["present_hold_ms"]["max"], 9.0)

    def test_csv_parsers(self):
        frames = compare.parse_frames_csv(
            FRAME_HEADER + "0,1,100,0,100,101,VK_SUCCESS,0,skipped\n"
            "1,2,200,1,200,201,VK_SUBOPTIMAL_KHR,250,VK_SUCCESS\n")
        self.assertEqual([row.present_id for row in frames], [1, 2])
        self.assertEqual(frames[1].complete_ns, 250)
        self.assertEqual(compare.parse_events_csv("index,event_ns\n1,20\n0,10\n"), [10, 20])
        self.assertEqual(compare.parse_window_csv("ns,event\n5,hidden\n"),
                         [compare.WindowEvent(ns=5, name="hidden")])


class ComparisonTests(unittest.TestCase):
    def test_deltas_use_valid_run_medians(self):
        runs = [
            summary_stub("baseline", 10.0, 20.0, name="b1"),
            summary_stub("baseline", 11.0, 21.0, name="b2"),
            summary_stub("baseline", 12.0, 22.0, name="b3"),
            summary_stub("fgvk", 8.0, 15.0, name="f1"),
            summary_stub("fgvk", 9.0, 16.0, name="f2"),
            summary_stub("fgvk", 10.0, 17.0, name="f3"),
        ]
        activation = {"fgvk": {"status": "active"}}
        comparison = compare.compare_runs(runs, activation)
        self.assertEqual(len(comparison), 1)
        rows = {row["mode"]: row for row in comparison[0]["modes"]}
        self.assertEqual(rows["baseline"]["metrics"]["event_p50_ms"]["median"], 11.0)
        self.assertEqual(rows["fgvk"]["metrics"]["event_p50_ms"]["min"], 8.0)
        self.assertEqual(rows["fgvk"]["metrics"]["event_p50_ms"]["max"], 10.0)
        self.assertAlmostEqual(rows["fgvk"]["delta_event_p50_ms"], -2.0)
        self.assertAlmostEqual(rows["fgvk"]["delta_event_p99_ms"], -5.0)
        self.assertIsNone(rows["baseline"]["delta_event_p50_ms"])
        # fgvk median p50 (9.0) is 2 ms below the baseline median (11.0), beyond the 0.5 ms margin.
        self.assertEqual(rows["fgvk"]["flags"], ["suspect"])

    def test_invalid_runs_are_excluded(self):
        runs = [
            summary_stub("baseline", 10.0, 20.0, name="b1"),
            summary_stub("baseline", 100.0, 200.0, name="b2", valid=False, reasons=["timeout"]),
            summary_stub("baseline", 12.0, 22.0, name="b3"),
        ]
        rows = {row["mode"]: row for row in compare.compare_runs(runs, {})[0]["modes"]}
        self.assertEqual(rows["baseline"]["runs_valid"], 2)
        self.assertEqual(rows["baseline"]["metrics"]["event_p50_ms"]["median"], 11.0)
        self.assertEqual(rows["baseline"]["invalid_runs"][0]["reasons"], ["timeout"])

    def test_activation_unverified_flag(self):
        runs = [summary_stub("baseline", 10.0, 20.0, name="b1"), summary_stub("baseline", 10.5, 20.5, name="b2"),
                summary_stub("fgvk", 10.0, 20.0, name="f1"), summary_stub("fgvk", 10.0, 20.0, name="f2"),
                summary_stub("smooth-motion", 10.0, 20.0, name="s1"),
                summary_stub("smooth-motion", 10.0, 20.0, name="s2")]
        rows = {row["mode"]: row for row in compare.compare_runs(runs, {"fgvk": {"status": "active"},
                                                                           "smooth-motion": {"status": "not-active"}})[0]["modes"]}
        self.assertNotIn("activation-unverified", rows["fgvk"]["flags"])
        self.assertIn("activation-unverified", rows["smooth-motion"]["flags"])
        self.assertNotIn("activation-unverified", rows["baseline"]["flags"])
        none_checked = {row["mode"]: row for row in compare.compare_runs(runs, {})[0]["modes"]}
        self.assertIn("activation-unverified", none_checked["fgvk"]["flags"])

    def test_suspect_flag_threshold(self):
        def flags_for(fgvk_p50):
            runs = [summary_stub("baseline", 10.0, 20.0, name="b1"),
                    summary_stub("baseline", 10.0, 20.0, name="b2"),
                    summary_stub("fgvk", fgvk_p50, 20.0, name="f1"),
                    summary_stub("fgvk", fgvk_p50, 20.0, name="f2")]
            rows = {row["mode"]: row for row in compare.compare_runs(runs, {"fgvk": {"status": "active"}})[0]["modes"]}
            return rows["fgvk"]["flags"]

        self.assertIn("suspect", flags_for(9.4))
        self.assertNotIn("suspect", flags_for(9.6))
        self.assertNotIn("suspect", flags_for(12.0))

    def test_insufficient_flag_needs_two_valid_runs(self):
        runs = [summary_stub("baseline", 10.0, 20.0, name="b1"), summary_stub("baseline", 10.0, 20.0, name="b2"),
                summary_stub("fgvk", 9.0, 19.0, name="f1"),
                summary_stub("fgvk", 9.0, 19.0, name="f2", valid=False, reasons=["window-hidden"])]
        rows = {row["mode"]: row for row in compare.compare_runs(runs, {"fgvk": {"status": "active"}})[0]["modes"]}
        self.assertIn("insufficient", rows["fgvk"]["flags"])
        self.assertNotIn("insufficient", rows["baseline"]["flags"])

    def test_groups_split_by_cap_reflex_present_and_gpu_load(self):
        runs = [summary_stub("baseline", 10.0, 20.0, name="a", cap=60, reflex="off"),
                summary_stub("baseline", 10.0, 20.0, name="b", cap=60, reflex="on"),
                summary_stub("baseline", 10.0, 20.0, name="c", cap=0, reflex="off", gpu_load=4),
                summary_stub("baseline", 10.0, 20.0, name="d", cap=60, reflex="off", present_mode="mailbox")]
        groups = compare.compare_runs(runs, {})
        self.assertEqual(len(groups), 4)
        self.assertEqual({(g["cap"], g["reflex"], g["present_mode"], g["gpu_load"]) for g in groups},
                         {(60, "off", "fifo", 0), (60, "on", "fifo", 0), (0, "off", "fifo", 4),
                          (60, "off", "mailbox", 0)})


class ReportFromSessionTests(unittest.TestCase):
    def _write_run(self, root, name, *, mode, reflex, rows, events, windows=(), returncode=0,
                   timed_out=False, start_ns=START_NS, write_events=True):
        probe = root / "runs" / name / "probe"
        probe.mkdir(parents=True)
        (probe / "frames.csv").write_text(FRAME_HEADER + "".join(
            f"{r.frame},{r.present_id},{r.sim_ns},{r.event_count},{r.present_call_ns},"
            f"{r.present_return_ns},{r.present_result},{r.complete_ns},{r.wait_result}\n" for r in rows),
            encoding="utf-8")
        if write_events:
            (probe / "events.csv").write_text("index,event_ns\n" + "".join(
                f"{i},{t}\n" for i, t in enumerate(events)), encoding="utf-8")
        (probe / "window.csv").write_text("ns,event\n" + "".join(
            f"{w.ns},{w.name}\n" for w in windows), encoding="utf-8")
        (probe / "meta.json").write_text(json.dumps({"start_ns": start_ns}), encoding="utf-8")
        return {"name": name, "mode": mode, "cap": 60, "reflex": reflex, "repeat": 1,
                "present_mode": "fifo", "gpu_load": 0, "returncode": returncode, "timed_out": timed_out,
                "dir": f"runs/{name}"}

    def test_session_report_end_to_end(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "session"
            root.mkdir()
            records = []
            for index, (mode, latency) in enumerate([("baseline", 20), ("baseline", 20), ("fgvk", 15),
                                                      ("fgvk", 15)], start=1):
                rows, events = synthetic_run(latency)
                records.append(self._write_run(root, f"{index}-{mode}", mode=mode, reflex="off",
                                               rows=rows, events=events))
            rows, events = synthetic_run(15)
            records.append(self._write_run(root, "5-smooth-motion", mode="smooth-motion", reflex="off",
                                           rows=rows, events=events, returncode=2))
            session = {"args": {"warmup": 0.0, "duration": 0.6, "repeats": 1, "seed": 1, "gpu_load": 0,
                                "present_mode": "fifo"},
                       "activation": [{"mode": "fgvk", "cap": 60, "attaches": 200, "presents": 100,
                                       "ratio": 2.0, "status": "active"}],
                       "runs": records}
            (root / "session.json").write_text(json.dumps(session), encoding="utf-8")

            report = compare.analyze_session(root)

            self.assertTrue((root / "report.json").is_file())
            self.assertTrue((root / "report.txt").is_file())
            self.assertIn("not photons", (root / "report.txt").read_text(encoding="utf-8"))
            self.assertIn("No smoothness claim", report["text"])
            by_name = {run["name"]: run for run in report["runs"]}
            self.assertTrue(by_name["1-baseline"]["valid"])
            self.assertAlmostEqual(by_name["1-baseline"]["event_latency_ms"]["p50"], 25.0)
            self.assertAlmostEqual(by_name["3-fgvk"]["event_latency_ms"]["p50"], 20.0)
            self.assertAlmostEqual(by_name["3-fgvk"]["source_fps"], 100.0, places=6)
            self.assertFalse(by_name["5-smooth-motion"]["valid"])
            self.assertIn("probe-exit-2", by_name["5-smooth-motion"]["invalid_reasons"])
            rows = {row["mode"]: row for row in report["comparison"][0]["modes"]}
            self.assertAlmostEqual(rows["fgvk"]["delta_event_p50_ms"], -5.0)
            self.assertIn("suspect", rows["fgvk"]["flags"])
            self.assertEqual(rows["fgvk"]["flags"].count("insufficient"), 0)
            self.assertIn("insufficient", rows["smooth-motion"]["flags"])
            self.assertIn("activation-unverified", rows["smooth-motion"]["flags"])
            self.assertIsNone(rows["baseline"]["activation_status"])

    def test_missing_outputs_make_a_run_invalid(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "session"
            root.mkdir()
            rows, events = synthetic_run(15)
            record = self._write_run(root, "1-fgvk", mode="fgvk", reflex="off", rows=rows, events=events,
                                     write_events=False)
            session = {"args": {"warmup": 0.0, "duration": 0.6, "repeats": 1, "seed": 1, "gpu_load": 0,
                                "present_mode": "fifo"},
                       "activation": [], "runs": [record]}
            (root / "session.json").write_text(json.dumps(session), encoding="utf-8")
            report = compare.analyze_session(root)
            self.assertFalse(report["runs"][0]["valid"])
            self.assertIn("missing-output-events.csv", report["runs"][0]["invalid_reasons"])
            self.assertEqual(report["runs"][0]["events_measured"], 0)


class ProcessTests(unittest.TestCase):
    def test_timeout_terminates_the_process_group(self):
        # A stand-in child that sleeps. The probe itself is never launched by these tests.
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "child.log"
            result = compare.run_child([sys.executable, "-I", "-c", "import time; time.sleep(60)"], {},
                                       timeout_s=0.2, log_path=log)
        self.assertTrue(result.timed_out)
        self.assertIsNotNone(result.returncode)
        self.assertNotEqual(result.returncode, 0)


class CommandLineTests(unittest.TestCase):
    def test_help_exits_cleanly(self):
        for argv in (["--help"], ["run", "--help"]):
            with contextlib.redirect_stdout(io.StringIO()) as out:
                with self.assertRaises(SystemExit) as raised:
                    compare.build_parser().parse_args(argv)
            self.assertEqual(raised.exception.code, 0)
            self.assertIn("usage", out.getvalue())

    def test_choice_lists_reject_unknown_values(self):
        with self.assertRaises(compare.SetupError):
            compare._parse_choice_list("baseline,turbo", compare.MODES, "--modes")
        with self.assertRaises(compare.SetupError):
            compare._parse_int_list("60,fast", "--caps", minimum=0)
        self.assertEqual(compare._parse_choice_list("off,on,off", compare.REFLEX_CHOICES, "--reflex"),
                         ["off", "on"])


if __name__ == "__main__":
    unittest.main()

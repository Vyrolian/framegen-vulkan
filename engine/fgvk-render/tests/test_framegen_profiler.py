"""Portable contracts for the external, Nsight-free comparison profiler."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
import textwrap


SCRIPT = Path(__file__).resolve().parents[2] / "scripts/profile-framegen.py"
SPEC = importlib.util.spec_from_file_location("framegen_profiler", SCRIPT)
profiler = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(profiler)


class ProfilerTests(unittest.TestCase):
    def test_modes_preserve_wrapper_environment_and_do_not_stack_fg(self):
        original = dict(ENABLE_FGVK="1", ENABLE_FGVK_SPATIAL_SCALING="1",
                        FGVK_PACING_PROFILE_TRACE="1", FGVK_PACING_PROFILE_GPU="1",
                        NVPRESENT_ENABLE_SMOOTH_MOTION="1", DISABLE_LAYER_NV_PRESENT_1="",
                        MANGOHUD_CONFIG="fps_limit=60,no_display", MANGOHUD_CONFIGFILE="/my/config",
                        NVPRESENT_QUEUE_FAMILY="1", CUSTOM_SECRET="keep-in-child",
                        VK_INSTANCE_LAYERS="VK_LAYER_FGVK_render:VK_LAYER_other:VK_LAYER_FGVK_spatial_scaling")
        for mode in ("baseline", "fgvk", "smooth-motion"):
            env = profiler.environment(original, mode, Path("/tmp/logs"), 30, 60, 0)
            self.assertEqual(env["FGVK_PACING_PROFILE_TRACE"], "0")
            self.assertEqual(env["FGVK_PACING_PROFILE_GPU"], "0")
            self.assertEqual(env["CUSTOM_SECRET"], "keep-in-child")
            self.assertEqual(env["MANGOHUD_CONFIGFILE"], "/my/config")
            self.assertEqual(env["NVPRESENT_QUEUE_FAMILY"], "1")
            self.assertTrue(env["MANGOHUD_CONFIG"].startswith("fps_limit=60,no_display,"))
            self.assertIn("permit_upload=0,upload_logs=0", env["MANGOHUD_CONFIG"])
            self.assertEqual(env["NVPRESENT_ENABLE_SMOOTH_MOTION"], "1" if mode == "smooth-motion" else "0")
            if mode == "fgvk":
                self.assertEqual(env["DISABLE_FGVK"], "0")
                self.assertEqual(env["VK_INSTANCE_LAYERS"], original["VK_INSTANCE_LAYERS"])
            else:
                self.assertEqual(env["DISABLE_FGVK"], "1")
                self.assertEqual(env["DISABLE_FGVK_SPATIAL_SCALING"], "1")
                self.assertEqual(env["VK_INSTANCE_LAYERS"], "VK_LAYER_other")
            self.assertEqual("DISABLE_LAYER_NV_PRESENT_1" in env, mode != "smooth-motion")
        self.assertEqual(original["FGVK_PACING_PROFILE_TRACE"], "1")

    def test_frame_csv_preamble_invalid_values_and_sampling_domain(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "game.csv"
            path.write_text("v1\n0.8\nos,cpu,gpu\nprivate metadata\nfps,frametime,elapsed\n"
                            "100,10,1000000000\n50,20,1020000000\n10,100,1120000000\n"
                            "0,nan,1200000000\n0,-1,1200000000\n100,10,1000000000\npartial\n")
            report = profiler.read_frames(path, 0)
            self.assertEqual(report["frametime_ms"]["samples"], 3)
            self.assertEqual(report["rejected_rows"], 4)
            self.assertAlmostEqual(report["observed_fps"], 3000 / 130)
            self.assertEqual(report["frames_over_50ms"], 1)
            self.assertIsNone(profiler.read_frames(path, 100)["observed_fps"])

    def test_gpu_window_missing_sensors_and_energy_do_not_bridge_gaps(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "telemetry.jsonl"
            rows = [dict(seconds=0, power_w=999), dict(seconds=1, power_w=10),
                    dict(seconds=1.5, power_w=30), dict(seconds=2, power_w=None),
                    dict(seconds=2.5, power_w=50), dict(seconds=6, power_w=60),
                    dict(seconds=7, power_w=999)]
            path.write_text("".join(json.dumps(row) + "\n" for row in rows) + "broken\n")
            report = profiler.telemetry_report(path, 1, 6)
            self.assertEqual(report["samples"], 5)
            self.assertEqual(report["power_w"]["samples"], 4)
            self.assertEqual(report["energy_j"], 10)
            self.assertEqual(report["power_covered_seconds"], .5)
            self.assertIsNone(report["vram_mib"])
            self.assertEqual(report["rejected_rows"], 1)
        self.assertIsNone(profiler.number("[N/A]"))
        self.assertIsNone(profiler.number("inf"))

    def run_capture(self, directory, child_code, extra=(), env=None):
        return subprocess.run([sys.executable, str(SCRIPT), "capture", "--mode", "baseline",
                               "--scenario", "fixture", "--warmup", "1", "--duration", "1",
                               "--output", str(directory), *extra, "--", sys.executable, "-c", child_code],
                              env=env, text=True, capture_output=True, timeout=12)

    def test_launch_arguments_exit_status_private_files_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory) / "capture"
            code = ("import os,sys; assert os.environ['DISABLE_FGVK']=='1'; "
                    "assert os.environ['FGVK_PACING_PROFILE_TRACE']=='0'; print('child output'); sys.exit(7)")
            result = self.run_capture(folder, code, env=os.environ | {"PATH": "/nonexistent", "MY_TOKEN": "not-for-report"})
            self.assertEqual(result.returncode, 7, result.stderr)
            self.assertIn("child output", result.stdout)
            self.assertEqual(folder.stat().st_mode & 0o777, 0o700)
            for name in ("metadata.json", "report.json", "report.txt", "telemetry.jsonl", "sampler-errors.log"):
                self.assertEqual((folder / name).stat().st_mode & 0o777, 0o600)
                self.assertNotIn("not-for-report", (folder / name).read_text())
            report = json.loads((folder / "report.json").read_text())
            self.assertIsNone(report["input_latency_ms"])
            self.assertIsNone(report["gpu_execution_ms"])
            self.assertEqual(report["frame_streams"], [])
            self.assertNotEqual(self.run_capture(folder, "pass").returncode, 0)

    def test_fake_gpu_stream_selection_and_child_outlives_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            binary = base / "nvidia-smi"
            binary.write_text(f"#!{sys.executable}\nimport time\n"
                              "for i in range(50):\n print('GPU-test, Test GPU, test-driver, 40, 70, 3000, 900, 1000, 55', flush=True); time.sleep(.1)\n")
            binary.chmod(0o700)
            started = time.monotonic()
            result = self.run_capture(base / "capture", "import time; time.sleep(2.5)",
                                      env=os.environ | {"PATH": str(base)})
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertGreaterEqual(time.monotonic() - started, 2.5)
            report = json.loads((base / "capture/report.json").read_text())
            self.assertGreater(report["telemetry"]["samples"], 0)
            self.assertEqual(report["gpu"]["uuid"], "GPU-test")
            self.assertEqual(report["telemetry"]["power_w"]["mean"], 40)

    def test_comparison_warns_on_unmatched_settings_and_keeps_streams(self):
        with tempfile.TemporaryDirectory() as directory:
            folders = []
            for mode in ("baseline", "fgvk"):
                folder = Path(directory) / mode
                folder.mkdir()
                (folder / "mangohud").mkdir()
                (folder / "telemetry.jsonl").write_text("")
                metadata = dict(mode=mode, scenario=mode, reflex="unknown", frame_domain="unknown",
                                gpu=None, warmup_seconds=1, duration_seconds=1, frame_log_interval_ms=0)
                report = profiler.summarize(folder, metadata)
                profiler.write_json(folder / "report.json", report)
                folders.append(folder)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                profiler.compare(folders)
            self.assertIn("scenario differs", output.getvalue())
            self.assertIn("unavailable", output.getvalue())
            self.assertIn("No input-latency", output.getvalue())

    def test_termination_is_forwarded_and_report_is_retained(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory) / "capture"
            ready = Path(directory) / "ready"
            code = f"from pathlib import Path; import time; Path({str(ready)!r}).touch(); time.sleep(20)"
            command = [sys.executable, str(SCRIPT), "capture", "--mode", "baseline", "--scenario", "signal",
                       "--warmup", "1", "--duration", "1", "--output", str(folder), "--", sys.executable, "-c", code]
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       env=os.environ | {"PATH": "/nonexistent"})
            try:
                deadline = time.monotonic() + 5
                while not ready.exists() and time.monotonic() < deadline:
                    time.sleep(.02)
                self.assertTrue(ready.exists())
                process.send_signal(signal.SIGTERM)
                stdout, stderr = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 128 + signal.SIGTERM, stderr)
                self.assertTrue((folder / "report.json").is_file())
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_rejects_known_isolating_launchers_before_creating_a_run(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory) / "capture"
            result = subprocess.run([sys.executable, str(SCRIPT), "capture", "--mode", "baseline",
                                     "--scenario", "fixture", "--output", str(folder), "--",
                                     "mangohud", "/path/to/fgvk-launch", "game"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn("isolate layers", result.stderr)
            self.assertFalse(folder.exists())

    def test_manual_disconnect_does_not_claim_a_successful_stop(self):
        class ClosedConnection:
            def recv(self, _size):
                return b""

            def close(self):
                pass

        control = profiler.RecordingControl(Path("/unused"), "unused")
        control.connection = ClosedConnection()
        control.started = time.monotonic()
        control.poll()
        self.assertIsNotNone(control.stopped)
        self.assertFalse(control.confirmed)
        self.assertIn("unconfirmed", control.message)

    def test_manual_unknown_control_version_cannot_start_capture(self):
        class FutureConnection:
            def recv(self, _size):
                return b":MangoHudControlVersion=2;"

            def close(self):
                pass

        control = profiler.RecordingControl(Path("/unused"), "unused")
        control.connection = FutureConnection()
        control.poll()
        self.assertIsNone(control.started)
        self.assertIsNotNone(control.stopped)
        self.assertFalse(control.confirmed)
        self.assertIn("Unsupported", control.message)

    def test_manual_key_recording_starts_gpu_only_with_log_and_saves_before_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            folder = base / "capture"
            folder.mkdir()
            (folder / "report.json").write_text('{"old":true}')
            sampler = base / "nvidia-smi"
            sampler.write_text(f"#!{sys.executable}\n" + textwrap.dedent(f"""\
                from pathlib import Path
                import time
                Path({str(base / 'sampler-started')!r}).touch()
                for i in range(200):
                    print('GPU-test, Test GPU, test-driver, 40, 70, 3000, 900, 1000, 55', flush=True)
                    if i == 4:
                        Path({str(base / 'sampled')!r}).touch()
                    time.sleep(.05)
                """))
            sampler.chmod(0o700)
            child = base / "fake-mangohud.py"
            child.write_text(textwrap.dedent("""\
                import os, socket, sys, time
                from pathlib import Path
                base = Path(sys.argv[1])
                config = dict(part.split('=', 1) for part in os.environ['MANGOHUD_CONFIG'].split(',') if '=' in part)
                assert config['autostart_log'] == config['log_duration'] == '0'
                assert config['toggle_logging'] == 'Shift_L+F2'
                server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                server.bind('\\0' + config['control'])
                server.listen(1)
                connection, _ = server.accept()
                connection.sendall(b':MangoHudControlVersion=1;:DeviceName=Test;:MangoHudVersion=fixture;')
                (base / 'ready').touch()
                while not (base / 'start').exists(): time.sleep(.01)
                with (Path(config['output_folder']) / 'game.csv').open('w') as log:
                    log.write('fps,frametime,elapsed\\n')
                    ordinal = 0
                    while not (base / 'stop').exists():
                        ordinal += 1
                        log.write(f'100,10,{ordinal * 10000000}\\n')
                        log.flush()
                        time.sleep(.01)
                connection.sendall(b':LoggingFin')
                time.sleep(.03)
                connection.sendall(b'ished=Test;')
                while not (base / 'exit').exists(): time.sleep(.01)
                connection.close()
                server.close()
                """))
            command = [sys.executable, str(SCRIPT), "capture", "--manual", "--mode", "baseline",
                       "--scenario", "manual", "--output", str(folder), "--", sys.executable, str(child), str(base)]
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       env=os.environ | {"PATH": str(base)})

            def await_path(path):
                deadline = time.monotonic() + 8
                while not path.exists() and time.monotonic() < deadline and process.poll() is None:
                    time.sleep(.02)
                self.assertTrue(path.exists(), f"missing event: {path}")

            try:
                await_path(base / "ready")
                self.assertFalse((base / "sampler-started").exists())
                (base / "start").touch()
                await_path(base / "sampled")
                (base / "stop").touch()
                sessions = list(folder.glob("session-*"))
                self.assertEqual(len(sessions), 1)
                session = sessions[0]
                await_path(session / "report.json")
                self.assertIsNone(process.poll(), "recording stop must not stop the game")
                report = json.loads((session / "report.json").read_text())
                self.assertTrue(report["manual"])
                self.assertTrue(report["stop_confirmed"])
                self.assertGreater(report["telemetry"]["samples"], 0)
                self.assertEqual(len(report["frame_streams"]), 1)
                snapshot = session / "recorded/game.csv"
                frozen = snapshot.read_text()
                (session / "mangohud/game.csv").write_text('unrelated later recording')
                self.assertEqual(snapshot.read_text(), frozen)
                self.assertEqual((folder / "report.json").read_text(), '{"old":true}')
                (base / "exit").touch()
                _stdout, stderr = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 0, stderr)
            finally:
                (base / "exit").touch()
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.communicate(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate()
                process.stdout.close()
                process.stderr.close()


if __name__ == "__main__":
    unittest.main()

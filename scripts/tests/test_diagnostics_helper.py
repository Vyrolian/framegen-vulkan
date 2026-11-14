"""Deterministic tests for the diagnostics preset helper."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
HELPER = REPOSITORY_ROOT / "scripts" / "fgvk-diagnostics"

# Mirror the wrapper-written diagnostics log contract consumed by the helper.
CONFIG_DIR = ".config/framegen-vulkan"
PRESENT_DIAGNOSTICS_LOG_FILENAME = "present-diagnostics.log"
PRESENT_DIAGNOSTICS_RETAINED_SESSION_COUNT = 10


FIXTURE = """\
[Vulkan Loader] Loading VK_LAYER_FGVK_frame_generation
[Vulkan Loader] Loading VK_LAYER_FGVK_render
framegen-vulkan: render layer active; identity=VK_LAYER_FGVK_render; build=1.0.0; fingerprint=abc123.dirty.12345678
framegen-vulkan: present diagnostics: operation=launch-environment pid=4242 steam_overlay_preload_filter_requested=1 steam_overlay_preload_removed=2
framegen-vulkan: present diagnostics: operation=process-identity pid=4242 executable=game.exe wine_executable=game.exe process_name=GameThread profile=fgvk identification=fallback build=1.0.0 fingerprint=abc123.dirty.12345678 steam_overlay_hook_loaded=0 steam_overlay_vulkan_loaded=0
framegen-vulkan: swapchain colour pipeline: format=64; color-space=1000104008; mode=hdr10-pq; source=gamescope-normalized; transport=packed-hdr10-32-bit; frame-generation=supported
framegen-vulkan: HDR generation precision: reduced_requested=1; model_space=pq-code-values; pq_conversion=0; requested_flow_scale=0.8; motion_flow_scale=0.4; motion_width=1024; motion_height=576
framegen-vulkan: HDR scaling precision: input_format=64; output_format=97; compact_input=1
framegen-vulkan: HDR shader precision: reduced=1; request_accepted=1
framegen-vulkan: HDR shader source footprint: source=2560x1440; output=3840x2160; accepted=1
framegen-vulkan: HDR10 transport: mode=packed-10-bit; nominal_bytes=16384000; nominal_bytes_saved=16384000; application_device_supported=1; backend_device_supported=1
framegen-vulkan: present diagnostics: operation=swapchain-context-create context=1 pid=4242 swapchain=1234 width=1280 height=800 application_width=854 application_height=532 frame_generation_width=1280 frame_generation_height=800 images=3 format=64 color_space=1000104008 present_mode=2 ordered_transport=1 active_contexts=1 inserted=1 live_profile_recreation=guarded-maintenance1-one-shot
framegen-vulkan: present diagnostics: operation=runtime-transition-pending context=1 state_revision=2 reason=profile-resources frame_generation_backend_pending=1 flow_scale_pending=1 lighter_model_pending=1 generated_capacity_pending=1 available_generated_capacity=1 requested_generated_capacity=3 process_restart_required=0 action=signal-out-of-date-after-successful-present
framegen-vulkan: present diagnostics: operation=runtime-transition-pending context=2 state_revision=4 reason=frame-generation-resources flow_scale_pending=1 lighter_model_pending=1 generated_capacity_pending=1 active_generated_capacity=1 requested_generated_capacity=4 action=prepare-private-context
framegen-vulkan: present diagnostics: operation=runtime-transition-prepared context=2 state_revision=4 reason=frame-generation-resources requested_generated_capacity=4 requested_flow_scale=0.75 requested_lighter_model=1 action=drain-private-work
framegen-vulkan: renderer-memory operation=private-context-prepared context=2 state_revision=4 live_internal_bytes=41951424 live_internal_allocations=16 live_exported_bytes=41287680 live_exported_allocations=9 peak_internal_bytes=41951424 peak_internal_allocations=16 peak_exported_bytes=41287680 peak_exported_allocations=9
framegen-vulkan: present diagnostics: operation=runtime-transition-failed context=3 state_revision=4 reason=frame-generation-resources retry_ms=5000 active_generated_capacity=1 action=retain-active-resources
framegen-vulkan: present diagnostics: operation=runtime-transition-applied context=2 state_revision=4 reason=frame-generation-resources transition=private-context effective_flow_scale=0.75 lighter_model=1 generated_frame_capacity=4 history_warmup_frames=3
framegen-vulkan: renderer-memory operation=private-context-applied context=2 state_revision=4 live_internal_bytes=20975712 live_internal_allocations=8 live_exported_bytes=27525120 live_exported_allocations=6 peak_internal_bytes=41951424 peak_internal_allocations=16 peak_exported_bytes=41287680 peak_exported_allocations=9
framegen-vulkan: process-static configuration changes remain pending until game restart; contexts=1
framegen-vulkan: present diagnostics: operation=runtime-transition-pending context=1 state_revision=3 reason=process-static-profile gpu_selection_pending=1 pacing_pending=0 frame_generation_interop_pending=0 ultra_performance_pending=0 action=wait-for-process-restart
framegen-vulkan: live profile resource change requested a game-owned swapchain recreation after one successful lower present
framegen-vulkan: present diagnostics: operation=runtime-transition-recreation-requested context=1 state_revision=2 reason=profile-resources lower_present_result=0 signal=VK_ERROR_OUT_OF_DATE_KHR delivery=one-shot-after-semaphore-consumption
framegen-vulkan: present diagnostics: operation=original-present-recreation-propagate context=2 frame=10 sequence=20 result=-1000001004 action=application-image-submitted-before-recreation
framegen-vulkan: present diagnostics: operation=swapchain-recreation-observed context=2 role=frame-generation swapchain=1234 source=upstream-or-driver
framegen-vulkan: present diagnostics: operation=replacement-backend-stabilization context=2 phase=started duration_ms=250 action=scaled-real-frame-only
framegen-vulkan: present diagnostics: operation=runtime-state-applied context=2 state_revision=2 adaptive=1 target_fps=110 effective_flow_scale=0.75 lighter_model=1 generated_frame_capacity=3 hdr=1
framegen-vulkan: presentation timing initialized; owner=renderer; transport=VK_EXT_present_timing; active=1
framegen-vulkan: presentation timing fallback; owner=renderer; transport=VK_EXT_present_timing; reason=feedback-or-clock-unavailable
framegen-vulkan: present diagnostics: operation=application-present-mode context=2 swapchain=1234 requested_present_mode=0 dynamic_override=1 effective_present_mode=2 ordered_transport=1 bridge=1 action=diagnostic-only
framegen-vulkan: present diagnostics: operation=application-present-wait waiter=19 pid=4242 device=55 swapchain=1234 api=KHR2 calls=60 successful=59 timeouts=1 errors=0 polls=1 first_present_id=20 last_present_id=79 duration_mean_ms=8.2 duration_max_ms=21.5 first_failure_result=2 first_failure_present_id=20 first_failure_timeout_ns=0 first_failure_duration_ms=0.02
framegen-vulkan: present diagnostics: operation=application-acquire stream=20 pid=4242 device=55 swapchain=1234 api=KHR2 calls=45 entry_interval_samples=45 entry_interval_mean_ms=22.2 entry_interval_max_ms=72.2 duration_mean_ms=1.1 duration_max_ms=50.1 successful=45 timeouts=0 not_ready=0 errors=0 polls=0
framegen-vulkan: present diagnostics: operation=application-queue-present stream=20 pid=4242 queue=66 swapchain=1234 api=KHR calls=45 entry_interval_samples=45 entry_interval_mean_ms=22.2 entry_interval_max_ms=72.2 duration_mean_ms=8.1 duration_max_ms=10 includes_intentional_pacing=1 configuration_update_samples=45 configuration_update_mean_ms=0.1 configuration_update_max_ms=0.5
framegen-vulkan: present diagnostics: operation=runtime-health monitor=21 pid=4242 role=frame-generation session_ms=3600000 gpu_render_node=226:128 process_rss_bytes=123456 gpu_busy_percent=95 gpu_temperature_mc=65000 fgvk_backend_internal_bytes=987654
framegen-vulkan: present diagnostics: operation=gpu-headroom context=1 state=limiting reserve_percent=10 target_utilization_percent=90 measured_utilization_percent=94 source_cap_fps=38 source_floor_fps=30
framegen-vulkan: present diagnostics: operation=adaptive-efficiency-probe-cancelled context=1 generated_limit=1 baseline_base_fps=40 current_base_fps=40 projected_output_fps=80 reason=gpu-headroom-source-cap
framegen-vulkan: present diagnostics: operation=runtime-health-unavailable reason=device-monitor-limit
framegen-vulkan: present diagnostics: operation=present-phase-summary context=1 pid=4242 tid=4243 sample_end_monotonic_ms=3600000 calls=45 total_mean_ms=2 total_max_ms=4 render_fence_mean_ms=0.2 render_fence_max_ms=1
framegen-vulkan: present diagnostics: operation=adaptive-ramp context=1 old_limit=0 new_limit=1
framegen-vulkan: present diagnostics: operation=adaptive-plan context=1 base_fps=60 target_fps=90 generated=1 max_generated=1 stable_cadence=0 target_clock=1 target_budget_credit_outputs=0.25 target_deferred_budget_output=1 target_phase_error_ms=-2.1 source_interval_samples=60 source_interval_mean_ms=16.6 source_interval_stddev_ms=1.2 source_interval_p95_ms=18.5 source_interval_p99_ms=20.5 generated_count_changes=29 requested_interval_samples=90 requested_interval_mean_ms=11.1 requested_interval_stddev_ms=1.3 requested_interval_p95_ms=13.5 requested_interval_p99_ms=13.5 target_phase_error_samples=60 target_phase_error_rms_ms=2.2 target_phase_error_max_ms=5.5
framegen-vulkan: present diagnostics: operation=adaptive-wsi-headroom-limit context=1 requested_min_images=3 images=5 requested_generated=2 admitted_generated=1 previous_generated_capacity=2 effective_generated_capacity=1 evidence=higher-multiplier-partial-admission action=retain-proven-generated-capacity
framegen-vulkan: present diagnostics: operation=fixed-plan context=2 base_fps=61.2 multiplier=2 generated_per_real=1 observed_output_fps=122.4 generated_presented=61 generated_skipped=0 configured_adaptive_target_fps=110 target_applies=0
framegen-vulkan: present diagnostics: operation=fixed-cadence-collapse-probe-start context=2 baseline_base_fps=30.1 observed_base_fps=30.1 confirmed_samples=0 consecutive_failures=0 retry_ms=0 action=history-only-probe
framegen-vulkan: present diagnostics: operation=fixed-cadence-collapse-probe-recovered context=2 baseline_base_fps=30.1 observed_base_fps=60 confirmed_samples=3 consecutive_failures=0 retry_ms=0 action=verify-fixed-resume
framegen-vulkan: present diagnostics: operation=fixed-cadence-collapse-recovery-verified context=2 baseline_base_fps=0 observed_base_fps=60 confirmed_samples=0 consecutive_failures=0 retry_ms=0 action=normal-fixed-policy
framegen-vulkan: present diagnostics: operation=acquire-generated-image context=1 duration_ms=50 result=VK_TIMEOUT
framegen-vulkan: present diagnostics: operation=skip-generated-frames context=1 reason=initial-timeout
framegen-vulkan: present diagnostics: operation=ordered-acquire-policy context=1 configured_timeout_ms=50 slow_threshold_ms=25 severe_threshold_ms=50 budget_scope=application-present first_slow_action=zero-wait-protection guard_miss_action=native-relief-history-warmup recovery_probe_timeout_ms=8.33 recovery_probe_timeout_max_ms=25 recovery_probe_failure=backoff post_probe_policy=native-only stabilization_ms=2000
framegen-vulkan: present diagnostics: operation=ordered-acquire-classification-split context=1 acquire_total_ms=27 acquire_max_ms=10.5 acquire_classification=maximum slow_threshold_ms=25 action=observe-only
framegen-vulkan: present diagnostics: operation=ordered-acquire-budget-exhausted context=1 phase=acquire acquire_total_ms=50 acquire_max_ms=30 budget_ms=50 requested_generated=2 admitted_generated=2 presented_generated=1 action=stop-acquiring
framegen-vulkan: present diagnostics: operation=ordered-acquire-guard context=1 phase=zero-wait-guard acquire_total_ms=31 acquire_max_ms=31 slow_threshold_ms=25 consecutive_slow_frames=1 action=zero-wait-protection
framegen-vulkan: present diagnostics: operation=ordered-acquire-guard-bypass context=1 phase=native-relief acquire_timeout_ns=0 history_warmup_frames=3 action=native-present-warm-history-then-normal-retry
framegen-vulkan: present diagnostics: operation=ordered-acquire-native-saturation context=1 state=entered native_base_fps=119.7 target_fps=120 consecutive_failures=1 bypassed_frames=30 recovery_ms=250 action=defer-probe-native-target-satisfied
framegen-vulkan: present diagnostics: operation=ordered-acquire-quarantine context=1 phase=native-drain reason=timeout retry_ms=250 action=native-drain
framegen-vulkan: present diagnostics: operation=ordered-acquire-retry context=1 phase=history-warmup bypassed_frames=12 action=warm-history-before-probe
framegen-vulkan: present diagnostics: operation=ordered-acquire-recovered context=1 phase=native-stabilization recovery_ms=400 action=native-only
framegen-vulkan: present diagnostics: operation=ordered-acquire-stabilized context=1 phase=history-warmup recovery_ms=2400 terminal_reason=stabilization-deadline-elapsed action=warm-history-before-normal-policy
framegen-vulkan: present diagnostics: operation=render-fence-budget-missed context=1 planned=1 action=native-present
framegen-vulkan: present diagnostics: operation=present-breakdown context=1 total_ms=44.4 render_fence_ms=0.1 schedule_ms=1.0 source_copy_ms=1.2 acquire_ms=40 generated_submit_ms=0.4 generated_present_ms=0.5 original_present_ms=0.6 unattributed_ms=0.6 frame=42 sequence=84
framegen-vulkan: present diagnostics: operation=resume-generated-frames context=1
framegen-vulkan: present diagnostics: operation=generated-image-recovered context=1
framegen-vulkan: present diagnostics: operation=swapchain-recreation-suppressed context=1
framegen-vulkan: frame-generation initialization failed; native presentation retained: test failure
unrelated application output
"""


class DiagnosticsHelperTests(unittest.TestCase):
    def test_wayland_protocol_errors_survive_every_preset(self):
        errors = [
            "wl_display#1: error 1: invalid method in synthetic protocol request",
            "wp_linux_drm_syncobj_surface_v1#37: error 4: synthetic synchronization failure",
            "wl_surface@9: error 2: synthetic surface failure",
            "gamescope_swapchain#52: error 1: synthetic swapchain failure",
            "[destroyed object]: error 7: synthetic buffer import failure",
        ]
        unrelated = [
            "unrelated application output",
            "application: error 7: unrelated failure",
            "wl_surface#9.attach(wl_buffer#10, 0, 0)",
            "prefix wl_display#1: error 1: quoted diagnostic",
            "wl_display#x: error 1: malformed object ID",
            "wl_display#1: error unknown: malformed error code",
        ]
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "protocol-error.log"
            path.write_text("\n".join(unrelated + errors + unrelated) + "\n", encoding="utf-8")
            for preset in self._run("--list").stdout.splitlines():
                with self.subTest(preset=preset):
                    result = self._run("--log", str(path), preset)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout.splitlines(), errors)
                    self.assertIn(f"matched_trace_lines={len(errors)}", result.stderr)

            result = self._run("--log", str(path), "--lines", "1", "all")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), errors[-1:])
            self.assertIn("omitted_trace_lines=4", result.stderr)

    def test_renderer_exception_details_survive_every_preset(self):
        records = [
            "unrelated application output",
            "- unrelated application exception",
            "framegen-vulkan: swapchain presentation failed:",
            "- vkQueuePresentKHR() failed (error -4)",
            "- nested test error",
            "unrelated output after the error",
            "- unrelated trailing exception",
        ]
        expected = "framegen-vulkan: swapchain presentation failed: - vkQueuePresentKHR() failed (error -4) - nested test error"
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "failure.log"
            path.write_text("\n".join(records) + "\n", encoding="utf-8")
            for preset in self._run("--list").stdout.splitlines():
                with self.subTest(preset=preset):
                    result = self._run("--log", str(path), "--lines", "1", preset)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout.strip(), expected)
                    self.assertIn("matched_trace_lines=1", result.stderr)

    def test_unrecognized_renderer_prefixes_are_not_read(self):
        records = [
            "previous-renderer: swapchain creation failed:",
            "- failed to create test context",
            "- nested test allocation failure",
            "framegen-vulkan: layer initialization failed:",
            "- test initialization error",
        ]
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "unrecognized-prefix.log"
            path.write_text("\n".join(records) + "\n", encoding="utf-8")
            result = self._run("--log", str(path), "all")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), [
            "framegen-vulkan: layer initialization failed: - test initialization error",
        ])

    def test_steam_timestamped_exception_details_survive_every_preset(self):
        timestamp = "[2026-10-14 20:31:05] "
        for prefix in ("framegen-vulkan:",):
            header = timestamp + prefix + " swapchain presentation failed:"
            records = [
                timestamp + "- unrelated exception before the failure",
                header,
                timestamp + "- vkQueuePresentKHR() failed (error -4)",
                "[2026-10-14 20:31:06] - nested synthetic error",
                timestamp + "unrelated application output",
                timestamp + "- unrelated exception after the failure",
            ]
            expected = header + " - vkQueuePresentKHR() failed (error -4) - nested synthetic error"
            with tempfile.TemporaryDirectory() as temporary_directory:
                path = Path(temporary_directory) / "console-linux.txt"
                path.write_text("\n".join(records) + "\n", encoding="utf-8")
                for preset in self._run("--list").stdout.splitlines():
                    with self.subTest(prefix=prefix, preset=preset):
                        result = self._run("--log", str(path), "--lines", "1", preset)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertEqual(result.stdout.strip(), expected)
                        self.assertIn("matched_trace_lines=1", result.stderr)

    def test_steam_timestamped_wayland_errors_survive_every_preset(self):
        timestamp = "[2026-10-14 20:31:05] "
        errors = [timestamp + message for message in (
            "wl_display#1: error 1: synthetic protocol error",
            "wl_surface@9: error 2: synthetic surface failure",
            "[destroyed object]: error 7: synthetic import failure",
        )]
        unrelated = [
            timestamp + "application: error 7: unrelated failure",
            timestamp + "wl_surface#9.attach(wl_buffer#10, 0, 0)",
            timestamp + "prefix wl_display#1: error 1: quoted error",
            "[not a timestamp] wl_display#1: error 1: unrelated output",
        ]
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "console-linux.txt"
            path.write_text("\n".join(unrelated + errors + unrelated) + "\n", encoding="utf-8")
            for preset in self._run("--list").stdout.splitlines():
                with self.subTest(preset=preset):
                    result = self._run("--log", str(path), preset)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout.splitlines(), errors)
                    self.assertIn("matched_trace_lines=3", result.stderr)

    def test_renderer_exception_detail_count_is_bounded(self):
        records = ["framegen-vulkan: device creation failed:"]
        records.extend(f"- test cause {index}" for index in range(20))
        records.append("framegen-vulkan: test-following-record")
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "bounded-failure.log"
            path.write_text("\n".join(records) + "\n", encoding="utf-8")
            result = self._run("--log", str(path), "all")
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), 2)
        for index in range(8):
            self.assertIn(f"- test cause {index}", lines[0])
        self.assertNotIn("- test cause 8", lines[0])
        self.assertEqual(lines[0].count("error_details_truncated=1"), 1)
        self.assertEqual(lines[1], records[-1])

    def test_runtime_health_is_retained_by_every_preset(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = self._fixture_path(Path(temporary_directory))
            for preset in self._run("--list").stdout.splitlines():
                with self.subTest(preset=preset):
                    result = self._run("--log", str(path), preset)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("operation=runtime-health monitor=21", result.stdout)
                    self.assertIn("session_ms=3600000", result.stdout)
                    self.assertIn("operation=runtime-health-unavailable", result.stdout)
                    self.assertIn("operation=present-phase-summary", result.stdout)
                    self.assertIn("operation=gpu-headroom", result.stdout)
                    self.assertIn("operation=adaptive-efficiency-probe-cancelled", result.stdout)
                    self.assertIn("reason=gpu-headroom-source-cap", result.stdout)

    def test_recent_trace_preserves_initial_and_latest_context(self):
        records = [
            "framegen-vulkan: present diagnostics: operation=launch-environment pid=42 steam_overlay_preload_filter_requested=1 steam_overlay_preload_removed=2",
            "framegen-vulkan: render layer active; identity=VK_LAYER_FGVK_render; build=4.0.0; fingerprint=long-session",
            "framegen-vulkan: present diagnostics: operation=process-identity pid=42 profile=test",
            "framegen-vulkan: backend GPU selection: following game device=1002:1435",
            "framegen-vulkan: present diagnostics: operation=swapchain-context-create context=91 width=1280 height=800",
            "framegen-vulkan: present diagnostics: operation=runtime-state-applied context=91 effective_frame_generation_enabled=1",
            "framegen-vulkan: present diagnostics: operation=runtime-health monitor=92 pid=42 session_ms=0 process_rss_bytes=100",
            "framegen-vulkan: present diagnostics: operation=runtime-state-applied context=91 effective_frame_generation_enabled=0",
            "framegen-vulkan: present diagnostics: operation=runtime-health monitor=92 pid=42 session_ms=3600000 process_rss_bytes=200",
        ]
        trace = [f"framegen-vulkan: present diagnostics: operation=present-phase-summary context=91 frame={index}" for index in range(2000)]
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "long.log"
            path.write_text("\n".join(records + trace + ["unrelated application output"]) + "\n", encoding="utf-8")
            for preset in self._run("--list").stdout.splitlines():
                with self.subTest(preset=preset):
                    result = self._run("--log", str(path), "--lines", "3", preset)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    for record in records:
                        self.assertEqual(result.stdout.count(record + "\n"), 1)
                    self.assertEqual(result.stdout.splitlines()[-3:], trace[-3:])
                    self.assertIn("omitted_trace_lines=", result.stderr)
                    self.assertIn("earlier_context_records=9", result.stderr)
                    self.assertNotIn("unrelated application output", result.stdout)

    def test_context_limits_do_not_lose_identity_to_swapchain_or_monitor_churn(self):
        identity = "framegen-vulkan: render layer active; identity=VK_LAYER_FGVK_render; build=3.3; fingerprint=legacy"
        records = [identity]
        for index in range(100):
            records.append(f"framegen-vulkan: present diagnostics: operation=swapchain-context-create context={index} width=1280")
        for monitor in range(40):
            records.extend(f"framegen-vulkan: present diagnostics: operation=runtime-health monitor={monitor} session_ms={stamp}" for stamp in (0, 10000))
        recent = "framegen-vulkan: present diagnostics: operation=present-phase-summary context=99 frame=9999"
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "churn.log"
            path.write_text("\n".join(records + [recent]) + "\n", encoding="utf-8")
            result = self._run("--log", str(path), "--lines", "1", "all")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(identity, result.stdout)
        self.assertIn("monitor=39 session_ms=0", result.stdout)
        self.assertIn("monitor=39 session_ms=10000", result.stdout)
        self.assertNotIn("monitor=0 session_ms=", result.stdout)
        self.assertLessEqual(len(result.stdout.splitlines()), 97)
        self.assertEqual(result.stdout.splitlines()[-1], recent)
        self.assertRegex(result.stderr, r"context_keys_evicted=[1-9][0-9]*")

    def test_short_report_does_not_repeat_context_already_in_recent_trace(self):
        records = [
            "framegen-vulkan: render layer active; identity=VK_LAYER_FGVK_render; build=4.0.0",
            "framegen-vulkan: present diagnostics: operation=runtime-state-applied context=1 effective_frame_generation_enabled=1",
            "framegen-vulkan: present diagnostics: operation=runtime-health monitor=2 session_ms=0",
            "framegen-vulkan: present diagnostics: operation=runtime-health monitor=2 session_ms=5000",
        ]
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "short.log"
            path.write_text("\n".join(records) + "\n", encoding="utf-8")
            result = self._run("--log", str(path), "all")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), records)
        self.assertIn("earlier_context_records=0", result.stderr)

    def test_bounded_rescue_decisions_survive_focused_collection(self):
        records = [
            "framegen-vulkan: present diagnostics: operation=adaptive-rescue-start context=1 reason=stable-cadence-collapse measurement_ms=1000",
            "framegen-vulkan: present diagnostics: operation=adaptive-auto-base-cap-suppressed context=1 reason=stable-cadence-collapse action=release-half-target-pacer-for-bounded-rescue",
            "framegen-vulkan: present diagnostics: operation=adaptive-auto-base-cap-restored context=1 measured_base_fps=60 reason=rescue-measurement-complete action=restore-half-target-pacer",
            "framegen-vulkan: present diagnostics: operation=adaptive-rescue-complete context=1 measured_base_fps=60 decision=verify-restored-policy",
            "framegen-vulkan: present diagnostics: operation=adaptive-rescue-complete context=1 measured_base_fps=45 decision=restored-policy-recovered",
            "framegen-vulkan: present diagnostics: operation=adaptive-rescue-complete context=1 measured_base_fps=35 decision=restored-policy-not-recovered",
            "framegen-vulkan: present diagnostics: operation=adaptive-rescue-complete context=1 measured_base_fps=35 decision=timing-reset",
            # Historical decisions remain readable through the same operation filters.
            "framegen-vulkan: present diagnostics: operation=adaptive-rescue-complete context=1 measured_base_fps=60 decision=verify-cap-release",
            "framegen-vulkan: present diagnostics: operation=adaptive-rescue-complete context=1 measured_base_fps=60 decision=cap-release-verified",
            "framegen-vulkan: present diagnostics: operation=adaptive-auto-base-cap-suppressed context=1 reason=stable-cadence-collapse",
            "framegen-vulkan: present diagnostics: operation=adaptive-auto-base-cap-restored context=1 measured_base_fps=35 reason=generated-load-not-recovered action=restore-half-target-pacer",
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "rescue.log"
            path.write_text("\n".join(records) + "\n", encoding="utf-8")
            for preset in ("all", "adaptive", "recovery", "performance"):
                result = self._run("--log", str(path), preset)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.splitlines(), records)

    def test_settled_pacing_decisions_survive_focused_collection(self):
        records = [
            "framegen-vulkan: present diagnostics: operation=adaptive-paced-load-start context=1 generated_limit=2 baseline_base_fps=54 current_base_fps=48 reason=ordered-fifo",
            "framegen-vulkan: present diagnostics: operation=adaptive-paced-load-baseline-probe context=1 generated_limit=1 baseline_base_fps=40 current_base_fps=40 reason=fresh-adjacent-lower-load",
            "framegen-vulkan: present diagnostics: operation=adaptive-load-shed context=1 previous_generated_limit=1 tested_generated_limit=2 previous_base_fps=54 current_base_fps=40 previous_output_fps=108 current_output_fps=120 reason=paced-unpaid-real-frame-cost",
            "framegen-vulkan: present diagnostics: operation=adaptive-paced-load-accepted context=1 generated_limit=3 baseline_base_fps=33 current_base_fps=30 reason=settled-real-frame-cost-paid",
            "framegen-vulkan: present diagnostics: operation=adaptive-paced-load-accepted context=1 generated_limit=2 baseline_base_fps=54 current_base_fps=40 reason=settled-output-qualified",
            "framegen-vulkan: present diagnostics: operation=adaptive-paced-load-cancelled context=1 generated_limit=4 baseline_base_fps=28 current_base_fps=24 reason=pacing-policy-changed",
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "paced-load.log"
            path.write_text("\n".join(records) + "\n", encoding="utf-8")
            for preset in ("all", "adaptive", "recovery", "performance"):
                with self.subTest(preset=preset):
                    result = self._run("--log", str(path), preset)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout.splitlines(), records)

    def test_context_anchors_isolate_layer_roles_and_processes(self):
        records = []
        for role in ("frame-generation",):
            records.extend(
                f"framegen-vulkan: present diagnostics: operation=runtime-state-applied context=1 role={role} state_revision={revision}"
                for revision in (1, 2)
            )
            for pid in (41, 42):
                records.extend(
                    f"framegen-vulkan: present diagnostics: operation=runtime-health monitor=2 pid={pid} role={role} session_ms={stamp}"
                    for stamp in (0, 5000)
                )
        recent = "framegen-vulkan: present diagnostics: operation=present-phase-summary context=1 frame=100"
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "roles.log"
            path.write_text("\n".join(records + [recent]) + "\n", encoding="utf-8")
            result = self._run("--log", str(path), "--lines", "1", "all")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), records + [recent])
        self.assertIn("earlier_context_records=6", result.stderr)

    def test_pacing_profile_is_retained_without_verbose_diagnostics(self):
        record = "framegen-vulkan: present diagnostics: operation=pacing-profile version=1 context=1 sources=60 within_batch_mean_ms=8.3 between_batches_mean_ms=8.4"
        gpu = "framegen-vulkan: present diagnostics: operation=pacing-profile version=2 context=1 gpu_requested=1 gpu_span_samples=60 gpu_span_mean_ms=3 gpu_readback_mean_ms=0.02"
        timing = "framegen-vulkan: present diagnostics: operation=pacing-profile version=2 context=1 timing_driver_attempts=0 timing_ext_attempts=120 fifo_caller_timing_sources=60 present_target_interval_samples=119 present_target_interval_mean_ms=8.333 present_target_discontinuities=0"
        output = "framegen-vulkan: present diagnostics: operation=pacing-profile-output version=1 context=1 pid=42 ordinal=2 source=1 generated=0 deferred=1 image_index=3 scheduled_ns=100 started_ns=110 finished_ns=120 result=0 copy_checked=1 copy_status=1"
        wait = "framegen-vulkan: present diagnostics: operation=application-present-wait pid=42 swapchain=123 api=KHR2 calls=60 successful=60 timeouts=0 errors=0 polls=0 duration_mean_ms=8 duration_max_ms=20"
        wayland = "framegen-vulkan: present diagnostics: operation=pacing-profile-wayland version=1 pid=42 surface_token=5 feedback=2 event=discarded context=1 source=1 generated=1"
        wayland_gap = "framegen-vulkan: present diagnostics: operation=pacing-profile-wayland-gap version=1 pid=42 surface_token=5 lost_records=1"
        content = "framegen-vulkan: present diagnostics: operation=pacing-profile-content version=1 pid=42 context=1 state=armed trigger=/tmp/private.bin.trigger path=/tmp/private.bin"
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "pacing.log"
            path.write_text(record + "\n" + gpu + "\n" + timing + "\n" + output + "\n" + wait + "\n" + wayland + "\n" + wayland_gap + "\n" + content + "\n", encoding="utf-8")
            for preset in ("all", "adaptive", "recovery", "performance", "hdr"):
                result = self._run("--log", str(path), preset)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(record, result.stdout)
                self.assertIn(gpu, result.stdout)
                self.assertIn(timing, result.stdout)
                self.assertIn(output, result.stdout)
                self.assertIn(wait, result.stdout)
                self.assertIn(wayland, result.stdout)
                self.assertIn(wayland_gap, result.stdout)
                self.assertIn(content, result.stdout)

    def test_detailed_frame_trace_records_survive_every_preset(self):
        records = [
            "framegen-vulkan: present diagnostics: operation=" + operation + " version=1 pid=42 context=1 source=8"
            for operation in ("pacing-profile-source", "pacing-profile-gpu", "pacing-profile-application", "pacing-profile-wait",
                              "pacing-profile-acquire-driver", "pacing-profile-copy", "pacing-profile-copy-gpu", "pacing-profile-copy-gpu-state",
                              "pacing-profile-vulkan-setup")
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "detailed.log"
            path.write_text("\n".join(records) + "\n", encoding="utf-8")
            for preset in ("all", "adaptive", "recovery", "performance", "hdr"):
                result = self._run("--log", str(path), preset)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.splitlines(), records)

    def test_frame_boundaries_are_retained_by_focused_presets(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = self._fixture_path(Path(temporary_directory))
            for preset in ("adaptive", "recovery", "performance"):
                result = self._run("--log", str(path), preset)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("operation=application-acquire", result.stdout)
                self.assertIn("operation=application-queue-present", result.stdout)
                self.assertIn("includes_intentional_pacing=1", result.stdout)

    def test_helper_uses_the_wrapper_log_contract(self):
        helper_source = HELPER.read_text(encoding="utf-8")
        self.assertIn(
            f'default_plugin_log="${{HOME}}/{CONFIG_DIR}/'
            f'{PRESENT_DIAGNOSTICS_LOG_FILENAME}"',
            helper_source,
        )
        self.assertIn(
            f"retained_session_count={PRESENT_DIAGNOSTICS_RETAINED_SESSION_COUNT}",
            helper_source,
        )

    def _run(self, *arguments, environment=None):
        return subprocess.run(
            ["bash", str(HELPER), *arguments],
            check=False,
            capture_output=True,
            text=True,
            env=environment,
        )

    def _fixture_path(self, directory: Path) -> Path:
        path = directory / "diagnostics.log"
        path.write_text(FIXTURE, encoding="utf-8")
        return path

    def test_hdr_preset_excludes_adaptive_policy(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = self._fixture_path(Path(temporary_directory))
            result = self._run("--log", str(path), "hdr")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("mode=hdr10-pq", result.stdout)
        self.assertIn("HDR10 transport: mode=packed-10-bit", result.stdout)
        self.assertIn("HDR scaling precision: input_format=64", result.stdout)
        self.assertIn("HDR shader precision: reduced=1", result.stdout)
        self.assertIn("HDR shader source footprint: source=2560x1440", result.stdout)
        self.assertIn("HDR generation precision: reduced_requested=1", result.stdout)
        self.assertIn("motion_flow_scale=0.4; motion_width=1024; motion_height=576", result.stdout)
        self.assertIn("nominal_bytes_saved=16384000", result.stdout)
        self.assertIn("initialization failed", result.stdout)
        self.assertIn("render layer active", result.stdout)
        self.assertNotIn("adaptive-ramp", result.stdout)

    def test_multiple_presets_combine_recovery_and_adaptive(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = self._fixture_path(Path(temporary_directory))
            result = self._run("--log", str(path), "adaptive", "recovery")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("adaptive-ramp", result.stdout)
        self.assertIn("adaptive-wsi-headroom-limit", result.stdout)
        self.assertIn("runtime-state-applied", result.stdout)
        self.assertIn("skip-generated-frames", result.stdout)
        self.assertIn("ordered-acquire-quarantine", result.stdout)
        self.assertIn("ordered-acquire-policy", result.stdout)
        self.assertIn("ordered-acquire-classification-split", result.stdout)
        self.assertIn("ordered-acquire-budget-exhausted", result.stdout)
        self.assertIn("ordered-acquire-guard", result.stdout)
        self.assertIn("ordered-acquire-guard-bypass", result.stdout)
        self.assertIn("ordered-acquire-native-saturation", result.stdout)
        self.assertIn("ordered-acquire-retry", result.stdout)
        self.assertIn("ordered-acquire-recovered", result.stdout)
        self.assertIn("ordered-acquire-stabilized", result.stdout)
        self.assertIn("fixed-cadence-collapse-probe-start", result.stdout)
        self.assertIn("fixed-cadence-collapse-probe-recovered", result.stdout)
        self.assertIn("fixed-cadence-collapse-recovery-verified", result.stdout)
        self.assertIn("render-fence-budget-missed", result.stdout)
        self.assertIn("resume-generated-frames", result.stdout)
        self.assertIn("generated-image-recovered", result.stdout)
        self.assertIn("swapchain-recreation-suppressed", result.stdout)
        self.assertIn("render layer active", result.stdout)
        self.assertNotIn("mode=hdr10-pq", result.stdout)
        self.assertNotIn("HDR10 transport", result.stdout)

    def test_adaptive_preset_keeps_fractional_pacing_contract(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = self._fixture_path(Path(temporary_directory))
            result = self._run("--log", str(path), "adaptive")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("operation=adaptive-plan", result.stdout)

        producer = (
            REPOSITORY_ROOT / "engine/fgvk-render/src/present_diagnostics.cpp"
        ).read_text(encoding="utf-8")
        for field in (
            "stable_cadence=",
            "target_clock=",
            "target_budget_credit_outputs=",
            "target_deferred_budget_output=",
            "target_phase_error_ms=",
            "source_interval_samples=",
            "source_interval_mean_ms=",
            "source_interval_stddev_ms=",
            "source_interval_p95_ms=",
            "source_interval_p99_ms=",
            "generated_count_changes=",
            "requested_interval_samples=",
            "requested_interval_mean_ms=",
            "requested_interval_stddev_ms=",
            "requested_interval_p95_ms=",
            "requested_interval_p99_ms=",
            "target_phase_error_samples=",
            "target_phase_error_rms_ms=",
            "target_phase_error_max_ms=",
        ):
            with self.subTest(field=field):
                self.assertIn(field, result.stdout)
                self.assertIn(field, producer)

    def test_current_recovery_operations_have_renderer_producers(self):
        renderer_source = "\n".join(
            path.read_text(encoding="utf-8")
            for path in (
                REPOSITORY_ROOT / "engine/fgvk-render/src"
            ).rglob("*.cpp")
        )
        current_operations = {
            "skip-generated-frames",
            "generated-delivery-miss",
            "generated-admission-pressure",
            "generated-admission-recovered",
            "adaptive-wsi-headroom-limit",
            "ordered-acquire-quarantine",
            "ordered-acquire-policy",
            "ordered-acquire-budget-exhausted",
            "ordered-acquire-retry",
            "ordered-acquire-recovered",
            "render-fence-budget-missed",
            "history-warmup",
            "runtime-transition-pending",
            "runtime-transition-prepared",
            "runtime-transition-failed",
            "runtime-transition-applied",
        }
        for operation in current_operations:
            with self.subTest(operation=operation):
                self.assertIn(f"operation={operation}", renderer_source)

    def test_recovery_presets_keep_rebuild_decisions(self):
        operations = (
            "lower-present-stall-quarantine",
            "lower-present-stall-recovered",
            "lower-present-stall-recreation-armed",
            "lower-present-stall-recreation-cancelled",
            "lower-present-stall-recreation-requested",
            "lower-present-stall-recreation-budget",
            "adaptive-recovery-recreation-armed",
            "adaptive-recovery-recreation-requested",
            "adaptive-recovery-recreation-budget",
            # Older builds emitted this obsolete cooldown-reset record.
            "adaptive-recovery-recreation-budget-reset",
            "adaptive-recovery-recreation-watchdog",
            "adaptive-recovery-in-place-requested",
            "adaptive-recovery-in-place-budget",
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "recovery.log"
            path.write_text("\n".join(
                f"framegen-vulkan: present diagnostics: operation={op} context=7 "
                "reason=sustained-post-menu-deficit output_fps=70 "
                "lower_present_share=0.8 deficit_ms=4000 surface_available=0 "
                "delivery=one-shot-per-context-after-retirement-fence-attachment"
                for op in operations
            ), encoding="utf-8")
            for preset in ("recovery", "performance"):
                result = self._run("--log", str(path), preset)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("reason=sustained-post-menu-deficit", result.stdout)
                self.assertIn("surface_available=0", result.stdout)
                self.assertIn("delivery=one-shot-per-context-after-retirement-fence-attachment", result.stdout)
                for operation in operations:
                    with self.subTest(preset=preset, operation=operation):
                        self.assertIn(f"operation={operation}", result.stdout)

    def test_every_preset_keeps_the_authoritative_build_marker(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = self._fixture_path(Path(temporary_directory))
            for preset in (
                "hdr", "config", "adaptive", "recovery", "performance",
                "lifecycle", "startup", "layers", "errors", "all",
            ):
                with self.subTest(preset=preset):
                    result = self._run("--log", str(path), preset)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("render layer active", result.stdout)
                    self.assertIn("fingerprint=abc123.dirty.12345678", result.stdout)
                    self.assertIn("operation=process-identity", result.stdout)
                    self.assertIn("operation=launch-environment", result.stdout)
                    self.assertIn("steam_overlay_hook_loaded=0", result.stdout)
                    self.assertIn("steam_overlay_vulkan_loaded=0", result.stdout)
                    self.assertIn("operation=swapchain-context-create", result.stdout)
                    self.assertIn("operation=replacement-backend-stabilization", result.stdout)
                    self.assertIn("frame_generation_width=1280", result.stdout)

    def test_config_preset_correlates_requested_and_applied_state(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = self._fixture_path(Path(temporary_directory))
            result = self._run("--log", str(path), "config")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("runtime-transition-pending", result.stdout)
        self.assertIn("runtime-transition-prepared", result.stdout)
        self.assertIn("runtime-transition-failed", result.stdout)
        self.assertIn("runtime-transition-applied", result.stdout)
        self.assertIn("action=prepare-private-context", result.stdout)
        self.assertIn("action=drain-private-work", result.stdout)
        self.assertIn("action=retain-active-resources", result.stdout)
        self.assertIn("history_warmup_frames=3", result.stdout)
        self.assertIn("renderer-memory operation=private-context-prepared", result.stdout)
        self.assertIn("renderer-memory operation=private-context-applied", result.stdout)
        self.assertIn("live_exported_allocations=6", result.stdout)
        self.assertIn(
            "signal-out-of-date-after-successful-present", result.stdout
        )
        self.assertIn("wait-for-process-restart", result.stdout)
        self.assertIn("runtime-state-applied", result.stdout)
        self.assertIn("generated_capacity_pending=1", result.stdout)
        self.assertIn("available_generated_capacity=1", result.stdout)
        self.assertIn("requested_generated_capacity=3", result.stdout)
        self.assertIn("generated_frame_capacity=3", result.stdout)
        self.assertIn("process-static configuration changes", result.stdout)
        self.assertIn("gpu_selection_pending=1", result.stdout)
        self.assertIn("frame_generation_interop_pending=0", result.stdout)
        self.assertIn("action=wait-for-process-restart", result.stdout)
        self.assertIn("runtime-transition-recreation-requested", result.stdout)
        self.assertIn("one-shot-after-semaphore-consumption", result.stdout)
        self.assertIn("original-present-recreation-propagate", result.stdout)
        self.assertIn("application-image-submitted-before-recreation", result.stdout)
        self.assertIn("swapchain-recreation-observed", result.stdout)
        self.assertNotIn("adaptive-ramp", result.stdout)

    def test_performance_preset_includes_fixed_multiplier_telemetry(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = self._fixture_path(Path(temporary_directory))
            result = self._run("--log", str(path), "performance")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("operation=fixed-plan", result.stdout)
        self.assertIn("presentation timing initialized; owner=renderer; transport=VK_EXT_present_timing; active=1", result.stdout)
        self.assertIn("presentation timing fallback; owner=renderer; transport=VK_EXT_present_timing; reason=feedback-or-clock-unavailable", result.stdout)
        self.assertIn("operation=application-present-mode", result.stdout)
        self.assertIn("operation=application-present-wait", result.stdout)
        self.assertIn("first_failure_result=2 first_failure_present_id=20 first_failure_timeout_ns=0 first_failure_duration_ms=0.02", result.stdout)
        self.assertIn("fixed-cadence-collapse-probe-start", result.stdout)
        self.assertIn("fixed-cadence-collapse-probe-recovered", result.stdout)
        self.assertIn("fixed-cadence-collapse-recovery-verified", result.stdout)
        self.assertIn("generated_skipped=0", result.stdout)
        self.assertIn("ordered-acquire-quarantine", result.stdout)
        self.assertIn("ordered-acquire-classification-split", result.stdout)
        self.assertIn("ordered-acquire-native-saturation", result.stdout)
        self.assertIn("ordered-acquire-recovered", result.stdout)
        self.assertIn("operation=present-breakdown", result.stdout)
        self.assertIn("renderer-memory operation=private-context-prepared", result.stdout)
        self.assertIn("renderer-memory operation=private-context-applied", result.stdout)
        self.assertNotIn("adaptive-ramp", result.stdout)

    def test_startup_includes_loader_and_hdr_context(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = self._fixture_path(Path(temporary_directory))
            result = self._run("--log", str(path), "startup")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Vulkan Loader", result.stdout)
        self.assertIn("swapchain-context-create", result.stdout)
        self.assertIn("mode=hdr10-pq", result.stdout)
        self.assertIn("HDR10 transport: mode=packed-10-bit", result.stdout)
        self.assertIn("HDR scaling precision: input_format=64", result.stdout)
        self.assertIn("HDR shader precision: reduced=1", result.stdout)
        self.assertIn("HDR shader source footprint: source=2560x1440", result.stdout)
        self.assertIn("HDR generation precision: reduced_requested=1", result.stdout)
        self.assertIn("motion_flow_scale=0.4; motion_width=1024; motion_height=576", result.stdout)

    def test_private_log_is_selected_from_home(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            home = Path(temporary_directory)
            path = home / ".config/framegen-vulkan/present-diagnostics.log"
            path.parent.mkdir(parents=True)
            path.write_text(FIXTURE, encoding="utf-8")
            environment = {**os.environ, "HOME": str(home)}
            environment.pop("FGVK_PRESENT_DIAGNOSTICS_LOG", None)
            result = self._run("errors", environment=environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("VK_TIMEOUT", result.stdout)
        self.assertIn("initialization failed", result.stdout)
        self.assertIn(str(path), result.stderr)
        self.assertIn("session: latest", result.stderr)

    def test_private_pacing_capture_selection(self):
        record = "framegen-vulkan: present diagnostics: operation=pacing-profile version=2 context=1 sources=60"
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            path = root / f"fgvk-pacing-{os.getuid()}-100-1.log"
            path.write_text(record + "\n", encoding="utf-8")
            foreign = root / f"fgvk-pacing-{os.getuid()}-999-1.log"
            foreign.symlink_to(path)
            environment = {**os.environ, "TMPDIR": str(root)}
            environment.pop("FGVK_PRESENT_DIAGNOSTICS_LOG", None)
            result = self._run("--pacing", "performance", environment=environment)
            previous = self._run("--pacing", "--session", "previous", environment=environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(record, result.stdout)
        self.assertIn(str(path), result.stderr)
        self.assertEqual(previous.returncode, 2)
        self.assertIn("use --log PATH", previous.stderr)

    def test_retained_sessions_can_be_selected_or_combined(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            base_path = Path(temporary_directory) / "present-diagnostics.log"
            retained_sessions = (
                (base_path, "latest-slot"),
                (Path(f"{base_path}.1"), "previous-slot"),
                (Path(f"{base_path}.2"), "two-back-slot"),
                (Path(f"{base_path}.3"), "three-back-slot"),
                (Path(f"{base_path}.4"), "four-back-slot"),
                (Path(f"{base_path}.5"), "five-back-slot"),
                (Path(f"{base_path}.6"), "six-back-slot"),
                (Path(f"{base_path}.7"), "seven-back-slot"),
                (Path(f"{base_path}.8"), "eight-back-slot"),
                (Path(f"{base_path}.9"), "oldest-slot"),
            )
            for path, label in retained_sessions:
                path.write_text(
                    f"framegen-vulkan: session-marker={label}\n",
                    encoding="utf-8",
                )

            latest = self._run("--log", str(base_path), "all")
            previous = self._run(
                "--log", str(base_path), "--session", "previous", "all"
            )
            oldest = self._run(
                "--log", str(base_path), "--session", "oldest", "all"
            )
            previous_two = self._run(
                "--log",
                str(base_path),
                "--session",
                "previous-two",
                "--lines",
                "1",
                "all",
            )
            combined = self._run(
                "--log",
                str(base_path),
                "--session",
                "all",
                "--lines",
                "1",
                "all",
            )

        self.assertEqual(latest.returncode, 0, latest.stderr)
        self.assertIn("session-marker=latest-slot", latest.stdout)
        self.assertNotIn("session-marker=previous-slot", latest.stdout)
        self.assertEqual(previous.returncode, 0, previous.stderr)
        self.assertIn("session-marker=previous-slot", previous.stdout)
        self.assertEqual(oldest.returncode, 0, oldest.stderr)
        self.assertIn("session-marker=oldest-slot", oldest.stdout)
        self.assertEqual(previous_two.returncode, 0, previous_two.stderr)
        self.assertLess(
            previous_two.stdout.index("session-marker=two-back-slot"),
            previous_two.stdout.index("session-marker=previous-slot"),
        )
        self.assertNotIn("session-marker=oldest-slot", previous_two.stdout)
        self.assertNotIn("session-marker=latest-slot", previous_two.stdout)
        self.assertEqual(combined.returncode, 0, combined.stderr)
        combined_markers = [
            combined.stdout.index(f"session-marker={label}")
            for _, label in reversed(retained_sessions)
        ]
        self.assertEqual(combined_markers, sorted(combined_markers))
        self.assertEqual(combined.stderr.count("retained sessions: 10"), 10)

    def test_missing_retained_session_fails_clearly(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = self._fixture_path(Path(temporary_directory))
            result = self._run(
                "--log", str(path), "--session", "previous", "all"
            )

        self.assertEqual(result.returncode, 1)
        self.assertIn("not found for session 'previous'", result.stderr)
        self.assertIn(f"{path}.1", result.stderr)

    def test_newest_native_or_flatpak_steam_log_is_selected(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            home = Path(temporary_directory)
            native_log = home / ".steam/steam/logs/console-linux.txt"
            flatpak_log = (
                home
                / ".var/app/com.valvesoftware.Steam/.steam/steam/logs/console-linux.txt"
            )
            native_log.parent.mkdir(parents=True)
            flatpak_log.parent.mkdir(parents=True)
            native_log.write_text(FIXTURE, encoding="utf-8")
            flatpak_log.write_text(FIXTURE, encoding="utf-8")
            os.utime(native_log, (1, 1))
            os.utime(flatpak_log, (2, 2))
            environment = {**os.environ, "HOME": str(home)}
            environment.pop("FGVK_PRESENT_DIAGNOSTICS_LOG", None)
            result = self._run("errors", environment=environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(str(flatpak_log), result.stderr)

    def test_invalid_preset_and_line_count_fail_clearly(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = self._fixture_path(Path(temporary_directory))
            invalid_preset = self._run("--log", str(path), "not-a-preset")
            invalid_lines = self._run(
                "--log", str(path), "--lines", "0", "all"
            )
            invalid_session = self._run(
                "--log", str(path), "--session", "fourth", "all"
            )
        self.assertEqual(invalid_preset.returncode, 2)
        self.assertIn("Unknown preset", invalid_preset.stderr)
        self.assertEqual(invalid_lines.returncode, 2)
        self.assertIn("positive integer", invalid_lines.stderr)
        self.assertEqual(invalid_session.returncode, 2)
        self.assertIn(
            "latest, previous, oldest, previous-two, or all",
            invalid_session.stderr,
        )


if __name__ == "__main__":
    unittest.main()

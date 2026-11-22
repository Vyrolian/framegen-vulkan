"""Portable attribution and privacy tests for the CPU pacing report."""
import importlib.util
import contextlib
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/analyze-pacing-profile.py"
spec = importlib.util.spec_from_file_location("pacing_report", SCRIPT)
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)
protocol_spec = importlib.util.spec_from_file_location("wayland_pacing_protocol", SCRIPT.with_name("wayland_pacing_protocol.py"))
protocol = importlib.util.module_from_spec(protocol_spec)
protocol_spec.loader.exec_module(protocol)
setup_spec = importlib.util.spec_from_file_location("vulkan_setup", SCRIPT.with_name("analyze-vulkan-setup.py"))
setup = importlib.util.module_from_spec(setup_spec)
setup_spec.loader.exec_module(setup)


class FrameTimelineTests(unittest.TestCase):
    @staticmethod
    def fixture(source=1, start=1000000000, epoch=1, frame=0):
        durations = {name+"_ns": 0 for name in ("cap_wait", "readiness_wait", "output_wait", "private_total", "render_fence", "schedule", "source_copy", "acquire", "generated_submit", "generated_present", "original_present", "unattributed")}
        sample = dict(pid=10, tid=20, context=30, source=source, started_ns=start, finished_ns=start+1000000,
                      backend_scheduled=1, backend_epoch=epoch, backend_frame=frame, frame_generation_enabled=1,
                      adaptive=0, multiplier=2, effective_cap_fps=60, configured_cap_fps=60,
                      swapchain="0x123", **durations)
        app = dict(pid=10, tid=20, stream=40, ordinal=source, context=30, source=source, api="present", image_index=2,
                   started_ns=start-5000000, finished_ns=start+2000000, result_known=0, result=0,
                   timeout_known=0, timeout_ns=0, join_known=1, join_ns=4000000,
                   configuration_known=1, configuration_ns=1000, swapchain="0x123")
        gpu = dict(pid=10, context=30, source=source, backend_epoch=epoch, backend_frame=frame, previous_wait_us=5,
                   prepass_submit_us=3, generation_submit_us=4, prepass_us=100, span_us=200, readback_us=1, outputs=1, output_0_us=100)
        return sample, app, gpu

    @staticmethod
    def line(operation, record):
        return "framegen-vulkan: present diagnostics: operation="+operation+" version=1 "+" ".join(f"{k}={v}" for k,v in record.items())+"\n"

    def parse(self, lines):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"pacing.log";path.write_text("".join(lines))
            return report.read_observations(path)

    def test_logs_from_before_the_smooth_cadence_field_still_parse(self):
        # Logs written before adaptive_stable_cadence was removed carry smooth_cadence=; new logs do not.
        new,_,_=self.fixture()
        old=dict(new, smooth_cadence=0)
        observations,invalid=self.parse([self.line("pacing-profile-source",old),self.line("pacing-profile-source",new)])
        self.assertEqual(invalid,0)
        self.assertEqual(len(observations["sources"]),2)

    def test_exact_source_gpu_outer_boundaries_and_partial_capture(self):
        first,app,gpu = self.fixture()
        second,app2,gpu2 = self.fixture(2, 1020000000, frame=1)
        lines=[self.line("pacing-profile-source",first),self.line("pacing-profile-application",app),
               self.line("pacing-profile-source",second),self.line("pacing-profile-application",app2),
               self.line("pacing-profile-gpu",gpu),self.line("pacing-profile-gpu",gpu2)]
        observations,invalid=self.parse(lines)
        self.assertEqual(invalid,0)
        timeline=report.frame_timeline(observations)
        self.assertEqual(len(timeline["frames"]),2)
        row=timeline["frames"][1]
        self.assertEqual(row["source_gap_ns"],20000000)
        self.assertEqual(row["outside_application_ns"],13000000)
        self.assertEqual(row["gpu"]["backend_frame"],1)
        self.assertEqual(row["missing"],[])
        text="\n".join(report.frame_trace_report(observations,10,30,5))
        self.assertIn("2 exact application joins, 2 exact completed GPU joins",text)
        self.assertIn("outside outer 13.000 ms",text)
        with tempfile.TemporaryDirectory() as directory:
            log=Path(directory)/"input"; log.write_text("".join(lines))
            output=Path(directory)/"frames.json"
            result=subprocess.run([sys.executable,str(SCRIPT),str(log),"--frames-json",str(output)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn("partial capture",result.stdout)
            self.assertEqual(output.stat().st_mode & 0o777,0o600)
            refused=subprocess.run([sys.executable,str(SCRIPT),str(log),"--frames-json",str(output)],capture_output=True,text=True)
            self.assertNotEqual(refused.returncode,0)

    def test_missing_duplicates_and_resource_epochs_do_not_create_joins(self):
        sample,app,gpu=self.fixture()
        observations,invalid=self.parse([self.line("pacing-profile-source",sample),
            self.line("pacing-profile-application",app|{"finished_ns":sample["started_ns"]}),
            self.line("pacing-profile-gpu",gpu|{"backend_epoch":2})])
        row=report.frame_timeline(observations)["frames"][0]
        self.assertIsNone(row["application"]);self.assertIsNone(row["gpu"])
        self.assertEqual(len(row["missing"]),2)
        observations["sources"].append(sample.copy())
        timeline=report.frame_timeline(observations)
        self.assertEqual(timeline["frames"],[])
        self.assertEqual(timeline["duplicate_source_identities"],1)

    def test_invalid_floats_booleans_and_backwards_calls_are_rejected(self):
        sample,app,gpu=self.fixture()
        observations,invalid=self.parse([self.line("pacing-profile-source",sample|{"adaptive":2}),
            self.line("pacing-profile-application",app|{"finished_ns":1}),
            self.line("pacing-profile-gpu",gpu|{"span_us":"nan"})])
        self.assertEqual(invalid,3)
        self.assertEqual(observations["sources"],[])

    def test_wrapper_trace_flag_applies_only_to_child(self):
        import os
        before=os.environ.get("FGVK_PACING_PROFILE_TRACE")
        code="import os,sys; sys.exit(0 if os.environ.get('FGVK_PACING_PROFILE_TRACE')=='1' else 8)"
        with tempfile.TemporaryDirectory() as directory:
            with contextlib.redirect_stderr(io.StringIO()):
                result=protocol.capture([sys.executable,"-c",code],Path(directory)/"protocol",frame_trace=True)
        self.assertEqual(result,0)
        self.assertEqual(os.environ.get("FGVK_PACING_PROFILE_TRACE"),before)

    def acquisition_fixture(self):
        source, app, _ = self.fixture()
        acquire = app | dict(api="acquire", ordinal=10, source=0, started_ns=source["started_ns"]-10000000,
                             finished_ns=app["started_ns"]-1000000, result_known=1, result=0,
                             call_id=91, lower_calls=2, lower_ns=1000000)
        attempt = dict(pid=10, tid=20, call_id=91, attempt=1, timeout_ns=0,
                       started_ns=acquire["started_ns"], finished_ns=acquire["started_ns"]+100,
                       result=1, image_index=0xffffffff)
        success = attempt | dict(attempt=2, timeout_ns=1000000, started_ns=attempt["finished_ns"],
                                 finished_ns=attempt["finished_ns"]+999900, result=0, image_index=2)
        return source, app, acquire, attempt, success

    def test_image_acquisition_is_consumed_once_and_partitioned(self):
        source,app,acquire,attempt,success=self.acquisition_fixture()
        observations,invalid=self.parse([self.line(op,r) for op,r in (
            ("pacing-profile-source",source),("pacing-profile-application",app),
            ("pacing-profile-application",acquire),("pacing-profile-acquire-driver",attempt),
            ("pacing-profile-acquire-driver",success))])
        self.assertEqual(invalid,0)
        row=report.frame_timeline(observations)["frames"][0]
        self.assertEqual(row["acquisition"]["call_id"],91)
        self.assertEqual(row["acquire_to_present_ns"],1000000)
        self.assertEqual(len(row["acquire_driver"]),2)
        text="\n".join(report.frame_trace_report(observations,10,30,5))
        self.assertIn("driver 1.000 ms, framegen-vulkan/observer 3.000 ms",text)
        observations["application_calls"].append(app | dict(ordinal=2,source=2,started_ns=app["finished_ns"]+1))
        self.assertEqual(len(report.acquisition_links(observations)),1)

    def test_acquisition_does_not_cross_lifetimes_duplicates_or_capture_gaps(self):
        source,app,acquire,_,_=self.acquisition_fixture()
        for changes in (dict(context=31),dict(result=2,image_index=0xffffffff),dict(context=0)):
            observations,_=self.parse([self.line("pacing-profile-source",source),
                self.line("pacing-profile-application",app),self.line("pacing-profile-application",acquire|changes)])
            self.assertIsNone(report.frame_timeline(observations)["frames"][0]["acquisition"])
        observations,_=self.parse([self.line("pacing-profile-source",source),
            self.line("pacing-profile-application",app),self.line("pacing-profile-application",acquire)])
        observations["application_calls"].append(acquire.copy())
        self.assertEqual(report.acquisition_links(observations),{})
        observations["application_calls"].pop()
        observations["capture_gaps"]=[dict(lost_records=1)]
        self.assertEqual(report.acquisition_links(observations),{})

    def test_native_copy_dependencies_and_incomplete_driver_partition(self):
        source,app,acquire,attempt,_=self.acquisition_fixture()
        copy=dict(pid=10,tid=20,context=30,source=1,swapchain="0x123",generated=1,image_index=3,pass_=0,
                  device="0x1",queue="0x2",command="0x3",started_ns=source["started_ns"],finished_ns=source["started_ns"]+100,
                  submitted=1,timeline_wait="0x4",wait_value=2,timeline_signal="0",signal_value=0,fence="0x5",
                  wait_count=1,wait_recorded=1,wait_0="0x6",signal_count=1,signal_recorded=1,signal_0="0x7")
        copy["pass"]=copy.pop("pass_")
        observations,invalid=self.parse([self.line(op,r) for op,r in (
            ("pacing-profile-source",source),("pacing-profile-application",app),
            ("pacing-profile-application",acquire),("pacing-profile-acquire-driver",attempt),
            ("pacing-profile-copy",copy))])
        self.assertEqual(invalid,0)
        row=report.frame_timeline(observations)["frames"][0]
        self.assertEqual(row["copies"][0]["waits"],["0x6"])
        self.assertEqual(row["acquire_driver"],[])
        self.assertEqual(len(report.frame_timeline(observations)["unmatched_acquire_driver"]),1)
        text="\n".join(report.frame_trace_report(observations,10,30,5))
        self.assertIn("driver partition unavailable",text)
        self.assertIn("source=0, generated=1",text)
        _,invalid=self.parse([self.line("pacing-profile-copy",copy|dict(wait_recorded=17)),
                              self.line("pacing-profile-acquire-driver",attempt|dict(image_index=2)),
                              self.line("pacing-profile-application",acquire|dict(lower_ns=100000000))])
        self.assertEqual(invalid,3)

    def native_gpu_fixture(self, entry=500):
        source,app,_=self.fixture()
        origin=source["started_ns"]
        copy=dict(pid=10,tid=20,context=30,source=1,swapchain="0x123",generated=1,image_index=3,
                  device="0x1",queue="0x2",command="0x3",started_ns=origin,finished_ns=origin+10,
                  submitted=1,timeline_wait="0x4",wait_value=2,timeline_signal="0",signal_value=0,fence="0x5",
                  wait_count=1,wait_recorded=1,wait_0="0x6",signal_count=1,signal_recorded=1,signal_0="0x7")
        copy["pass"]=0
        sample=dict(pid=10,context=30,source=1,generation=1,swapchain="0x123",device="0x1",queue="0x2",command="0x3",
                    slot=1,generated=1,image_index=3,query_result=0,available=1,begin_ticks=1800,end_ticks=2000,
                    valid_bits=64,period_ns=1,collected_ns=origin+1000000,read_started_ns=origin+1000010,read_finished_ns=origin+1000020,
                    before_result=0,before_device_ticks=1000,before_host_ns=origin-750,before_deviation_ns=10,
                    before_started_ns=origin-1000,before_finished_ns=origin-500,
                    after_result=0,after_device_ticks=1002000,after_host_ns=origin+1000250,after_deviation_ns=10,
                    after_started_ns=origin+1000200,after_finished_ns=origin+1000300)
        output=dict(pid=10,context=30,source=1,ordinal=1,generated=1,deferred=1,image_index=3,scheduled_ns=0,
                    started_ns=origin+entry,finished_ns=origin+entry+10,result=0,copy_checked=0,copy_status=1,copy_sampled_ns=0)
        records=[self.line(op,r) for op,r in (("pacing-profile-source",source),("pacing-profile-application",app),
                 ("pacing-profile-copy",copy),("pacing-profile-output",output),("pacing-profile-copy-gpu",sample))]
        return records,copy,sample

    def test_native_gpu_evidence_distinguishes_before_after_and_uncertainty(self):
        for entry,expected in ((500,"before"),(100,"after"),(250,"ambiguous")):
            records,_,_=self.native_gpu_fixture(entry)
            observations,invalid=self.parse(records)
            self.assertEqual(invalid,0)
            row=report.frame_timeline(observations)["frames"][0]
            self.assertEqual(row["copy_gpu"][0]["copy_end_before_entry_estimate"],expected)
            self.assertEqual(row["copy_gpu"][0]["duration_ns"],200)
            text="\n".join(report.frame_trace_report(observations,10,30,5))
            self.assertIn("1 exact joins, 1 calibrated host estimates",text)
            self.assertIn(expected+"=1",text)

    def test_native_gpu_clock_failures_and_unavailable_queries_remain_unknown(self):
        records,copy,sample=self.native_gpu_fixture()
        for changes,reason in ((dict(available=0,query_result=1),"query-result-unavailable"),
                               (dict(valid_bits=32),"narrow-device-clock"),
                               (dict(before_result=-7),"calibration-unavailable-or-wrong-host-clock"),
                               (dict(after_host_ns=1),"calibration-unavailable-or-wrong-host-clock"),
                               (dict(before_deviation_ns=0),"calibration-unavailable-or-wrong-host-clock")):
            changed=sample|changes
            observations,invalid=self.parse(records[:-1]+[self.line("pacing-profile-copy-gpu",changed)])
            self.assertEqual(invalid,0)
            gpu=report.frame_timeline(observations)["frames"][0]["copy_gpu"][0]
            self.assertIsNone(gpu["host_estimate"])
            self.assertEqual(gpu["unavailable"],reason)
        wrap=sample.copy()
        for name in ("before_device_ticks","after_device_ticks","begin_ticks","end_ticks"):
            wrap[name]=(wrap[name]+(1<<64)-1900)%(1<<64)
        self.assertEqual(report.native_copy_clock(wrap,copy)["host_estimate"],report.native_copy_clock(sample,copy)["host_estimate"])

    def test_native_gpu_does_not_treat_output_scope_setup_as_driver_entry(self):
        records,_,sample=self.native_gpu_fixture(entry=100)
        output_line=records[-2].replace("finished_ns=1000000110", "finished_ns=1000001000")
        observations,invalid=self.parse(records[:-2]+[output_line,records[-1]])
        self.assertEqual(invalid,0)
        joined=report.frame_timeline(observations)["frames"][0]["copy_gpu"][0]
        self.assertEqual(joined["copy_end_before_entry_estimate"],"ambiguous")
        self.assertEqual(joined["lower_entry_boundary"],"output-scope-bounds")
        origin=1000000000
        request=dict(pid=10,surface_token=1,feedback=1,event="request",context=30,source=1,image_index=3,generated=1,
                     requested_ns=origin+499,clock_id=1,presentation_now_ns=origin+499,calibration_span_ns=1)
        call=dict(pid=10,surface_token=1,feedback=1,event="call",started_ns=origin+500,finished_ns=origin+900,result=0)
        lines=[self.line("pacing-profile-wayland",r).replace("version=1", "version=2") for r in (request,call)]
        observations,invalid=self.parse(records[:-2]+[output_line,records[-1]]+lines)
        self.assertEqual(invalid,0)
        joined=report.frame_timeline(observations)["frames"][0]["copy_gpu"][0]
        self.assertEqual(joined["lower_entry_boundary"],"wayland-lower-call")
        self.assertEqual(joined["copy_end_before_entry_estimate"],"before")

    def test_native_gpu_duplicates_wrong_generation_and_invalid_values(self):
        records,_,sample=self.native_gpu_fixture()
        for observations in (self.parse(records+[records[-1]])[0],
                self.parse(records[:-1]+[self.line("pacing-profile-copy-gpu",sample|dict(generation=2))])[0]):
            timeline=report.frame_timeline(observations)
            self.assertEqual(timeline["frames"][0]["copy_gpu"],[])
            self.assertTrue(timeline["unmatched_copy_gpu"])
        _,invalid=self.parse([self.line("pacing-profile-copy-gpu",sample|changes) for changes in
            (dict(period_ns="nan"),dict(valid_bits=0),dict(generated=0),dict(query_result=-4),dict(read_finished_ns=0))])
        self.assertEqual(invalid,5)


class WaylandProtocolTests(unittest.TestCase):
    @staticmethod
    def fixture(extra_observer=False):
        lines = ["[999.000] wp_presentation@8.clock_id(1)"]
        observations = {"wayland": []}
        for index, (commit_ms, presented_ns) in enumerate(((1000.2, 2000000000), (1016, 2016000000), (1016.6, 2016632000)), 1):
            timestamp = 1000 + (index - 1) * 8.333333
            feedback = 10 + index
            lines += [f"[{timestamp:.3f}] -> wp_presentation@8.feedback(wl_surface@9, new id wp_presentation_feedback@{feedback})"]
            if extra_observer:
                lines += [f"[{timestamp:.3f}] -> wp_presentation@8.feedback(wl_surface@9, new id wp_presentation_feedback@{feedback+20})"]
            lines += [f"[{commit_ms:.3f}] -> wl_surface@9.attach(wl_buffer@{100+index}, 0, 0)",
                      f"[{commit_ms:.3f}] -> wl_surface@9.commit()"]
            for object_id in ([feedback, feedback+20] if extra_observer else [feedback]):
                lines += [f"[{commit_ms+5:.3f}] wp_presentation_feedback@{object_id}.sync_output(wl_output@26)",
                          f"[{commit_ms+5:.3f}] wp_presentation_feedback@{object_id}.presented(0, 2, {presented_ns-2000000000}, 4167404, 0, {100+index}, 14)"]
            r = PacingReportTests.wayland("request", index, presented_ns, generated=index%2,
                sequence=100+index, refresh_ns=4167404, flags=14, output=26,
                requested_ns=1000000000+round((index-1)*1000000000/120))
            observations["wayland"] += [r, r | {"event":"call"}, r | {"event":"presented"}]
        return lines, observations

    def test_correlates_actual_commits_and_deduplicates_other_observers(self):
        lines, observations = self.fixture(extra_observer=True)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"protocol.log"
            path.write_text("\n".join(lines))
            text = protocol.protocol_report(path, observations)
        self.assertIn("Matched outputs=3, unavailable=0, ambiguous=0", text)
        self.assertIn("2 adjacent matched pairs", text)
        self.assertIn("8.333/15.800/16.000 ms", text)
        self.assertIn("8.333/0.600/0.632 ms", text)
        self.assertIn("buffers 102->103", text)

    def test_fifo_state_commits_preserve_consecutive_attachment_comparison(self):
        original, observations = self.fixture(extra_observer=True)
        for inserted_attach, expected in ((None, 2), ("wl_buffer@101", 0), ("null", 0)):
            lines = []
            for line in original:
                lines.append(line)
                if "wl_surface@9.commit()" in line:
                    stamp = line.split("]", 1)[0] + "]"
                    if inserted_attach:
                        lines.append(stamp + f" -> wl_surface@9.attach({inserted_attach}, 0, 0)")
                    lines += [stamp + " -> wp_fifo_v1@80.wait_barrier()", stamp + " -> wl_surface@9.commit()"]
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory)/"protocol.log"
                path.write_text("\n".join(lines))
                text = protocol.protocol_report(path, observations)
            self.assertIn("0 adjacent matched pairs", text)
            self.assertIn(f"Consecutive buffer-attachment matched pairs: {expected}", text)
            if expected:
                self.assertIn("with intervening commits without a new attachment: 2", text)
                self.assertIn("Buffer commit attachment-pair median", text)
                self.assertIn("8.333/15.800/16.000 ms", text)

    def test_attachment_pairs_reject_feedback_gaps_and_context_changes(self):
        lines, observations = self.fixture()
        observations["wayland"] = [r for r in observations["wayland"] if r["feedback"] != 2]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"protocol.log"
            path.write_text("\n".join(lines))
            text = protocol.protocol_report(path, observations)
            self.assertIn("Consecutive buffer-attachment matched pairs: 0", text)
            _, observations = self.fixture()
            observations["wayland"] = [r | {"context": r["feedback"]} for r in observations["wayland"]]
            text = protocol.protocol_report(path, observations)
            self.assertIn("Consecutive buffer-attachment matched pairs: 0", text)
            _, observations = self.fixture()
            observations["wayland"] = [{k: v for k, v in r.items() if k != "context"} for r in observations["wayland"]]
            text = protocol.protocol_report(path, observations)
            self.assertIn("Consecutive buffer-attachment matched pairs: 0", text)

    def test_missing_extra_and_conflicting_commits_are_not_bridged(self):
        lines, observations = self.fixture()
        # Identical feedback outcome assigned to another commit must not choose
        # either commit. An extra unobserved commit prevents adjacent pairing.
        lines += ["[1020.000] -> wp_presentation@8.feedback(wl_surface@9, new id wp_presentation_feedback@50)",
                  "[1020.001] -> wl_surface@9.commit()",
                  "[1020.002] wp_presentation_feedback@50.sync_output(wl_output@26)",
                  "[1020.003] wp_presentation_feedback@50.presented(0, 2, 16000000, 4167404, 0, 102, 14)"]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"protocol.log"
            path.write_text("\n".join(lines))
            text = protocol.protocol_report(path, observations)
        self.assertIn("Matched outputs=2, unavailable=0, ambiguous=1", text)
        self.assertIn("0 adjacent matched pairs", text)

    def test_timestamp_formats_rollover_and_clock_steps(self):
        self.assertEqual(protocol.stamp_ns("01:02:03.000004")[0], 3723000004000)
        self.assertEqual(protocol.stamp_ns("1234.567")[0], 1234567000)
        lines, observations = self.fixture()
        lines[:0] = ["framegen-vulkan: wayland protocol clock monotonic_before_ns=100 realtime_ns=1000 monotonic_after_ns=110",
                     "framegen-vulkan: wayland protocol clock monotonic_before_ns=200 realtime_ns=2002000 monotonic_after_ns=210"]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"protocol.log"
            path.write_text("\n".join(lines))
            text = protocol.protocol_report(path, observations)
        self.assertIn("Realtime clock changed", text)
        self.assertNotIn("Matched outputs", text)
        self.assertIsNotNone(protocol.message(b"\x1b[32m[01:02:03.000004] TID#52 {queue} -> wl_surface#9.commit()\x1b[0m\n"))

    def test_wrapper_preserves_beginning_and_rejects_existing_files(self):
        code = "import sys; sys.stderr.write('[1000.000] -> wl_surface@9.commit()\\n'*200); sys.exit(7)"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"protocol.log"
            with contextlib.redirect_stderr(io.StringIO()):
                result = protocol.capture([sys.executable, "-c", code], path, limit=2048)
                with self.assertRaises(FileExistsError):
                    protocol.capture([sys.executable, "-c", "raise Exception('must not launch')"], path)
            data = path.read_text()
            self.assertEqual(result, 7)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertLessEqual(path.stat().st_size, 2048)
            self.assertTrue(data.startswith("framegen-vulkan: wayland protocol session"))
            self.assertEqual(data.count("capture-limit"), 1)
            self.assertIn("wl_surface@9.commit()", data)

    def test_wrapper_filters_input_and_keeps_normal_stderr_and_exit_status(self):
        for raw in (b'[1000.000] wl_keyboard@2.key(1, 2, 30, 1)',
                    b'[1000.000] -> xdg_toplevel@3.set_title("private title")',
                    b'[1000.000] -> wl_surface@3.attach("private", 0, 0)'):
            self.assertIsNone(protocol.message(raw))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"protocol.log"
            code = ("import os,sys; assert os.environ['WAYLAND_DEBUG']=='client'; "
                    "assert os.environ['FGVK_PACING_PROFILE_WAYLAND']=='1'; "
                    "sys.stderr.write('[1000.000] -> wl_surface@9.commit()\\n'); "
                    "sys.stderr.write('[1000.000] wl_keyboard@2.key(1, 2, 30, 1)\\n'); "
                    "sys.stderr.write('ordinary game stderr\\n'); sys.exit(9)")
            result = subprocess.run([sys.executable, str(SCRIPT.with_name("wayland_pacing_protocol.py")),
                                     "--output", str(path), "--", sys.executable, "-c", code], capture_output=True)
            self.assertEqual(result.returncode, 9, result.stderr)
            self.assertIn(b"ordinary game stderr", result.stderr)
            self.assertNotIn(b"wl_keyboard", result.stderr)
            self.assertNotIn(b"ordinary game", path.read_bytes())
            self.assertIn("wl_surface@9.commit()", path.read_text())


class PacingReportTests(unittest.TestCase):
    @staticmethod
    def wayland(event, feedback, timestamp=0, context=20, clock=1, surface=30, output=40, **updates):
        return dict(version=1, pid=10, surface_token=surface, feedback=feedback, context=context,
                    source=(feedback+1)//2, image_index=feedback%3, generated=feedback%2,
                    requested_ns=feedback*1000000, received_ns=100000000, finished_ns=feedback*1000000+100,
                    presentation_now_ns=0, calibration_span_ns=10, clock_id=clock, presented_ns=timestamp,
                    sequence=timestamp//1000000, refresh_ns=8000000, flags=7, output_id=output,
                    reason=0, result=0, event=event) | updates

    def test_wayland_outcomes_and_clock_isolation(self):
        w=self.wayland
        records=[w("request", i) for i in range(1,7)]
        records += [w("call", i, context=0) for i in range(1,7)]
        records += [w("presented", 1, 100000000), w("presented", 2, 108000000),
                    w("discarded", 3), w("presented", 4, 108000000),
                    w("presented", 5, 1, clock=2), w("abandoned", 6),
                    w("presented", 99, 200000000, context=21)]
        text="\n".join(report.wayland_report(records,10,20,0,1000000000,5))
        self.assertIn("requests=6, presented=4, discarded=1, abandoned=1, pending=0",text)
        self.assertIn("duplicate timestamp/sequence observations=1",text)
        self.assertIn("8.000/8.000/8.000/8.000/8.000",text)
        self.assertIn("clock=2",text)
        self.assertNotIn("Compositor gap 100.000",text)
        self.assertNotIn("feedback 99",text)
        self.assertIn("cannot be assigned distinct visible frames",text)

    def test_wayland_reader_keeps_provenance_and_rejects_bad_events(self):
        record=self.wayland("request",1)
        def line(values):
            return "framegen-vulkan: present diagnostics: operation=pacing-profile-wayland " + " ".join(f"{k}={v}" for k,v in values.items())
        prefix="framegen-vulkan: present diagnostics: operation=pacing-profile "
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"capture.log"
            path.write_text("\n".join([line(record),line(record | {"event":"private"}),line(record | {"generated":2}),
                prefix+"version=2 pid=10 context=20 wsi=wayland window_ms=1000 sources=60 output_attempts=120 source_interval_samples=60 source_interval_mean_ms=16 source_interval_max_ms=20"]))
            observations,invalid=report.read_observations(path)
            windows,bad=report.read_windows(path)
        self.assertEqual(invalid,2)
        self.assertEqual(bad,0)
        self.assertEqual(windows[0].wsi,"wayland")
        self.assertEqual(len(observations["wayland"]),1)
        self.assertIn("Recorded WSI: wayland",report.report(windows))
        self.assertIn("not evidence of zero discards", "\n".join(report.wayland_report([],10,20,0,100,5)))

    def test_compact_wayland_records_join_identity_without_inventing_missing_requests(self):
        prefix="framegen-vulkan: present diagnostics: operation=pacing-profile-wayland version=2 pid=10 surface_token=30 "
        lines=[prefix+"event=request feedback=1 context=20 source=1 image_index=2 generated=1 requested_ns=100 clock_id=1 presentation_now_ns=100 calibration_span_ns=1",
               prefix+"event=call feedback=1 started_ns=110 finished_ns=120 result=0",
               prefix+"event=presented feedback=1 received_ns=150 clock_id=1 presented_ns=140 sequence=1 refresh_ns=8 flags=7 output_id=40",
               prefix+"event=discarded feedback=2 received_ns=160",
               prefix+"event=call feedback=3 finished_ns=120 result=0"]
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"capture.log"
            path.write_text("\n".join(lines))
            observations,invalid=report.read_observations(path)
        self.assertEqual(invalid,1)
        records=observations["wayland"]
        self.assertEqual([r["context"] for r in records],[20,20,20,0])
        self.assertEqual(records[2]["generated"],1)
        self.assertEqual(records[1]["requested_ns"],110)
        text="\n".join(report.wayland_report(records,10,20,0,1000,3))
        self.assertIn("requests=1, presented=1, discarded=0",text)

    def test_capture_retention_limits_and_fg_transitions_are_explicit(self):
        prefix="framegen-vulkan: present diagnostics: operation="
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"capture.log"
            path.write_text(prefix+"pacing-profile-log-wrap version=2\n"+prefix+
                "pacing-profile-capture-limit version=1 pid=10 limit_bytes=67108864 captured_bytes=66000000 rejected_bytes=1000000\n")
            observations,invalid=report.read_observations(path)
        self.assertEqual(invalid,0)
        # smooth_cadence is a field of old logs only: it is accepted and never printed.
        base={"window_ms":1000,"sources":60,"output_attempts":120,"sample_end_monotonic_ms":1000,
              "smooth_cadence":0,"effective_base_cap_fps":60,"present_mode":2,"configured_multiplier":2,
              "configured_base_cap_fps":60,"adaptive":0}
        windows=[report.Window(10,20,2,base|{"frame_generation_enabled":1},"0x12","wayland"),
                 report.Window(10,20,2,base|{"frame_generation_enabled":0,"sample_end_monotonic_ms":2000},"0x12","wayland")]
        text=report.report(windows,observations=observations)
        self.assertIn("Capture has wrapped",text)
        self.assertIn("preserving the beginning",text)
        self.assertIn("fg=1 multiplier=2 adaptive=0",text)
        self.assertIn("fg=0 multiplier=2 adaptive=0",text)
        self.assertNotIn("smooth",text)

    def test_context_isolation_and_correlated_hitch(self):
        prefix = "framegen-vulkan: present diagnostics: operation=pacing-profile "
        base = "version=2 pid=10 window_ms=1000 sources=60 output_attempts=120 source_interval_samples=60 source_interval_mean_ms=16.667 source_interval_max_ms=50 "
        lines = ["private launcher text must never be printed",
                 prefix + base + "context=20 worst_source_interval_ms=50 worst_source_layer_ms=10 worst_source_outside_ms=40 worst_source_profiler_publish_ms=1 profiler_publish_max_ms=1",
                 prefix + base + "context=21",
                 prefix + base + "context=22 worst_source_interval_ms=nan"]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.log"
            path.write_text("\n".join(lines), encoding="utf-8")
            windows, invalid = report.read_windows(path)
        self.assertEqual(invalid, 1)
        text = report.report(windows)
        self.assertIn("context 20", text)
        self.assertIn("context 21", text)
        self.assertIn("60.00 frames/s", text)
        self.assertIn("120.00 attempts/s", text)
        self.assertIn("outside that present: 40.000 ms", text)
        self.assertIn("included in outside time", text)
        self.assertNotIn("private launcher", text)

    def test_legacy_does_not_invent_attribution(self):
        window = report.Window(10, 20, 1, {"window_ms": 1000, "sources": 20,
            "output_attempts": 40, "source_interval_max_ms": 50})
        self.assertIn("attribution is unavailable", report.report([window]))
        self.assertIn("Output timing owner/targets unavailable", report.report([window]))

    def test_timing_owners_and_requested_targets(self):
        base = {"window_ms": 1000, "sources": 60, "output_attempts": 120, "present_mode": 2}
        windows = [report.Window(10, 20, 2, base | {
            "timing_driver_attempts": 20, "timing_application_attempts": 20,
            "timing_ext_attempts": 80, "fifo_caller_timing_sources": 60, "present_target_interval_samples": 2,
            "present_target_interval_mean_ms": 5, "present_target_discontinuities": 1}),
            report.Window(10, 20, 2, base | {
                "timing_driver_attempts": 0, "timing_google_attempts": 120,
                "present_target_interval_samples": 6, "present_target_interval_mean_ms": 9}),
            report.Window(10, 20, 2, base)]
        text = report.report(windows)
        self.assertIn("driver=20, application=20, ext=80, google=120, bridge=0", text)
        self.assertIn("mean 8.000 ms, samples 8 (not display timing)", text)
        self.assertIn("Repeated/backwards requested targets: 1", text)
        self.assertIn("coverage is partial", text)
        self.assertIn("with caller timing/wait capabilities: 60", text)
        self.assertIn("Output timing owner/targets unavailable", report.report(windows[-1:]))

    def test_gpu_weighting_and_unavailable_are_distinct(self):
        base = {"window_ms": 1000, "sources": 70, "output_attempts": 140, "gpu_requested": 1}
        windows = [report.Window(10, 20, 2, base | {
            "gpu_span_samples": 2, "gpu_span_mean_ms": 3, "gpu_span_max_ms": 4,
            "gpu_readback_samples": 2, "gpu_readback_mean_ms": .02, "gpu_readback_max_ms": .03}),
            report.Window(10, 20, 2, base | {
                "gpu_span_samples": 6, "gpu_span_mean_ms": 1, "gpu_span_max_ms": 2}),
            report.Window(10, 21, 2, base | {"gpu_span_samples": 0})]
        text = report.report(windows)
        self.assertIn("mean 1.500 ms, maximum 4.000 ms, samples 8", text)
        self.assertIn("CPU query readback: mean 0.020 ms", text)
        self.assertIn("not zero GPU cost", text)
        self.assertIn("excludes game rendering", text)
        self.assertNotIn("GPU timing unavailable", report.report(windows[:2]))
        self.assertNotIn("GPU whole batch", report.report(windows[2:]))

    def test_individual_outputs_keep_gaps_and_unavailable_copy_state(self):
        def sample(ordinal, source, generated, started, status=1, checked=1):
            return dict(ordinal=ordinal, source=source, generated=generated, deferred=1, image_index=ordinal % 3,
                        scheduled_ns=started - 1000000, started_ns=started, finished_ns=started + 100000,
                        result=0, copy_checked=checked, copy_status=status, copy_sampled_ns=started + 101000 if checked else 0)
        samples = [sample(1, 1, 1, 1000000), sample(2, 1, 0, 2000000, 0),
                   sample(4, 2, 1, 12000000, checked=0), sample(5, 2, 0, 28000000, -4)]
        text = "\n".join(report.output_report(samples, 2))
        self.assertIn("1 discontinuous pairs excluded", text)
        self.assertIn("1.000/1.000/16.000/16.000/16.000", text)
        self.assertIn("ready=1, pending=1, errors=1, unchecked=1", text)
        self.assertIn("source 2 generated image 1 -> source 2 real image 2", text)
        self.assertNotIn("Gap 10.000", text)

    def test_trace_reader_and_actual_waits_are_swapchain_local(self):
        prefix = "framegen-vulkan: present diagnostics: operation="
        trace = (prefix + "pacing-profile-output version=1 pid=10 context=20 ordinal=1 source=1 generated=1 deferred=1 "
                 "image_index=2 scheduled_ns=0 started_ns=100 finished_ns=200 result=-4 copy_checked=0 copy_status=1 copy_sampled_ns=0")
        wait = (prefix + "application-present-wait pid=10 swapchain=0x12 api=KHR2 sample_end_monotonic_ms=1000 "
                "calls=60 successful=59 timeouts=1 errors=0 polls=1 duration_mean_ms=8 duration_max_ms=20")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.log"
            path.write_text("\n".join([trace, trace.replace("finished_ns=200", "finished_ns=50"),
                                        wait, wait.replace("swapchain=0x12", "swapchain=0x13"),
                                        wait.replace("duration_mean_ms=8", "duration_mean_ms=nan")]))
            observations, invalid = report.read_observations(path)
        self.assertEqual(invalid, 2)
        self.assertEqual(len(observations["outputs"]), 1)
        window = report.Window(10, 20, 2, {"window_ms": 1000, "sources": 60, "output_attempts": 120,
            "sample_end_monotonic_ms": 1000, "wait_capture": 1}, "0x12")
        text = report.report([window], observations=observations)
        self.assertIn("Actual KHR2 present waits: calls=60, successful=59, timeouts=1", text)
        self.assertNotIn("calls=120", text)
        old = report.report([window._replace(values=window.values | {"wait_capture": 0})])
        self.assertIn("did not include lightweight wait observations", old)

    def test_outer_present_join_is_not_reported_as_game_work(self):
        window = report.Window(10, 20, 2, {"window_ms": 1000, "sources": 60, "output_attempts": 120,
            "sample_end_monotonic_ms": 1000}, "0x12")
        presents = [dict(pid=10, swapchain="0x12", sample_end_monotonic_ms=1000, calls=60,
                         duration_mean_ms=14, duration_max_ms=18, deferred_join_samples=60,
                         deferred_join_mean_ms=13, deferred_join_max_ms=17)]
        text = report.report([window], observations={"presents": presents})
        self.assertIn("Outer application present CPU duration mean/max: 14.000/18.000", text)
        self.assertIn("before configuration/present mean/max: 13.000/17.000", text)
        old = {key: value for key, value in presents[0].items() if not key.startswith("deferred_join")}
        text = report.report([window], observations={"presents": [old]})
        self.assertIn("join timing unavailable", text)


class PresentationWindowTests(unittest.TestCase):
    @staticmethod
    def fixture():
        rows = []
        for i, presented_ms in enumerate((100, 112, 116, 128)):
            source = i // 2 + 1
            entry = 1_000_000_000 + i * 8_000_000
            request = PacingReportTests.wayland("request", i+1, context=30, surface=40, clock=2,
                source=source, requested_ns=entry-1000, image_index=i, generated=1-i%2)
            call = request | dict(event="call", requested_ns=entry, finished_ns=entry+100_000)
            presented = request | dict(event="presented", presented_ns=presented_ms*1_000_000,
                                       sequence=i+1, refresh_ns=4_000_000)
            output = dict(pid=10, context=30, source=source, ordinal=i+1, generated=1-i%2,
                          image_index=i, started_ns=entry-2000, finished_ns=entry+200_000,
                          scheduled_ns=entry-10_000, result=0)
            copy = dict(sample=dict(generated=1-i%2, image_index=i), lower_entry_boundary="wayland-lower-call",
                        lower_entry_low_ns=entry, copy_end_before_entry_estimate="before", copy_end_to_entry_low_ns=2_000_000)
            if i % 2 == 0:
                rows.append(dict(source=dict(pid=10, context=30, source=source), outputs=[], copy_gpu=[], wayland=[]))
            rows[-1]["outputs"].append(output)
            rows[-1]["copy_gpu"].append(copy)
            rows[-1]["wayland"].extend((request, call, presented))
        return dict(frames=rows)

    def test_stable_calls_uneven_feedback_and_different_clocks(self):
        timeline = self.fixture()
        text = report.presentation_window_report(timeline, [(0, .03), (.012, .017)], 3)
        self.assertIn("4 presented observations, 4 exact lower-output joins, 3 consecutive pairs", text)
        self.assertIn("Lower-call entry gaps min/p50/p95/p99/max (ms): 8.000/8.000/8.000/8.000/8.000", text)
        self.assertIn("Compositor-reported presentation gaps min/p50/p95/p99/max (ms): 4.000/12.000/12.000/12.000/12.000", text)
        self.assertIn("before=4, after=0, ambiguous=0, unavailable=0", text)
        self.assertIn("2 presented observations, 2 exact lower-output joins, 1 consecutive pairs", text)
        self.assertIn("copy_end_lead_ms=2.000", text)
        # The compositor's clock 2 starts far from the CPU clock; never
        # subtract these domains to manufacture a presentation latency.
        self.assertNotIn("latency", text)
        self.assertIn("requests, not verified buffer commits", text)

    def test_missing_or_ambiguous_boundaries_do_not_bridge_pairs(self):
        for kind in ("request", "call", "output", "outside-output", "feedback-gap", "ordinal-gap", "timestamp-duplicate", "failed"):
            with self.subTest(kind=kind):
                timeline = self.fixture()
                row = timeline["frames"][0]
                if kind in ("request", "call"):
                    record = next(e for e in row["wayland"] if e["feedback"] == 2 and e["event"] == kind)
                    row["wayland"].append(record.copy())
                elif kind == "output":
                    row["outputs"].append(row["outputs"][1].copy())
                elif kind == "outside-output":
                    row["outputs"][1]["finished_ns"] -= 150_000
                elif kind == "feedback-gap":
                    for e in row["wayland"]:
                        if e["feedback"] == 2:
                            e["feedback"] = 9
                elif kind == "ordinal-gap":
                    row["outputs"][1]["ordinal"] = 9
                elif kind == "timestamp-duplicate":
                    row["wayland"][-1]["presented_ns"] = row["wayland"][2]["presented_ns"]
                else:
                    row["outputs"][1]["result"] = -4
                text = report.presentation_window_report(timeline, [(0, .03)], 5)
                self.assertIn("1 consecutive pairs", text)
                self.assertNotIn("feedback=2 source=1 real lower_entry", text)

    def test_clock_output_and_missing_gpu_evidence_are_isolated(self):
        for field in ("clock_id", "output_id", "surface_token"):
            timeline = self.fixture()
            for e in timeline["frames"][1]["wayland"]:
                e[field] += 10
            text = report.presentation_window_report(timeline, [(0, .03)], 5)
            self.assertEqual(text.count("1 consecutive pairs"), 2)
            self.assertNotIn("3 consecutive pairs", text)
        timeline = self.fixture()
        timeline["frames"][0]["copy_gpu"][0]["lower_entry_boundary"] = "output-scope-bounds"
        timeline["frames"][0]["copy_gpu"][1]["copy_end_before_entry_estimate"] = "ambiguous"
        timeline["frames"][1]["copy_gpu"] = []
        text = report.presentation_window_report(timeline, [(0, .03)], 2)
        self.assertIn("before=0, after=0, ambiguous=1, unavailable=3", text)
        self.assertIn("copy_end_lead_ms=unavailable", text)

    def test_backwards_and_overlapping_calls_cannot_create_intervals(self):
        timeline = self.fixture()
        row = timeline["frames"][0]
        # First lower call extends into the second; the scopes still contain
        # each call, but their entries do not prove serial presentation.
        row["wayland"][1]["finished_ns"] += 9_000_000
        row["outputs"][0]["finished_ns"] += 9_000_000
        text = report.presentation_window_report(timeline, [(0, .03)], 5)
        self.assertIn("2 consecutive pairs; 1 pairs excluded", text)
        timeline = self.fixture()
        timeline["frames"][0]["wayland"][-1]["presented_ns"] = 99_000_000
        text = report.presentation_window_report(timeline, [(0, .03)], 5)
        self.assertIn("2 consecutive pairs; 1 pairs excluded", text)

    def test_interval_validation_and_missing_feedback(self):
        import argparse
        self.assertEqual(report.presentation_interval("90:95"), (90, 95))
        for value in ("nan:1", "0:inf", "-1:2", "2:1", "1:1", "1", "1:2:3", "a:2"):
            with self.assertRaises(argparse.ArgumentTypeError):
                report.presentation_interval(value)
        self.assertIn("No uniquely requested/presented feedback", report.presentation_window_report(dict(frames=[]), [(0, 1)], 1))

    def test_recreation_keeps_surface_origin_and_nominal_drift_is_separate(self):
        timeline = self.fixture()
        row = timeline["frames"][1]
        row["source"]["context"] = 31
        for record in row["wayland"] + row["outputs"]:
            record["context"] = 31
        text = report.presentation_window_report(timeline, [(.016, .03)], 1)
        self.assertIn("context 31", text)
        self.assertNotIn("context 30", text)
        self.assertIn("2 presented observations", text)
        self.assertIn("origin=100000000 ns", text)
        timeline = self.fixture()
        for row in timeline["frames"]:
            for output in row["outputs"]:
                output["scheduled_ns"] = 1_000_000_000 + (output["ordinal"]-1)*8_333_333
            for event in row["wayland"]:
                event["refresh_ns"] = 4_167_395
        text = report.presentation_window_report(timeline, [(0, .03)], 1)
        self.assertIn("refresh multiple: -1457 ns", text)
        self.assertIn("no measured vblank phase or causal attribution", text)


class VulkanSetupTests(unittest.TestCase):
    def fixture(self, pid=10, device="0x123", extensions="VK_KHR_swapchain", width=2560):
        lines = []
        for call, kind in ((1, "device"), (2, "swapchain")):
            for boundary in ("incoming", "forwarded"):
                fields = dict(version=1, pid=pid, role="frame-generation", call=call, kind=kind,
                              boundary=boundary, result=0, handle=device if kind == "device" else "0x456",
                              flags=0, pnext="-", pnext_truncated=0)
                if kind == "device":
                    fields.update(extension_count=len(extensions.split(",")), extensions=extensions,
                                  extensions_truncated=0, queues="0:1:0", queues_truncated=0, presentWait2=1)
                else:
                    fields.update(device=device, width=width, height=1440, min_images=3 if boundary == "incoming" else 6,
                                  present_mode=2, format=44)
                lines.append(setup.PREFIX + " ".join(f"{k}={v}" for k, v in fields.items()) + "\n")
        return lines

    def snapshot(self, lines, pid=None):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"setup.log"
            path.write_text("".join(lines))
            return setup.snapshot(path, pid)

    def test_enabled_sets_and_real_device_identity_not_raw_handle_equality(self):
        left = self.snapshot(self.fixture())
        right = self.snapshot(self.fixture(pid=20, device="0x789"))
        self.assertEqual(setup.compare(left, right), [])
        right = self.snapshot(self.fixture(extensions="VK_KHR_swapchain,VK_EXT_descriptor_heap", width=1920))
        differences = setup.compare(left, right)
        self.assertEqual({d["field"] for d in differences}, {"extension_count", "extensions", "width"})
        self.assertEqual(left["selected"]["swapchain.incoming"]["min_images"], "3")
        self.assertEqual(left["selected"]["swapchain.forwarded"]["min_images"], "6")

    def test_missing_old_or_dropped_setup_is_unavailable(self):
        old = self.snapshot(["framegen-vulkan: present diagnostics: operation=pacing-profile-source version=1\n"])
        self.assertFalse(old["available"])
        self.assertTrue(all(d["evidence"] == "unavailable" for d in setup.compare(old, old)))
        partial = self.snapshot(self.fixture()[:-1])
        self.assertEqual(partial["incomplete_calls"], 1)
        self.assertFalse(partial["available"])
        duplicate = self.snapshot(self.fixture() + self.fixture()[:1])
        self.assertTrue(duplicate["unavailable"])

    def test_processes_failed_creates_and_recreation_stay_separate(self):
        lines = self.fixture() + self.fixture(pid=20)
        with self.assertRaisesRegex(ValueError, "multiple PIDs"):
            self.snapshot(lines)
        self.assertEqual(self.snapshot(lines, 20)["pid"], 20)
        lines = self.fixture() + [s.replace("call=2", "call=3").replace("result=0", "result=-8") for s in self.fixture()[2:]]
        row = self.snapshot(lines)
        self.assertEqual(row["successful_swapchain_creates"], 1)
        self.assertEqual(row["selected"]["swapchain.forwarded"]["call"], 2)

    def test_malformed_values_and_truncation_never_invent_full_extension_set(self):
        rows = self.fixture()
        rows[0] = rows[0].replace("presentWait2=1", "presentWait2=7")
        row = self.snapshot(rows)
        self.assertEqual(row["malformed_records"], 1)
        self.assertTrue(row["unavailable"])
        rows = self.fixture()
        rows[0] = rows[0].replace("extensions_truncated=0", "extensions_truncated=1")
        row = self.snapshot(rows)
        self.assertEqual(setup.fields_for_comparison(row["selected"]["device.incoming"])["extensions"], "unavailable (truncated)")
        self.assertTrue(any(d.get("evidence") == "unavailable (truncated)" for d in setup.compare(row, row)))
        self.assertEqual(self.snapshot([setup.PREFIX + "version=1 pid=10 call=1 kind=device boundary=incoming role=frame-generation result=0\n"])["malformed_records"], 1)

    def test_runtime_companion_is_private_child_only_filtered_and_bounded(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runtime = root/"runtime.log"
            wire = root/"wire.log"
            child = ('import os,sys; '
                     'print("info: runtime="+os.environ["VKD3D_DEBUG"],file=sys.stderr); '
                     'print("unrelated private game output",file=sys.stderr); '
                     'print("[123.456] -> wl_keyboard@1.key(1, 2, 3, 4)",file=sys.stderr); '
                     'print("info: "+"x"*4194304,file=sys.stderr)')
            result = subprocess.run([sys.executable, str(SCRIPT.with_name("wayland_pacing_protocol.py")),
                                     "--output", str(wire), "--runtime-log", str(runtime), "--",
                                     sys.executable, "-c", child], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr[:1000])
            text = runtime.read_text()
            self.assertIn("info: runtime=info", text)
            self.assertNotIn("unrelated private", text)
            self.assertNotIn("wl_keyboard", text)
            self.assertIn("runtime capture-limit", text)
            self.assertLessEqual(runtime.stat().st_size, 4*1024*1024)
            self.assertEqual(runtime.stat().st_mode & 0o777, 0o600)
            again = subprocess.run([sys.executable, str(SCRIPT.with_name("wayland_pacing_protocol.py")),
                                    "--output", str(root/"second-wire.log"), "--runtime-log", str(runtime),
                                    "--", sys.executable, "-c", 'raise Exception("must not start")'], capture_output=True)
            self.assertNotEqual(again.returncode, 0)
            self.assertEqual(runtime.read_text(), text)


class WindowFieldTests(unittest.TestCase):
    """Summary lines from current logs carry text and signed fields that the report must accept."""
    prefix = "framegen-vulkan: present diagnostics: operation=pacing-profile "
    base = ("version=2 pid=10 context=20 sample_end_monotonic_ms=16871525 window_ms=1004.13 "
            "present_mode=0 sources=128 swapchain=0x3000 wsi=wayland wayland_requested=1 "
            "output_attempts=215 source_interval_samples=128 source_interval_mean_ms=7.8 "
            "source_interval_max_ms=20 internal_images=1 configured_multiplier=2 "
            "frame_generation_enabled=1 swapchain_present_mode=2 ")
    current = ("application_present_modes=immediate:75 real_present_modes=immediate:128 "
               "generated_present_modes=immediate:89 real_offset_us=-120")

    def read(self, lines):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.log"
            path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            return report.read_windows(path)

    def test_text_fields_and_signed_offsets_are_accepted(self):
        windows, invalid = self.read([self.prefix + self.base + self.current + " real_offset_samples=3"])
        self.assertEqual(invalid, 0)
        self.assertEqual(len(windows), 1)
        values = windows[0].values
        self.assertEqual(values["application_present_modes"], "immediate:75")
        self.assertEqual(values["generated_present_modes"], "immediate:89")
        self.assertEqual(values["real_offset_us"], -120.0)
        self.assertEqual(values["window_ms"], 1004.13)
        self.assertEqual(values["sources"], 128.0)
        text = report.report(windows)
        self.assertIn("PID 10, context 20: 1 windows", text)
        self.assertIn("Source: 127.47 frames/s", text)

    def test_empty_mode_fields_and_negative_present_mode_sentinel_are_accepted(self):
        line = self.prefix + self.base.replace("present_mode=0", "present_mode=-1") + \
            "application_present_modes=- real_present_modes= generated_present_modes= real_offset_us=0"
        windows, invalid = self.read([line])
        self.assertEqual((len(windows), invalid), (1, 0))
        self.assertEqual(windows[0].values["present_mode"], -1.0)
        self.assertEqual(windows[0].values["application_present_modes"], "-")

    def test_park_counters_are_plain_counts(self):
        # FP3: the park counters follow inline_call_parked. They are plain counts, not computed measurements.
        body = self.base + self.current + (" inline_call_parked=2 park_ok=2 park_full=1 park_budget=1 "
            "park_slot_busy=0 park_fence=1 park_acquire=0 park_error=3 park_retry=2 park_retry_ok=1")
        windows, invalid = self.read([self.prefix + body])
        self.assertEqual((len(windows), invalid), (1, 0))
        self.assertEqual(windows[0].values["park_ok"], 2.0)
        self.assertEqual(windows[0].values["park_error"], 3.0)
        self.assertEqual(windows[0].values["park_retry_ok"], 1.0)

    def test_computed_fields_still_reject_bad_values(self):
        cases = {
            "negative count": self.base.replace("sources=128", "sources=-1") + self.current,
            "negative duration": self.base + self.current + " worst_source_interval_ms=-5",
            "text in a computed field": self.base.replace("window_ms=1004.13", "window_ms=abc") + self.current,
            "nan duration": self.base + self.current + " source_interval_max_ms=nan",
            "negative unsigned offset": self.base + self.current.replace("real_offset_us=-120", "output_wait_us=-1"),
        }
        for name, body in cases.items():
            with self.subTest(name):
                windows, invalid = self.read([self.prefix + body])
                self.assertEqual((len(windows), invalid), (0, 1))

    def test_timing_counts_are_numbers_inside_the_report(self):
        timing = ("timing_driver_attempts=1 timing_application_attempts=0 timing_ext_attempts=0 "
                  "timing_google_attempts=0 timing_bridge_attempts=0 present_target_discontinuities=2 "
                  "present_target_interval_samples=3 present_target_interval_mean_ms=16.5 "
                  "present_target_interval_max_ms=17")
        windows, invalid = self.read([self.prefix + self.base + self.current + " " + timing])
        self.assertEqual((len(windows), invalid), (1, 0))
        text = report.report(windows)
        self.assertIn("Lower-present timing attempts: driver=1", text)
        self.assertIn("Repeated/backwards requested targets: 2", text)

    def test_command_line_reports_a_current_log(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.log"
            path.write_text(self.prefix + self.base + self.current + "\n", encoding="utf-8")
            result = subprocess.run([sys.executable, "-I", str(SCRIPT), str(path)],
                                    capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("No supported pacing-profile windows", result.stdout)
        self.assertIn("PID 10, context 20: 1 windows", result.stdout)


if __name__ == "__main__":
    unittest.main()

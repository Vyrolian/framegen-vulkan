# Collect MAKO Decky Diagnostics

Use this guide when MAKO Decky opens and games use `mako-run`. For `mako-launch`, use the [standalone guide](../../engine/docs/COLLECT_DIAGNOSTICS.md).

## 1. Enable logs

Confirm **MAKO Renderer is installed** in MAKO Decky. Fully close the game and save its current launch settings so you can restore them afterward.

**Steam or Proton:** prepend the two diagnostics variables to your existing Launch Options, preserving any other settings. For a basic launch, the result is:

```text
MAKO_PRESENT_DIAGNOSTICS=1 MAKO_PRESENT_DIAGNOSTICS_THRESHOLD_MS=25 /home/deck/.local/bin/mako-run %command%
```

**Heroic or Lutris:** keep the working Wrapper or Command prefix and add these per-game environment variables:

```text
MAKO_PRESENT_DIAGNOSTICS=1
MAKO_PRESENT_DIAGNOSTICS_THRESHOLD_MS=25
```

**EmuDeck Flatpak shortcut:** save the original **Target** and **Launch Options**. Set Target to `/usr/bin/env`, keep Start In as `/usr/bin`, and prepend this to the original Launch Options, followed by a space:

```text
MAKO_PRESENT_DIAGNOSTICS=1 MAKO_PRESENT_DIAGNOSTICS_THRESHOLD_MS=25 /home/deck/.local/bin/mako-run
```

Keep all original emulator arguments and the ROM path. Do not add `%command%` here. Native emulators and AppImages use the Steam method above.

If MAKO Decky's **Wrapper path for this device** uses another home directory, replace `/home/deck` in all commands in this guide with that directory.

## 2. Reproduce the problem

When comparing a game with a synthetic reproducer, supporting full-trace builds also retain enabled Vulkan extensions, selected feature values, queues, surface API and incoming/forwarded swapchain requests in `pacing-profile-vulkan-setup` records. The [Vulkan setup comparison guide](../../engine/docs/COLLECT_DIAGNOSTICS.md#vulkan-setup-comparisons) documents the reader and optional bounded VKD3D/DXVK runtime companion. Enabled setup is separate from actual wait/commit behavior and visible reproduction; missing older setup cannot be reconstructed from timing logs.

For a supporting Renderer build, `MAKO_PACING_PROFILE_TRACE=1` enables the [detailed game frame trace](../../engine/docs/COLLECT_DIAGNOSTICS.md#detailed-game-frame-trace): individual source/application/wait records, correctly attributed completed GPU batches, worker readiness/queue-lock timing, individual driver acquire attempts, actual present results, native FG copy queue/semaphore dependencies, compositor feedback and the mapped Renderer ELF build ID. Keep the existing `mako-run` command and settings. The private capture preserves its beginning up to 64 MiB, then stops with an explicit marker. Retain the raw file for exact joins; `analyze-pacing-profile.py --frames-json PATH` writes a new private per-source timeline and keeps missing or ambiguous evidence unavailable, including successful acquisition-to-present ownership and separate driver versus MAKO/observer acquisition time. Failed/unmatched acquisitions remain in the export. These additions use the same flag. Native unscaled copies additionally record bounded GPU timestamps and before/after device-to-host calibrations; the reader separates copy-end estimates before/after lower-call entry, uncertainty and missing evidence. Collection reuses the normal completion fence, adds no presentation wait and reports its query/calibration CPU cost. Unsupported clocks retain raw timestamps; estimates do not measure simulation, the later WSI semaphore signal or physical scanout. This diagnostic mode adds per-call file publication, GPU timestamp and protocol overhead; it changes no profile or pacing policy. Remove the flag after capture. The existing Wayland companion accepts `--frame-trace` before `--` to enable this mode only for its child while retaining exact commit evidence.

The source-checkout analyzer also accepts repeated `--presentation-window START:END` options to compare good and bad portions of an existing full trace without another game run. Offsets follow each process/surface/clock/output's first presented feedback across context recreation; exact consecutive output joins keep CPU call boundaries, calibrated copy completion and compositor timing separate. See the [detailed trace guide](../../engine/docs/COLLECT_DIAGNOSTICS.md#detailed-game-frame-trace) for the command and association limits.

For a lightweight source/wait/output-spacing comparison, including Minecraft, add `MAKO_PACING_PROFILE=1` to the existing launch environment. This is independent of the two verbose diagnostics variables above and writes a private MAKO-only capture under `${TMPDIR:-/tmp}`. Run `mako-diagnostics --pacing performance` to collect the newest capture. Follow the [pacing capture guide](../../engine/docs/COLLECT_DIAGNOSTICS.md#lightweight-minecraft-pacing-capture) for correlated hitch/CPU-phase fields, profiler publication cost, matched runs and evidence limits; keep your working `mako-run` integration. The collector retains the profile/session/wrap/gap records in every preset. For optional in-game Frame Generation GPU spans, use `MAKO_PACING_PROFILE_GPU=1`; this implies the CPU capture and adds timestamp writes/readback at the existing completed-work boundary. The linked guide explains scope, overhead and unavailable samples. Remove both flags after testing.

Current captures also retain lower-present timing-path counts and intervals between MAKO's requested Vulkan targets. Use these to identify whether FIFO had explicit MAKO targets, caller timing, bridge timing or ordinary driver presentation. CPU submission spacing and requested targets do not measure displayed cadence; older captures cannot establish the active timing path. The `fifo_caller_timing_sources` field records enabled caller timing/wait capabilities; actual target nodes, rather than capability enablement alone, exclude the synchronous native FIFO fallback. See the linked guide for exact field meanings.

Current lightweight captures also buffer individual output records with source/output ordinals, real/generated role, image index, CPU deadline and actual lower-call timestamps. Actual KHR/KHR2 application waits now appear in the same file without broad diagnostics. GPU mode samples the existing final-copy fence once after each applicable lower present returns, without blocking; unknown, pending and signaled states remain distinct, with a timestamp for the completed observation. Every preset retains these records. The report reader shows individual gaps and percentiles and separates setting changes; image identities, CPU calls and a final-copy fence do not establish visible frame content or scanout. See the linked guide for bounded storage, missing-record markers and exact fence scope.

Current `application-queue-present` records also expose the retained-output join before configuration polling as `deferred_join_*`. It is part of the outer application call and precedes the inner layer timer; configuration-update time excludes it. The reader preserves unavailable evidence in older captures, so an unexplained outer/inner timing difference is not relabelled as game work or a wait2 call.

Start the game, reproduce the issue once and note what happened. Fully exit the game and wait a few seconds.

## 3. Save the report

Switch to **Desktop Mode**, open **Konsole**, paste this command and press Enter:

```bash
/home/deck/.local/bin/mako-diagnostics --lines 5000 all > /home/deck/Desktop/MAKO-diagnostics.txt 2>&1
```

Open your **Desktop** folder to find `MAKO-diagnostics.txt`. Konsole normally prints nothing.

If the command is missing, select **Install MAKO Renderer** in Decky and retry. If no log is found, check the temporary launch settings and repeat the test.

For a slowdown after a long session, replace `--lines 5000` with `--lines 200000`. Collect before starting another game.

With diagnostics enabled, `mako-run` retains ten launch sessions: the current `~/.config/mako-render/present-diagnostics.log` and nine older logs numbered `.1` through `.9`. Add `--session previous` to collect the preceding run, `--session oldest` for `.9`, or `--session all` to combine the available retained sessions.

Collect from the original session log, not an older filtered report, which may have omitted error details. The report does not include the separate Gamescope system journal; support may request it if the game log is insufficient.

For comparisons, collect a separate report after each run and note which settings changed. Use the same scene and game settings where possible. For HDR sharpening comparisons, also note the game resolution and display resolution. For VRR comparisons, record the VRR setting, display refresh rate, desktop or Gaming Mode session, and Gamescope version if used.

`ready_batches` and `cap_batches` count planned output spacing. `worker_batches` counts completed retained batches, and `worker_readiness_misses` counts batches that exhausted readiness or failed it and drained images through ordinary semaphore-protected delivery. Planned counts alone do not establish actual submission spacing or physical cadence. Worker readiness runs outside the producer's `readiness_wait` timing; compare output intervals and GPU spans as well.

For timing-field meanings and the limits of log-based comparisons, see [Interpreting presentation records](../../engine/docs/WSI-ISOLATION.md#interpreting-presentation-records).

Native Wayland investigation can additionally use `MAKO_PACING_PROFILE_WAYLAND=1` with a supporting Renderer build. It implies the private CPU capture and records compositor `wp_presentation` timestamps/discards for the game's existing surface. All collector presets retain `pacing-profile-wayland` and gap records. This is separate from Xwayland completion evidence; the summary records the actual Vulkan surface entrypoint as `wsi`. Preserve the existing launch command, restart after adding the process-start flag, and remove it after capture. See the [native Wayland capture contract](../../engine/docs/COLLECT_DIAGNOSTICS.md#lightweight-minecraft-pacing-capture) for unavailable feedback, clock domains, overhead and ambiguous commit association.

For unresolved lower-call versus compositor timing, that guide also documents the source-checkout `engine/scripts/wayland_pacing_protocol.py` companion. It wraps the existing launch command, including `mako-run`, without rebuilding or installing MAKO; selected numeric/object-only protocol records are kept in a separate private 64 MiB capture that preserves its beginning. Keyboard events and other unselected protocol traffic are excluded, ordinary game stderr passes through, and `analyze-pacing-profile.py --wayland-protocol PATH` correlates exact feedback outcomes with actual buffer commits. Retain both raw files; commit timestamps describe client-side marshalling, and protocol logging can affect timing.

Supporting Renderer builds also offer `MAKO_PACING_PROFILE_CONTENT=1` for an explicit-trigger Fixed 2x SDR image capture without spatial scaling. The linked guide describes the warm-scene trigger, bounded 128 real/generated picture pairs, source/history/image/queue identities and `analyze-frame-content.py`. The separate private binary contains actual game pictures and is not included automatically in timing-only diagnostics. Capturing adds GPU transfers, CPU readback and file I/O at the existing completed-copy boundary; it adds no GPU wait or presentation deadline change.

## 4. Restore your settings

Restore the saved Launch Options and any changed Target, or remove the two diagnostics environment variables from Heroic/Lutris.

## 5. Send the report

Review `MAKO-diagnostics.txt` and remove personal information. Upload it through the [diagnostic form](https://docs.google.com/forms/d/e/1FAIpQLScSd9qgkYCq3Kbbc3_52k4_82iTmEqt3_FxOqGuxQ6FsjutgA/viewform), choosing **MAKO Decky (Decky Loader plugin)**. Answer **Unknown** when unsure. Do not post the log publicly or attach `Lossless.dll`.

## GPU Headroom evidence

Reports retain Headroom cap changes and any Adaptive efficiency trial cancelled because the cap made its output target unreachable. A cancellation restores the proven workload and is not a presentation failure.

Compare the same scene with GPU Headroom Off and On. Collect a report after each run and note the reserve, Fixed/Adaptive mode, multiplier ceiling and any game FPS cap. Brief utilization spikes can still reach 100%; the readings cover the whole GPU and do not measure per-game GPU time or power. See [presentation records](../../engine/docs/WSI-ISOLATION.md#interpreting-presentation-records) for the retained fields and controller states.

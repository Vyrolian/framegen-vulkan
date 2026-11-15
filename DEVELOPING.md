# Developing framegen-vulkan

framegen-vulkan separates deterministic product checks from real-hardware evidence. Portable tests are required for every change, but they do not prove Vulkan presentation, image quality, or game compatibility.

Use `scripts/dev.sh help` for the entry points. This guide covers build and test selection, the dependency and version owners, and the release procedure.

## Build and test

framegen-vulkan owns the product code, portable tests, production-path CLI tools, and package contracts.

Portable framegen-vulkan tests must not require `Lossless.dll`, an AMD GPU, Gamescope, or a compositor.

[Dependencies and version owners](#dependencies-and-version-owners) map upstream dependencies to their owners, update triggers and validation obligations. Record exact runtime/build identities with hardware evidence and keep them separate from build pins and source reviews.

### Validation cycles

| Cycle | Proves | Does not prove |
| --- | --- | --- |
| Portable change gates | Deterministic Renderer, package-policy, sanitizer, and producer behavior | Real Vulkan presentation, AMD image quality, or installation |
| Direct device iteration | A focused change works on the current development installation | Archive layout, clean-checkout reproducibility, 32-bit, or Flatpak behavior unless selected |
| Complete local archives | Local host, Flatpak, and Arch packages from one checkout | A clean pushed commit, real game behavior, or public asset |
| Local release-candidate check | The already-built complete host and Flatpak archives match the clean pushed commit and contain verified payloads | GPU behavior, installation, or the published download |
| Manual release-candidate matrix | Selected games survive relevant presentation, focus, overlay, hitch, and recreation scenarios | Untested games and hardware |
| Published-package check | The exact GitHub host, Flatpak, and Arch assets match their recorded checksums and install through their user-facing paths | Universal compatibility |

Use [Release](#release) for release validation and publication.

### Portable gates

The `Tests` workflow runs on every pull request and push to `main`:

- the `fgvk` settings window crate (`engine/fgvk-ui`): `cargo test --locked` and `cargo build --locked`, including the argument routing that hands `fgvk <command>` to the launch helper without opening a window;
- framegen-vulkan CTest with GCC and Clang using the [native package Vulkan-Headers revision](engine/vulkan-headers-revision.txt) and its required-header check, including synthetic model inspection, launch policy, generated-SPIR-V freshness, and shared Flatpak-header module freshness/mutation checks;
- the Arch package release-pin, alternative-package identity, non-mutating lifecycle-hook, automated release-sync, and exact-release build-wrapper contracts;
- portable Renderer policy tests under ASan and UBSan using the same pinned Vulkan headers as the native package;
- protected-input and local release-candidate contracts on their supported hosts.

The owning component tests remain authoritative for their detailed invariants.

`steamos-build-tools` exercises the prerequisite installer with temporary host files and stubbed commands. It covers the nonzero `disabled` status from SteamOS, preservation/restoration of the initial read-only state, transaction/restoration failures, and rejection of invalid status results before mutation. It does not install system packages or change the real root filesystem.

The `fgvk` settings-window crate tests (`scripts/dev.sh test-fgvk-ui`) cover profile and games-list parsing, the process picker, and argument routing. Routing tests check that no argument opens the window, that every argument reaches the launch helper unchanged, and that a missing helper exits with status 127. They do not open the window or start a game.

When testing the optional forced-FP16 native build, configure `BUILD_TESTING=ON` and `FGVK_EXPERIMENTAL_LSFG_FP16=ON` with the pinned sources described in [Building from source](engine/docs/BUILDING-FROM-SOURCE.md#experimental-forced-lsfg-fp16). Its additional `backend-experimental-lsfg-fp16` test uses original synthetic SPIR-V to check half arithmetic, preserved bindings, FP32 sampling and bit-pattern calculations through memory stores/loads, byte-identical FP32 image preparation/reconstruction, native FP16 priority, FP16-off isolation, selected modes, cache replacement, and validation failures. Run ordinary CTest as well to retain strict-build coverage. GPU quality and synchronization evidence must distinguish forced conversion from native FP16, preserve failing image-quality results, and state which actual DLL branches were available. Add warmed throughput evidence when precision protection changes the amount of half arithmetic; stage counts alone do not establish performance.

Default and Vulkan-layer Renderer builds and portable policy CTest check that every production `fgvk-render/src/**/*.cpp` file is registered with its CMake target. This catches a newly split file even when an existing build tree does not reconfigure. The complete local release-shaped package build then verifies the host archive, Flatpak bundles, Arch package, direct ELF dependencies, and checksums; a successful direct 64-bit build alone does not establish those package contracts.

Launcher exclusion freshness runs in Renderer CTest; `scripts/dev.sh check-launcher-exclusions` runs it directly. The shared registry is `engine/fgvk-common/launcher_exclusions.json`; regenerate both component bindings with `scripts/dev.sh generate-launcher-exclusions`. Portable tests validate registry entries, reject stale or missing bindings without rewriting them, and cover exclusion and child activation for the registered executables. New exclusions also need focused real-launcher evidence; moving the unchanged list does not establish new compatibility evidence.

Flatpak catalogue freshness runs in Renderer CTest and Renderer packaging; `scripts/dev.sh check-flatpak-runtimes` checks it directly. `engine/dist/flatpak/fgvk-render/runtime-versions.json` and the adjacent common manifest template generate every Renderer manifest and the shell matrix. Portable regression tests exercise adding and retiring a branch, missing/stale/foreign manifests, read-only failures, invalid catalogues, direct and inherited runtime detection, extension refresh, and 26.08 install/update/removal actions. Package checks require a bundle for every generated branch; actual package builds must qualify both architectures.

`scripts/dev.sh install-hooks` enables a commit hook that runs the protected-input check (rejects `Lossless.dll` and binary payloads) before every commit.

Run `scripts/dev.sh test` for protected-input and local release-candidate contracts, the `fgvk` settings-window crate tests, and Renderer CTest. Add the checks below for the complete local portable gate:

```bash
scripts/dev.sh test
scripts/dev.sh test-engine-sanitized
```

The focused Renderer policy script (`engine/scripts/test-adaptive-scheduler.sh`) compiles Vulkan-facing policies, so it needs Vulkan, X11, and XCB headers even though it does not need a Vulkan device, compositor, or Wayland development package. `scripts/dev.sh test-engine-portable` runs it, and `scripts/dev.sh test-engine-sanitized` runs it under ASan and UBSan. The full Renderer suite additionally needs the Vulkan loader:

```bash
cmake -S engine -B engine/build/local -DBUILD_TESTING=ON
cmake --build engine/build/local
ctest --test-dir engine/build/local --output-on-failure
```

Shader changes must regenerate their adjacent embedded payloads with the owning generator; never edit generated SPIR-V arrays or hashes directly.

Display-refresh matching is covered by `profile-update` and native round trips. Cases retain disabled defaults, 10–1000 FPS bounds, missing-feedback fallback, saved manual targets/caps, Fixed isolation, target-dependent caps, private-resource growth, superseding live edits, and older status records. Real display hotplug requires game evidence.

`x11-refresh-policy` checks the X5 refresh rules without X or Wayland I/O: a RandR mode's rate from dot clock and total pixels, with interlaced and double-scanned flags; the active CRTC with the largest window overlap, ties to the first CRTC, the primary-CRTC fallback that is looked up only when nothing overlaps, and off-screen or zero-size windows; the exact output-name match and the mHz-to-Hz conversion. `uncapped-auto-cap` checks the shared 20–1000 Hz range that every refresh source passes. The xcb and Wayland round trips have no test seam; they need a live X11 or Xwayland session to qualify, and a skipped live check is not evidence.

`profile-update` checks capped VSync-off output spacing without refresh feedback, and preservation of timing ownership and source-cap requirements. Existing `presentation-policy` clock tests cover output intervals, late-batch rejection and phase continuity; native submission-spacing evidence and remaining hardware rows are in the WSI guide.

The ready-output fallback tests cover game-only and combined 60 FPS caps with complete 2x–5x batches, bounded preparation/output spans, relative-limiter feedback, a 100 ms source hitch, slow preparation, slower/faster source changes and policy resets. Eligibility tests preserve actual caller/compositor targets, caller IDs and completion-wait forwarding and generation Off. Native FIFO coverage models the 43 FPS game cap with and without a nonbinding 60 FPS framegen-vulkan cap, requires 11.628 ms ready-output spacing, and releases that clock on timing-owner takeover; Native FIFO admits one complete Fixed batch to the bounded worker. Native Fixed cap preparation must work without refresh feedback, and worker readiness must leave the producer free to run. Readiness success must shift every output together; timeout or device loss must still return every acquired image and preserve errors. Native qualification must distinguish backend timeline readiness, lower-present submission intervals and compositor/physical completion, and compare cap-only, game-only, combined-cap and stall rows with FIFO controls. A synchronous fallback can slow a relative game limiter; deterministic tests establish bounded waits, not universal limiter compatibility.

`present-timing` checks first-pixel target-stage selection and paired calibration of distinct stage-local epochs, including wall-clock queue completion with monotonic output targets and rejection of invalid target-clock calibration. Queue-only admission and existing bounded feedback, clock-loss and property-recovery cases remain covered. The bounded local reproduction, independent Vulkan clock probe, protocol evidence and remaining hardware rows are recorded in [WSI isolation](engine/docs/WSI-ISOLATION.md); portable tests alone do not establish driver progress or visible cadence.

`pnext-chain` additionally verifies that generated outputs inherit only an immutable, detached present-mode override, while ID/ID2, fences, timing and damage remain on the caller real image. Native FIFO eligibility tests distinguish enabled timing/wait capabilities and ID-only presents from actual caller timing targets. The header-only pNext test belongs to the portable sanitizer lane and must also pass optimized builds; shared Vulkan ABI prefixes are inspected as bytes with an untyped next pointer.

`pacing-profile` checks timing-path counters and requested-target intervals, including repeated/backwards targets, owner changes, untimed gaps, context isolation and window resets. `pacing-profile-report` distinguishes older unavailable owner evidence from actual counters and weights target intervals by their sample count. These observations identify forwarded requests without changing pacing or querying presentation feedback; FIFO hardware qualification still needs effective per-output timing and game evidence.

Synthetic DLL inspection shares the production C ABI, allowing sanitizer tests to check function-pointer types and descriptor layouts. `backend-dll-inspection` also checks the parser's aggregate payload allocation budget for PE32 and PE32+, including exact-budget shared resources, one-byte overflow, small files with many aliased payloads, and zero-length payloads. These fixtures contain no licensed inputs.

Optional Lossless Scaling tests use `watched-configuration` to cover configured-path precedence, missing/replaced files, directories, stale replies, and unknown failures while retaining model-free controls. No licensed model is loaded.

`cli-precision-contract` checks `inspect-dll` precision selection and option conflicts. The shared query owner remains used by the pacing GPU profile. Changes to stage queries need hardware evidence from `FGVK_PACING_PROFILE_GPU=1` runs for single/multiple-output and FP16/FP32 cases; retain ordinary capacity evidence separately because query readback changes iteration spacing. These checks do not qualify generated-frame display pacing or game image quality.

`command-buffer-submit-storage` checks ordered multi-command submissions, binary/timeline semaphore composition, inline and overflow storage, queue selection, completion-fence placement and propagated driver failures without retries. It also runs in the portable sanitizer lane. Backend submission changes additionally need synchronization validation, temporal sequences with history-only steps and changing output counts, independent output repeatability and paired throughput evidence. Portable marshalling tests alone do not establish GPU execution order or displayed pacing.

Long-session timing coverage in `presentation-policy` advances the production source pacer and ordered timeline through two simulated hours at 45, 60, 90, and 120 FPS with a large clock origin, checks bounded deadlines and rounding, and confirms late-frame rebasing. The diagnostics collector tests retain the new boundaries in the applicable focused presets. These tests do not reproduce an hour-long game's asynchronous GPU, driver, compositor, resource growth, or thermal behavior; qualify those on hardware.

`pacing-profile` checks correlated worst-source waits/phases/publication cost, nested context isolation, independent activation, private capture permissions, refusal of existing files/symlinks/FIFOs, bounded rollover and retained acquisition records. `pacing-profile-report` checks process/context separation, malformed/legacy records and exclusion of unrelated launcher content. Collector tests retain profile/session/wrap/gap records and select private captures without following symlinks. `FGVK_PACING_PROFILE=1` adds CPU timing and file publication but no GPU query. Separate `FGVK_PACING_PROFILE_GPU=1` reuses backend queries and the existing previous-work fence boundary. `backend-hot-path` covers disabled/startup/history isolation, consume-once delivery, retry deduplication, bounded overwrite and permanent read-failure disablement; `pacing-profile` checks separate opt-in activation, GPU metric units/counts, invalid sample rejection and empty-window reset, while `pacing-profile-report` checks sample-weighted GPU means, CPU readback cost and unavailable-versus-zero evidence. The shared timestamp rollover/property tests remain authoritative. Hardware qualification additionally needs matched GPU-off/on game captures, backend timestamp validity, synchronization/recovery and repeated resource replacement, including FP32/FP16 on AMD. Minecraft cadence and complete profiler overhead still require paired game captures; portable summaries do not establish displayed cadence or input latency.

The `presentation-policy` jitter regression alternates ±2 ms arrivals around 60 FPS for 600 frames and requires the learned 2x spacing to remain 8.333 ms. The cap-increase rows reproduce a 20 FPS game limiter becoming a 40 FPS source, both uncapped in framegen-vulkan and below an existing 40 FPS framegen-vulkan cap. They require release of stale fallback waits after two substantially faster arrivals and requalification at the new cadence. Feedback-driven workload rows include a minute at 20 FPS, recovery to 180 FPS, and repeated 70/80 FPS changes across 2x–5x, with no cap or a 90/180 FPS framegen-vulkan cap. They feed actual scheduled output sleeps into absolute and relative source arrivals, require stale pacing to release within the first four recovery frames, and check the new spacing after twelve frames and sustained smooth batches after settling. These are simulated source-work rates, not measured game output FPS; a relative limiter still pays synchronous present time. The game-owned-cap regression holds a 60 FPS source below a 70 FPS framegen-vulkan cap across 2x–5x, includes a partial batch and isolated fast/compensating arrival pair, and requires fallback spacing to resume rather than becoming permanently unpaced. A second row establishes cap ownership at cold startup, then requires its release after learning the slower game period. A constant 60 FPS absolute-source row changes GPU-readiness waits every forty frames across 2x–5x, with complementary outside-game waits, and forbids requalification solely from changed wait-excluded time. Existing relative-limiter, slower-source, hitch, preparation-budget, source-cap and long-session rows remain required. Qualify real cap changes and isolated hitches in the same Minecraft scene; a synthetic timeline cannot prove a Java limiter, operating-system wakeup, GPU or compositor response.

Individual-output profiling additionally needs `pacing-profile`, `pacing-profile-report` and `deferred-present` tests. They cover real/generated ordering and source ordinals, timestamp/status preservation, fixed trace capacity with explicit omissions, cross-window ordinal continuity, actual wait1/wait2 capture with broad diagnostics disabled and unchanged forwarding, process/swapchain isolation, malformed or missing evidence, and nonblocking copy observations that cannot change delivery even when the observation returns an error. Native synchronization qualification must compare CPU-only and GPU-enabled profiling against the same unmodified baseline and retain every validation failure. A copy-fence status is the batch's final-copy observation, not a pixel, compositor or physical-smoothness measurement. Measure the new buffered publication cost separately, then qualify full observer cost in matched game captures.

The same profiler/report checks preserve the outer application's pre-configuration deferred-output join as its own CPU phase. It must remain inside outer present duration, outside configuration-update time, and unavailable in older captures that omitted it. Native controls must show actual nonzero join samples without changing output ownership, synchronization, deadlines or pacing policy. Do not infer physical smoothness from either the inner or outer CPU timer.

`wayland-presentation-feedback` exercises the opt-in native Wayland observer through a runtime libwayland ABI fixture. It checks private-queue inheritance through a display wrapper, protocol/version arguments, presented/discarded/abandoned callbacks, missing protocol/clock evidence, bounded outstanding proxies and record overflow, unchanged exactly-once lower forwarding/results, stop-before-borrowed-surface-destruction and cleanup. The fixture deliberately provides no socket-reader, blocking-dispatch, roundtrip, flush, surface-commit or display-disconnect functions. Reader tests preserve actual WSI provenance, separate clocks/surfaces/outputs, exclude unrelated feedback, and flag multiple requests sharing a timestamp/sequence. Collector tests retain observer records in every preset. These checks establish capture mechanics only; actual Wine Wayland feedback, protocol overhead, commit association and the Cyberpunk visible fault require a new game capture. Native XCB probes do not qualify that boundary.

The same `pacing-profile-report` Python test entry point checks the optional `wayland_pacing_protocol.py` companion against actual child stderr: selected-message privacy, argument/environment/exit preservation, exclusive mode-0600 files, beginning-preserving bounded retention, old/new Wayland log timestamps, exact presentation-to-commit joins, multiple observers sharing one commit, conflicting/missing associations and realtime clock changes. These checks validate collection and attribution mechanics only. Qualify game stderr availability, actual driver commit association and logging overhead on the user's Wayland run before drawing a causal conclusion.

The report's repeated `--presentation-window START:END` comparisons additionally reproduce stable lower calls with uneven compositor feedback. Tests require exact successful output/call joins, isolate clocks/surfaces/outputs/contexts, retain a common surface origin across recreation, reject duplicate/missing/backwards/discontinuous associations, and keep missing GPU evidence unavailable. Nominal CPU deadline drift against refresh multiples must not become a claim of measured vblank phase or causality.

Passing report tests cannot prove frame-generation correctness.

`color-pipeline` verifies SDR format precision, HDR10/PQ and linear scRGB classification, and passthrough for unvalidated colour spaces and formats. Real hardware is required to qualify display output, cadence and HDR colour.

`external-fd-ownership` checks transfer-only images without views and preserves export/cleanup and views for other usages. Its recorded-copy fixture checks that external uploads discard old contents and release after the copy, readback acquires before the copy, and internal images retain local ownership. It does not simulate a driver or prove cross-instance visibility; shared-image changes also need synchronization, temporal reuse and output-repeatability evidence. `descriptor-pool-capacity` covers sparse/empty layouts, exact capacity, destruction and creation failure. Both run in the portable sanitizer lane.

`backend-black-image` records the production zero-prior initialization against a dirty-image fixture and requires a complete zero clear between the transfer and compute dependencies. It belongs to full Renderer CTest; backend-only tests can also be built with `FGVK_ENABLE_SANITIZERS=ON`. Real GPU quality and repeatability checks remain necessary for model input changes.

`backend-output-split` verifies that the optional native FP16 convolution split preserves all calculations, partitions all four image writes exactly once and removes unused output-handle loads, with rejection coverage for malformed inputs, output reads, aliasing, mutable memory, subgroup-dependent inputs and unfamiliar graphs. `shader-pipeline-ownership` uses fake Vulkan entry points to cover shared layouts, one outer dependency, unchanged dispatch extents, module retirement, moves and cleanup after either pipeline creation fails; it runs in the portable sanitizer lane. The backend parser test also supports a focused ASan/UBSan build. Hardware qualification needs the complete quality and synchronization suites, independent temporal output comparisons, and paired full-Flow 1440p/4K performance evidence.

The pacing-log retention regression fills a Wayland capture past 64 MiB and requires its session header and both FG comparison markers to survive, an explicit capture-limit marker, no rollover, no writes after the limit and a bounded file. CPU/GPU-only capture retains its existing 16 MiB rollover coverage. Compact version-2 reader checks join event identity by process/surface/feedback without inventing missing requests, preserve version-1 input, and expose FG-setting transitions, rollover loss and capture limits. These checks validate evidence retention rather than the cause of visible stutter.

The detailed game trace additionally requires `pacing-profile`, `pacing-profile-report`, `deferred-present` and `backend-hot-path` coverage. Preserve source-local settings/waits/phases, bounded delayed-GPU source mapping, resource-generation isolation, nested application scopes, successful versus unavailable acquisition images, individual wait calls and short-session records. The reader must reject malformed values, duplicate/missing source identities and wrong GPU generations, and must not bridge missing source intervals or subtract GPU durations from host timestamps. Full-trace activation implies CPU/GPU/Wayland observers while leaving broad diagnostics independent. Verify mapped ELF build identity, private exclusive JSON output and child-only companion activation. Qualify the actual producer through a bounded D3D12/VKD3D run and report observer overhead separately from gameplay correctness. Full trace uses per-call publication; the lightweight CPU mode retains buffered output observations. Driver-acquire tracing must preserve every lower invocation, remaining timeout/result, unsuccessful-image unavailability and nested scope identity. Native-copy tracing must preserve exactly-once submission/exception behavior and bounded semaphore retention. Check actual present return codes and lower output queue/wait identities. Reader regressions must consume acquired-image ownership once within its context lifetime, refuse duplicate/malformed/gapped matches, preserve failed/unmatched observations and mark incomplete driver partitions unavailable. The CPU boundaries add no Vulkan operations and do not establish GPU completion or simulation timing. The full-trace native GPU observer additionally requires `native-copy-profile`: one bounded timestamp pool, no blocking query read, reuse only after existing completion proof, rejection of overlapping/stale recording, partial availability, allocation/query errors and source/resource-generation isolation. The report must distinguish deliberately ready versus pending copy controls, preserve calibration errors/disagreement and unmapped clocks, reject duplicate/mismatched samples and expose missing shutdown tails. Qualify the real producer with synchronization validation and inspect actual raw timestamp/clock joins; a successful process exit alone is insufficient. Timestamp recording, calibration/readback cost, ordinary source throughput and paired game overhead remain separate evidence.

Full-trace Vulkan setup observation additionally needs the `pacing-profile` and `pacing-profile-report` regressions. Preserve paired incoming/forwarded creation requests, lower results, failed retries, layer/PID/device identity, selected feature values, bounded extension/chain/queue lists and explicit truncation. The reader must keep old/missing/duplicate setup unavailable and exclude failed creates from successful device/swapchain selection. Runtime companion checks cover child-only logging, exclusive private output, retention bounds and filtered Wayland traffic. Verify the real producer with a bounded VKD3D run and synchronization validation; equal extensions or a smooth HUD do not prove reproduction of a game's visible delivery fault.

### Native Fixed output handoff validation

Released-source tests additionally model queued bursts whose median differs from throughput, slower/faster workload transitions, long pauses and return to ordinary source qualification. Worker regressions cover overlap acquisition returning the first available image while later batches remain pending, zero-time queue-lock contention, finite/infinite budgets and lower acquire/present errors. Worker-copy pressure tests require readiness/expedite/output-limit skips to consume staged work and signal the continuation/fence exactly once without acquiring a generated image. A join after generated delivery must preserve its trailing real slot. Run these cases in full Renderer CTest, repeated `deferred-present` runs and the portable sanitizer lane; hardware checks must distinguish real-frame throughput, generated skips and physical cadence.

The `deferred-present` portable test covers native Fixed's bounded output-batch worker: value-owned singleton inputs, caller IDs/ID2, modes and fences, rejection of unsupported or duplicate nodes and borrowed result arrays, one presentation queue across complete generated/real/next-batch sequences and synchronous unsupported-chain/native fallbacks, one-source-batch admission and sealing, capacity/error/empty drains, per-output completion timestamps, no catch-up after delayed calls, poll/finite/infinite completion, actual recreation errors, repeated reuse, shared private-queue serialization, spare-image polling before the output deadline, nonblocking lock contention and joining at teardown. Private queue reservation preserves application indices and priority arrays, rejects protected-only requests and counts protected/ordinary requests together against physical capacity. Run it in full CTest and the sanitizer portable-policy lane. The presentation-policy detached-output regression also covers early/late source jitter runs of two, four, six and sixteen intervals with short acquisition joins without qualification holes, bidirectional median updates across workload changes and recovery with actual detached-slot acquisition feedback, including an 8 ms readiness cost that conceals a smaller sustained improvement. Additional presentation-policy regressions feed readiness timeouts back into detached cadence, retain full output spacing and unchanged source-cap deadlines after late native Fixed preparation, require actual detached evidence before a short cap-miss fallback, and release detached cap ownership when a slower game limiter takes over. The loaded-readiness regression feeds 2x–5x source arrivals through GPU readiness longer than one output slot, overlaps the bounded trailing output slots with next-frame work without accumulating a whole source interval of debt. Hardware evidence additionally needs pending application GPU work at present entry, actual source CPU work overlapped with output pacing, acquisition timing, idle/recreation, and validation-layer synchronization; a source limiter alone does not establish throughput recovery.

The `deferred-present` completion-wait regression calls both ID namespaces from another thread while the real output is retained. It requires zero-time and exhausted finite waits to avoid forwarding an unsubmitted ID, subtracts handoff time from finite driver waits, preserves infinite waits and driver results, propagates a rejected lower present, checks registry removal at teardown, and distinguishes a producer gated by completion from an acquisition overlapped with the wait. The source-pacing regression feeds 240 such gated intervals without inflating the previously qualified output period. The `completion-spiral` portable test covers the log-only spiral rule: three consecutive one-second windows of growing source period with a growing, covering mean completion wait per source trip it once, including while the frame rate falls, two-frame hitches separated by normal windows, growth without a wait that grows and covers half the growth, and an empty window do not, a flat window clears an active episode, and a source gap over 250 ms clears it and restarts the windows. Native wait2 validation must enable the ID2 and wait2 swapchain flags and exercise actual asynchronous waits with one and multiple outstanding source frames. Enabled features without wait calls do not cover this boundary; a good source FPS graph does not excuse an unassociated lower ID.

The detached ready-batch phase regressions additionally check every generated/real/batch boundary under alternating GPU readiness, late worker delivery and an inactive configured cap. Producer feedback covers relative and absolute limits, faster and slower work, all Fixed multipliers and acquisition joins; first-output deadlines run on the existing worker. Worker tests admit exactly one complete five-image batch and reject a sixth image or a second source batch. Hardware checks must still establish physical cadence and game motion independently of flat source graphs.

### Presentation qualification

Fixed Frame Generation needs two kinds of evidence: deterministic policy tests for presentation decisions, and real Vulkan tests for drivers, presentation, compositor behavior, GPU work, and game behavior. Passing one does not substitute for the other.

Presentation changes must follow the rules in this section. A successful `vulkaninfo` or finite `vkcube` run proves only its narrow loader or presentation boundary.

The external-recovery inventory pairs unidentified-overlay controls across Fixed 2x and 3x and Frame Generation-only operation. Pause, covered-throttle, and repeated short/long journeys must independently prove source recovery, target-output recovery, focus sequencing, fresh history, generation suspension where applicable, and context stability. Portable policy tests separately cover ready-output spacing, 2x–5x multipliers, exact Fixed stability, transition-scoped acquire failure, inert successful transport, and the absence of FPS-authorized recovery so hardware rows do not carry the full combinatorial burden.

For affected changes, cover these runtime boundaries:

| Boundary | Minimum evidence |
| --- | --- |
| Generation modes | Live `0x`, Fixed 2x and affected higher multipliers. |
| Cadence changes | Startup, gameplay rate changes, true fixed-rate rejection, short hitches, long interruptions, and fast-present bursts. |
| Presentation | Ordered SDR, explicit acquire failure, configured cumulative acquire-deadline overrun, slow lower-present non-authority, focus and overlays, resize, recreation, and shutdown. |
| Resource transitions | Live Off/On, Flow Scale or model replacement, capacity growth with and without WSI headroom, and history warm-up. |
| API/runtime | Affected native Vulkan, DXVK, VKD3D-Proton, direct desktop, Flatpak, architecture, and Proton-family paths. |
| Quality/performance | Real FPS, displayed FPS, frame-time percentiles, generated share, visible artifacts, latency impression, GPU time, and memory where relevant. |

Record the commit and build identity, hardware, driver, compositor, runtime, game and resolution, policy settings, transitions, diagnostic path, and verdict. Mark unavailable hardware or paths **not tested**.

Presentation diagnostics are opt-in and can perturb pacing. Enable `FGVK_PACING_PROFILE=1` (see the [configuration reference](engine/docs/CONFIGURATION-REFERENCE.md)) for a separate diagnostic run, then disable it for subjective comparison. Their timestamps describe framegen-vulkan policy and queueing, not compositor scanout.

#### Release gates

An affected candidate is ready for broader testing when:

1. portable and sanitizer policy tests pass;
2. affected Fixed multipliers stay within their configured counts;
3. no case enters an unexpected indefinite wait, recreation loop, or permanent unintended native-only state;
4. Off/On and recreation restart from valid temporal and recovery state;
5. failures have context-correlated diagnostics; and
6. unavailable rows and known title-specific failures are recorded.

## Dependencies and version owners

This section identifies framegen-vulkan's dependencies, external interfaces, version owners, and ongoing maintenance responsibilities. Use it to review upstream changes, determine whether framegen-vulkan needs an update, fix affected code or pins, and run the relevant checks.

Keep this section focused on what to check, when to check it, and who owns the change. Exact versions belong in the linked version files; implementation details belong in component guides; review findings and test results belong in the owning compatibility records or QA evidence. Update this section when a dependency or maintenance responsibility changes.

[Build and test](#build-and-test) owns validation selection, [CLEANUPS.md](CLEANUPS.md) owns temporary compatibility removal, and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) owns license and source attribution.

### Dependencies and external interfaces

| Boundary and owner | What can change / when to review | Required maintenance and validation |
| --- | --- | --- |
| SteamOS platform: Renderer maintainers; [official release notes](https://www.steamdeck.com/en/news) | Stable releases and relevant Beta/Preview changes to bundled Gamescope, Mesa, kernel, system libraries/tools, display behavior, or installation. Review announcements even before a test machine upgrades. | Check Valve's release notes and package metadata against the affected entries below, including command aliases and installation tools. Fix any incompatible assumptions and run the relevant checks in [Build and test](#build-and-test). Check Steam client updates independently. framegen-vulkan does not pin the user's OS. |
| Native Wayland presentation observation: [`wayland_presentation_feedback.cpp`](engine/fgvk-render/src/wayland_presentation_feedback.cpp), shared [`wayland_client_abi.hpp`](engine/fgvk-render/src/wayland_client_abi.hpp), optional [`wayland_pacing_protocol.py`](engine/scripts/wayland_pacing_protocol.py) | Stable `wp_presentation` version-1 wire contract, libwayland-client runtime ABI, optional `WAYLAND_DEBUG` text layouts, driver commit threading and the application's display-reader behavior; compositor, Wine or driver changes. | Keep runtime resolution optional, native surface provenance explicit, borrowed display/surface lifetimes protected, private queue inheritance established before proxy construction, and outstanding proxies/records bounded. Preserve exactly-once lower forwarding without socket reads, blocking dispatch, commits, targets, IDs or new reader threads. Check clock/flag interpretation, missing/discarded/abandoned evidence and ambiguous commit association; qualify actual Wine Wayland feedback and paired observer overhead in games. Keep the separate protocol companion private/bounded, presentation-only and fail-closed on conflicting commit identities or clock steps; retain stderr/exit/signal forwarding and qualify its logging overhead. XCB evidence cannot establish this boundary. |
| Release-point source cap: [`release_gate.hpp`](engine/fgvk-render/src/swapchain/release_gate.hpp), [WSI isolation](engine/docs/WSI-ISOLATION.md#release-point-source-cap) | VKD3D-Proton presenting on its D3D12 queue-submission thread, then submitting one signal-only `vkQueueSubmit2` of its completion timeline that a separate thread waits for with `vkWaitSemaphores` before releasing frame-latency events (`dxgi_vk_swap_chain_present_signal_blit_semaphore` and `dxgi_vk_swap_chain_wait_worker`); VKD3D-Proton update. | Recheck that submit shape, its order after `vkQueuePresentKHR`, and the wait thread on each reviewed VKD3D-Proton release. A changed shape must leave the present hold in place, never delay an unrelated wait. Retain burst-throughput, overlap-acquisition, worker-copy skip and post-generated join regressions in portable/sanitizer coverage; qualify real-frame throughput and generated delivery separately under queued GPU work and Reflex changes. |
| FIFO pacing and application completion: [`presentation_policy.hpp`](engine/fgvk-render/src/presentation_policy.hpp), [Fixed pacing](engine/docs/WSI-ISOLATION.md#fixed-pacing-paths), [output timing](engine/docs/WSI-ISOLATION.md#output-timing-within-an-existing-base-cap) | Compositor queue/limiter behavior, Mesa/driver presentation, Proton/DXVK/VKD3D-Proton waits, refresh changes. | Preserve one pacing owner, real-frame completion semantics, bounded acquisition/recovery. Validate native FIFO ready-output fallback separately from Vulkan target timing: retain actual caller targets, real-image IDs/fences and unchanged completion-wait forwarding, bounded native FIFO 2x–5x/VSync-off 2x worker admission, FIFO cap preparation and caller real-image timing, no caller-target rewrites or generated-image metadata inheritance, worker-owned backend readiness within the source deadline and timeout image returns, one-source-batch sealing/draining including complete ready batches, bounded cross-batch phase and actual worker-call feedback without producer deadline waits, one native presentation queue across synchronous/retained/policy-off outputs and copied caller metadata; join a retained real-ID handoff before forwarding either completion-wait API on any thread, preserve the total timeout and lower result, remove device/swapchain delivery records at teardown, and validate actual asynchronous wait2 calls; detect actual producing-thread latest-ID waits and distinguish enabled capabilities from actual target nodes; compare actual game-cap source/output rates and fixed-refresh quantization. Keep a proven native Fixed source cap authoritative across brief late arrivals; require sustained cap-paced batches before replacing a learned game cadence, release ownership against the actual configured-cap period, and test the source/render/readiness/output feedback loop after loading or game-limiter bursts. Test source and output cadence, latency, below-refresh targets, Fixed, serialized/concurrent waits, and physical VRR. Standard FIFO support alone cannot qualify these interactions. |
| LSFG licensed resources: `engine/fgvk-backend/src/extraction/` | User-installed Lossless Scaling update, resource layouts, model interfaces, precision lanes, translated code. | Preserve the DLL reader's aggregate payload budget and synthetic alias/boundary tests under `backend-dll-inspection`, including sanitizer coverage. Run the canonical `inspect-dll` checks, synthetic contracts, and licensed-model quality/performance/repeatability in fgvk Gym. Record tested product build and private input identity with evidence. Capability checks are not a guarantee for future DLLs. Never distribute the DLL or extracted resources. |
| DXBC translation and optional FP16 conversion: [`dxbc_translation.cpp`](engine/fgvk-backend/src/extraction/dxbc_translation.cpp), [`prepare-lsfg-fp16-tools.sh`](engine/scripts/prepare-lsfg-fp16-tools.sh) | Runtime `libvkd3d-shader.so.1` ABI/output, architecture availability; pinned SPIRV-Tools/SPIRV-Headers in experimental builds. | Preserve the shared translator ABI and synthetic boundary tests. Identify the actual translator in evidence; test native/translated models and relevant precision modes. Experimental conversion remains unqualified until its quality rows pass. |
| Vulkan loader, driver, WSI, and extensions: `engine/fgvk-common/`, `engine/fgvk-render/src/entrypoint.cpp`, [WSI isolation](engine/docs/WSI-ISOLATION.md), [lifecycle](engine/docs/LIFECYCLE.md) | Mesa/vendor drivers, loader discovery, feature and extension combinations, new pNext nodes, present timing/ID/wait/maintenance APIs. | Keep build-header requirements distinct from enabled device capabilities and manifest API declarations. Preserve caller chains, synchronization, ownership, and bounded retirement. Recheck native Fixed private-queue capacity, copied worker inputs, delayed-result propagation, detached-output cadence evidence, synchronized spare-image polling versus blocking acquisition, recovery with millisecond readiness costs and longer jitter runs without unpaced output holes, readiness misses that preserve detached cadence, late cap-output phase with unchanged source deadlines, cap-to-game limiter transitions, total acquire timeouts, idle draining and teardown whenever device/queue or presentation boundaries change. Keep direct MAILBOX/IMMEDIATE admission separate from the private ordered transport and qualify CPU output spacing without assuming FIFO backpressure or known refresh. Calibrate stage-local target and queue-feedback clocks independently; prefer supported first-pixel clocks for targets and retain queue completion as the readiness observation. Record driver/protocol clock evidence in the WSI guide. Run portable/full CTest plus targeted feature, sync, recovery, and desktop/Proton matrices; build success does not prove driver support. |
| Host display and system libraries: native package scripts | libwayland-client, X11/XCB/X11-xcb, libdrm, libc/libstdc++, shell/Python utilities and sandbox visibility; distro/runtime updates. | Preserve dynamically resolved ABI declarations and missing-library handling. Capture actual package/library identities in build or runtime evidence; verify native and multilib packages and the oldest supported environment. Host library names are ABI contracts, not exact release pins. |
| Display feedback: [`color_pipeline.cpp`](engine/fgvk-render/src/color_pipeline.cpp) | Format classification and driver colour handling. | Verify unsupported colour-space passthrough, high-precision SDR generation, and HDR10/PQ and linear scRGB generation. HDR colour accuracy needs display evidence. |
| Optional layers: [layer chaining](engine/docs/LAYER-CHAINING.md), host MangoHud | Host MangoHud. | Verify immutable payload provenance, both x86 architectures, private manifest admission/order, renderer extension contracts, colour and performance. Preserve unsupported-path guards. |
| Steam Overlay and Steam Input: [`fgvk-launch`](engine/scripts/fgvk-launch), [WSI isolation](engine/docs/WSI-ISOLATION.md) | Overlay layer names/preloads, runtime reinjection, Proton input behavior; Steam client/runtime update. | Keep exact filtering and user opt-out. Test controller input, overlay controls, native/Proton/Flatpak launches, and long sessions. Removing a preload at launch does not prove later absence. |
| Flatpak and Steam Linux Runtime: [Flatpak guide](engine/docs/FLATPAK-GUIDE.md), runtime [catalogue](engine/dist/flatpak/fgvk-render/runtime-versions.json) | Runtime branches/SDKs, Pressure Vessel namespaces/sockets, extension discovery, multilib, sandbox permissions and GL drivers. | Maintain the generated catalogue, verify every packaged branch, preserve bounded per-app overrides, and test actual native/Proton/Flatpak execution. Host success does not qualify another runtime or bitness. |

### Canonical version and dependency owners

These files supply the current values. Use their exact contents when recording a build; a package's declared range and its lockfile resolution are different facts. Host-supplied libraries and rolling runtimes need observed build identities in evidence.

| Dependency or contract | Canonical version/provenance owner | Update check |
| --- | --- | --- |
| SteamOS platform and bundled runtimes | Valve's [release notes](https://www.steamdeck.com/en/news) and [package repositories](https://steamdeck-packages.steamos.cloud/archlinux-mirror/); system-provided versions | Check the release channel and bundled Gamescope, Mesa, kernel, libraries, and tools against framegen-vulkan's assumptions. No build pin. |
| Vulkan-Headers shared native/Flatpak pin | [`engine/vulkan-headers-revision.txt`](engine/vulkan-headers-revision.txt) | `scripts/dev.sh check-flatpak-headers`; native/package required-header check and generated module tests. |
| Flatpak platform branches and LLVM SDK selections | [`runtime-versions.json`](engine/dist/flatpak/fgvk-render/runtime-versions.json), common [manifest template](engine/dist/flatpak/fgvk-render/org.freedesktop.Platform.VulkanLayer.fgvkrender.yml.in) | `scripts/dev.sh check-flatpak-runtimes`; runtime package verification and launches. |
| Experimental SPIRV-Tools and SPIRV-Headers commits/checksums | [`prepare-lsfg-fp16-tools.sh`](engine/scripts/prepare-lsfg-fp16-tools.sh) | Pinned-source preparation, experimental backend tests, notices and quality qualification. |
| Shader compiler and embedded SPIR-V | [`generate-color-conversion-spirv.py`](engine/scripts/generate-color-conversion-spirv.py), adjacent GLSL/hash manifests | Read-only freshness gate (`scripts/dev.sh check-color-conversion-spirv`); record the resolved `glslangValidator` version when regenerating and qualify changed output. The host compiler is not pinned by the hash manifests. |
| LSFG model support and vkd3d-shader ABI | [`model_resources.cpp`](engine/fgvk-backend/src/extraction/model_resources.cpp), [`dxbc_translation_abi.hpp`](engine/fgvk-backend/src/extraction/dxbc_translation_abi.hpp) | Capability/ABI contract and actual DLL/translator evidence; no universal DLL or installed-translator release pin. |
| Vendored toml++ and other incorporated source | [`toml.hpp`](engine/fgvk-common/thirdparty/include/toml.hpp), [COPYRIGHT.md](COPYRIGHT.md), [third-party notices](THIRD_PARTY_NOTICES.md) | Configuration parser/serialization/migration tests and upstream notices when updating. |
| Released Renderer payload consumed by Arch | [`engine/renderer-release.json`](engine/renderer-release.json), release/pinning scripts | `scripts/dev.sh check-arch-package`, complete candidate archive verification and public-asset install verification. Update through release owners. |
| framegen-vulkan version | [`engine/VERSION`](engine/VERSION), [`engine/RELEASE_NOTES.md`](engine/RELEASE_NOTES.md) | [Release](#release); versions, tags and published assets remain release-owned. |
| Rust toolchain for the settings window and `fgvk <command>` entry point | [`engine/fgvk-ui/Cargo.toml`](engine/fgvk-ui/Cargo.toml) (edition 2021) and the host `cargo` that `package-local.sh` calls in every packaging mode | `cargo test --locked` and `cargo build --release --locked` in `engine/fgvk-ui` (`scripts/dev.sh test-fgvk-ui`, `scripts/dev.sh fgvk-ui`, CI `fgvk-ui`). Check the glibc symbol versions of the built `fgvk` when the build host changes. |
| eframe and egui settings window | [`engine/fgvk-ui/Cargo.toml`](engine/fgvk-ui/Cargo.toml) and [`Cargo.lock`](engine/fgvk-ui/Cargo.lock): `eframe` 0.37 (glow, wayland, x11) and `egui` 0.37 | Review on every eframe/egui update: Wayland and X11 window creation, the `Panel` and input APIs, and a manual window check on a real session (not run by CI). |
| TOML editing of `conf.toml` in the settings window | [`Cargo.toml`](engine/fgvk-ui/Cargo.toml): `toml_edit` 0.25 | `cargo test --locked` for comment preservation, validation and atomic writes in `config.rs`; the same file format is read by the layer, so check both after an upgrade. |
| Launcher exclusion registry parsing in the process picker | [`Cargo.toml`](engine/fgvk-ui/Cargo.toml): `serde_json` 1, and [`launcher_exclusions.json`](engine/fgvk-common/launcher_exclusions.json) embedded with `include_str!` | `cargo test --locked` parses every registry entry. After editing the shared registry, run `scripts/dev.sh generate-launcher-exclusions` for the C++ binding; never hand-edit the generated header. |

### Repository and delivery responsibilities

| Area | Ongoing obligation and owner |
| --- | --- |
| Diagnostics and evidence | Keep structured Renderer fields, tests and collection guides aligned, including independent `FGVK_PACING_PROFILE` CPU windows, correlated hitch phases, optional `FGVK_PACING_PROFILE_GPU` backend timestamps and CPU readback cost, capture/session/gap/wrap records, bounded individual-output traces, actual wait1/wait2 windows with broad diagnostics disabled, the pre-configuration deferred-output join in outer present timing, nonblocking final-copy-fence observations after lower return, and the versioned report reader. Maintain full-trace per-source/application/wait records, individual lower acquire attempts and their outer IDs/driver-time partitions, actual present return results, native FG copy queue/semaphore dependencies, bounded copy GPU queries and completion-proven reuse, optional KHR/EXT calibrated-timestamp negotiation and supported clock domains, raw/calibrated clock uncertainty and missing evidence, exact one-use acquired-image ownership and unmatched/invalid evidence, bounded source-to-backend-generation mapping, worker readiness/queue-lock timestamps, mapped ELF build identity, short-session output retention and exclusive private timeline exports. Preserve legacy input compatibility, identify builds and environments, and distinguish prediction/submission from completion/visible output. Retain bounded private capture output, completion-proven consume-once GPU readback without extra waits, and separate synthetic observer-cost measurements from paired game overhead evidence. |
| Installation and packaging | Preserve [atomic installation/rollback](INSTALLATION-TRANSACTIONS.md), shared native ownership, package contents, both x86 bitnesses, Flatpak branches, ABI checks, licenses and clean candidate identity. Exercise failure/recovery paths whenever an installation boundary changes. |
| CI and build infrastructure | Maintain [workflow](.github/workflows/) action versions, runner images, Python/Node, compiler/container/SDK resolution and cache identities. Re-run applicable portable/package gates after toolchain changes and record resolved identities for release artifacts. |
| Releases | Follow [Release](#release), preserve immutable tags/assets, and verify the public artifacts and links. |
| Licensing and provenance | Keep upstream notices and source provenance with shipped material; review new code/assets and dependency upgrades. Preserve licensed-input exclusion and the GPL lineage boundary in [AGENTS.md](AGENTS.md). |
| Hardware QA and regressions | Maintain targeted scenarios and assertions in fgvk Gym and comparative sessions in fgvk Traces. Record exact tested combinations, regressions and gaps. Include bounded resource-constraint evidence before claiming complete Renderer hardware qualification. |

Maintain full-trace Vulkan setup comparisons with the existing diagnostics owners: paired incoming/forwarded requests and actual results, bounded enabled-extension/feature/queue/chain records, device/PID/layer identity, failed-create and unavailable/truncated evidence, and the private runtime companion. Keep actual use and presentation outcomes separate from enabled setup. Preserve strict commit adjacency alongside the separately qualified consecutive-attachment comparison; repeated or null attachments must prevent bridging an intermediate content submission.

### When to check

- **When upstream releases change:** review the affected dependency rows, identify changes to the interfaces framegen-vulkan uses, and fix code or pins when needed. For SteamOS, review stable releases and relevant Beta/Preview notes even before a test machine upgrades.
- **Before a framegen-vulkan release:** check for relevant upstream changes and run the applicable checks from [Build and test](#build-and-test) and [candidate qualification](#qualify-the-candidate).
- **When framegen-vulkan changes a dependency or compatibility assumption:** update its canonical owner, code, tests, and component guide together. Update this section if the responsibility or review trigger changes.
- **When support policy changes:** align runtime catalogues, architecture/ABI minimums, compatibility cleanup gates, installation guidance, and package checks. A newer dependency version alone does not justify removing older protocol support.

## Release

A release ships framegen-vulkan as one `X.Y.Z` version. Reuse a version only to resume that same incomplete release. Release from the current clean, pushed `main` commit and preserve the enforced order: **Renderer → checksum pin → public assets**.

### Release stages

| Stage | Command | Result |
| --- | --- | --- |
| Focused local iteration | `engine/scripts/build-steamos-dev.sh` | Incremental development build; no package or release |
| Complete local archives (legacy container builders) | `engine/scripts/package-local-release.sh` | Host archive, Flatpak archive, Arch package, and `SHA256SUMS` in `engine/out/`; no tag or release; needs Docker or Podman |
| Release-candidate check | `./scripts/check-release-candidate.sh engine/out/framegen-vulkan-vX.Y.Z-linux.tar.xz engine/out/framegen-vulkan-vX.Y.Z-flatpaks.tar.xz` | Checks the already-built archives against the clean pushed source commit, install manifest, both native bitnesses, and the Flatpak runtime list; no rebuild or installation |
| Publication | `./scripts/publish-release.sh X.Y.Z` | Publishes the Renderer archives, records immutable checksums in `engine/renderer-release.json`, and updates the release links |
| Public-asset check | Download the released host archive and run its **Install framegen-vulkan** file | Verifies the actual public artifact and installation path |

Fast native-only packages and direct installations are intentionally incomplete and cannot become release candidates.

### One-time setup

- Install the [Renderer build prerequisites](engine/docs/BUILDING-FROM-SOURCE.md).
- Authenticate GitHub CLI with `gh auth login -h github.com`.
- Confirm `origin` targets this repository and the worktree is clean.
- Prepare the SteamOS test machine and complete any affected manual game checks described in [Build and test](#build-and-test). A compatible fgvk Gym checkout is needed only for hardware suites you choose to run.

The release archive is built natively by the `release-build` GitHub Actions workflow ([`.github/workflows/release-build.yml`](.github/workflows/release-build.yml)) on `ubuntu-22.04`, with `engine/scripts/package-local.sh`, Clang 14 from Ubuntu, and the Vulkan headers pinned in [`engine/vulkan-headers-revision.txt`](engine/vulkan-headers-revision.txt). This keeps the released toolchain independent of the host SDK and needs no container. The workflow is manual, runs `scripts/dev.sh test-all` and `scripts/dev.sh package-engine`, and uploads the archive and its `.sha256` file as a workflow artifact; it never creates a tag or a release.

The container builders (`FGVK_PORTABLE_PACKAGE=1`, Docker or Podman) are legacy. `engine/scripts/package-local-release.sh`, `engine/scripts/publish-package.sh` and the Flatpak fallback still use them, and they are kept until the maintainer decides whether to remove them and switch publication to the workflow artifact. Flatpak extensions use the same Vulkan-Headers pin while retaining their separate runtime-specific compilers, libraries, and verification.

Local native builds of one clean commit are reproducible with the same toolchain. `engine/scripts/package-local.sh` sets `SOURCE_DATE_EPOCH` to the commit time of `HEAD` unless you set it, so every archive timestamp follows the commit. To verify, run `engine/scripts/package-local.sh --64-bit-only` on the same commit in two different directories (for example two `git worktree add --detach <dir> HEAD` checkouts), with the same compiler, Rust toolchain and `FGVK_VULKAN_HEADERS_INCLUDE_DIR`, and compare the two `sha256sum` values. The details are in [Building from source](engine/docs/BUILDING-FROM-SOURCE.md).

### Keep tester and release builds aligned

Source identity alone is insufficient: Vulkan extension macros can change compiled presentation handling. In 3.2.0, older native build headers omitted `VkPresentId2KHR` support that was present in the earlier tester binary. The 3.2.1 hotfix restored the tester's native build setup; the exact cause of every reported game failure was not confirmed.

- Build complete local archives with `engine/scripts/package-local-release.sh`, which uses the legacy container builders for the host and Flatpak archives. A changed container image, external SDK, or compiler package can still alter output without changing framegen-vulkan source; remove the matching cached archives before rebuilding after such a change. Publication rebuilds native archives independently.
- Keep the shared package header check enabled for both 64-bit and 32-bit native and Flatpak builds. `FGVK_REQUIRE_NATIVE_PACKAGE_HEADERS=ON` requires headers at least as new as [`engine/vulkan-headers-revision.txt`](engine/vulkan-headers-revision.txt), with `VK_KHR_present_id2` and `VK_EXT_present_timing`, even when tests are skipped. It checks build-time declarations and does not raise the game's Vulkan runtime requirement. Do not turn required compatibility support into an optional compile-time path without an equivalent fallback or a package failure.
- Retain the exact archives, their SHA-256 checksums, the embedded binary fingerprint, source commit and dirty state, build log, compiler and Vulkan header versions, and Flatpak SDK/runtime identities. Record the devices, driver, Proton version, games, and results against that artifact. After changing the header pin, regenerate the shared Flatpak module with `scripts/dev.sh generate-flatpak-headers`. Package and portable gates reject stale generated modules. Do not identify tested content by filename or source commit alone.
- Treat any builder, header, compiler, build-option, or runtime SDK change as a new candidate requiring affected compatibility coverage. The `release-build` runner image and its apt packages still change over time, so the workflow log prints the resolved toolchain versions; record them with each build.
- After publication, download and verify the actual public assets and complete the public installation and affected-game checks below. A successful build or checksum check does not establish game compatibility.

### Prepare the release

Write the user-facing “What’s new” copy in [framegen-vulkan release notes](engine/RELEASE_NOTES.md).

Use [Prepare Release](.agents/skills/fgvk-prepare-release/SKILL.md) (`$fgvk-prepare-release`) to draft the notes, artwork, and flavor text from the current branch. This prepares local release materials; it does not publish, deploy, switch branches, or authorize Git mutations. Candidate qualification and publication remain separate stages below.

#### Notes, identity, and artwork

Review the current notes and earlier release notes with `git show`, then compare the latest applicable released tag with the candidate branch. Inspect the final code and relevant validation evidence as well as commit messages: reverted experiments and earlier-release features are not new features. Write concise, user-facing changes in the established style, preserving contributor credit. Keep internal test details in the preparation handoff; include user-relevant limitations where needed.

Use the v4.5 release-note layout: the version heading, shared banner, codename and flavor quote, then a horizontal rule, one short introductory paragraph and a single flat list of changes. An optional shared horizontal benchmark PNG may follow the introduction, before the list; include a concise caption naming the compared builds, hardware, workload and metric limits. Show both versions and percentage changes explicitly, use measured data, retain unchanged or worse results, and keep private raw evidence in fgvk Gym. Keep numerical comparisons in the chart instead of repeating them in feature bullets. Start each item with `**Feature or fix:**` and a concise user-facing explanation. Keep benchmark scope and material limitations in the chart caption or relevant item. Avoid extra feature-section headings, nested lists and standalone feature paragraphs.

Update the first heading to the intended `X.Y.Z` and use the display codename with an initial capital, such as `### Release codename: Inferno` or `### Release codename: Neptune Fury`. A lowercase branch or asset slug is separate from that display name. The canonical notes are the only codename owner.

Create one shared banner at root `assets/<lowercase-hyphenated-codename>.png`. Inspect recent shipped banners for the established wide, roughly 3:1 Renaissance pixel-art and nautical framegen-vulkan style; adapt the scene and palette to the new name. Use the built-in image-generation tool for original artwork and substantive edits, then resize and compress the selected output for the web. Measure the actual dimensions and byte sizes of recent comparable assets to choose the new image's size budget; do not infer sizes from filenames or old provenance prose. Preserve the aspect ratio, inspect the optimized result at release-page size, and report its dimensions and bytes against those examples. Keep one final PNG without a duplicate WebP variant. Preserve historical images referenced by published notes, including the existing Sea Rapture WebP. Record the actual generation and optimization steps in [asset provenance](ASSET_PROVENANCE.md).

Give the Renderer notes one original flavor quote in the fantasy-card storytelling spirit of Magic: The Gathering. It should fit the new codename, illustration, and framegen-vulkan's existing nautical fiction. Match the italic blockquote and fictional attribution style of recent notes; inspiration does not mean copying a card quotation or inventing a real-world attribution. Use the banner with the current HTML image format and a meaningful alt description.

Validate the heading with `scripts/read-release-notes.mjs` for the intended version. Check the shared local PNG, capitalization and the quote. A new banner's raw `main` URL can remain unavailable until the asset reaches `main`; inspect the local image during preparation. The notes file is the only manual release copy; actual package versions, pins, downloadable asset URLs, checksums, and README download links remain script-owned. Note validation alone does not qualify a release candidate.

#### Qualify the candidate

Review affected dependencies and upstream changes using the [dependency and version owners](#dependencies-and-version-owners). Fix incompatible code or pins and run the relevant checks. Update the owning compatibility records when new evidence is obtained; preserve the distinction between source inspection, portable fixtures and tested runtime builds.

Commit the notes and the artwork with the release changes when authorized. The release builds validate the note heading, content, version, and codename.

Push the candidate commit and wait for the normal portable `Tests` workflow. Reuse an existing complete set of local archives from that exact clean commit, or build one on the SteamOS test machine with the legacy container builders used by `package-local-release.sh` (Docker or Podman required):

```bash
engine/scripts/package-local-release.sh
./scripts/check-release-candidate.sh engine/out/framegen-vulkan-vX.Y.Z-linux.tar.xz engine/out/framegen-vulkan-vX.Y.Z-flatpaks.tar.xz
```

The check verifies the archive integrity, install manifest, build fingerprint of the clean engine commit, both x86 bitnesses, and all supported Flatpak bundles without another build or an installation simulation. Retain the SHA-256 checksums and build identity with your game-test notes. Run relevant fgvk Gym suites ad hoc when a change needs hardware measurements; [Build and test](#build-and-test) maps boundaries to suites. Record what was run, reused, or not tested. Complete the applicable manual game matrix; portable CI and a package contract cannot establish game smoothness or compatibility.

The local candidate archives are not promoted byte-for-byte: publication makes its own release-version and pin commits and rebuilds the public artifacts in the enforced order. The final public-asset installation check remains a separate required stage.

### Publish

From the repository root, replace `1.2.0` with the release version:

```bash
./scripts/publish-release.sh 1.2.0
```

The publisher:

1. validates the release notes and the heading version;
2. versions, tests, builds, and publishes the 64-bit/32-bit Renderer host archive and supported Flatpak bundles;
3. records the exact Renderer tag, source commit, URLs, and checksums in `engine/renderer-release.json`, synchronizes the Arch `framegen-vulkan-bin` recipe to that immutable host archive, verifies both pins, and publishes the exact verified `.pkg.tar.zst` alongside the Renderer archives without installing it;
4. marks the Renderer release as GitHub's **Latest** release;
5. updates release links, pushes `main`, and verifies remote tags, Renderer asset checksums, and pins; and
6. removes rebuildable release output and disposable staging after verification while preserving reusable caches.

Do not manually edit generated version links or pins.

#### Maintainer-directed hotfix without automated validation

Only when the maintainer explicitly requests skipping automated validation, run the publisher with `FGVK_RELEASE_SKIP_TESTS=1`. This omits Renderer CTest and launcher tests, and adds `[skip ci]` to release-owned commits. Build, ABI, archive-layout, package-contract, and checksum verification still run. Record the skipped validation in the release evidence; this path does not establish game or hardware validation.

The publisher still uses the legacy portable container builder (`FGVK_PORTABLE_PACKAGE=1`) and header checks on this path. Match the tester evidence as described above; a new release has new version metadata and binaries, so matching runtime source and toolchain is not a claim of byte-identical artifacts.

### Resume an interrupted release

The top-level command is resumable: it checksum-verifies complete Renderer state before skipping any step. Fix the reported cause and rerun the same command.

For a deliberate Renderer-only resume:

```bash
./engine/scripts/publish-package.sh --version 1.2.0
```

Never move an existing tag or replace a published asset; publish a new version when released content must change.

### Verify the public package

After the command reports success, confirm the Renderer release is **Latest**, download its published host archive rather than the retained candidate, install it on a test SteamOS device, and run its **Install framegen-vulkan** file. This final check validates the public download and normal user-facing installation path.

Verify the downloaded host and Flatpak archives against the checksums pinned in `engine/renderer-release.json`, and confirm the installed Renderer reports the expected build fingerprint. Close games before updating the Renderer. For a launch-compatibility hotfix, repeat the affected game launches and retain the diagnostics; include 64-bit/32-bit and native/Flatpak paths when affected. Record unavailable rows explicitly. Keep the prior working artifacts for comparison; never replace a published asset to correct a mismatch.

Verify the final pushed `main` commit. Check the Renderer host, Flatpak and Arch package links and assets on the Renderer release page, plus the Renderer release links in the root and Renderer READMEs.

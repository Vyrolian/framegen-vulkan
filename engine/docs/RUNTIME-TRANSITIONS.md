# Runtime configuration transitions

This guide defines how a saved profile becomes running Renderer state. The [configuration reference](CONFIGURATION-REFERENCE.md) defines fields, [Fixed validation](ADAPTIVE-VALIDATION.md) owns pacing, [WSI isolation](WSI-ISOLATION.md) owns process-start discovery. [Memory management](MEMORY-MANAGEMENT.md) explains allocation ownership, replacement peaks, and accounting.

## Lifetime boundaries

A setting belongs to the earliest boundary that can safely establish all state it affects.

Saving a clone under a new name changes the runtime profile identity without resetting unchanged live policy. Subsequent edits use the same projection and live-update planner for every profile; selecting a new profile does not bypass startup resources, private handoffs, or restart requirements. Ordinary local matching remains unchanged.

| Boundary | State | Completion |
| --- | --- | --- |
| Process start | Frame Generation provisioning, Game Swapchain Images compatibility, layer membership and order, Zink, audio compatibility, and the shared FP16 precision choice | Start a new game process |
| Process-wide backend | DLL, GPU, and Ultra Performance policy | Construct a new backend, normally by restarting the process |
| Private FG context | Flow Scale, lighter model, and generated-output capacity | Prepare, drain framegen-vulkan-owned work, and atomically replace |
| Live policy | Frame Generation `0x`/active execution state, Fixed policy, caps, and cadence controls | Next successful reload and application present |
| Dormant value | A setting for an inactive mode or unavailable private resource | Save now; apply when its owning mode or resource becomes active |

The process-wide backend is created lazily when the first active swapchain needs it. FP16 is captured at process start even when the backend is still dormant, so a later FG backend cannot disagree with it after a config reload. Once built, pending DLL, GPU, and Ultra changes are compared with the actual construction baseline, not merely the previous file.

## Update flow

`Root::update()` is reached from application presentation, but work is bounded and change-driven:

1. Presentation feedback updates its owned state independently.
2. Configuration is checked at most every 250 ms. An unchanged file is not reparsed.
3. A changed file is parsed and profile matching is repeated.
4. Process-static fields are projected back to their applied values while compatible fields continue.
5. Each live swapchain receives a `ProfileUpdatePlan` based on its applied profile, private resources, generated capacity, and current extent support.
6. The live-safe merge is applied with only the necessary state resets; other values stay pending at their owning boundary.

Every application-owned swapchain creation forces one configuration freshness check so a replacement cannot combine new upper-layer policy with a stale lower-layer snapshot.

New Renderer fields use the normal parser, writer, editor, and lifetime owner.

## Requested, applied, and pending state

The transition engine keeps three facts distinct:

| Fact | Owner | Meaning |
| --- | --- | --- |
| Requested profile | `Root::active_profile` | Latest matching saved profile |
| Applied profile | `Swapchain::profile` | Values true for this live context |
| Backend baseline | `Root::backendGlobal`, `Root::backendProfile` | Process-wide inputs used to construct the backend |

`ProfileUpdatePlan::appliedProfile` is a merge. It starts from the requested profile, restores values that cannot cross the current boundary, then classifies the remaining effective differences. A pending restart or recreation must not block an independent live update.

For example, a write that changes Base FPS Cap and Flow Scale applies the cap while preparing a private FG replacement. The old Flow Scale remains applied until handoff. Selecting `0x` later still applies immediately, and reverting Flow Scale before handoff cancels the replacement.

## Transition matrix

| Setting or input | Boundary | Effect |
| --- | --- | --- |
| Frame Generation provisioning | Restart | Adds or removes LSFG application-device interop, backend ownership, and private resources. |
| Frame Generation `0x` | Live | Releases the effective base cap, resets relevant scheduler and recovery state, and takes the real-frame path without LSFG work. |
| Frame Generation `2x`–`5x` | Live when startup provisioning succeeded; otherwise restart | Reuses retained interop and private resources, then warms temporal history where required. |
| Fixed multiplier | Live within current capacity; otherwise private FG replacement or recreation | Dormant mode values are saved without resetting the active mode. |
| Base FPS Cap | Live while generation is active; dormant while Off | Resets the real-frame pacer. |
| Game Swapchain Images compatibility | Restart | Existing contexts retain the process-start WSI image-count policy. |
| Flow Scale and Lighter FG Model | Private FG replacement | Coalesces for 500 ms, prepares a complete candidate, drains framegen-vulkan-owned work, switches atomically, and warms history. |
| Generated-output capacity | Private FG replacement when the current WSI pool fits; otherwise recreation | Keeps the old active policy until enough resources and WSI headroom exist. A compatible maintenance1 context may request one. |
| Pacing | Natural recreation | Does not change a live swapchain or its transport. |
| DLL, FP16, GPU, and Ultra Performance | Restart | Existing contexts retain the actual backend baseline. |
| Steam overlay removal, WSI compatibility, external layer, Zink, and ALSA | Restart | These affect discovery or application initialization. |
| Active profile match | Live for compatible fields | Losing the match disables generation immediately; static differences remain pending. |

Normal profiles reserve capacity for the configured Fixed multiplier. Ultra Performance reserves only its startup-active policy. A later capacity increase can still use private replacement when the lower WSI pool has `application minimum + generated capacity` images; otherwise it remains pending for recreation.

## Private replacement contract

Private FG changes use one last-value-wins coordinator:

1. Coalesce the request when required.
2. Construct a complete candidate while the old resources remain usable.
3. Poll only framegen-vulkan-owned work; never call `vkDeviceWaitIdle`.
4. Commit atomically after drain proof.
5. Warm temporal history when the replacement affects Frame Generation.
6. Retain the old resources and retry later if construction or drain fails.

Private transitions never destroy the application swapchain or replace its WSI-facing synchronization. During a drain, presentation uses native or reconstructed real frames. A superseded candidate is discarded rather than partially applied.

## Recreation and retirement

A compatible maintenance1 context may request recreation for private-resource growth that cannot fit the current WSI pool. Recovery requests use the same successful, retirement-protected lower-present boundary; framegen-vulkan never destroys the active game-owned swapchain itself. Repeated cadence stalls or low FPS alone do not authorize performance-driven recreation.

The surface owner accepts recovery authority only from explicit live generation-policy transitions and generated-image acquire outcomes measured against the configured transport budget. A live mode, multiplier, or cap change starts fresh history and arms one recreation permission. Policy edits written before that change is applied are last-value-wins: they do not repeatedly rebuild the policy or arm parallel permissions. The transition supplies the single transport permission. The first generated-image acquire batch completed within its configured budget consumes that permission without action; `VK_TIMEOUT`, `VK_NOT_READY`, budget exhaustion, or a cumulative acquire-deadline overrun may instead consume it by requesting one retirement-protected recreation. There is no seconds-long event window, failure-count qualifier, output-FPS comparison, or surface cooldown. Replacement contexts initialize from the current transition sequence, preventing the same old event from rearming a recreation loop. Ordinary gameplay FPS, cadence loss, post-processing cost, and slow successful lower presents cannot obtain transport-recovery authority or request recreation; Fixed never launches a cadence-collapse probe. Backend/fence and acquire safety remain independent. [Fixed validation](ADAPTIVE-VALIDATION.md) owns the direct-outcome state machines and negative cases.

Presentation retirement is provisioned for Frame Generation. It follows owned resources and transport. Supported maintenance1 devices supply per-image fences; unsupported devices retain natural recreation and cannot use guarded retries. Natural and requested recreation remove the old context from live updates immediately. Lower WSI destruction waits for same-surface replacement progress, a 50 ms compositor grace, and retirement fences. On native Wayland outside gamescope, the lower swapchain is instead destroyed by the application's destroy call after its retirement fences complete (bounded at 1 s; a timeout falls back to the deferred path), because Wine's driver and the lower WSI share the surface's colour-management object. When an application or upper layer passes a null `oldSwapchain` after destroying its predecessor, framegen-vulkan completes the exact retained same-device, same-surface retirement before forwarding replacement creation; the compositor grace and retirement fences remain mandatory. Non-null `oldSwapchain` contexts retain their application-owned lifetime. Surface destruction is the terminal same-surface boundary.

A known replacement with Frame Generation active then presents real frames for 250 ms and warms three new history frames inside scheduler stabilization. A replacement created while Frame Generation is Off does not arm that FG-only interval.

Native Fixed 2x in MAILBOX/IMMEDIATE and 2x–5x in ordinary FIFO may retain the current output batch on a private queue under the [WSI handoff contract](WSI-ISOLATION.md). Admission copies supported ID/ID2 values (up to three identified batches in flight), mode, fence and single-swapchain timing values; unknown chains remain synchronous. No wait pattern disables ready-output pacing: a blocking wait on the latest retained ID, on the producing thread or a separate waiter, does not turn it off, and completion-paced sources do not release it. Both wait APIs join an unsubmitted retained ID before calling the driver, sharing the caller's original timeout budget; driver completion is never replaced by worker submission. Caller IDs, fences and timing metadata stay on the final real output; generated outputs inherit only a persistent mode override. Configuration polling drains retained submissions before private resource changes. Acquisition first polls for a spare image under the worker queue mutex. Sealed release-gated overlap batches keep outputting while acquisition retries timeout-zero polls with bounded sleeps and the original total timeout; ordinary batches join before a blocking acquire. The next present, queue/device idle and retirement also join the bounded operation; destruction joins before any captured semaphore or fence can be freed. The ready-output fallback and cap-owned delivery may retain every generated output and the real image, up to five existing slots from one source batch. The worker waits on already-submitted backend readiness within one full source period from arrival, bounded at 50 ms; that wait does not run after a cap sleep on the producer. Native Fixed cap preparation and its worker remain eligible. Every exit seals the batch. Overlap acquisition returns after the first free image rather than draining future batches; resource replacement and teardown still drain all captured work. A live policy change affects the next batch and cannot cancel or reschedule retained images. All synchronous and retained native presents keep the same reserved private queue across policy changes, including FG Off transitions; render/copy submissions retain their graphics queue. Eligible cap-owned native 2x batches use the same handoff with a worker-owned readiness wait within one full source period; a late output phase does not move the application source-cap clock. Unpaced readiness misses preserve an established detached cadence episode until a synchronous paced batch or policy reset replaces it.

Release-gated overlapped batches (`Readiness::overlap`, the uncapped VKD3D path) keep a generated output whenever its lower image is free by the end of its give-up window, so neither an expedite nor the batch limit skips it (F7 rules 2 and 4). It is skipped only for readiness, stopping, or no lower image in that window, and an expedited join may therefore poll that window before it returns. A first output whose backend readiness misses the first wait gets one bounded extension of `outputInterval` times the batch limit, inside the existing 50 ms cap; success there is not a miss, and a second timeout is one. A join during a released real output's sleep pulls that output to at least a quarter output interval after its generated output, never past its normal deadline. `FGVK_PACING_PROFILE=1` reports `generated_skip_readiness`, `generated_skip_expedite`, `generated_skip_give_up`, `generated_skip_acquire`, `generated_skip_other`, `late_readiness_batches`, `generated_compressed` and `internal_images` per second, one `pacing-profile-skip` record per skipped generated output, and `arrival_ns` on each batch's first output.

## State resets

Reset only state whose assumptions changed:

- enable/disable clears affected pacing, admission, acquire recovery, and history state;
- enabling Frame Generation live on a swapchain created without it requests one game-owned swapchain recreation (one `VK_ERROR_OUT_OF_DATE_KHR` after a retirement-fenced present, 500 ms after the change), so the replacement gets internal application images; without a retirement-fenced present, generation starts at the game's next swapchain recreation;
- mode, multiplier, or cap changes rebuild the affected real-frame pacing;
- configured cap changes reset the real-frame pacer and scheduler observations; a bounded scheduler rescue changes only its effective automatic cap and output clock, with rollback owned by the same rescue;
- probe-interval-only changes update only the timer;
- applied 3x–5x integer pacing may verify its real-frame cost against a recent trial baseline or obtain a fresh adjacent lower-load sample through the existing efficiency probe after a delayed handoff; a measured rejection restores the adjacent lower workload and output clock without history warm-up, resource replacement, or transport recovery, while interrupted evidence is discarded;
- private-resource, mode, multiplier, refresh, or transport-recovery changes clear transient transport evidence when its assumptions are no longer valid; and
- dormant or deferred values reset nothing until they apply.

A broad “configuration changed” reset would discard validated cadence after unrelated edits and is not allowed.

## Frame Generation provisioning, 0x, and Ultra Performance

Disabling Frame Generation provisioning before launch omits LSFG device interop and backend/private generation ownership. The Renderer remains dormant when Frame Generation provisioning is off.

The `0x` execution state submits no LSFG model work, generated-image acquisition, or generated presents. The saved cap is dormant. When provisioning is on, the process retains interop, backend, private images, and synchronization so `0x` can return to an active factor live. Failed provisioning leaves real-frame presentation active and reports restart pending.

Ultra Performance remains a process-start policy: the global FP16 choice, Flow Scale 0.7, lighter model, active-policy-sized capacity. Compatible live controls still work, but changing Ultra itself waits for restart and cannot partially mutate the active backend.

## Runtime status and diagnostics

The stderr lines that request a game-owned swapchain recreation, report that frame generation is unavailable for a swapchain, or announce a live profile-resource change are always written. The per-transition records (pending, prepared, applied, failed and recreation-requested) and the swapchain lifecycle records were removed with the present-diagnostics switch; captures from older builds may still contain them. Follow [Collect diagnostics](COLLECT_DIAGNOSTICS.md) to capture a session.

## Adding a setting

Before adding or changing a field, define:

1. its schema and validation owner;
2. the earliest safe lifetime boundary;
3. requested, applied, and construction-baseline state;
4. behavior in mixed writes and inactive modes;
5. the smallest scheduler, pacing, history, recovery, or resource reset;
6. behavior inside an active Ultra Performance process;
7. diagnostics that prove application; and
8. portable, sanitizer, Vulkan, hardware, and package evidence.

Extend `ProfileUpdatePlan` and the existing transition owner rather than adding a second reload path.

## Validation

Run portable and sanitizer coverage from `engine/`:

```bash
scripts/test-adaptive-scheduler.sh
FGVK_ENABLE_SANITIZERS=ON scripts/test-adaptive-scheduler.sh
```

`profile_update_tests.cpp` must cover a field alone, mixed with live and deferred fields, superseded or reverted requests, insufficient resources, inactive modes, and process-static baselines as applicable. Vulkan-facing changes also need the fgvk Gym suites selected by [Testing framegen-vulkan](../../TESTING.md). Mark unavailable hardware rows **not tested**.

### External descriptor ownership during resource construction

Exported image-memory and timeline-semaphore descriptors are scoped until the backend context call. The backend consumes the complete batch on entry, including early rejection, and closes every descriptor it has not yet handed to an importing Vulkan wrapper when construction fails. Each wrapper closes its current descriptor on failure before import succeeds; successful Vulkan import transfers ownership to the driver, so later bind/view failures must not close the descriptor number again. Image export happens after image-view construction succeeds. This covers initial provisioning, failed live private replacements, and CLI setup without changing present-loop work, retirement limits, or synchronization policy. Portable fault tests exercise partial export/import and descriptor-number reuse; fgvk Gym owns the repeated real-resource plateau evidence.

## Code and test ownership

| Responsibility | Source of truth |
| --- | --- |
| Parsing and watched configuration | `fgvk-common/src/configuration/config.cpp`, `fgvk-common/include/fgvk-common/configuration/config.hpp` |
| Polling, profile selection, and backend baselines | `fgvk-render/src/instance.*` |
| Merge and transition classification | `fgvk-render/src/profile_update.hpp` |
| Private transitions and live application | `fgvk-render/src/runtime_transition.hpp`, `fgvk-render/src/swapchain/resources.cpp`, `fgvk-render/src/swapchain/profile.cpp` |
| Real-frame and generated presentation | `fgvk-render/src/swapchain/present.cpp`, `fgvk-render/src/swapchain/present/` |
| Process-start transport policy | `fgvk-render/src/presentation_policy.hpp`, `scripts/fgvk-launch` |
| Status and diagnostics | `fgvk-render/src/present_diagnostics.*` |
| Deterministic tests | `fgvk-render/tests/profile_update_tests.cpp`, `runtime_transition_tests.cpp` |

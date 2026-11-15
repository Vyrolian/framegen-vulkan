# framegen-vulkan lifecycle

This guide explains how framegen-vulkan discovers its environment, creates and replaces resources, applies settings, paces frames, recovers from pressure, and shuts down. It is a conceptual inventory of lifecycle-affecting probes and policies rather than a source-file map. The exact setting contract remains in the [configuration reference](CONFIGURATION-REFERENCE.md), [Runtime configuration transitions](RUNTIME-TRANSITIONS.md), [Fixed validation](ADAPTIVE-VALIDATION.md), and [WSI isolation](WSI-ISOLATION.md). [Memory management](MEMORY-MANAGEMENT.md) covers allocation, pooling, accounting, and cleanup ownership.

## Safety model

framegen-vulkan separates proof from policy:

- Vulkan ownership, format support, queue support, image counts, external-memory support, and completion are established through Vulkan capabilities, return values, and fences. A timing or performance heuristic must never substitute for one of those proofs.
- Missing evidence fails closed when proceeding could corrupt presentation or violate ownership.
- Missing performance feedback normally fails open to native or less-managed presentation. It may reduce smoothness or efficiency, but it must not invent support.
- The application owns swapchain creation, recreation, and destruction. framegen-vulkan may request one spec-defined recreation signal when the active presentation boundary can support it, but it does not destroy an active application swapchain merely to apply a setting.
- The original application frame has priority. Frame Generation or recovery failure falls back to the real frame wherever the negotiated swapchain geometry still makes that safe.

## Lifetime map

| Lifetime | Created by | What becomes fixed | What can still change |
| --- | --- | --- | --- |
| Process | Vulkan layer activation and profile match | Layer-chain role, Game Swapchain Images compatibility, and launcher-owned compatibility behavior | The configuration can be re-read, but changes to process-static choices remain pending until restart |
| Vulkan device | The application's device creation | Available queues, device extensions, synchronization features, and optional swapchain-maintenance support | Per-swapchain policy can change within those capabilities |
| Private Frame Generation backend | The first active swapchain that needs Frame Generation | Selected GPU, DLL-derived model family, startup FP16 precision choice, and Ultra Performance topology | Per-swapchain Frame Generation contexts may be replaced; the process backend is shared |
| Surface | Application surface creation | Native window-system provenance | Current capabilities and output feedback are rechecked at swapchain creation |
| Application swapchain | Application creation or recreation | Source and presentation extents, transport, image pool, and generated-output headroom | Live policy and compatible private resources may change |
| Private Frame Generation context | Initial swapchain setup or a live private transition | Images, pipelines, model topology, and synchronization objects for that context | A complete replacement can be built beside the old context and switched atomically |
| Present policy | Each successful application present | Nothing beyond the current frame plan | Caps, Fixed decisions, refresh guards, admission, recovery, and diagnostics update continuously |
| Deferred retirement | Application destruction of a swapchain | The retired lower swapchain and its completion evidence | Later same-surface presents, replacement creation, surface destruction, or device destruction complete retirement |

The normal progression is:

```text
process/profile match
  -> device capability registration
  -> surface observation
  -> swapchain negotiation and private context construction
  -> warm-up
  -> steady presentation
  -> live policy update or private-context replacement
  -> application-requested recreation
  -> deferred retirement
  -> surface/device/instance shutdown
```

## Probe inventory and cost

Costs are qualitative: **negligible** is cached state, arithmetic, or a nonblocking readiness check; **low** is a syscall or a small set of window-system queries; **moderate** is a group of Vulkan capability queries; **high** is GPU allocation, pipeline/model construction, or a potentially blocking wait. Driver calls can exceed their expected cost, so bounded waits are called out separately.

### Periodic and change-driven probes

| Probe | Trigger or cadence | Rough work and cost | Effect and safety behavior |
| --- | --- | --- | --- |
| Configuration freshness | At most once every 250 ms while application presents are reaching framegen-vulkan; forced at every application swapchain creation | One filesystem metadata check, **low**. A changed file causes a full parse and process-identity rematch, **low to moderate** | The last valid configuration remains active after read or parse failure. A failed parse can be retried after 50 ms, although the outer 250 ms poll usually sets the effective retry cadence |
| Process identity | Once at startup and again only after a changed configuration parses successfully | Executable/name reads and, for Wine or Proton, a process-map scan, **low to moderate** | An unmatched profile leaves framegen-vulkan dormant and presentation native |
| Cached display-feedback consumption | At most once every 250 ms on the presentation path | Lock and copy of cached state, **negligible** | Refresh and output geometry can update policy. |
| Runtime status publication | Only when requested/applied/pending state changes | Atomic small-file replacement, **low but not steady-state work** | Status is observational and cannot block rendering on cleanup errors. The first publisher performs one bounded cleanup of unlocked framegen-vulkan-owned stale entries |

Recovery backoffs are separate from background probes.

### Event-driven capability and allocation probes

| Probe | Trigger | Rough work and cost | Effect and safety behavior |
| --- | --- | --- | --- |
| Device support | First application device creation | Queue-family, extension, feature, and optional maintenance capability enumeration, **moderate once per device** | Unsupported queues or synchronization features constrain later presentation rather than being guessed |
| Current swapchain capabilities | Every swapchain creation | Surface capabilities plus selected queue/format checks, **moderate** | The current maximum image count and extents are authoritative even when earlier observations were favorable |
| Direct generated output | Frame Generation context construction | External-image format and usage capability queries, **moderate** | Supported devices can write directly to the presentation-compatible output; otherwise framegen-vulkan uses a private image and copy path |
| Private resource construction | Initial context creation or an accepted live transition | Image allocation, exported/imported synchronization, pipelines, model translation, and backend context creation, **high** | The old context stays active until a complete candidate is ready. Failure retains the old context and schedules a bounded retry |
| Frame Generation backend creation | Once, lazily, on the first swapchain that actually needs Frame Generation | DLL discovery and validation, GPU matching, a private Vulkan instance/device, resource extraction/translation, and pipelines, **high one-time cost** | Failure disables Frame Generation without changing the user-owned input. Neither path performs periodic backend work |

### Per-present observations and probes

| Probe | Trigger or cadence | Rough work and cost | Effect and safety behavior |
| --- | --- | --- | --- |
| Cadence observation | Every application present | Steady-clock samples and allocation-free arithmetic, **negligible** | Drives Fixed refresh budgeting and caps. It observes application delivery, not physical scanout, and cannot authorize transport recovery |
| Policy-plan admission | Every present that requests generated outputs | Arithmetic and state checks, **negligible** | May shorten or remove the synthetic plan before any backend work while preserving the real frame |
| Tight-headroom image admission | Once for each planned generated output in headroom-tight ordered paths | Zero-time image acquisition, **low per attempted output** | A miss drops synthetic work before the backend starts. |
| Private-context readiness | While a live replacement is pending | Zero-time fence and timeline checks, **negligible** | Presentation temporarily uses real frames instead of waiting for device-wide idle |
| Retired-image completion | When a protected image is reacquired and while retirement can progress | Zero-time maintenance-fence checks, **negligible** | Recreation is not signalled and destruction is not completed until ownership evidence is available |
| Generated-path recovery | Only after an explicit acquire, fence, or backend failure | Zero-time checks in normal recovery; after acquire backoff, one image uses the configured acquire budget or one confirmed display period, and is nonblocking if neither exists, **low to bounded high** | Failure returns to native drain and increases backoff. Success resumes after fresh history without inventing a performance classification |
| Render completion | Before reusing generated work in the normal ordered path | Fence wait capped at 150 ms, **bounded high** | A miss abandons generated output for the present and enters backend recovery; it does not sacrifice the real frame |
| Lower presentation | Every delivered image | Driver/compositor queue-present call, **potentially unbounded outside framegen-vulkan's control** | Returned duration is diagnostic only. framegen-vulkan cannot cancel a currently blocked driver call and does not infer recovery authority after it returns |
| Renderer-owned Vulkan presentation timing | First eligible FIFO present initializes support; one history query per source batch, with clock calibration at startup, on domain changes and after one second during use | Bounded Vulkan timing/history calls, **low to moderate** | Calibrated queue completion trains preparation lead; unsupported capabilities or failed feedback retain existing pacing. Application-owned timing keeps its namespace. [WSI isolation](WSI-ISOLATION.md#timing-capabilities-across-the-stack) owns negotiation, clock validation and fallback |
| Renderer-owned Google timing fallback | Every 32 scheduled outputs on the eligible optional WSI path | Count query and at most 128 timing records, **low** | Drains only framegen-vulkan-owned history; query failure disables the timing path. Application-owned timing and enabled presentation waits exclude this fallback |
| Optional diagnostics | Only when explicitly enabled | Clock reads around selected operations and conditional stderr writes; slow-operation default threshold 20 ms, **negligible until logging, potentially visible when verbose** | Diagnostics are observational. Logging overhead means performance measurements should compare equivalent diagnostic settings |

## Timing ledger

This table centralizes lifecycle timers. Frame-count gates are included because they act like time gates at the current cadence.

| Area | Current interval or threshold | Purpose |
| --- | --- | --- |
| Configuration | 250 ms poll; 50 ms earliest parse retry | Avoid a filesystem read/parse on every present while recovering quickly from an atomic editor update |
| Root discovery | At most once per second | Bound repeated candidate-display probing |
| Private resource coalescing | 500 ms for private-resource changes | Apply completed choices promptly while coalescing slider movement and related edits |
| Private transition retry | 1 second after a color-readiness exception; 5 seconds after construction failure | Avoid a rebuild or exception storm while retaining the old context |
| Replacement warm-up | 250 ms real-only, then three real history frames | Prevent interpolation across old/new swapchain history |
| Conservative discontinuity recovery | 1 second recovered evidence; at most 5 seconds before resuming from zero generated load | Bound conservative real-only recovery after a cadence discontinuity |
| Generated-image drain | Fixed: 250, 500, 1000, 2000, 5000, 15000, then 30000 ms after an acquire timeout, budget exhaustion, or configured cumulative deadline overrun | Pace bounded retries after the explicit transport budget is missed |
| Pipeline busy | 250 ms uninterrupted busy state | Treat a single busy frame as harmless but reset history after sustained pressure |
| Swapchain retirement | At least 50 ms compositor grace plus fence completion | Avoid destroying a lower swapchain still being observed or used |
| Device teardown retirement | Up to 250 ms total, then forced finalization | Bound application device destruction while still attempting orderly retirement |

## Configuration lifecycle

Process-static settings wait for restart; swapchain geometry and transport wait for application recreation; private-resource edits prepare and atomically replace framegen-vulkan-owned contexts; live policy applies on the next successful present. Inactive values remain dormant.

Requested, applied, pending, and effective state remain distinct. Independent live edits continue while other changes wait, the latest request wins, and reverting to the applied value cancels pending work. [Runtime transitions](RUNTIME-TRANSITIONS.md#transition-matrix) owns the field-by-field matrix, mixed-write behavior, and reset rules.

## Swapchain lifecycle

### Creation

For every application swapchain creation, framegen-vulkan:

1. Forces a fresh configuration check so multiple presentation roles use the same revision.
2. Resolves the surface provenance, current output target, source and presentation extents, queue support, format support, surface limits, and memory admission.
3. Chooses immutable presentation transport and generated-output headroom, plus the initial colour pipeline.
4. Requests the application minimum image count plus generated capacity when allowed. Ordered Frame-Generation-only paths may reserve one relief image. The surface maximum remains authoritative, and Game Swapchain Images compatibility preserves the application's original minimum from process start.
5. Creates the lower swapchain. If only an expanded minimum fails with initialization or host/device memory exhaustion, it retries exactly once with the application's original minimum.
6. Reads the images actually returned. Fewer images than the application minimum removes generated capacity rather than assuming headroom.
7. Builds the required Frame Generation resources. Frame Generation failure retains a valid native swapchain.

### Warm-up and steady presentation

A cold context begins with real history before generated delivery. A known replacement additionally holds real-only presentation for 250 ms and then gathers three real history frames.

On each present, framegen-vulkan updates cached policy, filters unsupported presentation extensions, applies an optional base-frame deadline, records cadence, advances private transitions, chooses a Fixed frame plan, performs admission, schedules backend work, delivers generated images, and finally presents the original real image.

Any exception or failed generated path returns to the real frame. If a failure occurs after generated images were acquired, framegen-vulkan still initializes and retires those images before the real present so image ownership remains valid.

### Recreation

Applications naturally recreate swapchains for resize, mode changes, out-of-date results, and other WSI events. For a configuration-driven change, framegen-vulkan can request recreation only when all of the following are true:

- the requested setting genuinely changes swapchain-owned resources;
- the presentation role is allowed to own that change;
- swapchain-maintenance retirement fences are supported;
- a successful or suboptimal lower present has supplied retirement evidence; and
- the same request has not already been signalled.

A compatible boundary may request recreation when generated capacity exceeds current WSI headroom. The request is one `OUT_OF_DATE` result; the application remains responsible for creating the replacement. Repeated polls do not loop the signal, while a distinct request can arm a new one. Transport-failure and transition-scoped recovery requests follow the separate rules under [Pressure and recovery policies](#pressure-and-recovery-policies).

Explicit old-swapchain linkage is preferred. When an application omits it, framegen-vulkan recognizes a replacement only from a unique exact same-device/surface live or retained context. This constrained inference prevents unrelated swapchains from donating temporal history or retirement state.

### Destruction and retirement

Application destruction immediately removes a context from live updates, but lower WSI destruction may be deferred. When maintenance fences and the default allocator permit it, framegen-vulkan retains the lower swapchain for at least 50 ms and until its image fences prove retirement. Later presents on the same surface advance this work through zero-time completion polls, including when the replacement fell back to native presentation. Collection skips a busy retirement mutex and retains the context while its backend or render work is still in flight, so presentation does not enter the backend's 250 ms close wait. An idle context still incurs its normal resource-destruction cost when collected.

If a matching null-old replacement arrives while an exact retained swapchain remains, framegen-vulkan completes and destroys the retained object before creating the replacement. Surface destruction is terminal and waits for same-surface retirement. Custom allocation callbacks require synchronous destruction because their callback data cannot safely outlive the application call.

Device destruction waits up to 250 ms across deferred retirements and then force-finalizes remaining lower swapchains because the parent device is itself going away. Instance destruction joins the display-feedback monitor before unload. The private backend Vulkan device is intentionally retained for process lifetime to avoid loader-destruction hazards in mixed layer stacks; the operating system reclaims it at process exit. This is retained memory, not periodic work.

## Frame pacing and generation policies

### Fixed

Fixed 2x–5x requests one to four generated frames between real frames. Confirmed refresh limits outputs the display cannot consume; absent feedback preserves the requested multiplier. The refresh threshold fails open when feedback is missing. Gameplay cadence cannot change the selected multiplier or authorize recovery.

### Base cap and Smooth Cadence

The real-frame cap uses an absolute deadline and rebases after late frames; its intentional sleep can span the selected cap interval. Smooth Cadence (`adaptive_stable_cadence`) remains a Fixed setting: ready-output pacing is eligible when it is on or the output is detached, and the uncapped automatic cap applies only while it is off. Manual caps remain authoritative.

## Pressure and recovery policies

framegen-vulkan separates explicit transitions, direct transport failure, and feature policy. An explicit transition refreshes three history frames and arms one recreation permission that is consumed by the first generated-image acquire batch completed within its configured budget. An unproven policy after a live reset and unsupported transports retain conservative settling but cannot authorize recreation. A second interruption restarts the full history warm-up.

1. **Pre-admission:** remove synthetic outputs before backend work when headroom, policy, or nonblocking image acquisition says they cannot be delivered.
2. **Acquire-budget failure:** `VK_TIMEOUT`, `VK_NOT_READY`, exhaustion of the configured cumulative application-present budget, or completing the batch after that cumulative wall-clock budget is reached stops the affected generated delivery. Fixed enters native backoff. The budget is explicit; FPS, cadence, resolution, and shader cost do not derive or change it.
3. **Bounded retry:** Fixed retries after 250, 500, 1000, 2000, 5000, 15000, then 30000 ms; fresh history precedes one bounded generated-image acquire. A real-only policy frame leaves the retry pending without acquisition or a failure count. [Transport recovery](ADAPTIVE-VALIDATION.md#transport-recovery) owns the detailed rules.
4. **Transition-scoped recreation:** an explicit live generation-policy change arms one permission. An acquire batch completed within its configured budget consumes it. An acquire-budget failure may consume it by requesting one retirement-protected game-owned recreation; there are no separate event windows, FPS qualifiers, or failure-count thresholds.
5. **Backend recovery:** a render-fence miss or backend failure switches to real frames while readiness is polled with zero-time waits. This is direct synchronization safety and cannot select a multiplier or request recreation. Recovery resets temporal history before generation resumes.

Normal ordered generated-image acquisition can be unbounded when no compatibility timeout is configured and relief headroom is available. Fixed batches without an additional relief image also enforce at most 50 ms, with shorter configured limits taking precedence. Headroom-tight paths use nonblocking admission. The shared deadline covers the complete application-present batch, so higher multipliers do not multiply the wait. See [WSI acquisition policy](WSI-ISOLATION.md#presentation-and-swapchain-policy) for the detailed rules. An unbounded driver/compositor acquire remains framegen-vulkan’s largest controlled blocking exposure. The lower queue-present call itself is always driver-controlled and cannot be bounded by framegen-vulkan, and its returned duration is diagnostic only.

## Heuristic audit

| Heuristic or inference | Why it exists | Guardrails and failure mode |
| --- | --- | --- |
| Exact same-device/surface null-old replacement inference | Recover lifecycle continuity from applications that omit explicit old-swapchain linkage | Requires a unique exact match and never reuses an unrelated context |
| 50 ms retirement grace | Cover compositor observation beyond the application's destroy call | Grace is not the proof: image retirement fences and terminal waits remain authoritative |
| Unbounded ordered acquisition where permitted | Avoid rejecting healthy but variably delayed compositor acquisition | Relief headroom and no configured or mode-specific deadline; see the acquisition policy above |

The first two rows affect placement, admission, state confirmation, or lifecycle association. None is allowed to assert Vulkan capability, completion, image ownership, transport failure, or recreation authority.

## What framegen-vulkan does not probe

- Vulkan memory budget is queried only for resource admission.
- framegen-vulkan does not observe physical scanout timestamps. Present/acquire duration and application cadence remain proxies and cannot prove scanout.
- framegen-vulkan does not continuously rescan Vulkan capabilities. Device and surface facts are cached for their owning lifetime and current swapchain limits are rechecked at creation.
- framegen-vulkan does not infer safety from average frame rate. Capability, return-code, queue, fence, and ownership checks remain separate from performance decisions.
- framegen-vulkan does not support managed multi-swapchain present batches by guessing semaphore ownership; those shapes fail closed before synchronization is consumed.
- framegen-vulkan cannot preempt a blocking Vulkan driver call. Recovery begins after control returns.

These limits are deliberate, but they define where diagnostics and real-hardware evidence matter most. Any new probe should document its owner lifetime, trigger, worst-case foreground cost, cache invalidation, failure behavior, and whether it supplies safety evidence or only policy evidence.

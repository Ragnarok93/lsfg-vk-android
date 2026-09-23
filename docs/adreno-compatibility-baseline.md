# Adreno compatibility baseline and reconstruction

## Scope

This document separates three things that were previously conflated:

1. the historical source topology that proves how the stable Adreno path was wired;
2. the device/runtime captures that prove useful S20+ delivery was achieved;
3. later experiments that are explicitly rejected for the protected Adreno route.

The protected route is a Qualcomm/Adreno driver-family compatibility policy. It
must not change the Xclipse capability-driven path.

## Historical source-topology reference: `364178af`

Commit
`364178afb7a35c5e83ebdf284be7281b24b00172`
is the authoritative source-topology reference for the September 18 lineage.

The source at that revision directly establishes the following behavior:

- Android OPAQUE_FD external-semaphore support enables the asynchronous AHB
  source handoff.
- Ordinary generated cycles submit the game-device source copy and export an
  OPAQUE_FD semaphore to the private framegen device instead of host-waiting for
  the source upload.
- Warmup and zero-generation cycles retain the conservative host-fence source
  upload path.
- After framegen dispatch, the source-present thread performs the bounded
  `waitContext()` completion wait before the game device reads generated AHBs.
- Generated WSI frames are presented in interpolation order and the matching
  real source is presented in the same intercepted call.
- A zero-generation Adaptive cycle still calls the zero-count framegen path,
  advances temporal history, and presents the real source normally.

The synchronization topology is therefore:

```text
game rendering
  -> OPAQUE_FD GPU-semaphore source handoff
  -> private framegen
  -> bounded host completion
  -> generated presents
  -> matching source present
```

The protected Adreno route must preserve that division of responsibility:
conservative generated-frame completion does **not** imply serializing the
ordinary generated-cycle source upload.

## Device/runtime evidence

The September 18 S20+ / Turnip / Adreno 650 capture from the `364178af`
lineage demonstrated viable fixed 2x delivery at approximately one generated
frame per real frame. Representative captured source/output pairs include:

- 29.82 -> 59.64
- 30.17 -> 60.34
- 29.98 -> 59.96
- 30.00 -> 60.00
- 30.06 -> 60.12

These values are evidence that the topology can sustain useful generated
delivery on that workload. They are not FPS-specific control targets; current
source protection remains cadence-relative.

A separate later GameNative recovery checkpoint,
`f01bf0c4697719a0a5407d9c7993fb5a2a4aa898`, explicitly pins native commit
`f5e43cf33ae7fbcacb06d1a2132fe7d28eb7c11f`. That package/capture checkpoint
is useful device evidence, but it is **not** a substitute for the direct
`364178af` source-topology reference. Historical GameNative at that checkpoint
still used the older runtime marker `gamenative-adaptive-50ef4ea6-r1`; the
modern integration requirement is stricter and requires the runtime marker to
contain the exact pinned native SHA.

## Current repaired Adreno contract

The active `adreno-latest-known-good` path must satisfy all of these
invariants:

| Area | Protected Adreno behavior |
| --- | --- |
| Ordinary generated source handoff | OPAQUE_FD GPU semaphore |
| Warmup/history/source-only source handoff | Conservative host fence where required |
| Generated completion | Bounded host `waitContext()` |
| Generated delivery | Generated frames then matching source, same intercepted call |
| Game-device queue topology | No synthetic queue |
| Cross-call source buffering | Disabled |
| Deferred generated completion | Disabled |
| Fixed scheduling | Requested multiplier is a ceiling; `FixedSourceCadenceGovernor` protects source cadence |
| Adaptive compute admission | Next real-source boundary is authoritative; ideal interpolation slots remain presentation/shadow information |
| Zero-generation history | Zero-count framegen advances temporal history; no lifecycle-Off transition |
| Xclipse | Existing capability-driven asynchronous route unchanged |

The runtime synchronization policy identifier for this route is
`adreno-source-protected-host-completion`.

## Rejected experiments

The following mechanisms are invalidated for the protected Adreno route and
must not be re-enabled without new reproducible device evidence:

- SYNC_FD source input on ordinary generated Adreno cycles;
- asynchronous/deferred generated completion across later source calls;
- retaining or buffering an application source swapchain image or application
  present wait across intercepted calls;
- synthetic game-device queues;
- fully host-serializing the source upload on every generated cycle;
- unconditional Fixed generation that delays the real source to satisfy the
  configured multiplier;
- using the ideal synthetic midpoint as the authoritative Adreno compute
  deadline;
- treating a fractional zero-generation opportunity as LSFG Off or as a
  lifecycle reprime.

Runtime
`gamenative-adaptive-2302f56ec75eaf20529484a4630fafed5e19585b-r24`
is a rejected intermediate package. Its deferred/SYNC_FD Adreno behavior is
retained in source only where needed for lifetime auditability; the active
compatibility selector hard-disables it.

## Retained lifetime/correctness work

Later correctness repairs remain valid when they do not change the topology:

- pass slots remain owned until their real completion dependencies retire;
- generated output copies retire on fences attached to the actual consuming
  submissions;
- presentation-wait semaphores retire only after the corresponding swapchain
  image is reacquired;
- AHB reuse waits for the real retained dependency;
- imported/exported external semaphore FDs have explicit ownership;
- configuration or discontinuity abandonment makes stale output ineligible
  without discarding private-device release evidence;
- B14 mipmaps work, Adaptive Flow, source-protection telemetry, WSI provenance,
  history reason codes, and configuration hot reload remain available.

## Xclipse isolation

`xclipse-current` is not routed through the Adreno compatibility policy.
Xclipse retains capability-driven source handoff and asynchronous completion
when its probed SYNC_FD capabilities permit them. Shared fixes must be
behavior-neutral when
`compatibilityPath_ == FramegenCompatibilityPath::XclipseCurrent`.

Expected initialization remains semantically equivalent to:

```text
path=xclipse-current behavior_changed=0
```

## Qualification telemetry

A device qualification run must make the route identifiable without inference.
For protected Adreno, initialization/runtime logs must expose at least:

- `path=adreno-latest-known-good`;
- `handoff=opaque-fd`;
- `completion=host-wait`;
- generated-before-source same-call presentation;
- `synthetic_queue=0`;
- `deadline_semantics=source-protection`.

Runtime metrics must continue separating source-copy submission, source-copy
host-fence wait, framegen dispatch, bounded framegen completion wait, source
interval/cycle timing, planned versus admitted generation, source-protection
budget, presentation-slot budget, WSI drops, admission/deadline drops, history
state, Fixed governor state, Adaptive demand, Flow Scale, and present failures.

## Validation matrix

| Runtime / device | Required evidence |
| --- | --- |
| S20+ LSFG Off | Establish real-source baseline; no FG controller may pace source cadence. |
| S20+ Fixed 2x | OPAQUE_FD ordinary source handoff, host-bounded completion, same-call generated/source delivery, useful generated throughput. |
| S20+ Fixed 3x / 4x | Synthetic work backs off before severe source collapse; multiplier remains a ceiling. |
| S20+ Adaptive 30 / 45 / 60 | Fractional demand remains target-driven; source-boundary compute admission is active; zero-generation cycles preserve history. |
| S25 FE / Xclipse 940 | Existing capability-driven path and Adaptive Flow behavior remain unchanged. |

Deterministic contracts and Android builds are mandatory gates before device
testing, but they do not replace the S20+ and S25 FE runtime runs.


## Compatibility-island boundary

The current scheduler architecture may evolve around this path, but the protected
Adreno transaction is an execution compatibility island.

Allowed **upstream** inputs from newer architecture:

- Fixed source-cadence governor generation ceiling.
- Adaptive frame-generation requested count.
- Source-protection deadline admission.
- Adaptive Flow Scale / performance-governor quality choice.
- Telemetry, diagnostics, and cost prediction.

These components may decide only **whether** a source cycle generates and **how
many** synthetic frames are requested.

Once a generated Adreno cycle is admitted, the downstream transaction is frozen
to the September 18 reference:

1. Export the reusable OPAQUE_FD source-handoff semaphore before submission.
2. Submit the source AHB copy on the application graphics queue with the reusable
   handoff fence attached, without host-waiting that fence.
3. Dispatch private framegen using that OPAQUE_FD input.
4. Perform bounded host completion before the game device reads generated AHBs.
5. Acquire generated swapchain images with the historical bounded wait.
6. Present generated images before the matching real source in the same
   intercepted call.
7. Preserve the September 18 application-pNext ownership: the first generated
   present owns the downstream application chain when synthetics exist; otherwise
   the real source owns it.
8. Present the matching real source last.

Zero-generation Adreno history cycles remain conservative: host-fence the source
copy, execute zero-count private preprocessing, complete it on the host, then
present the source. Cross-device zero-history SYNC_FD release is prohibited on
this compatibility path.

The following are **not** valid governor adaptations for Adreno: synthetic game
queues, cross-call source buffering, deferred generated completion, zero-time
generated WSI acquisition, cross-device zero-history release, or changing
generated/source present order. Xclipse and generic capability paths are outside
this compatibility island and retain their current behavior.

Runtime diagnostics must report
`execution_reference=364178af-sep18 governor_adapter=admission-only` whenever
this path is selected.

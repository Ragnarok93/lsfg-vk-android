# Adreno compatibility baseline and reconstruction

## Proven baseline

The newest repository state explicitly pinned as the validated Galaxy S20+
recovery runtime is:

- GameNative commit: `f01bf0c4697719a0a5407d9c7993fb5a2a4aa898`
- GameNative branch: `recovery/restore-device-proven-18865-20260918`
- pinned `lsfg-vk-android` commit: `f5e43cf33ae7fbcacb06d1a2132fe7d28eb7c11f`

This choice is not inferred from commit naming. The GameNative commit explicitly
pins the native runtime, and the associated September 18 S20+ capture identifies
Turnip/Adreno 650 and records stable fixed and Adaptive delivery near 30 source +
30 generated frames per second without present failures. That makes `f5e43cf`
the behavioral reference even though later lifetime fixes must be retained.

### Baseline contract

| Area | `f5e43cf` behavior on the validated S20+ capture |
| --- | --- |
| Source handoff | Game-device source copy to alternating input AHB; host-fence fallback selected on the captured Turnip stack. Optional OPAQUE_FD existed for capable generated cycles. |
| Completion | Bounded `waitContext()` on the source-present call before the game device read generated AHBs. Observed completion was about 15.8–16.3 ms rather than the later 60–142 ms regression. |
| AHB ownership | `direct-input-copy-output`: framegen sampled imported input AHBs and wrote output AHBs; the game device copied output AHBs into WSI images. EXTERNAL ownership transfers surrounded cross-device access. |
| Presentation | Acquire generated WSI images, submit output copies, present generated frames in interpolation order, then present the real source frame. |
| Queue topology | Application graphics/present queue on the game device plus the private framegen device compute queue; no separate game-device synthetic graphics queue was required by the validated path. |
| Present mode | Existing configured/surface-selected WSI mode. The capture reported present mode `1`; display-timing capability was unavailable and did not supply pacing. |
| History | Zero-generation Adaptive cycles advanced source history rather than entering an Off/source-only lifecycle. |
| Capability gates | AHB transport and external-semaphore support were probed. There was no explicit Qualcomm/Xclipse compatibility enum. |
| Lifetime | Per-pass command buffers and semaphores were ring-owned. Later fixes that distinguish producer completion from output-copy and WSI consumer retirement are required and are intentionally retained. |

## Regression boundary

Commit `144e3ff410f0dd12e40300c77af8b649c59af638` changed single-queue
Adreno from deferred completion back to `host-wait` and labeled the policy
`syncfd-input-host-completion-adreno`. Real-game evidence then showed framegen
dispatch below roughly 1 ms while host completion consumed approximately
60–116 ms (142.563 ms in the latest aggregate window), collapsing the source
timeline to roughly 8–15 FPS. AHB transfer remained sub-millisecond, so the
serialization was not explained by source-copy cost.

## Selective reconstruction

The repaired Adreno route keeps the proven baseline's observable invariants:

1. alternating source history remains authoritative;
2. generated WSI presents precede the source frame they interpolate into;
3. a missed generated opportunity is consumed once and never becomes debt;
4. source-only, fractional, and admission-rejected cycles do not invalidate
   history without a concrete discontinuity or ownership failure;
5. output AHBs are copied into game-device WSI images before presentation.

The r24 deferred experiment was invalidated by the September 22 S20+ capture.
It retained the first application source image and returned success without a
downstream source present. The application then submitted images 1 through 5;
when batch 1 became privately complete at source age 4, the layer unloaded and
the guest render process exited. Adreno therefore returns to the exact
device-proven host-fence/host-completion/same-call presentation topology. No
application source image or present wait is retained across calls.

Later correctness repairs remain in force:

- the pass slot remains owned until private batch completion;
- generated output copies retire on fences attached to the real consuming copy
  submissions, not an unrelated empty submit;
- presentation-wait semaphores retire only after the associated swapchain image
  is acquired again;
- input/output AHB reuse waits for the actual final retained dependency;
- imported/exported SYNC_FDs have single, explicit ownership;
- configuration/discontinuity abandonment makes output ineligible but does not
  discard private-device release evidence.

The selector is explicit:

| Path | Policy |
| --- | --- |
| `adreno-latest-known-good` | Device-proven host-fence handoff, bounded host completion, and generated-then-source presentation in the same intercepted call; later lifetime fixes remain active. |
| `xclipse-current` | Existing capability-driven asynchronous Xclipse behavior, unchanged. |
| `generic-capability` | Existing generic capability path. |

## Retained newer work

The reconstruction retains B14 mipmaps tail fusion, subgroup capability
queries, Adaptive Flow infrastructure, deadline/presentation diagnostics,
configuration hot reload, WSI provenance diagnostics, source-history reason
codes, and safe pass/semaphore/AHB retirement. These are not used to route
Xclipse through the Adreno policy.

## Validation matrix

| Runtime | Source cadence | Generated/output cadence | Completion | Presentation/stability |
| --- | --- | --- | --- | --- |
| S20+ LSFG Off | Device run required | N/A | N/A | Establish native p95/p99 and external cadence. |
| S20+ known-good `f5e43cf` | About 30 FPS in the recorded workload | About 30 generated / 60 LSFG output FPS | About 15.8–16.3 ms host completion | Fixed 2x and Adaptive delivered; no recorded present failure in the reference capture. |
| S20+ failing `144e3ff` | Roughly 8–15 FPS | External cadence materially below requested output | Roughly 60–116 ms representative; 142.563 ms latest aggregate | Source-present serialized on generated completion. |
| Repaired Adreno | Device run required | Device run required | Must show no routine source-critical host wait; `adreno-batch-delivery` and batch GPU timing share one `batch_id`. | Must match or improve `f5e43cf`; late output must drop without source delay or debt. |
| Current Xclipse | Regression run required | Regression run required | Existing async capability path | Initialization must report `path=xclipse-current behavior_changed=0`. |

Batch traces cover the first eight batches and every 120th batch by default to
avoid perturbing cadence. Set `LSFG_VK_BATCH_TRACE=1` for a full diagnostic
capture; `framegen-gpu-timing` and `adreno-batch-delivery` can then be joined by
`batch_id`.

Deterministic host tests and Android builds are necessary gates, but they do not
substitute for the S20+ and S25 FE runs above.

## Invalidated r24 route

Runtime `gamenative-adaptive-2302f56ec75eaf20529484a4630fafed5e19585b-r24`
is a rejected Adreno route. Its `deferred-sync-fd` completion and
`generated-before-buffered-source` policy must not be re-enabled on
Qualcomm/Turnip. The compatibility selector now hard-disables that route while
leaving the Xclipse capability path unchanged.

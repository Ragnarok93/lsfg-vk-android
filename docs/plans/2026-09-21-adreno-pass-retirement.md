# Adreno pass-retirement ownership audit

This document records the lifetime contract implemented by the pass-retirement
repair. It is intentionally separate from the LSFG scheduling and image-quality
plans.

## Retirement domains

| Resource | Producer | Consumer | Producer-complete evidence | Destruction permission |
| --- | --- | --- | --- | --- |
| `preCopyBuf` | Game-device source-copy submit | Game queue completion | `completionFence` | Deferred bundle after a real later source-submit fence, or teardown queue idle |
| `preCopySemaphores[0]` | Source-copy submit | Source `vkQueuePresentKHR` | `completionFence` only proves signal submission | Same deferred bundle rule; WSI wait is not inferred from the producer fence |
| `preCopySemaphores[1]` | Source-copy submit | Next actual source-copy submit | `completionFence` only proves signal submission | Explicit `lastSourceCopyDependency_` token plus deferred bundle |
| `framegenInputSemaphore` | Source-copy submit | Framegen-device imported handoff | Game producer fence | Deferred bundle and the existing AHB/framegen completion path |
| `framegenBatchCompleteSemaphore` | Framegen-device batch | Next source-copy submit | Framegen completion/export result | `lastBatchCompleteDependency_` token plus deferred bundle |
| `renderSemaphores` | Framegen output completion | Generated post-copy submit | Framegen output-ready dependency or host fallback | Deferred bundle; post-copy producer fence does not retire WSI consumers |
| `acquireSemaphores` | WSI image acquisition | Generated post-copy submit | Acquisition operation | Deferred bundle until the later real source-submit anchor |
| `postCopyBufs` | Game-device generated post-copy submit | Queue execution | Per-output post-copy fence | Deferred bundle until WSI consumers retire |
| `postCopySemaphores` | Generated post-copy submit | Generated `vkQueuePresentKHR` | Per-output post-copy fence only proves signal submit | Deferred bundle until the later real source-submit anchor |
| `prevPostCopySemaphores` | Generated post-copy submit | Next generated present or final source present | Per-output post-copy fence only proves signal submit | Deferred bundle until the later real source-submit anchor |
| Source/output AHB-backed images | Game/framegen devices | Cross-device image operations | Existing SYNC_FD/host completion and EXTERNAL barriers | Context lifetime; pass reuse does not release the imported images |

The distinction is deliberate: Vulkan's semaphore destruction rule requires all
submitted operations that refer to the semaphore to have completed. A producer
fence proves completion of its producer submission, not completion of a later
WSI or queue consumer. See the Vulkan reference for
[`vkDestroySemaphore`](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroySemaphore.html)
and [`vkQueuePresentKHR`](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html).

## State transitions

1. A pass generation records real source and generated submissions with their
   producer fences.
2. `tryRecyclePass()` waits for those producer fences with a zero timeout.
3. Once producer work is complete, the slot's command buffers and semaphore
   wrappers move to `RetiredPassResources`; the slot receives empty wrappers and
   can be prepared for a later generation.
4. The next real source-copy submission anchors all unanchored retired bundles
   to its existing completion fence. No empty post-present submission is used.
5. `collectRetiredPassResources()` removes a bundle only after that anchor fence
   signals. If the bundle is anchored to the same fence owned by the slot being
   reused, slot reuse is blocked until the consumer retirement is observed.
6. Destruction after context teardown is preceded by queue idle; the backend
   context is released before member-owned deferred bundles are destroyed.

The asynchronous policy uses only zero-timeout host queries on the normal path.
`LSFG_VK_RETIREMENT_POLICY=conservative-host` is a diagnostic isolation mode;
it waits for queue idle at pass recycling without changing shaders, scheduling,
AHB transport, or presentation policy.

## Dependency ownership

`frameIdx` remains the temporal/presentation counter and continues to select a
pass-ring slot for compatibility with the existing cadence behavior. It is no
longer used to identify the previous source producer. `lastSourceCopyDependency_`
and `lastBatchCompleteDependency_` name the actual pass index and generation
that produced each binary semaphore. A source-only pass-ring fallback advances
the logical frame but leaves both dependency tokens unchanged.

The normal Android handoff remains SYNC_FD when capability queries select it;
the host-fence path remains the bounded fallback. AHardwareBuffer EXTERNAL
queue-family barriers and framegen-device completion are unchanged by this
repair.

# Adreno pass-retirement ownership audit

This document records the lifetime contract implemented by the pass-retirement
repair. It is intentionally separate from the LSFG scheduling and image-quality
plans.

## Retirement domains

| Resource | Producer | Consumer | Producer-complete evidence | Destruction permission |
| --- | --- | --- | --- | --- |
| `preCopyBuf` | Game-device source-copy submit | Game queue completion | `completionFence` | Producer fence, or teardown queue idle |
| `preCopySemaphores[0]` | Source-copy submit | Source `vkQueuePresentKHR` | `completionFence` only proves signal submission | Retained by the presented swapchain image until that image is reacquired |
| `preCopySemaphores[1]` | Source-copy submit | Next actual source-copy submit | Consuming source submit's `completionFence` | Explicit producer token copied into the consuming pass |
| `framegenInputSemaphore` | Source-copy submit | Framegen-device imported SYNC_FD handoff | Game producer fence after payload export | Producer fence; the imported SYNC_FD owns the transferred payload independently |
| `framegenBatchCompleteSemaphore` | Framegen-device batch | Next source-copy submit | Consuming source submit's `completionFence` | Explicit producer token copied into the consuming pass |
| `renderSemaphores` | Framegen output completion | Generated post-copy submit | Per-output post-copy fence | Consuming post-copy producer fence |
| `acquireSemaphores` | WSI image acquisition | Generated post-copy submit | Per-output post-copy fence | Consuming post-copy producer fence |
| `postCopyBufs` | Game-device generated post-copy submit | Queue execution | Per-output post-copy fence | Per-output post-copy fence |
| `postCopySemaphores` | Generated post-copy submit | Generated `vkQueuePresentKHR` | Per-output post-copy fence only proves signal submit | Retained by the generated swapchain image until reacquisition |
| `prevPostCopySemaphores` | Generated post-copy submit | Next generated present or final source present | Per-output post-copy fence only proves signal submit | Retained by the image presented by that consumer until reacquisition |
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
3. Queue-wait semaphore owners are copied into the consuming pass and remain
   there until that pass's producer fence signals.
4. Every LSFG-owned semaphore passed to `vkQueuePresentKHR` is copied into the
   ownership record for the presented swapchain image.
5. Reacquiring that exact image retires its previous WSI ownership record. Pass
   slots may recycle independently because their WSI handles remain shared-owned.
6. No later queue submit is treated as proof of WSI completion, and no empty
   post-present submission is used.
7. Context teardown waits both producer and observed present queues idle before
   releasing any image-owned WSI semaphore handles.

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

# Exact dispatch and acquire-consumer lifetime audit

Status: implementation checkpoint; NOT a verified S20+ fix or release-ready build.

Starting heads: LSFG `19ae9f06cbda0adadedf2f25c9ca2c8042d9e466`, GameNative `f995c3a3acfb12a43e08eb715f43f11d9dcb10ef`.
Known-good comparison: LSFG `e99417e4ae7dc796ee487104c546a52e735c3816`, GameNative `e0f47d7b19e2eccc24939d5001003fd1491bb67e`.
Both remote fix/adreno-pass-retirement-20260921 heads were checked through the GitHub connector before editing.

## Confirmed defects addressed

- Queue2 acquisition bypassed the exact queue registry.
- Unknown submit/present queues used mutable process-global downstream PFNs.
- Swapchain lookup compared dispatch-pointer identity and accepted a unique numeric handle candidate. Neither establishes ownership across logical devices.
- Device dispatch snapshots were populated through mutable globals during construction. Construction now populates a local device table; the temporary loader GDPA is scoped and thread-local.
- Device/command-buffer wrappers no longer fall back to another device's function table. Queue1 and queue2 register the same exact ownership table. Helper-device tables contain queue submit/present PFNs where supported; destruction interception also remains active while config is disabled.
- WSI owners were released on the CPU when image acquisition returned, before the acquisition signal had necessarily completed. They now transfer into the pass of the real submission consuming acquisition. Its existing fence protects those owners, without an extra submission or host wait.
- A source-only bypass leaves unresolved WSI ownership in the image record; subsequent retention appends rather than overwriting it.

## Producer/consumer graph

Shared Mini::Semaphore owners release the Vulkan handle only at their final reference. The table describes normal successful submissions. Teardown/failure paths remain a release blocker below.

| Owner | Producer | Consumer | Starting-head retirement | Updated retirement |
|---|---|---|---|---|
| preCopySemaphores[0] | source copy | source WSI present when no generated output | pass producer fence plus image-owner retained until acquire returns | pass producer fence plus image-owner retained through acquire-wait consuming fence |
| preCopySemaphores[1] | source copy | next actual source copy | explicit lastSourceCopyDependency and consuming pass fence | unchanged; never inferred from frameIdx |
| framegenInputSemaphore | source copy | exported FD payload imported by private framegen device | game signal producer fence; backend owns imported object/payload | unchanged; SYNC_FD copy transfer and OPAQUE_FD shared payload remain enabled |
| framegenBatchCompleteSemaphore | private framegen batch exported/imported completion | next actual source copy | explicit lastBatchCompleteDependency and consuming pass fence | unchanged |
| postCopySemaphores[i] | generated image copy | WSI present of generated image i | producer fence plus image owner until acquire returns | image owner transfers through subsequent acquisition-consuming submission fence |
| prevPostCopySemaphores[i] | generated image copy | next generated WSI present or final source WSI present | separate WSI signal domain, retained on consumer image | same domain; acquire-wait fence now gates release |
| nextPostCopySemaphores[i] | generated image copy | next generated copy queue submit | consuming pass owns wait until its post-copy fence | unchanged |
| acquireSemaphores[i] | WSI acquisition | generated image copy | post-copy completion fence | unchanged; fence also now retires previous WSI owners for that image |
| renderSemaphores[i] | private framegen exported/imported output | generated image copy | consuming post-copy completion fence | unchanged |

No shaders, B14/B15, mipmaps, flow scale, scheduler, image quality, source dependency tokens, SYNC_FD selection, or async completion behavior were modified. No new wait-idle call, sleep, empty submit, or host fence wait was added.

## Tests and limits

`tests/run_android_dispatch_lifetime_test.py` compiles the production Android layer implementation and Mini::Semaphore implementation against fake downstream Vulkan functions. It extracts the actual swapchain lookup and WSI transfer/retention methods into a minimal context shell. This is not a GPU test or a complete LsContext integration test.

Passing cases: queue1/queue2, two logical devices with distinct downstream PFNs, 32 interleaved submissions/presents per device, unknown queues, colliding swapchain values, handle reuse, device map cleanup, conflicting live ownership, 32 delayed WSI generations over four ring wraps, delayed acquisition completion, and semaphore destructor ordering.

`--baseline-lifetime` substitutes the starting head's real early-release method (signature adapted only) and fails the delayed-acquisition assertion; the updated method passes. Producer completion alone does not release those handles.

## Remaining release blockers

1. `LsContext::~LsContext()` still waits queues idle and clears unresolved WSI owners. Vulkan does not guarantee that queue-idle proves presentation completion. The current patch does NOT prove safety for unresolved final presents on recreation/shutdown or every exceptional path. Do not label this checkpoint a complete synchronization repair. Resolve teardown using supported presentation fences or an explicitly owned deferred swapchain retirement mechanism, with tests covering the real teardown path, before release validation.
2. The existing invalid-image retention failure path and allocation/submit-failure paths require complete teardown ownership proof. Native normal-path tests do not certify these paths.
3. No attached ADB transport, Android SDK/NDK, S20+, or S25 FE was available locally. No physical test, current-head runtime log, cadence comparison, or image-quality validation has been performed.
4. Debug/signed CI results and exact packaged native SHA must be checked separately. A successful build is not runtime validation.

Required physical matrix remains: S20+ LSFG off/fixed/adaptive, launch enabled, Quick Menu off/on/re-enable, suspend/resume, screen off/on and recreation; Xclipse fixed/adaptive/Adaptive Flow, launch and Quick Menu, source/presentation cadence and absence of new host synchronization. Compare against the stated device baselines, not synthetic host counters.

Specification reference: https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html (acquisition signal must be waited; queue-idle shutdown limitation).

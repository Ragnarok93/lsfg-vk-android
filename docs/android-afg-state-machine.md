# Android Adaptive Frame Generation state machine

This branch treats **user-facing Off**, **Adaptive zero-generation**, and **FG-active** as different states. The Vulkan layer may remain resident in all three states so GameNative can hot-reload configuration, but residency is not permission to alter WSI while Off.

## Presentation / WSI states

### 1. Create-time Off

Condition: `enabled = false` or `multiplier <= 1` before `vkCreateSwapchainKHR`.

Behavior:

- Create the application's swapchain from the application's unmodified `VkSwapchainCreateInfoKHR`.
- Preserve the application's `minImageCount` and present mode.
- Do not create `LsContext`.
- `vkQueuePresentKHR` is a native pass-through except for an in-memory check for a staged configuration change.
- No adaptive scheduler, source/output pacer, AHB handoff, framegen wait, generated present, config filesystem I/O, or stats filesystem I/O executes on the present thread.

### 2. Runtime Off after FG-active

Condition: a staged config changes `enabled: true -> false`, crosses `multiplier > 1 -> <= 1`, or otherwise changes an FG WSI requirement.

Behavior:

1. Present the transition source frame natively.
2. Return `VK_ERROR_OUT_OF_DATE_KHR` once to request the application's normal swapchain recreation flow.
3. Apply the staged config at swapchain creation.
4. Recreate with the application's untouched image-count/present-mode request.
5. Drop the old `LsContext`; framegen's last-context release tears down its backend state.
6. Enter Create-time Off semantics for sustained Off.

A one-time transition hitch is acceptable. Periodic work while Off is not.

### 3. Adaptive zero-generation

Condition: FG is enabled and the swapchain/context is FG-capable, but `AdaptiveFrameScheduler::plan()` returns zero because the target is already satisfied or fractional demand has not accumulated to a frame.

Behavior:

- This is **not Off** and does not restore WSI.
- Present the source frame directly without AHB/framegen work for that frame.
- Mark interpolation history for warmup before generated output resumes.
- Do not intentionally pace/throttle source FPS from the Adaptive output target.

### 4. FG-active

Condition: `enabled = true`, `multiplier > 1`, capability checks succeeded, and the active plan requests generated frames.

Behavior:

- Use the bounded fence + AHB external-ownership path as the portable synchronization fallback.
- Attribute measured AHB handoff, dispatch, framegen completion and generated-present cost to the previous generation level.
- Feed the scheduler the time from the end of the previous LSFG cycle to the next present entry (game/source cadence), not the LSFG-inflated present-entry interval.
- Space generated/source output using a drift-corrected phase accumulator.

## Capability decision tree

The decision tree is the same for Samsung Xclipse and Qualcomm A6xx. Vendor/device identity is diagnostic/provenance data, not the primary policy selector.

1. **Exact device binding available?**
   - Require Vulkan physical-device/driver UUID identity for the framegen backend.
   - If unavailable: fail open to native presentation.
2. **AHB external-memory extension available?**
   - Require `VK_ANDROID_external_memory_android_hardware_buffer` for this Android exchange path.
   - If unavailable: create/present the game's swapchain unmodified.
3. **Required image transport supported by the active ICD?**
   - Ask the selected framegen backend for its AHB transport mode for the selected format.
   - If unsupported: fail open.
4. **Bidirectional blit support available for shared and swapchain formats?**
   - Require source+destination blit feature bits for both sides of the copy path.
   - If unavailable: fail open.
5. **Surface has enough swapchain image headroom?**
   - Reserve only the configured maximum generation capacity.
   - Respect `maxImageCount`; otherwise fail open.
6. **Configured present mode supported?**
   - Use it only if enumerated for the surface; otherwise preserve the game's mode, then FIFO as the standards-mandated fallback.
7. **Cross-device synchronization**
   - Portable baseline on both Xclipse and A6xx is explicit external queue-family ownership plus a game-device handoff fence and bounded framegen completion wait.
   - synchronization2/driver-specific fast paths may be added only after capability validation; legacy barrier shims remain available for older Adreno stacks.
8. **Runtime cost policy**
   - Controller thresholds are ratios of measured LSFG stage cost to measured source/game-work interval. No Xclipse-only or A6xx-only millisecond constants are used.

## Required device evidence

For each validation run, capture the ICD/device identity and `capability_matrix` records, then exercise: Off baseline -> fixed 2x -> Adaptive -> Off for at least two seconds -> On -> Off.

A valid FG-active window must contain non-zero `generated_fps`, increasing `generated_frames_total`, zero or bounded generated-present failures, and successful `generated-present-ready`/metrics output. A valid sustained-Off window must contain the WSI restore transition followed by native presentation without `LsContext::present` metrics, AHB handoff, framegen wait, or periodic config-poll cadence.

Physical acceptance requires one run on Exynos 2400 / Xclipse 940 stock Samsung Vulkan and one on an Adreno 6xx path used by GameNative (Turnip/Vortek or stock Adreno). CI and synthetic tests cannot substitute for those two device runs.

# Capability-Driven Android Framegen Implementation Plan

> **For agentic workers:** Use the host's available task-by-task implementation workflow. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Android frame generation select and operate on the exact game Vulkan driver stack, retain the current fast path when all probes pass, and degrade safely through standards-compliant fallbacks instead of vendor-name checks.

**Architecture:** Split initialization into immutable device provenance plus capability policy, then select transport, synchronization, WSI-capacity, and copy/conversion strategies from probe results. The presentation fast path remains the existing AHB/shared-storage + sync2 path when all capabilities are present; unsupported or timed-out operations transition the swapchain context to fail-open native presentation without weakening successful Xclipse behavior.

**Tech Stack:** C++20, Vulkan 1.1-1.3, Android NDK AHardwareBuffer, Volk, Python static contract tests, GitHub Actions Android arm64-v8a/x86_64 builds.

## Global Constraints

- Do not use GPU vendor checks to choose runtime behavior.
- Match framegen to the game's exact `deviceUUID` and `driverUUID` before creating shared resources.
- Preserve the current high-performance path when all probes succeed.
- Probe AHB external-image usage explicitly and add `AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER` only for direct storage-AHB mode.
- Provide transport-only AHB with private framegen storage images when direct storage import is unavailable.
- Size swapchain headroom from the configured multiplier and fail open before wrapping when surface capacity is insufficient.
- Eliminate unbounded handoff, generated-image acquire, frame-slot, and teardown waits.
- Probe exact source/destination format blit support before recording `vkCmdBlitImage`; use a compliant conversion/copy path or bypass.
- Vulkan 1.1 support must use binary semaphores/fences and legacy barriers rather than pretending Vulkan 1.2/timeline support exists.
- Emit initialization diagnostics with API/driver provenance, UUIDs, R16F AHB storage probe results, and swapchain capacity/headroom.

---

### Task 1: Exact driver identity and capability policy

**Files:**
- Create: `framegen/public/lsfg_device_identity.hpp`
- Modify: `include/layer.hpp`, `src/layer_android.cpp`, `src/layer.cpp`, `include/utils/utils.hpp`, `src/utils/utils.cpp`
- Modify: `framegen/include/core/{instance,device,capabilities}.hpp`, `framegen/src/core/{instance,device,capabilities}.cpp`
- Modify: `framegen/public/lsfg_3_1.hpp`, `framegen/public/lsfg_3_1p.hpp`, `framegen/v3.1_src/lsfg.cpp`, `framegen/v3.1p_src/lsfg.cpp`
- Test: `framegen/tests/capabilities_test.cpp`, `tests/android_capability_architecture_test.py`

**Interfaces:**
- Consumes: game-side `VkPhysicalDevice` and Vulkan properties2 queries.
- Produces: `LSFG::DeviceIdentity { deviceUUID, driverUUID }`, exact-match `Core::Device`, immutable capability/transport decision and diagnostics.

- [ ] Add failing policy/contracts proving Vulkan 1.1 can choose binary/fence synchronization, timeline absence is not fatal, fake vendor/device IDs are forbidden, and both UUIDs are required.
- [ ] Resolve `vkGetPhysicalDeviceProperties2`, `vkGetPhysicalDeviceImageFormatProperties2`, and format-property queries through the Android layer chain.
- [ ] Query `VkPhysicalDeviceIDProperties` on the game device and require byte-for-byte deviceUUID + driverUUID equality when framegen enumerates physical devices.
- [ ] Request Vulkan 1.2 on capable loaders and Vulkan 1.1 only when necessary; never attach Vulkan 1.2 feature structs to a 1.1 device.
- [ ] Keep core/KHR sync2 selection unchanged for capable devices; select legacy barrier + binary semaphore/fence synchronization for 1.1.
- [ ] Emit exact-match driver diagnostics and run capability policy + Android builds.

### Task 2: AHB storage contract and transport fallback

**Files:**
- Modify: `include/mini/image.hpp`, `src/mini/image.cpp`
- Modify: `framegen/include/core/{device,image}.hpp`, `framegen/src/core/{device,image}.cpp`
- Modify: `framegen/v3.1_include/v3_1/context.hpp`, `framegen/v3.1_src/context.cpp`
- Modify: `framegen/v3.1p_include/v3_1p/context.hpp`, `framegen/v3.1p_src/context.cpp`
- Modify: `include/context.hpp`, `src/context.cpp`
- Test: `tests/android_ahb_portability_test.py`, `tests/android_capability_architecture_test.py`

**Interfaces:**
- Consumes: exact framegen device and selected LSFG VkFormat.
- Produces: `DirectStorage` or `TransportOnly` AHB strategy; unsupported format yields clean context bypass.

- [ ] Probe R8/R16F with `VkPhysicalDeviceExternalImageFormatInfo` and `vkGetPhysicalDeviceImageFormatProperties2` for the exact AHB handle type and storage/transfer usage sets.
- [ ] In direct mode allocate AHB with sampled/color-output plus `AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER` and preserve existing shared-storage shader path.
- [ ] In transport-only mode import shared AHB only with transfer usage, allocate private storage/sampled images on framegen device, copy AHB input into private images before compute and private generated output back to AHB after compute.
- [ ] Preserve EXTERNAL queue-family ownership only for shared AHB images; private images remain owned by the framegen queue.
- [ ] Verify both normal and performance 3.1 contexts use the same transport decision.

### Task 3: WSI capacity, bounded waits, and degradation

**Files:**
- Modify: `include/config/config.hpp`, `src/config/config.cpp`
- Modify: `include/hooks.hpp`, `src/hooks.cpp`
- Modify: `include/context.hpp`, `src/context.cpp`
- Modify: `framegen/include/common/utils.hpp`, `framegen/v3.1*_include/*/context.hpp`, `framegen/v3.1*_src/{context,lsfg}.cpp`
- Test: `tests/android_runtime_stability_test.py`, `tests/android_capability_architecture_test.py`

**Interfaces:**
- Consumes: multiplier, surface min/max image count, configurable finite timeout values.
- Produces: required image headroom, per-context `Active`/`Degraded` presentation state, bounded frame-slot/context drain.

- [ ] Calculate required wrapper capacity from source image plus every generated presentation outstanding for the configured multiplier; if finite `maxImageCount` cannot satisfy it, create the game's original swapchain unchanged and never create an LSFG context.
- [ ] Replace AHB handoff fence and generated `vkAcquireNextImageKHR` infinity waits with configured finite nanosecond timeouts.
- [ ] On timeout, mark the context degraded, present the source frame through the native swapchain path using already-produced synchronization where safe, and bypass framegen on subsequent presents.
- [ ] Replace framegen slot waits with finite fence waits.
- [ ] Remove unconditional `vkDeviceWaitIdle()` from delete/finalize; drain tracked completion fences within the teardown budget and retain/abandon timed-out driver-owned resources rather than destroying objects still in flight.
- [ ] Verify no Android/framegen runtime path uses `UINT64_MAX` as a wait timeout.

### Task 4: Blit probing, diagnostics, and end-to-end verification

**Files:**
- Modify: `include/hooks.hpp`, `src/hooks.cpp`, `include/context.hpp`, `src/context.cpp`
- Modify as needed: framegen transport/conversion files from Task 2
- Test: `tests/android_capability_architecture_test.py`, Android build workflow

**Interfaces:**
- Consumes: game swapchain format, LSFG working format, `VkFormatProperties`, AHB/transport decision and WSI capacity.
- Produces: validated blit path, transfer/copy-conversion fallback or native bypass, unified initialization telemetry.

- [ ] Query exact BLIT_SRC/BLIT_DST format-feature bits for the source and destination formats before recording a blit.
- [ ] Use direct blit only when both formats advertise required features; otherwise use the transport/conversion pass when format-compatible, else cleanly bypass LSFG.
- [ ] Emit one initialization diagnostic block containing API version, driver name/info/version, deviceUUID, driverUUID, R16F AHB direct-storage result, min/max swapchain images, requested multiplier, and required headroom.
- [ ] Run policy/static tests plus arm64-v8a and x86_64 Android builds; preserve all existing AHB portability and WSI loader/provenance contracts.

## Unresolved externally observable decisions

None. Timeout defaults remain conservative at 500 ms and are configurable; every capability or timeout failure is fail-open to native presentation rather than an application-visible Vulkan initialization failure.

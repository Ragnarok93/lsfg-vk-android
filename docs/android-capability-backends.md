# Android capability-selected framegen backends

The Android frame-generation path selects behavior from Vulkan capabilities rather than GPU vendor names.

- Physical-device provenance is matched with both `VkPhysicalDeviceIDProperties::deviceUUID` and `driverUUID` so framegen cannot silently switch between Turnip and a proprietary ICD for the same GPU.
- Direct-storage AHardwareBuffer sharing is used when the exact external-image storage contract is supported. Otherwise, transport-only AHB sharing keeps shader storage private to the framegen device and copies through transfer-capable shared images. After history is primed, transport-only refreshes only the alternating source slot that changed.
- When both exact Vulkan devices advertise importable/exportable `OPAQUE_FD` external semaphores, Android orders source upload, framegen, generated readback, and shared-AHB reuse entirely on the GPU. Drivers without that capability retain the bounded host-fence / context-wait compatibility path.
- Vulkan 1.3/core synchronization2 remains the preferred path, followed by `VK_KHR_synchronization2`, with a Vulkan 1.1 legacy-barrier/binary-semaphore/fence backend when required.
- Swapchain wrapping is bypassed when multiplier-derived image headroom or exact blit format capabilities are unavailable. Headroom is calculated from `multiplier - 1`, so higher multipliers cannot silently reuse the old fixed `+1` assumption.
- Runtime handoff, acquire, frame-slot, and teardown waits are finite. `LSFG_VK_WAIT_TIMEOUT_MS` controls the wait budget and defaults to 250 ms (clamped to 5 seconds). A timeout degrades the wrapper to native presentation rather than waiting indefinitely.

These fallbacks are capability-triggered. Devices that pass the direct-storage, synchronization, WSI, and blit probes remain on the existing direct/high-performance path.

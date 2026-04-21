# lsfg-vk

Lossless Scaling is a Windows-exclusive app with the goal of bringing frame generation (among other features) to every single game or app.

lsfg-vk brings this frame generation to Linux users by acting as a Vulkan layer inbetween your game and your graphics card.

> [!TIP]
> **This is a pre-release**. We are still ironing out the last few issues before finally releasing a first version, so beware of any issues you encounter on the way and report them via GitHub issues or the Discord.

## Installation (Linux)

lsfg-vk can run on a variety of Linux distributions:
- Click [here](https://github.com/PancakeTAS/lsfg-vk/releases) to download lsfg-vk for your distribution
- Follow [this guide](https://github.com/PancakeTAS/lsfg-vk/wiki/Installation-Guide) if you any more help.

Once installed, open up the lsfg-vk Configuration Window which should hopefully appear in your application menu.

Please see the [Wiki](https://github.com/PancakeTAS/lsfg-vk/wiki) for more information and join the [Discord](https://discord.gg/losslessscaling) for help (needs Steam verification).

## Android / GameNative (Wine-on-Android)

lsfg-vk can also be cross-compiled for the [GameNative](https://github.com/gamenative) Android emulator, which runs Windows games via Wine/Proton + DXVK. Build with `ANDROID_NDK=/path/to/ndk ./scripts/build/android.sh` (needs NDK r25+). The script enables the `LSFGVK_ANDROID_WINE` CMake option, skips the GTK4 UI, and produces `build-android/liblsfg-vk.so` + the manifest JSON ready to drop into GameNative's `app/src/main/assets/lsfg-vk/`.

On that target the layer honors three extra env hooks set by the launcher: `LSFG_DLL_PATH_UNIX` (direct Unix path to `Lossless.dll` inside the Wine prefix), `WINEPREFIX` (fallback — probes `drive_c/Program Files (x86)/Steam/steamapps/common/Lossless Scaling/Lossless.dll`), and `LSFG_PROCESS_EXE` (the Windows .exe name so per-game TOML matching still works under Wine). The existing `LSFG_LEGACY=1` env-driven config path is used — no TOML file is required.

## Native Android (no Wine)

This fork carries **Android-specific patches on top of upstream framegen**, used by the sideload-only [`LSFG-Android`](../LSFG-Android/) app one directory above. The patches are guarded by `#ifdef __ANDROID__` and have no effect on Linux/Wine builds. Summary:

| Symbol | Where | What it adds |
|---|---|---|
| `LSFG_3_1::createContextFromAHB` / `LSFG_3_1P::createContextFromAHB` | `framegen/public/lsfg_3_1{,p}.hpp`, `framegen/v3.1{,p}_src/lsfg.cpp` | Public Android API: takes `AHardwareBuffer*` instead of opaque file descriptors. Adreno/Mali drivers refuse `vkGetMemoryFdKHR` on AHB-imported memory, so the FD path that works on Linux is unusable on Android. |
| `Image::Image(..., AHardwareBuffer*)` | `framegen/include/core/image.hpp`, `framegen/src/core/image.cpp` | Imports an AHB into a `VkImage` via `VK_ANDROID_external_memory_android_hardware_buffer` + `VkImportAndroidHardwareBufferInfoANDROID`, with dedicated allocation. |
| `Context::Context(..., AHardwareBuffer*, ...)` | `framegen/v3.1{,p}_include/v3_1{,p}/context.hpp`, `framegen/v3.1{,p}_src/context.cpp` | Mirror of the FD constructor, building the same shader chain but seeding inputs/outputs from caller-provided AHBs. |
| `Generate(..., std::vector<Core::Image> outImgs)` | `framegen/v3.1{,p}_include/v3_1{,p}/shaders/generate.hpp`, `framegen/v3.1{,p}_src/shaders/generate.cpp` | Variant that accepts pre-built output images instead of allocating from FDs. Used by the AHB context. |
| `LSFG_3_1::waitIdle()` / `LSFG_3_1P::waitIdle()` | `framegen/public/lsfg_3_1{,p}.hpp`, `framegen/v3.1{,p}_src/lsfg.cpp` | Exposes `vkDeviceWaitIdle` on framegen's internal device, so an external Vulkan session sharing AHBs can synchronize without a cross-device semaphore (which the spec doesn't define). |
| Device extension list, Android variant | `framegen/src/core/device.cpp` | Replaces `VK_KHR_external_memory_fd` + `VK_KHR_external_semaphore_fd` with the AHB extension chain (`VK_ANDROID_external_memory_android_hardware_buffer`, `VK_KHR_external_memory`, `VK_KHR_sampler_ycbcr_conversion`, `VK_KHR_dedicated_allocation`, `VK_KHR_get_memory_requirements2`, `VK_KHR_bind_memory2`, `VK_KHR_maintenance1`). `VK_EXT_robustness2` stays required on both targets. |

These patches are **only** invoked when the caller uses the new `*FromAHB` entry points; the original FD-based API is untouched and Linux/Wine builds behave identically to upstream.

The Android build doesn't run from this directory — the consumer is the Android Studio project at [`../LSFG-Android/`](../LSFG-Android/), which pulls `framegen/` in via CMake `add_subdirectory()`. See that project's [README](../LSFG-Android/README.md) for the build flow.

## Credits

Most of the project has still only been written by me, PancakeTAS, but I couldn't have done it without the help of these people:
- [0xNULLderef](https://github.com/0xNULLderef): Teaching me how to reverse engineer software.
- [Caliel666](https://github.com/Caliel666): Writing the initial draft of the user interface.
- [Samueru-sama](https://github.com/Samueru-sama): Helping with various things XDG as well as app images and testing.
- Other contributors: Thank you for your contribution!

I'd also like to thank every single person sponsoring this project. Thanks to you I'll be able to invest more time into this and hopefully bring some cool new features to everyone.

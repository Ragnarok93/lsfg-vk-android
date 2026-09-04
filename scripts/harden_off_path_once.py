#!/usr/bin/env python3
from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1))


hooks = Path("src/hooks.cpp")
replace_once(hooks, '''#ifdef __ANDROID__
        // AHB is required by this Android exchange path, but LSFG must never
        // turn a missing optional capability into failure of the game's own
        // Vulkan device. Probe first and fail open to the unmodified create info.
        const bool ahbSupported = supportsDeviceExtension(physicalDevice,
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
''', '''#ifdef __ANDROID__
        // Preserve the application's VkDevice contract exactly when frame
        // generation is disabled at process launch. Enabling later requires a
        // process/device restart instead of retrofitting AHB into a live device.
        const bool frameGenDeviceFeaturesEnabled =
            Config::activeConf.enable && Config::activeConf.multiplier > 1.0;
        if (!frameGenDeviceFeaturesEnabled) {
            std::cerr << "lsfg-vk: init stage=device-pass-through reason=disabled; "
                         "preserving game device extensions\\n";
            return Layer::ovkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
        }

        // AHB is required only by the active Android frame-generation exchange
        // path. Missing capability fails open to untouched game device creation.
        const bool ahbSupported = supportsDeviceExtension(physicalDevice,
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
''')
replace_once(hooks, '''#ifdef __ANDROID__
        const bool androidAhbSupported = supportsDeviceExtension(physicalDevice,
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
#else
''', '''#ifdef __ANDROID__
        const bool frameGenDeviceFeaturesEnabled =
            Config::activeConf.enable && Config::activeConf.multiplier > 1.0;
        const bool androidAhbSupported = frameGenDeviceFeaturesEnabled
            && supportsDeviceExtension(physicalDevice,
                VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
#else
''')

Path("tests/android_off_path_contract_test.py").write_text(r'''#!/usr/bin/env python3
from pathlib import Path
hooks = Path("src/hooks.cpp").read_text()
pre = hooks.split("VkResult myvkCreateDevicePre(", 1)[1].split("VkResult myvkCreateDevicePost(", 1)[0]
assert "frameGenDeviceFeaturesEnabled" in pre
assert "device-pass-through reason=disabled" in pre
assert pre.index("if (!frameGenDeviceFeaturesEnabled)") < pre.index("supportsDeviceExtension(physicalDevice")
assert "return Layer::ovkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);" in pre
post = hooks.split("VkResult myvkCreateDevicePost(", 1)[1].split("VkPresentModeKHR choosePresentMode(", 1)[0]
assert "const bool androidAhbSupported = frameGenDeviceFeaturesEnabled" in post
swap = hooks.split("VkResult myvkCreateSwapchainKHR(", 1)[1].split("VkResult myvkQueuePresentKHR(", 1)[0]
assert 'return createPassThrough("disabled");' in swap
pt = swap.split("const auto createPassThrough", 1)[1].split('if (!activeConf.enable || activeConf.multiplier <= 1)', 1)[0]
assert "device, pCreateInfo, pAllocator, pSwapchain" in pt
assert "effectivePresentMode = pCreateInfo->presentMode" in pt
assert "effectiveMinImageCount = pCreateInfo->minImageCount" in pt
present = hooks.split("VkResult myvkQueuePresentKHR(", 1)[1].split("void myvkDestroySwapchainKHR(", 1)[0]
guard = "if (!conf.enable || conf.multiplier <= 1)\n            return Layer::ovkQueuePresentKHR(queue, pPresentInfo);"
assert guard in present
assert present.index(guard) < present.index("sourceFramePacer.configure")
assert present.index(guard) < present.index("VkSwapchainPresentModeInfoEXT")
print("Android strict-OFF Vulkan contract: PASS")
''')
print("Applied strict LSFG OFF-path hardening")

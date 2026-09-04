#!/usr/bin/env python3
from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1))


hooks = Path("src/hooks.cpp")
replace_once(
    hooks,
    '''#ifdef __ANDROID__
        // AHB is required by this Android exchange path, but LSFG must never
        // turn a missing optional capability into failure of the game's own
        // Vulkan device. Probe first and fail open to the unmodified create info.
        const bool ahbSupported = supportsDeviceExtension(physicalDevice,
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
        if (!ahbSupported) {
            std::cerr << "lsfg-vk: init stage=ahb-extension-unavailable; "
                         "creating game device without LSFG AHB augmentation\\n";
            return Layer::ovkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
        }
        auto extensions = Utils::addExtensions(
            pCreateInfo->ppEnabledExtensionNames,
            pCreateInfo->enabledExtensionCount,
            { VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME }
        );
#else
''',
    '''#ifdef __ANDROID__
        // Device creation is the earliest point where the layer can materially
        // alter the game's Vulkan contract. If frame generation is disabled at
        // process launch, preserve the application's extension list exactly.
        // A later enable request therefore requires a process restart rather
        // than retroactively changing an already-created VkDevice.
        const bool frameGenDeviceFeaturesEnabled =
            Config::activeConf.enable && Config::activeConf.multiplier > 1.0;
        if (!frameGenDeviceFeaturesEnabled) {
            std::cerr << "lsfg-vk: init stage=device-pass-through reason=disabled; "
                         "preserving game device extensions\\n";
            return Layer::ovkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
        }

        // AHB is required only while the Android frame-generation exchange path
        // is active. Missing optional capability must fail open to the game's
        // untouched device creation rather than breaking native presentation.
        const bool ahbSupported = supportsDeviceExtension(physicalDevice,
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
        if (!ahbSupported) {
            std::cerr << "lsfg-vk: init stage=ahb-extension-unavailable; "
                         "creating game device without LSFG AHB augmentation\\n";
            return Layer::ovkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
        }
        auto extensions = Utils::addExtensions(
            pCreateInfo->ppEnabledExtensionNames,
            pCreateInfo->enabledExtensionCount,
            { VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME }
        );
#else
''')

replace_once(
    hooks,
    '''#ifdef __ANDROID__
        const bool androidAhbSupported = supportsDeviceExtension(physicalDevice,
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
#else
        const bool androidAhbSupported = true;
#endif
''',
    '''#ifdef __ANDROID__
        // Mark the exchange path usable only when this VkDevice was created for
        // an active frame-generation session. A layer that started disabled
        // must not begin AHB work after a hot config change on the same device.
        const bool frameGenDeviceFeaturesEnabled =
            Config::activeConf.enable && Config::activeConf.multiplier > 1.0;
        const bool androidAhbSupported = frameGenDeviceFeaturesEnabled
            && supportsDeviceExtension(physicalDevice,
                VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
#else
        const bool androidAhbSupported = true;
#endif
''')

Path("tests/android_off_path_contract_test.py").write_text(r'''#!/usr/bin/env python3
from pathlib import Path

hooks = Path("src/hooks.cpp").read_text()

device_pre = hooks.split("VkResult myvkCreateDevicePre(", 1)[1].split(
    "VkResult myvkCreateDevicePost(", 1
)[0]
assert "frameGenDeviceFeaturesEnabled" in device_pre
assert "Config::activeConf.enable && Config::activeConf.multiplier > 1.0" in device_pre
assert "device-pass-through reason=disabled" in device_pre
assert "return Layer::ovkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);" in device_pre
assert device_pre.index("if (!frameGenDeviceFeaturesEnabled)") < device_pre.index(
    "supportsDeviceExtension(physicalDevice"
)

device_post = hooks.split("VkResult myvkCreateDevicePost(", 1)[1].split(
    "VkPresentModeKHR choosePresentMode(", 1
)[0]
assert "const bool androidAhbSupported = frameGenDeviceFeaturesEnabled" in device_post

swapchain = hooks.split("VkResult myvkCreateSwapchainKHR(", 1)[1].split(
    "VkResult myvkQueuePresentKHR(", 1
)[0]
assert 'return createPassThrough("disabled");' in swapchain
pass_through = swapchain.split("const auto createPassThrough", 1)[1].split(
    'if (!activeConf.enable || activeConf.multiplier <= 1)', 1
)[0]
assert "Layer::ovkCreateSwapchainKHR(\n                device, pCreateInfo, pAllocator, pSwapchain)" in pass_through
assert "effectivePresentMode = pCreateInfo->presentMode" in pass_through
assert "effectiveMinImageCount = pCreateInfo->minImageCount" in pass_through

present = hooks.split("VkResult myvkQueuePresentKHR(", 1)[1].split(
    "void myvkDestroySwapchainKHR(", 1
)[0]
disabled_guard = "if (!conf.enable || conf.multiplier <= 1)\n            return Layer::ovkQueuePresentKHR(queue, pPresentInfo);"
assert disabled_guard in present
assert present.index(disabled_guard) < present.index("sourceFramePacer.configure")
assert present.index(disabled_guard) < present.index("VkSwapchainPresentModeInfoEXT")

print("Android strict-OFF Vulkan contract: PASS")
''')

print("Applied strict LSFG OFF-path hardening and contract test")

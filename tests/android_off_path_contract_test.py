#!/usr/bin/env python3
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

runtime_apply = hooks.split("PendingConfigAction applyPendingRuntimeConfig()", 1)[1].split(
    "bool applyPendingConfigForSwapchainCreation()", 1
)[0]
assert "const bool nextNeedsFrameGeneration" in runtime_apply
assert "if (!nextNeedsFrameGeneration)" in runtime_apply
disable_apply = runtime_apply.split("if (!nextNeedsFrameGeneration)", 1)[1]
assert "Config::activeConf = *state.pendingConfig;" in disable_apply
assert "state.appliedConf = Config::activeConf;" in disable_apply
assert "state.pendingConfig.reset();" in disable_apply
assert "state.configPending.store(false, std::memory_order_release);" in disable_apply

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
transition = present.split("if (pendingAction == PendingConfigAction::Recreate)", 1)[1].split(
    "#endif", 1
)[0]
assert "Layer::ovkQueuePresentKHR(queue, pPresentInfo);" in transition
assert "eraseSwapchainState(*pPresentInfo->pSwapchains);" in transition
assert transition.index("Layer::ovkQueuePresentKHR(queue, pPresentInfo);") < transition.index(
    "eraseSwapchainState(*pPresentInfo->pSwapchains);"
)
assert transition.index("eraseSwapchainState(*pPresentInfo->pSwapchains);") < transition.index(
    "return VK_ERROR_OUT_OF_DATE_KHR;"
)

disabled_guard = "if (!conf.enable || conf.multiplier <= 1)\n            return Layer::ovkQueuePresentKHR(queue, pPresentInfo);"
assert disabled_guard in present
assert present.index(disabled_guard) < present.index("sourceFramePacer.configure")
assert present.index(disabled_guard) < present.index("VkSwapchainPresentModeInfoEXT")

print("Android strict-OFF Vulkan contract: PASS")

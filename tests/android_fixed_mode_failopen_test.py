from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AndroidFixedModeFailOpenTest(unittest.TestCase):
    def test_context_creation_failure_recreates_original_swapchain(self) -> None:
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        marker = source.index("init stage=ls-context-failed")
        return_pos = source.index("return VK_SUCCESS;", marker)
        fallback = source[marker:return_pos]

        self.assertIn("VkSwapchainCreateInfoKHR fallbackCreateInfo = *pCreateInfo;", fallback)
        self.assertIn("fallbackCreateInfo.oldSwapchain = failedSwapchain;", fallback)
        self.assertIn("VkSwapchainKHR fallbackSwapchain = VK_NULL_HANDLE;", fallback)
        self.assertIn("device, &fallbackCreateInfo, pAllocator, &fallbackSwapchain", fallback)
        create_pos = fallback.index("&fallbackCreateInfo")
        destroy_pos = fallback.index("ovkDestroySwapchainKHR(device, failedSwapchain")
        self.assertLess(create_pos, destroy_pos)
        self.assertIn("eraseSwapchainState(failedSwapchain)", fallback)
        self.assertIn("*pSwapchain = fallbackSwapchain;", fallback)
        self.assertIn("swapchain-fallback-pass-through", fallback)
        self.assertIn("swapchainToDeviceTable.emplace(*pSwapchain, device)", fallback)


if __name__ == "__main__":
    unittest.main()

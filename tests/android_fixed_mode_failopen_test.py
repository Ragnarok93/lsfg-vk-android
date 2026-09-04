from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AndroidFixedModeFailOpenTest(unittest.TestCase):
    def test_context_creation_failure_recreates_original_swapchain(self) -> None:
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        marker = source.index("init stage=ls-context-failed")
        return_pos = source.index("return VK_SUCCESS;", marker)
        fallback = source[marker:return_pos]

        self.assertIn("ovkDestroySwapchainKHR", fallback)
        self.assertIn("eraseSwapchainState", fallback)
        self.assertIn("device, pCreateInfo, pAllocator, pSwapchain", fallback)
        self.assertIn("swapchain-fallback-pass-through", fallback)
        self.assertIn("swapchainToDeviceTable.emplace", fallback)


if __name__ == "__main__":
    unittest.main()

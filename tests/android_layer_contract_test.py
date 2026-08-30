#!/usr/bin/env python3
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class AndroidLayerContractTest(unittest.TestCase):
    def test_desktop_fd_exports_are_not_mandatory_on_android(self) -> None:
        source = (ROOT / "src/layer.cpp").read_text(encoding="utf-8")

        non_android = re.search(
            r"#ifndef __ANDROID__\s+"
            r"(?P<body>.*?)"
            r"#endif",
            source,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(
            non_android,
            "Desktop FD-export function initialization must be isolated behind #ifndef __ANDROID__",
        )
        body = non_android.group("body")
        self.assertIn('"vkGetMemoryFdKHR"', body)
        self.assertIn('"vkGetSemaphoreFdKHR"', body)

        outside = source[: non_android.start()] + source[non_android.end() :]
        self.assertNotIn(
            'success &= initDeviceFunc(*pDevice, "vkGetMemoryFdKHR"',
            outside,
            "Android must not fail device-layer initialization when vkGetMemoryFdKHR is absent",
        )
        self.assertNotIn(
            'success &= initDeviceFunc(*pDevice, "vkGetSemaphoreFdKHR"',
            outside,
            "Android must not fail device-layer initialization when vkGetSemaphoreFdKHR is absent",
        )

    def test_swapchain_and_present_hooks_emit_first_boundary_diagnostics(self) -> None:
        source = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        for diagnostic in (
            "lsfg-vk: vkCreateSwapchainKHR hook entered",
            "lsfg-vk: vkCreateSwapchainKHR next returned ",
            "lsfg-vk: vkQueuePresentKHR hook entered",
        ):
            self.assertIn(diagnostic, source)


if __name__ == "__main__":
    unittest.main()

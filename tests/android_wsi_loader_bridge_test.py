#!/usr/bin/env python3
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidWsiLoaderBridgeContractTest(unittest.TestCase):
    def test_manifest_routes_loader_procaddr_through_diagnostic_bridge(self) -> None:
        manifest = json.loads((ROOT / "VkLayer_LS_frame_generation.json").read_text(encoding="utf-8"))
        functions = manifest["layer"]["functions"]
        self.assertEqual(functions["vkGetInstanceProcAddr"], "lsfg_vkGetInstanceProcAddrDiagnostic")
        self.assertEqual(functions["vkGetDeviceProcAddr"], "lsfg_vkGetDeviceProcAddrDiagnostic")

    def test_bridge_only_wraps_lsfg_wsi_hooks_and_has_staged_breadcrumbs(self) -> None:
        source = (ROOT / "src/android_wsi_loader_bridge.cpp").read_text(encoding="utf-8")
        for token in (
            'Hooks::hooks.find("vkCreateSwapchainKHR")',
            'Hooks::hooks.find("vkQueuePresentKHR")',
            "resolved == swapchainHook->second",
            "resolved == presentHook->second",
            "runtime stage=wsi-hook-resolved resolver=gipa command=vkCreateSwapchainKHR",
            "runtime stage=wsi-hook-resolved resolver=gipa command=vkQueuePresentKHR",
            "runtime stage=wsi-hook-resolved resolver=gdpa command=vkCreateSwapchainKHR",
            "runtime stage=wsi-hook-resolved resolver=gdpa command=vkQueuePresentKHR",
            "runtime stage=swapchain-dispatch-enter",
            "runtime stage=present-hook-enter",
        ):
            self.assertIn(token, source)

        self.assertIn("std::atomic<PFN_vkCreateSwapchainKHR>", source)
        self.assertIn("std::atomic<PFN_vkQueuePresentKHR>", source)
        self.assertIn("compare_exchange_strong", source)


if __name__ == "__main__":
    unittest.main()

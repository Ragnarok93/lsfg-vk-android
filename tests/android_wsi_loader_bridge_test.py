#!/usr/bin/env python3
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidWsiLoaderBridgeContractTest(unittest.TestCase):
    def test_manifest_uses_production_layer_procaddr_entrypoints(self) -> None:
        manifest = json.loads((ROOT / "VkLayer_LS_frame_generation.json").read_text(encoding="utf-8"))
        functions = manifest["layer"]["functions"]
        self.assertEqual(functions["vkGetInstanceProcAddr"], "layer_vkGetInstanceProcAddr")
        self.assertEqual(functions["vkGetDeviceProcAddr"], "layer_vkGetDeviceProcAddr")
        self.assertNotIn("Diagnostic", functions["vkGetInstanceProcAddr"])
        self.assertNotIn("Diagnostic", functions["vkGetDeviceProcAddr"])

    def test_production_dispatch_path_retains_wsi_resolution_breadcrumbs(self) -> None:
        layer = (ROOT / "src/layer_android.cpp").read_text(encoding="utf-8")
        for token in (
            'logPresentationHookResolution("gipa", name)',
            'logPresentationHookResolution("gdpa", name)',
            '"vkCreateSwapchainKHR"',
            '"vkQueuePresentKHR"',
            '"-hook-resolved name="',
        ):
            self.assertIn(token, layer)

        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        self.assertIn("init stage=swapchain-hook-enter", hooks)
        self.assertIn("runtime stage=present-hook-enter", hooks)

    def test_diagnostic_bridge_is_not_required_by_manifest(self) -> None:
        source = (ROOT / "src/android_wsi_loader_bridge.cpp").read_text(encoding="utf-8")
        self.assertIn("lsfg_vkGetInstanceProcAddrDiagnostic", source)
        self.assertIn("lsfg_vkGetDeviceProcAddrDiagnostic", source)
        self.assertIn("runtime stage=wsi-hook-resolved", source)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidWsiLoaderBridgeContractTest(unittest.TestCase):
    def test_manifest_uses_android_loader_compatible_api13_and_production_procaddr(self) -> None:
        manifest = json.loads((ROOT / "VkLayer_LS_frame_generation.json").read_text(encoding="utf-8"))
        layer_manifest = manifest["layer"]
        functions = layer_manifest["functions"]

        # GameNative's known-good Xclipse/Proton run used an API 1.3 layer
        # manifest even though the application requested Vulkan 1.4.  With the
        # otherwise-equivalent 1.4.313 manifest, the loader resolves our GDPA
        # WSI hooks but the effective device dispatch bypasses them.  Keep the
        # layer's advertised API at the minimum API the implementation actually
        # requires so Android/Wine loaders retain the proven WSI dispatch path.
        self.assertEqual(layer_manifest["api_version"], "1.3.0")
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

        context = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.assertIn("runtime stage=first-present-enter", context)
        self.assertIn("runtime stage=first-present-cycle-ready", context)

    def test_android_runtime_metrics_cover_output_rate_latency_and_failures(self) -> None:
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")

        for token in (
            "windowSourceFrames",
            "windowGeneratedFrames",
            "windowSourcePresentFailures",
            "windowGeneratedPresentFailures",
            "windowCycleMs",
            "windowHandoffMs",
            "windowDispatchMs",
            "windowWaitIdleMs",
            "windowGeneratedPresentMs",
            "windowSourceIntervalMs",
        ):
            self.assertIn(token, header)

        for token in (
            '"lsfg-vk: metrics"',
            '" source_fps="',
            '" generated_fps="',
            '" output_fps="',
            '" source_frames_total="',
            '" generated_frames_total="',
            '" source_present_failures_total="',
            '" generated_present_failures_total="',
            '" cycle_avg_ms="',
            '" cycle_max_ms="',
            '" ahb_handoff_avg_ms="',
            '" framegen_dispatch_avg_ms="',
            '" framegen_wait_avg_ms="',
            '" generated_present_avg_ms="',
            '" source_interval_avg_ms="',
            '" source_interval_max_ms="',
        ):
            self.assertIn(token, source)

        # Telemetry must count only successful WSI submissions, and failures
        # must be recorded before the existing exceptions propagate.
        self.assertIn("metrics.windowGeneratedFrames++", source)
        self.assertIn("metrics.totalGeneratedFrames++", source)
        self.assertIn("metrics.windowSourceFrames++", source)
        self.assertIn("metrics.totalSourceFrames++", source)
        self.assertIn("metrics.windowGeneratedPresentFailures++", source)
        self.assertIn("metrics.windowSourcePresentFailures++", source)

    def test_runtime_disable_recreates_a_tracked_pass_through_swapchain(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        # Quick Menu intentionally keeps the layer resident while writing
        # enabled=false with multiplier=2.  The WSI hooks therefore have to
        # honor enable independently from multiplier, preserve the game's
        # original swapchain, and retain just enough tracking to notice a later
        # config change that turns LSFG back on.
        self.assertIn("if (!activeConf.enable || activeConf.multiplier <= 1)", hooks)
        self.assertIn("init stage=swapchain-pass-through enabled=", hooks)
        self.assertIn("swapchainToDeviceTable.emplace(*pSwapchain, device)", hooks)
        self.assertIn("if (!conf.enable || conf.multiplier <= 1)", hooks)

        reload_pos = hooks.index("init stage=config-reloaded multiplier=")
        disable_pos = hooks.index("if (!conf.enable || conf.multiplier <= 1)")
        context_lookup_pos = hooks.index("auto it3 = swapchains.find")
        self.assertLess(reload_pos, disable_pos)
        self.assertLess(disable_pos, context_lookup_pos)

    def test_diagnostic_bridge_is_not_required_by_manifest(self) -> None:
        source = (ROOT / "src/android_wsi_loader_bridge.cpp").read_text(encoding="utf-8")
        self.assertIn("lsfg_vkGetInstanceProcAddrDiagnostic", source)
        self.assertIn("lsfg_vkGetDeviceProcAddrDiagnostic", source)
        self.assertIn("runtime stage=wsi-hook-resolved", source)


if __name__ == "__main__":
    unittest.main()

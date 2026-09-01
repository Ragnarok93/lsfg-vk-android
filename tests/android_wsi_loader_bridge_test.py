#!/usr/bin/env python3
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidWsiLoaderBridgeContractTest(unittest.TestCase):
    def test_manifest_uses_production_procaddr_entrypoints(self) -> None:
        manifest = json.loads(
            (ROOT / "share/VkLayer_LS_frame_generation.json").read_text(encoding="utf-8")
        )["layer"]
        self.assertEqual("1.3.0", manifest["api_version"])
        self.assertEqual("layer_vkGetInstanceProcAddr", manifest["functions"]["vkGetInstanceProcAddr"])
        self.assertEqual("layer_vkGetDeviceProcAddr", manifest["functions"]["vkGetDeviceProcAddr"])

    def test_android_bridge_exports_diagnostic_shims_without_replacing_production_entrypoints(self) -> None:
        source = (ROOT / "src/android_wsi_loader_bridge.cpp").read_text(encoding="utf-8")
        self.assertIn('extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL\nlsfg_vkGetInstanceProcAddrDiagnostic', source)
        self.assertIn('extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL\nlsfg_vkGetDeviceProcAddrDiagnostic', source)
        self.assertIn("layer_vkGetInstanceProcAddr", source)
        self.assertIn("layer_vkGetDeviceProcAddr", source)

    def test_build_keeps_android_bridge_in_production_link_set(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("src/android_wsi_loader_bridge.cpp", cmake)

    def test_device_dispatch_is_keyed_per_device(self) -> None:
        source = (ROOT / "src/layer.cpp").read_text(encoding="utf-8")
        self.assertIn("std::unordered_map<void*, DeviceDispatchTable>", source)
        self.assertIn("deviceDispatchTables", source)
        self.assertNotIn("static PFN_vkGetDeviceProcAddr ovkGetDeviceProcAddr = nullptr", source)

    def test_early_gdpa_path_can_resolve_wsi_hooks_before_device_post(self) -> None:
        source = (ROOT / "src/layer.cpp").read_text(encoding="utf-8")
        self.assertIn("getOrCreateDeviceDispatch", source)
        self.assertIn("ensureDeviceDispatch", source)
        self.assertIn('name == "vkCreateSwapchainKHR"', source)
        self.assertIn('name == "vkQueuePresentKHR"', source)

    def test_android_wsi_resolution_logs_hook_provenance(self) -> None:
        bridge = (ROOT / "src/android_wsi_loader_bridge.cpp").read_text(encoding="utf-8")
        self.assertIn("LSFG_PROVENANCE acquire", bridge)
        self.assertIn("returnedModule", bridge)
        self.assertIn("targetModule", bridge)
        self.assertIn("downstreamModule", bridge)

    def test_android_wsi_invocation_logs_hook_provenance(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        bridge = (ROOT / "src/android_wsi_loader_bridge.cpp").read_text(encoding="utf-8")
        self.assertIn("swapchain-hook-enter", hooks)
        self.assertIn("LSFG_PROVENANCE invoke", bridge)
        self.assertIn("vkCreateSwapchainKHR", bridge)
        self.assertIn("vkQueuePresentKHR", bridge)

    def test_runtime_present_path_has_independent_binary_semaphore_consumers(self) -> None:
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        android_start = source.index("#ifdef __ANDROID__", source.index("VkResult LsContext::present"))
        desktop_start = source.index("#else", android_start)
        android = source[android_start:desktop_start]

        # A Vulkan binary semaphore signal can satisfy only one wait. The
        # generated present and following generated/source present therefore
        # require independently signaled semaphores, matching the desktop path.
        self.assertIn("pass.prevPostCopySemaphores.at(i) = Mini::Semaphore(info.device);", android)
        self.assertIn(
            "{ pass.postCopySemaphores.at(i).handle(),\n"
            "              pass.prevPostCopySemaphores.at(i).handle() }",
            android,
        )
        self.assertIn(
            "if (i != 0) waitSemaphores.emplace_back(pass.prevPostCopySemaphores.at(i - 1).handle());",
            android,
        )
        self.assertIn("VkSemaphore lastPrevPostCopySemaphore =", android)
        self.assertNotIn("VkSemaphore lastPostCopySem =", android)
        self.assertIn("runtime stage=present-sync-ready", android)

    def test_runtime_disable_recreates_a_tracked_pass_through_swapchain(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        self.assertIn("if (!activeConf.enable || activeConf.multiplier <= 1)", hooks)
        self.assertIn("init stage=swapchain-pass-through reason=", hooks)
        self.assertIn("enabled=", hooks)
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
        manifest = json.loads(
            (ROOT / "share/VkLayer_LS_frame_generation.json").read_text(encoding="utf-8")
        )["layer"]
        self.assertNotIn("lsfg_vkGetInstanceProcAddrDiagnostic", manifest["functions"].values())
        self.assertNotIn("lsfg_vkGetDeviceProcAddrDiagnostic", manifest["functions"].values())


if __name__ == "__main__":
    unittest.main()

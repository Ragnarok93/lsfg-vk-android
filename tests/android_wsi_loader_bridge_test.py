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

    def test_android_device_dispatch_is_per_logical_device(self) -> None:
        layer = (ROOT / "src/layer_android.cpp").read_text(encoding="utf-8")

        # Vulkan's distributed-dispatch contract requires device commands to be
        # resolved for the exact logical device.  Android vendor wrappers may
        # return device-specific thunks, so a process-global GDPA/PFN table is
        # not sufficient when Proton creates helper and presentation devices.
        for token in (
            "struct DeviceDispatch",
            "deviceDispatchTables",
            "deviceDispatchKey",
            "*reinterpret_cast<void* const*>(handle)",
            "storeDeviceDispatch(*pDevice, snapshotPresentationDispatch())",
            "loadDeviceDispatch(device, &dispatch)",
            "dispatch.presentationDevice",
            "downstream(device, pName)",
            "runtime stage=device-dispatch-ready presentation=1",
        ):
            self.assertIn(token, layer)

        # The presentation dispatch snapshot must exist before the post-create
        # hook obtains VkQueue/command buffers, otherwise those dispatchable
        # handles can be initialized against a later device's function table.
        snapshot = layer.index("storeDeviceDispatch(*pDevice, snapshotPresentationDispatch())")
        post_hook = layer.index("Hooks::hooks[\"vkCreateDevicePost\"]")
        self.assertLess(snapshot, post_hook)

    def test_wsi_hooks_are_not_advertised_for_helper_devices(self) -> None:
        layer = (ROOT / "src/layer_android.cpp").read_text(encoding="utf-8")
        self.assertIn("registerPassthroughDevice(*pDevice)", layer)
        self.assertIn("!dispatch.presentationDevice", layer)
        self.assertIn("if (!downstream || !downstream(device, pName))", layer)
        self.assertIn("return nullptr;", layer)

    def test_untracked_swapchain_device_keeps_wsi_hooks_during_loader_dispatch_build(self) -> None:
        layer = (ROOT / "src/layer_android.cpp").read_text(encoding="utf-8")
        start = layer.index("PFN_vkVoidFunction layer_vkGetDeviceProcAddr")
        body = layer[start:start + 2600]

        # The Vulkan loader may query GDPA while it is still constructing a
        # logical device's dispatch table. At that instant our per-device table
        # can legitimately be empty. If we return the downstream WSI pointer in
        # that window, the loader can cache a bypass forever even though later
        # GDPA calls correctly resolve the LSFG hook. Preserve WSI interception
        # whenever the exact device reports the command as available; helper
        # devices remain safe because their downstream GDPA returns NULL for
        # disabled VK_KHR_swapchain commands.
        for token in (
            "isDeviceWsiHook(name)",
            "!tracked && isDeviceWsiHook(name)",
            "runtime stage=gdpa-untracked-wsi-hook-resolved name=",
        ):
            self.assertIn(token, body)

        early_wsi = body.index("!tracked && isDeviceWsiHook(name)")
        generic_untracked_bypass = body.index("!tracked || (!dispatch.presentationDevice")
        self.assertLess(early_wsi, generic_untracked_bypass)

    def test_device_level_wrappers_use_dispatchable_handle_table(self) -> None:
        layer = (ROOT / "src/layer_android.cpp").read_text(encoding="utf-8")
        for signature in (
            "VkResult ovkCreateSwapchainKHR(VkDevice a",
            "VkResult ovkQueuePresentKHR(VkQueue a",
            "VkResult ovkQueueSubmit(VkQueue a",
            "VkResult ovkBeginCommandBuffer(VkCommandBuffer a",
            "void ovkCmdPipelineBarrier(VkCommandBuffer a",
        ):
            start = layer.index(signature)
            body = layer[start:start + 900]
            self.assertIn("loadDeviceDispatch(a, &dispatch)", body)

    def test_diagnostic_bridge_is_not_required_by_manifest(self) -> None:
        source = (ROOT / "src/android_wsi_loader_bridge.cpp").read_text(encoding="utf-8")
        self.assertIn("lsfg_vkGetInstanceProcAddrDiagnostic", source)
        self.assertIn("lsfg_vkGetDeviceProcAddrDiagnostic", source)
        self.assertIn("runtime stage=wsi-hook-resolved", source)


if __name__ == "__main__":
    unittest.main()

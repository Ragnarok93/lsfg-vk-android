#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidInstanceDispatchRegressionTest(unittest.TestCase):
    def test_private_framegen_instance_cannot_replace_game_instance_dispatch(self) -> None:
        layer = (ROOT / "src/layer_android.cpp").read_text(encoding="utf-8")
        framegen_instance = (ROOT / "framegen/src/core/instance.cpp").read_text(encoding="utf-8")

        self.assertIn('pApplicationName = "lsfg-vk-base"', framegen_instance)
        self.assertIn('pEngineName = "lsfg-vk-base"', framegen_instance)
        for token in (
            "isPrivateFramegenInstance",
            '"lsfg-vk-base"',
            "downstreamGipa",
            "downstreamCreateInstance",
            "storePrivateInstanceDispatch(*pInstance, downstreamGipa)",
        ):
            self.assertIn(token, layer)

        create_start = layer.index("VkResult layer_vkCreateInstance")
        create_end = layer.index("VkResult layer_vkCreateDevice", create_start)
        create = layer[create_start:create_end]
        private_check = create.index("if (isPrivateFramegenInstance")
        global_gipa_assignment = create.index("next_vkGetInstanceProcAddr =")
        active_hook = create.index('Hooks::hooks["vkCreateInstance"]')
        self.assertLess(private_check, global_gipa_assignment)
        self.assertLess(private_check, active_hook)

        private_body_end = create.index("next_vkGetInstanceProcAddr =", private_check)
        private_body = create[private_check:private_body_end]
        self.assertIn("downstreamCreateInstance(pCreateInfo, pAllocator, pInstance)", private_body)
        self.assertIn("storePrivateInstanceDispatch(*pInstance, downstreamGipa)", private_body)
        self.assertNotIn("next_vkGetInstanceProcAddr =", private_body)
        self.assertNotIn('Hooks::hooks["vkCreateInstance"]', private_body)
        self.assertNotIn("private-framegen-instance-pass-through", private_body)

    def test_private_framegen_instance_stays_bypassed_for_its_entire_lifetime(self) -> None:
        layer = (ROOT / "src/layer_android.cpp").read_text(encoding="utf-8")
        for token in (
            "struct PrivateInstanceDispatch",
            "privateInstanceDispatchTables",
            "storePrivateInstanceDispatch",
            "loadPrivateInstanceDispatch",
            "erasePrivateInstanceDispatch",
            "layer_vkDestroyPrivateInstance",
        ):
            self.assertIn(token, layer)

        create_start = layer.index("VkResult layer_vkCreateInstance")
        create_end = layer.index("VkResult layer_vkCreateDevice", create_start)
        create = layer[create_start:create_end]
        private_check = create.index("if (isPrivateFramegenInstance")
        private_body_end = create.index("next_vkGetInstanceProcAddr =", private_check)
        private_body = create[private_check:private_body_end]
        self.assertIn("storePrivateInstanceDispatch(*pInstance, downstreamGipa)", private_body)

        gipa_start = layer.index("PFN_vkVoidFunction layer_vkGetInstanceProcAddr")
        gipa_end = layer.index("PFN_vkVoidFunction layer_vkGetDeviceProcAddr", gipa_start)
        gipa = layer[gipa_start:gipa_end]
        private_lookup = gipa.index("loadPrivateInstanceDispatch")
        normal_layer_lookup = gipa.index("layerFunctions.find")
        self.assertLess(private_lookup, normal_layer_lookup)
        self.assertIn('name == "vkDestroyInstance"', gipa)
        self.assertIn("privateDispatch.GetInstanceProcAddr(instance, pName)", gipa)

        destroy_start = layer.index("void layer_vkDestroyPrivateInstance")
        destroy_end = layer.index("VkResult layer_vkCreateInstance", destroy_start)
        destroy = layer[destroy_start:destroy_end]
        self.assertLess(
            destroy.index("erasePrivateInstanceDispatch(instance)"),
            destroy.index("dispatch.DestroyInstance(instance, pAllocator)"),
        )
        self.assertNotIn("private-framegen-instance-destroy-pass-through", destroy)

    def test_game_instance_path_and_device_dispatch_contract_remain_unchanged(self) -> None:
        layer = (ROOT / "src/layer_android.cpp").read_text(encoding="utf-8")
        for token in (
            'Hooks::hooks["vkCreateInstance"]',
            "struct DeviceDispatch",
            "deviceDispatchTables",
            "storeDeviceDispatch(*pDevice, snapshotPresentationDispatch())",
            "loadDeviceDispatch(device, &dispatch)",
            "dispatch.presentationDevice",
            ".presentationDevice = true",
        ):
            self.assertIn(token, layer)

        snapshot = layer.index("storeDeviceDispatch(*pDevice, snapshotPresentationDispatch())")
        post_hook = layer.index('Hooks::hooks["vkCreateDevicePost"]')
        self.assertLess(snapshot, post_hook)
        self.assertNotIn("runtime stage=device-dispatch-ready presentation=1", layer)

    def test_swapchain_context_creation_cannot_finalize_other_active_contexts(self) -> None:
        context = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        ctor_start = context.index("LsContext::LsContext")
        ctor_end = context.index("LsContext::~LsContext", ctor_start)
        constructor = context[ctor_start:ctor_end]

        # The framegen backend is process-global while LsContext is per
        # swapchain. A second Vulkan instance/swapchain must not destroy the
        # first one's backend context merely because its config timestamp is
        # newer; initialize() already rejects incompatible active signatures.
        self.assertNotIn("LSFG_3_1P::finalize()", constructor)
        self.assertNotIn("LSFG_3_1::finalize()", constructor)
        self.assertIn("configuration reloaded target=", constructor)


if __name__ == "__main__":
    unittest.main()

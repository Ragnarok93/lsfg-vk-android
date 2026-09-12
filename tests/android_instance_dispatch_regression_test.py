#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidInstanceDispatchRegressionTest(unittest.TestCase):
    def test_private_framegen_instance_cannot_replace_game_instance_dispatch(self) -> None:
        layer = (ROOT / "src/layer_android.cpp").read_text(encoding="utf-8")
        framegen_instance = (ROOT / "framegen/src/core/instance.cpp").read_text(encoding="utf-8")

        # LsContext constructs a private Vulkan instance named lsfg-vk-base.
        # Because GameNative force-enables the implicit LSFG layer, that private
        # vkCreateInstance re-enters this layer. It must be passed directly to
        # the next layer before any process-global compatibility PFNs or the
        # Hooks::vkCreateInstance path can be touched.
        self.assertIn('pApplicationName = "lsfg-vk-base"', framegen_instance)
        self.assertIn('pEngineName = "lsfg-vk-base"', framegen_instance)
        for token in (
            "isPrivateFramegenInstance",
            '"lsfg-vk-base"',
            "downstreamGipa",
            "downstreamCreateInstance",
            "runtime stage=private-framegen-instance-pass-through",
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
        self.assertNotIn("next_vkGetInstanceProcAddr =", private_body)
        self.assertNotIn('Hooks::hooks["vkCreateInstance"]', private_body)

    def test_game_instance_path_and_device_dispatch_contract_remain_unchanged(self) -> None:
        layer = (ROOT / "src/layer_android.cpp").read_text(encoding="utf-8")

        # The fix is intentionally outside the steady-state presentation path.
        # Preserve the existing game-instance hook and the validated per-device
        # dispatch snapshot that eliminated helper-device/Wine WSI corruption.
        for token in (
            'Hooks::hooks["vkCreateInstance"]',
            "struct DeviceDispatch",
            "deviceDispatchTables",
            "storeDeviceDispatch(*pDevice, snapshotPresentationDispatch())",
            "loadDeviceDispatch(device, &dispatch)",
            "dispatch.presentationDevice",
            "runtime stage=device-dispatch-ready presentation=1",
        ):
            self.assertIn(token, layer)


if __name__ == "__main__":
    unittest.main()

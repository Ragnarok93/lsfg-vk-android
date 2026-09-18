#!/usr/bin/env python3
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidPrivateInstanceLayerSuppressionTest(unittest.TestCase):
    def test_manifest_exposes_disable_environment_for_private_helper(self) -> None:
        manifest = json.loads(
            (ROOT / "VkLayer_LS_frame_generation.json").read_text(encoding="utf-8")
        )
        self.assertEqual(
            manifest["layer"]["disable_environment"].get("DISABLE_LSFG"),
            "1",
        )

    def test_private_framegen_instance_suppresses_recursive_lsfg_load(self) -> None:
        source = (ROOT / "framegen/src/core/instance.cpp").read_text(encoding="utf-8")

        for token in (
            "privateInstanceEnvironmentMutex",
            "ScopedPrivateInstanceLayerSuppression",
            '"DISABLE_LSFG"',
            '"VK_INSTANCE_LAYERS"',
            '"VK_LAYER_LS_frame_generation"',
            "setenv",
            "unsetenv",
            "restoreEnvironment",
        ):
            self.assertIn(token, source)

        ctor = source.index("Instance::Instance()")
        create = source.index("vkCreateInstance(&createInfo", ctor)
        suppression = source.index("ScopedPrivateInstanceLayerSuppression", ctor)
        self.assertLess(suppression, create)

    def test_android_layer_keeps_private_instance_passthrough_as_fallback(self) -> None:
        layer = (ROOT / "src/layer_android.cpp").read_text(encoding="utf-8")
        for token in (
            "isPrivateFramegenInstance",
            '"lsfg-vk-base"',
            "storePrivateInstanceDispatch(*pInstance, downstreamGipa)",
            "layer_vkDestroyPrivateInstance",
        ):
            self.assertIn(token, layer)


if __name__ == "__main__":
    unittest.main()

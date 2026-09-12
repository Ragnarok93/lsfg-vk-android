#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidInstanceDispatchRegressionTest(unittest.TestCase):
    def test_android_instance_dispatch_is_per_instance_and_physical_device(self) -> None:
        layer = (ROOT / "src/layer_android.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/layer.hpp").read_text(encoding="utf-8")
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")

        # Crisis Core creates and destroys LSFG's private framegen Vulkan
        # instance while the game's Vulkan instance remains live. Instance-level
        # dispatch must therefore be owned by the exact instance/physical device,
        # just like device-level dispatch already is. A process-global instance
        # handle or PFN table can be overwritten by the private framegen instance
        # and later queried after that private instance has been destroyed.
        for token in (
            "struct InstanceDispatch",
            "instanceDispatchTables",
            "instanceDispatchKey",
            "storeInstanceDispatch",
            "loadInstanceDispatch",
            "eraseInstanceDispatchKey",
            "ovkGetPhysicalDeviceProcAddr",
        ):
            self.assertIn(token, layer + header)

        self.assertNotIn("static VkInstance layerInstance{}", hooks)
        self.assertNotIn("ovkGetInstanceProcAddr(layerInstance", hooks)

        # Every capability query used while constructing/recreating an LSFG
        # swapchain must resolve through the physical device's owning instance,
        # not whichever VkInstance happened to be created most recently.
        self.assertGreaterEqual(
            hooks.count("Layer::ovkGetPhysicalDeviceProcAddr(physicalDevice"),
            4,
        )

    def test_instance_lifetime_cleanup_cannot_leave_stale_dispatch(self) -> None:
        layer = (ROOT / "src/layer_android.cpp").read_text(encoding="utf-8")

        destroy_start = layer.index("void ovkDestroyInstance")
        destroy_body = layer[destroy_start:destroy_start + 1200]
        self.assertIn("loadInstanceDispatch", destroy_body)
        self.assertIn("eraseInstanceDispatchKey", destroy_body)

        # Physical-device wrappers used after LSFG's private instance teardown
        # must also select the matching instance dispatch table.
        for signature in (
            "void ovkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice a",
            "void ovkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice a",
            "void ovkGetPhysicalDeviceProperties(VkPhysicalDevice a",
            "VkResult ovkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice a",
        ):
            start = layer.index(signature)
            body = layer[start:start + 900]
            self.assertIn("loadInstanceDispatch(a, &dispatch)", body)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidFixedModeRecreateContractTest(unittest.TestCase):
    def test_old_swapchain_state_is_retired_only_after_replacement_succeeds(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        create_start = hooks.index("VkResult myvkCreateSwapchainKHR")
        present_start = hooks.index("VkResult myvkQueuePresentKHR", create_start)
        create = hooks[create_start:present_start]

        self.assertIn("const auto createPassThrough =", create)
        self.assertIn("if (pCreateInfo->oldSwapchain)\n                    eraseSwapchainState(pCreateInfo->oldSwapchain);", create)

        active_conf = create.index("const auto& activeConf")
        first_erase = create.index("eraseSwapchainState(pCreateInfo->oldSwapchain)")
        self.assertGreater(first_erase, active_conf)

    def test_modified_creation_fails_open_to_untouched_game_parameters(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        create_start = hooks.index("VkResult myvkCreateSwapchainKHR")
        present_start = hooks.index("VkResult myvkQueuePresentKHR", create_start)
        create = hooks[create_start:present_start]

        self.assertIn('return createPassThrough("modified-create-failed")', create)
        self.assertIn("recreatingExistingSwapchain", create)
        self.assertIn("? pCreateInfo->presentMode", create)

    def test_context_failure_uses_modified_swapchain_as_fallback_predecessor(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        create_start = hooks.index("VkResult myvkCreateSwapchainKHR")
        present_start = hooks.index("VkResult myvkQueuePresentKHR", create_start)
        create = hooks[create_start:present_start]
        catch_start = create.index("catch (const std::exception& e)")
        catch = create[catch_start:]

        self.assertIn("VkSwapchainCreateInfoKHR fallbackCreateInfo = *pCreateInfo;", catch)
        self.assertIn("fallbackCreateInfo.oldSwapchain = failedSwapchain;", catch)
        self.assertIn("VkSwapchainKHR fallbackSwapchain = VK_NULL_HANDLE;", catch)
        fallback_create = catch.index("&fallbackCreateInfo")
        destroy = catch.index("ovkDestroySwapchainKHR(device, failedSwapchain")
        self.assertLess(fallback_create, destroy)
        self.assertIn("*pSwapchain = fallbackSwapchain;", catch)

    def test_config_recreation_releases_lsfg_context_but_preserves_old_swapchain(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        present_start = hooks.index("VkResult myvkQueuePresentKHR")
        destroy_start = hooks.index("void myvkDestroySwapchainKHR", present_start)
        present = hooks[present_start:destroy_start]

        reload_start = present.index("if (shouldPollConfig")
        reload_end = present.index("if (!conf.enable", reload_start)
        reload_block = present[reload_start:reload_end]
        self.assertIn("swapchains.erase(*pPresentInfo->pSwapchains);", reload_block)
        self.assertNotIn("eraseSwapchainState(*pPresentInfo->pSwapchains);", reload_block)
        self.assertLess(
            reload_block.index("swapchains.erase(*pPresentInfo->pSwapchains);"),
            reload_block.index("Layer::ovkQueuePresentKHR(queue, pPresentInfo)"),
        )

    def test_present_mode_recreation_releases_old_lsfg_context(self) -> None:
        hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        present_start = hooks.index("VkResult myvkQueuePresentKHR")
        destroy_start = hooks.index("void myvkDestroySwapchainKHR", present_start)
        present = hooks[present_start:destroy_start]
        mode_start = present.index("if (configuredPresent != conf.e_present)")
        mode_end = present.index("try {", mode_start)
        mode_block = present[mode_start:mode_end]
        self.assertIn("swapchains.erase(*pPresentInfo->pSwapchains);", mode_block)
        self.assertNotIn("eraseSwapchainState(*pPresentInfo->pSwapchains);", mode_block)


if __name__ == "__main__":
    unittest.main()

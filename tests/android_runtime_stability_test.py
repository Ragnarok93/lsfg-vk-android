import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidRuntimeStabilityContractTest(unittest.TestCase):
    def setUp(self) -> None:
        self.hooks = (ROOT / "src/hooks.cpp").read_text(encoding="utf-8")
        self.context = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        self.header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")

    def test_present_path_does_not_poll_config_files(self) -> None:
        present_start = self.hooks.index("VkResult myvkQueuePresentKHR")
        present_end = self.hooks.index("void myvkDestroySwapchainKHR", present_start)
        present = self.hooks[present_start:present_end]
        self.assertNotIn("last_write_time", present)
        self.assertNotIn("Config::parse", present)
        self.assertNotIn("sleep_for", present)

    def test_runtime_io_worker_owns_file_polling_and_stats(self) -> None:
        self.assertIn("void runtimeIoWorker()", self.hooks)
        worker_start = self.hooks.index("void runtimeIoWorker()")
        worker_end = self.hooks.index("void startRuntimeIoWorker()", worker_start)
        worker = self.hooks[worker_start:worker_end]
        self.assertIn("last_write_time", worker)
        self.assertIn("Config::parse", worker)
        self.assertIn("sleep_for", worker)
        self.assertIn("pendingStats", worker)

    def test_runtime_io_worker_is_process_lifetime_not_swapchain_lifetime(self) -> None:
        self.assertIn("void startRuntimeIoWorker()", self.hooks)
        self.assertIn("void stopRuntimeIoWorker()", self.hooks)
        self.assertNotIn("stopRuntimeIoWorker();\n    runtimeIoState().configPath.clear()", self.hooks)

    def test_disabled_startup_is_pass_through(self) -> None:
        create_start = self.hooks.index("VkResult myvkCreateSwapchainKHR")
        create_end = self.hooks.index("VkResult myvkQueuePresentKHR", create_start)
        create = self.hooks[create_start:create_end]
        self.assertIn("reason=disabled", create)
        self.assertIn("Layer::ovkCreateSwapchainKHR", create)

    def test_sustained_off_fast_path_precedes_context_lookup(self) -> None:
        present_start = self.hooks.index("VkResult myvkQueuePresentKHR")
        present_end = self.hooks.index("void myvkDestroySwapchainKHR", present_start)
        present = self.hooks[present_start:present_end]
        off = present.index("if (!conf.enable || conf.multiplier <= 1)")
        lookup = present.index("swapchains.find", off)
        self.assertLess(off, lookup)
        self.assertIn("Layer::ovkQueuePresentKHR(queue, pPresentInfo)", present[off:lookup])

    def test_runtime_config_changes_are_staged(self) -> None:
        self.assertIn("pendingConfig", self.hooks)
        self.assertIn("configPending", self.hooks)
        self.assertIn("applyPendingRuntimeConfig", self.hooks)
        self.assertIn("applyPendingConfigForSwapchainCreation", self.hooks)

    def test_wsi_recreate_is_single_flight_and_coalesced(self) -> None:
        policy = (ROOT / "include/runtime_policy.hpp").read_text(encoding="utf-8")
        self.assertIn("class RuntimeWsiRecreateGate", policy)
        self.assertIn("compare_exchange_strong", policy)
        self.assertIn("wsiRecreateGate.inFlight()", self.hooks)
        self.assertIn("wsiRecreateGate.requestIfNeeded(true)", self.hooks)
        self.assertIn("wsiRecreateGate.onSwapchainCreation()", self.hooks)

        apply_start = self.hooks.index("PendingConfigAction applyPendingRuntimeConfig")
        apply_end = self.hooks.index("bool applyPendingConfigForSwapchainCreation", apply_start)
        apply = self.hooks[apply_start:apply_end]
        self.assertLess(
            apply.index("wsiRecreateGate.inFlight()"),
            apply.index("std::lock_guard lock(state.mutex)"),
        )

    def test_wsi_changes_restore_game_contract_before_recreate(self) -> None:
        source = self.hooks
        helper_start = source.index("bool requiresSwapchainRecreation")
        helper_end = source.index("enum class PendingConfigAction", helper_start)
        helper = source[helper_start:helper_end]
        present_start = source.index("VkResult myvkQueuePresentKHR")
        present_end = source.index("void myvkDestroySwapchainKHR", present_start)
        present = source[present_start:present_end]

        self.assertIn("previousNeedsFgWsi", helper)
        self.assertIn("nextNeedsFgWsi", helper)
        self.assertIn("previous.multiplier != next.multiplier", helper)
        self.assertIn("previous.e_present != next.e_present", helper)
        self.assertIn("PendingConfigAction::Recreate", present)
        self.assertIn("runtime stage=wsi-restore-requested", present)
        self.assertIn("if (!conf.enable || conf.multiplier <= 1)", present)
        off_fast_path = present.index("if (!conf.enable || conf.multiplier <= 1)")
        context_lookup = present.index("swapchains.find", off_fast_path)
        self.assertLess(off_fast_path, context_lookup)
        self.assertIn("gameMinImageCount", source)
        self.assertIn("effectiveMinImageCount", source)
        self.assertIn("gamePresentMode", source)
        self.assertIn("effectivePresentMode", source)

    def test_generated_and_source_presents_are_output_cadence_spaced(self) -> None:
        """Phase spacing remains, but its cadence is owned by the correct mode."""
        header = (ROOT / "include/context.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        frame_pacer = (ROOT / "include/frame_pacer.hpp").read_text(encoding="utf-8")
        policy = (ROOT / "include/runtime_policy.hpp").read_text(encoding="utf-8")

        self.assertIn('#include "output_frame_pacer.hpp"', header)
        self.assertIn('#include "runtime_policy.hpp"', source)
        self.assertIn("OutputFramePacer outputFramePacer_", header)
        self.assertIn("resolveOutputPacingTarget(", source)
        self.assertIn("conf.adaptiveFramegen, conf.fpsLimit", source)
        self.assertIn("conf.sourceFpsLimit, conf.multiplier", source)
        self.assertIn("outputFramePacer_.configure(outputPacingTarget)", source)
        self.assertNotIn("outputFramePacer_.configure(conf.fpsLimit)", source)

        self.assertIn("capacityFps = saturatingOutputRate(sourceFpsLimit, multiplier)", policy)
        self.assertIn("if (!adaptive)", policy)
        self.assertIn("return capacityFps", policy)
        self.assertIn("return std::min(adaptiveTargetFps, capacityFps)", policy)

        self.assertIn("periodRemainder_", frame_pacer)
        self.assertIn("phaseRemainder_", frame_pacer)
        self.assertIn("waitForFineOutputDeadline", source)
        generated_present = source.index("Layer::ovkQueuePresentKHR(queue, &presentInfo)")
        generated_pacing = source.rfind("paceOutputPresent", 0, generated_present)
        source_present = source.index("Layer::ovkQueuePresentKHR(queue, &finalPresentInfo)")
        source_pacing = source.rfind("paceOutputPresent", 0, source_present)
        self.assertGreater(generated_pacing, source.index("for (size_t i = 0; i < generatedFrameCount"))
        self.assertGreater(source_pacing, generated_present)


if __name__ == "__main__":
    unittest.main()

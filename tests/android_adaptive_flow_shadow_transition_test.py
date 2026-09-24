#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

BACKENDS = (
    (
        ROOT / "framegen/public/lsfg_3_1.hpp",
        ROOT / "framegen/v3.1_include/v3_1/context.hpp",
        ROOT / "framegen/v3.1_src/context.cpp",
        ROOT / "framegen/v3.1_src/lsfg.cpp",
    ),
    (
        ROOT / "framegen/public/lsfg_3_1p.hpp",
        ROOT / "framegen/v3.1p_include/v3_1p/context.hpp",
        ROOT / "framegen/v3.1p_src/context.cpp",
        ROOT / "framegen/v3.1p_src/lsfg.cpp",
    ),
)


class AndroidAdaptiveFlowShadowTransitionContractTest(unittest.TestCase):
    def test_public_android_api_exposes_prebuilt_scale_context_and_state(self) -> None:
        for public_header, _, _, backend_source in BACKENDS:
            header = public_header.read_text(encoding="utf-8")
            source = backend_source.read_text(encoding="utf-8")

            self.assertIn("createAdaptiveContextFromAHB", header, public_header.as_posix())
            self.assertIn("requestContextFlowScale", header, public_header.as_posix())
            self.assertIn("getContextFlowScaleState", header, public_header.as_posix())
            self.assertIn("getContextGpuTiming", header, public_header.as_posix())
            self.assertIn("AdaptiveFlowContextState", header, public_header.as_posix())
            self.assertIn("AdaptiveFlowGpuTiming", header, public_header.as_posix())

            self.assertIn("createAdaptiveContextFromAHB", source, backend_source.as_posix())
            self.assertIn("requestContextFlowScale", source, backend_source.as_posix())
            self.assertIn("getContextFlowScaleState", source, backend_source.as_posix())
            self.assertIn("getContextGpuTiming", source, backend_source.as_posix())

            # Fixed construction remains a separate, unchanged public path.
            self.assertIn("createContextFromAHB(", header, public_header.as_posix())
            self.assertIn("createContextFromAHB(", source, backend_source.as_posix())

    def test_context_keeps_three_frame_shadow_warmup_without_device_idle(self) -> None:
        for _, context_header, context_source, _ in BACKENDS:
            header = context_header.read_text(encoding="utf-8")
            source = context_source.read_text(encoding="utf-8")

            self.assertIn("kAdaptiveFlowHistoryFrames = 3", header, context_header.as_posix())
            self.assertIn("adaptiveFlowGraphs_", header, context_header.as_posix())
            self.assertIn("pendingFlowGraphIndex_", header, context_header.as_posix())
            self.assertIn("pendingFlowWarmupFrames_", header, context_header.as_posix())

            self.assertIn("dispatchAdaptiveFlowPreprocess", source, context_source.as_posix())
            self.assertIn("pendingFlowWarmupFrames_ + 1 < kAdaptiveFlowHistoryFrames", source,
                          context_source.as_posix())
            self.assertIn("data.cmdBuffer1, activeGraph, adaptiveFlowTimingPool", source,
                          context_source.as_posix())
            self.assertIn("shadowBudgetAvailable", source, context_source.as_posix())
            self.assertIn("shadowPreprocessEstimateMs", source, context_source.as_posix())
            self.assertIn("generationCount == 0 || shadowBudgetAvailable", source,
                          context_source.as_posix())
            self.assertIn("data.cmdBuffer1, pendingGraph", source,
                          context_source.as_posix())
            self.assertIn("generationGraphIndex = pendingIndex", source,
                          context_source.as_posix())
            self.assertIn("commitAdaptiveFlowTransition", source, context_source.as_posix())
            self.assertIn("adaptiveFlowTimingQueryPool", header, context_header.as_posix())
            self.assertIn("Core::DescriptorPool descriptorPool", header,
                          context_header.as_posix())
            self.assertIn("savedDescriptorPool_(vk.descriptorPool)", source,
                          context_source.as_posix())
            self.assertIn("vk_.descriptorPool = descriptorPool", source,
                          context_source.as_posix())
            self.assertIn("graph.descriptorPool = Core::DescriptorPool(vk.device)", source,
                          context_source.as_posix())
            self.assertIn("descriptor_pool_mode=per-state", source,
                          context_source.as_posix())
            self.assertIn("recordAdaptiveFlowGpuTiming", source, context_source.as_posix())
            self.assertIn("transitionActive = renderData.adaptiveFlowTransitionCycle", source,
                          context_source.as_posix())
            self.assertIn("timingPool->write(buffer.handle(), 1)", source,
                          context_source.as_posix())

            # The transition is recorded in the existing command/submission path.
            self.assertNotIn("vkDeviceWaitIdle", source, context_source.as_posix())

    def test_gpu_timing_is_available_for_fixed_and_adaptive_flow(self) -> None:
        for _, context_header, context_source, _ in BACKENDS:
            header = context_header.read_text(encoding="utf-8")
            source = context_source.read_text(encoding="utf-8")

            self.assertIn("adaptiveFlowTimingQueryPool", header, context_header.as_posix())
            self.assertIn(
                "data.adaptiveFlowTimingQueryPool =",
                source,
                context_source.as_posix(),
            )
            self.assertIn(
                "if (data.adaptiveFlowTimingQueryPool.supported())",
                source,
                context_source.as_posix(),
            )
            self.assertNotIn(
                "if (!this->adaptiveFlowScales_.empty()\n"
                "            && data.adaptiveFlowTimingQueryPool.supported())",
                source,
                context_source.as_posix(),
            )

            # Fixed Flow writes the same 0/1/2/3 timestamp boundaries as the
            # Adaptive Flow graph, so admission prediction has measurements in
            # both modes without changing generation policy.
            fixed_start = source.index("} else {\n        this->mipmaps.Dispatch")
            fixed_end = source.index("#else", fixed_start)
            fixed_block = source[fixed_start:fixed_end]
            self.assertIn(
                "adaptiveFlowTimingPool->write(data.cmdBuffer1.handle(), 1)",
                fixed_block,
                context_source.as_posix(),
            )
            record_start = source.index("void Context::recordAdaptiveFlowGpuTiming")
            record_end = source.index("LSFG::AdaptiveFlowGpuTiming Context::gpuTiming", record_start)
            record = source[record_start:record_end]
            self.assertIn("renderData.adaptiveFlowBatch.sessionEpoch", record,
                          context_source.as_posix())
            self.assertIn("renderData.adaptiveFlowBatch.batchId", record,
                          context_source.as_posix())
            self.assertIn("lastAdaptiveFlowGpuTiming_.valid = false", record,
                          context_source.as_posix())
            self.assertNotIn(
                "this->adaptiveFlowScales_.empty()",
                record,
                context_source.as_posix(),
            )

    def test_pending_transition_is_last_value_wins_and_cancellable(self) -> None:
        for _, _, context_source, _ in BACKENDS:
            source = context_source.read_text(encoding="utf-8")
            self.assertIn("requestFlowScale", source, context_source.as_posix())
            self.assertIn("pendingFlowWarmupFrames_ = 0", source, context_source.as_posix())
            self.assertIn("pendingFlowGraphIndex_.reset()", source, context_source.as_posix())
            self.assertIn("requestedFlowScale_", source, context_source.as_posix())


if __name__ == "__main__":
    unittest.main()

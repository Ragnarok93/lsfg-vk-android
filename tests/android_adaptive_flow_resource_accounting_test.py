#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AdaptiveFlowResourceAccountingTest(unittest.TestCase):
    def test_wrappers_count_only_native_object_construction(self) -> None:
        expected = {
            "image.cpp": "recordImageConstruction",
            "buffer.cpp": "recordBufferConstruction",
            "descriptorset.cpp": "recordDescriptorSetConstruction",
            "sampler.cpp": "recordSamplerConstruction",
            "pipeline.cpp": "recordPipelineConstruction",
            "shadermodule.cpp": "recordShaderModuleConstruction",
        }
        header = (ROOT / "framegen/include/core/resource_stats.hpp").read_text(encoding="utf-8")
        self.assertIn("struct ResourceConstructionStats", header)
        for filename, marker in expected.items():
            source = (ROOT / "framegen/src/core" / filename).read_text(encoding="utf-8")
            self.assertIn(marker, source, filename)

    def test_adaptive_prebuild_reports_only_additional_state_delta(self) -> None:
        for backend in ("v3.1_src", "v3.1p_src"):
            source = (ROOT / "framegen" / backend / "context.cpp").read_text(encoding="utf-8")
            ctor = source.index("const std::vector<float>& adaptiveFlowScales")
            request = source.index("void Context::requestFlowScale", ctor)
            block = source[ctor:request]
            self.assertIn("resourceStatsBefore", block)
            self.assertIn("snapshotResourceConstructionStats() - resourceStatsBefore", block)
            for marker in (
                "added_images=",
                "added_image_bytes=",
                "added_buffers=",
                "added_buffer_bytes=",
                "added_descriptor_sets=",
                "added_samplers=",
                "added_pipelines=",
                "added_shader_modules=",
                "prebuild_ms=",
            ):
                self.assertIn(marker, block)
            self.assertNotIn("snapshotResourceConstructionStats()", source[request:])


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AdaptiveFlowResourceSharingTest(unittest.TestCase):
    def test_sampler_cache_is_shared_but_scale_buffers_remain_per_state(self) -> None:
        header = (ROOT / "framegen/include/pool/resourcepool.hpp").read_text(encoding="utf-8")
        source = (ROOT / "framegen/src/pool/resourcepool.cpp").read_text(encoding="utf-8")
        self.assertIn("using SamplerCache =", header)
        self.assertIn("sharedSamplerCache()", header)
        self.assertIn("std::shared_ptr<SamplerCache> samplers", header)
        self.assertIn("std::unordered_map<uint64_t, Core::Buffer> buffers", header)
        self.assertIn("auto& cache = *samplers;", source)

    def test_both_adaptive_graph_builders_share_only_immutable_sampler_cache(self) -> None:
        for backend in ("v3.1_src", "v3.1p_src"):
            source = (ROOT / "framegen" / backend / "context.cpp").read_text(encoding="utf-8")
            self.assertIn(
                "const auto sharedSamplers = savedResources_.sharedSamplerCache();",
                source,
            )
            self.assertIn(
                "Pool::ResourcePool(\n            vk_.isHdr, vk_.flowScale, sharedSamplers)",
                source,
            )
            self.assertIn("descriptor_pool_mode=per-state", source)
            self.assertIn("sampler_cache=shared", source)

    def test_shader_and_pipeline_caches_remain_runtime_global(self) -> None:
        header = (ROOT / "framegen/include/pool/shaderpool.hpp").read_text(encoding="utf-8")
        self.assertIn("shaderCount() const", header)
        self.assertIn("pipelineCount() const", header)


if __name__ == "__main__":
    unittest.main()

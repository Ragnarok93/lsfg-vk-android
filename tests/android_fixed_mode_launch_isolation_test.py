#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

utils = (ROOT / "framegen/include/common/utils.hpp").read_text(encoding="utf-8")
outer = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
pool = (ROOT / "framegen/src/pool/resourcepool.cpp").read_text(encoding="utf-8")
buffer_cpp = (ROOT / "framegen/src/core/buffer.cpp").read_text(encoding="utf-8")
compact_outer = " ".join(outer.split())
benchmark = (ROOT / "src/utils/benchmark.cpp").read_text(encoding="utf-8")
compact_benchmark = " ".join(benchmark.split())

assert "bool dynamicInterpolationPhases" in utils
assert "runtimeMultiplier - 1, conf.adaptiveFramegen," in compact_outer
assert "conf.multiplier - 1, conf.adaptiveFramegen," in compact_outer
assert "if (!dynamicInterpolationPhases)" in pool
assert "return this->getBuffer" in pool
assert "return this->createTimestampRing" in pool
assert "if (!persistentlyMapped)" in buffer_cpp
assert "conf.multiplier - 1, false," in compact_benchmark

for variant in ("v3.1", "v3.1p"):
    root = ROOT / f"framegen/{variant}_src/shaders"
    for name in ("gamma.cpp", "delta.cpp", "generate.cpp"):
        source = (root / name).read_text(encoding="utf-8")
        compact = " ".join(source.split())
        assert "timestampDescriptorType" in source, (variant, name)
        assert "dynamicInterpolationPhases" in source, (variant, name)
        assert "? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER" in compact, (variant, name)
        assert "getTimestampBuffer" in source, (variant, name)
        assert "this->dynamicInterpolationPhases, timestampOffset" in compact, (variant, name)

print("Fixed-mode framegen construction is isolated from fractional dynamic UBOs")

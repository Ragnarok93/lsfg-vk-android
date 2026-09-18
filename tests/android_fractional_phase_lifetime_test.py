#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

utils_h = (ROOT / "framegen/include/common/utils.hpp").read_text(encoding="utf-8")
pool_h = (ROOT / "framegen/include/pool/resourcepool.hpp").read_text(encoding="utf-8")
pool_cpp = (ROOT / "framegen/src/pool/resourcepool.cpp").read_text(encoding="utf-8")
descriptor_h = (ROOT / "framegen/include/core/descriptorset.hpp").read_text(encoding="utf-8")
descriptor_cpp = (ROOT / "framegen/src/core/descriptorset.cpp").read_text(encoding="utf-8")
descriptor_pool_cpp = (ROOT / "framegen/src/core/descriptorpool.cpp").read_text(encoding="utf-8")
buffer_cpp = (ROOT / "framegen/src/core/buffer.cpp").read_text(encoding="utf-8")

assert "bool dynamicInterpolationPhases" in utils_h
assert "kTimestampRingSlots = 8" in pool_h
assert "getTimestampBuffer" in pool_h
assert "createTimestampRing" in pool_h
assert "timestampRingOffset" in pool_h
assert "minUniformBufferOffsetAlignment" in pool_cpp
assert "timestampRecordSize" in pool_cpp
assert "if (!dynamicInterpolationPhases)" in pool_cpp
assert "return this->getBuffer" in pool_cpp
assert "return this->createTimestampRing" in pool_cpp
assert "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC" in descriptor_pool_cpp
assert "1, &dynamicOffset" in descriptor_cpp
assert "bool useDynamicOffset" in descriptor_h
assert "if (useDynamicOffset)" in descriptor_cpp
assert "if (!persistentlyMapped)" in buffer_cpp
assert "if (persistentlyMapped)" in buffer_cpp
assert "VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true" in pool_cpp

for variant in ("v3.1", "v3.1p"):
    root = ROOT / f"framegen/{variant}_src/shaders"
    for name in ("gamma.cpp", "delta.cpp", "generate.cpp"):
        text = (root / name).read_text(encoding="utf-8")
        compact = " ".join(text.split())
        assert "dynamicInterpolationPhases" in text, (variant, name)
        assert "timestampDescriptorType" in text, (variant, name)
        assert "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC" in text, (variant, name)
        assert "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER" in text, (variant, name)
        assert "getTimestampBuffer" in text, (variant, name)
        assert "timestampRingOffset(pass.buffer, frameCount)" in text, (variant, name)
        assert "this->dynamicInterpolationPhases && interpolationPhase > 0.0F" in text, (variant, name)
        assert "writeTimestamp( pass.buffer, interpolationPhase, timestampOffset);" in compact, (variant, name)
        assert "this->dynamicInterpolationPhases, timestampOffset" in compact, (variant, name)

print("Fractional interpolation phase lifetime contract satisfied")

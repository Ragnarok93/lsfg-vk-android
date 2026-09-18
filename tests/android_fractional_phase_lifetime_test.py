#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

pool_h = (ROOT / "framegen/include/pool/resourcepool.hpp").read_text(encoding="utf-8")
pool_cpp = (ROOT / "framegen/src/pool/resourcepool.cpp").read_text(encoding="utf-8")
descriptor_h = (ROOT / "framegen/include/core/descriptorset.hpp").read_text(encoding="utf-8")
descriptor_cpp = (ROOT / "framegen/src/core/descriptorset.cpp").read_text(encoding="utf-8")
descriptor_pool_cpp = (ROOT / "framegen/src/core/descriptorpool.cpp").read_text(encoding="utf-8")
buffer_cpp = (ROOT / "framegen/src/core/buffer.cpp").read_text(encoding="utf-8")

# Explicit fractional timestamps must be ringed by the same eight-frame lifetime
# domain used by Context::RenderData. A shared mutable per-pass UBO would race
# older in-flight GPU consumers.
assert "kTimestampRingSlots = 8" in pool_h
assert "createTimestampRing" in pool_h
assert "timestampRingOffset" in pool_h
assert "minUniformBufferOffsetAlignment" in pool_cpp
assert "timestampRecordSize" in pool_cpp
assert "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC" in descriptor_pool_cpp
assert "1, &dynamicOffset" in descriptor_cpp
assert "uint32_t dynamicOffset" in descriptor_h

# Persistently mapped timestamp storage must actually require both host-visible
# and host-coherent memory; testing either bit alone is insufficient.
assert "== requiredHostFlags" in buffer_cpp

for variant in ("v3.1", "v3.1p"):
    prefix = "v3_1" if variant == "v3.1" else "v3_1p"
    root = ROOT / f"framegen/{variant}_src/shaders"
    for name in ("gamma.cpp", "delta.cpp", "generate.cpp"):
        text = (root / name).read_text(encoding="utf-8")
        assert "createTimestampRing" in text, (variant, name)
        assert "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC" in text, (variant, name)
        assert "timestampRingOffset(pass.buffer, frameCount)" in text, (variant, name)
        compact = " ".join(text.split())
        assert "writeTimestamp( pass.buffer, interpolationPhase, timestampOffset);" in compact, (variant, name)
        assert "writeTimestamp(pass.buffer, interpolationPhase);" not in text, (variant, name)
        assert "timestampRecordSize()" in text, (variant, name)

print("Fractional interpolation phase lifetime contract satisfied")

#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

utils = (ROOT / "framegen/include/common/utils.hpp").read_text(encoding="utf-8")
outer = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
compact_outer = " ".join(outer.split())

# Fractional timestamp resources are Adaptive-only. Fixed mode must retain the
# proven static UBO construction path and must not instantiate dynamic/ringed
# descriptors merely because the runtime also supports Adaptive FG.
assert "bool dynamicInterpolationPhases" in utils
assert "runtimeMultiplier - 1, conf.adaptiveFramegen," in compact_outer

for variant in ("v3.1", "v3.1p"):
    root = ROOT / f"framegen/{variant}_src/shaders"
    for name in ("gamma.cpp", "delta.cpp", "generate.cpp"):
        source = (root / name).read_text(encoding="utf-8")
        compact = " ".join(source.split())
        assert "timestampDescriptorType" in source, (variant, name)
        assert "dynamicInterpolationPhases" in source, (variant, name)
        assert "createTimestampRing" in source, (variant, name)
        assert "getBuffer" in source, (variant, name)
        assert "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC" in source, (variant, name)
        assert "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER" in source, (variant, name)
        assert "? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC" in compact, (variant, name)

print("Fixed-mode framegen construction is isolated from fractional dynamic UBOs")

#!/usr/bin/env python3
"""Composition-safe wrapper for the final non-adaptive Adreno sweep.

Reuses the validated probe builders/pipeline changes from
apply-final-nonadaptive-sweep.py, but places the deferred trigger at a boundary
that survives earlier async/profile transforms, gives disposable pipelines the
real descriptor schema of the shader being compiled, and keeps each Mipmaps
probe independent so one failed builder cannot poison the rest of the sweep.
"""
from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE_PATH = ROOT / "scripts/apply-final-nonadaptive-sweep.py"

spec = importlib.util.spec_from_file_location("final_nonadaptive_sweep_base", BASE_PATH)
if spec is None or spec.loader is None:
    raise RuntimeError(f"unable to load {BASE_PATH}")
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)


def patch_independent_mipmaps_probes(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    marker = "sweep-independent-mipmaps-probes"
    if marker in text:
        return
    old = (
        "    if (index >= 1U && index <= 5U) {\n"
        "        const auto variants = b8Variants(baseline);\n"
        "        if (variants.size() != 6U) return false;\n"
        "        out = variants.at(index).bytecode;\n"
        "        return true;\n"
        "    }\n"
    )
    new = (
        "    // sweep-independent-mipmaps-probes: a failed optional builder must not shift\n"
        "    // or suppress later compile-only probes in the fixed sweep index table.\n"
        "    if (index == 1U) return b8PowEquivalent(baseline, out);\n"
        "    if (index == 2U) return b8TransferBypass(baseline, out);\n"
        "    if (index == 3U) return b8NoU0Writes(baseline, out);\n"
        "    if (index == 4U) return b8Local16(baseline, out);\n"
        "    if (index == 5U) return b8Local8(baseline, out);\n"
    )
    text = base.replace_exact(text, old, new, "Independent Mipmaps sweep probes")
    path.write_text(text, encoding="utf-8")


def patch_probe_descriptor_layout(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    marker = "sweep-probe-descriptor-layout"
    if marker in text:
        return
    old = "        Core::ShaderModule shader(device, probe, {});\n"
    new = (
        "        // sweep-probe-descriptor-layout: pipeline layouts must match statically used bindings.\n"
        "        std::vector<std::pair<size_t, VkDescriptorType>> descriptorTypes;\n"
        "        if (std::string(spec.sourceName) == \"p_mipmaps\") {\n"
        "            descriptorTypes = {\n"
        "                {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},\n"
        "                {1, VK_DESCRIPTOR_TYPE_SAMPLER},\n"
        "                {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},\n"
        "                {7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},\n"
        "            };\n"
        "        } else {\n"
        "            descriptorTypes = {\n"
        "                {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},\n"
        "                {1, VK_DESCRIPTOR_TYPE_SAMPLER},\n"
        "                {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},\n"
        "                {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},\n"
        "            };\n"
        "        }\n"
        "        Core::ShaderModule shader(device, probe, descriptorTypes);\n"
    )
    text = base.replace_exact(text, old, new, "Sweep probe descriptor layout")
    path.write_text(text, encoding="utf-8")


def patch_context(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "kAdrenoSweepWarmupFrames" in text:
        return

    text = base.replace_exact(
        text,
        "namespace {\nuint64_t framegenWaitTimeoutNs() {\n",
        "namespace {\n"
        "constexpr uint64_t kAdrenoSweepWarmupFrames = 120U;\n"
        "constexpr uint64_t kAdrenoSweepIntervalFrames = 45U;\n"
        "uint64_t framegenWaitTimeoutNs() {\n",
        "Context sweep constants",
    )

    anchor = "    if (data.shouldWait)\n"
    insertion = (
        "#ifdef __ANDROID__\n"
        "    if (this->frameIdx >= kAdrenoSweepWarmupFrames\n"
        "            && ((this->frameIdx - kAdrenoSweepWarmupFrames)\n"
        "                % kAdrenoSweepIntervalFrames) == 0U)\n"
        "        vk.shaders.runNextAdrenoSweepProbe(vk.device);\n"
        "#endif\n\n"
        + anchor
    )
    text = base.replace_exact(text, anchor, insertion, "Context deferred probe trigger")
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    root = args.root.resolve()

    base.patch_pipeline(root / base.PIPELINE_CPP)
    base.patch_pool_hpp(root / base.POOL_HPP)
    base.patch_pool_cpp(root / base.POOL_CPP)
    patch_independent_mipmaps_probes(root / base.POOL_CPP)
    patch_probe_descriptor_layout(root / base.POOL_CPP)
    patch_context(root / base.CONTEXT_CPP)


if __name__ == "__main__":
    main()

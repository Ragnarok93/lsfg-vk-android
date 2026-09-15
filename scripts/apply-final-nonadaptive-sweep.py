#!/usr/bin/env python3
"""Deferred compiler sweep for the final non-adaptive Adreno optimization pass.

This transform is profiling-only and is applied after Candidate B6 + B8.  It
removes B8's eager startup matrix behavior, leaves the normal runtime pipeline
path untouched, and compiles one unsafe/compile-only probe at a time after a
warm-up interval.  Every probe is bracketed in the log and routed through the
B6 pipeline-executable capture path; no probe is ever bound for presentation.
"""
from __future__ import annotations

import argparse
from pathlib import Path

POOL_HPP = Path("framegen/include/pool/shaderpool.hpp")
POOL_CPP = Path("framegen/src/pool/shaderpool.cpp")
PIPELINE_CPP = Path("framegen/src/core/pipeline.cpp")
CONTEXT_CPP = Path("framegen/v3.1p_src/context.cpp")
MARKER = "final-nonadaptive-sweep"


def replace_exact(text: str, old: str, new: str, label: str) -> str:
    if new in text:
        return text
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one anchor, found {count}")
    return text.replace(old, new, 1)


def patch_pipeline(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if 'shaderName.rfind("p_mipmaps@sweep-", 0) == 0' in text:
        return
    text = replace_exact(
        text,
        '    if (shaderName != "p_mipmaps") return;\n',
        '    if (shaderName.rfind("p_mipmaps@sweep-", 0) != 0) return;\n',
        "B6 deferred logger prefix",
    )
    text = replace_exact(
        text,
        '    const bool captureExecutableInfo = shaderName.rfind("p_mipmaps", 0) == 0\n'
        '        && device.supportsPipelineExecutableProperties();\n',
        '    const bool captureExecutableInfo = shaderName.rfind("p_mipmaps@sweep-", 0) == 0\n'
        '        && device.supportsPipelineExecutableProperties();\n',
        "B8 deferred capture prefix",
    )
    path.write_text(text, encoding="utf-8")


def patch_pool_hpp(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "runNextAdrenoSweepProbe" in text:
        return
    text = replace_exact(
        text,
        "        Core::Pipeline getPipeline(\n"
        "            const Core::Device& device, const std::string& name);\n",
        "        Core::Pipeline getPipeline(\n"
        "            const Core::Device& device, const std::string& name);\n"
        "\n"
        "        /// Compile one deferred Adreno profiling probe. Probes are never bound.\n"
        "        void runNextAdrenoSweepProbe(const Core::Device& device);\n",
        "ShaderPool public deferred probe API",
    )
    text = replace_exact(
        text,
        "        std::unordered_map<std::string, Core::Pipeline> pipelines;\n",
        "        std::unordered_map<std::string, Core::Pipeline> pipelines;\n"
        "        size_t adrenoSweepProbeIndex{0};\n",
        "ShaderPool deferred probe state",
    )
    path.write_text(text, encoding="utf-8")


BETA_HELPER = r'''
constexpr uint16_t kSweepOpImageSampleExplicitLod = 88U;
constexpr uint16_t kSweepOpCopyObject = 83U;
constexpr uint16_t kSweepOpImageWrite = 99U;
constexpr uint16_t kSweepOpControlBarrier = 224U;
constexpr uint16_t kSweepOpExecutionMode = 16U;
constexpr uint32_t kSweepExecutionModeLocalSize = 17U;
constexpr size_t kSweepBeta4Bytes = 50764U;
constexpr uint32_t kSweepBeta4Bound = 2351U;

bool sweepBeta4Baseline(const std::vector<uint8_t>& code) {
    if (code.size() != kSweepBeta4Bytes || code.size() % 4U != 0U
            || b8Word(code, 0) != 0x07230203U || b8Word(code, 3) != kSweepBeta4Bound)
        return false;
    std::vector<B8Instruction> instructions;
    if (!b8Instructions(code, instructions)) return false;
    size_t samples = 0, writes = 0, barriers = 0, local32 = 0;
    for (const auto& ins : instructions) {
        if (ins.opCode == kSweepOpImageSampleExplicitLod) ++samples;
        else if (ins.opCode == kSweepOpImageWrite) ++writes;
        else if (ins.opCode == kSweepOpControlBarrier) ++barriers;
        else if (ins.opCode == kSweepOpExecutionMode && ins.wordCount == 6
                && b8Word(code, ins.word + 2) == kSweepExecutionModeLocalSize
                && b8Word(code, ins.word + 3) == 32U
                && b8Word(code, ins.word + 4) == 32U
                && b8Word(code, ins.word + 5) == 1U) ++local32;
    }
    return samples == 18U && writes == 6U && barriers == 5U && local32 == 1U;
}

bool sweepBeta4Samples(const std::vector<uint8_t>& code, std::vector<uint8_t>& out,
        bool singleSample) {
    if (!sweepBeta4Baseline(code)) return false;
    std::vector<B8Instruction> instructions;
    if (!b8Instructions(code, instructions)) return false;
    std::vector<uint32_t> words;
    words.reserve(code.size() / 4U);
    for (size_t i = 0; i < 5; ++i) words.push_back(b8Word(code, i));
    uint32_t firstResult = 0, previousResult = 0;
    size_t sampleIndex = 0, replaced = 0;
    for (const auto& ins : instructions) {
        if (ins.opCode == kSweepOpImageSampleExplicitLod) {
            const uint32_t resultType = b8Word(code, ins.word + 1);
            const uint32_t resultId = b8Word(code, ins.word + 2);
            const bool replace = sampleIndex > 0U && (singleSample || (sampleIndex % 2U) == 1U);
            if (replace) {
                const uint32_t sourceId = singleSample ? firstResult : previousResult;
                if (sourceId == 0U) return false;
                words.push_back((4U << 16U) | static_cast<uint32_t>(kSweepOpCopyObject));
                words.push_back(resultType);
                words.push_back(resultId);
                words.push_back(sourceId);
                ++replaced;
            } else {
                for (size_t i = 0; i < ins.wordCount; ++i)
                    words.push_back(b8Word(code, ins.word + i));
                if (firstResult == 0U) firstResult = resultId;
                previousResult = resultId;
            }
            ++sampleIndex;
            continue;
        }
        for (size_t i = 0; i < ins.wordCount; ++i)
            words.push_back(b8Word(code, ins.word + i));
    }
    const size_t expected = singleSample ? 17U : 9U;
    if (sampleIndex != 18U || replaced != expected) return false;
    out.resize(words.size() * 4U);
    std::memcpy(out.data(), words.data(), out.size());
    return true;
}

bool sweepBeta4HalfSamples(const std::vector<uint8_t>& code, std::vector<uint8_t>& out) {
    return sweepBeta4Samples(code, out, false);
}

bool sweepBeta4SingleSample(const std::vector<uint8_t>& code, std::vector<uint8_t>& out) {
    return sweepBeta4Samples(code, out, true);
}

bool sweepBeta4NoWrites(const std::vector<uint8_t>& code, std::vector<uint8_t>& out) {
    if (!sweepBeta4Baseline(code)) return false;
    std::vector<B8Instruction> instructions;
    if (!b8Instructions(code, instructions)) return false;
    std::vector<uint32_t> words;
    words.reserve(code.size() / 4U);
    for (size_t i = 0; i < 5; ++i) words.push_back(b8Word(code, i));
    size_t removed = 0;
    for (const auto& ins : instructions) {
        if (ins.opCode == kSweepOpImageWrite) {
            ++removed;
            continue;
        }
        for (size_t i = 0; i < ins.wordCount; ++i)
            words.push_back(b8Word(code, ins.word + i));
    }
    if (removed != 6U) return false;
    out.resize(words.size() * 4U);
    std::memcpy(out.data(), words.data(), out.size());
    return true;
}

bool sweepBeta4LocalSize(const std::vector<uint8_t>& code, std::vector<uint8_t>& out,
        uint32_t size) {
    if (!sweepBeta4Baseline(code)) return false;
    out = code;
    std::vector<B8Instruction> instructions;
    if (!b8Instructions(code, instructions)) return false;
    size_t changed = 0;
    for (const auto& ins : instructions) {
        if (ins.opCode == kSweepOpExecutionMode && ins.wordCount == 6
                && b8Word(code, ins.word + 2) == kSweepExecutionModeLocalSize
                && b8Word(code, ins.word + 3) == 32U
                && b8Word(code, ins.word + 4) == 32U
                && b8Word(code, ins.word + 5) == 1U) {
            std::memcpy(out.data() + (ins.word + 3) * 4U, &size, 4U);
            std::memcpy(out.data() + (ins.word + 4) * 4U, &size, 4U);
            ++changed;
        }
    }
    return changed == 1U;
}

bool sweepBeta4Local16(const std::vector<uint8_t>& code, std::vector<uint8_t>& out) {
    return sweepBeta4LocalSize(code, out, 16U);
}

bool sweepBeta4Local8(const std::vector<uint8_t>& code, std::vector<uint8_t>& out) {
    return sweepBeta4LocalSize(code, out, 8U);
}

struct SweepProbeSpec {
    const char* name;
    const char* sourceName;
};

constexpr std::array<SweepProbeSpec, 12> kSweepProbeSpecs{{
    {"mipmaps-baseline", "p_mipmaps"},
    {"mipmaps-pow-equivalent", "p_mipmaps"},
    {"mipmaps-transfer-bypass-upper-bound", "p_mipmaps"},
    {"mipmaps-no-u0-writes-upper-bound", "p_mipmaps"},
    {"mipmaps-local16-occupancy-probe", "p_mipmaps"},
    {"mipmaps-local8-occupancy-probe", "p_mipmaps"},
    {"beta4-baseline", "p_beta[4]"},
    {"beta4-half-samples-upper-bound", "p_beta[4]"},
    {"beta4-single-sample-upper-bound", "p_beta[4]"},
    {"beta4-no-output-writes-upper-bound", "p_beta[4]"},
    {"beta4-local16-occupancy-probe", "p_beta[4]"},
    {"beta4-local8-occupancy-probe", "p_beta[4]"},
}};

bool buildSweepProbe(size_t index, const std::vector<uint8_t>& baseline,
        std::vector<uint8_t>& out) {
    if (index == 0U) { if (!b8Baseline(baseline)) return false; out = baseline; return true; }
    if (index >= 1U && index <= 5U) {
        const auto variants = b8Variants(baseline);
        if (variants.size() != 6U) return false;
        out = variants.at(index).bytecode;
        return true;
    }
    if (index == 6U) { if (!sweepBeta4Baseline(baseline)) return false; out = baseline; return true; }
    if (index == 7U) return sweepBeta4HalfSamples(baseline, out);
    if (index == 8U) return sweepBeta4SingleSample(baseline, out);
    if (index == 9U) return sweepBeta4NoWrites(baseline, out);
    if (index == 10U) return sweepBeta4Local16(baseline, out);
    if (index == 11U) return sweepBeta4Local8(baseline, out);
    return false;
}
'''


def patch_pool_cpp(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if MARKER in text:
        return
    text = replace_exact(
        text,
        "#include <cstddef>\n",
        "#include <array>\n#include <cstddef>\n",
        "ShaderPool array include",
    )
    namespace_anchor = "} // namespace\n\nusing namespace LSFG;"
    if namespace_anchor not in text:
        raise RuntimeError("B8 helper namespace end anchor not found")
    text = text.replace(
        namespace_anchor,
        BETA_HELPER + "\n} // namespace\n\nusing namespace LSFG;",
        1,
    )

    start = text.find("Core::Pipeline ShaderPool::getPipeline(")
    if start < 0:
        raise RuntimeError("ShaderPool getPipeline anchor not found")
    replacement = r'''Core::Pipeline ShaderPool::getPipeline(
        const Core::Device& device, const std::string& name) {
    auto it = pipelines.find(name);
    if (it != pipelines.end())
        return it->second;

    auto shader = this->getShader(device, name, {});
    Core::Pipeline pipeline(device, shader, name);
    pipelines[name] = pipeline;
    return pipeline;
}

void ShaderPool::runNextAdrenoSweepProbe(const Core::Device& device) {
    if (!device.supportsPipelineExecutableProperties()
            || this->adrenoSweepProbeIndex >= kSweepProbeSpecs.size())
        return;

    const size_t index = this->adrenoSweepProbeIndex++;
    const auto& spec = kSweepProbeSpecs.at(index);
    std::vector<uint8_t> probe;
    const auto baseline = this->source(spec.sourceName);
    const bool built = buildSweepProbe(index, baseline, probe);
    std::cerr << "lsfg-vk: sweep-probe-begin suite=final-nonadaptive-sweep index="
              << index << " name=" << spec.name
              << " source=" << spec.sourceName
              << " compile_only=1 applied=" << (built ? 1 : 0)
              << " bytes=" << (built ? probe.size() : 0U) << '\n';
    if (!built) {
        std::cerr << "lsfg-vk: sweep-probe-end suite=final-nonadaptive-sweep index="
                  << index << " name=" << spec.name << " result=build-failed\n";
        return;
    }

    try {
        Core::ShaderModule shader(device, probe, {});
        Core::Pipeline pipeline(device, shader,
            std::string("p_mipmaps@sweep-") + spec.name);
        std::cerr << "lsfg-vk: sweep-probe-end suite=final-nonadaptive-sweep index="
                  << index << " name=" << spec.name << " result=ok\n";
    } catch (const std::exception& error) {
        std::cerr << "lsfg-vk: sweep-probe-end suite=final-nonadaptive-sweep index="
                  << index << " name=" << spec.name << " result=failed error=\""
                  << error.what() << "\"\n";
    }
}
'''
    text = text[:start] + replacement
    path.write_text(text, encoding="utf-8")


def patch_context(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "kAdrenoSweepWarmupFrames" in text:
        return
    text = replace_exact(
        text,
        "namespace {\nuint64_t framegenWaitTimeoutNs() {\n",
        "namespace {\n"
        "constexpr uint64_t kAdrenoSweepWarmupFrames = 120U;\n"
        "constexpr uint64_t kAdrenoSweepIntervalFrames = 45U;\n"
        "uint64_t framegenWaitTimeoutNs() {\n",
        "Context sweep constants",
    )
    anchor = "    auto& data = this->data.at(this->frameIdx % 8);\n\n"
    insertion = (
        "    auto& data = this->data.at(this->frameIdx % 8);\n\n"
        "#ifdef __ANDROID__\n"
        "    if (this->frameIdx >= kAdrenoSweepWarmupFrames\n"
        "            && ((this->frameIdx - kAdrenoSweepWarmupFrames)\n"
        "                % kAdrenoSweepIntervalFrames) == 0U)\n"
        "        vk.shaders.runNextAdrenoSweepProbe(vk.device);\n"
        "#endif\n\n"
    )
    text = replace_exact(text, anchor, insertion, "Context deferred probe trigger")
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    patch_pipeline(root / PIPELINE_CPP)
    patch_pool_hpp(root / POOL_HPP)
    patch_pool_cpp(root / POOL_CPP)
    patch_context(root / CONTEXT_CPP)


if __name__ == "__main__":
    main()

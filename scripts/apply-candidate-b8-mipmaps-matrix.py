#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

PIPELINE_CPP = Path('framegen/src/core/pipeline.cpp')
SHADERPOOL_CPP = Path('framegen/src/pool/shaderpool.cpp')
MARKER = 'candidate-b8-mipmaps-matrix'


def replace_exact(text: str, old: str, new: str, label: str) -> str:
    if new in text:
        return text
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'{label}: expected one anchor, found {count}')
    return text.replace(old, new, 1)


HELPER = r'''namespace {
constexpr uint64_t kB8ExpectedFnv = 0x65d3c6a69e9f9b07ULL;
constexpr size_t kB8ExpectedBytes = 28832;
constexpr uint32_t kB8ExpectedBound = 1270;
constexpr size_t kB8ExpectedTransferChains = 8;

struct B8Instruction {
    size_t word;
    uint16_t wordCount;
    uint16_t opCode;
};

struct B8Variant {
    std::string name;
    std::vector<uint8_t> bytecode;
    bool compileOnly;
    bool semanticsPreserving;
};

uint32_t b8Word(const std::vector<uint8_t>& code, size_t index) {
    uint32_t word = 0;
    std::memcpy(&word, code.data() + index * sizeof(uint32_t), sizeof(uint32_t));
    return word;
}

uint64_t b8Fnv(const std::vector<uint8_t>& code) {
    uint64_t hash = 1469598103934665603ULL;
    for (uint8_t value : code) {
        hash ^= static_cast<uint64_t>(value);
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool b8Baseline(const std::vector<uint8_t>& code) {
    return code.size() == kB8ExpectedBytes
        && code.size() % sizeof(uint32_t) == 0
        && b8Word(code, 0) == 0x07230203U
        && b8Word(code, 3) == kB8ExpectedBound
        && b8Fnv(code) == kB8ExpectedFnv;
}

bool b8Instructions(const std::vector<uint8_t>& code,
        std::vector<B8Instruction>& instructions) {
    if (code.size() % 4 != 0 || code.size() < 20)
        return false;
    instructions.clear();
    const size_t words = code.size() / 4;
    for (size_t cursor = 5; cursor < words;) {
        const uint32_t first = b8Word(code, cursor);
        const uint16_t wc = static_cast<uint16_t>(first >> 16U);
        const uint16_t op = static_cast<uint16_t>(first & 0xffffU);
        if (wc == 0 || cursor + wc > words)
            return false;
        instructions.push_back({cursor, wc, op});
        cursor += wc;
    }
    return true;
}

bool b8PowEquivalent(const std::vector<uint8_t>& code, std::vector<uint8_t>& out) {
    if (!b8Baseline(code)) return false;
    struct LogInfo { uint32_t input{}; uint32_t set{}; };
    struct MulInfo { uint32_t log{}; uint32_t exponent{}; };
    std::vector<LogInfo> logs(kB8ExpectedBound);
    std::vector<MulInfo> muls(kB8ExpectedBound);
    std::vector<B8Instruction> instructions;
    if (!b8Instructions(code, instructions)) return false;
    for (const auto& ins : instructions) {
        if (ins.opCode == static_cast<uint16_t>(spv::OpExtInst) && ins.wordCount == 6
                && b8Word(code, ins.word + 4) == 30U) {
            const uint32_t result = b8Word(code, ins.word + 2);
            if (result < logs.size())
                logs[result] = {b8Word(code, ins.word + 5), b8Word(code, ins.word + 3)};
        } else if (ins.opCode == static_cast<uint16_t>(spv::OpFMul) && ins.wordCount == 5) {
            const uint32_t result = b8Word(code, ins.word + 2);
            const uint32_t a = b8Word(code, ins.word + 3);
            const uint32_t b = b8Word(code, ins.word + 4);
            if (result < muls.size()) {
                if (a < logs.size() && logs[a].input) muls[result] = {a, b};
                else if (b < logs.size() && logs[b].input) muls[result] = {b, a};
            }
        }
    }
    std::vector<uint32_t> words;
    words.reserve(code.size() / 4 + kB8ExpectedTransferChains);
    for (size_t i = 0; i < 5; ++i) words.push_back(b8Word(code, i));
    size_t changed = 0;
    for (const auto& ins : instructions) {
        if (ins.opCode == static_cast<uint16_t>(spv::OpExtInst) && ins.wordCount == 6
                && b8Word(code, ins.word + 4) == 29U) {
            const uint32_t mulId = b8Word(code, ins.word + 5);
            if (mulId < muls.size() && muls[mulId].log) {
                const auto& mul = muls[mulId];
                const auto& log = logs[mul.log];
                words.push_back((7U << 16U) | static_cast<uint32_t>(spv::OpExtInst));
                words.push_back(b8Word(code, ins.word + 1));
                words.push_back(b8Word(code, ins.word + 2));
                words.push_back(log.set);
                words.push_back(26U);
                words.push_back(log.input);
                words.push_back(mul.exponent);
                ++changed;
                continue;
            }
        }
        for (size_t i = 0; i < ins.wordCount; ++i)
            words.push_back(b8Word(code, ins.word + i));
    }
    if (changed != kB8ExpectedTransferChains) return false;
    out.resize(words.size() * 4);
    std::memcpy(out.data(), words.data(), out.size());
    return b8Word(out, 3) == kB8ExpectedBound;
}

bool b8TransferBypass(const std::vector<uint8_t>& code, std::vector<uint8_t>& out) {
    if (!b8Baseline(code)) return false;
    std::vector<B8Instruction> instructions;
    if (!b8Instructions(code, instructions)) return false;
    std::vector<uint32_t> words;
    words.reserve(code.size() / 4);
    for (size_t i = 0; i < 5; ++i) words.push_back(b8Word(code, i));
    size_t changed = 0;
    for (const auto& ins : instructions) {
        if (ins.opCode == static_cast<uint16_t>(spv::OpExtInst) && ins.wordCount == 6) {
            const uint32_t ext = b8Word(code, ins.word + 4);
            if (ext == 29U || ext == 30U) {
                words.push_back((4U << 16U) | static_cast<uint32_t>(spv::OpCopyObject));
                words.push_back(b8Word(code, ins.word + 1));
                words.push_back(b8Word(code, ins.word + 2));
                words.push_back(b8Word(code, ins.word + 5));
                ++changed;
                continue;
            }
        }
        for (size_t i = 0; i < ins.wordCount; ++i)
            words.push_back(b8Word(code, ins.word + i));
    }
    if (changed != kB8ExpectedTransferChains * 2U) return false;
    out.resize(words.size() * 4);
    std::memcpy(out.data(), words.data(), out.size());
    return true;
}

bool b8NoU0Writes(const std::vector<uint8_t>& code, std::vector<uint8_t>& out) {
    if (!b8Baseline(code)) return false;
    std::vector<B8Instruction> instructions;
    if (!b8Instructions(code, instructions)) return false;
    std::vector<uint32_t> words;
    words.reserve(code.size() / 4);
    for (size_t i = 0; i < 5; ++i) words.push_back(b8Word(code, i));
    bool phase0 = true;
    size_t writes = 0, removed = 0;
    for (const auto& ins : instructions) {
        if (phase0 && ins.opCode == static_cast<uint16_t>(spv::OpControlBarrier))
            phase0 = false;
        if (phase0 && ins.opCode == static_cast<uint16_t>(spv::OpImageWrite)) {
            ++writes;
            if (writes <= 4U) { ++removed; continue; }
        }
        for (size_t i = 0; i < ins.wordCount; ++i)
            words.push_back(b8Word(code, ins.word + i));
    }
    if (writes != 5U || removed != 4U) return false;
    out.resize(words.size() * 4);
    std::memcpy(out.data(), words.data(), out.size());
    return true;
}

bool b8LocalSize(const std::vector<uint8_t>& code, std::vector<uint8_t>& out,
        uint32_t size) {
    if (!b8Baseline(code)) return false;
    out = code;
    std::vector<B8Instruction> instructions;
    if (!b8Instructions(code, instructions)) return false;
    size_t changed = 0;
    for (const auto& ins : instructions) {
        if (ins.opCode == static_cast<uint16_t>(spv::OpExecutionMode) && ins.wordCount == 6
                && b8Word(code, ins.word + 2) == static_cast<uint32_t>(spv::ExecutionModeLocalSize)
                && b8Word(code, ins.word + 3) == 32U
                && b8Word(code, ins.word + 4) == 32U
                && b8Word(code, ins.word + 5) == 1U) {
            std::memcpy(out.data() + (ins.word + 3) * 4, &size, 4);
            std::memcpy(out.data() + (ins.word + 4) * 4, &size, 4);
            ++changed;
        }
    }
    return changed == 1U;
}

bool b8Local16(const std::vector<uint8_t>& code, std::vector<uint8_t>& out) {
    return b8LocalSize(code, out, 16U);
}
bool b8Local8(const std::vector<uint8_t>& code, std::vector<uint8_t>& out) {
    return b8LocalSize(code, out, 8U);
}

std::vector<B8Variant> b8Variants(const std::vector<uint8_t>& baseline) {
    std::vector<B8Variant> variants{{"baseline", baseline, false, true}};
    if (!b8Baseline(baseline)) {
        std::cerr << "lsfg-vk: candidate-b8-mipmaps-matrix input_match=0 bytes="
                  << baseline.size() << '\n';
        return variants;
    }
    auto add = [&](const char* name, bool compileOnly, bool semanticsPreserving,
            bool (*builder)(const std::vector<uint8_t>&, std::vector<uint8_t>&)) {
        std::vector<uint8_t> code;
        const bool ok = builder(baseline, code);
        std::cerr << "lsfg-vk: candidate-b8-variant-build name=" << name
                  << " applied=" << (ok ? 1 : 0)
                  << " compile_only=" << (compileOnly ? 1 : 0)
                  << " semantics_preserving=" << (semanticsPreserving ? 1 : 0)
                  << " bytes=" << (ok ? code.size() : 0) << '\n';
        if (ok) variants.push_back({name, std::move(code), compileOnly, semanticsPreserving});
    };
    add("pow-equivalent", false, false, b8PowEquivalent);
    add("transfer-bypass-upper-bound", true, false, b8TransferBypass);
    add("no-u0-writes-upper-bound", true, false, b8NoU0Writes);
    add("local16-occupancy-probe", true, false, b8Local16);
    add("local8-occupancy-probe", true, false, b8Local8);
    std::cerr << "lsfg-vk: candidate-b8-mipmaps-matrix input_match=1 variants="
              << variants.size() << '\n';
    return variants;
}
} // namespace

'''


def patch_pipeline(path: Path) -> None:
    text = path.read_text(encoding='utf-8')
    if 'shaderName.rfind("p_mipmaps", 0) == 0' in text:
        return
    text = replace_exact(text,
        '    const bool captureExecutableInfo = shaderName == "p_mipmaps"\n'
        '        && device.supportsPipelineExecutableProperties();\n',
        '    const bool captureExecutableInfo = shaderName.rfind("p_mipmaps", 0) == 0\n'
        '        && device.supportsPipelineExecutableProperties();\n',
        'pipeline variant capture prefix')
    path.write_text(text, encoding='utf-8')


def patch_shaderpool(path: Path) -> None:
    text = path.read_text(encoding='utf-8')
    if MARKER in text:
        return
    text = replace_exact(text,
        '#include <vulkan/vulkan_core.h>\n\n',
        '#include <vulkan/vulkan_core.h>\n#include <thirdparty/spirv.hpp>\n\n',
        'shaderpool SPIR-V include')
    text = replace_exact(text,
        '#include <cstddef>\n',
        '#include <cstddef>\n#include <cstdlib>\n#include <cstring>\n#include <iostream>\n',
        'shaderpool matrix includes')
    text = replace_exact(text,
        'using namespace LSFG;\nusing namespace LSFG::Pool;\n',
        HELPER + 'using namespace LSFG;\nusing namespace LSFG::Pool;\n',
        'shaderpool matrix helper')
    old = '''Core::Pipeline ShaderPool::getPipeline(\n        const Core::Device& device, const std::string& name) {\n    auto it = pipelines.find(name);\n    if (it != pipelines.end())\n        return it->second;\n\n    // grab the shader module\n    auto shader = this->getShader(device, name, {});\n\n    // create the pipeline\n    Core::Pipeline pipeline(device, shader, name);\n    pipelines[name] = pipeline;\n    return pipeline;\n}\n'''
    new = r'''Core::Pipeline ShaderPool::getPipeline(
        const Core::Device& device, const std::string& name) {
    auto it = pipelines.find(name);
    if (it != pipelines.end())
        return it->second;

    if (name != "p_mipmaps" || !device.supportsPipelineExecutableProperties()) {
        auto shader = this->getShader(device, name, {});
        Core::Pipeline pipeline(device, shader, name);
        pipelines[name] = pipeline;
        return pipeline;
    }

    const auto variants = b8Variants(this->source(name));
    const char* requestedEnv = std::getenv("LSFGVK_B8_MIPMAPS_VARIANT");
    const std::string requested = requestedEnv ? requestedEnv : "";
    Core::Pipeline selectedPipeline;
    bool selected = false;
    std::string selectedName = "baseline";
    std::string selectedReason = "baseline-default";

    for (const auto& variant : variants) {
        try {
            std::cerr << "lsfg-vk: mipmaps-variant-begin name=" << variant.name
                      << " compileOnly=" << (variant.compileOnly ? 1 : 0)
                      << " semanticsPreserving=" << (variant.semanticsPreserving ? 1 : 0)
                      << " bytes=" << variant.bytecode.size() << '\n';
            Core::ShaderModule variantShader(device, variant.bytecode, {});
            Core::Pipeline variantPipeline(device, variantShader,
                "p_mipmaps@" + variant.name);
            std::cerr << "lsfg-vk: mipmaps-variant-end name=" << variant.name << '\n';

            if (variant.name == "baseline") {
                selectedPipeline = variantPipeline;
                selected = true;
                shaders[name] = variantShader;
            } else if (!variant.compileOnly && !requested.empty()
                    && requested == variant.name) {
                selectedPipeline = variantPipeline;
                selectedName = variant.name;
                selectedReason = "explicit-override";
                selected = true;
                shaders[name] = variantShader;
            }
        } catch (const std::exception& error) {
            std::cerr << "lsfg-vk: mipmaps-variant-failed name=" << variant.name
                      << " error=\"" << error.what() << "\"\n";
            if (variant.name == "baseline") throw;
        }
    }

    if (!selected)
        throw std::runtime_error("B8 failed to create baseline p_mipmaps pipeline");
    std::cerr << "lsfg-vk: mipmaps-variant-selected name=" << selectedName
              << " reason=" << selectedReason
              << " requested=" << (requested.empty() ? "auto" : requested) << '\n';
    pipelines[name] = selectedPipeline;
    return selectedPipeline;
}
'''
    text = replace_exact(text, old, new, 'shaderpool matrix getPipeline')
    path.write_text(text, encoding='utf-8')


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    patch_pipeline(root / PIPELINE_CPP)
    patch_shaderpool(root / SHADERPOOL_CPP)


if __name__ == '__main__':
    main()

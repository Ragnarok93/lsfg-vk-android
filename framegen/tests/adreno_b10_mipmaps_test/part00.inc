#include "adreno_b10_mipmaps.hpp"

#include <thirdparty/spirv.hpp>

#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <random>
#include <set>
#include <string>
#include <vector>

using LSFG::Optimizations::AdrenoB10::HeadTransformStats;
using LSFG::Optimizations::AdrenoB10::buildTailSpirv;
using LSFG::Optimizations::AdrenoB10::isAdreno6xxDevice;
using LSFG::Optimizations::AdrenoB10::transformHead;
using LSFG::Optimizations::AdrenoB10::transformHeadForSelfTest;

namespace {

void put(std::vector<uint32_t>& words, spv::Op op,
        std::initializer_list<uint32_t> args) {
    words.push_back((static_cast<uint32_t>(args.size() + 1) << 16u)
        | static_cast<uint32_t>(op));
    words.insert(words.end(), args.begin(), args.end());
}

std::vector<uint8_t> syntheticHead() {
    std::vector<uint32_t> w{
        0x07230203u, 0x00010300u, 0u, 100u, 0u,
    };
    constexpr uint32_t voidType = 1;
    constexpr uint32_t imageType = 2;
    constexpr uint32_t valueType = 3;
    constexpr uint32_t vec4Type = 4;
    constexpr uint32_t uintType = 5;
    constexpr uint32_t imagePtr = 6;
    constexpr uint32_t wgPtr = 7;
    constexpr uint32_t varU0 = 11;
    constexpr uint32_t varU1 = 12;
    constexpr uint32_t varScratch = 13;
    constexpr uint32_t varWg = 14;
    constexpr uint32_t function = 20;
    constexpr uint32_t imageU0 = 30;
    constexpr uint32_t imageU1 = 31;
    constexpr uint32_t imageOther = 32;

    put(w, spv::OpDecorate, {varU0, spv::DecorationDescriptorSet, 0});
    put(w, spv::OpDecorate, {varU0, spv::DecorationBinding, 3});
    put(w, spv::OpDecorate, {varU1, spv::DecorationDescriptorSet, 0});
    put(w, spv::OpDecorate, {varU1, spv::DecorationBinding, 4});
    put(w, spv::OpDecorate, {varScratch, spv::DecorationDescriptorSet, 0});
    put(w, spv::OpDecorate, {varScratch, spv::DecorationBinding, 5});
    put(w, spv::OpTypeVoid, {voidType});
    put(w, spv::OpTypeInt, {uintType, 32, 0});
    put(w, spv::OpTypeVector, {vec4Type, valueType, 4});
    put(w, spv::OpTypePointer, {imagePtr, spv::StorageClassUniformConstant, imageType});
    put(w, spv::OpTypePointer, {wgPtr, spv::StorageClassWorkgroup, uintType});
    put(w, spv::OpVariable, {imagePtr, varU0, spv::StorageClassUniformConstant});
    put(w, spv::OpVariable, {imagePtr, varU1, spv::StorageClassUniformConstant});
    put(w, spv::OpVariable, {imagePtr, varScratch, spv::StorageClassUniformConstant});
    put(w, spv::OpVariable, {wgPtr, varWg, spv::StorageClassWorkgroup});
    put(w, spv::OpExecutionMode, {function, spv::ExecutionModeLocalSize, 32, 32, 1});
    put(w, spv::OpFunction, {voidType, function, spv::FunctionControlMaskNone, 40});
    put(w, spv::OpLabel, {41});
    put(w, spv::OpLoad, {imageType, imageU0, varU0});
    put(w, spv::OpLoad, {imageType, imageU1, varU1});
    put(w, spv::OpLoad, {imageType, imageOther, varScratch});

    for (uint32_t i = 0; i < 4; ++i) {
        put(w, spv::OpImageSampleExplicitLod, {});
        put(w, spv::OpImageWrite, {imageU0, 50 + i, 60 + i});
    }
    put(w, spv::OpCompositeConstruct, {vec4Type, 72, 71, 71, 71, 71});
    put(w, spv::OpImageWrite, {imageU1, 70, 72});
    // Match the production shader's relevant SSA shape: TGSM receives the
    // bit representation of the scalar, not the same SSA id used by u1.
    put(w, spv::OpBitcast, {uintType, 73, 71});
    put(w, spv::OpStore, {varWg, 73});
    put(w, spv::OpControlBarrier, {80, 80, 81});

    for (uint32_t phase = 0; phase < 5; ++phase) {
        put(w, spv::OpImageWrite, {imageOther, 72 + phase, 82 + phase});
        if (phase != 4)
            put(w, spv::OpControlBarrier, {80, 80, 81});
    }
    put(w, spv::OpReturn, {});
    put(w, spv::OpFunctionEnd, {});

    std::vector<uint8_t> bytes(w.size() * sizeof(uint32_t));
    std::memcpy(bytes.data(), w.data(), bytes.size());
    return bytes;
}

struct TailShape {
    uint32_t localX{};
    uint32_t localY{};
    uint32_t localZ{};
    uint32_t reads{};
    uint32_t writes{};
    uint32_t barriers{};
    uint32_t samples{};
    uint32_t imageQueries{};
    uint32_t conditionalBranches{};
    uint32_t fadds{};
    uint32_t fmuls{};
    uint32_t workgroupVars{};
    std::set<uint32_t> bindings;
};

TailShape inspectTail(const std::vector<uint8_t>& bytes) {
    assert((bytes.size() % 4) == 0);
    std::vector<uint32_t> w(bytes.size() / 4);
    std::memcpy(w.data(), bytes.data(), bytes.size());
    assert(w.size() >= 5 && w[0] == 0x07230203u);

    TailShape shape;
    size_t off = 5;
    while (off < w.size()) {
        const uint32_t first = w[off];
        const uint32_t wc = first >> 16u;
        const auto op = static_cast<spv::Op>(first & 0xffffu);
        assert(wc != 0 && off + wc <= w.size());
        if (op == spv::OpExecutionMode && wc >= 6
                && w[off + 2] == spv::ExecutionModeLocalSize) {
            shape.localX = w[off + 3];
            shape.localY = w[off + 4];
            shape.localZ = w[off + 5];
        } else if (op == spv::OpDecorate && wc >= 4
                && w[off + 2] == spv::DecorationBinding) {
            shape.bindings.insert(w[off + 3]);
        } else if (op == spv::OpVariable && wc >= 4
                && w[off + 3] == spv::StorageClassWorkgroup) {
            ++shape.workgroupVars;
        } else if (op == spv::OpImageRead) {
            ++shape.reads;
        } else if (op == spv::OpImageWrite) {
            ++shape.writes;
        } else if (op == spv::OpControlBarrier) {
            ++shape.barriers;
        } else if (op == spv::OpImageQuerySize) {
            ++shape.imageQueries;
        } else if (op == spv::OpBranchConditional) {
            ++shape.conditionalBranches;
        } else if (op == spv::OpFAdd) {
            ++shape.fadds;
        } else if (op == spv::OpFMul) {
            ++shape.fmuls;
        } else if (op == spv::OpImageSampleImplicitLod
                || op == spv::OpImageSampleExplicitLod
                || op == spv::OpImageSampleDrefImplicitLod
                || op == spv::OpImageSampleDrefExplicitLod
                || op == spv::OpImageSampleProjImplicitLod
                || op == spv::OpImageSampleProjExplicitLod
                || op == spv::OpImageSampleProjDrefImplicitLod
                || op == spv::OpImageSampleProjDrefExplicitLod) {
            ++shape.samples;
        }
        off += wc;
    }
    assert(off == w.size());
    return shape;
}

float reduce(float a, float b, float c, float d) {
    const float ab = a + b;
    const float abc = ab + c;
    const float abcd = abc + d;
    return abcd * 0.25f;
}

using Level = std::vector<float>;

std::array<Level, 5> oldPyramid(const Level& u1) {
    assert(u1.size() == 32u * 32u);
    Level shared = u1;
    std::array<Level, 5> levels{
        Level(16u * 16u), Level(8u * 8u), Level(4u * 4u),
        Level(2u * 2u), Level(1u)};

    const std::array<uint32_t, 5> offsets{1, 2, 4, 8, 16};
    const std::array<uint32_t, 5> strides{2, 4, 8, 16, 32};
    const std::array<uint32_t, 5> dims{16, 8, 4, 2, 1};
    for (size_t level = 0; level < levels.size(); ++level) {
        const uint32_t offset = offsets[level];
        const uint32_t stride = strides[level];
        const uint32_t dim = dims[level];
        for (uint32_t x = 0; x < 32; x += stride) {
            for (uint32_t y = 0; y < 32; y += stride) {
                const float v = reduce(
                    shared[x * 32 + y],
                    shared[(x + offset) * 32 + y],
                    shared[x * 32 + (y + offset)],
                    shared[(x + offset) * 32 + (y + offset)]);
                levels[level][(x / stride) * dim + (y / stride)] = v;
                if (level != levels.size() - 1)
                    shared[x * 32 + y] = v;
            }
        }
    }
    return levels;
}

std::array<Level, 5> compactPyramid(const Level& scratch) {
    assert(scratch.size() == 32u * 32u);
    Level shared(16u * 16u);
    std::array<Level, 5> levels{
        Level(16u * 16u), Level(8u * 8u), Level(4u * 4u),
        Level(2u * 2u), Level(1u)};

    for (uint32_t x = 0; x < 16; ++x) {
        for (uint32_t y = 0; y < 16; ++y) {
            const uint32_t sx = x * 2;
            const uint32_t sy = y * 2;
            const float v = reduce(
                scratch[sx * 32 + sy],
                scratch[(sx + 1) * 32 + sy],
                scratch[sx * 32 + sy + 1],
                scratch[(sx + 1) * 32 + sy + 1]);
            levels[0][x * 16 + y] = v;
            shared[x * 16 + y] = v;
        }
    }

    const std::array<uint32_t, 4> offsets{1, 2, 4, 8};
    const std::array<uint32_t, 4> strides{2, 4, 8, 16};
    const std::array<uint32_t, 4> dims{8, 4, 2, 1};
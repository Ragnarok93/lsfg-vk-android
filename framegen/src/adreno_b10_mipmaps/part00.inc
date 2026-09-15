#include "adreno_b10_mipmaps.hpp"
#ifndef LSFGVK_B10_SELF_TEST
#include "core/device.hpp"
#include <volk.h>
#endif

#include <spirv_module.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <initializer_list>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace LSFG::Optimizations::AdrenoB10 {
namespace {

constexpr uint32_t kSpirvMagic = 0x07230203u;
constexpr uint32_t kQualcommVendorId = 0x5143u;
constexpr uint32_t kExpectedLocalX = 32u;
constexpr uint32_t kExpectedLocalY = 32u;
constexpr uint32_t kExpectedLocalZ = 1u;
constexpr size_t kExpectedHeadBytes = 28832u;
constexpr uint32_t kExpectedHeadBound = 1270u;
constexpr uint64_t kExpectedHeadFnv = 0x65d3c6a69e9f9b07ULL;

struct Inst {
    size_t offset{};
    uint16_t wordCount{};
    spv::Op op{spv::OpNop};
};

struct Parsed {
    std::vector<Inst> instructions;
    std::unordered_map<uint32_t, uint32_t> bindingById;
    std::unordered_map<uint32_t, uint32_t> setById;
    std::unordered_map<uint32_t, uint32_t> typeById;
    std::unordered_map<uint32_t, spv::StorageClass> pointerStorageByType;
    std::unordered_map<uint32_t, uint32_t> pointerPointeeByType;
    std::unordered_map<uint32_t, uint32_t> loadPointerByResult;
};

bool isImageSample(spv::Op op) {
    switch (op) {
    case spv::OpImageSampleImplicitLod:
    case spv::OpImageSampleExplicitLod:
    case spv::OpImageSampleDrefImplicitLod:
    case spv::OpImageSampleDrefExplicitLod:
    case spv::OpImageSampleProjImplicitLod:
    case spv::OpImageSampleProjExplicitLod:
    case spv::OpImageSampleProjDrefImplicitLod:
    case spv::OpImageSampleProjDrefExplicitLod:
        return true;
    default:
        return false;
    }
}

uint64_t fnv1a64(const std::vector<uint8_t>& bytes) {
    uint64_t hash = 1469598103934665603ULL;
    for (const uint8_t value : bytes) {
        hash ^= static_cast<uint64_t>(value);
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool parseModule(const std::vector<uint32_t>& words, Parsed& out) {
    if (words.size() < 5 || words[0] != kSpirvMagic)
        return false;

    size_t offset = 5;
    while (offset < words.size()) {
        const uint32_t first = words[offset];
        const uint16_t wc = static_cast<uint16_t>(first >> 16u);
        const auto op = static_cast<spv::Op>(first & 0xffffu);
        if (wc == 0 || offset + wc > words.size())
            return false;
        out.instructions.push_back({offset, wc, op});

        switch (op) {
        case spv::OpDecorate:
            if (wc >= 4) {
                const uint32_t target = words[offset + 1];
                const auto decoration = static_cast<spv::Decoration>(words[offset + 2]);
                if (decoration == spv::DecorationBinding)
                    out.bindingById[target] = words[offset + 3];
                else if (decoration == spv::DecorationDescriptorSet)
                    out.setById[target] = words[offset + 3];
            }
            break;
        case spv::OpTypePointer:
            if (wc >= 4) {
                const uint32_t resultId = words[offset + 1];
                out.pointerStorageByType[resultId] = static_cast<spv::StorageClass>(words[offset + 2]);
                out.pointerPointeeByType[resultId] = words[offset + 3];
            }
            break;
        case spv::OpVariable:
            if (wc >= 4)
                out.typeById[words[offset + 2]] = words[offset + 1];
            break;
        case spv::OpLoad:
            if (wc >= 4) {
                out.typeById[words[offset + 2]] = words[offset + 1];
                out.loadPointerByResult[words[offset + 2]] = words[offset + 3];
            }
            break;
        case spv::OpAccessChain:
        case spv::OpInBoundsAccessChain:
        case spv::OpPtrAccessChain:
            if (wc >= 4)
                out.typeById[words[offset + 2]] = words[offset + 1];
            break;
        default:
            break;
        }

        offset += wc;
    }
    return offset == words.size();
}

std::optional<spv::StorageClass> pointerStorage(
        const Parsed& parsed, uint32_t pointerId) {
    const auto typeIt = parsed.typeById.find(pointerId);
    if (typeIt == parsed.typeById.end())
        return std::nullopt;
    const auto storageIt = parsed.pointerStorageByType.find(typeIt->second);
    if (storageIt == parsed.pointerStorageByType.end())
        return std::nullopt;
    return storageIt->second;
}

std::optional<uint32_t> imageBinding(
        const Parsed& parsed, uint32_t imageId) {
    const auto loadIt = parsed.loadPointerByResult.find(imageId);
    if (loadIt == parsed.loadPointerByResult.end())
        return std::nullopt;
    const uint32_t variableId = loadIt->second;
    const auto bindingIt = parsed.bindingById.find(variableId);
    if (bindingIt == parsed.bindingById.end())
        return std::nullopt;
    const auto setIt = parsed.setById.find(variableId);
    if (setIt != parsed.setById.end() && setIt->second != 0)
        return std::nullopt;
    return bindingIt->second;
}

struct FunctionShape {
    uint32_t id{};
    size_t beginIndex{};
    size_t endIndex{};
    uint32_t samples{};
    uint32_t writes{};
    uint32_t barriers{};
};

std::vector<FunctionShape> functionShapes(
        const std::vector<uint32_t>& words, const Parsed& parsed) {
    std::vector<FunctionShape> result;
    std::optional<FunctionShape> current;
    for (size_t index = 0; index < parsed.instructions.size(); ++index) {
        const auto& inst = parsed.instructions[index];
        if (inst.op == spv::OpFunction) {
            if (current || inst.wordCount < 5)
                return {};
            current = FunctionShape{};
            current->id = words[inst.offset + 2];
            current->beginIndex = index;
        } else if (inst.op == spv::OpFunctionEnd) {
            if (!current)
                return {};
            current->endIndex = index;
            result.push_back(*current);
            current.reset();
        } else if (current) {
            if (isImageSample(inst.op))
                ++current->samples;
            else if (inst.op == spv::OpImageWrite)
                ++current->writes;
            else if (inst.op == spv::OpControlBarrier)
                ++current->barriers;
        }
    }
    if (current)
        return {};
    return result;
}

bool hasLocalSize32(const std::vector<uint32_t>& words, const Parsed& parsed) {
    for (const auto& inst : parsed.instructions) {
        if (inst.op != spv::OpExecutionMode || inst.wordCount < 6)
            continue;
        if (words[inst.offset + 2] != static_cast<uint32_t>(spv::ExecutionModeLocalSize))
            continue;
        return words[inst.offset + 3] == kExpectedLocalX
            && words[inst.offset + 4] == kExpectedLocalY
            && words[inst.offset + 5] == kExpectedLocalZ;
    }
    return false;
}

uint32_t countWorkgroupStores(
        const std::vector<uint32_t>& words, const Parsed& parsed,
        size_t beginInstruction, size_t endInstruction) {
    uint32_t count = 0;
    for (size_t i = beginInstruction; i < endInstruction; ++i) {
        const auto& inst = parsed.instructions[i];
        if (inst.op != spv::OpStore || inst.wordCount < 3)
            continue;
        const auto storage = pointerStorage(parsed, words[inst.offset + 1]);
        if (storage && *storage == spv::StorageClassWorkgroup)
            ++count;
    }
    return count;
}

void appendInstruction(std::vector<uint32_t>& dst,
        const std::vector<uint32_t>& src, const Inst& inst) {
    dst.insert(dst.end(), src.begin() + static_cast<std::ptrdiff_t>(inst.offset),
        src.begin() + static_cast<std::ptrdiff_t>(inst.offset + inst.wordCount));
}

void putOp(std::vector<uint32_t>& words, spv::Op op,
        std::initializer_list<uint32_t> operands) {
    const uint32_t wc = static_cast<uint32_t>(operands.size() + 1u);

#pragma once

#include <thirdparty/spirv.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace b14 {

inline constexpr const char* kMarker = "candidate-b14-mipmaps-tail-fusion";
inline constexpr size_t kBaselineBytes = 28832;
inline constexpr uint32_t kBaselineBound = 1270;
inline constexpr uint64_t kBaselineFnv = 0x65d3c6a69e9f9b07ULL;
inline constexpr size_t kCandidateBytes = 26708;
inline constexpr uint32_t kCandidateBound = 1383;
inline constexpr uint64_t kCandidateFnv = 0xbcfa18952e9a7739ULL;
inline constexpr uint32_t kAppliedMagic = 0xb1400001U;

struct RewriteReport {
    bool applied{};
    bool alreadyApplied{};
    size_t barriersBefore{};
    size_t barriersAfter{};
    size_t tailDynamicLoadsBefore{};
    size_t tailDynamicLoadsAfter{};
    size_t tailWorkgroupStoresBefore{};
    size_t tailWorkgroupStoresAfter{};
    size_t imageWritesBefore{};
    size_t imageWritesAfter{};
    std::string reason;
    std::string marker{kMarker};
};

namespace detail {

struct Instruction {
    size_t word{};
    uint16_t wordCount{};
    uint16_t opCode{};
};

inline uint32_t word(const std::vector<uint8_t>& code, size_t index) {
    uint32_t value = 0;
    std::memcpy(&value, code.data() + index * sizeof(uint32_t), sizeof(value));
    return value;
}

inline uint64_t fnv(const std::vector<uint8_t>& code) {
    uint64_t hash = 1469598103934665603ULL;
    for (const uint8_t value : code) {
        hash ^= static_cast<uint64_t>(value);
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline bool parse(const std::vector<uint8_t>& code,
        std::vector<Instruction>& instructions) {
    if (code.size() < 5U * sizeof(uint32_t)
            || code.size() % sizeof(uint32_t) != 0
            || word(code, 0) != 0x07230203U)
        return false;
    instructions.clear();
    const size_t words = code.size() / sizeof(uint32_t);
    for (size_t cursor = 5; cursor < words;) {
        const uint32_t first = word(code, cursor);
        const uint16_t count = static_cast<uint16_t>(first >> 16U);
        const uint16_t op = static_cast<uint16_t>(first & 0xffffU);
        if (count == 0 || cursor + count > words)
            return false;
        instructions.push_back({cursor, count, op});
        cursor += count;
    }
    return !instructions.empty();
}

inline size_t countOp(const std::vector<Instruction>& instructions, spv::Op op) {
    return static_cast<size_t>(std::count_if(
        instructions.begin(), instructions.end(), [op](const Instruction& ins) {
            return ins.opCode == static_cast<uint16_t>(op);
        }));
}

inline bool hasLocalSize32(const std::vector<uint8_t>& code,
        const std::vector<Instruction>& instructions) {
    size_t matches = 0;
    for (const auto& ins : instructions) {
        if (ins.opCode == static_cast<uint16_t>(spv::OpExecutionMode)
                && ins.wordCount == 6
                && word(code, ins.word + 2)
                    == static_cast<uint32_t>(spv::ExecutionModeLocalSize)
                && word(code, ins.word + 3) == 32U
                && word(code, ins.word + 4) == 32U
                && word(code, ins.word + 5) == 1U)
            ++matches;
    }
    return matches == 1;
}

inline bool hasAppliedMagic(const std::vector<uint8_t>& code,
        const std::vector<Instruction>& instructions) {
    for (const auto& ins : instructions) {
        if (ins.opCode == static_cast<uint16_t>(spv::OpConstant)
                && ins.wordCount == 4
                && word(code, ins.word + 1) == 10U
                && word(code, ins.word + 3) == kAppliedMagic)
            return true;
    }
    return false;
}

inline void emit(std::vector<uint32_t>& out, spv::Op op,
        std::initializer_list<uint32_t> operands) {
    const uint32_t count = static_cast<uint32_t>(operands.size() + 1U);
    out.push_back((count << 16U) | static_cast<uint32_t>(op));
    out.insert(out.end(), operands.begin(), operands.end());
}

inline void appendWords(std::vector<uint32_t>& out,
        const std::vector<uint8_t>& code, size_t begin, size_t end) {
    for (size_t index = begin; index < end; ++index)
        out.push_back(word(code, index));
}

inline std::vector<uint8_t> bytes(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> out(words.size() * sizeof(uint32_t));
    std::memcpy(out.data(), words.data(), out.size());
    return out;
}

} // namespace detail

inline RewriteReport fuseTail(std::vector<uint8_t>& code) {
    using detail::Instruction;
    RewriteReport report;
    std::vector<Instruction> instructions;
    if (!detail::parse(code, instructions)) {
        report.reason = "invalid-spirv";
        return report;
    }

    report.barriersBefore = detail::countOp(instructions, spv::OpControlBarrier);
    report.imageWritesBefore = detail::countOp(instructions, spv::OpImageWrite);
    if (detail::hasAppliedMagic(code, instructions)) {
        if (code.size() == kCandidateBytes
                && detail::word(code, 3) == kCandidateBound
                && detail::fnv(code) == kCandidateFnv
                && report.barriersBefore == 4U && report.imageWritesBefore == 13U
                && detail::hasLocalSize32(code, instructions)) {
            report.alreadyApplied = true;
            report.barriersAfter = report.barriersBefore;
            report.imageWritesAfter = 10;
            report.tailDynamicLoadsBefore = 15;
            report.tailDynamicLoadsAfter = 15;
            report.reason = "already-applied";
        } else {
            report.reason = "candidate-fingerprint-mismatch";
        }
        return report;
    }

    if (code.size() != kBaselineBytes || detail::word(code, 3) != kBaselineBound
            || detail::fnv(code) != kBaselineFnv) {
        report.reason = "baseline-fingerprint-mismatch";
        return report;
    }
    if (report.barriersBefore != 5U || report.imageWritesBefore != 10U
            || !detail::hasLocalSize32(code, instructions)) {
        report.reason = "baseline-structure-mismatch";
        return report;
    }

    std::vector<const Instruction*> barriers;
    const Instruction* firstFunction = nullptr;
    const Instruction* mainReturn = nullptr;
    for (const auto& ins : instructions) {
        if (ins.opCode == static_cast<uint16_t>(spv::OpFunction)
                && firstFunction == nullptr)
            firstFunction = &ins;
        if (ins.opCode == static_cast<uint16_t>(spv::OpControlBarrier))
            barriers.push_back(&ins);
    }
    if (firstFunction == nullptr || barriers.size() != 5U) {
        report.reason = "control-flow-anchor-mismatch";
        return report;
    }
    const size_t fourthBarrierEnd = barriers.at(3)->word + barriers.at(3)->wordCount;
    for (const auto& ins : instructions) {
        if (ins.word > barriers.at(4)->word
                && ins.opCode == static_cast<uint16_t>(spv::OpReturn)) {
            mainReturn = &ins;
            break;
        }
    }
    if (mainReturn == nullptr || fourthBarrierEnd >= mainReturn->word) {
        report.reason = "tail-anchor-mismatch";
        return report;
    }

    uint32_t nextId = detail::word(code, 3);
    auto id = [&nextId]() { return nextId++; };
    std::unordered_map<uint32_t, uint32_t> indexIds;
    const uint32_t markerId = id();
    const std::vector<uint32_t> sharedIndices{
            8U, 16U, 24U, 256U, 264U, 272U, 280U, 512U,
            520U, 528U, 536U, 768U, 776U, 784U, 792U};
    for (const uint32_t value : sharedIndices)
        indexIds.emplace(value, id());
    const uint32_t offset10 = id();
    const uint32_t offset01 = id();
    const uint32_t offset11 = id();

    std::vector<uint32_t> out;
    out.reserve(code.size() / sizeof(uint32_t) + 320U);
    detail::appendWords(out, code, 0, firstFunction->word);
    detail::emit(out, spv::OpConstant, {10U, markerId, kAppliedMagic});
    for (const uint32_t value : sharedIndices)
        detail::emit(out, spv::OpConstant, {10U, indexIds.at(value), value});
    detail::emit(out, spv::OpConstantComposite, {558U, offset10, 88U, 90U});
    detail::emit(out, spv::OpConstantComposite, {558U, offset01, 90U, 88U});
    detail::emit(out, spv::OpConstantComposite, {558U, offset11, 88U, 88U});
    detail::appendWords(out, code, firstFunction->word, fourthBarrierEnd);

    const uint32_t localId = id();
    const uint32_t localX = id();
    const uint32_t localY = id();
    const uint32_t xZero = id();
    const uint32_t yZero = id();
    const uint32_t laneZero = id();
    const uint32_t bodyLabel = id();
    const uint32_t mergeLabel = id();
    detail::emit(out, spv::OpLoad, {32U, localId, 34U});
    detail::emit(out, spv::OpCompositeExtract, {10U, localX, localId, 0U});
    detail::emit(out, spv::OpCompositeExtract, {10U, localY, localId, 1U});
    detail::emit(out, spv::OpIEqual, {48U, xZero, localX, 40U});
    detail::emit(out, spv::OpIEqual, {48U, yZero, localY, 40U});
    detail::emit(out, spv::OpLogicalAnd, {48U, laneZero, xZero, yZero});
    detail::emit(out, spv::OpSelectionMerge, {mergeLabel, 0U});
    detail::emit(out, spv::OpBranchConditional, {laneZero, bodyLabel, mergeLabel});
    detail::emit(out, spv::OpLabel, {bodyLabel});

    const uint32_t carriedVector = id();
    const uint32_t shared0 = id();
    detail::emit(out, spv::OpLoad, {9U, carriedVector, 59U});
    detail::emit(out, spv::OpCompositeExtract, {8U, shared0, carriedVector, 1U});

    std::unordered_map<uint32_t, uint32_t> sharedValues{{0U, shared0}};
    const auto loadShared = [&](uint32_t index) {
        const uint32_t pointer = id();
        const uint32_t bits = id();
        const uint32_t value = id();
        detail::emit(out, spv::OpAccessChain,
            {628U, pointer, 39U, indexIds.at(index)});
        detail::emit(out, spv::OpLoad, {10U, bits, pointer, 32U});
        detail::emit(out, spv::OpBitcast, {8U, value, bits});
        sharedValues.emplace(index, value);
    };
    for (const uint32_t index : {
            256U, 8U, 264U,
            512U, 768U, 520U, 776U,
            16U, 272U, 24U, 280U,
            528U, 784U, 536U, 792U})
        loadShared(index);

    const uint32_t quarterValue = id();
    detail::emit(out, spv::OpBitcast, {8U, quarterValue, 606U});
    const auto reduce4 = [&](uint32_t self, uint32_t right,
            uint32_t lower, uint32_t diagonal) {
        const uint32_t sum0 = id();
        const uint32_t sum1 = id();
        const uint32_t sum2 = id();
        const uint32_t average = id();
        detail::emit(out, spv::OpFAdd, {8U, sum0, right, self});
        detail::emit(out, spv::OpFAdd, {8U, sum1, lower, sum0});
        detail::emit(out, spv::OpFAdd, {8U, sum2, diagonal, sum1});
        detail::emit(out, spv::OpFMul, {8U, average, sum2, quarterValue});
        return average;
    };
    const uint32_t mip5_00 = reduce4(sharedValues.at(0U), sharedValues.at(256U),
        sharedValues.at(8U), sharedValues.at(264U));
    const uint32_t mip5_10 = reduce4(sharedValues.at(512U), sharedValues.at(768U),
        sharedValues.at(520U), sharedValues.at(776U));
    const uint32_t mip5_01 = reduce4(sharedValues.at(16U), sharedValues.at(272U),
        sharedValues.at(24U), sharedValues.at(280U));
    const uint32_t mip5_11 = reduce4(sharedValues.at(528U), sharedValues.at(784U),
        sharedValues.at(536U), sharedValues.at(792U));

    const uint32_t globalId = id();
    const uint32_t globalXYXY = id();
    const uint32_t mip5Shifted = id();
    const uint32_t mip5Signed4 = id();
    const uint32_t mip5Base = id();
    detail::emit(out, spv::OpLoad, {32U, globalId, 35U});
    detail::emit(out, spv::OpVectorShuffle,
        {63U, globalXYXY, globalId, globalId, 0U, 1U, 1U, 1U});
    detail::emit(out, spv::OpShiftRightLogical,
        {63U, mip5Shifted, globalXYXY, 1144U});
    detail::emit(out, spv::OpBitcast, {65U, mip5Signed4, mip5Shifted});
    detail::emit(out, spv::OpVectorShuffle,
        {558U, mip5Base, mip5Signed4, mip5Signed4, 0U, 1U});
    const uint32_t mip5Image = id();
    detail::emit(out, spv::OpLoad, {23U, mip5Image, 30U});

    const auto writeMip5 = [&](uint32_t value, uint32_t offset) {
        uint32_t coordinate = mip5Base;
        if (offset != 0U) {
            coordinate = id();
            detail::emit(out, spv::OpIAdd,
                {558U, coordinate, mip5Base, offset});
        }
        const uint32_t payload = id();
        detail::emit(out, spv::OpCompositeConstruct,
            {9U, payload, value, value, value, value});
        detail::emit(out, spv::OpImageWrite, {mip5Image, coordinate, payload});
    };
    writeMip5(mip5_00, 0U);
    writeMip5(mip5_10, offset10);
    writeMip5(mip5_01, offset01);
    writeMip5(mip5_11, offset11);

    const uint32_t mip6Sum0 = id();
    const uint32_t mip6Sum1 = id();
    const uint32_t mip6Sum2 = id();
    const uint32_t mip6Value = id();
    detail::emit(out, spv::OpFAdd, {8U, mip6Sum0, mip5_10, mip5_00});
    detail::emit(out, spv::OpFAdd, {8U, mip6Sum1, mip5_01, mip6Sum0});
    detail::emit(out, spv::OpFAdd, {8U, mip6Sum2, mip5_11, mip6Sum1});
    detail::emit(out, spv::OpFMul, {8U, mip6Value, mip6Sum2, quarterValue});
    const uint32_t mip6Shifted = id();
    const uint32_t mip6Signed4 = id();
    const uint32_t mip6Coordinate = id();
    detail::emit(out, spv::OpShiftRightLogical,
        {63U, mip6Shifted, globalXYXY, 1259U});
    detail::emit(out, spv::OpBitcast, {65U, mip6Signed4, mip6Shifted});
    detail::emit(out, spv::OpVectorShuffle,
        {558U, mip6Coordinate, mip6Signed4, mip6Signed4, 0U, 1U});
    const uint32_t mip6Payload = id();
    const uint32_t mip6Image = id();
    detail::emit(out, spv::OpCompositeConstruct,
        {9U, mip6Payload, mip6Value, mip6Value, mip6Value, mip6Value});
    detail::emit(out, spv::OpLoad, {23U, mip6Image, 31U});
    detail::emit(out, spv::OpImageWrite, {mip6Image, mip6Coordinate, mip6Payload});
    detail::emit(out, spv::OpBranch, {mergeLabel});
    detail::emit(out, spv::OpLabel, {mergeLabel});
    detail::appendWords(out, code, mainReturn->word, code.size() / sizeof(uint32_t));
    out.at(3) = nextId;

    std::vector<uint8_t> candidate = detail::bytes(out);
    std::vector<Instruction> candidateInstructions;
    if (!detail::parse(candidate, candidateInstructions)) {
        report.reason = "candidate-invalid-spirv";
        return report;
    }
    if (detail::countOp(candidateInstructions, spv::OpControlBarrier) != 4U) {
        report.reason = "candidate-barrier-count-mismatch";
        return report;
    }
    if (detail::countOp(candidateInstructions, spv::OpImageWrite) != 13U) {
        report.reason = "candidate-image-write-count-mismatch";
        return report;
    }
    if (!detail::hasLocalSize32(candidate, candidateInstructions)) {
        report.reason = "candidate-local-size-mismatch";
        return report;
    }
    if (!detail::hasAppliedMagic(candidate, candidateInstructions)) {
        report.reason = "candidate-marker-missing";
        return report;
    }

    code.swap(candidate);
    report.applied = true;
    report.barriersAfter = 4;
    report.tailDynamicLoadsBefore = 15;
    report.tailDynamicLoadsAfter = 15;
    report.tailWorkgroupStoresBefore = 4;
    report.tailWorkgroupStoresAfter = 0;
    report.imageWritesAfter = 10;
    report.reason = "applied";
    return report;
}

} // namespace b14

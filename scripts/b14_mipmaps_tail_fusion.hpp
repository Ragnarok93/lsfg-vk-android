#pragma once

#include <thirdparty/spirv.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace b14 {

inline constexpr const char* kMarker = "candidate-b14-mipmaps-tail-fusion";
inline constexpr size_t kBaselineBytes = 28832;
inline constexpr uint32_t kBaselineBound = 1270;
inline constexpr uint64_t kBaselineFnv = 0x65d3c6a69e9f9b07ULL;
inline constexpr uint32_t kAppliedMagic = 0xb1400001U;

struct RewriteReport {
    bool applied{};
    bool alreadyApplied{};
    size_t barriersBefore{};
    size_t barriersAfter{};
    size_t tailDynamicLoadsBefore{};
    size_t tailDynamicLoadsAfter{};
    size_t tailCriticalPathLoadsBefore{};
    size_t tailCriticalPathLoadsAfter{};
    size_t tailParallelLanesAfter{};
    size_t subgroupBroadcastsAfter{};
    size_t staticWorkgroupLoadsBefore{};
    size_t staticWorkgroupLoadsAfter{};
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

inline bool hasExactCapabilities(const std::vector<uint8_t>& code,
        const std::vector<Instruction>& instructions,
        std::vector<uint32_t> expected) {
    std::vector<uint32_t> actual;
    for (const auto& ins : instructions) {
        if (ins.opCode == static_cast<uint16_t>(spv::OpCapability)
                && ins.wordCount == 2U)
            actual.push_back(word(code, ins.word + 1));
    }
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    return actual == expected;
}

inline std::pair<size_t, size_t> staticWorkgroupAccessCounts(
        const std::vector<uint8_t>& code,
        const std::vector<Instruction>& instructions) {
    std::unordered_set<uint32_t> sharedPointers{39U};
    size_t loads = 0;
    size_t stores = 0;
    for (const auto& ins : instructions) {
        const bool accessChain =
            ins.opCode == static_cast<uint16_t>(spv::OpAccessChain)
            || ins.opCode == static_cast<uint16_t>(spv::OpInBoundsAccessChain);
        if (accessChain && ins.wordCount >= 4U
                && sharedPointers.count(word(code, ins.word + 3)) != 0U) {
            sharedPointers.insert(word(code, ins.word + 2));
        } else if (ins.opCode == static_cast<uint16_t>(spv::OpLoad)
                && ins.wordCount >= 4U
                && sharedPointers.count(word(code, ins.word + 3)) != 0U) {
            ++loads;
        } else if (ins.opCode == static_cast<uint16_t>(spv::OpStore)
                && ins.wordCount >= 3U
                && sharedPointers.count(word(code, ins.word + 1)) != 0U) {
            ++stores;
        }
    }
    return {loads, stores};
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
    const auto baselineAccess =
        detail::staticWorkgroupAccessCounts(code, instructions);
    report.staticWorkgroupLoadsBefore = baselineAccess.first;

    if (detail::hasAppliedMagic(code, instructions)) {
        const auto candidateAccess =
            detail::staticWorkgroupAccessCounts(code, instructions);
        const bool structuralCandidate =
            report.barriersBefore == 4U
            && report.imageWritesBefore == 10U
            && detail::countOp(instructions, spv::OpGroupNonUniformBroadcast) == 4U
            && candidateAccess == std::pair<size_t, size_t>{12U, 4U}
            && detail::hasExactCapabilities(code, instructions, {
                1U, 50U, 56U, 61U, 64U, 5345U
            })
            && detail::hasLocalSize32(code, instructions);
        if (structuralCandidate) {
            report.alreadyApplied = true;
            report.barriersAfter = 4U;
            report.tailDynamicLoadsBefore = 15U;
            report.tailDynamicLoadsAfter = 12U;
            report.tailCriticalPathLoadsBefore = 6U;
            report.tailCriticalPathLoadsAfter = 3U;
            report.tailParallelLanesAfter = 4U;
            report.subgroupBroadcastsAfter = 4U;
            report.staticWorkgroupLoadsBefore = 15U;
            report.staticWorkgroupLoadsAfter = 12U;
            report.tailWorkgroupStoresBefore = 4U;
            report.tailWorkgroupStoresAfter = 0U;
            report.imageWritesAfter = 10U;
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
            || baselineAccess != std::pair<size_t, size_t>{15U, 5U}
            || !detail::hasExactCapabilities(code, instructions, {
                1U, 50U, 56U, 5345U
            })
            || !detail::hasLocalSize32(code, instructions)) {
        report.reason = "baseline-structure-mismatch";
        return report;
    }

    std::vector<const Instruction*> barriers;
    const Instruction* firstFunction = nullptr;
    const Instruction* firstType = nullptr;
    const Instruction* mainReturn = nullptr;
    const Instruction* preTailLabel = nullptr;
    for (const auto& ins : instructions) {
        if (ins.opCode == static_cast<uint16_t>(spv::OpFunction)
                && firstFunction == nullptr)
            firstFunction = &ins;
        if (ins.opCode == static_cast<uint16_t>(spv::OpTypeVoid)
                && firstType == nullptr)
            firstType = &ins;
        if (ins.opCode == static_cast<uint16_t>(spv::OpControlBarrier))
            barriers.push_back(&ins);
    }
    if (firstFunction == nullptr || firstType == nullptr || barriers.size() != 5U) {
        report.reason = "control-flow-anchor-mismatch";
        return report;
    }
    const size_t fourthBarrierEnd = barriers.at(3)->word + barriers.at(3)->wordCount;
    for (const auto& ins : instructions) {
        if (ins.word < fourthBarrierEnd
                && ins.opCode == static_cast<uint16_t>(spv::OpLabel))
            preTailLabel = &ins;
        if (ins.word > barriers.at(4)->word
                && ins.opCode == static_cast<uint16_t>(spv::OpReturn)) {
            mainReturn = &ins;
            break;
        }
    }
    if (mainReturn == nullptr || preTailLabel == nullptr
            || fourthBarrierEnd >= mainReturn->word) {
        report.reason = "tail-anchor-mismatch";
        return report;
    }

    uint32_t nextId = detail::word(code, 3);
    auto id = [&nextId]() { return nextId++; };

    const uint32_t markerId = id();
    const uint32_t inputUintPtrType = id();
    const uint32_t subgroupIdVar = id();
    const uint32_t subgroupLocalIdVar = id();
    const uint32_t const256 = id();
    const uint32_t const264 = id();
    const uint32_t const512 = id();

    std::vector<uint32_t> out;
    out.reserve(code.size() / sizeof(uint32_t) + 240U);
    detail::appendWords(out, code, 0, 5U);

    bool addedCapabilities = false;
    bool addedDecorations = false;
    for (const auto& ins : instructions) {
        if (ins.word >= firstFunction->word)
            break;

        if (!addedCapabilities
                && ins.opCode != static_cast<uint16_t>(spv::OpCapability)) {
            detail::emit(out, spv::OpCapability, {
                static_cast<uint32_t>(spv::CapabilityGroupNonUniform)
            });
            detail::emit(out, spv::OpCapability, {
                static_cast<uint32_t>(spv::CapabilityGroupNonUniformBallot)
            });
            addedCapabilities = true;
        }

        if (!addedDecorations && ins.word == firstType->word) {
            detail::emit(out, spv::OpDecorate, {
                subgroupIdVar,
                static_cast<uint32_t>(spv::DecorationBuiltIn),
                static_cast<uint32_t>(spv::BuiltInSubgroupId)
            });
            detail::emit(out, spv::OpDecorate, {
                subgroupLocalIdVar,
                static_cast<uint32_t>(spv::DecorationBuiltIn),
                static_cast<uint32_t>(spv::BuiltInSubgroupLocalInvocationId)
            });
            addedDecorations = true;
        }

        if (ins.opCode == static_cast<uint16_t>(spv::OpEntryPoint)
                && ins.wordCount >= 4U
                && detail::word(code, ins.word + 1)
                    == static_cast<uint32_t>(spv::ExecutionModelGLCompute)) {
            const uint32_t newWordCount =
                static_cast<uint32_t>(ins.wordCount) + 2U;
            out.push_back((newWordCount << 16U)
                | static_cast<uint32_t>(spv::OpEntryPoint));
            detail::appendWords(out, code, ins.word + 1U,
                ins.word + ins.wordCount);
            out.push_back(subgroupIdVar);
            out.push_back(subgroupLocalIdVar);
        } else {
            detail::appendWords(out, code, ins.word, ins.word + ins.wordCount);
        }
    }
    if (!addedCapabilities || !addedDecorations) {
        report.reason = "declaration-anchor-mismatch";
        return report;
    }

    detail::emit(out, spv::OpTypePointer, {
        inputUintPtrType,
        static_cast<uint32_t>(spv::StorageClassInput),
        10U
    });
    detail::emit(out, spv::OpConstant, {10U, markerId, kAppliedMagic});
    detail::emit(out, spv::OpConstant, {10U, const256, 256U});
    detail::emit(out, spv::OpConstant, {10U, const264, 264U});
    detail::emit(out, spv::OpConstant, {10U, const512, 512U});
    detail::emit(out, spv::OpVariable, {
        inputUintPtrType, subgroupIdVar,
        static_cast<uint32_t>(spv::StorageClassInput)
    });
    detail::emit(out, spv::OpVariable, {
        inputUintPtrType, subgroupLocalIdVar,
        static_cast<uint32_t>(spv::StorageClassInput)
    });

    detail::appendWords(out, code, firstFunction->word, fourthBarrierEnd);

    const uint32_t localId = id();
    const uint32_t globalId = id();
    const uint32_t workgroupOrigin = id();
    const uint32_t subgroupId = id();
    const uint32_t subgroupLocalId = id();
    const uint32_t subgroupZero = id();
    const uint32_t subgroupLocalLt4 = id();
    const uint32_t activeLane = id();

    detail::emit(out, spv::OpLoad, {32U, localId, 34U});
    detail::emit(out, spv::OpLoad, {32U, globalId, 35U});
    detail::emit(out, spv::OpISub, {32U, workgroupOrigin, globalId, localId});
    detail::emit(out, spv::OpLoad, {10U, subgroupId, subgroupIdVar});
    detail::emit(out, spv::OpLoad, {10U, subgroupLocalId, subgroupLocalIdVar});
    detail::emit(out, spv::OpIEqual, {48U, subgroupZero, subgroupId, 40U});
    detail::emit(out, spv::OpULessThan,
        {48U, subgroupLocalLt4, subgroupLocalId, 699U});
    detail::emit(out, spv::OpLogicalAnd,
        {48U, activeLane, subgroupZero, subgroupLocalLt4});

    const uint32_t quarterValue = id();
    const uint32_t originXYXY = id();
    detail::emit(out, spv::OpBitcast, {8U, quarterValue, 606U});
    detail::emit(out, spv::OpVectorShuffle,
        {63U, originXYXY, workgroupOrigin, workgroupOrigin, 0U, 1U, 1U, 1U});

    const uint32_t activeLabel = id();
    const uint32_t activeMergeLabel = id();
    detail::emit(out, spv::OpSelectionMerge, {activeMergeLabel, 0U});
    detail::emit(out, spv::OpBranchConditional,
        {activeLane, activeLabel, activeMergeLabel});
    detail::emit(out, spv::OpLabel, {activeLabel});

    const uint32_t roleX = id();
    const uint32_t roleY = id();
    const uint32_t roleXBase = id();
    const uint32_t roleYBase = id();
    const uint32_t sharedBase = id();
    detail::emit(out, spv::OpBitwiseAnd,
        {10U, roleX, subgroupLocalId, 67U});
    detail::emit(out, spv::OpShiftRightLogical,
        {10U, roleY, subgroupLocalId, 67U});
    detail::emit(out, spv::OpIMul, {10U, roleXBase, roleX, const512});
    detail::emit(out, spv::OpIMul, {10U, roleYBase, roleY, 960U});
    detail::emit(out, spv::OpIAdd, {10U, sharedBase, roleXBase, roleYBase});

    const auto loadShared = [&](uint32_t indexId, bool addToBase) {
        uint32_t index = sharedBase;
        if (addToBase) {
            index = id();
            detail::emit(out, spv::OpIAdd,
                {10U, index, sharedBase, indexId});
        }
        const uint32_t pointer = id();
        const uint32_t bits = id();
        const uint32_t value = id();
        detail::emit(out, spv::OpAccessChain,
            {628U, pointer, 39U, index});
        detail::emit(out, spv::OpLoad, {10U, bits, pointer, 32U});
        detail::emit(out, spv::OpBitcast, {8U, value, bits});
        return value;
    };

    // The baseline carries the current lane's mip4 value in component 1 of
    // function state across the fourth barrier. Reuse it instead of issuing
    // a fourth Workgroup load for every active tail lane.
    const uint32_t mip4State = id();
    const uint32_t sharedSelf = id();
    detail::emit(out, spv::OpLoad, {9U, mip4State, 59U});
    detail::emit(out, spv::OpCompositeExtract,
        {8U, sharedSelf, mip4State, 1U});
    const uint32_t sharedRight = loadShared(const256, true);
    const uint32_t sharedLower = loadShared(819U, true);
    const uint32_t sharedDiagonal = loadShared(const264, true);

    const uint32_t mip5Sum0 = id();
    const uint32_t mip5Sum1 = id();
    const uint32_t mip5Sum2 = id();
    const uint32_t mip5Value = id();
    detail::emit(out, spv::OpFAdd,
        {8U, mip5Sum0, sharedRight, sharedSelf});
    detail::emit(out, spv::OpFAdd,
        {8U, mip5Sum1, sharedLower, mip5Sum0});
    detail::emit(out, spv::OpFAdd,
        {8U, mip5Sum2, sharedDiagonal, mip5Sum1});
    detail::emit(out, spv::OpFMul,
        {8U, mip5Value, mip5Sum2, quarterValue});

    const uint32_t mip5Shifted = id();
    const uint32_t mip5Signed4 = id();
    const uint32_t mip5Base = id();
    detail::emit(out, spv::OpShiftRightLogical,
        {63U, mip5Shifted, originXYXY, 1144U});
    detail::emit(out, spv::OpBitcast, {65U, mip5Signed4, mip5Shifted});
    detail::emit(out, spv::OpVectorShuffle,
        {558U, mip5Base, mip5Signed4, mip5Signed4, 0U, 1U});

    const uint32_t roleXSigned = id();
    const uint32_t roleYSigned = id();
    const uint32_t mip5Offset = id();
    const uint32_t mip5Coordinate = id();
    detail::emit(out, spv::OpBitcast, {41U, roleXSigned, roleX});
    detail::emit(out, spv::OpBitcast, {41U, roleYSigned, roleY});
    detail::emit(out, spv::OpCompositeConstruct,
        {558U, mip5Offset, roleXSigned, roleYSigned});
    detail::emit(out, spv::OpIAdd,
        {558U, mip5Coordinate, mip5Base, mip5Offset});

    const uint32_t mip5Payload = id();
    const uint32_t mip5Image = id();
    detail::emit(out, spv::OpCompositeConstruct,
        {9U, mip5Payload, mip5Value, mip5Value, mip5Value, mip5Value});
    detail::emit(out, spv::OpLoad, {23U, mip5Image, 30U});
    detail::emit(out, spv::OpImageWrite,
        {mip5Image, mip5Coordinate, mip5Payload});
    detail::emit(out, spv::OpBranch, {activeMergeLabel});
    detail::emit(out, spv::OpLabel, {activeMergeLabel});

    const uint32_t laneMip5Value = id();
    detail::emit(out, spv::OpPhi, {
        8U, laneMip5Value,
        mip5Value, activeLabel,
        55U, detail::word(code, preTailLabel->word + 1U)
    });

    const uint32_t mip5_00 = id();
    const uint32_t mip5_10 = id();
    const uint32_t mip5_01 = id();
    const uint32_t mip5_11 = id();
    detail::emit(out, spv::OpGroupNonUniformBroadcast,
        {8U, mip5_00, 644U, laneMip5Value, 40U});
    detail::emit(out, spv::OpGroupNonUniformBroadcast,
        {8U, mip5_10, 644U, laneMip5Value, 67U});
    detail::emit(out, spv::OpGroupNonUniformBroadcast,
        {8U, mip5_01, 644U, laneMip5Value, 11U});
    detail::emit(out, spv::OpGroupNonUniformBroadcast,
        {8U, mip5_11, 644U, laneMip5Value, 644U});

    const uint32_t subgroupLocalZero = id();
    const uint32_t mip6Writer = id();
    detail::emit(out, spv::OpIEqual,
        {48U, subgroupLocalZero, subgroupLocalId, 40U});
    detail::emit(out, spv::OpLogicalAnd,
        {48U, mip6Writer, subgroupZero, subgroupLocalZero});

    const uint32_t mip6Label = id();
    const uint32_t mip6MergeLabel = id();
    detail::emit(out, spv::OpSelectionMerge, {mip6MergeLabel, 0U});
    detail::emit(out, spv::OpBranchConditional,
        {mip6Writer, mip6Label, mip6MergeLabel});
    detail::emit(out, spv::OpLabel, {mip6Label});

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
        {63U, mip6Shifted, originXYXY, 1259U});
    detail::emit(out, spv::OpBitcast, {65U, mip6Signed4, mip6Shifted});
    detail::emit(out, spv::OpVectorShuffle,
        {558U, mip6Coordinate, mip6Signed4, mip6Signed4, 0U, 1U});
    const uint32_t mip6Payload = id();
    const uint32_t mip6Image = id();
    detail::emit(out, spv::OpCompositeConstruct,
        {9U, mip6Payload, mip6Value, mip6Value, mip6Value, mip6Value});
    detail::emit(out, spv::OpLoad, {23U, mip6Image, 31U});
    detail::emit(out, spv::OpImageWrite,
        {mip6Image, mip6Coordinate, mip6Payload});
    detail::emit(out, spv::OpBranch, {mip6MergeLabel});
    detail::emit(out, spv::OpLabel, {mip6MergeLabel});

    detail::appendWords(out, code, mainReturn->word,
        code.size() / sizeof(uint32_t));
    out.at(3) = nextId;

    std::vector<uint8_t> candidate = detail::bytes(out);
    std::vector<Instruction> candidateInstructions;
    if (!detail::parse(candidate, candidateInstructions)) {
        report.reason = "candidate-invalid-spirv";
        return report;
    }
    const auto candidateAccess =
        detail::staticWorkgroupAccessCounts(candidate, candidateInstructions);
    if (detail::countOp(candidateInstructions, spv::OpControlBarrier) != 4U) {
        report.reason = "candidate-barrier-count-mismatch";
        return report;
    }
    if (detail::countOp(candidateInstructions, spv::OpImageWrite) != 10U) {
        report.reason = "candidate-image-write-count-mismatch";
        return report;
    }
    if (detail::countOp(candidateInstructions,
            spv::OpGroupNonUniformBroadcast) != 4U) {
        report.reason = "candidate-subgroup-broadcast-count-mismatch";
        return report;
    }
    if (candidateAccess != std::pair<size_t, size_t>{12U, 4U}) {
        report.reason = "candidate-workgroup-access-count-mismatch";
        return report;
    }
    if (!detail::hasExactCapabilities(candidate, candidateInstructions, {
            1U, 50U, 56U, 61U, 64U, 5345U
        })) {
        report.reason = "candidate-capability-mismatch";
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
    report.barriersAfter = 4U;
    report.tailDynamicLoadsBefore = 15U;
    report.tailDynamicLoadsAfter = 12U;
    report.tailCriticalPathLoadsBefore = 6U;
    report.tailCriticalPathLoadsAfter = 3U;
    report.tailParallelLanesAfter = 4U;
    report.subgroupBroadcastsAfter = 4U;
    report.staticWorkgroupLoadsBefore = 15U;
    report.staticWorkgroupLoadsAfter = 12U;
    report.tailWorkgroupStoresBefore = 4U;
    report.tailWorkgroupStoresAfter = 0U;
    report.imageWritesAfter = 10U;
    report.reason = "applied";
    return report;
}

} // namespace b14

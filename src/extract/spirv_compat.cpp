#include "extract/spirv_compat.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
constexpr uint32_t kSpirvMagic = 0x07230203u;
constexpr uint32_t kOpTypeImage = 25;
constexpr uint32_t kOpTypeSampler = 26;
constexpr uint32_t kOpTypeSampledImage = 27;
constexpr uint32_t kOpTypeStruct = 30;
constexpr uint32_t kOpTypePointer = 32;
constexpr uint32_t kOpFunction = 54;
constexpr uint32_t kOpVariable = 59;
constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kDecorationBinding = 33;
constexpr uint32_t kDecorationDescriptorSet = 34;
constexpr uint32_t kStorageClassUniformConstant = 0;
constexpr uint32_t kStorageClassUniform = 2;

enum class DescriptorKind : uint8_t {
    UniformBuffer = 0,
    Sampler = 1,
    SampledImage = 2,
    StorageImage = 3,
    Unknown = 0xff,
};

struct PointerInfo {
    uint32_t storageClass{};
    uint32_t pointeeType{};
};

struct VariableInfo {
    uint32_t pointerType{};
    uint32_t storageClass{};
};

struct BindingSite {
    uint32_t variableId{};
    uint32_t originalBinding{};
    size_t bindingWord{};
    size_t descriptorSetWord{};
    DescriptorKind kind{DescriptorKind::Unknown};
};

} // namespace

bool Extract::isSpirvBytecode(const std::vector<uint8_t>& bytecode) {
    if (bytecode.size() < sizeof(uint32_t))
        return false;
    uint32_t magic{};
    std::memcpy(&magic, bytecode.data(), sizeof(magic));
    return magic == kSpirvMagic;
}

std::vector<uint8_t> Extract::normalizePrecompiledSpirv(std::vector<uint8_t> bytecode) {
    if (bytecode.size() < 5 * sizeof(uint32_t) || bytecode.size() % sizeof(uint32_t) != 0)
        throw std::runtime_error("Malformed precompiled SPIR-V size");

    std::vector<uint32_t> words(bytecode.size() / sizeof(uint32_t));
    std::memcpy(words.data(), bytecode.data(), bytecode.size());
    if (words.front() != kSpirvMagic)
        throw std::runtime_error("Precompiled shader is not SPIR-V");

    std::unordered_map<uint32_t, DescriptorKind> typedDescriptors;
    std::unordered_set<uint32_t> structTypes;
    std::unordered_map<uint32_t, PointerInfo> pointers;
    std::unordered_map<uint32_t, VariableInfo> variables;
    std::unordered_map<uint32_t, size_t> descriptorSetWords;
    std::vector<BindingSite> bindings;
    std::unordered_set<uint32_t> boundVariables;

    size_t cursor = 5;
    while (cursor < words.size()) {
        const uint32_t header = words[cursor];
        const uint32_t wordCount = header >> 16;
        const uint32_t opcode = header & 0xffffu;
        if (wordCount == 0 || cursor + wordCount > words.size())
            throw std::runtime_error("Malformed SPIR-V instruction stream");
        if (opcode == kOpFunction)
            break;

        switch (opcode) {
            case kOpTypeSampler:
                if (wordCount >= 2)
                    typedDescriptors[words[cursor + 1]] = DescriptorKind::Sampler;
                break;
            case kOpTypeImage:
                if (wordCount >= 9) {
                    const uint32_t sampled = words[cursor + 7];
                    typedDescriptors[words[cursor + 1]] = sampled == 2
                        ? DescriptorKind::StorageImage
                        : DescriptorKind::SampledImage;
                }
                break;
            case kOpTypeSampledImage:
                if (wordCount >= 3)
                    typedDescriptors[words[cursor + 1]] = DescriptorKind::SampledImage;
                break;
            case kOpTypeStruct:
                if (wordCount >= 2)
                    structTypes.insert(words[cursor + 1]);
                break;
            case kOpTypePointer:
                if (wordCount == 4) {
                    pointers[words[cursor + 1]] = PointerInfo{
                        .storageClass = words[cursor + 2],
                        .pointeeType = words[cursor + 3],
                    };
                }
                break;
            case kOpVariable:
                if (wordCount >= 4) {
                    variables[words[cursor + 2]] = VariableInfo{
                        .pointerType = words[cursor + 1],
                        .storageClass = words[cursor + 3],
                    };
                }
                break;
            case kOpDecorate:
                if (wordCount == 4 && words[cursor + 2] == kDecorationBinding) {
                    const uint32_t variableId = words[cursor + 1];
                    if (!boundVariables.insert(variableId).second)
                        throw std::runtime_error("Duplicate SPIR-V Binding decoration");
                    bindings.push_back(BindingSite{
                        .variableId = variableId,
                        .originalBinding = words[cursor + 3],
                        .bindingWord = cursor + 3,
                    });
                } else if (wordCount == 4
                        && words[cursor + 2] == kDecorationDescriptorSet) {
                    descriptorSetWords[words[cursor + 1]] = cursor + 3;
                }
                break;
            default:
                break;
        }
        cursor += wordCount;
    }

    for (auto& binding : bindings) {
        const auto variable = variables.find(binding.variableId);
        if (variable == variables.end())
            throw std::runtime_error("Bound SPIR-V variable declaration is missing");
        const auto pointer = pointers.find(variable->second.pointerType);
        if (pointer == pointers.end())
            throw std::runtime_error("Bound SPIR-V pointer type is missing");
        const auto descriptorSet = descriptorSetWords.find(binding.variableId);
        if (descriptorSet == descriptorSetWords.end())
            throw std::runtime_error("Bound SPIR-V variable has no DescriptorSet decoration");
        binding.descriptorSetWord = descriptorSet->second;

        if (variable->second.storageClass == kStorageClassUniform
                && structTypes.contains(pointer->second.pointeeType)) {
            binding.kind = DescriptorKind::UniformBuffer;
            continue;
        }
        if (variable->second.storageClass == kStorageClassUniformConstant) {
            const auto typed = typedDescriptors.find(pointer->second.pointeeType);
            if (typed != typedDescriptors.end()) {
                binding.kind = typed->second;
                continue;
            }
        }
        throw std::runtime_error("Unsupported descriptor type in precompiled SPIR-V");
    }

    std::stable_sort(bindings.begin(), bindings.end(), [](const BindingSite& left,
            const BindingSite& right) {
        if (left.kind != right.kind)
            return static_cast<uint8_t>(left.kind) < static_cast<uint8_t>(right.kind);
        if (left.originalBinding != right.originalBinding)
            return left.originalBinding < right.originalBinding;
        return left.variableId < right.variableId;
    });

    for (size_t index = 0; index < bindings.size(); ++index) {
        words[bindings[index].bindingWord] = static_cast<uint32_t>(index);
        words[bindings[index].descriptorSetWord] = 0;
    }

    std::memcpy(bytecode.data(), words.data(), bytecode.size());
    return bytecode;
}

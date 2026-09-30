#include "core/spirv_compat.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace LSFG::Core {
namespace {

constexpr uint32_t kSpirvMagic = 0x07230203u;
constexpr size_t kHeaderWords = 5u;
constexpr uint32_t kWordCountShift = 16u;
constexpr uint32_t kOpcodeMask = 0xffffu;
constexpr uint32_t kSpirv14 = 0x00010400u;
constexpr uint32_t kSpirv15 = 0x00010500u;
constexpr uint32_t kSpirv16 = 0x00010600u;

constexpr uint32_t kOpExtension = 10u;
constexpr uint32_t kOpExtInstImport = 11u;
constexpr uint32_t kOpMemoryModel = 14u;
constexpr uint32_t kOpCapability = 17u;
constexpr uint32_t kOpFunction = 54u;

constexpr uint32_t kCapVulkanMemoryModel = 5345u;
constexpr uint32_t kCapVulkanMemoryModelDeviceScope = 5346u;
constexpr uint32_t kCapDemoteToHelperInvocation = 5379u;

bool validTarget(uint32_t target) {
    return target == kSpirv14 || target == kSpirv15 || target == kSpirv16;
}

std::vector<uint32_t> bytesToWords(const std::vector<uint8_t>& bytes) {
    std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
    if (!words.empty())
        std::memcpy(words.data(), bytes.data(), bytes.size());
    return words;
}

std::vector<uint8_t> wordsToBytes(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> bytes(words.size() * sizeof(uint32_t));
    if (!bytes.empty())
        std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}

std::vector<uint32_t> encodeExtension(const char* name) {
    const size_t length = std::strlen(name) + 1;
    const size_t stringWords = (length + 3) / 4;
    std::vector<uint32_t> instruction(1 + stringWords, 0u);
    instruction[0] =
        (static_cast<uint32_t>(1 + stringWords) << kWordCountShift) | kOpExtension;
    std::memcpy(&instruction[1], name, length);
    return instruction;
}

bool hasExtension(const std::vector<uint32_t>& words, const char* expected) {
    size_t offset = kHeaderWords;
    while (offset < words.size()) {
        const uint32_t count = words[offset] >> kWordCountShift;
        const uint32_t opcode = words[offset] & kOpcodeMask;
        if (count == 0 || offset + count > words.size())
            return false;
        if (opcode == kOpMemoryModel || opcode == kOpFunction)
            break;
        if (opcode == kOpExtension && count >= 2) {
            const auto* value =
                reinterpret_cast<const char*>(&words[offset + 1]);
            const size_t bytes = static_cast<size_t>(count - 1) * sizeof(uint32_t);
            if (std::strncmp(value, expected, bytes) == 0)
                return true;
        }
        offset += count;
    }
    return false;
}

} // namespace

SpirvCompatibilityResult prepareSpirvForTarget(
        const std::vector<uint8_t>& source, uint32_t targetVersion) {
    SpirvCompatibilityResult result{};
    result.targetVersion = targetVersion;

    if (!validTarget(targetVersion)) {
        result.rejectionReason = "unsupported SPIR-V target";
        return result;
    }
    if (source.size() < kHeaderWords * sizeof(uint32_t)
            || source.size() % sizeof(uint32_t) != 0) {
        result.rejectionReason = "SPIR-V module has invalid byte length";
        return result;
    }

    auto words = bytesToWords(source);
    if (words[0] != kSpirvMagic) {
        result.rejectionReason = "SPIR-V module has invalid magic";
        return result;
    }
    result.sourceVersion = words[1];
    if (result.sourceVersion < kSpirv14 || result.sourceVersion > kSpirv16) {
        result.rejectionReason =
            "SPIR-V module version is outside audited 1.4-1.6 range";
        return result;
    }

    bool needsMemoryModelExtension = false;
    size_t insertAt = kHeaderWords;
    size_t offset = kHeaderWords;
    while (offset < words.size()) {
        const uint32_t count = words[offset] >> kWordCountShift;
        const uint32_t opcode = words[offset] & kOpcodeMask;
        if (count == 0 || offset + count > words.size()) {
            result.rejectionReason =
                "SPIR-V module contains malformed instruction";
            return result;
        }

        if (opcode == kOpCapability && count >= 2) {
            const uint32_t capability = words[offset + 1];
            result.capabilities.push_back(capability);
            if (capability == kCapVulkanMemoryModel
                    || capability == kCapVulkanMemoryModelDeviceScope) {
                needsMemoryModelExtension = true;
            }
            if (targetVersion < kSpirv16
                    && capability == kCapDemoteToHelperInvocation) {
                result.rejectionReason =
                    "SPIR-V module requires DemoteToHelperInvocation on an unaudited compatibility path";
                return result;
            }
            insertAt = offset + count;
        } else if (opcode == kOpExtension) {
            insertAt = offset + count;
        }

        offset += count;
    }

    std::sort(result.capabilities.begin(), result.capabilities.end());
    result.capabilities.erase(
        std::unique(result.capabilities.begin(), result.capabilities.end()),
        result.capabilities.end());

    if (result.sourceVersion > targetVersion) {
        if (targetVersion < kSpirv15 && needsMemoryModelExtension
                && !hasExtension(words, "SPV_KHR_vulkan_memory_model")) {
            const auto extension =
                encodeExtension("SPV_KHR_vulkan_memory_model");
            words.insert(
                words.begin() + static_cast<std::ptrdiff_t>(insertAt),
                extension.begin(), extension.end());
        }
        words[1] = targetVersion;
        result.lowered = true;
    }

    result.code = wordsToBytes(words);
    result.supported = true;
    return result;
}

} // namespace LSFG::Core

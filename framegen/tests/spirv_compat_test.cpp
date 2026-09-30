#include "core/spirv_compat.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace LSFG::Core;

namespace {

constexpr uint32_t kMagic = 0x07230203u;
constexpr uint32_t kSpirv14 = 0x00010400u;
constexpr uint32_t kSpirv15 = 0x00010500u;
constexpr uint32_t kSpirv16 = 0x00010600u;

std::vector<uint8_t> bytes(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> out(words.size() * sizeof(uint32_t));
    std::memcpy(out.data(), words.data(), out.size());
    return out;
}

uint32_t version(const std::vector<uint8_t>& code) {
    uint32_t value{};
    std::memcpy(&value, code.data() + sizeof(uint32_t), sizeof(value));
    return value;
}

void native_16_is_unchanged() {
    const auto source = bytes({
        kMagic, kSpirv16, 0, 1, 0,
        (2u << 16) | 17u, 1u,
        (3u << 16) | 14u, 0u, 3u,
    });
    const auto result = prepareSpirvForTarget(source, kSpirv16);
    assert(result.supported);
    assert(!result.lowered);
    assert(result.code == source);
}

void lowers_16_to_15_only_after_validating_stream() {
    const auto source = bytes({
        kMagic, kSpirv16, 0, 1, 0,
        (2u << 16) | 17u, 1u,
        (3u << 16) | 14u, 0u, 3u,
    });
    const auto result = prepareSpirvForTarget(source, kSpirv15);
    assert(result.supported);
    assert(result.lowered);
    assert(version(result.code) == kSpirv15);

    auto malformed = source;
    const uint32_t zeroWordCountInstruction = 17u;
    std::memcpy(
        malformed.data() + 5 * sizeof(uint32_t),
        &zeroWordCountInstruction, sizeof(zeroWordCountInstruction));
    const auto bad = prepareSpirvForTarget(malformed, kSpirv15);
    assert(!bad.supported);
    assert(bad.rejectionReason.find("malformed") != std::string::npos);
}

void spirv14_gets_memory_model_extension() {
    const auto source = bytes({
        kMagic, kSpirv16, 0, 1, 0,
        (2u << 16) | 17u, 1u,
        (2u << 16) | 17u, 5345u,
        (3u << 16) | 14u, 0u, 3u,
    });
    const auto result = prepareSpirvForTarget(source, kSpirv14);
    assert(result.supported);
    assert(result.lowered);
    assert(version(result.code) == kSpirv14);
    const std::string raw(
        reinterpret_cast<const char*>(result.code.data()), result.code.size());
    assert(raw.find("SPV_KHR_vulkan_memory_model") != std::string::npos);
}

void demote_capability_is_rejected_below_16() {
    const auto source = bytes({
        kMagic, kSpirv16, 0, 1, 0,
        (2u << 16) | 17u, 1u,
        (2u << 16) | 17u, 5379u,
        (3u << 16) | 14u, 0u, 3u,
    });
    const auto result = prepareSpirvForTarget(source, kSpirv15);
    assert(!result.supported);
    assert(result.rejectionReason.find("DemoteToHelperInvocation")
        != std::string::npos);
}

void invalid_magic_is_rejected() {
    const auto source = bytes({
        0u, kSpirv16, 0, 1, 0,
        (3u << 16) | 14u, 0u, 3u,
    });
    const auto result = prepareSpirvForTarget(source, kSpirv15);
    assert(!result.supported);
    assert(result.rejectionReason.find("magic") != std::string::npos);
}

} // namespace

int main() {
    native_16_is_unchanged();
    lowers_16_to_15_only_after_validating_stream();
    spirv14_gets_memory_model_extension();
    demote_capability_is_rejected_below_16();
    invalid_magic_is_rejected();
    return 0;
}

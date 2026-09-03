#include "extract/spirv_compat.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
constexpr uint32_t op(uint16_t wordCount, uint16_t opcode) {
    return (static_cast<uint32_t>(wordCount) << 16) | opcode;
}

std::vector<uint8_t> makeModule() {
    std::vector<uint32_t> words{
        0x07230203u, 0x00010300u, 0, 32, 0,
        op(2, 30), 1,
        op(4, 32), 2, 2, 1,
        op(4, 59), 2, 3, 2,
        op(2, 26), 4,
        op(4, 32), 5, 0, 4,
        op(4, 59), 5, 6, 0,
        op(9, 25), 7, 1, 2, 0, 0, 0, 1, 0,
        op(4, 32), 8, 0, 7,
        op(4, 59), 8, 9, 0,
        op(9, 25), 10, 1, 2, 0, 0, 0, 2, 0,
        op(4, 32), 11, 0, 10,
        op(4, 59), 11, 12, 0,
        op(4, 71), 6, 33, 0,
        op(4, 71), 6, 34, 4,
        op(4, 71), 9, 33, 0,
        op(4, 71), 9, 34, 4,
        op(4, 71), 12, 33, 0,
        op(4, 71), 12, 34, 4,
        op(4, 71), 3, 33, 0,
        op(4, 71), 3, 34, 4,
        op(5, 54), 0, 20, 0, 21,
    };
    std::vector<uint8_t> bytes(words.size() * sizeof(uint32_t));
    std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}

uint32_t bindingFor(const std::vector<uint8_t>& bytes, uint32_t variableId, uint32_t decoration) {
    std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
    std::memcpy(words.data(), bytes.data(), bytes.size());
    for (size_t i = 5; i < words.size();) {
        const uint32_t wc = words[i] >> 16;
        const uint32_t opcode = words[i] & 0xffffu;
        if (wc == 0 || i + wc > words.size())
            break;
        if (opcode == 71 && wc == 4 && words[i + 1] == variableId
                && words[i + 2] == decoration)
            return words[i + 3];
        i += wc;
    }
    throw std::runtime_error("decoration not found");
}
}

int main() {
    const auto input = makeModule();
    assert(Extract::isSpirvBytecode(input));
    const auto normalized = Extract::normalizePrecompiledSpirv(input);

    assert(bindingFor(normalized, 3, 33) == 0);
    assert(bindingFor(normalized, 6, 33) == 1);
    assert(bindingFor(normalized, 9, 33) == 2);
    assert(bindingFor(normalized, 12, 33) == 3);
    assert(bindingFor(normalized, 3, 34) == 0);
    assert(bindingFor(normalized, 6, 34) == 0);
    assert(bindingFor(normalized, 9, 34) == 0);
    assert(bindingFor(normalized, 12, 34) == 0);

    auto badMagic = input;
    badMagic[0] = 0;
    assert(!Extract::isSpirvBytecode(badMagic));
    bool threw = false;
    try {
        (void)Extract::normalizePrecompiledSpirv(badMagic);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    return 0;
}

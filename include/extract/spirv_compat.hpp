#pragma once

#include <cstdint>
#include <vector>

namespace Extract {

    bool isSpirvBytecode(const std::vector<uint8_t>& bytecode);

    /// Normalize the descriptor decorations in a precompiled Lossless.dll
    /// SPIR-V module to the single-set, densely flattened descriptor layout
    /// used by framegen. Throws std::runtime_error for malformed or unsupported
    /// descriptor declarations so the caller never submits a partially
    /// normalized module to the driver.
    std::vector<uint8_t> normalizePrecompiledSpirv(std::vector<uint8_t> bytecode);

}

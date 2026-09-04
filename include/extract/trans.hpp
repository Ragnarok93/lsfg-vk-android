#pragma once

#include "extract/spirv_compat.hpp"

#include <cstdint>
#include <vector>

namespace Extract {

    /// Translate DXBC bytecode to SPIR-V bytecode. If the input is already a
    /// validated precompiled SPIR-V resource, normalize its descriptor bindings
    /// instead of passing it through the DXBC compiler.
    std::vector<uint8_t> translateShader(std::vector<uint8_t> bytecode);

}

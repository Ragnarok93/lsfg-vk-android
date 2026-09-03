#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Extract {

    enum class ShaderVariant : uint8_t {
        Dxbc,
        PrecompiledFp16,
        PrecompiledFp32,
    };

    /// Extract all known shader resources from Lossless.dll.
    /// DXBC resources are required. Precompiled SPIR-V variants are optional
    /// and are validated before selection.
    void extractShaders();

    /// Get a shader using the active platform/default route. Android defaults
    /// to validated precompiled FP32 SPIR-V and falls back to DXBC.
    std::vector<uint8_t> getShader(const std::string& name);

    /// Get a specific shader representation.
    std::vector<uint8_t> getShader(const std::string& name, ShaderVariant variant);

    /// Returns true only when the complete resource set for a variant exists
    /// and, for precompiled variants, every resource has a SPIR-V header.
    bool shaderVariantAvailable(ShaderVariant variant);

    /// Resolve LSFG_VK_SHADER_ROUTE against available resources. This is
    /// intentionally vendor-neutral; no GPU vendor ID participates.
    ShaderVariant selectShaderVariant();

    const char* shaderVariantName(ShaderVariant variant);

}

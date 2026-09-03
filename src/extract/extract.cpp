#include "extract/extract.hpp"
#include "config/config.hpp"

#include <pe-parse/parse.h>

#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace Extract;

const std::unordered_map<std::string, uint32_t> nameIdxTable = {{
    { "mipmaps", 255 },
    { "alpha[0]", 267 },
    { "alpha[1]", 268 },
    { "alpha[2]", 269 },
    { "alpha[3]", 270 },
    { "beta[0]", 275 },
    { "beta[1]", 276 },
    { "beta[2]", 277 },
    { "beta[3]", 278 },
    { "beta[4]", 279 },
    { "gamma[0]", 257 },
    { "gamma[1]", 259 },
    { "gamma[2]", 260 },
    { "gamma[3]", 261 },
    { "gamma[4]", 262 },
    { "delta[0]", 257 },
    { "delta[1]", 263 },
    { "delta[2]", 264 },
    { "delta[3]", 265 },
    { "delta[4]", 266 },
    { "delta[5]", 258 },
    { "delta[6]", 271 },
    { "delta[7]", 272 },
    { "delta[8]", 273 },
    { "delta[9]", 274 },
    { "generate", 256 },
    { "p_mipmaps", 255 },
    { "p_alpha[0]", 290 },
    { "p_alpha[1]", 291 },
    { "p_alpha[2]", 292 },
    { "p_alpha[3]", 293 },
    { "p_beta[0]", 298 },
    { "p_beta[1]", 299 },
    { "p_beta[2]", 300 },
    { "p_beta[3]", 301 },
    { "p_beta[4]", 302 },
    { "p_gamma[0]", 280 },
    { "p_gamma[1]", 282 },
    { "p_gamma[2]", 283 },
    { "p_gamma[3]", 284 },
    { "p_gamma[4]", 285 },
    { "p_delta[0]", 280 },
    { "p_delta[1]", 286 },
    { "p_delta[2]", 287 },
    { "p_delta[3]", 288 },
    { "p_delta[4]", 289 },
    { "p_delta[5]", 281 },
    { "p_delta[6]", 294 },
    { "p_delta[7]", 295 },
    { "p_delta[8]", 296 },
    { "p_delta[9]", 297 },
    { "p_generate", 256 },
}};

namespace {
    constexpr uint32_t kFirstDxbcResource = 255;
    constexpr uint32_t kLastDxbcResource = 302;
    constexpr uint32_t kFp16ResourceOffset = 49;
    constexpr uint32_t kFp32ResourceOffset = 98;

    auto& shaders() {
        static std::unordered_map<uint32_t, std::vector<uint8_t>> shaderData;
        return shaderData;
    }

    bool isSpirvResource(const std::vector<uint8_t>& data) {
        static constexpr uint8_t magic[] = {0x03, 0x02, 0x23, 0x07};
        return data.size() >= sizeof(magic)
            && std::memcmp(data.data(), magic, sizeof(magic)) == 0;
    }

    uint32_t resourceOffset(ShaderVariant variant) {
        switch (variant) {
            case ShaderVariant::Dxbc: return 0;
            case ShaderVariant::PrecompiledFp16: return kFp16ResourceOffset;
            case ShaderVariant::PrecompiledFp32: return kFp32ResourceOffset;
        }
        return 0;
    }

    bool completeVariantAvailable(ShaderVariant variant) {
        if (shaders().empty())
            return false;
        const uint32_t offset = resourceOffset(variant);
        for (uint32_t id = kFirstDxbcResource; id <= kLastDxbcResource; ++id) {
            const auto hit = shaders().find(id + offset);
            if (hit == shaders().end())
                return false;
            if (variant != ShaderVariant::Dxbc && !isSpirvResource(hit->second))
                return false;
        }
        return true;
    }

    int on_resource(void*, const peparse::resource& res) {
        if (res.type != peparse::RT_RCDATA || res.buf == nullptr || res.buf->bufLen <= 0)
            return 0;
        std::vector<uint8_t> resource_data(res.buf->bufLen);
        std::copy_n(res.buf->buf, res.buf->bufLen, resource_data.data());
        shaders()[res.name] = std::move(resource_data);
        return 0;
    }

    const std::vector<std::filesystem::path> PATHS{{
        ".local/share/Steam/steamapps/common",
        ".steam/steam/steamapps/common",
        ".steam/debian-installation/steamapps/common",
        ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common",
        "snap/steam/common/.local/share/Steam/steamapps/common"
    }};

    std::string getDllPath() {
        std::string dllPath = Config::activeConf.dll;
        if (!dllPath.empty())
            return dllPath;
        const char* directPath = getenv("LSFG_DLL_PATH_UNIX");
        if (directPath && *directPath != '\0' && std::filesystem::exists(directPath))
            return std::string(directPath);
        const char* winePrefix = getenv("WINEPREFIX");
        if (winePrefix && *winePrefix != '\0') {
            const std::vector<std::filesystem::path> WINE_PATHS{{
                "drive_c/Program Files (x86)/Steam/steamapps/common/Lossless Scaling/Lossless.dll",
                "drive_c/Program Files/Steam/steamapps/common/Lossless Scaling/Lossless.dll"
            }};
            for (const auto& rel : WINE_PATHS) {
                const std::filesystem::path path = std::filesystem::path(winePrefix) / rel;
                if (std::filesystem::exists(path))
                    return path.string();
            }
        }
        const char* home = getenv("HOME");
        const std::string homeStr = home ? home : "";
        for (const auto& base : PATHS) {
            const std::filesystem::path path =
                std::filesystem::path(homeStr) / base / "Lossless Scaling" / "Lossless.dll";
            if (std::filesystem::exists(path))
                return path.string();
        }
        const char* dataDir = getenv("XDG_DATA_HOME");
        if (dataDir && *dataDir != '\0')
            return std::string(dataDir) + "/Steam/steamapps/common/Lossless Scaling/Lossless.dll";
        return "Lossless.dll";
    }
}

void Extract::extractShaders() {
    if (!shaders().empty())
        return;

    peparse::parsed_pe* dll = peparse::ParsePEFromFile(getDllPath().c_str());
    if (!dll)
        throw std::runtime_error("Unable to read Lossless.dll, is it installed?");
    peparse::IterRsrc(dll, on_resource, nullptr);
    peparse::DestructParsedPE(dll);

    for (const auto& [name, idx] : nameIdxTable)
        if (shaders().find(idx) == shaders().end())
            throw std::runtime_error("Shader not found: " + name + ".\n- Is Lossless Scaling up to date?");
}

bool Extract::shaderVariantAvailable(ShaderVariant variant) {
    return completeVariantAvailable(variant);
}

const char* Extract::shaderVariantName(ShaderVariant variant) {
    switch (variant) {
        case ShaderVariant::Dxbc: return "dxbc";
        case ShaderVariant::PrecompiledFp16: return "spirv-fp16";
        case ShaderVariant::PrecompiledFp32: return "spirv-fp32";
    }
    return "unknown";
}

ShaderVariant Extract::selectShaderVariant() {
    const char* raw = std::getenv("LSFG_VK_SHADER_ROUTE");
    const std::string_view preference = raw != nullptr ? std::string_view(raw) : std::string_view("auto");

    if (preference == "dxbc")
        return ShaderVariant::Dxbc;
    if (preference == "spirv-fp16") {
        if (completeVariantAvailable(ShaderVariant::PrecompiledFp16))
            return ShaderVariant::PrecompiledFp16;
        if (completeVariantAvailable(ShaderVariant::PrecompiledFp32))
            return ShaderVariant::PrecompiledFp32;
        return ShaderVariant::Dxbc;
    }
    if (preference == "spirv-fp32") {
        if (completeVariantAvailable(ShaderVariant::PrecompiledFp32))
            return ShaderVariant::PrecompiledFp32;
        return ShaderVariant::Dxbc;
    }

#ifdef __ANDROID__
    if (completeVariantAvailable(ShaderVariant::PrecompiledFp32))
        return ShaderVariant::PrecompiledFp32;
#endif
    return ShaderVariant::Dxbc;
}

std::vector<uint8_t> Extract::getShader(const std::string& name, ShaderVariant variant) {
    if (shaders().empty())
        throw std::runtime_error("Shaders are not loaded.");

    const auto nameHit = nameIdxTable.find(name);
    if (nameHit == nameIdxTable.end())
        throw std::runtime_error("Shader hash not found: " + name);

    const uint32_t resourceId = nameHit->second + resourceOffset(variant);
    const auto shaderHit = shaders().find(resourceId);
    if (shaderHit == shaders().end())
        throw std::runtime_error("Shader variant not found: " + name);
    if (variant != ShaderVariant::Dxbc && !isSpirvResource(shaderHit->second))
        throw std::runtime_error("Precompiled shader resource is not valid SPIR-V: " + name);

    return shaderHit->second;
}

std::vector<uint8_t> Extract::getShader(const std::string& name) {
    const ShaderVariant variant = selectShaderVariant();
    static ShaderVariant loggedVariant = static_cast<ShaderVariant>(0xff);
    if (variant != loggedVariant) {
        std::cerr << "lsfg-vk: shader-route=" << shaderVariantName(variant)
                  << " fp32Available="
                  << (completeVariantAvailable(ShaderVariant::PrecompiledFp32) ? 1 : 0)
                  << " fp16Available="
                  << (completeVariantAvailable(ShaderVariant::PrecompiledFp16) ? 1 : 0)
                  << '\n';
        loggedVariant = variant;
    }
    return getShader(name, variant);
}

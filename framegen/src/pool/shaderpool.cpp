#include "pool/shaderpool.hpp"
#include "core/shadermodule.hpp"
#include "core/device.hpp"
#include "core/pipeline.hpp"
#include "adreno_b10_mipmaps.hpp"

#include <vulkan/vulkan_core.h>

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

using namespace LSFG;
using namespace LSFG::Pool;

Core::ShaderModule ShaderPool::getShader(
        const Core::Device& device, const std::string& name,
        const std::vector<std::pair<size_t, VkDescriptorType>>& types) {
    auto it = shaders.find(name);
    if (it != shaders.end())
        return it->second;

    std::vector<uint8_t> bytecode;
    bool b10HeadTransformed = false;
#ifdef LSFGVK_ADRENO_B10_MIPMAPS
    const bool b10Supported = Optimizations::AdrenoB10::isRuntimeSupported(device);
    if (name == "p_mipmaps_b10_tail") {
        if (!b10Supported)
            throw std::runtime_error("B10 Mipmaps tail requested on unsupported device");
        bytecode = Optimizations::AdrenoB10::buildTailSpirv();
        std::cerr << "lsfg-vk: candidate-b10-mipmaps-tail generated=1 bytes="
                  << bytecode.size() << " local=16x16\n";
    } else
#endif
    {
        bytecode = this->source(name);
    }

    if (bytecode.empty())
        throw std::runtime_error("Shader code is empty: " + name);

#ifdef LSFGVK_ADRENO_B10_MIPMAPS
    if (name == "p_mipmaps" && b10Supported) {
        Optimizations::AdrenoB10::HeadTransformStats stats;
        auto candidate = bytecode;
        if (Optimizations::AdrenoB10::transformHead(candidate, stats)) {
            bytecode = std::move(candidate);
            b10HeadTransformed = true;
            std::cerr << "lsfg-vk: candidate-b10-mipmaps-head applied=1 function="
                      << stats.targetFunction
                      << " samples=" << stats.imageSamplesBefore << "->" << stats.imageSamplesAfter
                      << " writes=" << stats.imageWritesBefore << "->" << stats.imageWritesAfter
                      << " barriers=" << stats.barriersBefore << "->" << stats.barriersAfter
                      << " wg_stores=" << stats.workgroupStoresBeforePhase0 << "->"
                      << stats.workgroupStoresAfter
                      << " bytes=" << bytecode.size() << '\n';
        } else {
            std::cerr << "lsfg-vk: candidate-b10-mipmaps-head applied=0 reason="
                      << stats.reason << " fallback=baseline\n";
        }
    }
#endif

    Core::ShaderModule shader(device, bytecode, types);
    if (name == "p_mipmaps")
        this->b10MipmapsHeadActive = b10HeadTransformed;
    shaders[name] = shader;
    return shader;
}

Core::Pipeline ShaderPool::getPipeline(
        const Core::Device& device, const std::string& name) {
    auto it = pipelines.find(name);
    if (it != pipelines.end())
        return it->second;

    // grab the shader module
    auto shader = this->getShader(device, name, {});

    // create the pipeline
    Core::Pipeline pipeline(device, shader);
    pipelines[name] = pipeline;
    return pipeline;
}

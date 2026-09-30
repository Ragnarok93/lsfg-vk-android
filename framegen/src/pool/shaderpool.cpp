#include "pool/shaderpool.hpp"
#include "core/shadermodule.hpp"
#include "core/device.hpp"
#include "core/pipeline.hpp"
#include "core/spirv_compat.hpp"

#include <vulkan/vulkan_core.h>

#include <cstddef>
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

    // grab the shader
    auto bytecode = this->source(name);
    if (bytecode.empty())
        throw std::runtime_error("Shader code is empty: " + name);

    const auto compatible = Core::prepareSpirvForTarget(
        bytecode, this->spirvTargetVersion);
    if (!compatible.supported) {
        throw std::runtime_error(
            "Shader compatibility rejected " + name + ": "
            + compatible.rejectionReason);
    }

    // create the shader module from the validated device-compatible payload.
    Core::ShaderModule shader(device, compatible.code, types);
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

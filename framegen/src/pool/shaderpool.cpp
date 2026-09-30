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

    // Create the shader module from the validated device-compatible payload.
    // Preserve the module name in any driver failure so Android diagnostics can
    // identify the exact shader that is incompatible with a restrictive ICD.
    try {
        Core::ShaderModule shader(device, compatible.code, types);
        shaders[name] = shader;
        return shader;
    } catch (const std::exception& e) {
        throw std::runtime_error(
            "Shader module creation failed for " + name + ": " + e.what());
    }
}

Core::Pipeline ShaderPool::getPipeline(
        const Core::Device& device, const std::string& name) {
    auto it = pipelines.find(name);
    if (it != pipelines.end())
        return it->second;

    // grab the shader module
    auto shader = this->getShader(device, name, {});

    // Preserve the shader/pipeline name when vkCreateComputePipelines fails.
    try {
        Core::Pipeline pipeline(device, shader);
        pipelines[name] = pipeline;
        return pipeline;
    } catch (const std::exception& e) {
        throw std::runtime_error(
            "Compute pipeline creation failed for " + name + ": " + e.what());
    }
}

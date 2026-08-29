#pragma once

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <string>

namespace LSFG::Core {

enum class SynchronizationPath {
    Unsupported,
    Core13Sync2,
    KhrSync2,
    LegacyPipelineBarrier,
};

enum class AhbFormatClass {
    Unsupported,
    DefinedFormat,
    ExternalFormatSampledOnly,
};

enum class ShaderPrecision {
    Fp32,
    Fp16,
};

struct VulkanCapabilities {
    uint32_t apiVersion{VK_API_VERSION_1_0};
    bool androidHardwareBuffer{false};
    AhbFormatClass ahbFormatClass{AhbFormatClass::Unsupported};
    bool synchronization2Core{false};
    bool synchronization2Extension{false};
    bool synchronization2Feature{false};
    bool legacyPipelineBarrier{true};
    bool timelineSemaphore{false};
    bool shaderFloat16{false};
    VkShaderStageFlags subgroupStages{0};
    VkSubgroupFeatureFlags subgroupOperations{0};
    uint32_t subgroupSize{0};
};

struct SupportRequirements {
    bool requireAhb{true};
    bool requireWritableAhbFormat{false};
    VkShaderStageFlags requiredSubgroupStages{0};
    VkSubgroupFeatureFlags requiredSubgroupOperations{0};
};

struct SupportDecision {
    bool supported{false};
    SynchronizationPath synchronizationPath{SynchronizationPath::Unsupported};
    ShaderPrecision shaderPrecision{ShaderPrecision::Fp32};
    std::string rejectionReason;
};

[[nodiscard]] SupportDecision evaluateCapabilities(
    const VulkanCapabilities& capabilities,
    const SupportRequirements& requirements = {});

[[nodiscard]] const char* synchronizationPathName(SynchronizationPath path);
[[nodiscard]] const char* shaderPrecisionName(ShaderPrecision precision);
[[nodiscard]] const char* ahbFormatClassName(AhbFormatClass formatClass);

} // namespace LSFG::Core

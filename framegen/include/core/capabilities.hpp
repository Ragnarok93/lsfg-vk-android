#pragma once

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <string>

namespace LSFG::Core {

enum class VulkanPath {
    Unsupported,
    Vulkan11Extensions,
    Vulkan12,
    Vulkan13Plus,
};

enum class SpirvTarget : uint32_t {
    Unsupported = 0,
    Spirv14 = 0x00010400,
    Spirv15 = 0x00010500,
    Spirv16 = 0x00010600,
};

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

    bool spirv14Extension{false};
    bool shaderFloatControlsExtension{false};
    bool vulkanMemoryModelExtension{false};
    bool vulkanMemoryModel{false};
    bool vulkanMemoryModelDeviceScope{false};
    bool shaderStorageImageWriteWithoutFormat{false};
    bool shaderStorageImageExtendedFormats{false};

    bool synchronization2Core{false};
    bool synchronization2Extension{false};
    bool synchronization2Feature{false};
    bool legacyPipelineBarrier{true};
    bool timelineSemaphore{false};
    bool shaderFloat16{false};
    bool nullDescriptor{false};
    bool externalSemaphoreOpaqueFd{false};
    bool externalSemaphoreSyncFd{false};
    VkShaderStageFlags subgroupStages{0};
    VkSubgroupFeatureFlags subgroupOperations{0};
    uint32_t subgroupSize{0};
};

struct SupportRequirements {
    bool requireAhb{true};
    bool requireWritableAhbFormat{false};
    bool requireShaderCompatibility{true};
    bool requireVulkanMemoryModel{true};
    bool requireStorageImageWriteWithoutFormat{true};
    bool requireStorageImageExtendedFormats{true};
    VkShaderStageFlags requiredSubgroupStages{0};
    VkSubgroupFeatureFlags requiredSubgroupOperations{0};
};

struct SupportDecision {
    bool supported{false};
    VulkanPath vulkanPath{VulkanPath::Unsupported};
    SpirvTarget spirvTarget{SpirvTarget::Unsupported};
    SynchronizationPath synchronizationPath{SynchronizationPath::Unsupported};
    ShaderPrecision shaderPrecision{ShaderPrecision::Fp32};
    bool fp16{false};
    bool nullDescriptor{false};
    bool externalOpaqueFd{false};
    bool externalSyncFd{false};
    std::string rejectionReason;
};

[[nodiscard]] SupportDecision evaluateCapabilities(
    const VulkanCapabilities& capabilities,
    const SupportRequirements& requirements = {});

[[nodiscard]] const char* vulkanPathName(VulkanPath path);
[[nodiscard]] const char* spirvTargetName(SpirvTarget target);
[[nodiscard]] const char* synchronizationPathName(SynchronizationPath path);
[[nodiscard]] const char* shaderPrecisionName(ShaderPrecision precision);
[[nodiscard]] const char* ahbFormatClassName(AhbFormatClass formatClass);

} // namespace LSFG::Core

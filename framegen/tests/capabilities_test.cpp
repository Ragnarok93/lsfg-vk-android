#include "core/capabilities.hpp"

#include <vulkan/vulkan_core.h>

#include <cassert>
#include <string>

using namespace LSFG::Core;

namespace {

VulkanCapabilities base12() {
    VulkanCapabilities caps{};
    caps.apiVersion = VK_API_VERSION_1_2;
    caps.androidHardwareBuffer = true;
    caps.ahbFormatClass = AhbFormatClass::DefinedFormat;
    caps.vulkanMemoryModel = true;
    caps.shaderStorageImageWriteWithoutFormat = true;
    caps.shaderStorageImageExtendedFormats = true;
    caps.timelineSemaphore = true;
    caps.legacyPipelineBarrier = true;
    caps.subgroupSize = 32;
    return caps;
}

VulkanCapabilities compatible11() {
    auto caps = base12();
    caps.apiVersion = VK_API_VERSION_1_1;
    caps.spirv14Extension = true;
    caps.shaderFloatControlsExtension = true;
    caps.vulkanMemoryModelExtension = true;
    caps.timelineSemaphore = false;
    return caps;
}

void vulkan11_extension_path_uses_spirv14_and_legacy_sync() {
    const auto decision = evaluateCapabilities(compatible11());
    assert(decision.supported);
    assert(decision.vulkanPath == VulkanPath::Vulkan11Extensions);
    assert(decision.spirvTarget == SpirvTarget::Spirv14);
    assert(decision.synchronizationPath == SynchronizationPath::LegacyPipelineBarrier);
}

void vulkan11_missing_compat_extensions_fails_cleanly() {
    auto caps = compatible11();
    caps.spirv14Extension = false;
    auto decision = evaluateCapabilities(caps);
    assert(!decision.supported);
    assert(decision.rejectionReason.find("VK_KHR_spirv_1_4") != std::string::npos);

    caps = compatible11();
    caps.shaderFloatControlsExtension = false;
    decision = evaluateCapabilities(caps);
    assert(!decision.supported);
    assert(decision.rejectionReason.find("shader_float_controls") != std::string::npos);

    caps = compatible11();
    caps.vulkanMemoryModelExtension = false;
    decision = evaluateCapabilities(caps);
    assert(!decision.supported);
    assert(decision.rejectionReason.find("vulkan_memory_model") != std::string::npos);
}

void vulkan11_missing_memory_model_feature_fails_cleanly() {
    auto caps = compatible11();
    caps.vulkanMemoryModel = false;
    const auto decision = evaluateCapabilities(caps);
    assert(!decision.supported);
    assert(decision.rejectionReason.find("memory model") != std::string::npos);
}

void vulkan12_without_sync2_uses_spirv15_and_legacy_barriers() {
    const auto decision = evaluateCapabilities(base12());
    assert(decision.supported);
    assert(decision.vulkanPath == VulkanPath::Vulkan12);
    assert(decision.spirvTarget == SpirvTarget::Spirv15);
    assert(decision.synchronizationPath == SynchronizationPath::LegacyPipelineBarrier);
}

void vulkan12_with_khr_sync2_uses_extension_path() {
    auto caps = base12();
    caps.synchronization2Extension = true;
    caps.synchronization2Feature = true;
    const auto decision = evaluateCapabilities(caps);
    assert(decision.supported);
    assert(decision.spirvTarget == SpirvTarget::Spirv15);
    assert(decision.synchronizationPath == SynchronizationPath::KhrSync2);
}

void vulkan13_with_sync2_uses_spirv16_core_path() {
    auto caps = base12();
    caps.apiVersion = VK_API_VERSION_1_3;
    caps.synchronization2Core = true;
    caps.synchronization2Feature = true;
    const auto decision = evaluateCapabilities(caps);
    assert(decision.supported);
    assert(decision.vulkanPath == VulkanPath::Vulkan13Plus);
    assert(decision.spirvTarget == SpirvTarget::Spirv16);
    assert(decision.synchronizationPath == SynchronizationPath::Core13Sync2);
}

void required_shader_features_are_checked() {
    auto caps = base12();
    caps.shaderStorageImageWriteWithoutFormat = false;
    auto decision = evaluateCapabilities(caps);
    assert(!decision.supported);
    assert(decision.rejectionReason.find("WriteWithoutFormat") != std::string::npos);

    caps = base12();
    caps.shaderStorageImageExtendedFormats = false;
    decision = evaluateCapabilities(caps);
    assert(!decision.supported);
    assert(decision.rejectionReason.find("ExtendedFormats") != std::string::npos);
}

void ahb_absent_is_rejected() {
    auto caps = base12();
    caps.androidHardwareBuffer = false;
    const auto decision = evaluateCapabilities(caps);
    assert(!decision.supported);
    assert(decision.rejectionReason.find("android_hardware_buffer") != std::string::npos);
}

void external_format_ahb_is_rejected_only_when_writable_ahb_is_required() {
    auto caps = base12();
    caps.ahbFormatClass = AhbFormatClass::ExternalFormatSampledOnly;
    assert(evaluateCapabilities(caps).supported);

    SupportRequirements requirements{};
    requirements.requireWritableAhbFormat = true;
    const auto decision = evaluateCapabilities(caps, requirements);
    assert(!decision.supported);
    assert(decision.rejectionReason.find("external-format-only") != std::string::npos);

    caps.ahbFormatClass = AhbFormatClass::DefinedFormat;
    assert(evaluateCapabilities(caps, requirements).supported);
}

void fp16_is_optional() {
    auto caps = base12();
    caps.shaderFloat16 = false;
    assert(evaluateCapabilities(caps).shaderPrecision == ShaderPrecision::Fp32);
    caps.shaderFloat16 = true;
    assert(evaluateCapabilities(caps).shaderPrecision == ShaderPrecision::Fp16);
}

void external_sync_and_null_descriptor_are_reported_not_required() {
    auto caps = base12();
    caps.nullDescriptor = true;
    caps.externalSemaphoreOpaqueFd = true;
    caps.externalSemaphoreSyncFd = false;
    const auto decision = evaluateCapabilities(caps);
    assert(decision.supported);
    assert(decision.nullDescriptor);
    assert(decision.externalOpaqueFd);
    assert(!decision.externalSyncFd);
}

void required_subgroup_masks_are_checked_exactly() {
    auto caps = base12();
    caps.subgroupStages = VK_SHADER_STAGE_COMPUTE_BIT;
    caps.subgroupOperations = VK_SUBGROUP_FEATURE_BASIC_BIT;
    const SupportRequirements requirements{
        .requireAhb = true,
        .requiredSubgroupStages = VK_SHADER_STAGE_COMPUTE_BIT,
        .requiredSubgroupOperations =
            VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT,
    };
    assert(!evaluateCapabilities(caps, requirements).supported);

    caps.subgroupOperations |= VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
    assert(evaluateCapabilities(caps, requirements).supported);

    caps.subgroupSize = 0;
    assert(!evaluateCapabilities(caps, requirements).supported);
}

} // namespace

int main() {
    vulkan11_extension_path_uses_spirv14_and_legacy_sync();
    vulkan11_missing_compat_extensions_fails_cleanly();
    vulkan11_missing_memory_model_feature_fails_cleanly();
    vulkan12_without_sync2_uses_spirv15_and_legacy_barriers();
    vulkan12_with_khr_sync2_uses_extension_path();
    vulkan13_with_sync2_uses_spirv16_core_path();
    required_shader_features_are_checked();
    ahb_absent_is_rejected();
    external_format_ahb_is_rejected_only_when_writable_ahb_is_required();
    fp16_is_optional();
    external_sync_and_null_descriptor_are_reported_not_required();
    required_subgroup_masks_are_checked_exactly();
    return 0;
}

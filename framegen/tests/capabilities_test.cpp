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
    caps.timelineSemaphore = true;
    caps.legacyPipelineBarrier = true;
    caps.subgroupSize = 32;
    return caps;
}

void vulkan11_uses_binary_fence_fallback() {
    auto caps = base12();
    caps.apiVersion = VK_API_VERSION_1_1;
    caps.timelineSemaphore = false;
    const auto decision = evaluateCapabilities(caps);
    assert(decision.supported);
    assert(decision.synchronizationPath == SynchronizationPath::LegacyPipelineBarrier);
}

void vulkan12_without_sync2_uses_legacy_barriers() {
    const auto decision = evaluateCapabilities(base12());
    assert(decision.supported);
    assert(decision.synchronizationPath == SynchronizationPath::LegacyPipelineBarrier);
}

void vulkan12_with_khr_sync2_uses_extension_path() {
    auto caps = base12();
    caps.synchronization2Extension = true;
    caps.synchronization2Feature = true;
    const auto decision = evaluateCapabilities(caps);
    assert(decision.supported);
    assert(decision.synchronizationPath == SynchronizationPath::KhrSync2);
}

void vulkan13_with_sync2_uses_core_path() {
    auto caps = base12();
    caps.apiVersion = VK_API_VERSION_1_3;
    caps.synchronization2Core = true;
    caps.synchronization2Feature = true;
    const auto decision = evaluateCapabilities(caps);
    assert(decision.supported);
    assert(decision.synchronizationPath == SynchronizationPath::Core13Sync2);
}

void ahb_absent_is_rejected() {
    auto caps = base12();
    caps.androidHardwareBuffer = false;
    const auto decision = evaluateCapabilities(caps);
    assert(!decision.supported);
    assert(decision.rejectionReason.find("android_hardware_buffer") != std::string::npos);
}

void external_format_ahb_is_rejected_for_write_path() {
    auto caps = base12();
    caps.ahbFormatClass = AhbFormatClass::ExternalFormatSampledOnly;
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

void missing_timeline_uses_binary_fence_fallback() {
    auto caps = base12();
    caps.timelineSemaphore = false;
    const auto decision = evaluateCapabilities(caps);
    assert(decision.supported);
    assert(decision.synchronizationPath == SynchronizationPath::LegacyPipelineBarrier);
}

void required_subgroup_masks_are_checked_exactly() {
    auto caps = base12();
    caps.subgroupStages = VK_SHADER_STAGE_COMPUTE_BIT;
    caps.subgroupOperations = VK_SUBGROUP_FEATURE_BASIC_BIT;
    const SupportRequirements requirements{
        .requireAhb = true,
        .requiredSubgroupStages = VK_SHADER_STAGE_COMPUTE_BIT,
        .requiredSubgroupOperations = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT,
    };
    assert(!evaluateCapabilities(caps, requirements).supported);

    caps.subgroupOperations |= VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
    assert(evaluateCapabilities(caps, requirements).supported);

    caps.subgroupSize = 0;
    assert(!evaluateCapabilities(caps, requirements).supported);
}

} // namespace

int main() {
    vulkan11_uses_binary_fence_fallback();
    vulkan12_without_sync2_uses_legacy_barriers();
    vulkan12_with_khr_sync2_uses_extension_path();
    vulkan13_with_sync2_uses_core_path();
    ahb_absent_is_rejected();
    external_format_ahb_is_rejected_for_write_path();
    fp16_is_optional();
    missing_timeline_uses_binary_fence_fallback();
    required_subgroup_masks_are_checked_exactly();
    return 0;
}

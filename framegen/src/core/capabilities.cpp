#include "core/capabilities.hpp"

namespace LSFG::Core {

SupportDecision evaluateCapabilities(
        const VulkanCapabilities& caps,
        const SupportRequirements& requirements) {
    SupportDecision decision{};
    decision.fp16 = caps.shaderFloat16;
    decision.nullDescriptor = caps.nullDescriptor;
    decision.externalOpaqueFd = caps.externalSemaphoreOpaqueFd;
    decision.externalSyncFd = caps.externalSemaphoreSyncFd;

    if (caps.apiVersion < VK_API_VERSION_1_1) {
        decision.rejectionReason = "Vulkan 1.1 or newer is required";
        return decision;
    }

    if (requirements.requireShaderCompatibility) {
        if (caps.apiVersion >= VK_API_VERSION_1_3) {
            decision.vulkanPath = VulkanPath::Vulkan13Plus;
            decision.spirvTarget = SpirvTarget::Spirv16;
        } else if (caps.apiVersion >= VK_API_VERSION_1_2) {
            decision.vulkanPath = VulkanPath::Vulkan12;
            decision.spirvTarget = SpirvTarget::Spirv15;
        } else {
            if (!caps.spirv14Extension) {
                decision.rejectionReason =
                    "Vulkan 1.1 driver lacks VK_KHR_spirv_1_4";
                return decision;
            }
            if (!caps.shaderFloatControlsExtension) {
                decision.rejectionReason =
                    "Vulkan 1.1 driver lacks VK_KHR_shader_float_controls";
                return decision;
            }
            if (!caps.vulkanMemoryModelExtension) {
                decision.rejectionReason =
                    "Vulkan 1.1 driver lacks VK_KHR_vulkan_memory_model";
                return decision;
            }
            decision.vulkanPath = VulkanPath::Vulkan11Extensions;
            decision.spirvTarget = SpirvTarget::Spirv14;
        }

        if (requirements.requireVulkanMemoryModel && !caps.vulkanMemoryModel) {
            decision.rejectionReason =
                "driver lacks required Vulkan memory model feature";
            return decision;
        }
        if (requirements.requireStorageImageWriteWithoutFormat
                && !caps.shaderStorageImageWriteWithoutFormat) {
            decision.rejectionReason =
                "driver lacks shaderStorageImageWriteWithoutFormat";
            return decision;
        }
        if (requirements.requireStorageImageExtendedFormats
                && !caps.shaderStorageImageExtendedFormats) {
            decision.rejectionReason =
                "driver lacks shaderStorageImageExtendedFormats";
            return decision;
        }
    } else if (caps.apiVersion >= VK_API_VERSION_1_3) {
        decision.vulkanPath = VulkanPath::Vulkan13Plus;
        decision.spirvTarget = SpirvTarget::Spirv16;
    } else if (caps.apiVersion >= VK_API_VERSION_1_2) {
        decision.vulkanPath = VulkanPath::Vulkan12;
        decision.spirvTarget = SpirvTarget::Spirv15;
    } else {
        decision.vulkanPath = VulkanPath::Vulkan11Extensions;
        decision.spirvTarget = SpirvTarget::Spirv14;
    }

    if (requirements.requireAhb && !caps.androidHardwareBuffer) {
        decision.rejectionReason =
            "VK_ANDROID_external_memory_android_hardware_buffer is required by the Android frame exchange";
        return decision;
    }
    if (requirements.requireWritableAhbFormat) {
        if (caps.ahbFormatClass == AhbFormatClass::ExternalFormatSampledOnly) {
            decision.rejectionReason =
                "AHardwareBuffer is external-format-only; LSFG requires storage-image writes";
            return decision;
        }
        if (caps.ahbFormatClass != AhbFormatClass::DefinedFormat) {
            decision.rejectionReason =
                "AHardwareBuffer writable VkFormat compatibility was not established";
            return decision;
        }
    }

    if ((caps.subgroupStages & requirements.requiredSubgroupStages)
            != requirements.requiredSubgroupStages) {
        decision.rejectionReason = "required subgroup shader stages are unavailable";
        return decision;
    }
    if ((caps.subgroupOperations & requirements.requiredSubgroupOperations)
            != requirements.requiredSubgroupOperations) {
        decision.rejectionReason = "required subgroup operations are unavailable";
        return decision;
    }
    if ((requirements.requiredSubgroupStages != 0
            || requirements.requiredSubgroupOperations != 0)
            && caps.subgroupSize == 0) {
        decision.rejectionReason = "driver reported an invalid subgroup size";
        return decision;
    }

    if (caps.apiVersion >= VK_API_VERSION_1_3
            && caps.synchronization2Core && caps.synchronization2Feature) {
        decision.synchronizationPath = SynchronizationPath::Core13Sync2;
    } else if (caps.synchronization2Extension && caps.synchronization2Feature) {
        decision.synchronizationPath = SynchronizationPath::KhrSync2;
    } else if (caps.legacyPipelineBarrier) {
        decision.synchronizationPath = SynchronizationPath::LegacyPipelineBarrier;
    } else {
        decision.rejectionReason = "no usable Vulkan synchronization path is available";
        return decision;
    }

    decision.shaderPrecision = caps.shaderFloat16
        ? ShaderPrecision::Fp16
        : ShaderPrecision::Fp32;
    decision.supported = true;
    return decision;
}

const char* vulkanPathName(VulkanPath path) {
    switch (path) {
        case VulkanPath::Vulkan11Extensions: return "vulkan-1.1-extensions";
        case VulkanPath::Vulkan12: return "vulkan-1.2";
        case VulkanPath::Vulkan13Plus: return "vulkan-1.3+";
        case VulkanPath::Unsupported: return "unsupported";
    }
    return "unsupported";
}

const char* spirvTargetName(SpirvTarget target) {
    switch (target) {
        case SpirvTarget::Spirv14: return "1.4";
        case SpirvTarget::Spirv15: return "1.5";
        case SpirvTarget::Spirv16: return "1.6";
        case SpirvTarget::Unsupported: return "unsupported";
    }
    return "unsupported";
}

const char* synchronizationPathName(SynchronizationPath path) {
    switch (path) {
        case SynchronizationPath::Core13Sync2: return "core-1.3-sync2";
        case SynchronizationPath::KhrSync2: return "khr-sync2";
        case SynchronizationPath::LegacyPipelineBarrier: return "legacy-pipeline-barrier";
        case SynchronizationPath::Unsupported: return "unsupported";
    }
    return "unsupported";
}

const char* shaderPrecisionName(ShaderPrecision precision) {
    return precision == ShaderPrecision::Fp16 ? "fp16" : "fp32";
}

const char* ahbFormatClassName(AhbFormatClass formatClass) {
    switch (formatClass) {
        case AhbFormatClass::DefinedFormat: return "defined-format";
        case AhbFormatClass::ExternalFormatSampledOnly: return "external-format-sampled-only";
        case AhbFormatClass::Unsupported: return "unsupported-or-unprobed";
    }
    return "unsupported-or-unprobed";
}

} // namespace LSFG::Core

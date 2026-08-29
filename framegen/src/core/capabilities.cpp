#include "core/capabilities.hpp"

namespace LSFG::Core {

SupportDecision evaluateCapabilities(
        const VulkanCapabilities& caps,
        const SupportRequirements& requirements) {
    SupportDecision decision{};

    if (caps.apiVersion < VK_API_VERSION_1_2) {
        decision.rejectionReason = "Vulkan 1.2 or newer is required";
        return decision;
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
    if (!caps.timelineSemaphore) {
        decision.rejectionReason = "timeline semaphore support is required";
        return decision;
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
        // The Android framegen backend carries an audited sync2->sync1 barrier
        // translation shim in common/utils.cpp. This path intentionally keeps
        // Vulkan 1.2 devices eligible when synchronization2 is unavailable.
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

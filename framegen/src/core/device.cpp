#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/capabilities.hpp"
#include "core/device.hpp"
#include "core/image.hpp"
#include "core/instance.hpp"
#include "common/exception.hpp"
#include "lsfg_backend.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace LSFG::Core;

namespace {

bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    for (const auto& extension : extensions)
        if (std::strcmp(extension.extensionName, name) == 0) return true;
    return false;
}

void requireExtension(const std::vector<VkExtensionProperties>& available,
        std::vector<const char*>& enabled, const char* name) {
    if (!hasExtension(available, name))
        throw LSFG::vulkan_error(VK_ERROR_EXTENSION_NOT_PRESENT,
            std::string("Missing required device extension: ") + name);
    enabled.push_back(name);
}

LSFG::DeviceIdentity readIdentity(VkPhysicalDevice device) {
    VkPhysicalDeviceIDProperties idProperties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &idProperties,
    };
    vkGetPhysicalDeviceProperties2(device, &properties);

    LSFG::DeviceIdentity identity{};
    std::copy_n(idProperties.deviceUUID, VK_UUID_SIZE, identity.deviceUUID.begin());
    std::copy_n(idProperties.driverUUID, VK_UUID_SIZE, identity.driverUUID.begin());
    return identity;
}

std::string uuidString(const std::array<uint8_t, VK_UUID_SIZE>& uuid) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : uuid)
        out << std::setw(2) << static_cast<unsigned int>(byte);
    return out.str();
}

bool probeAhbImageUsage(VkPhysicalDevice physicalDevice, VkFormat format,
        VkImageUsageFlags usage) {
#ifdef __ANDROID__
    if (vkGetPhysicalDeviceImageFormatProperties2 == nullptr)
        return false;
    const VkPhysicalDeviceExternalImageFormatInfo externalInfo{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
    };
    const VkPhysicalDeviceImageFormatInfo2 imageInfo{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &externalInfo,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
    };
    VkExternalImageFormatProperties externalProperties{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 imageProperties{
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &externalProperties,
    };
    const auto result = vkGetPhysicalDeviceImageFormatProperties2(
        physicalDevice, &imageInfo, &imageProperties);
    return result == VK_SUCCESS
        && (externalProperties.externalMemoryProperties.externalMemoryFeatures
            & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
#else
    (void)physicalDevice;
    (void)format;
    (void)usage;
    return true;
#endif
}


bool probeOpaqueFdExternalSemaphore(VkPhysicalDevice physicalDevice) {
#ifdef __ANDROID__
    if (vkGetPhysicalDeviceExternalSemaphoreProperties == nullptr)
        return false;
    const VkPhysicalDeviceExternalSemaphoreInfo info{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkExternalSemaphoreProperties properties{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
    };
    vkGetPhysicalDeviceExternalSemaphoreProperties(physicalDevice, &info, &properties);
    constexpr VkExternalSemaphoreFeatureFlags required =
        VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
        VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
    return (properties.externalSemaphoreFeatures & required) == required;
#else
    (void)physicalDevice;
    return false;
#endif
}

} // namespace

const Image& Device::getFallbackDescriptorImage() const {
    return *this->fallbackDescriptorImage;
}

Device::Device(const Instance& instance, const LSFG::DeviceIdentity& requestedIdentity,
        VkFormat sharedFormat) {
    uint32_t deviceCount{};
    auto res = vkEnumeratePhysicalDevices(instance.handle(), &deviceCount, nullptr);
    if (res != VK_SUCCESS || deviceCount == 0)
        throw LSFG::vulkan_error(res, "Failed to enumerate physical devices");
    std::vector<VkPhysicalDevice> devices(deviceCount);
    res = vkEnumeratePhysicalDevices(instance.handle(), &deviceCount, devices.data());
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to get physical devices");

    std::optional<VkPhysicalDevice> selectedPhysicalDevice;
    VkPhysicalDeviceProperties properties{};
    LSFG::DeviceIdentity selectedIdentity{};
    for (const auto candidate : devices) {
        const auto candidateIdentity = readIdentity(candidate);
        if (candidateIdentity == requestedIdentity) {
            selectedPhysicalDevice = candidate;
            selectedIdentity = candidateIdentity;
            vkGetPhysicalDeviceProperties(candidate, &properties);
            break;
        }
    }
    if (!selectedPhysicalDevice)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Could not find the exact physical-device/driver UUID pair selected by the game");
    if (properties.apiVersion < VK_API_VERSION_1_1)
        throw LSFG::vulkan_error(VK_ERROR_INCOMPATIBLE_DRIVER,
            "Selected physical device does not expose Vulkan 1.1");

    const auto physicalDevice = *selectedPhysicalDevice;
    uint32_t extensionCount{};
    res = vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to enumerate device extensions");
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    res = vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr,
        &extensionCount, availableExtensions.data());
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to read device extensions");

    const bool api12 = properties.apiVersion >= VK_API_VERSION_1_2;
    const bool api13 = properties.apiVersion >= VK_API_VERSION_1_3;
    const bool hasSync2Ext = hasExtension(availableExtensions,
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    const bool hasFloat16Ext = hasExtension(availableExtensions,
        VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
    const bool hasTimelineExt = hasExtension(availableExtensions,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    const bool hasDriverProperties = api12 || hasExtension(availableExtensions,
        VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);

    VkPhysicalDeviceDriverProperties driverProperties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
    };
    VkPhysicalDeviceIDProperties idProperties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
        .pNext = hasDriverProperties ? &driverProperties : nullptr,
    };
    VkPhysicalDeviceProperties2 properties2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &idProperties,
    };
    vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);

    VkPhysicalDeviceSubgroupProperties subgroup{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 subgroupProperties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &subgroup,
    };
    vkGetPhysicalDeviceProperties2(physicalDevice, &subgroupProperties);

    VkPhysicalDeviceSynchronization2Features sync2Probe{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
    };
    VkPhysicalDeviceVulkan13Features features13Probe{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    };
    VkPhysicalDeviceVulkan12Features features12Probe{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    };
    VkPhysicalDeviceShaderFloat16Int8FeaturesKHR float16Probe{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_KHR,
    };
    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR timelineProbe{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR,
    };

    void* featureProbeHead = nullptr;
    if (api13) {
        features13Probe.pNext = featureProbeHead;
        featureProbeHead = &features13Probe;
    } else if (hasSync2Ext) {
        sync2Probe.pNext = featureProbeHead;
        featureProbeHead = &sync2Probe;
    }
    if (api12) {
        features12Probe.pNext = featureProbeHead;
        featureProbeHead = &features12Probe;
    } else {
        if (hasFloat16Ext) {
            float16Probe.pNext = featureProbeHead;
            featureProbeHead = &float16Probe;
        }
        if (hasTimelineExt) {
            timelineProbe.pNext = featureProbeHead;
            featureProbeHead = &timelineProbe;
        }
    }
    VkPhysicalDeviceFeatures2 featuresProbe{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = featureProbeHead,
    };
    vkGetPhysicalDeviceFeatures2(physicalDevice, &featuresProbe);

    VulkanCapabilities caps{};
    caps.apiVersion = properties.apiVersion;
#ifdef __ANDROID__
    caps.androidHardwareBuffer = hasExtension(availableExtensions,
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
#else
    caps.androidHardwareBuffer = true;
#endif
    caps.synchronization2Core = api13;
    caps.synchronization2Extension = hasSync2Ext;
    caps.synchronization2Feature = api13
        ? features13Probe.synchronization2 == VK_TRUE
        : (hasSync2Ext && sync2Probe.synchronization2 == VK_TRUE);
    caps.legacyPipelineBarrier = true;
    caps.timelineSemaphore = api12
        ? features12Probe.timelineSemaphore == VK_TRUE
        : (hasTimelineExt && timelineProbe.timelineSemaphore == VK_TRUE);
    caps.shaderFloat16 = api12
        ? features12Probe.shaderFloat16 == VK_TRUE
        : (hasFloat16Ext && float16Probe.shaderFloat16 == VK_TRUE);
    caps.subgroupStages = subgroup.supportedStages;
    caps.subgroupOperations = subgroup.supportedOperations;
    caps.subgroupSize = subgroup.subgroupSize;

    const SupportRequirements requirements{
#ifdef __ANDROID__
        .requireAhb = true,
#else
        .requireAhb = false,
#endif
        .requiredSubgroupStages = 0,
        .requiredSubgroupOperations = 0,
    };
    const auto decision = evaluateCapabilities(caps, requirements);
    if (!decision.supported)
        throw LSFG::vulkan_error(VK_ERROR_FEATURE_NOT_PRESENT,
            "LSFG capability check failed: " + decision.rejectionReason);

    this->diagnostics.apiVersion = properties.apiVersion;
    this->diagnostics.driverVersion = properties.driverVersion;
    this->diagnostics.identity = selectedIdentity;
    this->diagnostics.driverName = hasDriverProperties && driverProperties.driverName[0] != '\0'
        ? driverProperties.driverName
        : properties.deviceName;
    this->diagnostics.driverInfo = hasDriverProperties ? driverProperties.driverInfo : "";
#ifdef __ANDROID__
    this->diagnostics.ahbR16fStorage = probeAhbImageUsage(physicalDevice,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    this->diagnostics.ahbR16fTransferSrc = probeAhbImageUsage(physicalDevice,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    this->diagnostics.ahbR16fTransferDst = probeAhbImageUsage(physicalDevice,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    this->diagnostics.ahbR8Storage = probeAhbImageUsage(physicalDevice,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    this->diagnostics.externalSemaphoreFd =
        hasExtension(availableExtensions, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME)
        && probeOpaqueFdExternalSemaphore(physicalDevice);
    const bool directStorage = probeAhbImageUsage(physicalDevice, sharedFormat,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    const bool transferSrc = probeAhbImageUsage(physicalDevice, sharedFormat,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    const bool transferDst = probeAhbImageUsage(physicalDevice, sharedFormat,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    this->diagnostics.ahbTransportMode = directStorage
        ? LSFG::AhbTransportMode::DirectStorage
        : (transferSrc && transferDst
            ? LSFG::AhbTransportMode::TransportOnly
            : LSFG::AhbTransportMode::Unsupported);
#else
    this->diagnostics.ahbTransportMode = LSFG::AhbTransportMode::DirectStorage;
#endif

    std::cerr << "lsfg-vk: backend-init apiVersion="
              << VK_VERSION_MAJOR(this->diagnostics.apiVersion) << '.'
              << VK_VERSION_MINOR(this->diagnostics.apiVersion)
              << " driverName=\"" << this->diagnostics.driverName << "\""
              << " driverVersion=" << this->diagnostics.driverVersion
              << " deviceUUID=" << uuidString(this->diagnostics.identity.deviceUUID)
              << " driverUUID=" << uuidString(this->diagnostics.identity.driverUUID)
              << " ahbR16fStorage=" << (this->diagnostics.ahbR16fStorage ? 1 : 0)
              << " ahbMode=" << LSFG::ahbTransportModeName(this->diagnostics.ahbTransportMode)
              << " externalSemaphoreFd=" << (this->diagnostics.externalSemaphoreFd ? 1 : 0)
              << " sync=" << synchronizationPathName(decision.synchronizationPath)
              << " fp=" << shaderPrecisionName(decision.shaderPrecision) << '\n';

    uint32_t familyCount{};
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, queueFamilies.data());
    std::optional<uint32_t> computeFamilyIdx;
    for (uint32_t i = 0; i < familyCount; ++i) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            computeFamilyIdx = i;
            break;
        }
    }
    if (!computeFamilyIdx)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "No compute queue family found");

    std::vector<const char*> enabledExtensions;
#ifdef __ANDROID__
    requireExtension(availableExtensions, enabledExtensions,
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
    if (this->diagnostics.externalSemaphoreFd)
        enabledExtensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
#else
    requireExtension(availableExtensions, enabledExtensions,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    requireExtension(availableExtensions, enabledExtensions,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
#endif
    if (decision.synchronizationPath == SynchronizationPath::KhrSync2)
        enabledExtensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    if (!api12 && caps.shaderFloat16)
        enabledExtensions.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);

    const bool hasRobustness2 = hasExtension(availableExtensions,
        VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    VkPhysicalDeviceRobustness2FeaturesEXT robustnessProbe{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
    };
    if (hasRobustness2) {
        VkPhysicalDeviceFeatures2 probe{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &robustnessProbe,
        };
        vkGetPhysicalDeviceFeatures2(physicalDevice, &probe);
        if (robustnessProbe.nullDescriptor == VK_TRUE)
            enabledExtensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    }
    const bool enableNullDescriptor = hasRobustness2 && robustnessProbe.nullDescriptor == VK_TRUE;

    VkPhysicalDeviceFeatures enabledCore{};
    enabledCore.shaderStorageImageExtendedFormats =
        featuresProbe.features.shaderStorageImageExtendedFormats;
    enabledCore.shaderStorageImageReadWithoutFormat =
        featuresProbe.features.shaderStorageImageReadWithoutFormat;
    enabledCore.shaderStorageImageWriteWithoutFormat =
        featuresProbe.features.shaderStorageImageWriteWithoutFormat;
    enabledCore.shaderInt16 = featuresProbe.features.shaderInt16;

    VkPhysicalDeviceRobustness2FeaturesEXT robustnessEnable{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
        .nullDescriptor = enableNullDescriptor ? VK_TRUE : VK_FALSE,
    };
    VkPhysicalDeviceSynchronization2Features sync2Enable{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
        .synchronization2 = decision.synchronizationPath == SynchronizationPath::KhrSync2,
    };
    VkPhysicalDeviceVulkan13Features features13Enable{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = decision.synchronizationPath == SynchronizationPath::Core13Sync2,
    };
    VkPhysicalDeviceVulkan12Features features12Enable{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .shaderFloat16 = api12 && caps.shaderFloat16 ? VK_TRUE : VK_FALSE,
        .timelineSemaphore = VK_FALSE,
        .vulkanMemoryModel = api12 ? features12Probe.vulkanMemoryModel : VK_FALSE,
    };
    VkPhysicalDeviceShaderFloat16Int8FeaturesKHR float16Enable{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_KHR,
        .shaderFloat16 = !api12 && caps.shaderFloat16 ? VK_TRUE : VK_FALSE,
    };

    void* enableHead = enableNullDescriptor ? &robustnessEnable : nullptr;
    if (decision.synchronizationPath == SynchronizationPath::Core13Sync2) {
        features13Enable.pNext = enableHead;
        enableHead = &features13Enable;
    } else if (decision.synchronizationPath == SynchronizationPath::KhrSync2) {
        sync2Enable.pNext = enableHead;
        enableHead = &sync2Enable;
    }
    if (api12) {
        features12Enable.pNext = enableHead;
        enableHead = &features12Enable;
    } else if (caps.shaderFloat16) {
        float16Enable.pNext = enableHead;
        enableHead = &float16Enable;
    }

    VkPhysicalDeviceFeatures2 enabledFeatures2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = enableHead,
        .features = enabledCore,
    };
    const float priority = 1.0F;
    const VkDeviceQueueCreateInfo queueInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = *computeFamilyIdx,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const VkDeviceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enabledFeatures2,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
        .enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size()),
        .ppEnabledExtensionNames = enabledExtensions.data(),
    };

    VkDevice handle{};
    res = vkCreateDevice(physicalDevice, &createInfo, nullptr, &handle);
    if (res != VK_SUCCESS || handle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create capability-selected logical device");
    volkLoadDevice(handle);

    if (decision.synchronizationPath == SynchronizationPath::KhrSync2
            && vkCmdPipelineBarrier2 == nullptr && vkCmdPipelineBarrier2KHR != nullptr) {
        vkCmdPipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(vkCmdPipelineBarrier2KHR);
    }
    if (decision.synchronizationPath == SynchronizationPath::LegacyPipelineBarrier)
        vkCmdPipelineBarrier2 = nullptr;

    VkQueue queue{};
    vkGetDeviceQueue(handle, *computeFamilyIdx, 0, &queue);
    this->computeQueue = queue;
    this->computeFamilyIdx = *computeFamilyIdx;
    this->physicalDevice = physicalDevice;
    this->nullDescriptorSupported = enableNullDescriptor;
    this->device = std::shared_ptr<VkDevice>(
        new VkDevice(handle), [](VkDevice* device) { vkDestroyDevice(*device, nullptr); });

    if (!this->nullDescriptorSupported) {
        this->fallbackDescriptorImage = std::make_shared<Core::Image>(*this,
            VkExtent2D{1, 1}, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    }
}

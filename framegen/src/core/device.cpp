#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/capabilities.hpp"
#include "core/device.hpp"
#include "core/image.hpp"
#include "core/instance.hpp"
#include "common/exception.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
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

} // namespace

const Image& Device::getFallbackDescriptorImage() const {
    return *this->fallbackDescriptorImage;
}

Device::Device(const Instance& instance, uint64_t deviceUUID) {
    uint32_t deviceCount{};
    auto res = vkEnumeratePhysicalDevices(instance.handle(), &deviceCount, nullptr);
    if (res != VK_SUCCESS || deviceCount == 0)
        throw LSFG::vulkan_error(res, "Failed to enumerate physical devices");
    std::vector<VkPhysicalDevice> devices(deviceCount);
    res = vkEnumeratePhysicalDevices(instance.handle(), &deviceCount, devices.data());
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to get physical devices");

    std::optional<VkPhysicalDevice> physicalDevice;
    VkPhysicalDeviceProperties properties{};
    for (const auto device : devices) {
        VkPhysicalDeviceProperties candidate{};
        vkGetPhysicalDeviceProperties(device, &candidate);
        const uint64_t id = static_cast<uint64_t>(candidate.vendorID) << 32 | candidate.deviceID;
        if (deviceUUID == id || deviceUUID == 0x1463ABAC) {
            physicalDevice = device;
            properties = candidate;
            break;
        }
    }
    if (!physicalDevice)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Could not find physical device selected by the game");
    if (properties.apiVersion < VK_API_VERSION_1_2)
        throw LSFG::vulkan_error(VK_ERROR_INCOMPATIBLE_DRIVER,
            "Selected physical device does not expose Vulkan 1.2");

    uint32_t extensionCount{};
    res = vkEnumerateDeviceExtensionProperties(*physicalDevice, nullptr, &extensionCount, nullptr);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to enumerate device extensions");
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    res = vkEnumerateDeviceExtensionProperties(*physicalDevice, nullptr,
        &extensionCount, availableExtensions.data());
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to read device extensions");

    const bool api13 = properties.apiVersion >= VK_API_VERSION_1_3;
    const bool hasSync2Ext = hasExtension(availableExtensions,
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);

    VkPhysicalDeviceSubgroupProperties subgroup{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 properties2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &subgroup,
    };
    vkGetPhysicalDeviceProperties2(*physicalDevice, &properties2);

    VkPhysicalDeviceSynchronization2Features sync2Probe{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
    };
    VkPhysicalDeviceVulkan13Features features13Probe{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    };
    VkPhysicalDeviceVulkan12Features features12Probe{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    };
    void* featureProbeHead = &features12Probe;
    if (api13) {
        features13Probe.pNext = featureProbeHead;
        featureProbeHead = &features13Probe;
    } else if (hasSync2Ext) {
        sync2Probe.pNext = featureProbeHead;
        featureProbeHead = &sync2Probe;
    }
    VkPhysicalDeviceFeatures2 featuresProbe{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = featureProbeHead,
    };
    vkGetPhysicalDeviceFeatures2(*physicalDevice, &featuresProbe);

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
    caps.timelineSemaphore = features12Probe.timelineSemaphore == VK_TRUE;
    caps.shaderFloat16 = features12Probe.shaderFloat16 == VK_TRUE;
    caps.subgroupStages = subgroup.supportedStages;
    caps.subgroupOperations = subgroup.supportedOperations;
    caps.subgroupSize = subgroup.subgroupSize;

    // The audited LSFG 3.1/3.1P shaders are compute shaders, but the current
    // DXBC/SPIR-V and precompiled FP16 shader paths do not emit Vulkan subgroup
    // instructions. Keep the complete reported masks for diagnostics while
    // requiring no subgroup operation category until the shader payload does.
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

    std::cerr << "lsfg-vk: capabilities api="
              << VK_VERSION_MAJOR(caps.apiVersion) << '.' << VK_VERSION_MINOR(caps.apiVersion)
              << " ahb=" << caps.androidHardwareBuffer
              << " sync=" << synchronizationPathName(decision.synchronizationPath)
              << " fp=" << shaderPrecisionName(decision.shaderPrecision)
              << " subgroupStages=0x" << std::hex << caps.subgroupStages
              << " subgroupOps=0x" << caps.subgroupOperations
              << " subgroupSize=" << std::dec << caps.subgroupSize << '\n';

    uint32_t familyCount{};
    vkGetPhysicalDeviceQueueFamilyProperties(*physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(*physicalDevice, &familyCount, queueFamilies.data());
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
#else
    requireExtension(availableExtensions, enabledExtensions,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    requireExtension(availableExtensions, enabledExtensions,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
#endif
    if (decision.synchronizationPath == SynchronizationPath::KhrSync2)
        enabledExtensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);

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
        vkGetPhysicalDeviceFeatures2(*physicalDevice, &probe);
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
        .pNext = enableNullDescriptor ? &robustnessEnable : nullptr,
        .synchronization2 = decision.synchronizationPath == SynchronizationPath::KhrSync2,
    };
    VkPhysicalDeviceVulkan13Features features13Enable{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = enableNullDescriptor ? &robustnessEnable : nullptr,
        .synchronization2 = decision.synchronizationPath == SynchronizationPath::Core13Sync2,
    };
    VkPhysicalDeviceVulkan12Features features12Enable{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .shaderFloat16 = caps.shaderFloat16 ? VK_TRUE : VK_FALSE,
        .timelineSemaphore = VK_TRUE,
        .vulkanMemoryModel = features12Probe.vulkanMemoryModel,
    };
    if (decision.synchronizationPath == SynchronizationPath::Core13Sync2)
        features12Enable.pNext = &features13Enable;
    else if (decision.synchronizationPath == SynchronizationPath::KhrSync2)
        features12Enable.pNext = &sync2Enable;
    else if (enableNullDescriptor)
        features12Enable.pNext = &robustnessEnable;

    VkPhysicalDeviceFeatures2 enabledFeatures2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &features12Enable,
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
    res = vkCreateDevice(*physicalDevice, &createInfo, nullptr, &handle);
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
    this->physicalDevice = *physicalDevice;
    this->nullDescriptorSupported = enableNullDescriptor;
    this->device = std::shared_ptr<VkDevice>(
        new VkDevice(handle), [](VkDevice* device) { vkDestroyDevice(*device, nullptr); });

    if (!this->nullDescriptorSupported) {
        this->fallbackDescriptorImage = std::make_shared<Core::Image>(*this,
            VkExtent2D{1, 1}, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    }
}

#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def write(path: str, content: str) -> None:
    target = ROOT / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(content, encoding="utf-8")


def replace_once(path: str, old: str, new: str) -> None:
    content = read(path)
    count = content.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one match, found {count}: {old[:80]!r}")
    write(path, content.replace(old, new, 1))


def replace_all(path: str, old: str, new: str, expected: int) -> None:
    content = read(path)
    count = content.count(old)
    if count != expected:
        raise RuntimeError(f"{path}: expected {expected} matches, found {count}: {old[:80]!r}")
    write(path, content.replace(old, new))


write("framegen/public/lsfg_backend.hpp", r'''#pragma once

#include <vulkan/vulkan_core.h>

#include <array>
#include <cstdint>
#include <string>

namespace LSFG {

struct DeviceIdentity {
    std::array<uint8_t, VK_UUID_SIZE> deviceUUID{};
    std::array<uint8_t, VK_UUID_SIZE> driverUUID{};

    [[nodiscard]] bool operator==(const DeviceIdentity& other) const noexcept {
        return deviceUUID == other.deviceUUID && driverUUID == other.driverUUID;
    }
};

enum class AhbTransportMode {
    Unsupported,
    DirectStorage,
    TransportOnly,
};

struct BackendDiagnostics {
    uint32_t apiVersion{VK_API_VERSION_1_0};
    uint32_t driverVersion{0};
    std::string driverName;
    std::string driverInfo;
    DeviceIdentity identity{};
    bool ahbR16fStorage{false};
    bool ahbR16fTransferSrc{false};
    bool ahbR16fTransferDst{false};
    bool ahbR8Storage{false};
    AhbTransportMode ahbTransportMode{AhbTransportMode::Unsupported};
};

inline constexpr uint64_t DEFAULT_DRIVER_WAIT_TIMEOUT_NS = 500'000'000ULL;

[[nodiscard]] inline const char* ahbTransportModeName(AhbTransportMode mode) noexcept {
    switch (mode) {
        case AhbTransportMode::DirectStorage: return "direct-storage";
        case AhbTransportMode::TransportOnly: return "transport-only";
        case AhbTransportMode::Unsupported: return "unsupported";
    }
    return "unsupported";
}

} // namespace LSFG
''')

# Game-side device identity is captured at logical-device creation and carried to every swapchain.
replace_once(
    "include/hooks.hpp",
    '#include <vulkan/vulkan_core.h>\n',
    '#include <vulkan/vulkan_core.h>\n#include "lsfg_backend.hpp"\n'
)
replace_once(
    "include/hooks.hpp",
    '        VkPhysicalDevice physicalDevice;\n        std::pair<uint32_t, VkQueue> queue;\n',
    '        VkPhysicalDevice physicalDevice;\n        LSFG::DeviceIdentity identity{};\n        bool identityValid{false};\n        std::pair<uint32_t, VkQueue> queue;\n'
)

# Replace the surrogate vendor/device ID API with real VkPhysicalDeviceIDProperties.
replace_once(
    "include/utils/utils.hpp",
    '#include <vulkan/vulkan_core.h>\n',
    '#include <vulkan/vulkan_core.h>\n#include "lsfg_backend.hpp"\n'
)
replace_once(
    "include/utils/utils.hpp",
    '#include <utility>\n',
    '#include <utility>\n#include <optional>\n'
)
replace_once(
    "include/utils/utils.hpp",
    '''    ///\n    /// Get the UUID of the physical device.\n    ///\n    /// @param physicalDevice The physical device to get the UUID from.\n    /// @return The UUID of the physical device.\n    ///\n    uint64_t getDeviceUUID(VkPhysicalDevice physicalDevice);\n''',
    '''    ///\n    /// Get the Vulkan physical-device and driver UUID pair used for external-object compatibility.\n    ///\n    /// @param physicalDevice The game physical device.\n    /// @param getProperties2 The downstream properties2 entrypoint for the game VkInstance.\n    /// @return Exact device/driver identity, or nullopt when properties2 is unavailable.\n    ///\n    std::optional<LSFG::DeviceIdentity> getDeviceIdentity(\n        VkPhysicalDevice physicalDevice, PFN_vkGetPhysicalDeviceProperties2 getProperties2);\n'''
)
replace_once(
    "src/utils/utils.cpp",
    '''uint64_t Utils::getDeviceUUID(VkPhysicalDevice physicalDevice) {\n    VkPhysicalDeviceProperties properties{};\n    Layer::ovkGetPhysicalDeviceProperties(physicalDevice, &properties);\n\n    return static_cast<uint64_t>(properties.vendorID) << 32 | properties.deviceID;\n}\n''',
    '''std::optional<LSFG::DeviceIdentity> Utils::getDeviceIdentity(\n        VkPhysicalDevice physicalDevice, PFN_vkGetPhysicalDeviceProperties2 getProperties2) {\n    if (getProperties2 == nullptr)\n        return std::nullopt;\n\n    VkPhysicalDeviceIDProperties idProperties{\n        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,\n    };\n    VkPhysicalDeviceProperties2 properties{\n        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,\n        .pNext = &idProperties,\n    };\n    getProperties2(physicalDevice, &properties);\n\n    LSFG::DeviceIdentity identity{};\n    std::copy_n(idProperties.deviceUUID, VK_UUID_SIZE, identity.deviceUUID.begin());\n    std::copy_n(idProperties.driverUUID, VK_UUID_SIZE, identity.driverUUID.begin());\n    return identity;\n}\n'''
)

# Capture identity from the exact downstream instance dispatch used by the game.
replace_once(
    "src/hooks.cpp",
    '''#ifdef __ANDROID__\n        const bool androidAhbSupported = supportsDeviceExtension(physicalDevice,\n            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);\n#else\n        const bool androidAhbSupported = true;\n#endif\n        deviceToInfo.emplace(*pDevice, DeviceInfo {\n            .device = *pDevice,\n            .physicalDevice = physicalDevice,\n            .queue = Utils::findQueue(*pDevice, physicalDevice, pCreateInfo, VK_QUEUE_GRAPHICS_BIT),\n            .androidAhbSupported = androidAhbSupported\n        });\n''',
    '''#ifdef __ANDROID__\n        const bool androidAhbSupported = supportsDeviceExtension(physicalDevice,\n            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);\n#else\n        const bool androidAhbSupported = true;\n#endif\n        auto getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(\n            Layer::ovkGetInstanceProcAddr(layerInstance, "vkGetPhysicalDeviceProperties2"));\n        if (getProperties2 == nullptr) {\n            getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(\n                Layer::ovkGetInstanceProcAddr(layerInstance, "vkGetPhysicalDeviceProperties2KHR"));\n        }\n        const auto identity = Utils::getDeviceIdentity(physicalDevice, getProperties2);\n        if (!identity.has_value()) {\n            Utils::logLimitN("deviceIdentity", 1,\n                "Physical-device ID properties unavailable; LSFG will fail open for this device.");\n        }\n        deviceToInfo.emplace(*pDevice, DeviceInfo {\n            .device = *pDevice,\n            .physicalDevice = physicalDevice,\n            .identity = identity.value_or(LSFG::DeviceIdentity{}),\n            .identityValid = identity.has_value(),\n            .queue = Utils::findQueue(*pDevice, physicalDevice, pCreateInfo, VK_QUEUE_GRAPHICS_BIT),\n            .androidAhbSupported = androidAhbSupported\n        });\n'''
)

# Vulkan instance requests 1.2 on the established fast path and falls back to 1.1 only when needed.
write("framegen/src/core/instance.cpp", r'''#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/instance.hpp"
#include "common/exception.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>

using namespace LSFG::Core;

Instance::Instance() {
    if (volkInitialize() != VK_SUCCESS)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "Failed to initialize Vulkan loader");

    uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion)
        vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < VK_API_VERSION_1_1)
        throw LSFG::vulkan_error(VK_ERROR_INCOMPATIBLE_DRIVER, "LSFG requires Vulkan 1.1 or newer");

    const uint32_t requestedVersion = loaderVersion >= VK_API_VERSION_1_2
        ? VK_API_VERSION_1_2
        : VK_API_VERSION_1_1;
    const VkApplicationInfo appInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "lsfg-vk-base",
        .applicationVersion = VK_MAKE_VERSION(0, 0, 1),
        .pEngineName = "lsfg-vk-base",
        .engineVersion = VK_MAKE_VERSION(0, 0, 1),
        .apiVersion = requestedVersion,
    };
    const VkInstanceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
    };
    VkInstance instanceHandle{};
    const auto res = vkCreateInstance(&createInfo, nullptr, &instanceHandle);
    if (res != VK_SUCCESS || instanceHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create capability-selected Vulkan instance");

    volkLoadInstance(instanceHandle);
    this->instance = std::shared_ptr<VkInstance>(
        new VkInstance(instanceHandle),
        [](VkInstance* instance) { vkDestroyInstance(*instance, nullptr); });
}
''')

# Vulkan 1.1 is valid through legacy barriers + binary semaphores/fences; timeline is not a gate.
replace_once(
    "framegen/src/core/capabilities.cpp",
    '''    if (caps.apiVersion < VK_API_VERSION_1_2) {\n        decision.rejectionReason = "Vulkan 1.2 or newer is required";\n        return decision;\n    }\n''',
    '''    if (caps.apiVersion < VK_API_VERSION_1_1) {\n        decision.rejectionReason = "Vulkan 1.1 or newer is required";\n        return decision;\n    }\n'''
)
replace_once(
    "framegen/src/core/capabilities.cpp",
    '''    if (!caps.timelineSemaphore) {\n        decision.rejectionReason = "timeline semaphore support is required";\n        return decision;\n    }\n\n''',
    '''    // Framegen submissions use binary semaphores and completion fences. Timeline\n    // semaphores are therefore an optional acceleration capability, not a correctness gate.\n\n'''
)

write("framegen/include/core/device.hpp", r'''#pragma once

#include "core/instance.hpp"
#include "lsfg_backend.hpp"

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <memory>

namespace LSFG::Core {

    class Image;

    class Device {
    public:
        Device(const Instance& instance, const LSFG::DeviceIdentity& identity, VkFormat sharedFormat);

        [[nodiscard]] auto handle() const { return *this->device; }
        [[nodiscard]] VkPhysicalDevice getPhysicalDevice() const { return this->physicalDevice; }
        [[nodiscard]] uint32_t getComputeFamilyIdx() const { return this->computeFamilyIdx; }
        [[nodiscard]] VkQueue getComputeQueue() const { return this->computeQueue; }
        [[nodiscard]] bool supportsNullDescriptor() const { return this->nullDescriptorSupported; }
        [[nodiscard]] const Image& getFallbackDescriptorImage() const;
        [[nodiscard]] LSFG::AhbTransportMode getAhbTransportMode() const {
            return this->diagnostics.ahbTransportMode;
        }
        [[nodiscard]] const LSFG::BackendDiagnostics& getDiagnostics() const {
            return this->diagnostics;
        }

        Device(const Core::Device&) noexcept = default;
        Device& operator=(const Core::Device&) noexcept = default;
        Device(Device&&) noexcept = default;
        Device& operator=(Device&&) noexcept = default;
        ~Device() = default;
    private:
        std::shared_ptr<VkDevice> device;
        VkPhysicalDevice physicalDevice{};
        uint32_t computeFamilyIdx{0};
        VkQueue computeQueue{};
        bool nullDescriptorSupported{false};
        std::shared_ptr<Image> fallbackDescriptorImage;
        LSFG::BackendDiagnostics diagnostics{};
    };

}
''')

write("framegen/src/core/device.cpp", r'''#include <volk.h>
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
''')

# Public APIs now carry exact driver identity and the selected shared format.
for public_header in ("framegen/public/lsfg_3_1.hpp", "framegen/public/lsfg_3_1p.hpp"):
    replace_once(public_header, '#include <vulkan/vulkan_core.h>\n', '#include <vulkan/vulkan_core.h>\n#include "lsfg_backend.hpp"\n')
    replace_once(
        public_header,
        '    void initialize(uint64_t deviceUUID,\n        bool isHdr, float flowScale, uint64_t generationCount,\n',
        '    void initialize(const LSFG::DeviceIdentity& identity, VkFormat sharedFormat,\n        bool isHdr, float flowScale, uint64_t generationCount,\n'
    )

for source in ("framegen/v3.1_src/lsfg.cpp", "framegen/v3.1p_src/lsfg.cpp"):
    replace_once(
        source,
        'void LSFG_3_1::initialize(uint64_t deviceUUID,\n' if "v3.1_src" in source else 'void LSFG_3_1P::initialize(uint64_t deviceUUID,\n',
        'void LSFG_3_1::initialize(const LSFG::DeviceIdentity& identity, VkFormat sharedFormat,\n' if "v3.1_src" in source else 'void LSFG_3_1P::initialize(const LSFG::DeviceIdentity& identity, VkFormat sharedFormat,\n'
    )
    replace_once(source, '        .device{*instance, deviceUUID},\n', '        .device{*instance, identity, sharedFormat},\n')

# The game/framegen identity match now happens before Android shared AHB allocation.
replace_once(
    "src/context.cpp",
    '''    const VkFormat format = conf.hdr\n        ? VK_FORMAT_R8G8B8A8_UNORM\n        : VK_FORMAT_R16G16B16A16_SFLOAT;\n\n#ifdef __ANDROID__\n''',
    '''    const VkFormat format = conf.hdr\n        ? VK_FORMAT_R8G8B8A8_UNORM\n        : VK_FORMAT_R16G16B16A16_SFLOAT;\n\n    if (!info.identityValid)\n        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,\n            "Exact Vulkan device/driver UUID provenance is unavailable");\n\n#ifdef __ANDROID__\n'''
)
replace_once(
    "src/context.cpp",
    '''    // Android path: use AHardwareBuffer-backed images for sharing with framegen.\n    // The game VkDevice and framegen VkDevice explicitly transfer EXTERNAL\n    // ownership around every shared-image access, so this path is valid on\n    // stock Android ICDs as well as wrapper/custom drivers.\n\n    this->frame_0 = Mini::Image(info.device, info.physicalDevice,\n''',
    '''    // Select and validate the exact framegen ICD before allocating any shared AHB.\n    auto* lsfgInitialize = LSFG_3_1::initialize;\n    auto* lsfgDeleteContext = LSFG_3_1::deleteContext;\n    if (conf.performance) {\n        lsfgInitialize = LSFG_3_1P::initialize;\n        lsfgDeleteContext = LSFG_3_1P::deleteContext;\n    }\n    setenv("DISABLE_LSFG", "1", 1); // NOLINT\n    lsfgInitialize(\n        info.identity, format,\n        conf.hdr, 1.0F / conf.flowScale, conf.multiplier - 1,\n        [](const std::string& name) {\n            auto dxbc = Extract::getShader(name);\n            auto spirv = Extract::translateShader(dxbc);\n            return spirv;\n        }\n    );\n\n    // Android path: use AHardwareBuffer-backed images for sharing with framegen.\n    // The game VkDevice and framegen VkDevice explicitly transfer EXTERNAL\n    // ownership around every shared-image access, so this path is valid on\n    // stock Android ICDs as well as wrapper/custom drivers.\n    this->frame_0 = Mini::Image(info.device, info.physicalDevice,\n'''
)
replace_once(
    "src/context.cpp",
    '''    // initialize lsfg\n    auto* lsfgInitialize = LSFG_3_1::initialize;\n    auto* lsfgDeleteContext = LSFG_3_1::deleteContext;\n    if (conf.performance) {\n        lsfgInitialize = LSFG_3_1P::initialize;\n        lsfgDeleteContext = LSFG_3_1P::deleteContext;\n    }\n\n    setenv("DISABLE_LSFG", "1", 1); // NOLINT\n\n    lsfgInitialize(\n        Utils::getDeviceUUID(info.physicalDevice),\n        conf.hdr, 1.0F / conf.flowScale, conf.multiplier - 1,\n        [](const std::string& name) {\n            auto dxbc = Extract::getShader(name);\n            auto spirv = Extract::translateShader(dxbc);\n            return spirv;\n        }\n    );\n\n''',
    ''
)
replace_once(
    "src/context.cpp",
    '''    lsfgInitialize(\n        Utils::getDeviceUUID(info.physicalDevice),\n        conf.hdr, 1.0F / conf.flowScale, conf.multiplier - 1,\n''',
    '''    lsfgInitialize(\n        info.identity, format,\n        conf.hdr, 1.0F / conf.flowScale, conf.multiplier - 1,\n'''
)

# Remove this one-shot bootstrap machinery from the generated source commit.
(ROOT / "scripts/apply_task1_capability_refactor.py").unlink()
bootstrap = ROOT / ".github/workflows/task1-capability-bootstrap.yml"
if bootstrap.exists():
    bootstrap.unlink()

print("Task 1 capability refactor applied")

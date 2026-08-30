#include "mini/image.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>

#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#include <android/log.h>
#endif

#include <memory>
#include <cstdint>
#include <optional>
#include <iostream>

using namespace Mini;

Image::Image(VkDevice device, VkPhysicalDevice physicalDevice,
        VkExtent2D extent, VkFormat format,
        VkImageUsageFlags usage, VkImageAspectFlags aspectFlags, int* fd)
        : extent(extent), format(format), aspectFlags(aspectFlags) {
    // create image
    const VkExternalMemoryImageCreateInfo externalInfo{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR
    };
    const VkImageCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &externalInfo,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {
            .width = extent.width,
            .height = extent.height,
            .depth = 1
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkImage imageHandle{};
    auto res = Layer::ovkCreateImage(device, &desc, nullptr, &imageHandle);
    if (res != VK_SUCCESS || imageHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create Vulkan image");

    // find memory type
    VkPhysicalDeviceMemoryProperties memProps;
    Layer::ovkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

    VkMemoryRequirements memReqs;
    Layer::ovkGetImageMemoryRequirements(device, imageHandle, &memReqs);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
    std::optional<uint32_t> memType{};
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1 << i)) && // NOLINTBEGIN
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memType.emplace(i);
            break;
        } // NOLINTEND
    }
    if (!memType.has_value())
        throw LSFG::vulkan_error(VK_ERROR_UNKNOWN, "Unable to find memory type for image");
#pragma clang diagnostic pop

    // allocate and bind memory
    const VkMemoryDedicatedAllocateInfoKHR dedicatedInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
        .image = imageHandle,
    };
    const VkExportMemoryAllocateInfo exportInfo{
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicatedInfo,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR
    };
    const VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &exportInfo,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = memType.value()
    };
    VkDeviceMemory memoryHandle{};
    res = Layer::ovkAllocateMemory(device, &allocInfo, nullptr, &memoryHandle);
    if (res != VK_SUCCESS || memoryHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to allocate memory for Vulkan image");

    res = Layer::ovkBindImageMemory(device, imageHandle, memoryHandle, 0);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to bind memory to Vulkan image");

    // obtain the sharing fd
    const VkMemoryGetFdInfoKHR fdInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = memoryHandle,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
    };
    res = Layer::ovkGetMemoryFdKHR(device, &fdInfo, fd);
    if (res != VK_SUCCESS || *fd < 0)
        throw LSFG::vulkan_error(res, "Failed to obtain sharing fd for Vulkan image");

    // store objects in shared ptr
    this->image = std::shared_ptr<VkImage>(
        new VkImage(imageHandle),
        [dev = device](VkImage* img) {
            Layer::ovkDestroyImage(dev, *img, nullptr);
        }
    );
    this->memory = std::shared_ptr<VkDeviceMemory>(
        new VkDeviceMemory(memoryHandle),
        [dev = device](VkDeviceMemory* mem) {
            Layer::ovkFreeMemory(dev, *mem, nullptr);
        }
    );
}

#ifdef __ANDROID__
Image::Image(VkDevice device, VkPhysicalDevice physicalDevice,
        VkExtent2D extent, VkFormat format,
        VkImageUsageFlags usage, VkImageAspectFlags aspectFlags)
        : extent(extent), format(format), aspectFlags(aspectFlags) {
    // Convert VkFormat to AHardwareBuffer format.
    uint32_t ahbFormat = 0;
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:        ahbFormat = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM; break;
        case VK_FORMAT_R16G16B16A16_SFLOAT:   ahbFormat = AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT; break;
        default:
            throw LSFG::vulkan_error(VK_ERROR_FORMAT_NOT_SUPPORTED,
                "Unsupported VkFormat for AHB allocation");
    }

    // Allocate the shared AHardwareBuffer. These usage flags describe the
    // cross-device GPU use; the VkImage on this device is still limited by
    // its own VkImageUsageFlags below.
    AHardwareBuffer_Desc ahbDesc{
        .width = extent.width,
        .height = extent.height,
        .layers = 1,
        .format = ahbFormat,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE
               | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT
               | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
        .stride = 0,
        .rfu0 = 0,
        .rfu1 = 0,
    };
    AHardwareBuffer* ahbHandle{};
    if (AHardwareBuffer_allocate(&ahbDesc, &ahbHandle) != 0 || ahbHandle == nullptr)
        throw LSFG::vulkan_error(VK_ERROR_OUT_OF_DEVICE_MEMORY,
            "Failed to allocate AHardwareBuffer for image");
    this->ahb = ahbHandle;

    // A stock Android ICD exposes the AHB properties query when the extension
    // is enabled. Query it before importing so allocationSize and memoryTypeBits
    // come from the external allocation, as required by the Android Vulkan
    // import contract. Some wrapper ICDs historically fail to forward this
    // entrypoint, so retain the old image-requirements path only as an explicit
    // compatibility fallback when the entrypoint itself is unavailable.
    VkAndroidHardwareBufferFormatPropertiesANDROID ahbFormatProps{
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID,
    };
    VkAndroidHardwareBufferPropertiesANDROID ahbProps{
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
        .pNext = &ahbFormatProps,
    };
    const bool hasAhbPropertiesQuery =
        Layer::ovkGetDeviceProcAddr(device,
            "vkGetAndroidHardwareBufferPropertiesANDROID") != nullptr;
    VkResult res = VK_SUCCESS;
    if (hasAhbPropertiesQuery) {
        res = Layer::ovkGetAndroidHardwareBufferPropertiesANDROID(device, ahbHandle, &ahbProps);
        if (res != VK_SUCCESS)
            throw LSFG::vulkan_error(res,
                "Failed to query Android hardware-buffer properties");
        if (ahbFormatProps.format == VK_FORMAT_UNDEFINED || ahbFormatProps.format != format)
            throw LSFG::vulkan_error(VK_ERROR_FORMAT_NOT_SUPPORTED,
                "AHardwareBuffer format is not importable as the requested Vulkan format");
    } else {
        std::cerr << "lsfg-vk: AHB properties query unavailable; using compatibility fallback.\n";
    }

    VkExternalMemoryImageCreateInfo extImageInfo{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
    };
    VkImageCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &extImageInfo,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { extent.width, extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage imageHandle{};
    res = Layer::ovkCreateImage(device, &desc, nullptr, &imageHandle);
    if (res != VK_SUCCESS || imageHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create Vulkan image from AHB");

    // Image requirements still constrain this particular VkImage import. On a
    // conformant ICD their memory-type mask intersects the AHB mask. The wrapper
    // fallback uses the image requirements alone, preserving PR #8 compatibility.
    VkMemoryRequirements memReqs;
    Layer::ovkGetImageMemoryRequirements(device, imageHandle, &memReqs);

    VkDeviceSize allocationSize = memReqs.size;
    uint32_t memoryTypeBits = memReqs.memoryTypeBits;
    if (hasAhbPropertiesQuery) {
        allocationSize = ahbProps.allocationSize;
        memoryTypeBits = ahbProps.memoryTypeBits & memReqs.memoryTypeBits;
        if (memoryTypeBits == 0)
            throw LSFG::vulkan_error(VK_ERROR_FORMAT_NOT_SUPPORTED,
                "AHardwareBuffer and VkImage expose no common memory type");
    }

    VkPhysicalDeviceMemoryProperties memProps;
    Layer::ovkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

    uint32_t typeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            typeIndex = i;
            break;
        }
    }
    if (typeIndex == UINT32_MAX) {
        // External Android memory is not required to advertise DEVICE_LOCAL on
        // every ICD. Fall back to the first compatible type from the contract.
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if (memoryTypeBits & (1u << i)) {
                typeIndex = i;
                break;
            }
        }
    }
    if (typeIndex == UINT32_MAX)
        throw LSFG::vulkan_error(VK_ERROR_UNKNOWN,
            "No memory type matches AHardwareBuffer import requirements");

    VkMemoryDedicatedAllocateInfo dedicatedInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = imageHandle,
    };
    VkImportAndroidHardwareBufferInfoANDROID importInfo{
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        .pNext = &dedicatedInfo,
        .buffer = ahbHandle,
    };
    VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &importInfo,
        .allocationSize = allocationSize,
        .memoryTypeIndex = typeIndex,
    };
    VkDeviceMemory memoryHandle{};
    res = Layer::ovkAllocateMemory(device, &allocInfo, nullptr, &memoryHandle);
    if (res != VK_SUCCESS || memoryHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to import AHB into Vulkan memory");

    res = Layer::ovkBindImageMemory(device, imageHandle, memoryHandle, 0);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to bind AHB memory to Vulkan image");

    // Store objects with proper cleanup.
    this->image = std::shared_ptr<VkImage>(
        new VkImage(imageHandle),
        [dev = device](VkImage* img) {
            Layer::ovkDestroyImage(dev, *img, nullptr);
        }
    );
    this->memory = std::shared_ptr<VkDeviceMemory>(
        new VkDeviceMemory(memoryHandle),
        [dev = device](VkDeviceMemory* mem) {
            Layer::ovkFreeMemory(dev, *mem, nullptr);
        }
    );
    this->ahbRef = std::shared_ptr<AHardwareBuffer>(
        ahbHandle,
        [](AHardwareBuffer* b) {
            if (b) AHardwareBuffer_release(b);
        }
    );
}
#endif

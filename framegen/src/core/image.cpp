#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/image.hpp"
#include "core/device.hpp"
#include "common/exception.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#endif

namespace {
#ifdef __ANDROID__
    struct AhbHandleGuard {
        AHardwareBuffer* handle{};

        ~AhbHandleGuard() {
            if (handle != nullptr)
                AHardwareBuffer_release(handle);
        }

        void release() noexcept { handle = nullptr; }
    };
#endif

    struct VulkanImageHandlesGuard {
        VkDevice device{VK_NULL_HANDLE};
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};

        void destroyView() noexcept {
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(device, view, nullptr);
                view = VK_NULL_HANDLE;
            }
        }

        void destroyImage() noexcept {
            if (image != VK_NULL_HANDLE) {
                vkDestroyImage(device, image, nullptr);
                image = VK_NULL_HANDLE;
            }
        }

        void destroyMemory() noexcept {
            if (memory != VK_NULL_HANDLE) {
                vkFreeMemory(device, memory, nullptr);
                memory = VK_NULL_HANDLE;
            }
        }

        ~VulkanImageHandlesGuard() {
            destroyView();
            destroyImage();
            destroyMemory();
        }
    };

    struct VulkanImageOwners {
        VulkanImageHandlesGuard handles;
        std::shared_ptr<VkImage> image;
        std::shared_ptr<VkDeviceMemory> memory;
        std::shared_ptr<VkImageView> view;
        std::shared_ptr<VkImageLayout> layout;

        ~VulkanImageOwners() {
            // Release dependencies before the raw-handle fallback cleanup.
            handles.destroyView();
            view.reset();
            handles.destroyImage();
            image.reset();
            handles.destroyMemory();
            memory.reset();
            layout.reset();
        }
    };
}

using namespace LSFG::Core;

Image::Image(const Core::Device& device, VkExtent2D extent, VkFormat format,
        VkImageUsageFlags usage, VkImageAspectFlags aspectFlags)
        : extent(extent), format(format), aspectFlags(aspectFlags) {
    const VkImageCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { .width = extent.width, .height = extent.height, .depth = 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkImage imageHandle{};
    auto res = vkCreateImage(device.handle(), &desc, nullptr, &imageHandle);
    VulkanImageOwners owners;
    owners.handles.device = device.handle();
    owners.handles.image = imageHandle;
    if (res != VK_SUCCESS || imageHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create Vulkan image");

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(device.getPhysicalDevice(), &memProps);
    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device.handle(), imageHandle, &memReqs);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
    std::optional<uint32_t> memType{};
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memType.emplace(i);
            break;
        }
    }
    if (!memType.has_value())
        throw LSFG::vulkan_error(VK_ERROR_UNKNOWN, "Unable to find memory type for image");
#pragma clang diagnostic pop

    const VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = memType.value()
    };
    VkDeviceMemory memoryHandle{};
    res = vkAllocateMemory(device.handle(), &allocInfo, nullptr, &memoryHandle);
    owners.handles.memory = memoryHandle;
    if (res != VK_SUCCESS || memoryHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to allocate memory for Vulkan image");
    res = vkBindImageMemory(device.handle(), imageHandle, memoryHandle, 0);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to bind memory to Vulkan image");

    const VkImageViewCreateInfo viewDesc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = imageHandle,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY
        },
        .subresourceRange = {
            .aspectMask = aspectFlags,
            .levelCount = 1,
            .layerCount = 1
        }
    };
    VkImageView viewHandle{};
    res = vkCreateImageView(device.handle(), &viewDesc, nullptr, &viewHandle);
    owners.handles.view = viewHandle;
    if (res != VK_SUCCESS || viewHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create image view");

    owners.layout = std::make_shared<VkImageLayout>(VK_IMAGE_LAYOUT_UNDEFINED);
    owners.image = std::shared_ptr<VkImage>(new VkImage(imageHandle),
        [dev = device.handle()](VkImage* img) {
            if (img != nullptr) {
                vkDestroyImage(dev, *img, nullptr);
                delete img;
            }
        });
    owners.handles.image = VK_NULL_HANDLE;
    owners.memory = std::shared_ptr<VkDeviceMemory>(new VkDeviceMemory(memoryHandle),
        [dev = device.handle()](VkDeviceMemory* mem) {
            if (mem != nullptr) {
                vkFreeMemory(dev, *mem, nullptr);
                delete mem;
            }
        });
    owners.handles.memory = VK_NULL_HANDLE;
    owners.view = std::shared_ptr<VkImageView>(new VkImageView(viewHandle),
        [dev = device.handle()](VkImageView* imgView) {
            if (imgView != nullptr) {
                vkDestroyImageView(dev, *imgView, nullptr);
                delete imgView;
            }
        });
    owners.handles.view = VK_NULL_HANDLE;

    this->layout = std::move(owners.layout);
    this->image = std::move(owners.image);
    this->memory = std::move(owners.memory);
    this->view = std::move(owners.view);
}

Image::Image(const Core::Device& device, VkExtent2D extent, VkFormat format,
        VkImageUsageFlags usage, VkImageAspectFlags aspectFlags, int fd)
        : extent(extent), format(format), aspectFlags(aspectFlags) {
    const VkExternalMemoryImageCreateInfo externalInfo{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR
    };
    const VkImageCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &externalInfo,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { .width = extent.width, .height = extent.height, .depth = 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkImage imageHandle{};
    auto res = vkCreateImage(device.handle(), &desc, nullptr, &imageHandle);
    VulkanImageOwners owners;
    owners.handles.device = device.handle();
    owners.handles.image = imageHandle;
    if (res != VK_SUCCESS || imageHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create Vulkan image");

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(device.getPhysicalDevice(), &memProps);
    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device.handle(), imageHandle, &memReqs);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
    std::optional<uint32_t> memType{};
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memType.emplace(i);
            break;
        }
    }
    if (!memType.has_value())
        throw LSFG::vulkan_error(VK_ERROR_UNKNOWN, "Unable to find memory type for image");
#pragma clang diagnostic pop

    const VkMemoryDedicatedAllocateInfoKHR dedicatedInfo2{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
        .image = imageHandle,
    };
    const VkImportMemoryFdInfoKHR importInfo{
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &dedicatedInfo2,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
        .fd = fd
    };
    const VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = fd == -1 ? nullptr : &importInfo,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = memType.value()
    };
    VkDeviceMemory memoryHandle{};
    res = vkAllocateMemory(device.handle(), &allocInfo, nullptr, &memoryHandle);
    owners.handles.memory = memoryHandle;
    if (res != VK_SUCCESS || memoryHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to allocate memory for Vulkan image");
    res = vkBindImageMemory(device.handle(), imageHandle, memoryHandle, 0);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to bind memory to Vulkan image");

    const VkImageViewCreateInfo viewDesc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = imageHandle,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY
        },
        .subresourceRange = { .aspectMask = aspectFlags, .levelCount = 1, .layerCount = 1 }
    };
    VkImageView viewHandle{};
    res = vkCreateImageView(device.handle(), &viewDesc, nullptr, &viewHandle);
    owners.handles.view = viewHandle;
    if (res != VK_SUCCESS || viewHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create image view");

    owners.layout = std::make_shared<VkImageLayout>(VK_IMAGE_LAYOUT_UNDEFINED);
    owners.image = std::shared_ptr<VkImage>(new VkImage(imageHandle),
        [dev = device.handle()](VkImage* img) {
            if (img != nullptr) {
                vkDestroyImage(dev, *img, nullptr);
                delete img;
            }
        });
    owners.handles.image = VK_NULL_HANDLE;
    owners.memory = std::shared_ptr<VkDeviceMemory>(new VkDeviceMemory(memoryHandle),
        [dev = device.handle()](VkDeviceMemory* mem) {
            if (mem != nullptr) {
                vkFreeMemory(dev, *mem, nullptr);
                delete mem;
            }
        });
    owners.handles.memory = VK_NULL_HANDLE;
    owners.view = std::shared_ptr<VkImageView>(new VkImageView(viewHandle),
        [dev = device.handle()](VkImageView* imgView) {
            if (imgView != nullptr) {
                vkDestroyImageView(dev, *imgView, nullptr);
                delete imgView;
            }
        });
    owners.handles.view = VK_NULL_HANDLE;

    this->layout = std::move(owners.layout);
    this->image = std::move(owners.image);
    this->memory = std::move(owners.memory);
    this->view = std::move(owners.view);
}

#ifdef __ANDROID__

Image::Image(const Core::Device& device, VkExtent2D extent, VkFormat format,
        VkImageUsageFlags usage, VkImageAspectFlags aspectFlags,
        AHardwareBuffer* ahb)
        : extent(extent), format(format), aspectFlags(aspectFlags), externalShared(true) {
    if (ahb == nullptr)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "AHB is null");

    // The caller owns the original reference. Keep an independent reference
    // so this imported image remains valid even if the caller's Mini::Image
    // is destroyed first.
    AHardwareBuffer_acquire(ahb);
    AhbHandleGuard ahbGuard{ahb};
    this->ahbRef = std::shared_ptr<AHardwareBuffer>(
        ahb,
        [](AHardwareBuffer* buffer) {
            if (buffer != nullptr)
                AHardwareBuffer_release(buffer);
        });
    ahbGuard.release();

    VkAndroidHardwareBufferFormatPropertiesANDROID fmtProps{
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID,
    };
    VkAndroidHardwareBufferPropertiesANDROID ahbProps{
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
        .pNext = &fmtProps,
    };
    auto res = vkGetAndroidHardwareBufferPropertiesANDROID(device.handle(), ahb, &ahbProps);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "vkGetAndroidHardwareBufferPropertiesANDROID failed");

    // External-format AHBs (VK_FORMAT_UNDEFINED) are only legal as sampled
    // images with a matching Y'CbCr conversion. LSFG reads and writes these
    // images as storage images, so such a buffer is explicitly incompatible.
    if (fmtProps.format == VK_FORMAT_UNDEFINED)
        throw LSFG::vulkan_error(VK_ERROR_FORMAT_NOT_SUPPORTED,
            "AHardwareBuffer is external-format-only; LSFG requires storage-image writes");
    if (fmtProps.format != format)
        throw LSFG::vulkan_error(VK_ERROR_FORMAT_NOT_SUPPORTED,
            "AHardwareBuffer VkFormat does not match the LSFG image format");

    const VkExternalMemoryImageCreateInfo externalInfo{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
    };
    const VkImageCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &externalInfo,
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
    res = vkCreateImage(device.handle(), &desc, nullptr, &imageHandle);
    VulkanImageOwners owners;
    owners.handles.device = device.handle();
    owners.handles.image = imageHandle;
    if (res != VK_SUCCESS || imageHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create Vulkan image (AHB)");

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(device.getPhysicalDevice(), &memProps);
    std::optional<uint32_t> memType{};
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if (ahbProps.memoryTypeBits & (1u << i)) { memType.emplace(i); break; }
    }
    if (!memType.has_value()) {
        throw LSFG::vulkan_error(VK_ERROR_UNKNOWN, "No memory type for AHB import");
    }

    const VkMemoryDedicatedAllocateInfo dedicated{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = imageHandle,
    };
    const VkImportAndroidHardwareBufferInfoANDROID importInfo{
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        .pNext = &dedicated,
        .buffer = ahb,
    };
    const VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &importInfo,
        .allocationSize = ahbProps.allocationSize,
        .memoryTypeIndex = memType.value(),
    };
    VkDeviceMemory memoryHandle{};
    res = vkAllocateMemory(device.handle(), &allocInfo, nullptr, &memoryHandle);
    owners.handles.memory = memoryHandle;
    if (res != VK_SUCCESS || memoryHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to import AHB into VkDeviceMemory");
    res = vkBindImageMemory(device.handle(), imageHandle, memoryHandle, 0);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to bind AHB memory");

    const VkImageViewCreateInfo viewDescAhb{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = imageHandle,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = { .aspectMask = aspectFlags, .levelCount = 1, .layerCount = 1 },
    };
    VkImageView viewHandleAhb{};
    res = vkCreateImageView(device.handle(), &viewDescAhb, nullptr, &viewHandleAhb);
    owners.handles.view = viewHandleAhb;
    if (res != VK_SUCCESS || viewHandleAhb == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create AHB image view");

    // Construct all owners off-object first. If any allocation throws, the
    // bundle releases view, image, and memory in Vulkan dependency order.
    owners.layout = std::make_shared<VkImageLayout>(VK_IMAGE_LAYOUT_GENERAL);
    owners.image = std::shared_ptr<VkImage>(new VkImage(imageHandle),
        [dev = device.handle()](VkImage* img) {
            if (img != nullptr) {
                vkDestroyImage(dev, *img, nullptr);
                delete img;
            }
        });
    owners.handles.image = VK_NULL_HANDLE;
    owners.memory = std::shared_ptr<VkDeviceMemory>(new VkDeviceMemory(memoryHandle),
        [dev = device.handle()](VkDeviceMemory* mem) {
            if (mem != nullptr) {
                vkFreeMemory(dev, *mem, nullptr);
                delete mem;
            }
        });
    owners.handles.memory = VK_NULL_HANDLE;
    owners.view = std::shared_ptr<VkImageView>(new VkImageView(viewHandleAhb),
        [dev = device.handle()](VkImageView* imgView) {
            if (imgView != nullptr) {
                vkDestroyImageView(dev, *imgView, nullptr);
                delete imgView;
            }
        });
    owners.handles.view = VK_NULL_HANDLE;

    this->layout = std::move(owners.layout);
    this->image = std::move(owners.image);
    this->memory = std::move(owners.memory);
    this->view = std::move(owners.view);
}

#endif // __ANDROID__

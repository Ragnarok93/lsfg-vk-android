#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/buffer.hpp"
#include "core/device.hpp"
#include "common/exception.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

using namespace LSFG::Core;

namespace {

struct BufferHandleGuard {
    VkDevice device{VK_NULL_HANDLE};
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};

    ~BufferHandleGuard() {
        // A bound buffer must be destroyed before its memory is freed.
        if (buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, buffer, nullptr);
        if (memory != VK_NULL_HANDLE)
            vkFreeMemory(device, memory, nullptr);
    }
};

} // namespace

void Buffer::construct(const Core::Device& device, const void* data, VkBufferUsageFlags usage) {
    // create buffer
    const VkBufferCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = this->size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkBuffer bufferHandle{};
    auto res = vkCreateBuffer(device.handle(), &desc, nullptr, &bufferHandle);
    if (res != VK_SUCCESS || bufferHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create Vulkan buffer");
    BufferHandleGuard handles{.device = device.handle(), .buffer = bufferHandle};

    // find memory type
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(device.getPhysicalDevice(), &memProps);

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device.handle(), bufferHandle, &memReqs);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
    std::optional<uint32_t> memType{};
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1 << i)) && // NOLINTBEGIN
            (memProps.memoryTypes[i].propertyFlags &
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
            memType.emplace(i);
            break;
        } // NOLINTEND
    }
    if (!memType.has_value())
        throw LSFG::vulkan_error(VK_ERROR_UNKNOWN, "Unable to find memory type for buffer");
#pragma clang diagnostic pop

    // allocate and bind memory
    const VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = memType.value()
    };
    VkDeviceMemory memoryHandle{};
    res = vkAllocateMemory(device.handle(), &allocInfo, nullptr, &memoryHandle);
    handles.memory = memoryHandle;
    if (res != VK_SUCCESS || memoryHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to allocate memory for Vulkan buffer");

    res = vkBindBufferMemory(device.handle(), bufferHandle, memoryHandle, 0);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to bind memory to Vulkan buffer");

    // upload data to buffer
    uint8_t* buf{};
    res = vkMapMemory(device.handle(), memoryHandle, 0, this->size, 0, reinterpret_cast<void**>(&buf));
    if (res != VK_SUCCESS || buf == nullptr)
        throw LSFG::vulkan_error(res, "Failed to map memory for Vulkan buffer");
    std::copy_n(reinterpret_cast<const uint8_t*>(data), this->size, buf);
    vkUnmapMemory(device.handle(), memoryHandle);

    // Store buffer and memory only after every operation has succeeded. The
    // stack guard owns partial Vulkan construction on all exceptional paths.
    auto bufferOwner = std::shared_ptr<VkBuffer>(
        new VkBuffer(bufferHandle),
        [dev = device.handle()](VkBuffer* img) {
            vkDestroyBuffer(dev, *img, nullptr);
            delete img;
        }
    );
    handles.buffer = VK_NULL_HANDLE;
    auto memoryOwner = std::shared_ptr<VkDeviceMemory>(
        new VkDeviceMemory(memoryHandle),
        [dev = device.handle()](VkDeviceMemory* mem) {
            vkFreeMemory(dev, *mem, nullptr);
            delete mem;
        }
    );
    handles.memory = VK_NULL_HANDLE;
    this->buffer = std::move(bufferOwner);
    this->memory = std::move(memoryOwner);
}

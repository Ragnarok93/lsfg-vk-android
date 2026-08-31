#include "mini/commandpool.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <memory>

using namespace Mini;

namespace {

struct CommandPoolOwner {
    VkDevice device{};
    VkCommandPool handle{};

    CommandPoolOwner(VkDevice device, VkCommandPool handle)
        : device(device), handle(handle) {}

    CommandPoolOwner(const CommandPoolOwner&) = delete;
    CommandPoolOwner& operator=(const CommandPoolOwner&) = delete;

    ~CommandPoolOwner() {
        if (handle != VK_NULL_HANDLE)
            Layer::ovkDestroyCommandPool(device, handle, nullptr);
    }
};

} // namespace

CommandPool::CommandPool(VkDevice device, uint32_t graphicsFamilyIdx) {
    // LSFG records short-lived copy command buffers every frame. Marking the
    // pool transient lets Android ICDs choose backing storage optimized for
    // frequent allocation/free rather than long-lived command buffers.
    const VkCommandPoolCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = graphicsFamilyIdx
    };
    VkCommandPool commandPoolHandle{};
    auto res = Layer::ovkCreateCommandPool(device, &desc, nullptr, &commandPoolHandle);
    if (res != VK_SUCCESS || commandPoolHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Unable to create command pool");

    // Keep the handle and its lifetime owner in one allocation. The old custom
    // shared_ptr deleter destroyed the Vulkan object but leaked the heap cell
    // containing VkCommandPool whenever a context was destroyed.
    auto owner = std::make_shared<CommandPoolOwner>(device, commandPoolHandle);
    this->commandPool = std::shared_ptr<VkCommandPool>(owner, &owner->handle);
}

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

CommandPool::CommandPool(VkDevice device, uint32_t graphicsFamilyIdx,
        bool enableIndividualReset) {
    // Keep the existing transient-only behavior unless a caller explicitly
    // opts into individual command-buffer reset. The Adreno wrapper path is
    // the only production caller that enables it.
    VkCommandPoolCreateFlags flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (enableIndividualReset)
        flags |= VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    const VkCommandPoolCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = flags,
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

#include "mini/commandbuffer.hpp"
#include "mini/commandpool.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace Mini;

namespace {

struct CommandBufferOwner {
    VkDevice device{};
    VkCommandPool pool{};
    VkCommandBuffer handle{};

    CommandBufferOwner(VkDevice device, VkCommandPool pool, VkCommandBuffer handle)
        : device(device), pool(pool), handle(handle) {}

    CommandBufferOwner(const CommandBufferOwner&) = delete;
    CommandBufferOwner& operator=(const CommandBufferOwner&) = delete;

    ~CommandBufferOwner() {
        if (handle != VK_NULL_HANDLE)
            Layer::ovkFreeCommandBuffers(device, pool, 1, &handle);
    }
};

} // namespace

CommandBuffer::CommandBuffer(VkDevice device, const CommandPool& pool) {
    const VkCommandBufferAllocateInfo desc{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool.handle(),
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VkCommandBuffer commandBufferHandle{};
    auto res = Layer::ovkAllocateCommandBuffers(device, &desc, &commandBufferHandle);
    if (res != VK_SUCCESS || commandBufferHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Unable to allocate command buffer");
    res = Layer::ovkSetDeviceLoaderData(device, commandBufferHandle);
    if (res != VK_SUCCESS) {
        Layer::ovkFreeCommandBuffers(device, pool.handle(), 1, &commandBufferHandle);
        throw LSFG::vulkan_error(res, "Unable to set device loader data for command buffer");
    }

    this->state = std::make_shared<CommandBufferState>(CommandBufferState::Empty);
    auto owner = std::make_shared<CommandBufferOwner>(
        device, pool.handle(), commandBufferHandle);
    this->commandBuffer = std::shared_ptr<VkCommandBuffer>(owner, &owner->handle);
}

void CommandBuffer::begin() {
    if (*this->state != CommandBufferState::Empty)
        throw std::logic_error("Command buffer is not in Empty state");

    const VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    auto res = Layer::ovkBeginCommandBuffer(*this->commandBuffer, &beginInfo);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Unable to begin command buffer");

    *this->state = CommandBufferState::Recording;
}

void CommandBuffer::end() {
    if (*this->state != CommandBufferState::Recording)
        throw std::logic_error("Command buffer is not in Recording state");

    auto res = Layer::ovkEndCommandBuffer(*this->commandBuffer);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Unable to end command buffer");

    *this->state = CommandBufferState::Full;
}

void CommandBuffer::submit(VkQueue queue,
        const std::vector<VkSemaphore>& waitSemaphores,
        const std::vector<VkSemaphore>& signalSemaphores,
        VkFence fence) {
    if (*this->state != CommandBufferState::Full)
        throw std::logic_error("Command buffer is not in Full state");

    // LSFG's game-device submits wait on at most a couple of semaphores in the
    // common path. Keep those stage masks on the stack instead of allocating a
    // vector on every copy submission; retain a fallback for generic callers.
    std::array<VkPipelineStageFlags, 4> inlineWaitStages{};
    std::vector<VkPipelineStageFlags> overflowWaitStages;
    const VkPipelineStageFlags* waitStages = nullptr;
    if (!waitSemaphores.empty()) {
        if (waitSemaphores.size() <= inlineWaitStages.size()) {
            std::fill_n(inlineWaitStages.begin(), waitSemaphores.size(),
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
            waitStages = inlineWaitStages.data();
        } else {
            overflowWaitStages.assign(waitSemaphores.size(),
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
            waitStages = overflowWaitStages.data();
        }
    }

    const VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size()),
        .pWaitSemaphores = waitSemaphores.data(),
        .pWaitDstStageMask = waitStages,
        .commandBufferCount = 1,
        .pCommandBuffers = &(*this->commandBuffer),
        .signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size()),
        .pSignalSemaphores = signalSemaphores.data()
    };
    auto res = Layer::ovkQueueSubmit(queue, 1, &submitInfo, fence);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Unable to submit command buffer");

    *this->state = CommandBufferState::Submitted;
}

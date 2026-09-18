#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "common/utils.hpp"
#include "core/buffer.hpp"
#include "core/image.hpp"
#include "core/device.hpp"
#include "core/commandpool.hpp"
#include "core/fence.hpp"
#include "common/exception.hpp"

#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <string>
#include <ios>
#include <stdexcept>
#include <system_error>
#include <vector>

using namespace LSFG;
using namespace LSFG::Utils;

namespace {

VkPipelineStageFlags sync2_to_sync1_stages(VkPipelineStageFlags2 mask) {
    VkPipelineStageFlags out = static_cast<VkPipelineStageFlags>(mask & 0xFFFFFFFFULL);
    constexpr VkPipelineStageFlags2 SYNC2_NEW_BITS =
        VK_PIPELINE_STAGE_2_COPY_BIT |
        VK_PIPELINE_STAGE_2_RESOLVE_BIT |
        VK_PIPELINE_STAGE_2_BLIT_BIT |
        VK_PIPELINE_STAGE_2_CLEAR_BIT |
        VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
        VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT |
        VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT;
    if (mask & SYNC2_NEW_BITS) out |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (out == 0 && mask != 0) out = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    return out;
}

VkAccessFlags sync2_to_sync1_access(VkAccessFlags2 mask) {
    VkAccessFlags out = static_cast<VkAccessFlags>(mask & 0xFFFFFFFFULL);
    constexpr VkAccessFlags2 SYNC2_NEW_BITS =
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    if (mask & SYNC2_NEW_BITS) out |= VK_ACCESS_SHADER_READ_BIT;
    if (mask & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT) out |= VK_ACCESS_SHADER_WRITE_BIT;
    return out;
}

} // namespace

void Utils::cmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* dep) {
    if (dep == nullptr) return;
    if (vkCmdPipelineBarrier2 != nullptr) {
        vkCmdPipelineBarrier2(cb, dep);
        return;
    }

    VkPipelineStageFlags srcStage = 0;
    VkPipelineStageFlags dstStage = 0;
    std::vector<VkMemoryBarrier> memBarriers;
    memBarriers.reserve(dep->memoryBarrierCount);
    for (uint32_t i = 0; i < dep->memoryBarrierCount; ++i) {
        const auto& m2 = dep->pMemoryBarriers[i];
        srcStage |= sync2_to_sync1_stages(m2.srcStageMask);
        dstStage |= sync2_to_sync1_stages(m2.dstStageMask);
        memBarriers.push_back(VkMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = sync2_to_sync1_access(m2.srcAccessMask),
            .dstAccessMask = sync2_to_sync1_access(m2.dstAccessMask),
        });
    }

    std::vector<VkBufferMemoryBarrier> bufBarriers;
    bufBarriers.reserve(dep->bufferMemoryBarrierCount);
    for (uint32_t i = 0; i < dep->bufferMemoryBarrierCount; ++i) {
        const auto& b2 = dep->pBufferMemoryBarriers[i];
        srcStage |= sync2_to_sync1_stages(b2.srcStageMask);
        dstStage |= sync2_to_sync1_stages(b2.dstStageMask);
        bufBarriers.push_back(VkBufferMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = sync2_to_sync1_access(b2.srcAccessMask),
            .dstAccessMask = sync2_to_sync1_access(b2.dstAccessMask),
            .srcQueueFamilyIndex = b2.srcQueueFamilyIndex,
            .dstQueueFamilyIndex = b2.dstQueueFamilyIndex,
            .buffer = b2.buffer,
            .offset = b2.offset,
            .size = b2.size,
        });
    }

    std::vector<VkImageMemoryBarrier> imgBarriers;
    imgBarriers.reserve(dep->imageMemoryBarrierCount);
    for (uint32_t i = 0; i < dep->imageMemoryBarrierCount; ++i) {
        const auto& i2 = dep->pImageMemoryBarriers[i];
        srcStage |= sync2_to_sync1_stages(i2.srcStageMask);
        dstStage |= sync2_to_sync1_stages(i2.dstStageMask);
        imgBarriers.push_back(VkImageMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = sync2_to_sync1_access(i2.srcAccessMask),
            .dstAccessMask = sync2_to_sync1_access(i2.dstAccessMask),
            .oldLayout = i2.oldLayout,
            .newLayout = i2.newLayout,
            .srcQueueFamilyIndex = i2.srcQueueFamilyIndex,
            .dstQueueFamilyIndex = i2.dstQueueFamilyIndex,
            .image = i2.image,
            .subresourceRange = i2.subresourceRange,
        });
    }

    if (srcStage == 0) srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (dstStage == 0) dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    vkCmdPipelineBarrier(cb,
        srcStage, dstStage, 0,
        static_cast<uint32_t>(memBarriers.size()), memBarriers.data(),
        static_cast<uint32_t>(bufBarriers.size()), bufBarriers.data(),
        static_cast<uint32_t>(imgBarriers.size()), imgBarriers.data());
}

BarrierBuilder& BarrierBuilder::addR2W(Core::Image& image) {
    if (this->barrierCount >= kInlineBarrierCapacity)
        throw std::logic_error("BarrierBuilder capacity exceeded");
    this->barriers[this->barrierCount++] = VkImageMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        // Read-after-use -> overwrite is a WAR hazard: preserve compute
        // execution ordering, but do not request source memory availability or
        // cache visibility for data that will not be consumed again.
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .oldLayout = image.getLayout(),
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = image.getAspectFlags(),
            .levelCount = 1,
            .layerCount = 1
        }
    };
    image.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    return *this;
}

BarrierBuilder& BarrierBuilder::addW2R(Core::Image& image) {
    if (this->barrierCount >= kInlineBarrierCapacity)
        throw std::logic_error("BarrierBuilder capacity exceeded");
    this->barriers[this->barrierCount++] = VkImageMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
        .oldLayout = image.getLayout(),
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = image.getAspectFlags(),
            .levelCount = 1,
            .layerCount = 1
        }
    };
    image.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    return *this;
}

void BarrierBuilder::build() const {
    const VkDependencyInfo dependencyInfo{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = static_cast<uint32_t>(this->barrierCount),
        .pImageMemoryBarriers = this->barriers.data()
    };
    Utils::cmdPipelineBarrier2(this->commandBuffer->handle(), &dependencyInfo);
}

void Utils::uploadImage(const Core::Device& device, const Core::CommandPool& commandPool,
        Core::Image& image, const std::string& path) {
    std::ifstream file(path.data(), std::ios::binary | std::ios::ate);
    if (!file.is_open())
        throw std::system_error(errno, std::generic_category(), "Failed to open image: " + path);
    std::streamsize size = file.tellg();
    size -= 124 + 4;
    std::vector<char> code(static_cast<size_t>(size));
    file.seekg(124 + 4, std::ios::beg);
    if (!file.read(code.data(), size))
        throw std::system_error(errno, std::generic_category(), "Failed to read image: " + path);
    file.close();

    const Core::Buffer stagingBuffer(
        device, code.data(), static_cast<uint32_t>(code.size()), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    Core::CommandBuffer commandBuffer(device, commandPool);
    commandBuffer.begin();
    const VkImageMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_NONE,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = image.getLayout(),
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = image.getAspectFlags(), .levelCount = 1, .layerCount = 1
        }
    };
    image.setLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdPipelineBarrier(commandBuffer.handle(),
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    auto extent = image.getExtent();
    const VkBufferImageCopy region{
        .bufferImageHeight = 0,
        .imageSubresource = {.aspectMask = image.getAspectFlags(), .layerCount = 1},
        .imageExtent = {extent.width, extent.height, 1}
    };
    vkCmdCopyBufferToImage(commandBuffer.handle(), stagingBuffer.handle(), image.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    commandBuffer.end();
    Core::Fence fence(device);
    commandBuffer.submit(device.getComputeQueue(), fence);
    if (!fence.wait(device))
        throw LSFG::vulkan_error(VK_TIMEOUT, "Upload operation timed out");
}

void Utils::clearImage(const Core::Device& device, Core::Image& image, bool white) {
    Core::Fence fence(device);
    const Core::CommandPool cmdPool(device);
    Core::CommandBuffer cmdBuf(device, cmdPool);
    cmdBuf.begin();
    const VkImageMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = image.getLayout(),
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = image.getAspectFlags(), .levelCount = 1, .layerCount = 1
        }
    };
    const VkDependencyInfo dependencyInfo{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier
    };
    image.setLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    Utils::cmdPipelineBarrier2(cmdBuf.handle(), &dependencyInfo);
    const float clearValue = white ? 1.0F : 0.0F;
    const VkClearColorValue clearColor = {{clearValue, clearValue, clearValue, clearValue}};
    const VkImageSubresourceRange subresourceRange{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1
    };
    vkCmdClearColorImage(cmdBuf.handle(), image.handle(), image.getLayout(),
        &clearColor, 1, &subresourceRange);
    cmdBuf.end();
    cmdBuf.submit(device.getComputeQueue(), fence);
    if (!fence.wait(device))
        throw LSFG::vulkan_error(VK_TIMEOUT, "Failed to wait for clearing fence.");
}

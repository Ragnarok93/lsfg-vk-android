#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "v3_1/context.hpp"
#include "common/utils.hpp"
#include "common/exception.hpp"

#include <vector>
#include <cstddef>
#include <algorithm>
#include <optional>
#include <cstdint>

using namespace LSFG_3_1;

#ifdef __ANDROID__
namespace {

void add_external_acquire(std::vector<VkImageMemoryBarrier2>& barriers,
        Vulkan& vk, Core::Image& image, VkAccessFlags2 accessMask) {
    if (!image.isExternalShared())
        return;

    barriers.emplace_back(VkImageMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = accessMask,
        .oldLayout = image.getLayout(),
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .dstQueueFamilyIndex = vk.device.getComputeFamilyIdx(),
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = image.getAspectFlags(),
            .levelCount = 1,
            .layerCount = 1,
        },
    });
}

void add_external_release(std::vector<VkImageMemoryBarrier2>& barriers,
        Vulkan& vk, Core::Image& image, VkAccessFlags2 accessMask) {
    if (!image.isExternalShared())
        return;

    barriers.emplace_back(VkImageMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = accessMask,
        .dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
        .dstAccessMask = 0,
        .oldLayout = image.getLayout(),
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = vk.device.getComputeFamilyIdx(),
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = image.getAspectFlags(),
            .levelCount = 1,
            .layerCount = 1,
        },
    });
}

void emit_external_barriers(const Core::CommandBuffer& buf,
        const std::vector<VkImageMemoryBarrier2>& barriers) {
    if (barriers.empty())
        return;

    const VkDependencyInfo dependencyInfo{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
        .pImageMemoryBarriers = barriers.data(),
    };
    LSFG::Utils::cmdPipelineBarrier2(buf.handle(), &dependencyInfo);
}


void add_external_transfer_acquire(std::vector<VkImageMemoryBarrier2>& barriers,
        Vulkan& vk, Core::Image& image, VkImageLayout newLayout,
        VkAccessFlags2 dstAccessMask) {
    if (!image.isExternalShared()) return;
    barriers.emplace_back(VkImageMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = dstAccessMask,
        .oldLayout = image.getLayout(),
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .dstQueueFamilyIndex = vk.device.getComputeFamilyIdx(),
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = image.getAspectFlags(), .levelCount = 1, .layerCount = 1,
        },
    });
    image.setLayout(newLayout);
}

void add_external_transfer_release(std::vector<VkImageMemoryBarrier2>& barriers,
        Vulkan& vk, Core::Image& image, VkImageLayout oldLayout,
        VkAccessFlags2 srcAccessMask) {
    if (!image.isExternalShared()) return;
    barriers.emplace_back(VkImageMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = srcAccessMask,
        .dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
        .dstAccessMask = 0,
        .oldLayout = oldLayout,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = vk.device.getComputeFamilyIdx(),
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = image.getAspectFlags(), .levelCount = 1, .layerCount = 1,
        },
    });
    image.setLayout(VK_IMAGE_LAYOUT_GENERAL);
}

void add_local_transition(std::vector<VkImageMemoryBarrier2>& barriers,
        Core::Image& image, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
        VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
        VkImageLayout newLayout) {
    barriers.emplace_back(VkImageMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = srcStage,
        .srcAccessMask = srcAccess,
        .dstStageMask = dstStage,
        .dstAccessMask = dstAccess,
        .oldLayout = image.getLayout(),
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = image.getAspectFlags(), .levelCount = 1, .layerCount = 1,
        },
    });
    image.setLayout(newLayout);
}

void copy_same_format(const Core::CommandBuffer& buf,
        Core::Image& src, Core::Image& dst) {
    const VkExtent2D extent = src.getExtent();
    const VkImageCopy region{
        .srcSubresource = { .aspectMask = src.getAspectFlags(), .layerCount = 1 },
        .dstSubresource = { .aspectMask = dst.getAspectFlags(), .layerCount = 1 },
        .extent = { extent.width, extent.height, 1 },
    };
    vkCmdCopyImage(buf.handle(), src.handle(), src.getLayout(),
        dst.handle(), dst.getLayout(), 1, &region);
}

} // namespace
#endif

Context::Context(Vulkan& vk,
        int in0, int in1, const std::vector<int>& outN,
        VkExtent2D extent, VkFormat format) {
    // import input images
    this->inImg_0 = Core::Image(vk.device, extent, format,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, in0);
    this->inImg_1 = Core::Image(vk.device, extent, format,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, in1);

    // prepare render data
    for (size_t i = 0; i < 8; i++) {
        auto& data = this->data.at(i);
        data.internalSemaphores.resize(vk.generationCount);
        data.outSemaphores.resize(vk.generationCount);
        data.completionFences.resize(vk.generationCount);
        data.cmdBuffers2.resize(vk.generationCount);
    }

    // create shader chains
    this->mipmaps = Shaders::Mipmaps(vk, this->inImg_0, this->inImg_1);
    for (size_t i = 0; i < 7; i++)
        this->alpha.at(i) = Shaders::Alpha(vk, this->mipmaps.getOutImages().at(i));
    this->beta = Shaders::Beta(vk, this->alpha.at(0).getOutImages());
    for (size_t i = 0; i < 7; i++) {
        this->gamma.at(i) = Shaders::Gamma(vk,
            this->alpha.at(6 - i).getOutImages(),
            this->beta.getOutImages().at(std::min<size_t>(6 - i, 5)),
            (i == 0) ? std::nullopt : std::make_optional(this->gamma.at(i - 1).getOutImage()));
        if (i < 4) continue;

        this->delta.at(i - 4) = Shaders::Delta(vk,
            this->alpha.at(6 - i).getOutImages(),
            this->beta.getOutImages().at(6 - i),
            (i == 4) ? std::nullopt : std::make_optional(this->gamma.at(i - 1).getOutImage()),
            (i == 4) ? std::nullopt : std::make_optional(this->delta.at(i - 5).getOutImage1()),
            (i == 4) ? std::nullopt : std::make_optional(this->delta.at(i - 5).getOutImage2()));
    }
    this->generate = Shaders::Generate(vk,
        this->inImg_0, this->inImg_1,
        this->gamma.at(6).getOutImage(),
        this->delta.at(2).getOutImage1(),
        this->delta.at(2).getOutImage2(),
        outN, format);
}

void Context::present(Vulkan& vk,
        int inSem, const std::vector<int>& outSem) {
    auto& data = this->data.at(this->frameIdx % 8);

    // 3. wait for completion of previous frame in this slot
    if (data.shouldWait)
        for (auto& fence : data.completionFences)
            if (!fence.wait(vk.device, UINT64_MAX))
                throw LSFG::vulkan_error(VK_TIMEOUT, "Fence wait timed out");
    data.shouldWait = true;

    // 1. create mipmaps and process input image
    if (inSem >= 0) data.inSemaphore = Core::Semaphore(vk.device, inSem);
    for (size_t i = 0; i < vk.generationCount; i++)
        data.internalSemaphores.at(i) = Core::Semaphore(vk.device);

    data.cmdBuffer1 = Core::CommandBuffer(vk.device, vk.commandPool);
    data.cmdBuffer1.begin();

#ifdef __ANDROID__
    if (this->transportOnly) {
        std::vector<VkImageMemoryBarrier2> barriers;
        barriers.reserve(4);
        add_external_transfer_acquire(barriers, vk, this->sharedInImg_0,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);
        add_external_transfer_acquire(barriers, vk, this->sharedInImg_1,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);
        add_local_transition(barriers, this->inImg_0, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        add_local_transition(barriers, this->inImg_1, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        emit_external_barriers(data.cmdBuffer1, barriers);
        copy_same_format(data.cmdBuffer1, this->sharedInImg_0, this->inImg_0);
        copy_same_format(data.cmdBuffer1, this->sharedInImg_1, this->inImg_1);
        barriers.clear();
        add_local_transition(barriers, this->inImg_0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);
        add_local_transition(barriers, this->inImg_1, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);
        add_external_transfer_release(barriers, vk, this->sharedInImg_0,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);
        add_external_transfer_release(barriers, vk, this->sharedInImg_1,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);
        emit_external_barriers(data.cmdBuffer1, barriers);
    } else {
        std::vector<VkImageMemoryBarrier2> acquireBarriers;
        acquireBarriers.reserve(2);
        add_external_acquire(acquireBarriers, vk, this->inImg_0, VK_ACCESS_2_SHADER_READ_BIT);
        add_external_acquire(acquireBarriers, vk, this->inImg_1, VK_ACCESS_2_SHADER_READ_BIT);
        emit_external_barriers(data.cmdBuffer1, acquireBarriers);
    }
#endif

    this->mipmaps.Dispatch(data.cmdBuffer1, this->frameIdx);
    for (size_t i = 0; i < 7; i++)
        this->alpha.at(6 - i).Dispatch(data.cmdBuffer1, this->frameIdx);
    this->beta.Dispatch(data.cmdBuffer1, this->frameIdx);

    data.cmdBuffer1.end();
    std::vector<Core::Semaphore> waits = { data.inSemaphore };
    if (inSem < 0) waits.clear();
    data.cmdBuffer1.submit(vk.device.getComputeQueue(), std::nullopt,
        waits, std::nullopt,
        data.internalSemaphores, std::nullopt);

    // 2. generate intermediary frames
    for (size_t pass = 0; pass < vk.generationCount; pass++) {
        auto& internalSemaphore = data.internalSemaphores.at(pass);
        auto& outSemaphore = data.outSemaphores.at(pass);
        if (inSem >= 0) outSemaphore = Core::Semaphore(vk.device, outSem.empty() ? -1 : outSem.at(pass));
        auto& completionFence = data.completionFences.at(pass);
        completionFence = Core::Fence(vk.device);

        auto& buf2 = data.cmdBuffers2.at(pass);
        buf2 = Core::CommandBuffer(vk.device, vk.commandPool);
        buf2.begin();

#ifdef __ANDROID__
        if (!this->transportOnly) {
            std::vector<VkImageMemoryBarrier2> acquireBarriers;
            acquireBarriers.reserve(1);
            add_external_acquire(acquireBarriers, vk, this->generate.getOutImages().at(pass),
                VK_ACCESS_2_SHADER_WRITE_BIT);
            emit_external_barriers(buf2, acquireBarriers);
        }
#endif

        for (size_t i = 0; i < 7; i++) {
            this->gamma.at(i).Dispatch(buf2, this->frameIdx, pass);
            if (i >= 4)
                this->delta.at(i - 4).Dispatch(buf2, this->frameIdx, pass);
        }
        this->generate.Dispatch(buf2, this->frameIdx, pass);

#ifdef __ANDROID__
        if (this->transportOnly) {
            auto& localOut = this->generate.getOutImages().at(pass);
            auto& sharedOut = this->sharedOutImages.at(pass);
            std::vector<VkImageMemoryBarrier2> barriers;
            barriers.reserve(2);
            add_local_transition(barriers, localOut, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            add_external_transfer_acquire(barriers, vk, sharedOut,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            emit_external_barriers(buf2, barriers);
            copy_same_format(buf2, localOut, sharedOut);
            barriers.clear();
            add_local_transition(barriers, localOut, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);
            add_external_transfer_release(barriers, vk, sharedOut,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            emit_external_barriers(buf2, barriers);
        } else {
            std::vector<VkImageMemoryBarrier2> releaseBarriers;
            releaseBarriers.reserve(pass + 1 == vk.generationCount ? 3 : 1);
            add_external_release(releaseBarriers, vk, this->generate.getOutImages().at(pass),
                VK_ACCESS_2_SHADER_WRITE_BIT);
            if (pass + 1 == vk.generationCount) {
                add_external_release(releaseBarriers, vk, this->inImg_0, VK_ACCESS_2_SHADER_READ_BIT);
                add_external_release(releaseBarriers, vk, this->inImg_1, VK_ACCESS_2_SHADER_READ_BIT);
            }
            emit_external_barriers(buf2, releaseBarriers);
        }
#endif

        buf2.end();
        std::vector<Core::Semaphore> signals = { outSemaphore };
        if (inSem < 0) signals.clear();
        buf2.submit(vk.device.getComputeQueue(), completionFence,
            { internalSemaphore }, std::nullopt,
            signals, std::nullopt);
    }

    this->frameIdx++;
}

#ifdef __ANDROID__

#include <android/hardware_buffer.h>

Context::Context(Vulkan& vk,
        AHardwareBuffer* in0, AHardwareBuffer* in1,
        const std::vector<AHardwareBuffer*>& outN,
        VkExtent2D extent, VkFormat format) {
    this->transportOnly = vk.device.getAhbTransportMode() == LSFG::AhbTransportMode::TransportOnly;
    if (vk.device.getAhbTransportMode() == LSFG::AhbTransportMode::Unsupported)
        throw LSFG::vulkan_error(VK_ERROR_FORMAT_NOT_SUPPORTED,
            "No compatible AHardwareBuffer transport path for framegen format");

    std::vector<Core::Image> outImgs;
    outImgs.reserve(outN.size());
    if (this->transportOnly) {
        this->sharedInImg_0 = Core::Image(vk.device, extent, format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, in0);
        this->sharedInImg_1 = Core::Image(vk.device, extent, format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, in1);
        this->sharedOutImages.reserve(outN.size());
        for (auto* ahb : outN)
            this->sharedOutImages.emplace_back(vk.device, extent, format,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, ahb);

        this->inImg_0 = Core::Image(vk.device, extent, format,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        this->inImg_1 = Core::Image(vk.device, extent, format,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        for (size_t i = 0; i < outN.size(); ++i)
            outImgs.emplace_back(vk.device, extent, format,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    } else {
        // Fast path: exactly the established direct shader-storage AHB binding.
        this->inImg_0 = Core::Image(vk.device, extent, format,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, in0);
        this->inImg_1 = Core::Image(vk.device, extent, format,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, in1);
        for (auto* ahb : outN)
            outImgs.emplace_back(vk.device, extent, format,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, ahb);
    }

    for (size_t i = 0; i < 8; i++) {
        auto& data = this->data.at(i);
        data.internalSemaphores.resize(vk.generationCount);
        data.outSemaphores.resize(vk.generationCount);
        data.completionFences.resize(vk.generationCount);
        data.cmdBuffers2.resize(vk.generationCount);
    }

    this->mipmaps = Shaders::Mipmaps(vk, this->inImg_0, this->inImg_1);
    for (size_t i = 0; i < 7; i++)
        this->alpha.at(i) = Shaders::Alpha(vk, this->mipmaps.getOutImages().at(i));
    this->beta = Shaders::Beta(vk, this->alpha.at(0).getOutImages());
    for (size_t i = 0; i < 7; i++) {
        this->gamma.at(i) = Shaders::Gamma(vk,
            this->alpha.at(6 - i).getOutImages(),
            this->beta.getOutImages().at(std::min<size_t>(6 - i, 5)),
            (i == 0) ? std::nullopt : std::make_optional(this->gamma.at(i - 1).getOutImage()));
        if (i < 4) continue;
        this->delta.at(i - 4) = Shaders::Delta(vk,
            this->alpha.at(6 - i).getOutImages(),
            this->beta.getOutImages().at(6 - i),
            (i == 4) ? std::nullopt : std::make_optional(this->gamma.at(i - 1).getOutImage()),
            (i == 4) ? std::nullopt : std::make_optional(this->delta.at(i - 5).getOutImage1()),
            (i == 4) ? std::nullopt : std::make_optional(this->delta.at(i - 5).getOutImage2()));
    }
    this->generate = Shaders::Generate(vk,
        this->inImg_0, this->inImg_1,
        this->gamma.at(6).getOutImage(),
        this->delta.at(2).getOutImage1(),
        this->delta.at(2).getOutImage2(),
        std::move(outImgs));
}

#endif // __ANDROID__

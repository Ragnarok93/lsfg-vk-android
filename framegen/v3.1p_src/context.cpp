#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "v3_1p/context.hpp"
#include "common/utils.hpp"
#include "common/exception.hpp"

#include <vector>
#include <chrono>
#include <cstddef>
#include <algorithm>
#include <optional>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <utility>

using namespace LSFG;
using namespace LSFG_3_1P;

namespace {
uint64_t framegenWaitTimeoutNs() {
    constexpr uint64_t defaultMs = 250;
    constexpr uint64_t maxMs = 5000;
    const char* raw = std::getenv("LSFG_VK_WAIT_TIMEOUT_MS");
    if (raw == nullptr || *raw == '\0')
        return defaultMs * 1000000ULL;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (end == raw || *end != '\0' || parsed == 0)
        return defaultMs * 1000000ULL;
    const uint64_t boundedMs = parsed > maxMs ? maxMs : static_cast<uint64_t>(parsed);
    return boundedMs * 1000000ULL;
}
}

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

class ScopedAdaptiveFlowConstruction {
public:
    ScopedAdaptiveFlowConstruction(
            Vulkan& vk, float userFlowScale,
            const Core::DescriptorPool& descriptorPool)
        : vk_(vk), savedFlowScale_(vk.flowScale),
          userFlowScale_(validateUserFlowScale(userFlowScale)),
          savedDescriptorPool_(vk.descriptorPool),
          savedResources_(std::move(vk.resources)) {
        vk_.flowScale = 1.0F / userFlowScale_;
        vk_.descriptorPool = descriptorPool;
        vk_.resources = Pool::ResourcePool(vk_.isHdr, vk_.flowScale);
    }

    ScopedAdaptiveFlowConstruction(const ScopedAdaptiveFlowConstruction&) = delete;
    ScopedAdaptiveFlowConstruction& operator=(const ScopedAdaptiveFlowConstruction&) = delete;

    ~ScopedAdaptiveFlowConstruction() {
        vk_.resources = std::move(savedResources_);
        vk_.descriptorPool = savedDescriptorPool_;
        vk_.flowScale = savedFlowScale_;
    }

private:
    static float validateUserFlowScale(float userFlowScale) {
        if (!std::isfinite(userFlowScale) || userFlowScale < 0.25F
                || userFlowScale > 1.0F)
            throw std::invalid_argument(
                "Adaptive Flow Scale must be within 0.25..1.0");
        return userFlowScale;
    }

    Vulkan& vk_;
    float savedFlowScale_;
    float userFlowScale_;
    Core::DescriptorPool savedDescriptorPool_;
    Pool::ResourcePool savedResources_;
};

} // namespace
#endif

Context::Context(Vulkan& vk,
        int in0, int in1, const std::vector<int>& outN,
        VkExtent2D extent, VkFormat format) {
    this->inImg_0 = Core::Image(vk.device, extent, format,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, in0);
    this->inImg_1 = Core::Image(vk.device, extent, format,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, in1);

    for (size_t i = 0; i < 8; i++) {
        auto& data = this->data.at(i);
        data.preprocessingFence = Core::Fence(vk.device);
        data.internalSemaphores.resize(vk.generationCount);
        for (auto& internalSemaphore : data.internalSemaphores)
            internalSemaphore = Core::Semaphore(vk.device);
        data.outSemaphores.resize(vk.generationCount);
        data.completionFences.resize(vk.generationCount);
        for (auto& completionFence : data.completionFences)
            completionFence = Core::Fence(vk.device);
        data.cmdBuffers2.resize(vk.generationCount);
#ifdef __ANDROID__
        data.adaptiveFlowTimingQueryPool =
            Core::TimestampQueryPool(vk.device, 4);
#endif
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
        outN, format);
}

LSFG::AndroidFrameSyncFds Context::present(Vulkan& vk,
        int inSem, const std::vector<int>& outSem,
        size_t activeGenerationCount,
        VkExternalSemaphoreHandleTypeFlagBits inSemHandleType,
        bool exportAndroidSyncFdOutputs) {
    LSFG::AndroidFrameSyncFds exportedSync{};

#ifdef __ANDROID__
    const size_t completedWindow =
        std::min<size_t>(this->frameIdx, this->data.size());
    for (size_t age = completedWindow; age > 0; --age) {
        auto& completedData =
            this->data.at((this->frameIdx - age) % this->data.size());
        if (!completedData.shouldWait || completedData.generationCount == 0)
            continue;
        bool complete = true;
        for (size_t i = 0; i < completedData.generationCount; ++i) {
            if (!completedData.completionFences.at(i).isSignaled(vk.device)) {
                complete = false;
                break;
            }
        }
        if (complete) {
            this->recordAdaptiveFlowGpuTiming(vk, completedData);
            completedData.shouldWait = false;
        }
    }
#endif
    const size_t generationCount = std::min(activeGenerationCount, vk.generationCount);
    auto& data = this->data.at(this->frameIdx % 8);

    if (data.shouldWait) {
        for (size_t i = 0; i < data.generationCount; ++i)
            if (!data.completionFences.at(i).wait(vk.device, framegenWaitTimeoutNs()))
                throw LSFG::vulkan_error(VK_TIMEOUT, "Fence wait timed out");
#ifdef __ANDROID__
        this->recordAdaptiveFlowGpuTiming(vk, data);
#endif
        data.shouldWait = false;
    }
    data.shouldWait = generationCount > 0;
    data.generationCount = generationCount;
#ifdef __ANDROID__
    data.adaptiveFlowTransitionCycle =
        !this->adaptiveFlowScales_.empty() && this->pendingFlowGraphIndex_.has_value();
#endif

    const bool hasInputSemaphore =
        inSem >= 0
        || (inSem == -1
            && inSemHandleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
    if (hasInputSemaphore)
        data.inSemaphore = Core::Semaphore(vk.device, inSem, inSemHandleType);

    data.cmdBuffer1 = Core::CommandBuffer(vk.device, vk.commandPool);
    data.cmdBuffer1.begin();

#ifdef __ANDROID__
    if (this->inputCopyRequired) {
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

#ifdef __ANDROID__
    Core::TimestampQueryPool* adaptiveFlowTimingPool = nullptr;
    if (data.adaptiveFlowTimingQueryPool.supported()) {
        adaptiveFlowTimingPool = &data.adaptiveFlowTimingQueryPool;
        adaptiveFlowTimingPool->reset(data.cmdBuffer1.handle());
        adaptiveFlowTimingPool->write(data.cmdBuffer1.handle(), 0);
    }

    size_t generationGraphIndex = 0;
    bool adaptiveFlowShadowSubmitted = false;
    bool adaptiveFlowCommitAfterSubmit = false;
    if (!this->adaptiveFlowScales_.empty()) {
        generationGraphIndex = this->activeFlowGraphIndex_;
        const auto activeGraph = this->flowGraph(this->activeFlowGraphIndex_);
        if (this->pendingFlowGraphIndex_.has_value()) {
            const size_t pendingIndex = *this->pendingFlowGraphIndex_;
            const auto pendingGraph = this->flowGraph(pendingIndex);
            if (this->pendingFlowWarmupFrames_ + 1 < kAdaptiveFlowHistoryFrames) {
                this->dispatchAdaptiveFlowPreprocess(
                    data.cmdBuffer1, activeGraph, adaptiveFlowTimingPool);
                this->dispatchAdaptiveFlowPreprocess(
                    data.cmdBuffer1, pendingGraph);
                if (generationCount > 0)
                    activeGraph.beta->Dispatch(data.cmdBuffer1, this->frameIdx);
                adaptiveFlowShadowSubmitted = true;
            } else {
                // The first two shadow cycles already refreshed two distinct
                // temporal slots. Writing the current source into the pending
                // graph refreshes the third; generation can switch immediately
                // after this submission without a native/source-only gap.
                this->dispatchAdaptiveFlowPreprocess(
                    data.cmdBuffer1, pendingGraph, adaptiveFlowTimingPool);
                if (generationCount > 0)
                    pendingGraph.beta->Dispatch(data.cmdBuffer1, this->frameIdx);
                generationGraphIndex = pendingIndex;
                adaptiveFlowCommitAfterSubmit = true;
            }
        } else {
            this->dispatchAdaptiveFlowPreprocess(
                data.cmdBuffer1, activeGraph, adaptiveFlowTimingPool);
            if (generationCount > 0)
                activeGraph.beta->Dispatch(data.cmdBuffer1, this->frameIdx);
        }
    } else {
        this->mipmaps.Dispatch(data.cmdBuffer1, this->frameIdx);
        if (adaptiveFlowTimingPool != nullptr)
            adaptiveFlowTimingPool->write(data.cmdBuffer1.handle(), 1);
        for (size_t i = 0; i < 7; i++)
            this->alpha.at(6 - i).Dispatch(data.cmdBuffer1, this->frameIdx);
        if (generationCount > 0)
            this->beta.Dispatch(data.cmdBuffer1, this->frameIdx);
    }
    if (adaptiveFlowTimingPool != nullptr)
        adaptiveFlowTimingPool->write(data.cmdBuffer1.handle(), 2);
#else
    this->mipmaps.Dispatch(data.cmdBuffer1, this->frameIdx);
    for (size_t i = 0; i < 7; i++)
        this->alpha.at(6 - i).Dispatch(data.cmdBuffer1, this->frameIdx);
    if (generationCount > 0)
        this->beta.Dispatch(data.cmdBuffer1, this->frameIdx);
#endif

#ifdef __ANDROID__
    if (generationCount == 0 && !this->inputCopyRequired) {
        std::vector<VkImageMemoryBarrier2> releaseBarriers;
        releaseBarriers.reserve(2);
        add_external_release(releaseBarriers, vk, this->inImg_0,
            VK_ACCESS_2_SHADER_READ_BIT);
        add_external_release(releaseBarriers, vk, this->inImg_1,
            VK_ACCESS_2_SHADER_READ_BIT);
        emit_external_barriers(data.cmdBuffer1, releaseBarriers);
    }
#endif

#ifdef __ANDROID__
    if (adaptiveFlowTimingPool != nullptr && generationCount == 0)
        adaptiveFlowTimingPool->write(data.cmdBuffer1.handle(), 3);
#endif
    data.cmdBuffer1.end();
    std::vector<Core::Semaphore> waits = { data.inSemaphore };
    if (!hasInputSemaphore) waits.clear();

    if (generationCount == 0) {
        data.preprocessingFence.reset(vk.device);
        data.cmdBuffer1.submit(vk.device.getComputeQueue(), data.preprocessingFence,
            waits, std::nullopt, {}, std::nullopt);
        if (!data.preprocessingFence.wait(vk.device, framegenWaitTimeoutNs()))
            throw LSFG::vulkan_error(VK_TIMEOUT,
                "Temporal preprocessing fence wait timed out");
#ifdef __ANDROID__
        this->recordAdaptiveFlowGpuTiming(vk, data);
        if (adaptiveFlowShadowSubmitted)
            ++this->pendingFlowWarmupFrames_;
        if (adaptiveFlowCommitAfterSubmit)
            this->commitAdaptiveFlowTransition(generationGraphIndex);
#endif
        this->frameIdx++;
        return exportedSync;
    }

    const std::vector<Core::Semaphore> activeInternalSemaphores(
        data.internalSemaphores.begin(),
        data.internalSemaphores.begin() + static_cast<std::ptrdiff_t>(generationCount));
    data.cmdBuffer1.submit(vk.device.getComputeQueue(), std::nullopt,
        waits, std::nullopt,
        activeInternalSemaphores, std::nullopt);

#ifdef __ANDROID__
    if (adaptiveFlowShadowSubmitted)
        ++this->pendingFlowWarmupFrames_;
    if (adaptiveFlowCommitAfterSubmit)
        this->commitAdaptiveFlowTransition(generationGraphIndex);
    auto* presentGenerate = this->adaptiveFlowScales_.empty()
        ? &this->generate
        : this->flowGraph(generationGraphIndex).generate;
#else
    auto* presentGenerate = &this->generate;
#endif

    bool exportSyncFdOutputs =
#ifdef __ANDROID__
        exportAndroidSyncFdOutputs && generationCount > 0;
#else
        false;
#endif
    bool hostWaitFallback = false;
#ifdef __ANDROID__
    if (exportSyncFdOutputs) {
        try {
            for (size_t pass = 0; pass < generationCount; ++pass)
                data.outSemaphores.at(pass) = Core::Semaphore(
                    vk.device, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
            data.batchCompleteSemaphore = Core::Semaphore(
                vk.device, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
        } catch (const std::exception& e) {
            std::cerr << "lsfg-vk: framegen output SYNC_FD setup failed: "
                      << e.what() << "; using bounded host fallback\n";
            exportSyncFdOutputs = false;
            hostWaitFallback = true;
        }
    }
#endif

    for (size_t pass = 0; pass < generationCount; pass++) {
        auto& internalSemaphore = data.internalSemaphores.at(pass);
        auto& outSemaphore = data.outSemaphores.at(pass);
        const bool hasImportedOutSemaphore =
            pass < outSem.size() && outSem.at(pass) >= 0;
        const bool hasOutSemaphore =
            exportSyncFdOutputs || hasImportedOutSemaphore;
        if (!exportSyncFdOutputs && hasImportedOutSemaphore)
            outSemaphore = Core::Semaphore(vk.device, outSem.at(pass));
        auto& completionFence = data.completionFences.at(pass);
        completionFence.reset(vk.device);

        auto& buf2 = data.cmdBuffers2.at(pass);
        buf2 = Core::CommandBuffer(vk.device, vk.commandPool);
        buf2.begin();

#ifdef __ANDROID__
        if (!this->outputCopyRequired) {
            std::vector<VkImageMemoryBarrier2> acquireBarriers;
            acquireBarriers.reserve(1);
            add_external_acquire(acquireBarriers, vk, presentGenerate->getOutImages().at(pass),
                VK_ACCESS_2_SHADER_WRITE_BIT);
            emit_external_barriers(buf2, acquireBarriers);
        }
#endif

#ifdef __ANDROID__
        if (!this->adaptiveFlowScales_.empty()) {
            const auto generationGraph = this->flowGraph(generationGraphIndex);
            for (size_t i = 0; i < 7; i++) {
                generationGraph.gamma->at(i).Dispatch(
                    buf2, this->frameIdx, pass, generationCount);
                if (i >= 4)
                    generationGraph.delta->at(i - 4).Dispatch(
                        buf2, this->frameIdx, pass, generationCount);
            }
            generationGraph.generate->Dispatch(
                buf2, this->frameIdx, pass, generationCount);
        } else {
            for (size_t i = 0; i < 7; i++) {
                this->gamma.at(i).Dispatch(buf2, this->frameIdx, pass, generationCount);
                if (i >= 4)
                    this->delta.at(i - 4).Dispatch(buf2, this->frameIdx, pass, generationCount);
            }
            this->generate.Dispatch(buf2, this->frameIdx, pass, generationCount);
        }
#else
        for (size_t i = 0; i < 7; i++) {
            this->gamma.at(i).Dispatch(buf2, this->frameIdx, pass, generationCount);
            if (i >= 4)
                this->delta.at(i - 4).Dispatch(buf2, this->frameIdx, pass, generationCount);
        }
        this->generate.Dispatch(buf2, this->frameIdx, pass, generationCount);
#endif

#ifdef __ANDROID__
        if (adaptiveFlowTimingPool != nullptr && pass + 1 == generationCount)
            adaptiveFlowTimingPool->write(buf2.handle(), 3);

        if (this->outputCopyRequired) {
            auto& localOut = presentGenerate->getOutImages().at(pass);
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
            if (pass + 1 == generationCount && !this->inputCopyRequired) {
                add_external_release(
                    barriers, vk, this->inImg_0, VK_ACCESS_2_SHADER_READ_BIT);
                add_external_release(
                    barriers, vk, this->inImg_1, VK_ACCESS_2_SHADER_READ_BIT);
            }
            emit_external_barriers(buf2, barriers);
        } else {
            std::vector<VkImageMemoryBarrier2> releaseBarriers;
            releaseBarriers.reserve(pass + 1 == generationCount ? 3 : 1);
            add_external_release(releaseBarriers, vk, presentGenerate->getOutImages().at(pass),
                VK_ACCESS_2_SHADER_WRITE_BIT);
            if (pass + 1 == generationCount) {
                add_external_release(releaseBarriers, vk, this->inImg_0, VK_ACCESS_2_SHADER_READ_BIT);
                add_external_release(releaseBarriers, vk, this->inImg_1, VK_ACCESS_2_SHADER_READ_BIT);
            }
            emit_external_barriers(buf2, releaseBarriers);
        }
#endif

        buf2.end();
        std::vector<Core::Semaphore> signals;
        if (hasOutSemaphore)
            signals.emplace_back(outSemaphore);
#ifdef __ANDROID__
        if (exportSyncFdOutputs && pass + 1 == generationCount)
            signals.emplace_back(data.batchCompleteSemaphore);
#endif
        buf2.submit(vk.device.getComputeQueue(), completionFence,
            { internalSemaphore }, std::nullopt,
            signals, std::nullopt);
    }

#ifdef __ANDROID__
    if (exportSyncFdOutputs) {
        try {
            exportedSync.outputReadyFds.reserve(generationCount);
            for (size_t pass = 0; pass < generationCount; ++pass) {
                exportedSync.outputReadyFds.emplace_back(
                    data.outSemaphores.at(pass).exportFd(
                        vk.device, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT));
            }
            exportedSync.batchCompleteFd = data.batchCompleteSemaphore.exportFd(
                vk.device, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
            exportedSync.gpuDependenciesExported = true;
        } catch (const std::exception& e) {
            for (const int fd : exportedSync.outputReadyFds)
                if (fd >= 0) ::close(fd);
            exportedSync.outputReadyFds.clear();
            if (exportedSync.batchCompleteFd >= 0)
                ::close(exportedSync.batchCompleteFd);
            exportedSync.batchCompleteFd = -1;
            std::cerr << "lsfg-vk: framegen output SYNC_FD export failed: "
                      << e.what() << "; using bounded host fallback\n";
            hostWaitFallback = true;
        }
    }

    if (hostWaitFallback) {
        for (size_t pass = 0; pass < generationCount; ++pass) {
            if (!data.completionFences.at(pass).wait(vk.device, framegenWaitTimeoutNs()))
                throw LSFG::vulkan_error(
                    VK_TIMEOUT, "Framegen output fallback wait timed out");
        }
        this->recordAdaptiveFlowGpuTiming(vk, data);
        data.shouldWait = false;
        exportedSync.hostWaitFallback = true;
        exportedSync.gpuDependenciesExported = false;
    }
#endif

    this->frameIdx++;
    return exportedSync;
}

bool Context::waitForLastPresent(Vulkan& vk, uint64_t timeoutNs) {
    if (this->frameIdx == 0)
        return true;

    auto& renderData = this->data.at((this->frameIdx - 1) % this->data.size());
    if (!renderData.shouldWait)
        return true;

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::nanoseconds(timeoutNs);
    for (size_t i = 0; i < renderData.generationCount; ++i) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false;
        const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
            deadline - now).count();
        if (!renderData.completionFences.at(i).wait(
                vk.device, static_cast<uint64_t>(remaining)))
            return false;
    }
#ifdef __ANDROID__
    this->recordAdaptiveFlowGpuTiming(vk, renderData);
#endif
    renderData.shouldWait = false;
    return true;
}

bool Context::waitForCompletion(Vulkan& vk) {
    for (auto& renderData : this->data) {
        if (!renderData.shouldWait)
            continue;
        for (size_t i = 0; i < renderData.generationCount; ++i) {
            if (!renderData.completionFences.at(i).wait(
                    vk.device, framegenWaitTimeoutNs()))
                return false;
        }
        renderData.shouldWait = false;
    }
    return true;
}

#ifdef __ANDROID__

#include <android/hardware_buffer.h>
#include <unistd.h>

Context::Context(Vulkan& vk,
        AHardwareBuffer* in0, AHardwareBuffer* in1,
        const std::vector<AHardwareBuffer*>& outN,
        VkExtent2D extent, VkFormat format) {
    const auto ahbMode = vk.device.getAhbTransportMode();
    this->inputCopyRequired = LSFG::ahbInputCopyRequired(ahbMode);
    this->outputCopyRequired = LSFG::ahbOutputCopyRequired(ahbMode);
    if (vk.device.getAhbTransportMode() == LSFG::AhbTransportMode::Unsupported)
        throw LSFG::vulkan_error(VK_ERROR_FORMAT_NOT_SUPPORTED,
            "No compatible AHardwareBuffer transport path for framegen format");
    std::cerr << "lsfg-vk: ahb-directional-transport mode="
              << LSFG::ahbTransportModeName(ahbMode)
              << " input_copy=" << (this->inputCopyRequired ? 1 : 0)
              << " output_copy=" << (this->outputCopyRequired ? 1 : 0)
              << '\n';

    std::vector<Core::Image> outImgs;
    outImgs.reserve(outN.size());
    if (this->inputCopyRequired) {
        this->sharedInImg_0 = Core::Image(vk.device, extent, format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, in0);
        this->sharedInImg_1 = Core::Image(vk.device, extent, format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, in1);
        this->inImg_0 = Core::Image(vk.device, extent, format,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        this->inImg_1 = Core::Image(vk.device, extent, format,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    } else {
        this->inImg_0 = Core::Image(vk.device, extent, format,
            VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, in0);
        this->inImg_1 = Core::Image(vk.device, extent, format,
            VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, in1);
    }
    if (this->outputCopyRequired) {
        this->sharedOutImages.reserve(outN.size());
        for (auto* ahb : outN)
            this->sharedOutImages.emplace_back(vk.device, extent, format,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, ahb);
        for (size_t i = 0; i < outN.size(); ++i)
            outImgs.emplace_back(vk.device, extent, format,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    } else {
        for (auto* ahb : outN)
            outImgs.emplace_back(vk.device, extent, format,
                VK_IMAGE_USAGE_STORAGE_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, ahb);
    }

    for (size_t i = 0; i < 8; i++) {
        auto& data = this->data.at(i);
        data.preprocessingFence = Core::Fence(vk.device);
        data.internalSemaphores.resize(vk.generationCount);
        for (auto& internalSemaphore : data.internalSemaphores)
            internalSemaphore = Core::Semaphore(vk.device);
        data.outSemaphores.resize(vk.generationCount);
        data.completionFences.resize(vk.generationCount);
        for (auto& completionFence : data.completionFences)
            completionFence = Core::Fence(vk.device);
        data.cmdBuffers2.resize(vk.generationCount);
#ifdef __ANDROID__
        data.adaptiveFlowTimingQueryPool =
            Core::TimestampQueryPool(vk.device, 4);
#endif
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

Context::Context(Vulkan& vk,
        AHardwareBuffer* in0, AHardwareBuffer* in1,
        const std::vector<AHardwareBuffer*>& outN,
        VkExtent2D extent, VkFormat format,
        const std::vector<float>& adaptiveFlowScales)
        : Context(vk, in0, in1, outN, extent, format) {
    if (adaptiveFlowScales.empty())
        throw std::invalid_argument("Adaptive Flow Scale requires at least one prepared state");

    const float primaryScale = 1.0F / vk.flowScale;
    if (std::abs(adaptiveFlowScales.front() - primaryScale) > 0.0005F)
        throw std::invalid_argument(
            "Adaptive Flow Scale target must match the initialized runtime scale");

    float previous = 1.01F;
    for (const float scale : adaptiveFlowScales) {
        if (!std::isfinite(scale) || scale < 0.25F || scale > 1.0F)
            throw std::invalid_argument("Adaptive Flow Scale state outside 0.25..1.0");
        if (scale >= previous)
            throw std::invalid_argument(
                "Adaptive Flow Scale states must be strictly target-to-minimum");
        previous = scale;
    }

    this->adaptiveFlowScales_ = adaptiveFlowScales;
    this->activeFlowGraphIndex_ = 0;
    this->requestedFlowScale_ = adaptiveFlowScales.front();
    this->adaptiveFlowGraphs_.reserve(adaptiveFlowScales.size() - 1);

    const auto outputImages = this->generate.getOutImages();
    for (size_t i = 1; i < adaptiveFlowScales.size(); ++i)
        this->adaptiveFlowGraphs_.emplace_back(
            this->buildAdaptiveFlowGraph(vk, adaptiveFlowScales.at(i), outputImages));

    double scaleArea = 0.0;
    for (const float scale : adaptiveFlowScales)
        scaleArea += static_cast<double>(scale) * static_cast<double>(scale);
    const double targetArea =
        static_cast<double>(adaptiveFlowScales.front())
        * static_cast<double>(adaptiveFlowScales.front());
    std::cerr << "lsfg-vk: adaptive-flow-resources states="
              << adaptiveFlowScales.size()
              << " descriptor_pool_mode=per-state"
              << " target=" << adaptiveFlowScales.front()
              << " min=" << adaptiveFlowScales.back()
              << " scale_area_ratio=" << (scaleArea / targetArea)
              << " history_frames=" << kAdaptiveFlowHistoryFrames
              << '\n';
}

Context::AdaptiveFlowGraph Context::buildAdaptiveFlowGraph(
        Vulkan& vk, float userFlowScale,
        const std::vector<Core::Image>& outImgs) {
    AdaptiveFlowGraph graph;
    graph.userFlowScale = userFlowScale;
    graph.descriptorPool = Core::DescriptorPool(vk.device);
    ScopedAdaptiveFlowConstruction construction(
        vk, userFlowScale, graph.descriptorPool);
    graph.mipmaps = Shaders::Mipmaps(vk, this->inImg_0, this->inImg_1);
    for (size_t i = 0; i < 7; ++i)
        graph.alpha.at(i) =
            Shaders::Alpha(vk, graph.mipmaps.getOutImages().at(i));
    graph.beta = Shaders::Beta(vk, graph.alpha.at(0).getOutImages());
    for (size_t i = 0; i < 7; ++i) {
        graph.gamma.at(i) = Shaders::Gamma(vk,
            graph.alpha.at(6 - i).getOutImages(),
            graph.beta.getOutImages().at(std::min<size_t>(6 - i, 5)),
            (i == 0)
                ? std::nullopt
                : std::make_optional(graph.gamma.at(i - 1).getOutImage()));
        if (i < 4)
            continue;
        graph.delta.at(i - 4) = Shaders::Delta(vk,
            graph.alpha.at(6 - i).getOutImages(),
            graph.beta.getOutImages().at(6 - i),
            (i == 4)
                ? std::nullopt
                : std::make_optional(graph.gamma.at(i - 1).getOutImage()),
            (i == 4)
                ? std::nullopt
                : std::make_optional(graph.delta.at(i - 5).getOutImage1()),
            (i == 4)
                ? std::nullopt
                : std::make_optional(graph.delta.at(i - 5).getOutImage2()));
    }
    graph.generate = Shaders::Generate(vk,
        this->inImg_0, this->inImg_1,
        graph.gamma.at(6).getOutImage(),
        graph.delta.at(2).getOutImage1(),
        graph.delta.at(2).getOutImage2(),
        outImgs);
    return graph;
}

Context::FlowGraphRef Context::flowGraph(size_t index) {
    if (index == 0) {
        return FlowGraphRef{
            .userFlowScale = this->adaptiveFlowScales_.at(0),
            .mipmaps = &this->mipmaps,
            .alpha = &this->alpha,
            .beta = &this->beta,
            .gamma = &this->gamma,
            .delta = &this->delta,
            .generate = &this->generate,
        };
    }
    auto& graph = this->adaptiveFlowGraphs_.at(index - 1);
    return FlowGraphRef{
        .userFlowScale = graph.userFlowScale,
        .mipmaps = &graph.mipmaps,
        .alpha = &graph.alpha,
        .beta = &graph.beta,
        .gamma = &graph.gamma,
        .delta = &graph.delta,
        .generate = &graph.generate,
    };
}

void Context::dispatchAdaptiveFlowPreprocess(
        const Core::CommandBuffer& buffer, FlowGraphRef graph,
        Core::TimestampQueryPool* timingPool) {
    graph.mipmaps->Dispatch(buffer, this->frameIdx);
    if (timingPool != nullptr)
        timingPool->write(buffer.handle(), 1);
    for (size_t i = 0; i < 7; ++i)
        graph.alpha->at(6 - i).Dispatch(buffer, this->frameIdx);
}

void Context::recordAdaptiveFlowGpuTiming(
        Vulkan& vk, RenderData& renderData) {
    if (!renderData.adaptiveFlowTimingQueryPool.supported())
        return;

    const auto durations =
        renderData.adaptiveFlowTimingQueryPool.durationsMs(vk.device);
    if (durations.size() != 3)
        return;

    const double mipmapsMs = durations.at(0);
    const double opticalFlowMs = durations.at(0) + durations.at(1);
    const double totalLsfgMs =
        durations.at(0) + durations.at(1) + durations.at(2);
    if (!std::isfinite(mipmapsMs) || !std::isfinite(opticalFlowMs)
            || !std::isfinite(totalLsfgMs))
        return;

    this->lastAdaptiveFlowGpuTiming_ = LSFG::AdaptiveFlowGpuTiming{
        .mipmapsMs = mipmapsMs,
        .opticalFlowMs = opticalFlowMs,
        .totalLsfgMs = totalLsfgMs,
        .generationCount = renderData.generationCount,
        .transitionActive = renderData.adaptiveFlowTransitionCycle,
        .valid = true,
    };
}

LSFG::AdaptiveFlowGpuTiming Context::gpuTiming() const {
    return this->lastAdaptiveFlowGpuTiming_;
}

void Context::requestFlowScale(float flowScale) {
    if (this->adaptiveFlowScales_.empty())
        throw std::logic_error("Context was not created for Adaptive Flow Scale");

    size_t requestedIndex = this->adaptiveFlowScales_.size();
    for (size_t i = 0; i < this->adaptiveFlowScales_.size(); ++i) {
        if (std::abs(this->adaptiveFlowScales_.at(i) - flowScale) <= 0.0005F) {
            requestedIndex = i;
            break;
        }
    }
    if (requestedIndex == this->adaptiveFlowScales_.size())
        throw std::invalid_argument("Requested Adaptive Flow Scale was not prebuilt");

    this->requestedFlowScale_ = this->adaptiveFlowScales_.at(requestedIndex);
    if (requestedIndex == this->activeFlowGraphIndex_) {
        this->pendingFlowGraphIndex_.reset();
        this->pendingFlowWarmupFrames_ = 0;
        return;
    }
    if (this->pendingFlowGraphIndex_ == requestedIndex)
        return;

    this->pendingFlowGraphIndex_ = requestedIndex;
    this->pendingFlowWarmupFrames_ = 0;
    std::cerr << "lsfg-vk: adaptive-flow-handoff requested="
              << this->requestedFlowScale_
              << " active=" << this->adaptiveFlowScales_.at(this->activeFlowGraphIndex_)
              << " history_frames=" << kAdaptiveFlowHistoryFrames
              << '\n';
}

LSFG::AdaptiveFlowContextState Context::flowScaleState() const {
    if (this->adaptiveFlowScales_.empty())
        return {};
    const uint32_t remaining = this->pendingFlowGraphIndex_.has_value()
        ? kAdaptiveFlowHistoryFrames
            - std::min(this->pendingFlowWarmupFrames_, kAdaptiveFlowHistoryFrames)
        : 0;
    return LSFG::AdaptiveFlowContextState{
        .requestedScale = this->requestedFlowScale_,
        .activeScale = this->adaptiveFlowScales_.at(this->activeFlowGraphIndex_),
        .warmupRemaining = remaining,
        .transitionPending = this->pendingFlowGraphIndex_.has_value(),
    };
}

void Context::commitAdaptiveFlowTransition(size_t index) {
    const float previous = this->adaptiveFlowScales_.at(this->activeFlowGraphIndex_);
    this->activeFlowGraphIndex_ = index;
    this->pendingFlowGraphIndex_.reset();
    this->pendingFlowWarmupFrames_ = 0;
    std::cerr << "lsfg-vk: adaptive-flow-handoff applied="
              << this->adaptiveFlowScales_.at(index)
              << " previous=" << previous
              << " history_frames=" << kAdaptiveFlowHistoryFrames
              << '\n';
}

#endif // __ANDROID__

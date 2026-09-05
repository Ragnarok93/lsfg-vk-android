#include "context.hpp"
#include "config/config.hpp"
#include "common/exception.hpp"
#include "extract/extract.hpp"
#include "extract/trans.hpp"
#include "utils/utils.hpp"
#include "hooks.hpp"
#include "layer.hpp"

#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#include <android/log.h>
#endif

#include <vulkan/vulkan_core.h>
#include <lsfg_3_1.hpp>
#include <lsfg_3_1p.hpp>

#include <filesystem>
#include <exception>
#include <iostream>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <array>

#ifdef __ANDROID__
namespace {

uint64_t runtimeWaitTimeoutNs() {
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

VkImageSubresourceRange colorSubresourceRange() {
    return VkImageSubresourceRange{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

VkImageBlit fullImageBlit(uint32_t width, uint32_t height) {
    return VkImageBlit{
        .srcSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .srcOffsets = {
            { 0, 0, 0 },
            { static_cast<int32_t>(width), static_cast<int32_t>(height), 1 },
        },
        .dstSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .dstOffsets = {
            { 0, 0, 0 },
            { static_cast<int32_t>(width), static_cast<int32_t>(height), 1 },
        },
    };
}

void copySwapchainToExternalAhb(VkCommandBuffer buf,
        VkImage swapchainImage, VkImage ahbImage,
        uint32_t width, uint32_t height,
        uint32_t graphicsFamily, bool firstUse) {
    const auto range = colorSubresourceRange();
    const VkImageMemoryBarrier acquireBarriers[] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchainImage,
            .subresourceRange = range,
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = firstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
            .dstQueueFamilyIndex = graphicsFamily,
            .image = ahbImage,
            .subresourceRange = range,
        },
    };
    Layer::ovkCmdPipelineBarrier(buf,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, nullptr, 0, nullptr,
        static_cast<uint32_t>(std::size(acquireBarriers)), acquireBarriers);

    const auto blit = fullImageBlit(width, height);
    Layer::ovkCmdBlitImage(buf,
        swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        ahbImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blit, VK_FILTER_NEAREST);

    const VkImageMemoryBarrier releaseBarriers[] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchainImage,
            .subresourceRange = range,
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = 0,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = graphicsFamily,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
            .image = ahbImage,
            .subresourceRange = range,
        },
    };
    Layer::ovkCmdPipelineBarrier(buf,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
        0, nullptr, 0, nullptr,
        static_cast<uint32_t>(std::size(releaseBarriers)), releaseBarriers);
}

void copyExternalAhbToSwapchain(VkCommandBuffer buf,
        VkImage ahbImage, VkImage swapchainImage,
        uint32_t width, uint32_t height,
        uint32_t graphicsFamily) {
    const auto range = colorSubresourceRange();
    const VkImageMemoryBarrier acquireBarriers[] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
            .dstQueueFamilyIndex = graphicsFamily,
            .image = ahbImage,
            .subresourceRange = range,
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchainImage,
            .subresourceRange = range,
        },
    };
    Layer::ovkCmdPipelineBarrier(buf,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, nullptr, 0, nullptr,
        static_cast<uint32_t>(std::size(acquireBarriers)), acquireBarriers);

    const auto blit = fullImageBlit(width, height);
    Layer::ovkCmdBlitImage(buf,
        ahbImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blit, VK_FILTER_NEAREST);

    const VkImageMemoryBarrier releaseBarriers[] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = 0,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = graphicsFamily,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
            .image = ahbImage,
            .subresourceRange = range,
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchainImage,
            .subresourceRange = range,
        },
    };
    Layer::ovkCmdPipelineBarrier(buf,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
        0, nullptr, 0, nullptr,
        static_cast<uint32_t>(std::size(releaseBarriers)), releaseBarriers);
}

void submitAndWaitForAhbHandoff(VkDevice device, Mini::CommandBuffer& commandBuffer,
        VkQueue queue, const std::vector<VkSemaphore>& waitSemaphores,
        const std::vector<VkSemaphore>& signalSemaphores,
        VkFence fence, PFN_vkResetFences resetFences,
        PFN_vkWaitForFences waitForFences) {
    if (fence == VK_NULL_HANDLE || resetFences == nullptr || waitForFences == nullptr)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Android AHB handoff fence is unavailable");

    auto res = resetFences(device, 1, &fence);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed resetting Android AHB handoff fence");

    commandBuffer.submit(queue, waitSemaphores, signalSemaphores, fence);
    res = waitForFences(device, 1, &fence, VK_TRUE, runtimeWaitTimeoutNs());
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed waiting for Android AHB handoff copy");
}

} // namespace
#endif

LsContext::LsContext(const Hooks::DeviceInfo& info, VkSwapchainKHR swapchain,
        VkExtent2D extent, const std::vector<VkImage>& swapchainImages)
        : swapchain(swapchain), swapchainImages(swapchainImages),
          extent(extent) {
    auto& conf = Config::activeConf;
    if (!conf.config_file.empty()
            && (
                    !std::filesystem::exists(conf.config_file)
                  || conf.timestamp != std::filesystem::last_write_time(conf.config_file)
            )) {
        std::cerr << "lsfg-vk: Rereading configuration, as it is no longer valid.\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        const std::string file = Utils::getConfigFile();
        const auto name = Utils::getProcessName();
        try {
            Config::updateConfig(file);
            conf = Config::getConfig(name);
        } catch (const std::exception& e) {
            std::cerr << "lsfg-vk: Failed to update configuration, continuing using old:\n";
            std::cerr << "- " << e.what() << '\n';
        }

        LSFG_3_1P::finalize();
        LSFG_3_1::finalize();

        std::cerr << "lsfg-vk: Reloaded configuration for " << name.second << ":\n";
        if (!conf.dll.empty()) std::cerr << "  Using DLL from: " << conf.dll << '\n';
        std::cerr << "  Multiplier: " << conf.multiplier << '\n';
        std::cerr << "  Flow Scale: " << conf.flowScale << '\n';
        std::cerr << "  Performance Mode: " << (conf.performance ? "Enabled" : "Disabled") << '\n';
        std::cerr << "  HDR Mode: " << (conf.hdr ? "Enabled" : "Disabled") << '\n';
        std::cerr << "  Adaptive FrameGen: " << (conf.adaptiveFramegen ? "Enabled" : "Disabled") << '\n';
        if (conf.adaptiveFramegen) std::cerr << "  Output FPS Cap: " << conf.fpsLimit << '\n';
        if (conf.e_present != 2) std::cerr << "  ! Present Mode: " << conf.e_present << '\n';

        if (conf.multiplier <= 1) return;
    }
    const VkFormat format = conf.hdr
        ? VK_FORMAT_R8G8B8A8_UNORM
        : VK_FORMAT_R16G16B16A16_SFLOAT;

    if (!info.identityValid)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Exact Vulkan device/driver UUID provenance is unavailable");

#ifdef __ANDROID__
    auto* lsfgInitialize = LSFG_3_1::initialize;
    auto* lsfgDeleteContext = LSFG_3_1::deleteContext;
    if (conf.performance) {
        lsfgInitialize = LSFG_3_1P::initialize;
        lsfgDeleteContext = LSFG_3_1P::deleteContext;
    }
    setenv("DISABLE_LSFG", "1", 1); // NOLINT
    lsfgInitialize(
        info.identity, format,
        conf.hdr, 1.0F / conf.flowScale, conf.multiplier - 1,
        [](const std::string& name) {
            auto dxbc = Extract::getShader(name);
            auto spirv = Extract::translateShader(dxbc);
            return spirv;
        }
    );

    const LSFG::BackendDiagnostics backendDiagnostics = conf.performance
        ? LSFG_3_1P::getBackendDiagnostics()
        : LSFG_3_1::getBackendDiagnostics();
    const auto ahbTransportMode = backendDiagnostics.ahbTransportMode;
    if (ahbTransportMode == LSFG::AhbTransportMode::Unsupported)
        throw LSFG::vulkan_error(VK_ERROR_FORMAT_NOT_SUPPORTED,
            "Exact game/framegen ICD has no supported AHB image transport for LSFG format");

    this->externalSemaphoreFdSync_ = info.androidExternalSemaphoreFdSupported
        && backendDiagnostics.externalSemaphoreFd;

    this->frame_0 = Mini::Image(info.device, info.physicalDevice,
        extent, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
        ahbTransportMode);
    this->frame_1 = Mini::Image(info.device, info.physicalDevice,
        extent, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
        ahbTransportMode);

    for (size_t i = 0; i < static_cast<size_t>(conf.multiplier - 1); ++i)
        this->out_n.emplace_back(info.device, info.physicalDevice,
            extent, format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
            ahbTransportMode);

    std::vector<AHardwareBuffer*> outAhbs;
    outAhbs.reserve(conf.multiplier - 1);
    for (size_t i = 0; i < static_cast<size_t>(conf.multiplier - 1); ++i)
        outAhbs.push_back(this->out_n.at(i).getAhb());

    int32_t ctxId;
    if (conf.performance)
        ctxId = LSFG_3_1P::createContextFromAHB(
            this->frame_0.getAhb(), this->frame_1.getAhb(),
            outAhbs, extent, format);
    else
        ctxId = LSFG_3_1::createContextFromAHB(
            this->frame_0.getAhb(), this->frame_1.getAhb(),
            outAhbs, extent, format);

    this->lsfgCtxId = std::shared_ptr<int32_t>(
        new int32_t(ctxId),
        [lsfgDeleteContext = lsfgDeleteContext](const int32_t* id) {
            lsfgDeleteContext(*id);
        }
    );

    unsetenv("DISABLE_LSFG"); // NOLINT

    if (!this->externalSemaphoreFdSync_) {
        const auto createHandoffFence = reinterpret_cast<PFN_vkCreateFence>(
            Layer::ovkGetDeviceProcAddr(info.device, "vkCreateFence"));
        this->resetHandoffFences = reinterpret_cast<PFN_vkResetFences>(
            Layer::ovkGetDeviceProcAddr(info.device, "vkResetFences"));
        this->waitHandoffFences = reinterpret_cast<PFN_vkWaitForFences>(
            Layer::ovkGetDeviceProcAddr(info.device, "vkWaitForFences"));
        const auto destroyHandoffFence = reinterpret_cast<PFN_vkDestroyFence>(
            Layer::ovkGetDeviceProcAddr(info.device, "vkDestroyFence"));
        if (createHandoffFence == nullptr || this->resetHandoffFences == nullptr
                || this->waitHandoffFences == nullptr || destroyHandoffFence == nullptr)
            throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
                "Required fence functions unavailable for Android AHB handoff");

        const VkFenceCreateInfo handoffFenceInfo{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        };
        VkFence handoffFence{};
        const auto handoffFenceRes = createHandoffFence(
            info.device, &handoffFenceInfo, nullptr, &handoffFence);
        if (handoffFenceRes != VK_SUCCESS || handoffFence == VK_NULL_HANDLE)
            throw LSFG::vulkan_error(handoffFenceRes,
                "Failed to create Android AHB handoff fence");

        this->ahbHandoffFence = std::shared_ptr<VkFence>(
            new VkFence(handoffFence),
            [device = info.device, destroyHandoffFence](VkFence* ownedFence) {
                if (ownedFence != nullptr) {
                    if (*ownedFence != VK_NULL_HANDLE)
                        destroyHandoffFence(device, *ownedFence, nullptr);
                    delete ownedFence;
                }
            });
    }

    std::cerr << "lsfg-vk: Android AHB context created (id=" << ctxId
              << ", sync="
              << (this->externalSemaphoreFdSync_ ? "external-semaphore-fd" : "host-fence")
              << ")\n";

#else
    std::array<int, 2> fds{};
    this->frame_0 = Mini::Image(info.device, info.physicalDevice,
        extent, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
        &fds.at(0));
    this->frame_1 = Mini::Image(info.device, info.physicalDevice,
        extent, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
        &fds.at(1));

    std::vector<int> outFds(conf.multiplier - 1);
    for (size_t i = 0; i < (conf.multiplier - 1); ++i)
        this->out_n.emplace_back(info.device, info.physicalDevice,
            extent, format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
            &outFds.at(i));

    auto* lsfgInitialize = LSFG_3_1::initialize;
    auto* lsfgCreateContext = LSFG_3_1::createContext;
    auto* lsfgDeleteContext = LSFG_3_1::deleteContext;
    if (conf.performance) {
        lsfgInitialize = LSFG_3_1P::initialize;
        lsfgCreateContext = LSFG_3_1P::createContext;
        lsfgDeleteContext = LSFG_3_1P::deleteContext;
    }

    setenv("DISABLE_LSFG", "1", 1); // NOLINT

    lsfgInitialize(
        info.identity, format,
        conf.hdr, 1.0F / conf.flowScale, conf.multiplier - 1,
        [](const std::string& name) {
            auto dxbc = Extract::getShader(name);
            auto spirv = Extract::translateShader(dxbc);
            return spirv;
        }
    );

    this->lsfgCtxId = std::shared_ptr<int32_t>(
        new int32_t(lsfgCreateContext(fds.at(0), fds.at(1), outFds, extent, format)),
        [lsfgDeleteContext = lsfgDeleteContext](const int32_t* id) {
            lsfgDeleteContext(*id);
        }
    );

    unsetenv("DISABLE_LSFG"); // NOLINT
#endif

    this->cmdPool = Mini::CommandPool(info.device, info.queue.first);
    for (size_t i = 0; i < 8; i++) {
        auto& pass = this->passInfos.at(i);
        pass.renderSemaphores.resize(conf.multiplier - 1);
        pass.acquireSemaphores.resize(conf.multiplier - 1);
        pass.postCopyBufs.resize(conf.multiplier - 1);
        pass.postCopySemaphores.resize(conf.multiplier - 1);
        pass.prevPostCopySemaphores.resize(conf.multiplier - 1);
    }
}

VkResult LsContext::present(const Hooks::DeviceInfo& info, const void* pNext, VkQueue queue,
        const std::vector<VkSemaphore>& gameRenderSemaphores, uint32_t presentIdx) {
    const auto& conf = Config::activeConf;
    auto& pass = this->passInfos.at(this->frameIdx % 8);

#ifdef __ANDROID__
    auto& metrics = this->runtimeMetrics;
    const auto cycleStart = RuntimeMetrics::Clock::now();
    this->adaptiveScheduler_.configure(
        conf.adaptiveFramegen ? conf.fpsLimit : 0,
        conf.multiplier > 1 ? static_cast<size_t>(conf.multiplier - 1) : 0);
    std::chrono::nanoseconds sourceInterval{};
    if (metrics.hasLastSourcePresent) {
        sourceInterval = std::chrono::duration_cast<std::chrono::nanoseconds>(
            cycleStart - metrics.lastSourcePresent);
        const double sourceIntervalMs = std::chrono::duration<double, std::milli>(
            sourceInterval).count();
        metrics.windowSourceIntervalMs += sourceIntervalMs;
        if (sourceIntervalMs > metrics.windowSourceIntervalMaxMs)
            metrics.windowSourceIntervalMaxMs = sourceIntervalMs;
        metrics.windowSourceIntervals++;
    }
    metrics.lastSourcePresent = cycleStart;
    metrics.hasLastSourcePresent = true;
    const size_t generatedFrameCount = conf.adaptiveFramegen
        ? this->adaptiveScheduler_.plan(sourceInterval)
        : static_cast<size_t>(conf.multiplier - 1);
    const bool warmupSourceHistory =
        generatedFrameCount > 0 && this->requiresSourceHistoryWarmup_;
    this->lastGeneratedFrameCount_ = generatedFrameCount;

    const bool firstPresentDiagnostic = this->frameIdx == 0;
    if (firstPresentDiagnostic) {
        std::cerr << "lsfg-vk: runtime stage=first-present-enter image=" << presentIdx
                  << " multiplier=" << conf.multiplier
                  << " adaptive=" << (conf.adaptiveFramegen ? 1 : 0)
                  << " target_fps=" << conf.fpsLimit
                  << " performance=" << (conf.performance ? 1 : 0)
                  << " sync="
                  << (this->externalSemaphoreFdSync_ ? "external-semaphore-fd" : "host-fence")
                  << "\n";
    }

    const auto finishSourcePresent = [&](VkResult result, const char* sourceWait) -> VkResult {
        metrics.windowSourceFrames++;
        metrics.totalSourceFrames++;
        if (firstPresentDiagnostic) {
            std::cerr << "lsfg-vk: runtime stage=present-sync-ready generatedSignals="
                      << generatedFrameCount << " sourceWait=" << sourceWait << "\n";
            std::cerr << "lsfg-vk: runtime stage=first-present-cycle-ready result=" << result
                      << " generated=" << generatedFrameCount << "\n";
        }

        const auto cycleEnd = RuntimeMetrics::Clock::now();
        const double cycleMs = std::chrono::duration<double, std::milli>(
            cycleEnd - cycleStart).count();
        metrics.windowCycleMs += cycleMs;
        if (cycleMs > metrics.windowCycleMaxMs)
            metrics.windowCycleMaxMs = cycleMs;

        const double elapsedSeconds = std::chrono::duration<double>(
            cycleEnd - metrics.windowStart).count();
        if (elapsedSeconds >= 1.0) {
            const double sourceCount = static_cast<double>(metrics.windowSourceFrames);
            const double generatedCount = static_cast<double>(metrics.windowGeneratedFrames);
            const double sourceFps = sourceCount / elapsedSeconds;
            const double generatedFps = generatedCount / elapsedSeconds;
            const double outputFps = (sourceCount + generatedCount) / elapsedSeconds;
            const double cycleAvgMs = sourceCount > 0.0 ? metrics.windowCycleMs / sourceCount : 0.0;
            const double handoffAvgMs = sourceCount > 0.0 ? metrics.windowHandoffMs / sourceCount : 0.0;
            const double dispatchAvgMs = sourceCount > 0.0 ? metrics.windowDispatchMs / sourceCount : 0.0;
            const double waitIdleAvgMs = sourceCount > 0.0 ? metrics.windowWaitIdleMs / sourceCount : 0.0;
            const double generatedPresentAvgMs = generatedCount > 0.0
                ? metrics.windowGeneratedPresentMs / generatedCount : 0.0;
            const double sourceIntervalAvgMs = metrics.windowSourceIntervals > 0
                ? metrics.windowSourceIntervalMs / static_cast<double>(metrics.windowSourceIntervals)
                : 0.0;

            std::cerr << "lsfg-vk: metrics"
                      << " source_fps=" << sourceFps
                      << " generated_fps=" << generatedFps
                      << " output_fps=" << outputFps
                      << " source_frames=" << metrics.windowSourceFrames
                      << " generated_frames=" << metrics.windowGeneratedFrames
                      << " source_frames_total=" << metrics.totalSourceFrames
                      << " generated_frames_total=" << metrics.totalGeneratedFrames
                      << " source_present_failures=" << metrics.windowSourcePresentFailures
                      << " generated_present_failures=" << metrics.windowGeneratedPresentFailures
                      << " source_present_failures_total=" << metrics.totalSourcePresentFailures
                      << " generated_present_failures_total=" << metrics.totalGeneratedPresentFailures
                      << " cycle_avg_ms=" << cycleAvgMs
                      << " cycle_max_ms=" << metrics.windowCycleMaxMs
                      << " ahb_handoff_avg_ms=" << handoffAvgMs
                      << " framegen_dispatch_avg_ms=" << dispatchAvgMs
                      << " framegen_wait_avg_ms=" << waitIdleAvgMs
                      << " generated_present_avg_ms=" << generatedPresentAvgMs
                      << " source_interval_avg_ms=" << sourceIntervalAvgMs
                      << " source_interval_max_ms=" << metrics.windowSourceIntervalMaxMs
                      << " multiplier=" << conf.multiplier
                      << " adaptive=" << (conf.adaptiveFramegen ? 1 : 0)
                      << " target_fps=" << conf.fpsLimit
                      << " performance=" << (conf.performance ? 1 : 0)
                      << " sync="
                      << (this->externalSemaphoreFdSync_ ? "external-semaphore-fd" : "host-fence")
                      << "\n";

            metrics.windowStart = cycleEnd;
            metrics.windowSourceFrames = 0;
            metrics.windowGeneratedFrames = 0;
            metrics.windowSourcePresentFailures = 0;
            metrics.windowGeneratedPresentFailures = 0;
            metrics.windowCycleMs = 0.0;
            metrics.windowCycleMaxMs = 0.0;
            metrics.windowHandoffMs = 0.0;
            metrics.windowDispatchMs = 0.0;
            metrics.windowWaitIdleMs = 0.0;
            metrics.windowGeneratedPresentMs = 0.0;
            metrics.windowSourceIntervalMs = 0.0;
            metrics.windowSourceIntervalMaxMs = 0.0;
            metrics.windowSourceIntervals = 0;
        }

        this->frameIdx++;
        return result;
    };

    if (generatedFrameCount == 0) {
        this->requiresSourceHistoryWarmup_ = true;
        this->previousSourceCopySignalValid_ = false;
        const auto delay = this->adaptiveScheduler_.delayUntilNextSourceOutput(
            AdaptiveFrameScheduler::Clock::now());
        if (delay > std::chrono::nanoseconds::zero())
            std::this_thread::sleep_for(delay);

        const VkPresentInfoKHR directPresentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = pNext,
            .waitSemaphoreCount = static_cast<uint32_t>(gameRenderSemaphores.size()),
            .pWaitSemaphores = gameRenderSemaphores.data(),
            .swapchainCount = 1,
            .pSwapchains = &this->swapchain,
            .pImageIndices = &presentIdx,
        };
        const auto directResult = Layer::ovkQueuePresentKHR(queue, &directPresentInfo);
        if (directResult != VK_SUCCESS && directResult != VK_SUBOPTIMAL_KHR) {
            metrics.windowSourcePresentFailures++;
            metrics.totalSourcePresentFailures++;
            throw LSFG::vulkan_error(directResult, "Failed to present source frame directly");
        }
        if (firstPresentDiagnostic)
            std::cerr << "lsfg-vk: runtime stage=source-direct-present\n";
        return finishSourcePresent(directResult, "game-render");
    }

    int preCopySemaphoreFd = -1;
    if (this->externalSemaphoreFdSync_ && !warmupSourceHistory)
        pass.preCopySemaphores.at(0) = Mini::Semaphore(info.device, &preCopySemaphoreFd);
    else
        pass.preCopySemaphores.at(0) = Mini::Semaphore(info.device);
    pass.preCopySemaphores.at(1) = Mini::Semaphore(info.device);
    pass.preCopyBuf = Mini::CommandBuffer(info.device, this->cmdPool);
    pass.preCopyBuf.begin();

    copySwapchainToExternalAhb(pass.preCopyBuf.handle(),
        this->swapchainImages.at(presentIdx),
        this->frameIdx % 2 == 0 ? this->frame_0.handle() : this->frame_1.handle(),
        this->extent.width, this->extent.height,
        info.queue.first, this->frameIdx < 2);

    pass.preCopyBuf.end();

    std::vector<VkSemaphore> gameRenderSemaphores2 = gameRenderSemaphores;
    if (this->previousSourceCopySignalValid_)
        gameRenderSemaphores2.emplace_back(this->passInfos.at((this->frameIdx - 1) % 8)
            .preCopySemaphores.at(1).handle());

    const auto handoffStart = RuntimeMetrics::Clock::now();
    std::vector<VkSemaphore> preCopySignals{
        pass.preCopySemaphores.at(0).handle(),
        pass.preCopySemaphores.at(1).handle(),
    };
    if (this->externalSemaphoreFdSync_) {
        pass.preCopyBuf.submit(info.queue.second, gameRenderSemaphores2, preCopySignals);
    } else {
        submitAndWaitForAhbHandoff(info.device, pass.preCopyBuf, info.queue.second,
            gameRenderSemaphores2, preCopySignals,
            *this->ahbHandoffFence, this->resetHandoffFences,
            this->waitHandoffFences);
    }
    this->previousSourceCopySignalValid_ = true;
    metrics.windowHandoffMs += std::chrono::duration<double, std::milli>(
        RuntimeMetrics::Clock::now() - handoffStart).count();
    if (firstPresentDiagnostic)
        std::cerr << "lsfg-vk: runtime stage=source-ahb-handoff-ready sync="
                  << (this->externalSemaphoreFdSync_ ? "external-semaphore-fd" : "host-fence")
                  << "\n";

    if (warmupSourceHistory) {
        this->requiresSourceHistoryWarmup_ = false;
        this->lastGeneratedFrameCount_ = 0;
        const VkSemaphore sourceReady = pass.preCopySemaphores.at(0).handle();
        const VkPresentInfoKHR warmupPresentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = pNext,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &sourceReady,
            .swapchainCount = 1,
            .pSwapchains = &this->swapchain,
            .pImageIndices = &presentIdx,
        };
        const auto warmupResult = Layer::ovkQueuePresentKHR(queue, &warmupPresentInfo);
        if (warmupResult != VK_SUCCESS && warmupResult != VK_SUBOPTIMAL_KHR) {
            metrics.windowSourcePresentFailures++;
            metrics.totalSourcePresentFailures++;
            throw LSFG::vulkan_error(warmupResult,
                "Failed to present source-history warmup frame");
        }
        std::cerr << "lsfg-vk: runtime stage=source-history-warmup\n";
        return finishSourcePresent(warmupResult, "pre-copy-warmup");
    }

    std::vector<int> renderSemaphoreFds;
    if (this->externalSemaphoreFdSync_) {
        renderSemaphoreFds.resize(generatedFrameCount, -1);
        for (size_t i = 0; i < generatedFrameCount; ++i)
            pass.renderSemaphores.at(i) = Mini::Semaphore(
                info.device, &renderSemaphoreFds.at(i));
    }

    if (firstPresentDiagnostic) {
        std::cerr << "lsfg-vk: runtime stage=framegen-dispatch-begin mode="
                  << (conf.performance ? "performance" : "quality")
                  << " generated=" << generatedFrameCount
                  << " sync="
                  << (this->externalSemaphoreFdSync_ ? "external-semaphore-fd" : "host-fence")
                  << "\n";
    }
    const auto dispatchStart = RuntimeMetrics::Clock::now();
    if (conf.performance)
        LSFG_3_1P::presentContextWithCount(
            *this->lsfgCtxId,
            this->externalSemaphoreFdSync_ ? preCopySemaphoreFd : -1,
            renderSemaphoreFds, generatedFrameCount);
    else
        LSFG_3_1::presentContextWithCount(
            *this->lsfgCtxId,
            this->externalSemaphoreFdSync_ ? preCopySemaphoreFd : -1,
            renderSemaphoreFds, generatedFrameCount);
    metrics.windowDispatchMs += std::chrono::duration<double, std::milli>(
        RuntimeMetrics::Clock::now() - dispatchStart).count();
    if (firstPresentDiagnostic)
        std::cerr << "lsfg-vk: runtime stage=framegen-dispatch-returned\n";

    if (!this->externalSemaphoreFdSync_) {
        const auto waitIdleStart = RuntimeMetrics::Clock::now();
        const uint64_t framegenCompletionTimeoutNs = runtimeWaitTimeoutNs();
        const bool framegenReady = conf.performance
            ? LSFG_3_1P::waitContext(*this->lsfgCtxId, framegenCompletionTimeoutNs)
            : LSFG_3_1::waitContext(*this->lsfgCtxId, framegenCompletionTimeoutNs);
        metrics.windowWaitIdleMs += std::chrono::duration<double, std::milli>(
            RuntimeMetrics::Clock::now() - waitIdleStart).count();
        if (!framegenReady) {
            this->lastGeneratedFrameCount_ = 0;
            std::cerr << "lsfg-vk: runtime stage=framegen-completion-timeout timeout_ms="
                      << (framegenCompletionTimeoutNs / 1'000'000ULL)
                      << "; presenting source and requesting swapchain recreation\n";
            const VkSemaphore sourceReady = pass.preCopySemaphores.at(0).handle();
            const VkPresentInfoKHR timeoutPresentInfo{
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .pNext = pNext,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &sourceReady,
                .swapchainCount = 1,
                .pSwapchains = &this->swapchain,
                .pImageIndices = &presentIdx,
            };
            const auto timeoutPresentResult = Layer::ovkQueuePresentKHR(queue, &timeoutPresentInfo);
            if (timeoutPresentResult != VK_SUCCESS && timeoutPresentResult != VK_SUBOPTIMAL_KHR) {
                metrics.windowSourcePresentFailures++;
                metrics.totalSourcePresentFailures++;
                throw LSFG::vulkan_error(timeoutPresentResult,
                    "Failed to present source frame after framegen timeout");
            }
            return finishSourcePresent(VK_ERROR_OUT_OF_DATE_KHR, "pre-copy-timeout");
        }
        if (firstPresentDiagnostic)
            std::cerr << "lsfg-vk: runtime stage=framegen-idle-ready\n";
    }

    for (size_t i = 0; i < generatedFrameCount; i++) {
        const auto generatedPresentStart = RuntimeMetrics::Clock::now();
        pass.acquireSemaphores.at(i) = Mini::Semaphore(info.device);
        uint32_t imageIdx{};
        auto res = Layer::ovkAcquireNextImageKHR(info.device, this->swapchain, runtimeWaitTimeoutNs(),
            pass.acquireSemaphores.at(i).handle(), VK_NULL_HANDLE, &imageIdx);
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
            metrics.windowGeneratedPresentFailures++;
            metrics.totalGeneratedPresentFailures++;
            throw LSFG::vulkan_error(res, "Failed to acquire next swapchain image");
        }

        pass.postCopySemaphores.at(i) = Mini::Semaphore(info.device);
        pass.prevPostCopySemaphores.at(i) = Mini::Semaphore(info.device);
        pass.postCopyBufs.at(i) = Mini::CommandBuffer(info.device, this->cmdPool);
        pass.postCopyBufs.at(i).begin();

        copyExternalAhbToSwapchain(pass.postCopyBufs.at(i).handle(),
            this->out_n.at(i).handle(),
            this->swapchainImages.at(imageIdx),
            this->extent.width, this->extent.height,
            info.queue.first);

        pass.postCopyBufs.at(i).end();
        std::vector<VkSemaphore> postCopyWaits{
            pass.acquireSemaphores.at(i).handle(),
        };
        if (this->externalSemaphoreFdSync_)
            postCopyWaits.emplace_back(pass.renderSemaphores.at(i).handle());
        pass.postCopyBufs.at(i).submit(info.queue.second,
            postCopyWaits,
            { pass.postCopySemaphores.at(i).handle(),
              pass.prevPostCopySemaphores.at(i).handle() });

        std::vector<VkSemaphore> waitSemaphores{ pass.postCopySemaphores.at(i).handle() };
        if (i != 0) waitSemaphores.emplace_back(pass.prevPostCopySemaphores.at(i - 1).handle());

        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = i == 0 ? pNext : nullptr,
            .waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size()),
            .pWaitSemaphores = waitSemaphores.data(),
            .swapchainCount = 1,
            .pSwapchains = &this->swapchain,
            .pImageIndices = &imageIdx,
        };
        res = Layer::ovkQueuePresentKHR(queue, &presentInfo);
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
            metrics.windowGeneratedPresentFailures++;
            metrics.totalGeneratedPresentFailures++;
            throw LSFG::vulkan_error(res, "Failed to present swapchain image");
        }
        metrics.windowGeneratedFrames++;
        metrics.totalGeneratedFrames++;
        metrics.windowGeneratedPresentMs += std::chrono::duration<double, std::milli>(
            RuntimeMetrics::Clock::now() - generatedPresentStart).count();
        if (firstPresentDiagnostic && i == 0) {
            std::cerr << "lsfg-vk: runtime stage=generated-present-ready image=" << imageIdx
                      << " result=" << res << "\n";
        }
    }

    VkSemaphore lastPrevPostCopySemaphore = generatedFrameCount > 0
        ? pass.prevPostCopySemaphores.at(generatedFrameCount - 1).handle()
        : pass.preCopySemaphores.at(0).handle();
    const VkPresentInfoKHR finalPresentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = generatedFrameCount == 0 ? pNext : nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &lastPrevPostCopySemaphore,
        .swapchainCount = 1,
        .pSwapchains = &this->swapchain,
        .pImageIndices = &presentIdx,
    };
    auto res = Layer::ovkQueuePresentKHR(queue, &finalPresentInfo);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        metrics.windowSourcePresentFailures++;
        metrics.totalSourcePresentFailures++;
        throw LSFG::vulkan_error(res, "Failed to present swapchain image");
    }
    return finishSourcePresent(res, "prev-post-copy");

#else
    int preCopySemaphoreFd{};
    pass.preCopySemaphores.at(0) = Mini::Semaphore(info.device, &preCopySemaphoreFd);
    pass.preCopySemaphores.at(1) = Mini::Semaphore(info.device);
    pass.preCopyBuf = Mini::CommandBuffer(info.device, this->cmdPool);
    pass.preCopyBuf.begin();

    Utils::copyImage(pass.preCopyBuf.handle(),
        this->swapchainImages.at(presentIdx),
        this->frameIdx % 2 == 0 ? this->frame_0.handle() : this->frame_1.handle(),
        this->extent.width, this->extent.height,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        true, false);

    pass.preCopyBuf.end();

    std::vector<VkSemaphore> gameRenderSemaphores2 = gameRenderSemaphores;
    if (this->frameIdx > 0)
        gameRenderSemaphores2.emplace_back(this->passInfos.at((this->frameIdx - 1) % 8)
            .preCopySemaphores.at(1).handle());
    pass.preCopyBuf.submit(info.queue.second,
        gameRenderSemaphores2,
        { pass.preCopySemaphores.at(0).handle(),
          pass.preCopySemaphores.at(1).handle() });

    std::vector<int> renderSemaphoreFds(conf.multiplier - 1);
    for (size_t i = 0; i < (conf.multiplier - 1); ++i)
        pass.renderSemaphores.at(i) = Mini::Semaphore(info.device, &renderSemaphoreFds.at(i));

    if (conf.performance)
        LSFG_3_1P::presentContext(*this->lsfgCtxId,
            preCopySemaphoreFd,
            renderSemaphoreFds);
    else
        LSFG_3_1::presentContext(*this->lsfgCtxId,
            preCopySemaphoreFd,
            renderSemaphoreFds);

    for (size_t i = 0; i < (conf.multiplier - 1); i++) {
        pass.acquireSemaphores.at(i) = Mini::Semaphore(info.device);
        uint32_t imageIdx{};
        auto res = Layer::ovkAcquireNextImageKHR(info.device, this->swapchain, runtimeWaitTimeoutNs(),
            pass.acquireSemaphores.at(i).handle(), VK_NULL_HANDLE, &imageIdx);
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
            throw LSFG::vulkan_error(res, "Failed to acquire next swapchain image");

        pass.postCopySemaphores.at(i) = Mini::Semaphore(info.device);
        pass.prevPostCopySemaphores.at(i) = Mini::Semaphore(info.device);
        pass.postCopyBufs.at(i) = Mini::CommandBuffer(info.device, this->cmdPool);
        pass.postCopyBufs.at(i).begin();

        Utils::copyImage(pass.postCopyBufs.at(i).handle(),
            this->out_n.at(i).handle(),
            this->swapchainImages.at(imageIdx),
            this->extent.width, this->extent.height,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            false, true);

        pass.postCopyBufs.at(i).end();
        pass.postCopyBufs.at(i).submit(info.queue.second,
            { pass.acquireSemaphores.at(i).handle(),
              pass.renderSemaphores.at(i).handle() },
            { pass.postCopySemaphores.at(i).handle(),
              pass.prevPostCopySemaphores.at(i).handle() });

        std::vector<VkSemaphore> waitSemaphores{ pass.postCopySemaphores.at(i).handle() };
        if (i != 0) waitSemaphores.emplace_back(pass.prevPostCopySemaphores.at(i - 1).handle());

        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = i == 0 ? pNext : nullptr,
            .waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size()),
            .pWaitSemaphores = waitSemaphores.data(),
            .swapchainCount = 1,
            .pSwapchains = &this->swapchain,
            .pImageIndices = &imageIdx,
        };
        res = Layer::ovkQueuePresentKHR(queue, &presentInfo);
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
            throw LSFG::vulkan_error(res, "Failed to present swapchain image");
    }

    VkSemaphore lastPrevPostCopySemaphore =
        pass.prevPostCopySemaphores.at(conf.multiplier - 1 - 1).handle();
    const VkPresentInfoKHR presentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &lastPrevPostCopySemaphore,
        .swapchainCount = 1,
        .pSwapchains = &this->swapchain,
        .pImageIndices = &presentIdx,
    };
    auto res = Layer::ovkQueuePresentKHR(queue, &presentInfo);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
        throw LSFG::vulkan_error(res, "Failed to present swapchain image");

    this->frameIdx++;
    return res;
#endif
}

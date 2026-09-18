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
#include <time.h>
#endif

#include <vulkan/vulkan_core.h>
#include <lsfg_3_1.hpp>
#include <lsfg_3_1p.hpp>

#include <filesystem>
#include <algorithm>
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
#include <cmath>

namespace {

size_t residentCapacityMultiplier(const Config::Configuration& conf) {
#ifdef __ANDROID__
    constexpr size_t kAndroidResidentMaxMultiplier = 4;
    if (conf.targeted)
        return std::max(conf.multiplier, kAndroidResidentMaxMultiplier);
#endif
    return conf.multiplier;
}

#ifdef __ANDROID__
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

uint64_t monotonicNowNs() {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return static_cast<uint64_t>(now.tv_sec) * 1'000'000'000ULL
        + static_cast<uint64_t>(now.tv_nsec);
}

AdaptiveFlowPreset adaptiveFlowPresetFromConfig(const std::string& preset) {
    if (preset == "balanced")
        return AdaptiveFlowPreset::Balanced;
    if (preset == "low")
        return AdaptiveFlowPreset::Low;
    return AdaptiveFlowPreset::Quality;
}

bool adaptiveLsfgTransition(const AdaptiveSchedulerTelemetry& telemetry) {
    return telemetry.sourceRateSnapped
        || telemetry.costRaised
        || telemetry.costBackedOff
        || telemetry.costProbe
        || telemetry.discontinuityReset
        || telemetry.configWarmStart;
}

double adaptiveFlowFrameBudgetMs(
        const Config::Configuration& conf,
        std::chrono::nanoseconds sourceInterval,
        size_t generatedFrameCount) {
    if (conf.adaptiveFramegen && conf.fpsLimit > 0)
        return 1000.0 / static_cast<double>(conf.fpsLimit);

    const double sourceIntervalMs =
        std::chrono::duration<double, std::milli>(sourceInterval).count();
    if (!(sourceIntervalMs > 0.0) || !std::isfinite(sourceIntervalMs))
        return 0.0;
    return sourceIntervalMs / static_cast<double>(generatedFrameCount + 1);
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

// Copy the real game frame into an AHardwareBuffer-backed VkImage and release
// ownership to the external queue family. The framegen VkDevice performs the
// matching EXTERNAL -> compute-family acquire before reading the same AHB.
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

// Acquire a generated AHB from framegen, copy it into an acquired swapchain
// image, then release the AHB back to EXTERNAL for the next framegen cycle.
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

void submitAhbHandoff(VkDevice device, Mini::CommandBuffer& commandBuffer,
        VkQueue queue, const std::vector<VkSemaphore>& waitSemaphores,
        const std::vector<VkSemaphore>& signalSemaphores,
        VkFence fence, PFN_vkResetFences resetFences) {
    if (fence == VK_NULL_HANDLE || resetFences == nullptr)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Android AHB handoff fence is unavailable");

    const auto resetRes = resetFences(device, 1, &fence);
    if (resetRes != VK_SUCCESS)
        throw LSFG::vulkan_error(resetRes,
            "Failed resetting Android AHB handoff fence");

    commandBuffer.submit(queue, waitSemaphores, signalSemaphores, fence);
}

void waitForAhbHandoff(VkDevice device, VkFence fence,
        PFN_vkWaitForFences waitForFences) {
    if (fence == VK_NULL_HANDLE || waitForFences == nullptr)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Android AHB handoff wait is unavailable");

    const auto res = waitForFences(
        device, 1, &fence, VK_TRUE, runtimeWaitTimeoutNs());
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res,
            "Failed waiting for Android AHB handoff copy");
}

// Proven compatibility fallback: host-wait the game-device source copy before
// framegen's separate VkDevice acquires the AHB. The async path below only
// replaces this wait when both devices explicitly support a shared semaphore FD.
void submitAndWaitForAhbHandoff(VkDevice device, Mini::CommandBuffer& commandBuffer,
        VkQueue queue, const std::vector<VkSemaphore>& waitSemaphores,
        const std::vector<VkSemaphore>& signalSemaphores,
        VkFence fence, PFN_vkResetFences resetFences,
        PFN_vkWaitForFences waitForFences) {
    submitAhbHandoff(device, commandBuffer, queue, waitSemaphores,
        signalSemaphores, fence, resetFences);
    waitForAhbHandoff(device, fence, waitForFences);
}

#endif

} // namespace

LsContext::LsContext(const Hooks::DeviceInfo& info, VkSwapchainKHR swapchain,
        VkExtent2D extent, const std::vector<VkImage>& swapchainImages)
        : swapchain(swapchain), swapchainImages(swapchainImages),
          extent(extent) {
    // get updated configuration
    auto& conf = Config::activeConf;
    if (!conf.config_file.empty()
            && (
                    !std::filesystem::exists(conf.config_file)
                  || conf.timestamp != std::filesystem::last_write_time(conf.config_file)
            )) {
        std::cerr << "lsfg-vk: Rereading configuration, as it is no longer valid.\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // reread configuration
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

        std::cerr << "lsfg-vk: configuration reloaded target=" << name.second
                  << " multiplier=" << conf.multiplier
                  << " adaptive=" << (conf.adaptiveFramegen ? 1 : 0)
                  << " target_fps=" << conf.fpsLimit << '\n';

        if (conf.multiplier <= 1 && !conf.targeted) return;
    }
    const size_t runtimeMultiplier = residentCapacityMultiplier(conf);

    // we could take the format from the swapchain,
    // but honestly this is safer.
    const VkFormat format = conf.hdr
        ? VK_FORMAT_R8G8B8A8_UNORM
        : VK_FORMAT_R16G16B16A16_SFLOAT;

    if (!info.identityValid)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Exact Vulkan device/driver UUID provenance is unavailable");

#ifdef __ANDROID__
    // Select and validate the exact framegen ICD before allocating any shared AHB.
    auto* lsfgInitialize = LSFG_3_1::initialize;
    auto* lsfgDeleteContext = LSFG_3_1::deleteContext;
    if (conf.performance) {
        lsfgInitialize = LSFG_3_1P::initialize;
        lsfgDeleteContext = LSFG_3_1P::deleteContext;
    }

    this->adaptiveFlowPreset_ = adaptiveFlowPresetFromConfig(conf.adaptiveFlowPreset);
    this->adaptiveFlowController_.configure(
        conf.adaptiveFlowScale, this->adaptiveFlowPreset_);
    this->adaptiveDisplayTimingEnabled_ =
        info.androidDisplayTimingSupported
        && (conf.adaptiveFramegen || conf.adaptiveFlowScale);
    std::cerr << "lsfg-vk: adaptive-present-pacing"
              << " fifo=1"
              << " display_timing="
              << (this->adaptiveDisplayTimingEnabled_ ? 1 : 0)
              << " adaptive_fg=" << (conf.adaptiveFramegen ? 1 : 0)
              << " adaptive_flow=" << (conf.adaptiveFlowScale ? 1 : 0)
              << '\n';

    std::vector<float> adaptiveFlowScales;
    float initialFlowScale = conf.flowScale;
    if (conf.adaptiveFlowScale) {
        const auto presetStates =
            AdaptiveFlowController::statesForPreset(this->adaptiveFlowPreset_);
        adaptiveFlowScales.assign(presetStates.begin(), presetStates.end());
        initialFlowScale = adaptiveFlowScales.front();
        this->adaptiveFlowRequestedScale_ = initialFlowScale;
        this->adaptiveFlowActiveScale_ = initialFlowScale;
        std::cerr << "lsfg-vk: adaptive-flow-controller enabled=1"
                  << " preset="
                  << AdaptiveFlowController::presetName(this->adaptiveFlowPreset_)
                  << " target=" << presetStates.front()
                  << " minimum=" << presetStates.back()
                  << " states=" << presetStates.size()
                  << '\n';
    }

    setenv("DISABLE_LSFG", "1", 1); // NOLINT
    lsfgInitialize(
        info.identity, format,
        conf.hdr, 1.0F / initialFlowScale, runtimeMultiplier - 1,
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

    // Android path: use AHardwareBuffer-backed images for sharing with framegen.
    // The game VkDevice and framegen VkDevice explicitly transfer EXTERNAL
    // ownership around every shared-image access, so this path is valid on
    // stock Android ICDs as well as wrapper/custom drivers.
    this->frame_0 = Mini::Image(info.device, info.physicalDevice,
        extent, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
        ahbTransportMode, LSFG::AhbImageRole::Input);
    this->frame_1 = Mini::Image(info.device, info.physicalDevice,
        extent, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
        ahbTransportMode, LSFG::AhbImageRole::Input);

    for (size_t i = 0; i < static_cast<size_t>(runtimeMultiplier - 1); ++i)
        this->out_n.emplace_back(info.device, info.physicalDevice,
            extent, format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
            ahbTransportMode, LSFG::AhbImageRole::Output);

    // Create framegen context using AHB sharing
    std::vector<AHardwareBuffer*> outAhbs;
    outAhbs.reserve(runtimeMultiplier - 1);
    for (size_t i = 0; i < static_cast<size_t>(runtimeMultiplier - 1); ++i)
        outAhbs.push_back(this->out_n.at(i).getAhb());

    int32_t ctxId;
    if (conf.adaptiveFlowScale) {
        try {
            if (conf.performance)
                ctxId = LSFG_3_1P::createAdaptiveContextFromAHB(
                    this->frame_0.getAhb(), this->frame_1.getAhb(),
                    outAhbs, extent, format, adaptiveFlowScales);
            else
                ctxId = LSFG_3_1::createAdaptiveContextFromAHB(
                    this->frame_0.getAhb(), this->frame_1.getAhb(),
                    outAhbs, extent, format, adaptiveFlowScales);
            this->adaptiveFlowRuntimeAvailable_ = true;
        } catch (const std::exception& e) {
            std::cerr << "lsfg-vk: adaptive-flow-fallback mode=fixed-target"
                      << " target=" << initialFlowScale
                      << " reason=" << e.what() << '\n';
            this->adaptiveFlowController_.configure(
                false, this->adaptiveFlowPreset_);
            if (conf.performance)
                ctxId = LSFG_3_1P::createContextFromAHB(
                    this->frame_0.getAhb(), this->frame_1.getAhb(),
                    outAhbs, extent, format);
            else
                ctxId = LSFG_3_1::createContextFromAHB(
                    this->frame_0.getAhb(), this->frame_1.getAhb(),
                    outAhbs, extent, format);
        }
    } else if (conf.performance) {
        ctxId = LSFG_3_1P::createContextFromAHB(
            this->frame_0.getAhb(), this->frame_1.getAhb(),
            outAhbs, extent, format);
    } else {
        ctxId = LSFG_3_1::createContextFromAHB(
            this->frame_0.getAhb(), this->frame_1.getAhb(),
            outAhbs, extent, format);
    }

    this->lsfgCtxId = std::shared_ptr<int32_t>(
        new int32_t(ctxId),
        [lsfgDeleteContext = lsfgDeleteContext](const int32_t* id) {
            lsfgDeleteContext(*id);
        }
    );

    unsetenv("DISABLE_LSFG"); // NOLINT

    // Resolve and allocate one handoff fence per swapchain context. It remains
    // authoritative for compatibility fallback and is also attached to async
    // source-copy submissions so their lifetime can be proven complete before
    // the next reset/reuse.
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

    const auto gameGetSemaphoreFd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        Layer::ovkGetDeviceProcAddr(info.device, "vkGetSemaphoreFdKHR"));
    this->asyncAhbHandoffEnabled_ =
        info.androidOpaqueFdSemaphoreSupported
        && backendDiagnostics.externalSemaphoreOpaqueFd
        && gameGetSemaphoreFd != nullptr;
    this->asyncFramegenCompletionEnabled_ = this->asyncAhbHandoffEnabled_;

    std::cerr << "lsfg-vk: Android AHB context created (id=" << ctxId
              << ", mode=" << LSFG::ahbTransportModeName(ahbTransportMode)
              << ", inputCopy=" << (LSFG::ahbInputCopyRequired(ahbTransportMode) ? 1 : 0)
              << ", outputCopy=" << (LSFG::ahbOutputCopyRequired(ahbTransportMode) ? 1 : 0)
              << ", handoff="
              << (this->asyncAhbHandoffEnabled_ ? "gpu-semaphore" : "host-fence")
              << ")\n";

#else
    // Desktop Linux path: use OPAQUE_FD-based image sharing

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

    // initialize lsfg
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

    // prepare render passes
    this->cmdPool = Mini::CommandPool(info.device, info.queue.first);
    for (size_t i = 0; i < 8; i++) {
        auto& pass = this->passInfos.at(i);
        pass.renderSemaphores.resize(runtimeMultiplier - 1);
        pass.acquireSemaphores.resize(runtimeMultiplier - 1);
        pass.postCopyBufs.resize(runtimeMultiplier - 1);
        pass.postCopySemaphores.resize(runtimeMultiplier - 1);
        pass.prevPostCopySemaphores.resize(runtimeMultiplier - 1);
    }
}

VkResult LsContext::present(const Hooks::DeviceInfo& info, const void* pNext, VkQueue queue,
        const std::vector<VkSemaphore>& gameRenderSemaphores, uint32_t presentIdx) {
    const auto& conf = Config::activeConf;
    auto& pass = this->passInfos.at(this->frameIdx % 8);

#ifdef __ANDROID__
    auto& metrics = this->runtimeMetrics;
    const auto cycleStart = RuntimeMetrics::Clock::now();
    bool excludeCurrentCycleFromTimingMetrics = false;
    this->adaptiveScheduler_.configure(
        conf.adaptiveFramegen ? conf.fpsLimit : 0,
        conf.multiplier > 1 ? static_cast<size_t>(conf.multiplier - 1) : 0);
    std::chrono::nanoseconds sourceInterval{};
    constexpr double kRuntimeTimingDiscontinuityMs = 250.0;
    if (metrics.hasLastSourcePresent) {
        sourceInterval = std::chrono::duration_cast<std::chrono::nanoseconds>(
            cycleStart - metrics.lastSourcePresent);
        const double sourceIntervalMs = std::chrono::duration<double, std::milli>(
            sourceInterval).count();
        if (sourceIntervalMs < kRuntimeTimingDiscontinuityMs) {
            metrics.windowSourceIntervalMs += sourceIntervalMs;
            if (sourceIntervalMs > metrics.windowSourceIntervalMaxMs)
                metrics.windowSourceIntervalMaxMs = sourceIntervalMs;
            metrics.windowSourceIntervals++;
        }
    }
    metrics.lastSourcePresent = cycleStart;
    metrics.hasLastSourcePresent = true;

    const uint64_t sourceArrivalNs = monotonicNowNs();
    const SourceTimelineCycle sourceCycle =
        this->sourceTimeline_.observe(sourceArrivalNs, sourceInterval);

    AdaptiveGenerationPlan adaptivePlan{};
    std::vector<float> interpolationPhases;
    size_t generatedFrameCount = 0;
    if (conf.adaptiveFramegen) {
        adaptivePlan = this->adaptiveScheduler_.planSlots(sourceInterval);
        interpolationPhases.reserve(adaptivePlan.slotPhases.size());
        for (const double phase : adaptivePlan.slotPhases) {
            // The framegen API intentionally rejects the real-frame endpoints.
            // Keep any floating conversion safely inside the open interval.
            const double bounded = std::clamp(phase, 0.000001, 0.999999);
            interpolationPhases.emplace_back(static_cast<float>(bounded));
        }
        generatedFrameCount = interpolationPhases.size();
    } else {
        generatedFrameCount = static_cast<size_t>(conf.multiplier - 1);
        interpolationPhases.reserve(generatedFrameCount);
        for (size_t i = 0; i < generatedFrameCount; ++i) {
            interpolationPhases.emplace_back(
                static_cast<float>(i + 1)
                / static_cast<float>(generatedFrameCount + 1));
        }
    }
    const auto& adaptiveTelemetry = this->adaptiveScheduler_.telemetry();

    metrics.windowSyntheticOpportunities += generatedFrameCount;
    metrics.totalSyntheticOpportunities += generatedFrameCount;

    // This value remains diagnostic only. Desired present times below are
    // derived directly from sourceCycle and never advanced by present calls.
    if (this->adaptiveDisplayTimingEnabled_ && sourceCycle.valid) {
        this->adaptivePresentPeriodNs_ = generatedFrameCount > 0
            ? sourceCycle.intervalNs / static_cast<uint64_t>(generatedFrameCount + 1)
            : sourceCycle.intervalNs;
    } else {
        this->adaptivePresentPeriodNs_ = 0;
    }

    const bool adaptiveZeroGeneration = conf.adaptiveFramegen && generatedFrameCount == 0;
    const bool warmupSourceHistory =
        generatedFrameCount > 0 && this->requiresSourceHistoryWarmup_;
    this->lastGeneratedFrameCount_ = generatedFrameCount;

    const auto updateAdaptiveFlowGovernor = [&]() {
        if (!conf.adaptiveFlowScale || !this->adaptiveFlowRuntimeAvailable_)
            return;

        const LSFG::AdaptiveFlowGpuTiming timing = conf.performance
            ? LSFG_3_1P::getContextGpuTiming(*this->lsfgCtxId)
            : LSFG_3_1::getContextGpuTiming(*this->lsfgCtxId);
        const double budgetMs = adaptiveFlowFrameBudgetMs(
            conf, sourceInterval, generatedFrameCount);
        const bool schedulerTransition =
            conf.adaptiveFramegen && adaptiveLsfgTransition(adaptiveTelemetry);
        const bool timingUsable = timing.valid && !timing.transitionActive;
        const bool budgetValid = budgetMs > 0.0 && std::isfinite(budgetMs);
        constexpr double kAdaptiveFlowCadenceDiscontinuityMs = 250.0;
        const double sourceIntervalMs =
            std::chrono::duration<double, std::milli>(sourceInterval).count();
        const bool cadenceDiscontinuity =
            sourceIntervalMs >= kAdaptiveFlowCadenceDiscontinuityMs;

        AdaptiveFlowObservation observation{
            .elapsed = sourceInterval,
            .frameBudgetMs = budgetMs,
            .totalLsfgMs = timingUsable ? timing.totalLsfgMs : 0.0,
            .flowMs = timingUsable ? timing.opticalFlowMs : 0.0,
            .mipmapsMs = timingUsable ? timing.mipmapsMs : 0.0,
            .generationCount = timing.valid
                ? timing.generationCount : generatedFrameCount,
            .deadlineMissed = timingUsable && budgetValid
                && timing.totalLsfgMs > budgetMs,
            .schedulerTransition = schedulerTransition,
            .valid = budgetValid && (schedulerTransition
                || (!cadenceDiscontinuity
                    && timingUsable && sourceInterval.count() > 0)),
        };

        const float previousScale = this->adaptiveFlowController_.currentScale();
        const float selectedScale =
            this->adaptiveFlowController_.observe(observation);
        const auto& flowTelemetry = this->adaptiveFlowController_.telemetry();

        if (flowTelemetry.changed
                && std::fabs(selectedScale - previousScale) > 0.0005F) {
            if (conf.performance)
                LSFG_3_1P::requestContextFlowScale(
                    *this->lsfgCtxId, selectedScale);
            else
                LSFG_3_1::requestContextFlowScale(
                    *this->lsfgCtxId, selectedScale);

            std::cerr << "lsfg-vk: adaptive-flow-decision"
                      << " previous=" << previousScale
                      << " requested=" << selectedScale
                      << " reason="
                      << AdaptiveFlowController::reasonName(flowTelemetry.reason)
                      << " mipmaps_ms=" << observation.mipmapsMs
                      << " flow_ms=" << observation.flowMs
                      << " lsfg_ms=" << observation.totalLsfgMs
                      << " budget_ms=" << observation.frameBudgetMs
                      << " generation_count=" << observation.generationCount
                      << '\n';
        }

        const LSFG::AdaptiveFlowContextState state = conf.performance
            ? LSFG_3_1P::getContextFlowScaleState(*this->lsfgCtxId)
            : LSFG_3_1::getContextFlowScaleState(*this->lsfgCtxId);
        this->adaptiveFlowRequestedScale_ = state.requestedScale;
        this->adaptiveFlowActiveScale_ = state.activeScale;
        this->adaptiveFlowWarmupRemaining_ = state.warmupRemaining;
        this->adaptiveFlowTransitionPending_ = state.transitionPending;
        this->adaptiveFlowTimingValid_ = timingUsable;
        this->adaptiveFlowMipmapsMs_ = timing.valid ? timing.mipmapsMs : 0.0;
        this->adaptiveFlowWorkMs_ = timing.valid ? timing.opticalFlowMs : 0.0;
        this->adaptiveFlowTotalLsfgMs_ = timing.valid ? timing.totalLsfgMs : 0.0;
        this->adaptiveFlowBudgetMs_ = budgetMs;
        this->adaptiveFlowGenerationCount_ =
            timing.valid ? timing.generationCount : generatedFrameCount;
        this->adaptiveFlowReason_ = flowTelemetry.reason;
    };

    if (conf.adaptiveFramegen) {
        if (adaptiveTelemetry.sourceRateSnapped) {
            metrics.windowAdaptiveRateSnaps++;
            metrics.totalAdaptiveRateSnaps++;
        }
        if (adaptiveTelemetry.costRaised) {
            metrics.windowAdaptiveCostRaises++;
            metrics.totalAdaptiveCostRaises++;
        }
        if (adaptiveTelemetry.costBackedOff) {
            metrics.windowAdaptiveCostBackoffs++;
            metrics.totalAdaptiveCostBackoffs++;
        }
        if (adaptiveTelemetry.costProbe) {
            metrics.windowAdaptiveCostProbes++;
            metrics.totalAdaptiveCostProbes++;
        }
        if (adaptiveTelemetry.discontinuityReset) {
            metrics.windowAdaptiveDiscontinuities++;
            metrics.totalAdaptiveDiscontinuities++;
        }

        if (adaptiveTelemetry.sourceRateSnapped || adaptiveTelemetry.costRaised
                || adaptiveTelemetry.costBackedOff || adaptiveTelemetry.costProbe
                || adaptiveTelemetry.discontinuityReset) {
            std::cerr << "lsfg-vk: adaptive-event"
                      << " source_fps=" << adaptiveTelemetry.sourceFps
                      << " smoothed_source_fps=" << adaptiveTelemetry.smoothedSourceFps
                      << " wanted_generated=" << adaptiveTelemetry.wantedGeneratedFrames
                      << " cost_limit=" << adaptiveTelemetry.costLimit
                      << " final_generated=" << adaptiveTelemetry.generatedFrames
                      << " rate_snap=" << (adaptiveTelemetry.sourceRateSnapped ? 1 : 0)
                      << " cost_raise=" << (adaptiveTelemetry.costRaised ? 1 : 0)
                      << " cost_backoff=" << (adaptiveTelemetry.costBackedOff ? 1 : 0)
                      << " cost_probe=" << (adaptiveTelemetry.costProbe ? 1 : 0)
                      << " discontinuity=" << (adaptiveTelemetry.discontinuityReset ? 1 : 0)
                      << "\n";
        }
    }

    const auto adaptivePresentPNext = [&](
            const void* downstream,
            uint64_t desiredPresentTimeNs,
            VkPresentTimeGOOGLE& presentTime,
            VkPresentTimesInfoGOOGLE& presentTimes) -> const void* {
        if (!this->adaptiveDisplayTimingEnabled_
                || !sourceCycle.valid
                || desiredPresentTimeNs == 0)
            return downstream;

        const uint64_t nowNs = monotonicNowNs();
        if (nowNs == 0 || desiredPresentTimeNs <= nowNs)
            return downstream;

        uint32_t presentId = this->adaptivePresentId_++;
        if (presentId == 0) {
            presentId = 1;
            this->adaptivePresentId_ = 2;
        }
        presentTime = VkPresentTimeGOOGLE{
            .presentID = presentId,
            .desiredPresentTime = desiredPresentTimeNs,
        };
        presentTimes = VkPresentTimesInfoGOOGLE{
            .sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE,
            .pNext = downstream,
            .swapchainCount = 1,
            .pTimes = &presentTime,
        };
        return &presentTimes;
    };

    const bool firstPresentDiagnostic = this->frameIdx == 0;
    if (firstPresentDiagnostic) {
        std::cerr << "lsfg-vk: runtime stage=first-present-enter image=" << presentIdx
                  << " multiplier=" << conf.multiplier
                  << " adaptive=" << (conf.adaptiveFramegen ? 1 : 0)
                  << " target_fps=" << conf.fpsLimit
                  << " performance=" << (conf.performance ? 1 : 0)
                  << " display_timing="
                  << (this->adaptiveDisplayTimingEnabled_ ? 1 : 0)
                  << " present_period_ns=" << this->adaptivePresentPeriodNs_
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
        if (cycleMs >= kRuntimeTimingDiscontinuityMs) {
            // Android can stop the guest while it is already inside this
            // present call. In that case sourceInterval was sampled before the
            // stop and looks normal, while host wall-clock dispatch/wait/cycle
            // timers absorb the entire pause. Drop the whole current metrics
            // window so a Quick Menu/suspend boundary cannot masquerade as a
            // multi-second GPU or frame-pacing stall.
            excludeCurrentCycleFromTimingMetrics = true;
            std::cerr << "lsfg-vk: runtime-timing-discontinuity"
                      << " cycle_ms=" << cycleMs
                      << " action=reset-window\n";
            metrics.windowStart = cycleEnd;
            metrics.windowSourceFrames = 0;
            metrics.windowGeneratedFrames = 0;
            metrics.windowSourcePresentFailures = 0;
            metrics.windowGeneratedPresentFailures = 0;
            metrics.windowAdaptiveZeroGenerationCycles = 0;
            metrics.windowAdaptiveRateSnaps = 0;
            metrics.windowAdaptiveCostRaises = 0;
            metrics.windowAdaptiveCostBackoffs = 0;
            metrics.windowAdaptiveCostProbes = 0;
            metrics.windowAdaptiveDiscontinuities = 0;
            metrics.windowAsyncHandoffs = 0;
            metrics.windowSyncHandoffs = 0;
            metrics.windowCycleMs = 0.0;
            metrics.windowCycleMaxMs = 0.0;
            metrics.windowHandoffMs = 0.0;
            metrics.windowDispatchMs = 0.0;
            metrics.windowWaitIdleMs = 0.0;
            metrics.windowGeneratedPresentMs = 0.0;
            metrics.windowSourceIntervalMs = 0.0;
            metrics.windowSourceIntervalMaxMs = 0.0;
            metrics.windowSourceDeadlineErrorMs = 0.0;
            metrics.windowSourceDeadlineErrorAbsMs = 0.0;
            metrics.windowSyntheticDeadlineErrorMs = 0.0;
            metrics.windowInterceptPresentMs = 0.0;
            metrics.windowSourceIntervals = 0;
            metrics.windowSourceDeadlineSamples = 0;
            metrics.windowSyntheticDeadlineSamples = 0;
            metrics.windowSyntheticOpportunities = 0;
            metrics.windowDeadlineShadowRejects = 0;
            metrics.windowDeadlineShadowLate = 0;
            metrics.windowDeadlinePredictionSamples = 0;
            metrics.windowPredictedLsfgMs = 0.0;
            metrics.windowActualLsfgMs = 0.0;
        }
        if (!excludeCurrentCycleFromTimingMetrics) {
            metrics.windowCycleMs += cycleMs;
            metrics.windowInterceptPresentMs += cycleMs;
            if (cycleMs > metrics.windowCycleMaxMs)
                metrics.windowCycleMaxMs = cycleMs;
            if (sourceCycle.valid) {
                const uint64_t finishNs = monotonicNowNs();
                if (finishNs > 0) {
                    const double deadlineErrorMs =
                        (static_cast<double>(finishNs)
                            - static_cast<double>(sourceCycle.sourceDeadlineNs))
                        / 1'000'000.0;
                    metrics.windowSourceDeadlineErrorMs += deadlineErrorMs;
                    metrics.windowSourceDeadlineErrorAbsMs +=
                        std::fabs(deadlineErrorMs);
                    metrics.windowSourceDeadlineSamples++;
                }
            }
        }

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
            const double sourceDeadlineErrorAvgMs =
                metrics.windowSourceDeadlineSamples > 0
                ? metrics.windowSourceDeadlineErrorMs
                    / static_cast<double>(metrics.windowSourceDeadlineSamples)
                : 0.0;
            const double sourceDeadlineErrorAbsAvgMs =
                metrics.windowSourceDeadlineSamples > 0
                ? metrics.windowSourceDeadlineErrorAbsMs
                    / static_cast<double>(metrics.windowSourceDeadlineSamples)
                : 0.0;
            const double syntheticDeadlineErrorAvgMs =
                metrics.windowSyntheticDeadlineSamples > 0
                ? metrics.windowSyntheticDeadlineErrorMs
                    / static_cast<double>(metrics.windowSyntheticDeadlineSamples)
                : 0.0;
            const double predictedLsfgAvgMs =
                metrics.windowDeadlinePredictionSamples > 0
                ? metrics.windowPredictedLsfgMs
                    / static_cast<double>(metrics.windowDeadlinePredictionSamples)
                : 0.0;
            const double actualLsfgAvgMs =
                metrics.windowDeadlinePredictionSamples > 0
                ? metrics.windowActualLsfgMs
                    / static_cast<double>(metrics.windowDeadlinePredictionSamples)
                : 0.0;
            const double interceptPresentAvgMs = sourceCount > 0.0
                ? metrics.windowInterceptPresentMs / sourceCount : 0.0;

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
                      << " ahb_async_handoffs=" << metrics.windowAsyncHandoffs
                      << " ahb_async_handoffs_total=" << metrics.totalAsyncHandoffs
                      << " ahb_sync_handoffs=" << metrics.windowSyncHandoffs
                      << " ahb_sync_handoffs_total=" << metrics.totalSyncHandoffs
                      << " ahb_async_fallbacks_total=" << metrics.totalAsyncFallbacks
                      << " framegen_dispatch_avg_ms=" << dispatchAvgMs
                      << " framegen_wait_avg_ms=" << waitIdleAvgMs
                      << " generated_present_avg_ms=" << generatedPresentAvgMs
                      << " source_interval_avg_ms=" << sourceIntervalAvgMs
                      << " source_interval_max_ms=" << metrics.windowSourceIntervalMaxMs
                      << " source_deadline_submit_error_avg_ms="
                      << sourceDeadlineErrorAvgMs
                      << " source_deadline_submit_error_abs_avg_ms="
                      << sourceDeadlineErrorAbsAvgMs
                      << " synthetic_deadline_submit_error_avg_ms="
                      << syntheticDeadlineErrorAvgMs
                      << " synthetic_opportunities="
                      << metrics.windowSyntheticOpportunities
                      << " synthetic_opportunities_total="
                      << metrics.totalSyntheticOpportunities
                      << " deadline_shadow_rejects="
                      << metrics.windowDeadlineShadowRejects
                      << " deadline_shadow_rejects_total="
                      << metrics.totalDeadlineShadowRejects
                      << " deadline_shadow_late="
                      << metrics.windowDeadlineShadowLate
                      << " deadline_shadow_late_total="
                      << metrics.totalDeadlineShadowLate
                      << " deadline_prediction_samples="
                      << metrics.windowDeadlinePredictionSamples
                      << " deadline_prediction_samples_total="
                      << metrics.totalDeadlinePredictionSamples
                      << " predicted_lsfg_avg_ms=" << predictedLsfgAvgMs
                      << " actual_lsfg_avg_ms=" << actualLsfgAvgMs
                      << " prediction_error_margin_ms="
                      << this->deadlinePositivePredictionErrorEwmaMs_
                      << " intercepted_present_avg_ms=" << interceptPresentAvgMs
                      << " generated_completion_sync=host-wait"
                      << " adaptive_source_fps=" << adaptiveTelemetry.sourceFps
                      << " adaptive_smoothed_source_fps=" << adaptiveTelemetry.smoothedSourceFps
                      << " adaptive_wanted_generated=" << adaptiveTelemetry.wantedGeneratedFrames
                      << " adaptive_governed_density="
                      << adaptiveTelemetry.governedGeneratedDensity
                      << " adaptive_fractional_phase="
                      << adaptiveTelemetry.fractionalPhase
                      << " adaptive_cost_limit=" << adaptiveTelemetry.costLimit
                      << " adaptive_final_generated=" << adaptiveTelemetry.generatedFrames
                      << " adaptive_zero_cycles=" << metrics.windowAdaptiveZeroGenerationCycles
                      << " adaptive_zero_cycles_total=" << metrics.totalAdaptiveZeroGenerationCycles
                      << " adaptive_rate_snaps=" << metrics.windowAdaptiveRateSnaps
                      << " adaptive_rate_snaps_total=" << metrics.totalAdaptiveRateSnaps
                      << " adaptive_cost_raises=" << metrics.windowAdaptiveCostRaises
                      << " adaptive_cost_raises_total=" << metrics.totalAdaptiveCostRaises
                      << " adaptive_cost_backoffs=" << metrics.windowAdaptiveCostBackoffs
                      << " adaptive_cost_backoffs_total=" << metrics.totalAdaptiveCostBackoffs
                      << " adaptive_cost_probes=" << metrics.windowAdaptiveCostProbes
                      << " adaptive_cost_probes_total=" << metrics.totalAdaptiveCostProbes
                      << " adaptive_discontinuities=" << metrics.windowAdaptiveDiscontinuities
                      << " adaptive_discontinuities_total=" << metrics.totalAdaptiveDiscontinuities
                      << " source_history_valid=" << (this->requiresSourceHistoryWarmup_ ? 0 : 1)
                      << " adaptive_flow_enabled=" << (this->adaptiveFlowRuntimeAvailable_ ? 1 : 0)
                       << " adaptive_flow_mode_requested=" << (conf.adaptiveFlowScale ? 1 : 0)
                      << " adaptive_flow_preset="
                      << AdaptiveFlowController::presetName(this->adaptiveFlowPreset_)
                      << " adaptive_flow_target="
                      << this->adaptiveFlowController_.telemetry().targetScale
                      << " adaptive_flow_minimum="
                      << this->adaptiveFlowController_.telemetry().minimumScale
                      << " adaptive_flow_requested=" << this->adaptiveFlowRequestedScale_
                      << " adaptive_flow_active=" << this->adaptiveFlowActiveScale_
                      << " adaptive_flow_transition="
                      << (this->adaptiveFlowTransitionPending_ ? 1 : 0)
                      << " adaptive_flow_warmup_remaining="
                      << this->adaptiveFlowWarmupRemaining_
                      << " adaptive_flow_timing_valid="
                      << (this->adaptiveFlowTimingValid_ ? 1 : 0)
                      << " adaptive_flow_mipmaps_ms=" << this->adaptiveFlowMipmapsMs_
                      << " adaptive_flow_work_ms=" << this->adaptiveFlowWorkMs_
                      << " adaptive_flow_lsfg_ms=" << this->adaptiveFlowTotalLsfgMs_
                      << " adaptive_flow_budget_ms=" << this->adaptiveFlowBudgetMs_
                      << " adaptive_flow_generation_count="
                      << this->adaptiveFlowGenerationCount_
                      << " adaptive_flow_reason="
                      << AdaptiveFlowController::reasonName(this->adaptiveFlowReason_)
                      << " adaptive_present_timing="
                      << (this->adaptiveDisplayTimingEnabled_ ? 1 : 0)
                      << " adaptive_present_period_ns="
                      << this->adaptivePresentPeriodNs_
                      << " multiplier=" << conf.multiplier
                      << " adaptive=" << (conf.adaptiveFramegen ? 1 : 0)
                      << " target_fps=" << conf.fpsLimit
                      << " performance=" << (conf.performance ? 1 : 0)
                      << "\n";

            metrics.windowStart = cycleEnd;
            metrics.windowSourceFrames = 0;
            metrics.windowGeneratedFrames = 0;
            metrics.windowSourcePresentFailures = 0;
            metrics.windowGeneratedPresentFailures = 0;
            metrics.windowAdaptiveZeroGenerationCycles = 0;
            metrics.windowAdaptiveRateSnaps = 0;
            metrics.windowAdaptiveCostRaises = 0;
            metrics.windowAdaptiveCostBackoffs = 0;
            metrics.windowAdaptiveCostProbes = 0;
            metrics.windowAdaptiveDiscontinuities = 0;
            metrics.windowAsyncHandoffs = 0;
            metrics.windowSyncHandoffs = 0;
            metrics.windowCycleMs = 0.0;
            metrics.windowCycleMaxMs = 0.0;
            metrics.windowHandoffMs = 0.0;
            metrics.windowDispatchMs = 0.0;
            metrics.windowWaitIdleMs = 0.0;
            metrics.windowGeneratedPresentMs = 0.0;
            metrics.windowSourceIntervalMs = 0.0;
            metrics.windowSourceIntervalMaxMs = 0.0;
            metrics.windowSourceDeadlineErrorMs = 0.0;
            metrics.windowSourceDeadlineErrorAbsMs = 0.0;
            metrics.windowSyntheticDeadlineErrorMs = 0.0;
            metrics.windowInterceptPresentMs = 0.0;
            metrics.windowSourceIntervals = 0;
            metrics.windowSourceDeadlineSamples = 0;
            metrics.windowSyntheticDeadlineSamples = 0;
            metrics.windowSyntheticOpportunities = 0;
            metrics.windowDeadlineShadowRejects = 0;
            metrics.windowDeadlineShadowLate = 0;
            metrics.windowDeadlinePredictionSamples = 0;
            metrics.windowPredictedLsfgMs = 0.0;
            metrics.windowActualLsfgMs = 0.0;
        }

        this->frameIdx++;
        return result;
    };

    // Android path: AHardwareBuffer exchange between two VkDevices. Keep the
    // validated presentation sequence and EXTERNAL ownership barriers intact.

    // 1. Copy every active Adaptive source frame into frame_0/frame_1, even on
    // a zero-generation cadence cycle. That zero is cadence, not lifecycle: it
    // must refresh temporal history instead of entering the Off/source-only path.
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

    // HistoryOnly uses the same dedicated cross-device input semaphore as
    // generated work when supported, avoiding an unnecessary source-copy host
    // wait. Source-history warm-up and unsupported/export-failure cases retain
    // the proven bounded host-fence fallback.
    bool useAsyncHandoff = this->asyncAhbHandoffEnabled_
        && !warmupSourceHistory;
    int framegenInputSemaphoreFd = -1;
    if (useAsyncHandoff) {
        try {
            pass.framegenInputSemaphore =
                Mini::Semaphore(info.device, &framegenInputSemaphoreFd);
            preCopySignals.emplace_back(pass.framegenInputSemaphore.handle());
        } catch (const std::exception& e) {
            this->asyncAhbHandoffEnabled_ = false;
            useAsyncHandoff = false;
            framegenInputSemaphoreFd = -1;
            metrics.totalAsyncFallbacks++;
            std::cerr << "lsfg-vk: Android async AHB handoff disabled after export failure: "
                      << e.what() << "; falling back to host fence\n";
        }
    }

    if (useAsyncHandoff) {
        submitAhbHandoff(info.device, pass.preCopyBuf, info.queue.second,
            gameRenderSemaphores2, preCopySignals,
            *this->ahbHandoffFence, this->resetHandoffFences);
        metrics.windowAsyncHandoffs++;
        metrics.totalAsyncHandoffs++;
    } else {
        submitAndWaitForAhbHandoff(info.device, pass.preCopyBuf, info.queue.second,
            gameRenderSemaphores2, preCopySignals,
            *this->ahbHandoffFence, this->resetHandoffFences,
            this->waitHandoffFences);
        metrics.windowSyncHandoffs++;
        metrics.totalSyncHandoffs++;
    }
    this->previousSourceCopySignalValid_ = true;
    metrics.windowHandoffMs += std::chrono::duration<double, std::milli>(
        RuntimeMetrics::Clock::now() - handoffStart).count();
    if (firstPresentDiagnostic) {
        std::cerr << "lsfg-vk: runtime stage=source-ahb-handoff-ready mode="
                  << (useAsyncHandoff ? "gpu-semaphore" : "host-fence") << "\n";
    }

    const auto advanceAdaptiveHistoryAndPresentSource =
        [&](const char* reason) -> VkResult {
            std::vector<int> noOutSems;
            const auto historyAdvanceStart = RuntimeMetrics::Clock::now();
            if (conf.performance)
                LSFG_3_1P::presentContextWithCount(
                    *this->lsfgCtxId, framegenInputSemaphoreFd, noOutSems, 0);
            else
                LSFG_3_1::presentContextWithCount(
                    *this->lsfgCtxId, framegenInputSemaphoreFd, noOutSems, 0);
            metrics.windowDispatchMs += std::chrono::duration<double, std::milli>(
                RuntimeMetrics::Clock::now() - historyAdvanceStart).count();
            updateAdaptiveFlowGovernor();
            metrics.windowAdaptiveZeroGenerationCycles++;
            metrics.totalAdaptiveZeroGenerationCycles++;
            this->requiresSourceHistoryWarmup_ = false;
            this->lastGeneratedFrameCount_ = 0;

            const VkSemaphore sourceReady = pass.preCopySemaphores.at(0).handle();
            VkPresentTimeGOOGLE adaptiveSourcePresentTime{};
            VkPresentTimesInfoGOOGLE adaptiveSourcePresentTimes{};
            const VkPresentInfoKHR adaptiveSourcePresentInfo{
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .pNext = adaptivePresentPNext(
                    pNext, sourceCycle.sourceDeadlineNs,
                    adaptiveSourcePresentTime, adaptiveSourcePresentTimes),
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &sourceReady,
                .swapchainCount = 1,
                .pSwapchains = &this->swapchain,
                .pImageIndices = &presentIdx,
            };
            const auto adaptiveSourceResult = Layer::ovkQueuePresentKHR(
                queue, &adaptiveSourcePresentInfo);
            if (adaptiveSourceResult != VK_SUCCESS
                    && adaptiveSourceResult != VK_SUBOPTIMAL_KHR) {
                metrics.windowSourcePresentFailures++;
                metrics.totalSourcePresentFailures++;
                throw LSFG::vulkan_error(adaptiveSourceResult,
                    "Failed to present Adaptive history-only source frame");
            }
            if (firstPresentDiagnostic || adaptiveTelemetry.discontinuityReset
                    || reason != nullptr) {
                std::cerr << "lsfg-vk: runtime stage=adaptive-history-advance"
                          << " generated=0 history_valid=1"
                          << " reason=" << (reason != nullptr ? reason : "cadence")
                          << " discontinuity="
                          << (adaptiveTelemetry.discontinuityReset ? 1 : 0)
                          << "\n";
            }
            return finishSourcePresent(
                adaptiveSourceResult, "pre-copy-adaptive-history");
        };

    if (adaptiveZeroGeneration)
        return advanceAdaptiveHistoryAndPresentSource("scheduler-zero");

    if (warmupSourceHistory) {
        this->requiresSourceHistoryWarmup_ = false;
        this->lastGeneratedFrameCount_ = 0;
        const VkSemaphore sourceReady = pass.preCopySemaphores.at(0).handle();
        VkPresentTimeGOOGLE warmupPresentTime{};
        VkPresentTimesInfoGOOGLE warmupPresentTimes{};
        const VkPresentInfoKHR warmupPresentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = adaptivePresentPNext(
                pNext, sourceCycle.sourceDeadlineNs,
                warmupPresentTime, warmupPresentTimes),
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

    // 2. Deadline admission is the fast source-protection gate. The scheduler
    // has already consumed these fractional opportunities, so rejected/late
    // slots are dropped permanently and never become catch-up debt.
    std::vector<double> deadlinePhases;
    deadlinePhases.reserve(interpolationPhases.size());
    for (const float phase : interpolationPhases)
        deadlinePhases.emplace_back(static_cast<double>(phase));

    double predictedSharedCostMs = 0.0;
    double predictedPerSyntheticCostMs = 0.0;
    const bool detailedPrediction =
        this->adaptiveFlowTimingValid_
        && this->adaptiveFlowGenerationCount_ > 0
        && this->adaptiveFlowTotalLsfgMs_ > 0.0
        && this->adaptiveFlowWorkMs_ > 0.0
        && this->adaptiveFlowTotalLsfgMs_ >= this->adaptiveFlowWorkMs_;
    if (detailedPrediction) {
        predictedSharedCostMs = this->adaptiveFlowWorkMs_;
        predictedPerSyntheticCostMs =
            (this->adaptiveFlowTotalLsfgMs_ - this->adaptiveFlowWorkMs_)
            / static_cast<double>(this->adaptiveFlowGenerationCount_);
    } else if (this->deadlineHostCostValid_) {
        const double previousCount = static_cast<double>(
            std::max<size_t>(1, this->deadlineHostCostGenerationCount_));
        const double requestedCount = static_cast<double>(
            std::max<size_t>(1, generatedFrameCount));
        predictedSharedCostMs = this->deadlineHostCostEwmaMs_
            * std::max(1.0, requestedCount / previousCount);
    }

    const SyntheticDeadlineAdmissionPlan deadlineAdmissionPlan =
        SyntheticDeadlineAdmission::evaluate(
            monotonicNowNs(),
            sourceCycle,
            deadlinePhases,
            predictedSharedCostMs,
            predictedPerSyntheticCostMs,
            this->deadlinePositivePredictionErrorEwmaMs_);
    metrics.windowDeadlineShadowRejects += deadlineAdmissionPlan.rejectedCount;
    metrics.totalDeadlineShadowRejects += deadlineAdmissionPlan.rejectedCount;

    double deadlinePredictedTotalMs = 0.0;
    if (deadlineAdmissionPlan.predictionValid
            && !deadlineAdmissionPlan.slots.empty()) {
        deadlinePredictedTotalMs =
            deadlineAdmissionPlan.slots.back().predictedCompletionMs;
        metrics.windowPredictedLsfgMs += deadlinePredictedTotalMs;
        metrics.windowDeadlinePredictionSamples++;
        metrics.totalDeadlinePredictionSamples++;
    }

    if (conf.adaptiveFramegen && !deadlineAdmissionPlan.slots.empty()) {
        std::vector<float> admittedPhases;
        admittedPhases.reserve(interpolationPhases.size());
        for (size_t i = 0; i < deadlineAdmissionPlan.slots.size(); ++i) {
            const auto& slot = deadlineAdmissionPlan.slots.at(i);
            if (slot.admitted)
                admittedPhases.emplace_back(interpolationPhases.at(i));
        }
        interpolationPhases = std::move(admittedPhases);
        generatedFrameCount = interpolationPhases.size();
        this->lastGeneratedFrameCount_ = generatedFrameCount;

        if (generatedFrameCount == 0)
            return advanceAdaptiveHistoryAndPresentSource("deadline-reject");
    }

    // Tell framegen to generate intermediary frames. The Adaptive fast path
    // exports one game-device binary semaphore per output and imports it into
    // framegen, so post-copy can wait on GPU completion without a host stall.
    std::vector<int> noOutSems;
    std::vector<int> renderSemaphoreFds;
    bool useAsyncFramegenCompletion =
        conf.adaptiveFramegen
        && this->asyncFramegenCompletionEnabled_
        && deadlineAdmissionPlan.predictionValid
        && this->deadlineHostCostSamples_ >= 2;
    if (useAsyncFramegenCompletion) {
        renderSemaphoreFds.resize(generatedFrameCount, -1);
        try {
            for (size_t i = 0; i < generatedFrameCount; ++i) {
                pass.renderSemaphores.at(i) =
                    Mini::Semaphore(info.device, &renderSemaphoreFds.at(i));
            }
        } catch (const std::exception& e) {
            this->asyncFramegenCompletionEnabled_ = false;
            useAsyncFramegenCompletion = false;
            renderSemaphoreFds.clear();
            metrics.totalAsyncFallbacks++;
            std::cerr << "lsfg-vk: Adaptive framegen completion semaphore disabled: "
                      << e.what() << "; using bounded host completion wait\n";
        }
    }
    if (firstPresentDiagnostic) {
        std::cerr << "lsfg-vk: runtime stage=framegen-dispatch-begin mode="
                  << (conf.performance ? "performance" : "quality")
                  << " generated=" << generatedFrameCount
                  << " handoff=" << (useAsyncHandoff ? "gpu-semaphore" : "host-fence")
                  << "\n";
    }
    const auto dispatchStart = RuntimeMetrics::Clock::now();
    if (conf.adaptiveFramegen) {
        if (conf.performance)
            LSFG_3_1P::presentContextWithPhases(
                *this->lsfgCtxId, framegenInputSemaphoreFd,
                useAsyncFramegenCompletion ? renderSemaphoreFds : noOutSems,
                interpolationPhases);
        else
            LSFG_3_1::presentContextWithPhases(
                *this->lsfgCtxId, framegenInputSemaphoreFd,
                useAsyncFramegenCompletion ? renderSemaphoreFds : noOutSems,
                interpolationPhases);
    } else if (conf.performance) {
        LSFG_3_1P::presentContextWithCount(
            *this->lsfgCtxId, framegenInputSemaphoreFd, noOutSems, generatedFrameCount);
    } else {
        LSFG_3_1::presentContextWithCount(
            *this->lsfgCtxId, framegenInputSemaphoreFd, noOutSems, generatedFrameCount);
    }
    metrics.windowDispatchMs += std::chrono::duration<double, std::milli>(
        RuntimeMetrics::Clock::now() - dispatchStart).count();
    if (firstPresentDiagnostic)
        std::cerr << "lsfg-vk: runtime stage=framegen-dispatch-returned\n";

    // 3. Source-protected Adaptive completion. Once calibration is valid,
    // generated-output release is ordered entirely by cross-device binary
    // semaphores and this present thread does not wait for framegen. Fixed mode,
    // calibration cycles, and capability failures keep the proven bounded wait.
    constexpr double kCostEwmaAlpha = 0.20;
    if (!useAsyncFramegenCompletion) {
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
            const auto timeoutPresentResult =
                Layer::ovkQueuePresentKHR(queue, &timeoutPresentInfo);
            if (timeoutPresentResult != VK_SUCCESS
                    && timeoutPresentResult != VK_SUBOPTIMAL_KHR) {
                metrics.windowSourcePresentFailures++;
                metrics.totalSourcePresentFailures++;
                throw LSFG::vulkan_error(timeoutPresentResult,
                    "Failed to present source frame after framegen timeout");
            }
            return finishSourcePresent(
                VK_ERROR_OUT_OF_DATE_KHR, "pre-copy-timeout");
        }
        if (firstPresentDiagnostic)
            std::cerr << "lsfg-vk: runtime stage=framegen-idle-ready\n";

        const auto completionObservedAt = RuntimeMetrics::Clock::now();
        const double actualCompletionMs =
            std::chrono::duration<double, std::milli>(
                completionObservedAt - dispatchStart).count();
        if (!this->deadlineHostCostValid_) {
            this->deadlineHostCostEwmaMs_ = actualCompletionMs;
            this->deadlineHostCostValid_ = true;
        } else {
            this->deadlineHostCostEwmaMs_ += kCostEwmaAlpha
                * (actualCompletionMs - this->deadlineHostCostEwmaMs_);
        }
        this->deadlineHostCostGenerationCount_ = generatedFrameCount;
        this->deadlineHostCostSamples_++;

        if (deadlineAdmissionPlan.predictionValid) {
            metrics.windowActualLsfgMs += actualCompletionMs;
            const double positiveErrorMs =
                std::max(0.0, actualCompletionMs - deadlinePredictedTotalMs);
            this->deadlinePositivePredictionErrorEwmaMs_ += kCostEwmaAlpha
                * (positiveErrorMs - this->deadlinePositivePredictionErrorEwmaMs_);
        }

        const uint64_t completionObservedNs = monotonicNowNs();
        if (completionObservedNs > 0) {
            uint64_t lateSlots = 0;
            for (const auto& slot : deadlineAdmissionPlan.slots) {
                if (slot.deadlineNs > 0 && completionObservedNs > slot.deadlineNs)
                    ++lateSlots;
            }
            metrics.windowDeadlineShadowLate += lateSlots;
            metrics.totalDeadlineShadowLate += lateSlots;
        }

        if (firstPresentDiagnostic && deadlineAdmissionPlan.predictionValid) {
            std::cerr << "lsfg-vk: runtime stage=deadline-calibration"
                      << " predicted_total_ms=" << deadlinePredictedTotalMs
                      << " actual_total_ms=" << actualCompletionMs
                      << " rejected=" << deadlineAdmissionPlan.rejectedCount
                      << " prediction_error_margin_ms="
                      << this->deadlinePositivePredictionErrorEwmaMs_
                      << "\n";
        }
    } else if (firstPresentDiagnostic) {
        std::cerr << "lsfg-vk: runtime stage=framegen-completion-async"
                  << " generated=" << generatedFrameCount
                  << " rejected=" << deadlineAdmissionPlan.rejectedCount
                  << "\n";
    }

    updateAdaptiveFlowGovernor();

    // 4. Copy generated frames to swapchain images and present them. Each
    // copy submission signals two binary semaphores: one consumed by this
    // generated present, and one reserved for the next generated/source
    // present. A binary semaphore signal must not be consumed twice.
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
        std::vector<VkSemaphore> postCopyWaitSemaphores{
            pass.acquireSemaphores.at(i).handle()
        };
        if (useAsyncFramegenCompletion)
            postCopyWaitSemaphores.emplace_back(
                pass.renderSemaphores.at(i).handle());
        pass.postCopyBufs.at(i).submit(info.queue.second,
            postCopyWaitSemaphores,
            { pass.postCopySemaphores.at(i).handle(),
              pass.prevPostCopySemaphores.at(i).handle() });

        std::vector<VkSemaphore> waitSemaphores{ pass.postCopySemaphores.at(i).handle() };
        if (i != 0) waitSemaphores.emplace_back(pass.prevPostCopySemaphores.at(i - 1).handle());

        VkPresentTimeGOOGLE generatedPresentTime{};
        VkPresentTimesInfoGOOGLE generatedPresentTimes{};
        const void* generatedDownstreamPNext = i == 0 ? pNext : nullptr;
        const uint64_t generatedDesiredPresentTimeNs =
            i < interpolationPhases.size()
            ? sourceCycle.syntheticDeadlineNs(interpolationPhases.at(i))
            : 0;
        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = adaptivePresentPNext(
                generatedDownstreamPNext,
                generatedDesiredPresentTimeNs,
                generatedPresentTime,
                generatedPresentTimes),
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
        if (sourceCycle.valid && generatedDesiredPresentTimeNs > 0) {
            const uint64_t submittedNs = monotonicNowNs();
            if (submittedNs > 0) {
                const double errorMs =
                    (static_cast<double>(submittedNs)
                        - static_cast<double>(generatedDesiredPresentTimeNs))
                    / 1'000'000.0;
                metrics.windowSyntheticDeadlineErrorMs += errorMs;
                metrics.windowSyntheticDeadlineSamples++;
            }
        }
        if (firstPresentDiagnostic && i == 0) {
            std::cerr << "lsfg-vk: runtime stage=generated-present-ready image=" << imageIdx
                      << " result=" << res << "\n";
        }
    }

    // 5. Present the actual game frame after generated frames using the signal
    // reserved for this present, rather than waiting a second time on the
    // generated-present semaphore.
    VkSemaphore lastPrevPostCopySemaphore = generatedFrameCount > 0
        ? pass.prevPostCopySemaphores.at(generatedFrameCount - 1).handle()
        : pass.preCopySemaphores.at(0).handle();
    VkPresentTimeGOOGLE finalSourcePresentTime{};
    VkPresentTimesInfoGOOGLE finalSourcePresentTimes{};
    const void* finalSourceDownstreamPNext =
        generatedFrameCount == 0 ? pNext : nullptr;
    const VkPresentInfoKHR finalPresentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = adaptivePresentPNext(
            finalSourceDownstreamPNext,
            sourceCycle.sourceDeadlineNs,
            finalSourcePresentTime,
            finalSourcePresentTimes),
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
    // Desktop Linux path: OPAQUE_FD semaphore-based synchronization

    // 1. copy swapchain image to frame_0/frame_1
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

    // 2. render intermediary frames
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
        // 3. acquire next swapchain image
        pass.acquireSemaphores.at(i) = Mini::Semaphore(info.device);
        uint32_t imageIdx{};
        auto res = Layer::ovkAcquireNextImageKHR(info.device, this->swapchain, UINT64_MAX,
            pass.acquireSemaphores.at(i).handle(), VK_NULL_HANDLE, &imageIdx);
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
            throw LSFG::vulkan_error(res, "Failed to acquire next swapchain image");

        // 4. copy output image to swapchain image
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

        // 5. present swapchain image
        std::vector<VkSemaphore> waitSemaphores{ pass.postCopySemaphores.at(i).handle() };
        if (i != 0) waitSemaphores.emplace_back(pass.prevPostCopySemaphores.at(i - 1).handle());

        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = i == 0 ? pNext : nullptr, // only set on first present
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

    // 6. present actual next frame
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

#ifdef __ANDROID__
void LsContext::enterSourceOnlyBypass() {
    this->lastGeneratedFrameCount_ = 0;
    this->requiresSourceHistoryWarmup_ = true;
    this->previousSourceCopySignalValid_ = false;
    this->sourceTimeline_.reset();
    this->adaptiveScheduler_.reset();
    this->deadlineHostCostValid_ = false;
    this->deadlineHostCostEwmaMs_ = 0.0;
    this->deadlineHostCostGenerationCount_ = 0;
    this->deadlinePositivePredictionErrorEwmaMs_ = 0.0;
}
#endif

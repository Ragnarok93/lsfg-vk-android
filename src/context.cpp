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
#include <unistd.h>
#endif

#include <vulkan/vulkan_core.h>
#include <lsfg_3_1.hpp>
#include <lsfg_3_1p.hpp>

#include <filesystem>
#include <fstream>
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

const char* handoffTypeName(VkExternalSemaphoreHandleTypeFlagBits handleType) {
    if (handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT)
        return "sync-fd";
    if (handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT)
        return "opaque-fd";
    return "unknown-fd";
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

struct RuntimePressureSample {
    bool valid{false};
    double gpuUsagePercent{0.0};
    double outputFps{0.0};
    double frameTimeP95Ms{0.0};
    double slowFrameRatio{0.0};
};

RuntimePressureSample readRuntimePressure(
        const std::filesystem::path& configFile) {
    RuntimePressureSample sample{};
    if (configFile.empty())
        return sample;

    const auto path = configFile.parent_path() / "runtime-pressure.txt";
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(path, ec);
    if (ec)
        return sample;

    const auto age = std::filesystem::file_time_type::clock::now() - modified;
    if (age < std::filesystem::file_time_type::duration::zero()
            || age > std::chrono::seconds(2))
        return sample;

    std::ifstream input(path);
    if (!input)
        return sample;

    bool sawGpu = false;
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos)
            continue;
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        try {
            const double parsed = std::stod(value);
            if (!std::isfinite(parsed))
                continue;
            if (key == "gpu_usage_percent") {
                sample.gpuUsagePercent = parsed;
                sawGpu = true;
            } else if (key == "output_fps") {
                sample.outputFps = parsed;
            } else if (key == "frame_time_p95_ms") {
                sample.frameTimeP95Ms = parsed;
            } else if (key == "slow_frame_ratio") {
                sample.slowFrameRatio = parsed;
            }
        } catch (const std::exception&) {
            continue;
        }
    }

    sample.valid =
        sawGpu
        && sample.gpuUsagePercent >= 0.0
        && sample.gpuUsagePercent <= 100.0
        && sample.outputFps >= 0.0
        && sample.frameTimeP95Ms >= 0.0
        && sample.slowFrameRatio >= 0.0
        && sample.slowFrameRatio <= 1.0;
    return sample;
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
    if (fence == VK_NULL_HANDLE) {
        commandBuffer.submit(queue, waitSemaphores, signalSemaphores);
        return;
    }
    if (resetFences == nullptr)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Android AHB handoff fence reset is unavailable");

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
    // Restore the known-good Android WSI contract first. Display-timing hints
    // were introduced together with the Adaptive FIFO override and are kept
    // dormant until their pacing behavior can be validated independently.
    this->adaptiveDisplayTimingEnabled_ = false;
    std::cerr << "lsfg-vk: adaptive-present-pacing"
              << " fifo_override=0"
              << " display_timing=0"
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
    const auto gameImportSemaphoreFd = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
        Layer::ovkGetDeviceProcAddr(info.device, "vkImportSemaphoreFdKHR"));
    const bool syncFdHandoffSupported =
        info.androidSyncFdSemaphoreSupported
        && backendDiagnostics.externalSemaphoreSyncFd;
    const bool opaqueFdHandoffSupported =
        info.androidOpaqueFdSemaphoreSupported
        && backendDiagnostics.externalSemaphoreOpaqueFd;
    this->asyncAhbHandoffEnabled_ =
        gameGetSemaphoreFd != nullptr
        && (syncFdHandoffSupported || opaqueFdHandoffSupported);
    this->asyncAhbHandoffHandleType_ = syncFdHandoffSupported
        ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
        : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    this->asyncFramegenCompletionEnabled_ =
        syncFdHandoffSupported && gameImportSemaphoreFd != nullptr;

    std::cerr << "lsfg-vk: Android AHB context created (id=" << ctxId
              << ", mode=" << LSFG::ahbTransportModeName(ahbTransportMode)
              << ", inputCopy=" << (LSFG::ahbInputCopyRequired(ahbTransportMode) ? 1 : 0)
              << ", outputCopy=" << (LSFG::ahbOutputCopyRequired(ahbTransportMode) ? 1 : 0)
              << ", handoff="
              << (this->asyncAhbHandoffEnabled_
                    ? handoffTypeName(this->asyncAhbHandoffHandleType_)
                    : "host-fence")
              << ", completion="
              << (this->asyncFramegenCompletionEnabled_ ? "sync-fd" : "host-wait")
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
    const size_t requestedFixedGeneratedFrameCount =
        static_cast<size_t>(conf.multiplier - 1);
    const size_t plannedGeneratedFrameCount = conf.adaptiveFramegen
        ? this->adaptiveScheduler_.plan(sourceInterval)
        : requestedFixedGeneratedFrameCount;
    size_t generatedFrameCount = plannedGeneratedFrameCount;
    size_t interpolationGenerationCount = plannedGeneratedFrameCount;
    const auto& adaptiveTelemetry = this->adaptiveScheduler_.telemetry();

    const uint64_t sourceArrivalTimeNs = monotonicNowNs();
    // Scheduler discontinuities are cadence-relative. Do not reinterpret a
    // legitimately slow source as a timing failure through an absolute FPS
    // threshold here.
    const bool sourceTimelineDiscontinuity =
        conf.adaptiveFramegen && adaptiveTelemetry.discontinuityReset;

    if (sourceTimelineDiscontinuity) {
        this->sourceHistoryWarmupRemaining_ = kSourceHistoryWarmupFrames;
        this->requiresSourceHistoryWarmup_ = true;
        this->deadlineAdmissionPredictor_.reset();
        this->lastDispatchedGeneratedFrameCount_ = 0;
        this->sourceTimeline_.reset();
        this->currentSourceTimeline_ = {};
        this->adaptivePresentPeriodNs_ = 0;
    } else {
        const bool hadValidSourceTimeline = this->currentSourceTimeline_.valid;
        this->currentSourceTimeline_ = this->sourceTimeline_.observe(
            sourceArrivalTimeNs, sourceInterval, false);
        if (hadValidSourceTimeline
                && !this->currentSourceTimeline_.valid
                && sourceInterval.count() > 0) {
            this->sourceHistoryWarmupRemaining_ = kSourceHistoryWarmupFrames;
            this->requiresSourceHistoryWarmup_ = true;
            this->deadlineAdmissionPredictor_.reset();
            this->lastDispatchedGeneratedFrameCount_ = 0;
        }
        if (this->currentSourceTimeline_.valid) {
            if (this->currentSourceTimeline_.sourceIndex > 0) {
                const double deadlineErrorMs = std::abs(
                    static_cast<double>(
                        this->currentSourceTimeline_.sourceDeadlineErrorNs))
                    / 1'000'000.0;
                metrics.windowSourceDeadlineErrorAbsMs += deadlineErrorMs;
                metrics.windowSourceDeadlineErrorMaxMs = std::max(
                    metrics.windowSourceDeadlineErrorMaxMs, deadlineErrorMs);
                metrics.windowSourceDeadlineSamples++;
                if (this->currentSourceTimeline_.rebased)
                    metrics.windowSourceTimelineRebases++;
            }
        } else {
            this->adaptivePresentPeriodNs_ = 0;
        }
    }

    const bool sourceHistoryWarmupActive =
        this->requiresSourceHistoryWarmup_
        && this->sourceHistoryWarmupRemaining_ > 0;

    // Active deadline admission: generation is subordinate to the protected
    // source timeline. Use measured GPU cost to choose the largest evenly
    // distributed synthetic count whose prefixes can meet their own slots.
    // A rejected opportunity is dropped, never accumulated as catch-up debt.
    this->deadlineBatchDecision_ = {};
    if (conf.adaptiveFramegen
            && !sourceHistoryWarmupActive
            && generatedFrameCount > 0
            && this->currentSourceTimeline_.valid) {
        const uint64_t admissionNowNs = monotonicNowNs();
        if (admissionNowNs > 0) {
            if (this->currentSourceTimeline_.sourceDesiredTimeNs <= admissionNowNs) {
                metrics.windowGeneratedLateDrops += generatedFrameCount;
                metrics.totalGeneratedLateDrops += generatedFrameCount;
                metrics.windowAdmissionRejects += generatedFrameCount;
                metrics.totalAdmissionRejects += generatedFrameCount;
                generatedFrameCount = 0;
            } else {
                const double sourceBudgetMs =
                    static_cast<double>(
                        this->currentSourceTimeline_.sourceDesiredTimeNs - admissionNowNs)
                    / 1'000'000.0;
                const auto plannedBatchDecision =
                    this->deadlineAdmissionPredictor_.predict(
                        generatedFrameCount, sourceBudgetMs);

                // Preserve the historical opportunity counters as predictor
                // diagnostics, but the decision below is now authoritative.
                if (!plannedBatchDecision.valid && generatedFrameCount > 1) {
                    const size_t rejectedGeneratedFrameCount =
                        generatedFrameCount - 1;
                    metrics.windowGeneratedLateDrops +=
                        rejectedGeneratedFrameCount;
                    metrics.totalGeneratedLateDrops +=
                        rejectedGeneratedFrameCount;
                    metrics.windowAdmissionRejects +=
                        rejectedGeneratedFrameCount;
                    metrics.totalAdmissionRejects +=
                        rejectedGeneratedFrameCount;
                    generatedFrameCount = 1;
                } else if (plannedBatchDecision.valid) {
                    for (size_t slot = 0; slot < generatedFrameCount; ++slot) {
                        const double interpolationFraction =
                            static_cast<double>(slot + 1)
                            / static_cast<double>(interpolationGenerationCount + 1);
                        const uint64_t slotDeadlineNs =
                            this->sourceTimeline_.syntheticDesiredTimeNs(
                                this->currentSourceTimeline_, interpolationFraction);
                        const double slotBudgetMs =
                            slotDeadlineNs > admissionNowNs
                                ? static_cast<double>(slotDeadlineNs - admissionNowNs)
                                    / 1'000'000.0
                                : 0.0;
                        const auto slotDecision =
                            this->deadlineAdmissionPredictor_.predict(
                                slot + 1, slotBudgetMs);
                        if (!slotDecision.valid)
                            continue;

                        ++metrics.windowDeadlineShadowOpportunities;
                        ++metrics.totalDeadlineShadowOpportunities;
                        if (slotDecision.wouldAdmit) {
                            ++metrics.windowDeadlineShadowWouldAdmit;
                        } else {
                            ++metrics.windowDeadlineShadowWouldReject;
                            ++metrics.totalDeadlineShadowWouldReject;
                        }
                    }

                    size_t admittedGeneratedFrameCount = 0;
                    for (size_t candidate = generatedFrameCount;
                            candidate > 0; --candidate) {
                        bool candidateFits = true;
                        for (size_t slot = 0; slot < candidate; ++slot) {
                            // Admission occurs before dispatch. Evaluate each
                            // candidate using the spacing it would actually use
                            // so a 2 -> 1 reduction tests a midpoint rather than
                            // retaining a prefix-biased 1/3 position.
                            const double interpolationFraction =
                                static_cast<double>(slot + 1)
                                / static_cast<double>(candidate + 1);
                            const uint64_t slotDeadlineNs =
                                this->sourceTimeline_.syntheticDesiredTimeNs(
                                    this->currentSourceTimeline_,
                                    interpolationFraction);
                            const double slotBudgetMs =
                                slotDeadlineNs > admissionNowNs
                                    ? static_cast<double>(
                                        slotDeadlineNs - admissionNowNs)
                                        / 1'000'000.0
                                    : 0.0;
                            const auto slotDecision =
                                this->deadlineAdmissionPredictor_.predict(
                                    slot + 1, slotBudgetMs);
                            if (!slotDecision.valid || !slotDecision.wouldAdmit) {
                                candidateFits = false;
                                break;
                            }
                        }
                        if (candidateFits) {
                            admittedGeneratedFrameCount = candidate;
                            break;
                        }
                    }

                    if (admittedGeneratedFrameCount < generatedFrameCount) {
                        const size_t rejectedGeneratedFrameCount =
                            generatedFrameCount - admittedGeneratedFrameCount;
                        metrics.windowGeneratedLateDrops +=
                            rejectedGeneratedFrameCount;
                        metrics.totalGeneratedLateDrops +=
                            rejectedGeneratedFrameCount;
                        metrics.windowAdmissionRejects +=
                            rejectedGeneratedFrameCount;
                        metrics.totalAdmissionRejects +=
                            rejectedGeneratedFrameCount;
                        generatedFrameCount = admittedGeneratedFrameCount;
                    }

                    if (generatedFrameCount > 0) {
                        this->deadlineBatchDecision_ =
                            this->deadlineAdmissionPredictor_.predict(
                                generatedFrameCount, sourceBudgetMs);
                    }
                }
            }
        }
    }

    // Admission is complete before any framegen dispatch. Re-space the
    // surviving batch evenly across the protected source interval; rejected
    // opportunities are consumed and never become catch-up debt.
    interpolationGenerationCount = generatedFrameCount;

    if (this->currentSourceTimeline_.valid) {
        const size_t timingGenerationCount =
            generatedFrameCount > 0 ? interpolationGenerationCount : 0;
        this->adaptivePresentPeriodNs_ =
            this->currentSourceTimeline_.intervalNs
            / static_cast<uint64_t>(timingGenerationCount + 1);
    } else {
        this->adaptivePresentPeriodNs_ = 0;
    }

    enum class AndroidFrameCycleMode {
        Generate,
        HistoryOnly,
    };
    const bool historyOnly =
        sourceHistoryWarmupActive
        || plannedGeneratedFrameCount == 0
        || (plannedGeneratedFrameCount > 0 && generatedFrameCount == 0);
    const AndroidFrameCycleMode cycleMode =
        historyOnly
            ? AndroidFrameCycleMode::HistoryOnly
            : AndroidFrameCycleMode::Generate;
    this->lastGeneratedFrameCount_ = historyOnly ? 0 : generatedFrameCount;

    const auto updateAdaptiveFlowGovernor = [&]() {
        const LSFG::AdaptiveFlowGpuTiming timing = conf.performance
            ? LSFG_3_1P::getContextGpuTiming(*this->lsfgCtxId)
            : LSFG_3_1::getContextGpuTiming(*this->lsfgCtxId);
        const bool timingUsable = timing.valid && !timing.transitionActive;
        const bool generatedWorkSample =
            timingUsable && timing.generationCount > 0;

        if (generatedWorkSample) {
            this->adaptiveFlowGeneratedTimingValid_ = true;
            this->adaptiveFlowRetainedMipmapsMs_ = timing.mipmapsMs;
            this->adaptiveFlowRetainedWorkMs_ = timing.opticalFlowMs;
            this->adaptiveFlowRetainedTotalLsfgMs_ = timing.totalLsfgMs;
            this->adaptiveFlowRetainedGenerationCount_ = timing.generationCount;

            if (this->deadlineBatchDecision_.valid) {
                metrics.windowDeadlinePredictionAbsErrorMs += std::abs(
                    timing.totalLsfgMs
                    - this->deadlineBatchDecision_.predictedTotalLsfgMs);
                ++metrics.windowDeadlinePredictionSamples;
            }
            this->deadlineAdmissionPredictor_.observe(
                DeadlineAdmissionObservation{
                    .mipmapsMs = timing.mipmapsMs,
                    .opticalFlowMs = timing.opticalFlowMs,
                    .totalLsfgMs = timing.totalLsfgMs,
                    .generationCount = timing.generationCount,
                    .valid = true,
                });
        }

        if (!conf.adaptiveFlowScale || !this->adaptiveFlowRuntimeAvailable_)
            return;

        const auto pressureNow = std::chrono::steady_clock::now();
        if (this->adaptiveFlowNextPressureRead_.time_since_epoch().count() == 0
                || pressureNow >= this->adaptiveFlowNextPressureRead_) {
            this->adaptiveFlowNextPressureRead_ =
                pressureNow + std::chrono::milliseconds(500);
            const auto pressure = readRuntimePressure(conf.config_file);
            this->adaptiveFlowGlobalPressureValid_ = pressure.valid;
            this->adaptiveFlowGlobalGpuUsagePercent_ =
                pressure.valid ? pressure.gpuUsagePercent : 0.0;
            this->adaptiveFlowGlobalOutputFps_ =
                pressure.valid ? pressure.outputFps : 0.0;
            this->adaptiveFlowGlobalFrameTimeP95Ms_ =
                pressure.valid ? pressure.frameTimeP95Ms : 0.0;
            this->adaptiveFlowGlobalSlowFrameRatio_ =
                pressure.valid ? pressure.slowFrameRatio : 0.0;
        }

        const double budgetMs = adaptiveFlowFrameBudgetMs(
            conf, sourceInterval, generatedFrameCount);
        const bool schedulerTransition =
            conf.adaptiveFramegen && adaptiveLsfgTransition(adaptiveTelemetry);
        const bool budgetValid = budgetMs > 0.0 && std::isfinite(budgetMs);
        constexpr double kAdaptiveFlowCadenceDiscontinuityMs = 250.0;
        const double sourceIntervalMs =
            std::chrono::duration<double, std::milli>(sourceInterval).count();
        const bool cadenceDiscontinuity =
            sourceIntervalMs >= kAdaptiveFlowCadenceDiscontinuityMs;

        const bool adaptiveOutputDeficit =
            conf.adaptiveFramegen
            && conf.fpsLimit > 0
            && this->adaptiveFlowGlobalPressureValid_
            && this->adaptiveFlowGlobalOutputFps_ > 0.0
            && this->adaptiveFlowGlobalOutputFps_
                < static_cast<double>(conf.fpsLimit) * 0.97;
        const bool fixedOutputDeficit =
            !conf.adaptiveFramegen
            && this->adaptiveFlowGlobalPressureValid_
            && (this->adaptiveFlowGlobalSlowFrameRatio_ >= 0.08
                || (budgetValid
                    && this->adaptiveFlowGlobalFrameTimeP95Ms_
                        > budgetMs * 1.25));
        const bool outputDeficit =
            adaptiveOutputDeficit || fixedOutputDeficit;

        const bool syntheticDropPressure =
            metrics.totalGeneratedLateDrops
                > this->adaptiveFlowLastObservedLateDrops_;
        this->adaptiveFlowLastObservedLateDrops_ =
            metrics.totalGeneratedLateDrops;
        this->adaptiveFlowSyntheticDropPressure_ = syntheticDropPressure;

        const bool retainedTimingUsable =
            this->adaptiveFlowGeneratedTimingValid_
            && this->adaptiveFlowRetainedGenerationCount_ > 0;
        const double observationMipmapsMs = generatedWorkSample
            ? timing.mipmapsMs : this->adaptiveFlowRetainedMipmapsMs_;
        const double observationFlowMs = generatedWorkSample
            ? timing.opticalFlowMs : this->adaptiveFlowRetainedWorkMs_;
        const double observationTotalMs = generatedWorkSample
            ? timing.totalLsfgMs : this->adaptiveFlowRetainedTotalLsfgMs_;
        const size_t observationGenerationCount = generatedWorkSample
            ? timing.generationCount : this->adaptiveFlowRetainedGenerationCount_;

        AdaptiveFlowObservation observation{
            .elapsed = sourceInterval,
            .frameBudgetMs = budgetMs,
            .totalLsfgMs = observationTotalMs,
            .flowMs = observationFlowMs,
            .mipmapsMs = observationMipmapsMs,
            .generationCount = observationGenerationCount,
            .deadlineMissed = generatedWorkSample && budgetValid
                && timing.totalLsfgMs > budgetMs,
            .globalGpuUsagePercent =
                this->adaptiveFlowGlobalGpuUsagePercent_,
            .globalPressureValid =
                this->adaptiveFlowGlobalPressureValid_,
            .outputDeficit = outputDeficit,
            .syntheticDropPressure = syntheticDropPressure,
            .generatedWorkSample = generatedWorkSample,
            .schedulerTransition = schedulerTransition,
            .valid = budgetValid
                && !cadenceDiscontinuity
                && sourceInterval.count() > 0
                && retainedTimingUsable,
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
                      << " generated_work_sample="
                      << (observation.generatedWorkSample ? 1 : 0)
                      << " global_gpu_percent="
                      << observation.globalGpuUsagePercent
                      << " global_pressure_valid="
                      << (observation.globalPressureValid ? 1 : 0)
                      << " output_deficit="
                      << (observation.outputDeficit ? 1 : 0)
                      << " synthetic_drop_pressure="
                      << (observation.syntheticDropPressure ? 1 : 0)
                      << '\n';
        }

        const LSFG::AdaptiveFlowContextState state = conf.performance
            ? LSFG_3_1P::getContextFlowScaleState(*this->lsfgCtxId)
            : LSFG_3_1::getContextFlowScaleState(*this->lsfgCtxId);
        this->adaptiveFlowRequestedScale_ = state.requestedScale;
        this->adaptiveFlowActiveScale_ = state.activeScale;
        this->adaptiveFlowWarmupRemaining_ = state.warmupRemaining;
        this->adaptiveFlowTransitionPending_ = state.transitionPending;
        this->adaptiveFlowTimingValid_ = retainedTimingUsable;
        this->adaptiveFlowMipmapsMs_ =
            retainedTimingUsable ? observationMipmapsMs : 0.0;
        this->adaptiveFlowWorkMs_ =
            retainedTimingUsable ? observationFlowMs : 0.0;
        this->adaptiveFlowTotalLsfgMs_ =
            retainedTimingUsable ? observationTotalMs : 0.0;
        this->adaptiveFlowBudgetMs_ = budgetMs;
        this->adaptiveFlowGenerationCount_ =
            retainedTimingUsable ? observationGenerationCount : 0;
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
        if (!this->adaptiveDisplayTimingEnabled_ || desiredPresentTimeNs == 0)
            return downstream;

        const uint64_t nowNs = monotonicNowNs();
        if (nowNs == 0)
            return downstream;

        // VK_GOOGLE_display_timing only supplies a "not earlier than" hint.
        // Once a source-anchored slot is already late, do not invent a new
        // generated-driven deadline: present it as soon as the WSI permits.
        const uint64_t effectiveDesiredTimeNs =
            desiredPresentTimeNs > nowNs ? desiredPresentTimeNs : 0;

        uint32_t presentId = this->adaptivePresentId_++;
        if (presentId == 0) {
            presentId = 1;
            this->adaptivePresentId_ = 2;
        }
        presentTime = VkPresentTimeGOOGLE{
            .presentID = presentId,
            .desiredPresentTime = effectiveDesiredTimeNs,
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
                  << " source_timeline_valid="
                  << (this->currentSourceTimeline_.valid ? 1 : 0)
                  << " source_index=" << this->currentSourceTimeline_.sourceIndex
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
            metrics.windowGeneratedLateDrops = 0;
            metrics.windowAdmissionRejects = 0;
            metrics.windowGeneratedDeadlineDrops = 0;
            metrics.windowGeneratedWsiDrops = 0;
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
            metrics.windowDeadlineShadowOpportunities = 0;
            metrics.windowDeadlineShadowWouldAdmit = 0;
            metrics.windowDeadlineShadowWouldReject = 0;
            metrics.windowCycleMs = 0.0;
            metrics.windowCycleMaxMs = 0.0;
            metrics.windowHandoffMs = 0.0;
            metrics.windowDispatchMs = 0.0;
            metrics.windowWaitIdleMs = 0.0;
            metrics.windowGeneratedPresentMs = 0.0;
            metrics.windowSourceIntervalMs = 0.0;
            metrics.windowSourceIntervalMaxMs = 0.0;
            metrics.windowSourceDeadlineErrorAbsMs = 0.0;
            metrics.windowSourceDeadlineErrorMaxMs = 0.0;
            metrics.windowDeadlinePredictionAbsErrorMs = 0.0;
            metrics.windowDeadlinePredictionSamples = 0;
            metrics.windowSourceIntervals = 0;
            metrics.windowSourceDeadlineSamples = 0;
            metrics.windowSourceTimelineRebases = 0;
        }
        if (!excludeCurrentCycleFromTimingMetrics) {
            metrics.windowCycleMs += cycleMs;
            if (cycleMs > metrics.windowCycleMaxMs)
                metrics.windowCycleMaxMs = cycleMs;
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
                    ? metrics.windowSourceDeadlineErrorAbsMs
                        / static_cast<double>(metrics.windowSourceDeadlineSamples)
                    : 0.0;
            const double deadlinePredictionErrorAvgMs =
                metrics.windowDeadlinePredictionSamples > 0
                    ? metrics.windowDeadlinePredictionAbsErrorMs
                        / static_cast<double>(metrics.windowDeadlinePredictionSamples)
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
                      << " generated_late_drops=" << metrics.windowGeneratedLateDrops
                      << " generated_late_drops_total=" << metrics.totalGeneratedLateDrops
                      << " admission_rejects=" << metrics.windowAdmissionRejects
                      << " admission_rejects_total=" << metrics.totalAdmissionRejects
                      << " generated_deadline_drops="
                      << metrics.windowGeneratedDeadlineDrops
                      << " generated_deadline_drops_total="
                      << metrics.totalGeneratedDeadlineDrops
                      << " generated_wsi_drops=" << metrics.windowGeneratedWsiDrops
                      << " generated_wsi_drops_total=" << metrics.totalGeneratedWsiDrops
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
                      << " source_deadline_error_avg_ms=" << sourceDeadlineErrorAvgMs
                      << " source_deadline_error_max_ms="
                      << metrics.windowSourceDeadlineErrorMaxMs
                      << " source_timeline_rebases="
                      << metrics.windowSourceTimelineRebases
                      << " source_timeline_index="
                      << this->currentSourceTimeline_.sourceIndex
                      << " deadline_admission_valid="
                      << (this->deadlineBatchDecision_.valid ? 1 : 0)
                      << " deadline_planned_generated="
                      << plannedGeneratedFrameCount
                      << " deadline_admitted_generated="
                      << generatedFrameCount
                      << " interpolation_denominator="
                      << interpolationGenerationCount
                      << " deadline_pred_mipmaps_ms="
                      << this->deadlineBatchDecision_.predictedMipmapsMs
                      << " deadline_pred_flow_ms="
                      << this->deadlineBatchDecision_.predictedOpticalFlowMs
                      << " deadline_pred_total_ms="
                      << this->deadlineBatchDecision_.predictedTotalLsfgMs
                      << " deadline_safety_margin_ms="
                      << this->deadlineBatchDecision_.safetyMarginMs
                      << " deadline_delivery_reserve_ms="
                      << this->deadlineBatchDecision_.deliveryReserveMs
                      << " deadline_usable_budget_ms="
                      << this->deadlineBatchDecision_.usableBudgetMs
                      << " deadline_effective_budget_ms="
                      << this->deadlineBatchDecision_.effectiveUsableBudgetMs
                      << " deadline_batch_admit="
                      << (this->deadlineBatchDecision_.wouldAdmit ? 1 : 0)
                      << " deadline_shadow_opportunities="
                      << metrics.windowDeadlineShadowOpportunities
                      << " deadline_shadow_would_admit="
                      << metrics.windowDeadlineShadowWouldAdmit
                      << " deadline_shadow_would_reject="
                      << metrics.windowDeadlineShadowWouldReject
                      << " deadline_shadow_would_reject_total="
                      << metrics.totalDeadlineShadowWouldReject
                      << " deadline_prediction_error_avg_ms="
                      << deadlinePredictionErrorAvgMs
                      << " deadline_prediction_samples="
                      << metrics.windowDeadlinePredictionSamples
                      << " fixed_requested_generated="
                      << requestedFixedGeneratedFrameCount
                      << " adaptive_source_fps=" << adaptiveTelemetry.sourceFps
                      << " adaptive_smoothed_source_fps=" << adaptiveTelemetry.smoothedSourceFps
                      << " adaptive_wanted_generated=" << adaptiveTelemetry.wantedGeneratedFrames
                      << " adaptive_cost_limit=" << adaptiveTelemetry.costLimit
                      << " adaptive_final_generated=" << adaptiveTelemetry.generatedFrames
                      << " adaptive_fractional_phase=" << adaptiveTelemetry.fractionalPhase
                      << " adaptive_synthetic_opportunities="
                      << adaptiveTelemetry.syntheticOpportunitiesCreated
                      << " history_only_cycles=" << metrics.windowAdaptiveZeroGenerationCycles
                      << " history_only_cycles_total=" << metrics.totalAdaptiveZeroGenerationCycles
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
                      << " source_history_warmup_remaining="
                      << this->sourceHistoryWarmupRemaining_
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
                      << " adaptive_flow_global_pressure_valid="
                      << (this->adaptiveFlowGlobalPressureValid_ ? 1 : 0)
                      << " adaptive_flow_global_gpu_percent="
                      << this->adaptiveFlowGlobalGpuUsagePercent_
                      << " adaptive_flow_global_output_fps="
                      << this->adaptiveFlowGlobalOutputFps_
                      << " adaptive_flow_global_p95_ms="
                      << this->adaptiveFlowGlobalFrameTimeP95Ms_
                      << " adaptive_flow_global_slow_ratio="
                      << this->adaptiveFlowGlobalSlowFrameRatio_
                      << " adaptive_flow_global_pressure="
                      << (this->adaptiveFlowController_.telemetry().globalPressure ? 1 : 0)
                      << " adaptive_flow_output_deficit="
                      << (this->adaptiveFlowController_.telemetry().outputDeficit ? 1 : 0)
                      << " adaptive_flow_synthetic_drop_pressure="
                      << (this->adaptiveFlowSyntheticDropPressure_ ? 1 : 0)
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
            metrics.windowGeneratedLateDrops = 0;
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
            metrics.windowDeadlineShadowOpportunities = 0;
            metrics.windowDeadlineShadowWouldAdmit = 0;
            metrics.windowDeadlineShadowWouldReject = 0;
            metrics.windowCycleMs = 0.0;
            metrics.windowCycleMaxMs = 0.0;
            metrics.windowHandoffMs = 0.0;
            metrics.windowDispatchMs = 0.0;
            metrics.windowWaitIdleMs = 0.0;
            metrics.windowGeneratedPresentMs = 0.0;
            metrics.windowSourceIntervalMs = 0.0;
            metrics.windowSourceIntervalMaxMs = 0.0;
            metrics.windowSourceDeadlineErrorAbsMs = 0.0;
            metrics.windowSourceDeadlineErrorMaxMs = 0.0;
            metrics.windowDeadlinePredictionAbsErrorMs = 0.0;
            metrics.windowDeadlinePredictionSamples = 0;
            metrics.windowSourceIntervals = 0;
            metrics.windowSourceDeadlineSamples = 0;
            metrics.windowSourceTimelineRebases = 0;
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

    // Framegen owns a two-image source pair and some AHB transport modes acquire
    // both images even during zero-count preprocessing. Define both images on
    // the first source frame; the second slot is overwritten by the next real
    // source before interpolation is permitted after the three-frame warmup.
    if (this->frameIdx == 0) {
        copySwapchainToExternalAhb(pass.preCopyBuf.handle(),
            this->swapchainImages.at(presentIdx),
            this->frame_1.handle(),
            this->extent.width, this->extent.height,
            info.queue.first, true);
    }

    pass.preCopyBuf.end();

    std::vector<VkSemaphore> gameRenderSemaphores2 = gameRenderSemaphores;
    RenderPassInfo* previousPass = nullptr;
    bool consumePreviousBatchComplete = false;
    if (this->frameIdx > 0)
        previousPass = &this->passInfos.at((this->frameIdx - 1) % 8);
    if (this->previousSourceCopySignalValid_ && previousPass != nullptr)
        gameRenderSemaphores2.emplace_back(
            previousPass->preCopySemaphores.at(1).handle());
    if (previousPass != nullptr && previousPass->framegenBatchCompleteValid) {
        gameRenderSemaphores2.emplace_back(
            previousPass->framegenBatchCompleteSemaphore.handle());
        consumePreviousBatchComplete = true;
    }

    const auto handoffStart = RuntimeMetrics::Clock::now();
    std::vector<VkSemaphore> preCopySignals{
        pass.preCopySemaphores.at(0).handle(),
        pass.preCopySemaphores.at(1).handle(),
    };

    // Every enabled cycle submits either interpolation or zero-count history
    // preprocessing. Prefer SYNC_FD for both so the source thread never needs a
    // host wait in the normal path.
    bool useAsyncHandoff = this->asyncAhbHandoffEnabled_;
    bool asyncSubmissionIssued = false;
    bool asyncExportFailed = false;
    int framegenInputSemaphoreFd = -1;

    if (useAsyncHandoff) {
        try {
            pass.framegenInputSemaphore = Mini::Semaphore(
                info.device, this->asyncAhbHandoffHandleType_);
            preCopySignals.emplace_back(pass.framegenInputSemaphore.handle());
        } catch (const std::exception& e) {
            this->asyncAhbHandoffEnabled_ = false;
            useAsyncHandoff = false;
            metrics.totalAsyncFallbacks++;
            std::cerr << "lsfg-vk: Android async AHB handoff disabled after "
                      << handoffTypeName(this->asyncAhbHandoffHandleType_)
                      << " semaphore creation failure: " << e.what()
                      << "; falling back to host fence\n";
        }
    }

    if (useAsyncHandoff) {
        submitAhbHandoff(info.device, pass.preCopyBuf, info.queue.second,
            gameRenderSemaphores2, preCopySignals,
            VK_NULL_HANDLE, nullptr);
        asyncSubmissionIssued = true;
        if (consumePreviousBatchComplete && previousPass != nullptr)
            previousPass->framegenBatchCompleteValid = false;

        try {
            // SYNC_FD copy transference requires its signal operation to be
            // submitted before vkGetSemaphoreFdKHR. OPAQUE_FD is also valid here.
            framegenInputSemaphoreFd = pass.framegenInputSemaphore.exportFd(
                info.device, this->asyncAhbHandoffHandleType_);
            metrics.windowAsyncHandoffs++;
            metrics.totalAsyncHandoffs++;
        } catch (const std::exception& e) {
            // The copy was already submitted without a reusable fence. Do not
            // dispatch framegen unsynchronized and do not block the source
            // thread. Present the real frame from the source-copy signal and
            // require one history warmup before generation resumes.
            this->asyncAhbHandoffEnabled_ = false;
            useAsyncHandoff = false;
            asyncExportFailed = true;
            framegenInputSemaphoreFd = -1;
            metrics.totalAsyncFallbacks++;
            std::cerr << "lsfg-vk: Android async AHB handoff disabled after "
                      << handoffTypeName(this->asyncAhbHandoffHandleType_)
                      << " export failure: " << e.what()
                      << "; failing open to source-only cycle\n";
        }
    }

    if (!useAsyncHandoff && !asyncSubmissionIssued) {
        submitAndWaitForAhbHandoff(info.device, pass.preCopyBuf, info.queue.second,
            gameRenderSemaphores2, preCopySignals,
            *this->ahbHandoffFence, this->resetHandoffFences,
            this->waitHandoffFences);
        if (consumePreviousBatchComplete && previousPass != nullptr)
            previousPass->framegenBatchCompleteValid = false;
        metrics.windowSyncHandoffs++;
        metrics.totalSyncHandoffs++;
    }
    this->previousSourceCopySignalValid_ = true;
    metrics.windowHandoffMs += std::chrono::duration<double, std::milli>(
        RuntimeMetrics::Clock::now() - handoffStart).count();
    if (firstPresentDiagnostic) {
        std::cerr << "lsfg-vk: runtime stage=source-ahb-handoff-ready mode="
                  << (useAsyncHandoff
                        ? handoffTypeName(this->asyncAhbHandoffHandleType_)
                        : "host-fence")
                  << "\n";
    }

    if (asyncExportFailed) {
        this->lastDispatchedGeneratedFrameCount_ = 0;
        this->sourceHistoryWarmupRemaining_ = kSourceHistoryWarmupFrames;
        this->requiresSourceHistoryWarmup_ = true;
        this->lastGeneratedFrameCount_ = 0;
        const VkSemaphore sourceReady = pass.preCopySemaphores.at(0).handle();
        const VkPresentInfoKHR failOpenPresentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = pNext,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &sourceReady,
            .swapchainCount = 1,
            .pSwapchains = &this->swapchain,
            .pImageIndices = &presentIdx,
        };
        const auto failOpenResult =
            Layer::ovkQueuePresentKHR(queue, &failOpenPresentInfo);
        if (failOpenResult != VK_SUCCESS
                && failOpenResult != VK_SUBOPTIMAL_KHR) {
            metrics.windowSourcePresentFailures++;
            metrics.totalSourcePresentFailures++;
            throw LSFG::vulkan_error(
                failOpenResult, "Failed source present after SYNC_FD export failure");
        }
        // The game-side source copy submission advanced while framegen did
        // not consume this source. Recreate the LSFG/swapchain context after
        // presenting the real frame so the two temporal indices cannot remain
        // permanently offset.
        return finishSourcePresent(
            VK_ERROR_OUT_OF_DATE_KHR, "pre-copy-syncfd-fail-open-recreate");
    }

    if (historyOnly) {
        this->lastDispatchedGeneratedFrameCount_ = 0;
        // Zero-generation cadence still refreshes mipmaps/alpha history, but it
        // must not stall the real source. On the normal SYNC_FD path framegen
        // exports one batch-complete dependency after preprocessing and AHB
        // release; the next source copy consumes it before reusing the inputs.
        std::vector<int> noOutSems;
        LSFG::AndroidFrameSyncFds historySync{};
        bool historyRequiresHostCompletionWait = false;
        const auto historyAdvanceStart = RuntimeMetrics::Clock::now();

        if (this->asyncFramegenCompletionEnabled_ && useAsyncHandoff) {
            historySync = conf.performance
                ? LSFG_3_1P::presentContextWithCountExportSyncFd(
                    *this->lsfgCtxId, framegenInputSemaphoreFd, 0,
                    this->asyncAhbHandoffHandleType_)
                : LSFG_3_1::presentContextWithCountExportSyncFd(
                    *this->lsfgCtxId, framegenInputSemaphoreFd, 0,
                    this->asyncAhbHandoffHandleType_);

            for (const int fd : historySync.outputReadyFds)
                if (fd >= 0) ::close(fd);

            if (historySync.gpuDependenciesExported) {
                try {
                    if (historySync.batchCompleteFd >= 0) {
                        pass.framegenBatchCompleteSemaphore = Mini::Semaphore(
                            info.device, historySync.batchCompleteFd,
                            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
                        historySync.batchCompleteFd = -1;
                        pass.framegenBatchCompleteValid = true;
                    } else {
                        pass.framegenBatchCompleteValid = false;
                    }
                } catch (const std::exception& e) {
                    if (historySync.batchCompleteFd >= 0)
                        ::close(historySync.batchCompleteFd);
                    historySync.batchCompleteFd = -1;
                    pass.framegenBatchCompleteValid = false;
                    historyRequiresHostCompletionWait = true;
                    this->asyncFramegenCompletionEnabled_ = false;
                    std::cerr << "lsfg-vk: zero-history completion SYNC_FD import failed: "
                              << e.what() << "; using bounded host fallback\n";
                }
            } else if (historySync.hostWaitFallback) {
                pass.framegenBatchCompleteValid = false;
            } else {
                pass.framegenBatchCompleteValid = false;
                historyRequiresHostCompletionWait = true;
                this->asyncFramegenCompletionEnabled_ = false;
            }
        } else {
            pass.framegenBatchCompleteValid = false;
            if (conf.performance)
                LSFG_3_1P::presentContextWithCount(
                    *this->lsfgCtxId,
                    useAsyncHandoff ? framegenInputSemaphoreFd : -1,
                    noOutSems, 0,
                    useAsyncHandoff
                        ? this->asyncAhbHandoffHandleType_
                        : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
            else
                LSFG_3_1::presentContextWithCount(
                    *this->lsfgCtxId,
                    useAsyncHandoff ? framegenInputSemaphoreFd : -1,
                    noOutSems, 0,
                    useAsyncHandoff
                        ? this->asyncAhbHandoffHandleType_
                        : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
        }

        if (historyRequiresHostCompletionWait) {
            const auto historyWaitStart = RuntimeMetrics::Clock::now();
            const uint64_t historyTimeoutNs = runtimeWaitTimeoutNs();
            const bool historyReady = conf.performance
                ? LSFG_3_1P::waitContext(*this->lsfgCtxId, historyTimeoutNs)
                : LSFG_3_1::waitContext(*this->lsfgCtxId, historyTimeoutNs);
            metrics.windowWaitIdleMs += std::chrono::duration<double, std::milli>(
                RuntimeMetrics::Clock::now() - historyWaitStart).count();
            if (!historyReady) {
                this->sourceHistoryWarmupRemaining_ =
                    kSourceHistoryWarmupFrames;
                this->requiresSourceHistoryWarmup_ = true;
                this->lastGeneratedFrameCount_ = 0;
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
                const auto timeoutResult =
                    Layer::ovkQueuePresentKHR(queue, &timeoutPresentInfo);
                if (timeoutResult != VK_SUCCESS
                        && timeoutResult != VK_SUBOPTIMAL_KHR) {
                    metrics.windowSourcePresentFailures++;
                    metrics.totalSourcePresentFailures++;
                    throw LSFG::vulkan_error(
                        timeoutResult,
                        "Failed source present after zero-history completion timeout");
                }
                return finishSourcePresent(
                    VK_ERROR_OUT_OF_DATE_KHR, "history-completion-timeout");
            }
        }

        metrics.windowDispatchMs += std::chrono::duration<double, std::milli>(
            RuntimeMetrics::Clock::now() - historyAdvanceStart).count();
        updateAdaptiveFlowGovernor();
        metrics.windowAdaptiveZeroGenerationCycles++;
        metrics.totalAdaptiveZeroGenerationCycles++;
        if (this->sourceHistoryWarmupRemaining_ > 0)
            --this->sourceHistoryWarmupRemaining_;
        this->requiresSourceHistoryWarmup_ =
            this->sourceHistoryWarmupRemaining_ > 0;
        this->lastGeneratedFrameCount_ = 0;

        const VkSemaphore sourceReady = pass.preCopySemaphores.at(0).handle();
        VkPresentTimeGOOGLE adaptiveSourcePresentTime{};
        VkPresentTimesInfoGOOGLE adaptiveSourcePresentTimes{};
        const VkPresentInfoKHR adaptiveSourcePresentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = adaptivePresentPNext(
                pNext,
                this->currentSourceTimeline_.sourceDesiredTimeNs,
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
                "Failed to present zero-generation source frame");
        }
        if (firstPresentDiagnostic || adaptiveTelemetry.discontinuityReset) {
            std::cerr << "lsfg-vk: runtime stage=history-only"
                      << " generated=0 history_valid="
                      << (this->requiresSourceHistoryWarmup_ ? 0 : 1)
                      << " async_completion="
                      << (pass.framegenBatchCompleteValid ? 1 : 0)
                      << " history_warmup_remaining="
                      << this->sourceHistoryWarmupRemaining_
                      << " discontinuity="
                      << (adaptiveTelemetry.discontinuityReset ? 1 : 0)
                      << "\n";
        }
        return finishSourcePresent(adaptiveSourceResult, "pre-copy-history-only");
    }


    this->lastDispatchedGeneratedFrameCount_ = generatedFrameCount;

    // 2. Tell framegen to generate intermediary frames. The normal Android
    //    path exports output-ready and batch-complete SYNC_FDs after submission,
    //    allowing game-device work to queue without a host completion wait.
    std::vector<int> noOutSems;
    std::vector<bool> outputReadyWaitValid(generatedFrameCount, false);
    LSFG::AndroidFrameSyncFds framegenSync{};
    if (firstPresentDiagnostic) {
        std::cerr << "lsfg-vk: runtime stage=framegen-dispatch-begin mode="
                  << (conf.performance ? "performance" : "quality")
                  << " generated=" << generatedFrameCount
                  << " handoff=" << (useAsyncHandoff
                        ? handoffTypeName(this->asyncAhbHandoffHandleType_)
                        : "host-fence")
                  << "\n";
    }
    const auto dispatchStart = RuntimeMetrics::Clock::now();
    if (this->asyncFramegenCompletionEnabled_) {
        framegenSync = conf.performance
            ? LSFG_3_1P::presentContextWithCountExportSyncFd(
                *this->lsfgCtxId, framegenInputSemaphoreFd,
                generatedFrameCount, this->asyncAhbHandoffHandleType_,
                interpolationGenerationCount)
            : LSFG_3_1::presentContextWithCountExportSyncFd(
                *this->lsfgCtxId, framegenInputSemaphoreFd,
                generatedFrameCount, this->asyncAhbHandoffHandleType_,
                interpolationGenerationCount);
    } else if (conf.performance) {
        LSFG_3_1P::presentContextWithCount(
            *this->lsfgCtxId, framegenInputSemaphoreFd, noOutSems,
            generatedFrameCount, this->asyncAhbHandoffHandleType_,
                interpolationGenerationCount);
    } else {
        LSFG_3_1::presentContextWithCount(
            *this->lsfgCtxId, framegenInputSemaphoreFd, noOutSems,
            generatedFrameCount, this->asyncAhbHandoffHandleType_,
                interpolationGenerationCount);
    }
    metrics.windowDispatchMs += std::chrono::duration<double, std::milli>(
        RuntimeMetrics::Clock::now() - dispatchStart).count();
    if (firstPresentDiagnostic)
        std::cerr << "lsfg-vk: runtime stage=framegen-dispatch-returned"
                  << " completion="
                  << (framegenSync.gpuDependenciesExported ? "sync-fd"
                      : (framegenSync.hostWaitFallback ? "host-fallback"
                          : "host-wait"))
                  << "\n";

    bool requireHostCompletionWait = !this->asyncFramegenCompletionEnabled_;
    if (this->asyncFramegenCompletionEnabled_
            && framegenSync.gpuDependenciesExported) {
        bool importFailed = framegenSync.outputReadyFds.size() != generatedFrameCount;
        try {
            if (!importFailed) {
                for (size_t i = 0; i < generatedFrameCount; ++i) {
                    const int fd = framegenSync.outputReadyFds.at(i);
                    if (fd < 0)
                        continue;
                    pass.renderSemaphores.at(i) = Mini::Semaphore(
                        info.device, fd,
                        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
                    framegenSync.outputReadyFds.at(i) = -1;
                    outputReadyWaitValid.at(i) = true;
                }
                if (framegenSync.batchCompleteFd >= 0) {
                    pass.framegenBatchCompleteSemaphore = Mini::Semaphore(
                        info.device, framegenSync.batchCompleteFd,
                        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
                    framegenSync.batchCompleteFd = -1;
                    pass.framegenBatchCompleteValid = true;
                } else {
                    pass.framegenBatchCompleteValid = false;
                }
            }
        } catch (const std::exception& e) {
            importFailed = true;
            std::cerr << "lsfg-vk: framegen completion SYNC_FD import failed: "
                      << e.what() << "; using bounded host fallback\n";
        }

        if (importFailed) {
            for (const int fd : framegenSync.outputReadyFds)
                if (fd >= 0) ::close(fd);
            if (framegenSync.batchCompleteFd >= 0)
                ::close(framegenSync.batchCompleteFd);
            std::fill(outputReadyWaitValid.begin(), outputReadyWaitValid.end(), false);
            pass.framegenBatchCompleteValid = false;
            this->asyncFramegenCompletionEnabled_ = false;
            requireHostCompletionWait = true;
        }
    } else if (this->asyncFramegenCompletionEnabled_
            && framegenSync.hostWaitFallback) {
        requireHostCompletionWait = false;
        pass.framegenBatchCompleteValid = false;
    } else if (this->asyncFramegenCompletionEnabled_) {
        requireHostCompletionWait = true;
        this->asyncFramegenCompletionEnabled_ = false;
    }

    // 3. Compatibility/error fallback only. The normal SYNC_FD path queues the
    //    game-device copies against framegen completion and does not block here.
    bool framegenReady = true;
    if (requireHostCompletionWait) {
        const auto waitIdleStart = RuntimeMetrics::Clock::now();
        const uint64_t framegenCompletionTimeoutNs = runtimeWaitTimeoutNs();
        framegenReady = conf.performance
            ? LSFG_3_1P::waitContext(*this->lsfgCtxId, framegenCompletionTimeoutNs)
            : LSFG_3_1::waitContext(*this->lsfgCtxId, framegenCompletionTimeoutNs);
        metrics.windowWaitIdleMs += std::chrono::duration<double, std::milli>(
            RuntimeMetrics::Clock::now() - waitIdleStart).count();
    }
    if (!framegenReady) {
        const uint64_t framegenCompletionTimeoutNs = runtimeWaitTimeoutNs();
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
        std::cerr << "lsfg-vk: runtime stage=framegen-idle-ready"
                  << " host_wait=" << (requireHostCompletionWait ? 1 : 0)
                  << "\n";
    updateAdaptiveFlowGovernor();

    // 4. Generated presentation is opportunistic. Never wait for a synthetic
    // swapchain image: if WSI has no image immediately available, drop this and
    // the remaining synthetic opportunities so the real source present can be
    // queued without generated-frame backpressure.
    size_t queuedGeneratedFrameCount = 0;
    for (size_t i = 0; i < generatedFrameCount; i++) {
        const auto generatedPresentStart = RuntimeMetrics::Clock::now();
        const double syntheticFraction =
            static_cast<double>(i + 1)
            / static_cast<double>(interpolationGenerationCount + 1);
        const uint64_t syntheticDesiredTimeNs =
            this->sourceTimeline_.syntheticDesiredTimeNs(
                this->currentSourceTimeline_, syntheticFraction);
        const uint64_t syntheticAdmissionNowNs = monotonicNowNs();
        if (syntheticDesiredTimeNs > 0
                && syntheticAdmissionNowNs >= syntheticDesiredTimeNs) {
            const double deliveryLatenessMs =
                static_cast<double>(
                    syntheticAdmissionNowNs - syntheticDesiredTimeNs)
                / 1'000'000.0;
            this->deadlineAdmissionPredictor_.observeDeliveryMiss(
                deliveryLatenessMs);
            const size_t droppedGeneratedFrames = generatedFrameCount - i;
            metrics.windowGeneratedLateDrops += droppedGeneratedFrames;
            metrics.totalGeneratedLateDrops += droppedGeneratedFrames;
            metrics.windowGeneratedDeadlineDrops += droppedGeneratedFrames;
            metrics.totalGeneratedDeadlineDrops += droppedGeneratedFrames;
            if (firstPresentDiagnostic) {
                std::cerr << "lsfg-vk: runtime stage=generated-deadline-drop"
                          << " planned=" << generatedFrameCount
                          << " queued=" << queuedGeneratedFrameCount
                          << " dropped=" << droppedGeneratedFrames
                          << "\n";
            }
            break;
        }

        pass.acquireSemaphores.at(i) = Mini::Semaphore(info.device);
        uint32_t imageIdx{};
        auto res = Layer::ovkAcquireNextImageKHR(info.device, this->swapchain, 0,
            pass.acquireSemaphores.at(i).handle(), VK_NULL_HANDLE, &imageIdx);
        if (res == VK_NOT_READY || res == VK_TIMEOUT) {
            this->deadlineAdmissionPredictor_.observeDeliveryMiss(0.0);
            const size_t droppedGeneratedFrames = generatedFrameCount - i;
            metrics.windowGeneratedLateDrops += droppedGeneratedFrames;
            metrics.totalGeneratedLateDrops += droppedGeneratedFrames;
            metrics.windowGeneratedWsiDrops += droppedGeneratedFrames;
            metrics.totalGeneratedWsiDrops += droppedGeneratedFrames;
            if (firstPresentDiagnostic) {
                std::cerr << "lsfg-vk: runtime stage=generated-wsi-drop"
                          << " planned=" << generatedFrameCount
                          << " queued=" << queuedGeneratedFrameCount
                          << " dropped=" << droppedGeneratedFrames
                          << "\n";
            }
            break;
        }
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
        std::vector<VkSemaphore> generatedCopyWaits{
            pass.acquireSemaphores.at(i).handle()
        };
        if (outputReadyWaitValid.at(i))
            generatedCopyWaits.emplace_back(pass.renderSemaphores.at(i).handle());
        pass.postCopyBufs.at(i).submit(info.queue.second,
            generatedCopyWaits,
            { pass.postCopySemaphores.at(i).handle(),
              pass.prevPostCopySemaphores.at(i).handle() });

        std::vector<VkSemaphore> waitSemaphores{ pass.postCopySemaphores.at(i).handle() };
        if (i != 0) waitSemaphores.emplace_back(pass.prevPostCopySemaphores.at(i - 1).handle());

        VkPresentTimeGOOGLE generatedPresentTime{};
        VkPresentTimesInfoGOOGLE generatedPresentTimes{};
        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            // The application's pNext chain describes its real source present.
            // Synthetic presents carry only LSFG's own optional timing hint.
            .pNext = adaptivePresentPNext(
                nullptr,
                syntheticDesiredTimeNs,
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
        queuedGeneratedFrameCount++;
        metrics.windowGeneratedFrames++;
        metrics.totalGeneratedFrames++;
        metrics.windowGeneratedPresentMs += std::chrono::duration<double, std::milli>(
            RuntimeMetrics::Clock::now() - generatedPresentStart).count();
        if (firstPresentDiagnostic && i == 0) {
            std::cerr << "lsfg-vk: runtime stage=generated-present-ready image=" << imageIdx
                      << " result=" << res << "\n";
        }
    }

    if (generatedFrameCount > 0
            && queuedGeneratedFrameCount == generatedFrameCount) {
        this->deadlineAdmissionPredictor_.observeDeliverySuccess();
    }
    this->lastGeneratedFrameCount_ = queuedGeneratedFrameCount;

    // 5. Present the real game frame after only the synthetic frames that were
    // actually queued. A WSI drop therefore shortens this cycle instead of
    // making the source wait for an unavailable synthetic swapchain image.
    VkSemaphore lastPrevPostCopySemaphore = queuedGeneratedFrameCount > 0
        ? pass.prevPostCopySemaphores.at(queuedGeneratedFrameCount - 1).handle()
        : pass.preCopySemaphores.at(0).handle();
    VkPresentTimeGOOGLE finalSourcePresentTime{};
    VkPresentTimesInfoGOOGLE finalSourcePresentTimes{};
    const VkPresentInfoKHR finalPresentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = adaptivePresentPNext(
            pNext,
            this->currentSourceTimeline_.sourceDesiredTimeNs,
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
    return finishSourcePresent(
        res, queuedGeneratedFrameCount > 0
            ? "prev-post-copy"
            : "pre-copy-generated-drop");

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
    this->sourceTimeline_.reset();
    this->currentSourceTimeline_ = {};
    this->lastGeneratedFrameCount_ = 0;
    this->sourceHistoryWarmupRemaining_ = kSourceHistoryWarmupFrames;
    this->requiresSourceHistoryWarmup_ = true;
    this->lastDispatchedGeneratedFrameCount_ = 0;
    this->previousSourceCopySignalValid_ = false;
}
#endif

#include "context.hpp"
#include "android_sync_policy.hpp"
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
#include <poll.h>
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
#include <atomic>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace {

std::mutex lsfgDisableEnvMutex;

class ScopedLsfgDisable {
public:
    ScopedLsfgDisable()
            : lock_(lsfgDisableEnvMutex) {
        if (const char* previous = std::getenv("DISABLE_LSFG"))
            previousValue_ = previous;
        if (setenv("DISABLE_LSFG", "1", 1) != 0)
            throw std::runtime_error("Unable to disable recursive LSFG interception");
        active_ = true;
    }

    ScopedLsfgDisable(const ScopedLsfgDisable&) = delete;
    ScopedLsfgDisable& operator=(const ScopedLsfgDisable&) = delete;

    ~ScopedLsfgDisable() noexcept {
        if (!active_)
            return;
        const int result = previousValue_.has_value()
            ? setenv("DISABLE_LSFG", previousValue_->c_str(), 1)
            : unsetenv("DISABLE_LSFG");
        if (result != 0)
            std::cerr << "lsfg-vk: failed to restore DISABLE_LSFG after backend setup\n";
    }

private:
    std::unique_lock<std::mutex> lock_;
    std::optional<std::string> previousValue_;
    bool active_{false};
};

size_t residentCapacityMultiplier(const Config::Configuration& conf) {
#ifdef __ANDROID__
    constexpr size_t kAndroidResidentMaxMultiplier = 4;
    if (conf.targeted)
        return std::max(conf.multiplier, kAndroidResidentMaxMultiplier);
#endif
    return conf.multiplier;
}

#ifdef __ANDROID__
constexpr uint32_t kConservativeSourceReprimeFrames = 2;

bool isAdrenoWsiRetirementResult(VkResult result) noexcept {
    return result == VK_ERROR_OUT_OF_DATE_KHR
        || result == VK_ERROR_SURFACE_LOST_KHR;
}

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

uint64_t processRuntimeSessionId() {
    static const uint64_t sessionId = [] {
        const uint64_t now = monotonicNowNs();
        const uint64_t pid = static_cast<uint64_t>(
            static_cast<uint32_t>(::getpid()));
        const uint64_t mixed =
            (pid << 32) ^ now ^ 0x9e3779b97f4a7c15ULL;
        return mixed != 0 ? mixed : 1ULL;
    }();
    return sessionId;
}

uint64_t nextRuntimeConfigRevision() {
    static std::atomic<uint64_t> nextRevision{1};
    uint64_t revision =
        nextRevision.fetch_add(1, std::memory_order_relaxed);
    if (revision == 0)
        revision = nextRevision.fetch_add(1, std::memory_order_relaxed);
    return revision;
}

uint64_t runtimeDiagnosticConfigSignature(
        const Config::Configuration& conf) {
    uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };

    mix(conf.enable ? 1ULL : 0ULL);
    mix(conf.targeted ? 1ULL : 0ULL);
    mix(static_cast<uint64_t>(conf.multiplier));
    const double flowScale = static_cast<double>(conf.flowScale);
    mix(std::isfinite(flowScale)
        ? static_cast<uint64_t>(std::llround(flowScale * 1'000'000.0))
        : 0ULL);
    mix(conf.adaptiveFlowScale ? 1ULL : 0ULL);
    for (const unsigned char ch : conf.adaptiveFlowPreset)
        mix(static_cast<uint64_t>(ch));
    mix(conf.performance ? 1ULL : 0ULL);
    mix(conf.hdr ? 1ULL : 0ULL);
    mix(conf.adaptiveFramegen ? 1ULL : 0ULL);
    mix(static_cast<uint64_t>(conf.fpsLimit));
    return hash;
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
    const double sourceIntervalMs =
        std::chrono::duration<double, std::milli>(sourceInterval).count();
    if (!(sourceIntervalMs > 0.0) || !std::isfinite(sourceIntervalMs))
        return 0.0;

    // Adaptive Flow timing is a complete LSFG batch measurement. Its fallback
    // budget is therefore the whole source-owned interval, not one output
    // period or one synthetic slot. A deadline decision supplies a tighter
    // batch budget when one is available at the call site.
    if (conf.adaptiveFramegen)
        return sourceIntervalMs;

    if (conf.fpsLimit > 0)
        return 1000.0 / static_cast<double>(conf.fpsLimit);
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

    bool sawTimestamp = false;
    bool sawGpu = false;
    bool sawOutput = false;
    bool sawFrameTime = false;
    bool sawSlowRatio = false;
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos)
            continue;
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        const bool knownKey =
            key == "timestamp_ms"
            || key == "gpu_usage_percent"
            || key == "output_fps"
            || key == "frame_time_p95_ms"
            || key == "slow_frame_ratio";
        if (!knownKey)
            continue;

        std::size_t consumed = 0;
        double parsed = 0.0;
        try {
            parsed = std::stod(value, &consumed);
        } catch (const std::exception&) {
            continue;
        }
        if (consumed != value.size() || !std::isfinite(parsed))
            continue;

        if (key == "timestamp_ms") {
            sawTimestamp = parsed > 0.0;
        } else if (key == "gpu_usage_percent") {
            sample.gpuUsagePercent = parsed;
            sawGpu = true;
        } else if (key == "output_fps") {
            sample.outputFps = parsed;
            sawOutput = true;
        } else if (key == "frame_time_p95_ms") {
            sample.frameTimeP95Ms = parsed;
            sawFrameTime = true;
        } else if (key == "slow_frame_ratio") {
            sample.slowFrameRatio = parsed;
            sawSlowRatio = true;
        }
    }

    // GameNative publishes this file atomically. Require the complete record so
    // a reader that races a replacement, or a stale producer with missing
    // fields, cannot turn default-zero values into false global pressure.
    sample.valid =
        sawTimestamp
        && sawGpu
        && sawOutput
        && sawFrameTime
        && sawSlowRatio
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
        PFN_vkWaitForFences waitForFences,
        double* submitMs = nullptr, double* waitMs = nullptr) {
    const auto submitStart = std::chrono::steady_clock::now();
    submitAhbHandoff(device, commandBuffer, queue, waitSemaphores,
        signalSemaphores, fence, resetFences);
    const auto submitEnd = std::chrono::steady_clock::now();
    const auto waitStart = submitEnd;
    waitForAhbHandoff(device, fence, waitForFences);
    const auto waitEnd = std::chrono::steady_clock::now();
    if (submitMs != nullptr) {
        *submitMs += std::chrono::duration<double, std::milli>(
            submitEnd - submitStart).count();
    }
    if (waitMs != nullptr) {
        *waitMs += std::chrono::duration<double, std::milli>(
            waitEnd - waitStart).count();
    }
}

#endif

} // namespace

LsContext::LsContext(const Hooks::DeviceInfo& info, VkSwapchainKHR swapchain,
        VkExtent2D extent, const std::vector<VkImage>& swapchainImages)
        : swapchain(swapchain), swapchainImages(swapchainImages),
          presentWaitRetirements_(swapchainImages.size()),
          extent(extent), device_(info.device), queue_(info.queue.second) {
    // get updated configuration
    auto conf = Config::snapshot();
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
            Config::setActive(Config::getConfig(name));
            conf = Config::snapshot();
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

    LSFG::BackendDiagnostics backendDiagnostics{};
    auto ahbTransportMode = LSFG::AhbTransportMode::Unsupported;
    int32_t ctxId{};
    {
        ScopedLsfgDisable disableRecursiveInterception;
        lsfgInitialize(
            info.identity, format,
            conf.hdr, 1.0F / initialFlowScale, runtimeMultiplier - 1,
            [](const std::string& name) {
                auto dxbc = Extract::getShader(name);
                auto spirv = Extract::translateShader(dxbc);
                return spirv;
            }
        );

        backendDiagnostics = conf.performance
            ? LSFG_3_1P::getBackendDiagnostics()
            : LSFG_3_1::getBackendDiagnostics();
        ahbTransportMode = backendDiagnostics.ahbTransportMode;
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
            [lsfgDeleteContext = lsfgDeleteContext](int32_t* id) {
                if (id != nullptr) {
                    lsfgDeleteContext(*id);
                    delete id;
                }
            }
        );
    }

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
    this->conservativeCrossDeviceSync_ =
        AndroidSyncPolicy::requiresConservativeCrossDeviceSync(
            backendDiagnostics.driverId, backendDiagnostics.driverName);
    if (this->conservativeCrossDeviceSync_
            && info.adrenoSyntheticQueueAvailable
            && info.syntheticQueue != VK_NULL_HANDLE) {
        this->syntheticQueue_ = info.syntheticQueue;
    }
    // Source ownership and framegen completion are independent policies.
    // Qualcomm/Adreno keeps the protected source/history topology below:
    // true source-only, reprime, and fractional zero-generation cycles never
    // cross into framegen. Generated cycles, however, can safely use the same
    // one-shot SYNC_FD output-ready + batch-complete dependency chain as other
    // capable drivers. This removes the source-thread completion wait while
    // preserving the r11 zero-count/lifetime repair. Xclipse remains on its
    // existing capability-driven SYNC_FD path unchanged.
    this->asyncAhbHandoffEnabled_ =
        gameGetSemaphoreFd != nullptr
        && (this->conservativeCrossDeviceSync_
            ? syncFdHandoffSupported
            : (syncFdHandoffSupported || opaqueFdHandoffSupported));
    this->asyncAhbHandoffHandleType_ =
        this->conservativeCrossDeviceSync_
            ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
            : (syncFdHandoffSupported
                ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
                : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
    const bool adrenoSingleQueueReadinessPoll =
        this->conservativeCrossDeviceSync_
        && this->syntheticQueue_ == VK_NULL_HANDLE
        && syncFdHandoffSupported;
    this->asyncFramegenCompletionEnabled_ =
        syncFdHandoffSupported
        && (adrenoSingleQueueReadinessPoll
            || gameImportSemaphoreFd != nullptr);

    // The known-good Qualcomm/Adreno path can generate immediately because the
    // first source upload initializes both AHB inputs. Do not inherit the newer
    // four-cycle private-history startup warmup on this compatibility path.
    if (this->conservativeCrossDeviceSync_) {
        this->sourceHistoryWarmupRemaining_ = 0;
        this->requiresSourceHistoryWarmup_ = false;
    }

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
              << ", sync_policy="
              << AndroidSyncPolicy::crossDeviceSyncPolicyName(
                    this->conservativeCrossDeviceSync_)
              << ", synthetic_queue="
              << (this->syntheticQueue_ != VK_NULL_HANDLE ? 1 : 0)
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

    {
        ScopedLsfgDisable disableRecursiveInterception;
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
            [lsfgDeleteContext = lsfgDeleteContext](int32_t* id) {
                if (id != nullptr) {
                    lsfgDeleteContext(*id);
                    delete id;
                }
            }
        );
    }
#endif

    const auto createCompletionFence = reinterpret_cast<PFN_vkCreateFence>(
        Layer::ovkGetDeviceProcAddr(info.device, "vkCreateFence"));
    this->completionWaitFences_ = reinterpret_cast<PFN_vkWaitForFences>(
        Layer::ovkGetDeviceProcAddr(info.device, "vkWaitForFences"));
    this->completionResetFences_ = reinterpret_cast<PFN_vkResetFences>(
        Layer::ovkGetDeviceProcAddr(info.device, "vkResetFences"));
    this->waitQueueIdle_ = reinterpret_cast<PFN_vkQueueWaitIdle>(
        Layer::ovkGetDeviceProcAddr(info.device, "vkQueueWaitIdle"));
    const auto destroyCompletionFence = reinterpret_cast<PFN_vkDestroyFence>(
        Layer::ovkGetDeviceProcAddr(info.device, "vkDestroyFence"));
    if (createCompletionFence == nullptr
            || this->completionWaitFences_ == nullptr
            || this->completionResetFences_ == nullptr
            || this->waitQueueIdle_ == nullptr
            || destroyCompletionFence == nullptr) {
        throw LSFG::vulkan_error(
            VK_ERROR_INITIALIZATION_FAILED,
            "Required pass-retirement fence functions unavailable");
    }

    // prepare render passes
    bool reuseGameCopyCommandBuffers = false;
#ifdef __ANDROID__
    reuseGameCopyCommandBuffers = this->conservativeCrossDeviceSync_;
#endif
    this->cmdPool = Mini::CommandPool(
        info.device, info.queue.first, reuseGameCopyCommandBuffers);
    for (size_t i = 0; i < 8; i++) {
        auto& pass = this->passInfos.at(i);
        pass.renderSemaphores.resize(runtimeMultiplier - 1);
        pass.acquireSemaphores.resize(runtimeMultiplier - 1);
        pass.postCopyBufs.resize(runtimeMultiplier - 1);
        if (reuseGameCopyCommandBuffers) {
            pass.preCopyBuf = Mini::CommandBuffer(info.device, this->cmdPool);
            for (auto& postCopyBuf : pass.postCopyBufs)
                postCopyBuf = Mini::CommandBuffer(info.device, this->cmdPool);
        }
        pass.postCopySemaphores.resize(runtimeMultiplier - 1);
        pass.prevPostCopySemaphores.resize(runtimeMultiplier - 1);

        const VkFenceCreateInfo fenceInfo{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = 0,
        };
        VkFence fence = VK_NULL_HANDLE;
        const auto fenceResult = createCompletionFence(
            info.device, &fenceInfo, nullptr, &fence);
        if (fenceResult != VK_SUCCESS || fence == VK_NULL_HANDLE) {
            throw LSFG::vulkan_error(
                fenceResult == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : fenceResult,
                "Failed to create pass-retirement fence");
        }
        pass.completionFence = std::shared_ptr<VkFence>(
            new VkFence(fence),
            [device = info.device, destroyCompletionFence](VkFence* ownedFence) {
                if (ownedFence != nullptr) {
                    if (*ownedFence != VK_NULL_HANDLE)
                        destroyCompletionFence(device, *ownedFence, nullptr);
                    delete ownedFence;
                }
            });
    }
}

LsContext::~LsContext() {
#ifdef __ANDROID__
    if (this->conservativePendingBatchCompletePollFd_ >= 0) {
        ::close(this->conservativePendingBatchCompletePollFd_);
        this->conservativePendingBatchCompletePollFd_ = -1;
    }
    if (this->waitQueueIdle_ != nullptr
            && this->syntheticQueue_ != VK_NULL_HANDLE
            && this->syntheticQueue_ != this->queue_) {
        const auto syntheticWaitResult =
            this->waitQueueIdle_(this->syntheticQueue_);
        if (syntheticWaitResult != VK_SUCCESS) {
            std::cerr << "lsfg-vk: synthetic queueWaitIdle failed result="
                      << syntheticWaitResult << "\n";
        }
    }
#endif
    // All pass command buffers and semaphores may still be referenced by the
    // game/WSI queue. This is teardown-only synchronization: it is never used
    // on the present hot path.
    if (this->waitQueueIdle_ != nullptr && this->queue_ != VK_NULL_HANDLE) {
        const auto queueWaitResult = this->waitQueueIdle_(this->queue_);
        if (queueWaitResult != VK_SUCCESS) {
            std::cerr << "lsfg-vk: pass teardown queueWaitIdle failed result="
                      << queueWaitResult << "\n";
        }
    }

    // Destroy the backend context first so any imported VkImages are released
    // before the Mini::Image owners release their AHardwareBuffers. Core::Image
    // also retains its own AHB reference, but this ordering keeps both layers'
    // Vulkan and native-handle lifetimes unambiguous on failure paths.
    this->lsfgCtxId.reset();
}

void LsContext::releasePresentWaitRetirements(uint32_t imageIdx) {
    if (imageIdx < this->presentWaitRetirements_.size())
        this->presentWaitRetirements_.at(imageIdx).clear();
}

void LsContext::retainPresentWait(
        uint32_t imageIdx, const Mini::Semaphore& semaphore) {
    if (imageIdx >= this->presentWaitRetirements_.size()
            || semaphore.handle() == VK_NULL_HANDLE)
        return;
    this->presentWaitRetirements_.at(imageIdx).emplace_back(semaphore);
}

bool LsContext::tryRecyclePass(RenderPassInfo& pass) {
    if (!pass.completionFenceSubmitted) {
        if (!pass.completionFenceFailed)
            pass.crossFrameWaitRetentions.clear();
        return !pass.completionFenceFailed;
    }
    if (pass.completionFenceFailed
            || pass.completionFence == nullptr
            || this->completionWaitFences_ == nullptr
            || this->completionResetFences_ == nullptr)
        return false;

    const VkFence fence = *pass.completionFence;
    const auto waitResult = this->completionWaitFences_(
        this->device_, 1, &fence, VK_TRUE, 0);
    if (waitResult != VK_SUCCESS) {
        if (waitResult != VK_TIMEOUT && waitResult != VK_NOT_READY) {
            Utils::logLimitN(
                "passRetirement",
                5,
                "Pass completion fence query failed: "
                    + std::to_string(waitResult));
        }
        return false;
    }

    const auto resetResult = this->completionResetFences_(
        this->device_, 1, &fence);
    if (resetResult != VK_SUCCESS) {
        pass.completionFenceFailed = true;
        Utils::logLimitN(
            "passRetirement",
            5,
            "Pass completion fence reset failed: "
                + std::to_string(resetResult));
        return false;
    }

    pass.completionFenceSubmitted = false;
    pass.crossFrameWaitRetentions.clear();
#ifdef __ANDROID__
    pass.framegenBatchCompleteValid = false;
#endif
    return true;
}

bool LsContext::submitPassCompletionFence(RenderPassInfo& pass, VkQueue queue) {
    if (pass.completionFence == nullptr
            || pass.completionFenceSubmitted
            || pass.completionFenceFailed)
        return false;

    // This fence is queued after the pass's game-device submissions and before
    // the final source present. It proves command-buffer/acquire/cross-frame
    // wait consumption only. vkQueuePresentKHR wait semaphores are retired
    // separately when their swapchain image is acquired again.
    const VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 0,
    };
    const auto submitResult = Layer::ovkQueueSubmit(
        queue, 1, &submitInfo, *pass.completionFence);
    if (submitResult == VK_SUCCESS) {
        pass.completionFenceSubmitted = true;
        return true;
    }

    // The present already succeeded. Use a teardown-style queue wait only on
    // this exceptional bookkeeping failure so resources remain safe to reuse.
    Utils::logLimitN(
        "passRetirement",
        5,
        "Pass completion fence submit failed: "
            + std::to_string(submitResult));
    if (this->waitQueueIdle_ != nullptr
            && this->waitQueueIdle_(queue) == VK_SUCCESS)
        return true;

    pass.completionFenceFailed = true;
    return false;
}

VkResult LsContext::present(const Hooks::DeviceInfo& info, const void* pNext, VkQueue queue,
        const std::vector<VkSemaphore>& gameRenderSemaphores, uint32_t presentIdx) {
    const auto conf = Config::snapshot();
#ifdef __ANDROID__
    const bool adrenoSingleQueueReadinessPoll =
        this->conservativeCrossDeviceSync_
        && this->syntheticQueue_ == VK_NULL_HANDLE
        && this->asyncFramegenCompletionEnabled_;
#else
    constexpr bool adrenoSingleQueueReadinessPoll = false;
#endif
    this->releasePresentWaitRetirements(presentIdx);
    auto& pass = this->passInfos.at(this->frameIdx % 8);
    if (!this->tryRecyclePass(pass)) {
        const VkPresentInfoKHR passthroughPresentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = pNext,
            .waitSemaphoreCount =
                static_cast<uint32_t>(gameRenderSemaphores.size()),
            .pWaitSemaphores = gameRenderSemaphores.data(),
            .swapchainCount = 1,
            .pSwapchains = &this->swapchain,
            .pImageIndices = &presentIdx,
        };
        const auto passthroughResult = Layer::ovkQueuePresentKHR(
            queue, &passthroughPresentInfo);
        if (passthroughResult == VK_SUCCESS
                || passthroughResult == VK_SUBOPTIMAL_KHR) {
            this->lastGeneratedFrameCount_ = 0;
#ifdef __ANDROID__
            this->resetAdaptiveSourceEpoch(true);
#endif
            Utils::logLimitN(
                "passRetirement",
                5,
                "Pass ring busy; queued source-only present until a slot retires");
        }
        return passthroughResult;
    }

#ifdef __ANDROID__
    if (this->runtimeSessionId_ == 0)
        this->runtimeSessionId_ = processRuntimeSessionId();
    const uint64_t currentConfigSignature =
        runtimeDiagnosticConfigSignature(conf);
    if (!this->runtimeConfigSignatureValid_) {
        this->runtimeConfigSignature_ = currentConfigSignature;
        this->runtimeConfigSignatureValid_ = true;
        this->configRevision_ = nextRuntimeConfigRevision();
    } else if (this->runtimeConfigSignature_ != currentConfigSignature) {
        this->runtimeConfigSignature_ = currentConfigSignature;
        this->configRevision_ = nextRuntimeConfigRevision();
        this->advanceAdaptiveFlowTimingEpoch();
    }

    auto& metrics = this->runtimeMetrics;
    const auto cycleStart = RuntimeMetrics::Clock::now();
    bool excludeCurrentCycleFromTimingMetrics = false;
    const size_t maxAdaptiveGeneratedFrames =
        conf.multiplier > 1 ? static_cast<size_t>(conf.multiplier - 1) : 0;
    this->adaptiveScheduler_.configure(
        conf.adaptiveFramegen ? conf.fpsLimit : 0,
        maxAdaptiveGeneratedFrames);
    this->generatedPresentationCapacityTracker_.configure(
        conf.adaptiveFramegen ? maxAdaptiveGeneratedFrames : 0);
    this->lsfgOutputCadenceTracker_.configure(
        conf.adaptiveFramegen && conf.fpsLimit > 0,
        conf.adaptiveFramegen ? conf.fpsLimit : 0);
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
        conf.multiplier > 1
            ? static_cast<size_t>(conf.multiplier - 1)
            : 0;

    // Capacity feedback is advisory and comes from the previous measured GPU
    // cost/timeline. It may accelerate one scheduler level only after repeated
    // safe evidence; per-cycle deadline admission remains authoritative.
    double capacityIntervalMs = 0.0;
    if (conf.adaptiveFramegen && this->currentSourceTimeline_.valid
            && this->currentSourceTimeline_.intervalNs > 0) {
        capacityIntervalMs =
            static_cast<double>(this->currentSourceTimeline_.intervalNs)
            / 1'000'000.0;
    } else if (conf.adaptiveFramegen && sourceInterval.count() > 0) {
        const double observedIntervalMs =
            std::chrono::duration<double, std::milli>(sourceInterval).count();
        if (observedIntervalMs < kRuntimeTimingDiscontinuityMs)
            capacityIntervalMs = observedIntervalMs;
    }
    const bool safeGenerationHintValid =
        conf.adaptiveFramegen
        && maxAdaptiveGeneratedFrames > 0
        && capacityIntervalMs > 0.0
        && this->deadlineAdmissionPredictor_.hasEstimate();
    const size_t safeGenerationHint = safeGenerationHintValid
        ? this->deadlineAdmissionPredictor_.safeGenerationHint(
            maxAdaptiveGeneratedFrames, capacityIntervalMs)
        : 0;
    this->adaptiveScheduler_.setSafeGenerationHint(
        safeGenerationHint,
        safeGenerationHintValid);

    size_t plannedGeneratedFrameCount = conf.adaptiveFramegen
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
        // The scheduler already consumed this discontinuity and cleared its
        // own cadence state. Reset every dependent controller without
        // overwriting the scheduler telemetry used for this cycle's logs.
        this->resetAdaptiveSourceEpoch(false);
        this->lsfgOutputCadenceTracker_.configure(
            conf.adaptiveFramegen && conf.fpsLimit > 0,
            conf.adaptiveFramegen ? conf.fpsLimit : 0);
    } else {
        const bool hadValidSourceTimeline = this->currentSourceTimeline_.valid;
        this->currentSourceTimeline_ = this->sourceTimeline_.observe(
            sourceArrivalTimeNs, sourceInterval, false);
        if (hadValidSourceTimeline
                && !this->currentSourceTimeline_.valid
                && sourceInterval.count() > 0) {
            // SourceProtectedTimeline can reject an interval independently of
            // the scheduler. Consume the source interval and force a clean
            // epoch rather than letting stale demand/capacity state leak
            // across a suspend or translation-layer discontinuity.
            this->resetAdaptiveSourceEpoch(true);
            this->lsfgOutputCadenceTracker_.configure(
                conf.adaptiveFramegen && conf.fpsLimit > 0,
                conf.adaptiveFramegen ? conf.fpsLimit : 0);
            plannedGeneratedFrameCount = 0;
            generatedFrameCount = 0;
            interpolationGenerationCount = 0;
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
    const bool adrenoDeadlineBootstrapProbe =
        adrenoSingleQueueReadinessPoll
        && conf.adaptiveFramegen
        && generatedFrameCount > 0
        && !this->deadlineAdmissionPredictor_.hasEstimate();
    if (conf.adaptiveFramegen
            && !sourceHistoryWarmupActive
            && generatedFrameCount > 0
            && this->currentSourceTimeline_.valid) {
        const uint64_t admissionNowNs = monotonicNowNs();
        if (adrenoDeadlineBootstrapProbe) {
            if (generatedFrameCount > 1) {
                const size_t rejectedGeneratedFrameCount =
                    generatedFrameCount - 1;
                metrics.windowAdmissionRejects += rejectedGeneratedFrameCount;
                metrics.totalAdmissionRejects += rejectedGeneratedFrameCount;
            }
            generatedFrameCount =
                std::min<std::size_t>(generatedFrameCount, 1);
        } else if (admissionNowNs > 0) {
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

    const auto& outputCadenceForPresentation =
        this->lsfgOutputCadenceTracker_.snapshot();
    const bool presentationOutputDeficit =
        conf.adaptiveFramegen
        && conf.fpsLimit > 0
        && outputCadenceForPresentation.valid
        && outputCadenceForPresentation.deficitConfirmed;
    const uint64_t sourceDeadlineSlackNs = std::max<uint64_t>(
        1'000'000ULL, this->currentSourceTimeline_.intervalNs / 8ULL);
    const bool sourceInsidePresentationBudget =
        conf.adaptiveFramegen
        && this->currentSourceTimeline_.valid
        && sourceInterval.count() > 0
        && !sourceTimelineDiscontinuity
        && !sourceHistoryWarmupActive
        && this->currentSourceTimeline_.sourceDeadlineErrorNs
            <= static_cast<int64_t>(sourceDeadlineSlackNs);
    const auto& currentPresentationCapacity =
        this->generatedPresentationCapacityTracker_.telemetry();
    const bool higherPresentationCapacityProven =
        currentPresentationCapacity.highestUsefulCapacity
            > currentPresentationCapacity.generationCap;
    const GeneratedPresentationCapacityContext presentationCapacityContext{
        .outputDeficit = presentationOutputDeficit,
        .deadlineCapacityValid = safeGenerationHintValid,
        .safeGenerationHint = safeGenerationHint,
        .schedulerCostLimit = adaptiveTelemetry.costLimit,
        .sourceInsideBudget = sourceInsidePresentationBudget,
        .sourceDeadlineErrorNs =
            this->currentSourceTimeline_.sourceDeadlineErrorNs,
        .higherCapacityProven = higherPresentationCapacityProven,
    };

    // WSI capacity is a separate downstream constraint from GPU generation
    // capacity. Apply its provisional cap before expensive framegen dispatch;
    // a suppressed slot is consumed and never repaid. Fixed mode is untouched.
    if (conf.adaptiveFramegen && generatedFrameCount > 0
            && !adrenoDeadlineBootstrapProbe) {
        const size_t presentationCappedGeneratedFrameCount =
            this->generatedPresentationCapacityTracker_.limit(
                generatedFrameCount, presentationCapacityContext);
        if (presentationCappedGeneratedFrameCount < generatedFrameCount) {
            const size_t cappedGeneratedFrames =
                generatedFrameCount - presentationCappedGeneratedFrameCount;
            metrics.windowGeneratedPresentationCapDrops +=
                cappedGeneratedFrames;
            metrics.totalGeneratedPresentationCapDrops +=
                cappedGeneratedFrames;
            generatedFrameCount = presentationCappedGeneratedFrameCount;
        }
    }

    // Admission and presentation-cap limiting are complete before any framegen
    // dispatch. Re-space the surviving batch evenly across the protected source
    // interval; rejected opportunities are consumed and never become catch-up debt.
    interpolationGenerationCount = generatedFrameCount;

    // Flow GPU timestamps cover the complete submitted batch. Keep the budget
    // and predictor value attached to that exact batch instead of comparing a
    // delayed sample with the next source cycle's one-slot period.
    if (conf.adaptiveFramegen && generatedFrameCount > 0
            && this->deadlineBatchDecision_.valid) {
        const auto finalBatchDecision = this->deadlineAdmissionPredictor_.predict(
            generatedFrameCount,
            this->deadlineBatchDecision_.usableBudgetMs);
        if (finalBatchDecision.valid)
            this->deadlineBatchDecision_ = finalBatchDecision;
    }
    const double adaptiveFlowBatchBudgetMs = [&]() {
        double budgetMs = 0.0;
        if (conf.adaptiveFramegen
                && generatedFrameCount > 0
                && this->deadlineBatchDecision_.valid
                && this->deadlineBatchDecision_.effectiveUsableBudgetMs > 0.0) {
            budgetMs = this->deadlineBatchDecision_.effectiveUsableBudgetMs;
        } else {
            budgetMs = adaptiveFlowFrameBudgetMs(
                conf, sourceInterval, generatedFrameCount);
        }

        const double protectedAdrenoTargetBudgetMs =
            this->conservativeCrossDeviceSync_
            && conf.adaptiveFramegen
            && conf.fpsLimit > 0
            && generatedFrameCount > 0
                ? 1000.0
                    * static_cast<double>(generatedFrameCount + 1)
                    / static_cast<double>(conf.fpsLimit)
                : 0.0;
        if (protectedAdrenoTargetBudgetMs > 0.0) {
            budgetMs = budgetMs > 0.0
                ? std::min(budgetMs, protectedAdrenoTargetBudgetMs)
                : protectedAdrenoTargetBudgetMs;
        }
        return budgetMs;
    }();
    const auto nextAdaptiveFlowBatch = [&]() {
        ++this->adaptiveFlowNextBatchId_;
        if (this->adaptiveFlowNextBatchId_ == 0)
            this->adaptiveFlowNextBatchId_ = 1;
        return LSFG::AdaptiveFlowBatchMetadata{
            .sessionEpoch = this->adaptiveFlowTimingEpoch_,
            .batchId = this->adaptiveFlowNextBatchId_,
            .frameBudgetMs = adaptiveFlowBatchBudgetMs,
            .predictedTotalLsfgMs =
                this->deadlineBatchDecision_.valid
                    ? this->deadlineBatchDecision_.predictedTotalLsfgMs
                    : 0.0,
        };
    };
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

    // Fractional Adaptive LSFG intentionally creates zero-generation gaps.
    // Those gaps must still advance temporal history, even on the conservative
    // Adreno path. True zero-demand/admission-drop cycles and startup history
    // warmup remain source-only so they cannot force unnecessary private-device
    // ownership work onto the protected source timeline.
    const bool conservativeFractionalHistoryGap =
        this->conservativeCrossDeviceSync_
        && conf.adaptiveFramegen
        && !sourceHistoryWarmupActive
        && plannedGeneratedFrameCount == 0
        && adaptiveTelemetry.wantedGeneratedFrames > 0.0;
    const bool conservativeSourceOnlyWarmup =
        this->conservativeCrossDeviceSync_
        && sourceHistoryWarmupActive;
    const bool conservativeTrueSourceOnlyCycle =
        this->conservativeCrossDeviceSync_
        && historyOnly
        && !conservativeSourceOnlyWarmup
        && !conservativeFractionalHistoryGap;

    this->lastGeneratedFrameCount_ = historyOnly ? 0 : generatedFrameCount;

    const auto updateAdaptiveFlowGovernor = [&]() {
        const LSFG::AdaptiveFlowGpuTiming timing = conf.performance
            ? LSFG_3_1P::getContextGpuTiming(*this->lsfgCtxId)
            : LSFG_3_1::getContextGpuTiming(*this->lsfgCtxId);
        const bool timingSessionMatches =
            timing.valid
            && timing.sessionEpoch == this->adaptiveFlowTimingEpoch_
            && timing.batchId > 0;
        const bool timingFresh = timingSessionMatches
            && timing.batchId > this->adaptiveFlowLastObservedBatchId_;
        if (timingFresh)
            this->adaptiveFlowLastObservedBatchId_ = timing.batchId;
        const bool timingUsable = timingFresh && !timing.transitionActive;
        const bool generatedWorkSample =
            timingUsable && timing.generationCount > 0;

        if (generatedWorkSample) {
            this->adaptiveFlowGeneratedTimingValid_ = true;
            this->adaptiveFlowRetainedMipmapsMs_ = timing.mipmapsMs;
            this->adaptiveFlowRetainedWorkMs_ = timing.opticalFlowMs;
            this->adaptiveFlowRetainedTotalLsfgMs_ = timing.totalLsfgMs;
            this->adaptiveFlowRetainedBudgetMs_ = timing.frameBudgetMs;
            this->adaptiveFlowRetainedGenerationCount_ = timing.generationCount;

            if (timing.predictedTotalLsfgMs > 0.0
                    && std::isfinite(timing.predictedTotalLsfgMs)) {
                metrics.windowDeadlinePredictionAbsErrorMs += std::abs(
                    timing.totalLsfgMs
                    - timing.predictedTotalLsfgMs);
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

        const bool schedulerTransition =
            conf.adaptiveFramegen && adaptiveLsfgTransition(adaptiveTelemetry);
        constexpr double kAdaptiveFlowCadenceDiscontinuityMs = 250.0;
        const double sourceIntervalMs =
            std::chrono::duration<double, std::milli>(sourceInterval).count();
        const bool cadenceDiscontinuity =
            sourceIntervalMs >= kAdaptiveFlowCadenceDiscontinuityMs;

        const auto& outputCadence =
            this->lsfgOutputCadenceTracker_.snapshot();
        const bool outputDeficit =
            conf.adaptiveFramegen
            && conf.fpsLimit > 0
            && outputCadence.valid
            && outputCadence.deficitConfirmed;

        // Keep compute/deadline pressure and downstream WSI pressure separate.
        // A WSI rejection never trains the deadline predictor and only becomes
        // a Flow actuator when the controller also sees global GPU pressure and
        // enough scale-sensitive Flow work to plausibly help.
        const uint64_t computeDropTotal =
            metrics.totalAdmissionRejects
            + metrics.totalGeneratedDeadlineDrops;
        const bool computeDropPressure =
            computeDropTotal > this->adaptiveFlowLastObservedComputeDrops_;
        this->adaptiveFlowLastObservedComputeDrops_ = computeDropTotal;

        const bool newWsiDropPressure =
            metrics.totalGeneratedWsiDrops
                > this->adaptiveFlowLastObservedWsiDrops_;
        this->adaptiveFlowLastObservedWsiDrops_ =
            metrics.totalGeneratedWsiDrops;
        const auto& presentationCapacity =
            this->generatedPresentationCapacityTracker_.telemetry();
        const bool wsiPresentationPressure =
            newWsiDropPressure || presentationCapacity.pressure;
        this->adaptiveFlowComputePressure_ = computeDropPressure;
        this->adaptiveFlowWsiPressure_ = wsiPresentationPressure;

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
        const double observationBudgetMs = generatedWorkSample
            && timing.frameBudgetMs > 0.0
            && std::isfinite(timing.frameBudgetMs)
            ? timing.frameBudgetMs
            : (this->adaptiveFlowRetainedBudgetMs_ > 0.0
                && std::isfinite(this->adaptiveFlowRetainedBudgetMs_)
                ? this->adaptiveFlowRetainedBudgetMs_
                : adaptiveFlowBatchBudgetMs);
        const bool observationBudgetValid =
            observationBudgetMs > 0.0 && std::isfinite(observationBudgetMs);

        AdaptiveFlowObservation observation{
            .elapsed = sourceInterval,
            .frameBudgetMs = observationBudgetMs,
            .totalLsfgMs = observationTotalMs,
            .flowMs = observationFlowMs,
            .mipmapsMs = observationMipmapsMs,
            .generationCount = observationGenerationCount,
            .deadlineMissed = generatedWorkSample && observationBudgetValid
                && timing.totalLsfgMs > observationBudgetMs,
            .computeDeadlinePressure = computeDropPressure,
            .wsiPresentationPressure = wsiPresentationPressure,
            .wsiLossRate = presentationCapacity.wsiRejectionRatio,
            .sourceFps = adaptiveTelemetry.smoothedSourceFps,
            .outputFps = outputCadence.outputFps,
            .outputCadenceValid = outputCadence.valid,
            .outputTargeted = outputCadence.targeted,
            .outputTargetSatisfied =
                outputCadence.targetSatisfiedConfirmed,
            .globalGpuUsagePercent =
                this->adaptiveFlowGlobalGpuUsagePercent_,
            .globalPressureValid =
                this->adaptiveFlowGlobalPressureValid_,
            .outputDeficit = outputDeficit,
            .syntheticDropPressure = false,
            .generatedWorkSample = generatedWorkSample,
            .schedulerTransition = schedulerTransition,
            .valid = observationBudgetValid
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
                      << " runtime_session_id=" << this->runtimeSessionId_
                      << " config_revision=" << this->configRevision_
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
                      << " output_fps=" << observation.outputFps
                      << " output_deficit="
                      << (observation.outputDeficit ? 1 : 0)
                      << " output_satisfied="
                      << (observation.outputTargetSatisfied ? 1 : 0)
                      << " compute_pressure="
                      << (observation.computeDeadlinePressure ? 1 : 0)
                      << " wsi_pressure="
                      << (observation.wsiPresentationPressure ? 1 : 0)
                      << " wsi_loss_rate=" << observation.wsiLossRate
                      << '\n';
#ifdef __ANDROID__
            __android_log_print(
                ANDROID_LOG_INFO,
                "LSFG_FLOW",
                "runtime_session_id=%llu config_revision=%llu "
                "previous=%.3f requested=%.3f reason=%s flow_ms=%.3f lsfg_ms=%.3f "
                "budget_ms=%.3f generation_count=%zu gpu=%.1f pressure_valid=%d "
                "output_fps=%.3f output_deficit=%d output_satisfied=%d "
                "compute_pressure=%d wsi_pressure=%d wsi_loss_rate=%.3f",
                static_cast<unsigned long long>(this->runtimeSessionId_),
                static_cast<unsigned long long>(this->configRevision_),
                static_cast<double>(previousScale),
                static_cast<double>(selectedScale),
                AdaptiveFlowController::reasonName(flowTelemetry.reason),
                observation.flowMs,
                observation.totalLsfgMs,
                observation.frameBudgetMs,
                observation.generationCount,
                observation.globalGpuUsagePercent,
                observation.globalPressureValid ? 1 : 0,
                observation.outputFps,
                observation.outputDeficit ? 1 : 0,
                observation.outputTargetSatisfied ? 1 : 0,
                observation.computeDeadlinePressure ? 1 : 0,
                observation.wsiPresentationPressure ? 1 : 0,
                observation.wsiLossRate);
#endif
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
        this->adaptiveFlowBudgetMs_ = observationBudgetMs;
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
                      << " runtime_session_id=" << this->runtimeSessionId_
                      << " config_revision=" << this->configRevision_
                      << " source_fps=" << adaptiveTelemetry.sourceFps
                      << " smoothed_source_fps=" << adaptiveTelemetry.smoothedSourceFps
                      << " wanted_generated=" << adaptiveTelemetry.wantedGeneratedFrames
                      << " cost_limit=" << adaptiveTelemetry.costLimit
                      << " final_generated=" << adaptiveTelemetry.generatedFrames
                      << " rate_snap=" << (adaptiveTelemetry.sourceRateSnapped ? 1 : 0)
                      << " cost_raise=" << (adaptiveTelemetry.costRaised ? 1 : 0)
                      << " capacity_promoted="
                      << (adaptiveTelemetry.capacityPromoted ? 1 : 0)
                      << " safe_generation_hint="
                      << adaptiveTelemetry.safeGenerationHint
                      << " cost_backoff=" << (adaptiveTelemetry.costBackedOff ? 1 : 0)
                      << " cost_probe=" << (adaptiveTelemetry.costProbe ? 1 : 0)
                      << " discontinuity=" << (adaptiveTelemetry.discontinuityReset ? 1 : 0)
                      << "\n";
#ifdef __ANDROID__
            __android_log_print(
                ANDROID_LOG_INFO,
                "LSFG_EVENT",
                "runtime_session_id=%llu config_revision=%llu "
                "source_fps=%.3f smoothed_source_fps=%.3f wanted_generated=%.3f "
                "cost_limit=%zu final_generated=%zu rate_snap=%d cost_raise=%d "
                "capacity_promoted=%d safe_generation_hint=%zu "
                "cost_backoff=%d cost_probe=%d discontinuity=%d",
                static_cast<unsigned long long>(this->runtimeSessionId_),
                static_cast<unsigned long long>(this->configRevision_),
                adaptiveTelemetry.sourceFps,
                adaptiveTelemetry.smoothedSourceFps,
                adaptiveTelemetry.wantedGeneratedFrames,
                adaptiveTelemetry.costLimit,
                adaptiveTelemetry.generatedFrames,
                adaptiveTelemetry.sourceRateSnapped ? 1 : 0,
                adaptiveTelemetry.costRaised ? 1 : 0,
                adaptiveTelemetry.capacityPromoted ? 1 : 0,
                adaptiveTelemetry.safeGenerationHint,
                adaptiveTelemetry.costBackedOff ? 1 : 0,
                adaptiveTelemetry.costProbe ? 1 : 0,
                adaptiveTelemetry.discontinuityReset ? 1 : 0);
#endif
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
            metrics.windowGeneratedPresentationCapDrops = 0;
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
            metrics.windowHandoffPrevSourceDeps = 0;
            metrics.windowHandoffBatchDeps = 0;
            metrics.windowDeadlineShadowOpportunities = 0;
            metrics.windowDeadlineShadowWouldAdmit = 0;
            metrics.windowDeadlineShadowWouldReject = 0;
            metrics.windowCycleMs = 0.0;
            metrics.windowCycleMaxMs = 0.0;
            metrics.windowHandoffMs = 0.0;
            metrics.windowHandoffSubmitMs = 0.0;
            metrics.windowHandoffFenceWaitMs = 0.0;
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

        // Control feedback uses a fresh rolling LSFG presentation cadence, not
        // the previous completed one-second metrics window. A pause while this
        // present call is in flight clears stale evidence rather than creating
        // a false output deficit.
        if (cycleMs >= kRuntimeTimingDiscontinuityMs) {
            this->lsfgOutputCadenceTracker_.observe(
                std::chrono::milliseconds(250), 0, 0);
        } else if (sourceInterval.count() > 0) {
            this->lsfgOutputCadenceTracker_.observe(
                sourceInterval, 1, this->lastGeneratedFrameCount_);
        }

        const double elapsedSeconds = std::chrono::duration<double>(
            cycleEnd - metrics.windowStart).count();
        if (elapsedSeconds >= 1.0) {
            const double sourceCount = static_cast<double>(metrics.windowSourceFrames);
            const double generatedCount = static_cast<double>(metrics.windowGeneratedFrames);
            const double sourceFps = sourceCount / elapsedSeconds;
            const double generatedFps = generatedCount / elapsedSeconds;
            const double outputFps = (sourceCount + generatedCount) / elapsedSeconds;
            metrics.lastWindowOutputFps = outputFps;
            metrics.lastWindowOutputFpsValid = sourceCount > 0.0;
            metrics.lastWindowAdaptiveFramegen = conf.adaptiveFramegen;
            metrics.lastWindowTargetFps = conf.fpsLimit;
            const double cycleAvgMs = sourceCount > 0.0 ? metrics.windowCycleMs / sourceCount : 0.0;
            const double handoffAvgMs = sourceCount > 0.0 ? metrics.windowHandoffMs / sourceCount : 0.0;
            const double handoffSubmitAvgMs = sourceCount > 0.0
                ? metrics.windowHandoffSubmitMs / sourceCount : 0.0;
            const double handoffFenceWaitAvgMs = sourceCount > 0.0
                ? metrics.windowHandoffFenceWaitMs / sourceCount : 0.0;
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
            const auto& presentationTelemetry =
                this->generatedPresentationCapacityTracker_.telemetry();

            std::cerr << "lsfg-vk: metrics"
                      << " runtime_session_id=" << this->runtimeSessionId_
                      << " config_revision=" << this->configRevision_
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
                      << " presentation_cap_drops="
                      << metrics.windowGeneratedPresentationCapDrops
                      << " presentation_cap_drops_total="
                      << metrics.totalGeneratedPresentationCapDrops
                      << " presentation_cap="
                      << presentationTelemetry.generationCap
                      << " presentation_duty="
                      << presentationTelemetry.singleFrameDuty
                      << " wsi_reject_ratio="
                      << presentationTelemetry.wsiRejectionRatio
                      << " presentation_rejection_evidence="
                      << presentationTelemetry.rejectionEvidence
                      << " presentation_recovery_evidence="
                      << presentationTelemetry.recoveryEvidence
                      << " presentation_attempted_generated="
                      << presentationTelemetry.attemptedGeneratedFrames
                      << " presentation_accepted_generated="
                      << presentationTelemetry.acceptedGeneratedFrames
                      << " presentation_delivered_efficiency="
                      << presentationTelemetry.deliveredEfficiency
                      << " presentation_last_change_reason="
                      << generatedPresentationCapChangeReasonName(
                          presentationTelemetry.lastChangeReason)
                      << " presentation_last_change_output_deficit="
                      << (presentationTelemetry.lastChangeOutputDeficit ? 1 : 0)
                      << " presentation_provisional_lower="
                      << (presentationTelemetry.provisionalLowerActive ? 1 : 0)
                      << " presentation_upward_probe="
                      << (presentationTelemetry.upwardProbePending ? 1 : 0)
                      << " cycle_avg_ms=" << cycleAvgMs
                      << " cycle_max_ms=" << metrics.windowCycleMaxMs
                      << " ahb_handoff_avg_ms=" << handoffAvgMs
                      << " ahb_submit_avg_ms=" << handoffSubmitAvgMs
                      << " ahb_host_wait_avg_ms=" << handoffFenceWaitAvgMs
                      << " ahb_prev_source_deps=" << metrics.windowHandoffPrevSourceDeps
                      << " ahb_batch_deps=" << metrics.windowHandoffBatchDeps
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
                      << " deadline_bootstrap_probe="
                      << (adrenoDeadlineBootstrapProbe ? 1 : 0)
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
                      << " adaptive_flow_lsfg_output_valid="
                      << (this->lsfgOutputCadenceTracker_.snapshot().valid ? 1 : 0)
                      << " adaptive_flow_lsfg_output_fps="
                      << this->lsfgOutputCadenceTracker_.snapshot().outputFps
                      << " adaptive_flow_global_p95_ms="
                      << this->adaptiveFlowGlobalFrameTimeP95Ms_
                      << " adaptive_flow_global_slow_ratio="
                      << this->adaptiveFlowGlobalSlowFrameRatio_
                      << " adaptive_flow_global_pressure="
                      << (this->adaptiveFlowController_.telemetry().globalPressure ? 1 : 0)
                      << " adaptive_flow_output_deficit="
                      << (this->adaptiveFlowController_.telemetry().outputDeficit ? 1 : 0)
                      << " adaptive_flow_compute_pressure="
                      << (this->adaptiveFlowComputePressure_ ? 1 : 0)
                      << " adaptive_flow_wsi_pressure="
                      << (this->adaptiveFlowWsiPressure_ ? 1 : 0)
                      << " adaptive_flow_wsi_loss_rate="
                      << presentationTelemetry.wsiRejectionRatio
                      << " adaptive_flow_presentation_cap="
                      << presentationTelemetry.generationCap
                      << " adaptive_flow_presentation_duty="
                      << presentationTelemetry.singleFrameDuty
                      << " adaptive_flow_presentation_rejection_evidence="
                      << presentationTelemetry.rejectionEvidence
                      << " adaptive_flow_presentation_recovery_evidence="
                      << presentationTelemetry.recoveryEvidence
                      << " adaptive_flow_presentation_attempted_generated="
                      << presentationTelemetry.attemptedGeneratedFrames
                      << " adaptive_flow_presentation_accepted_generated="
                      << presentationTelemetry.acceptedGeneratedFrames
                      << " adaptive_flow_presentation_delivered_efficiency="
                      << presentationTelemetry.deliveredEfficiency
                      << " adaptive_flow_presentation_last_change_reason="
                      << generatedPresentationCapChangeReasonName(
                          presentationTelemetry.lastChangeReason)
                      << " adaptive_flow_presentation_last_change_output_deficit="
                      << (presentationTelemetry.lastChangeOutputDeficit ? 1 : 0)
                      << " adaptive_flow_presentation_provisional_lower="
                      << (presentationTelemetry.provisionalLowerActive ? 1 : 0)
                      << " adaptive_flow_presentation_upward_probe="
                      << (presentationTelemetry.upwardProbePending ? 1 : 0)
                      << " adaptive_flow_synthetic_drop_pressure="
                      << ((this->adaptiveFlowComputePressure_
                          || this->adaptiveFlowWsiPressure_) ? 1 : 0)
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

#ifdef __ANDROID__
            __android_log_print(
                ANDROID_LOG_INFO,
                "LSFG_METRICS",
                "runtime_session_id=%llu config_revision=%llu "
                "source_fps=%.3f generated_fps=%.3f output_fps=%.3f "
                "late=%llu admission=%llu deadline=%llu wsi=%llu cap_drop=%llu "
                "presentation_cap=%zu presentation_duty=%.3f wsi_reject_ratio=%.3f "
                "cycle_avg_ms=%.3f cycle_max_ms=%.3f handoff_ms=%.3f dispatch_ms=%.3f "
                "wait_ms=%.3f source_interval_ms=%.3f source_interval_max_ms=%.3f "
                "deadline_error_ms=%.3f rebases=%llu planned=%zu admitted=%zu "
                "pred_total_ms=%.3f reserve_ms=%.3f effective_budget_ms=%.3f "
                "wanted=%.3f cost_limit=%zu final_generated=%zu history_only=%llu "
                "flow_active=%.3f flow_gpu=%.1f flow_output_fps=%.3f "
                "flow_deficit=%d flow_reason=%s multiplier=%zu adaptive=%d target=%u",
                static_cast<unsigned long long>(this->runtimeSessionId_),
                static_cast<unsigned long long>(this->configRevision_),
                sourceFps,
                generatedFps,
                outputFps,
                static_cast<unsigned long long>(metrics.windowGeneratedLateDrops),
                static_cast<unsigned long long>(metrics.windowAdmissionRejects),
                static_cast<unsigned long long>(metrics.windowGeneratedDeadlineDrops),
                static_cast<unsigned long long>(metrics.windowGeneratedWsiDrops),
                static_cast<unsigned long long>(
                    metrics.windowGeneratedPresentationCapDrops),
                this->generatedPresentationCapacityTracker_.telemetry().generationCap,
                this->generatedPresentationCapacityTracker_.telemetry().singleFrameDuty,
                this->generatedPresentationCapacityTracker_.telemetry().wsiRejectionRatio,
                cycleAvgMs,
                metrics.windowCycleMaxMs,
                handoffAvgMs,
                dispatchAvgMs,
                waitIdleAvgMs,
                sourceIntervalAvgMs,
                metrics.windowSourceIntervalMaxMs,
                sourceDeadlineErrorAvgMs,
                static_cast<unsigned long long>(metrics.windowSourceTimelineRebases),
                plannedGeneratedFrameCount,
                generatedFrameCount,
                this->deadlineBatchDecision_.predictedTotalLsfgMs,
                this->deadlineBatchDecision_.deliveryReserveMs,
                this->deadlineBatchDecision_.effectiveUsableBudgetMs,
                adaptiveTelemetry.wantedGeneratedFrames,
                adaptiveTelemetry.costLimit,
                adaptiveTelemetry.generatedFrames,
                static_cast<unsigned long long>(metrics.windowAdaptiveZeroGenerationCycles),
                static_cast<double>(this->adaptiveFlowActiveScale_),
                this->adaptiveFlowGlobalGpuUsagePercent_,
                this->lsfgOutputCadenceTracker_.snapshot().outputFps,
                this->adaptiveFlowController_.telemetry().outputDeficit ? 1 : 0,
                AdaptiveFlowController::reasonName(this->adaptiveFlowReason_),
                conf.multiplier,
                conf.adaptiveFramegen ? 1 : 0,
                conf.fpsLimit);
            __android_log_print(
                ANDROID_LOG_INFO,
                "LSFG_METRICS",
                "presentation_rejection_evidence=%u "
                "presentation_recovery_evidence=%.3f "
                "presentation_attempted_generated=%llu "
                "presentation_accepted_generated=%llu "
                "presentation_delivered_efficiency=%.3f "
                "presentation_last_change_reason=%s "
                "presentation_last_change_output_deficit=%d "
                "presentation_provisional_lower=%d "
                "presentation_upward_probe=%d",
                presentationTelemetry.rejectionEvidence,
                presentationTelemetry.recoveryEvidence,
                static_cast<unsigned long long>(
                    presentationTelemetry.attemptedGeneratedFrames),
                static_cast<unsigned long long>(
                    presentationTelemetry.acceptedGeneratedFrames),
                presentationTelemetry.deliveredEfficiency,
                generatedPresentationCapChangeReasonName(
                    presentationTelemetry.lastChangeReason),
                presentationTelemetry.lastChangeOutputDeficit ? 1 : 0,
                presentationTelemetry.provisionalLowerActive ? 1 : 0,
                presentationTelemetry.upwardProbePending ? 1 : 0);
#endif

            metrics.windowStart = cycleEnd;
            metrics.windowSourceFrames = 0;
            metrics.windowGeneratedFrames = 0;
            metrics.windowGeneratedLateDrops = 0;
            metrics.windowAdmissionRejects = 0;
            metrics.windowGeneratedDeadlineDrops = 0;
            metrics.windowGeneratedWsiDrops = 0;
            metrics.windowGeneratedPresentationCapDrops = 0;
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
            metrics.windowHandoffPrevSourceDeps = 0;
            metrics.windowHandoffBatchDeps = 0;
            metrics.windowDeadlineShadowOpportunities = 0;
            metrics.windowDeadlineShadowWouldAdmit = 0;
            metrics.windowDeadlineShadowWouldReject = 0;
            metrics.windowCycleMs = 0.0;
            metrics.windowCycleMaxMs = 0.0;
            metrics.windowHandoffMs = 0.0;
            metrics.windowHandoffSubmitMs = 0.0;
            metrics.windowHandoffFenceWaitMs = 0.0;
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

    const auto armPassGpuRetirement = [&](VkQueue retirementQueue = VK_NULL_HANDLE) {
        const VkQueue targetQueue = retirementQueue != VK_NULL_HANDLE
            ? retirementQueue
            : info.queue.second;
        if (!this->submitPassCompletionFence(pass, targetQueue)) {
            Utils::logLimitN(
                "passRetirement",
                5,
                "Pass GPU completion fence unavailable; this slot will remain source-only");
        }
    };

    bool conservativeBatchStillInFlight = false;
    if (this->conservativeCrossDeviceSync_
            && this->conservativePendingBatchCompleteValid_) {
        bool batchReady = false;
        if (this->conservativePendingBatchCompletePollFd_ >= 0) {
            pollfd batchPoll{
                .fd = this->conservativePendingBatchCompletePollFd_,
                .events = POLLIN,
                .revents = 0,
            };
            const int pollResult = ::poll(&batchPoll, 1, 0);
            if (pollResult > 0 && (batchPoll.revents & POLLIN) != 0) {
                batchReady = true;
                ::close(this->conservativePendingBatchCompletePollFd_);
                this->conservativePendingBatchCompletePollFd_ = -1;
            } else if (pollResult < 0
                    || (pollResult > 0
                        && (batchPoll.revents
                            & (POLLERR | POLLHUP | POLLNVAL)) != 0)) {
                // Do not treat an fd error as proof that framegen released the
                // shared AHB pair. Drop the OS handle and fall back to the
                // zero-time context completion check below.
                ::close(this->conservativePendingBatchCompletePollFd_);
                this->conservativePendingBatchCompletePollFd_ = -1;
            }
        }
        if (!batchReady && this->conservativePendingBatchCompletePollFd_ < 0) {
            batchReady = conf.performance
                ? LSFG_3_1P::waitContext(*this->lsfgCtxId, 0)
                : LSFG_3_1::waitContext(*this->lsfgCtxId, 0);
        }
        if (batchReady) {
            this->conservativePendingBatchCompleteValid_ = false;
            this->conservativePendingBatchCompleteSemaphore_ = {};
        }
        conservativeBatchStillInFlight = !batchReady;
    }

    const bool conservativePreCopySourceBypass =
        this->conservativeCrossDeviceSync_
        && (conservativeTrueSourceOnlyCycle || conservativeBatchStillInFlight);
    if (conservativePreCopySourceBypass) {
        this->lastDispatchedGeneratedFrameCount_ = 0;
        this->lastGeneratedFrameCount_ = 0;
        this->previousSourceCopySignalValid_ = false;
        this->sourceHistoryWarmupRemaining_ =
            kConservativeSourceReprimeFrames - 1;
        this->requiresSourceHistoryWarmup_ = true;
        updateAdaptiveFlowGovernor();
        metrics.windowAdaptiveZeroGenerationCycles++;
        metrics.totalAdaptiveZeroGenerationCycles++;

        VkPresentTimeGOOGLE bypassPresentTime{};
        VkPresentTimesInfoGOOGLE bypassPresentTimes{};
        const VkPresentInfoKHR bypassPresentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = adaptivePresentPNext(
                pNext,
                this->currentSourceTimeline_.sourceDesiredTimeNs,
                bypassPresentTime, bypassPresentTimes),
            .waitSemaphoreCount =
                static_cast<uint32_t>(gameRenderSemaphores.size()),
            .pWaitSemaphores = gameRenderSemaphores.empty()
                ? nullptr : gameRenderSemaphores.data(),
            .swapchainCount = 1,
            .pSwapchains = &this->swapchain,
            .pImageIndices = &presentIdx,
        };
        const auto bypassResult =
            Layer::ovkQueuePresentKHR(queue, &bypassPresentInfo);
        if (this->conservativeCrossDeviceSync_ && isAdrenoWsiRetirementResult(bypassResult))
            return bypassResult;
        if (bypassResult != VK_SUCCESS
                && bypassResult != VK_SUBOPTIMAL_KHR) {
            metrics.windowSourcePresentFailures++;
            metrics.totalSourcePresentFailures++;
            throw LSFG::vulkan_error(
                bypassResult, "Failed protected Adreno source-only present");
        }
        if (firstPresentDiagnostic || adaptiveTelemetry.discontinuityReset) {
            std::cerr << "lsfg-vk: runtime stage="
                      << (conservativeBatchStillInFlight
                            ? "compat-source-batch-inflight-bypass"
                            : "compat-source-precopy-bypass")
                      << " generated=0"
                      << " planned=" << plannedGeneratedFrameCount
                      << " admitted=" << generatedFrameCount
                      << " history_warmup_remaining="
                      << this->sourceHistoryWarmupRemaining_
                      << "\n";
        }
        return finishSourcePresent(
            bypassResult, "game-render-precopy-bypass");
    }

    // Android path: AHardwareBuffer exchange between two VkDevices. Keep the
    // validated presentation sequence and EXTERNAL ownership barriers intact.

    // 1. Copy every active Adaptive source frame into frame_0/frame_1, even on
    // a zero-generation cadence cycle. That zero is cadence, not lifecycle: it
    // must refresh temporal history instead of entering the Off/source-only path.
    pass.preCopySemaphores.at(0) = Mini::Semaphore(info.device);
    pass.preCopySemaphores.at(1) = Mini::Semaphore(info.device);
    if (this->conservativeCrossDeviceSync_) {
        if (pass.preCopyBuf.getState() == Mini::CommandBufferState::Submitted)
            pass.preCopyBuf.reset();
    } else {
        pass.preCopyBuf = Mini::CommandBuffer(info.device, this->cmdPool);
    }
    pass.preCopyBuf.begin();

    const uint64_t sourceCopyIndex =
        this->conservativeCrossDeviceSync_
            ? this->conservativeFramegenSourceIndex_
            : this->frameIdx;
    copySwapchainToExternalAhb(pass.preCopyBuf.handle(),
        this->swapchainImages.at(presentIdx),
        sourceCopyIndex % 2 == 0 ? this->frame_0.handle() : this->frame_1.handle(),
        this->extent.width, this->extent.height,
        info.queue.first, sourceCopyIndex < 2);

    // Framegen owns a two-image source pair. On Adreno, source-only presents
    // intentionally do not advance framegen; initialize the second slot against
    // framegen's own source index rather than the wrapper present count.
    if (sourceCopyIndex == 0) {
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
    if (this->previousSourceCopySignalValid_ && previousPass != nullptr) {
        metrics.windowHandoffPrevSourceDeps++;
        pass.crossFrameWaitRetentions.emplace_back(
            previousPass->preCopySemaphores.at(1));
        gameRenderSemaphores2.emplace_back(
            pass.crossFrameWaitRetentions.back().handle());
    }
    bool consumeConservativeBatchComplete = false;
    if (this->conservativeCrossDeviceSync_
            && this->conservativePendingBatchCompleteValid_) {
        metrics.windowHandoffBatchDeps++;
        pass.crossFrameWaitRetentions.emplace_back(
            this->conservativePendingBatchCompleteSemaphore_);
        gameRenderSemaphores2.emplace_back(
            pass.crossFrameWaitRetentions.back().handle());
        consumeConservativeBatchComplete = true;
    } else if (!this->conservativeCrossDeviceSync_
            && previousPass != nullptr
            && previousPass->framegenBatchCompleteValid) {
        metrics.windowHandoffBatchDeps++;
        pass.crossFrameWaitRetentions.emplace_back(
            previousPass->framegenBatchCompleteSemaphore);
        gameRenderSemaphores2.emplace_back(
            pass.crossFrameWaitRetentions.back().handle());
        consumePreviousBatchComplete = true;
    }

    const auto handoffStart = RuntimeMetrics::Clock::now();
    std::vector<VkSemaphore> preCopySignals{
        pass.preCopySemaphores.at(0).handle(),
        pass.preCopySemaphores.at(1).handle(),
    };

    // Xclipse keeps the async handoff for every private-history/generation
    // cycle. Conservative Adreno exports one-shot SYNC_FD only for cycles that
    // actually generate. Reprime and fractional zero-generation cycles queue a
    // game-device copy for source-pair parity without crossing into framegen.
    bool useAsyncHandoff =
        this->asyncAhbHandoffEnabled_
        && (!this->conservativeCrossDeviceSync_
            || (!conservativeSourceOnlyWarmup
                && !conservativeFractionalHistoryGap
                && !conservativeTrueSourceOnlyCycle));
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
        const auto asyncSubmitStart = RuntimeMetrics::Clock::now();
        submitAhbHandoff(info.device, pass.preCopyBuf, info.queue.second,
            gameRenderSemaphores2, preCopySignals,
            VK_NULL_HANDLE, nullptr);
        metrics.windowHandoffSubmitMs +=
            std::chrono::duration<double, std::milli>(
                RuntimeMetrics::Clock::now() - asyncSubmitStart).count();
        asyncSubmissionIssued = true;
        if (consumeConservativeBatchComplete) {
            if (this->conservativePendingBatchCompletePollFd_ >= 0) {
                ::close(this->conservativePendingBatchCompletePollFd_);
                this->conservativePendingBatchCompletePollFd_ = -1;
            }
            this->conservativePendingBatchCompleteValid_ = false;
            this->conservativePendingBatchCompleteSemaphore_ = {};
        }
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

    bool queuedCopyWithoutHostWait = false;
    if (!useAsyncHandoff && !asyncSubmissionIssued) {
        const bool conservativeCopyOnlyHistory =
            conservativeSourceOnlyWarmup
            || conservativeFractionalHistoryGap;
        if (conservativeCopyOnlyHistory) {
            const auto copyOnlySubmitStart = RuntimeMetrics::Clock::now();
            submitAhbHandoff(
                info.device, pass.preCopyBuf, info.queue.second,
                gameRenderSemaphores2, preCopySignals,
                VK_NULL_HANDLE, nullptr);
            metrics.windowHandoffSubmitMs +=
                std::chrono::duration<double, std::milli>(
                    RuntimeMetrics::Clock::now() - copyOnlySubmitStart).count();
            queuedCopyWithoutHostWait = true;
        } else {
            submitAndWaitForAhbHandoff(
                info.device, pass.preCopyBuf, info.queue.second,
                gameRenderSemaphores2, preCopySignals,
                *this->ahbHandoffFence, this->resetHandoffFences,
                this->waitHandoffFences,
                &metrics.windowHandoffSubmitMs,
                &metrics.windowHandoffFenceWaitMs);
            metrics.windowSyncHandoffs++;
            metrics.totalSyncHandoffs++;
        }
        if (consumeConservativeBatchComplete) {
            if (this->conservativePendingBatchCompletePollFd_ >= 0) {
                ::close(this->conservativePendingBatchCompletePollFd_);
                this->conservativePendingBatchCompletePollFd_ = -1;
            }
            this->conservativePendingBatchCompleteValid_ = false;
            this->conservativePendingBatchCompleteSemaphore_ = {};
        }
        if (consumePreviousBatchComplete && previousPass != nullptr)
            previousPass->framegenBatchCompleteValid = false;
    }
    this->previousSourceCopySignalValid_ = true;
    metrics.windowHandoffMs += std::chrono::duration<double, std::milli>(
        RuntimeMetrics::Clock::now() - handoffStart).count();
    if (firstPresentDiagnostic) {
        std::cerr << "lsfg-vk: runtime stage=source-ahb-handoff-ready mode="
                  << (useAsyncHandoff
                        ? handoffTypeName(this->asyncAhbHandoffHandleType_)
                        : (queuedCopyWithoutHostWait
                            ? "queued-copy"
                            : "host-fence"))
                  << "\n";
    }

    if (asyncExportFailed) {
        this->lastDispatchedGeneratedFrameCount_ = 0;
        this->sourceHistoryWarmupRemaining_ =
            this->conservativeCrossDeviceSync_
                ? kConservativeSourceReprimeFrames
                : kSourceHistoryWarmupFrames;
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
        armPassGpuRetirement();
        this->retainPresentWait(presentIdx, pass.preCopySemaphores.at(0));
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

    const auto presentCompatibilitySourceOnly = [&](
            const char* stage, const char* sourceWait) -> VkResult {
        const VkSemaphore sourceReady = pass.preCopySemaphores.at(0).handle();
        VkPresentTimeGOOGLE sourcePresentTime{};
        VkPresentTimesInfoGOOGLE sourcePresentTimes{};
        const VkPresentInfoKHR sourcePresentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = adaptivePresentPNext(
                pNext,
                this->currentSourceTimeline_.sourceDesiredTimeNs,
                sourcePresentTime, sourcePresentTimes),
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &sourceReady,
            .swapchainCount = 1,
            .pSwapchains = &this->swapchain,
            .pImageIndices = &presentIdx,
        };
        armPassGpuRetirement();
        this->retainPresentWait(presentIdx, pass.preCopySemaphores.at(0));
        const auto sourceResult =
            Layer::ovkQueuePresentKHR(queue, &sourcePresentInfo);
        if (this->conservativeCrossDeviceSync_ && isAdrenoWsiRetirementResult(sourceResult))
            return sourceResult;
        if (sourceResult != VK_SUCCESS
                && sourceResult != VK_SUBOPTIMAL_KHR) {
            metrics.windowSourcePresentFailures++;
            metrics.totalSourcePresentFailures++;
            throw LSFG::vulkan_error(
                sourceResult, "Failed compatibility source-only present");
        }
        if (firstPresentDiagnostic || adaptiveTelemetry.discontinuityReset) {
            std::cerr << "lsfg-vk: runtime stage=" << stage
                      << " generated=0"
                      << " history_warmup_remaining="
                      << this->sourceHistoryWarmupRemaining_
                      << " sync_policy="
                      << AndroidSyncPolicy::crossDeviceSyncPolicyName(
                            this->conservativeCrossDeviceSync_)
                      << "\n";
        }
        return finishSourcePresent(sourceResult, sourceWait);
    };

    if (conservativeFractionalHistoryGap) {
        // The source pair is the only temporal state Adreno needs to preserve
        // across a fractional zero-generation cadence slot. The copy is queued
        // on the game device and the source present waits it GPU-side; do not
        // host-wait and do not run zero-count framegen preprocessing. The next
        // generated cycle waits the previous copy signal before updating the
        // opposite AHB slot, yielding two consecutive real source frames.
        this->lastDispatchedGeneratedFrameCount_ = 0;
        this->lastGeneratedFrameCount_ = 0;
        pass.framegenBatchCompleteValid = false;
        updateAdaptiveFlowGovernor();
        metrics.windowAdaptiveZeroGenerationCycles++;
        metrics.totalAdaptiveZeroGenerationCycles++;
        return presentCompatibilitySourceOnly(
            "compat-fractional-history-copy",
            "pre-copy-compat-fractional-history");
    }

    if (conservativeSourceOnlyWarmup) {
        this->lastDispatchedGeneratedFrameCount_ = 0;
        this->lastGeneratedFrameCount_ = 0;
        pass.framegenBatchCompleteValid = false;
        if (this->sourceHistoryWarmupRemaining_ > 0)
            --this->sourceHistoryWarmupRemaining_;
        this->requiresSourceHistoryWarmup_ =
            this->sourceHistoryWarmupRemaining_ > 0;
        updateAdaptiveFlowGovernor();
        metrics.windowAdaptiveZeroGenerationCycles++;
        metrics.totalAdaptiveZeroGenerationCycles++;
        return presentCompatibilitySourceOnly(
            "compat-source-warmup", "pre-copy-compat-warmup");
    }

    if (historyOnly) {
        this->lastDispatchedGeneratedFrameCount_ = 0;
        const auto adaptiveFlowBatch = nextAdaptiveFlowBatch();
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
                    this->asyncAhbHandoffHandleType_, 0, adaptiveFlowBatch)
                : LSFG_3_1::presentContextWithCountExportSyncFd(
                    *this->lsfgCtxId, framegenInputSemaphoreFd, 0,
                    this->asyncAhbHandoffHandleType_, 0, adaptiveFlowBatch);

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
                        : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
                    0, adaptiveFlowBatch);
            else
                LSFG_3_1::presentContextWithCount(
                    *this->lsfgCtxId,
                    useAsyncHandoff ? framegenInputSemaphoreFd : -1,
                    noOutSems, 0,
                    useAsyncHandoff
                        ? this->asyncAhbHandoffHandleType_
                        : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
                    0, adaptiveFlowBatch);

            // Without an exported completion dependency, framegen's private
            // VkDevice must finish its zero-count preprocessing before the game
            // device can reuse either shared AHB on the next source cycle.
            historyRequiresHostCompletionWait = true;
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
                armPassGpuRetirement();
                this->retainPresentWait(presentIdx, pass.preCopySemaphores.at(0));
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
        armPassGpuRetirement();
        this->retainPresentWait(presentIdx, pass.preCopySemaphores.at(0));
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
    const auto adaptiveFlowBatch = nextAdaptiveFlowBatch();

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
                interpolationGenerationCount, adaptiveFlowBatch)
            : LSFG_3_1::presentContextWithCountExportSyncFd(
                *this->lsfgCtxId, framegenInputSemaphoreFd,
                generatedFrameCount, this->asyncAhbHandoffHandleType_,
                interpolationGenerationCount, adaptiveFlowBatch);
    } else if (conf.performance) {
        LSFG_3_1P::presentContextWithCount(
            *this->lsfgCtxId, framegenInputSemaphoreFd, noOutSems,
            generatedFrameCount, this->asyncAhbHandoffHandleType_,
                interpolationGenerationCount, adaptiveFlowBatch);
    } else {
        LSFG_3_1::presentContextWithCount(
            *this->lsfgCtxId, framegenInputSemaphoreFd, noOutSems,
            generatedFrameCount, this->asyncAhbHandoffHandleType_,
                interpolationGenerationCount, adaptiveFlowBatch);
    }
    if (this->conservativeCrossDeviceSync_)
        ++this->conservativeFramegenSourceIndex_;
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
        if (adrenoSingleQueueReadinessPoll && !importFailed) {
            size_t readyGeneratedFrameCount = 0;
            bool readyPrefix = true;
            for (size_t i = 0; i < generatedFrameCount; ++i) {
                int fd = framegenSync.outputReadyFds.at(i);
                bool outputReady = fd < 0;
                if (fd >= 0) {
                    pollfd outputPoll{
                        .fd = fd,
                        .events = POLLIN,
                        .revents = 0,
                    };
                    const int pollResult = ::poll(&outputPoll, 1, 0);
                    outputReady = pollResult > 0
                        && (outputPoll.revents & POLLIN) != 0;
                    ::close(fd);
                    framegenSync.outputReadyFds.at(i) = -1;
                }
                if (readyPrefix && outputReady) {
                    ++readyGeneratedFrameCount;
                } else {
                    readyPrefix = false;
                }
            }

            if (framegenSync.batchCompleteFd >= 0) {
                if (this->conservativePendingBatchCompletePollFd_ >= 0)
                    ::close(this->conservativePendingBatchCompletePollFd_);
                this->conservativePendingBatchCompletePollFd_ =
                    framegenSync.batchCompleteFd;
                framegenSync.batchCompleteFd = -1;
                this->conservativePendingBatchCompleteSemaphore_ = {};
                this->conservativePendingBatchCompleteValid_ = true;
            } else {
                this->conservativePendingBatchCompleteValid_ = false;
                this->conservativePendingBatchCompleteSemaphore_ = {};
            }

            if (readyGeneratedFrameCount < generatedFrameCount) {
                const size_t droppedGeneratedFrames =
                    generatedFrameCount - readyGeneratedFrameCount;
                metrics.windowGeneratedLateDrops += droppedGeneratedFrames;
                metrics.totalGeneratedLateDrops += droppedGeneratedFrames;
                metrics.windowGeneratedDeadlineDrops += droppedGeneratedFrames;
                metrics.totalGeneratedDeadlineDrops += droppedGeneratedFrames;
                if (firstPresentDiagnostic) {
                    std::cerr << "lsfg-vk: runtime stage=generated-readiness-drop"
                              << " planned=" << generatedFrameCount
                              << " ready=" << readyGeneratedFrameCount
                              << " dropped=" << droppedGeneratedFrames
                              << "\n";
                }
                generatedFrameCount = readyGeneratedFrameCount;
            }
            requireHostCompletionWait = false;
        } else {
        try {
            if (!importFailed) {
                for (size_t i = 0; i < generatedFrameCount; ++i) {
                    const int fd = framegenSync.outputReadyFds.at(i);
                    if (fd < 0)
                        continue;
                    // Mini::Semaphore consumes/closes a SYNC_FD when import
                    // fails. On the Adreno compatibility path transfer ownership
                    // before import so fallback cleanup cannot double-close a
                    // descriptor that the constructor already consumed.
                    if (this->conservativeCrossDeviceSync_)
                        framegenSync.outputReadyFds.at(i) = -1;
                    pass.renderSemaphores.at(i) = Mini::Semaphore(
                        info.device, fd,
                        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
                    if (!this->conservativeCrossDeviceSync_)
                        framegenSync.outputReadyFds.at(i) = -1;
                    outputReadyWaitValid.at(i) = true;
                }
                if (framegenSync.batchCompleteFd >= 0) {
                    const int batchCompleteFd = framegenSync.batchCompleteFd;
                    int batchCompletePollFd = -1;
                    if (this->conservativeCrossDeviceSync_)
                        batchCompletePollFd = ::dup(batchCompleteFd);
                    if (this->conservativeCrossDeviceSync_)
                        framegenSync.batchCompleteFd = -1;
                    pass.framegenBatchCompleteSemaphore = Mini::Semaphore(
                        info.device, batchCompleteFd,
                        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
                    if (!this->conservativeCrossDeviceSync_)
                        framegenSync.batchCompleteFd = -1;
                    pass.framegenBatchCompleteValid = true;
                    if (this->conservativeCrossDeviceSync_) {
                        if (this->conservativePendingBatchCompletePollFd_ >= 0)
                            ::close(this->conservativePendingBatchCompletePollFd_);
                        this->conservativePendingBatchCompletePollFd_ =
                            batchCompletePollFd;
                        batchCompletePollFd = -1;
                        this->conservativePendingBatchCompleteSemaphore_ =
                            pass.framegenBatchCompleteSemaphore;
                        this->conservativePendingBatchCompleteValid_ = true;
                    }
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
        armPassGpuRetirement();
        this->retainPresentWait(presentIdx, pass.preCopySemaphores.at(0));
        const auto timeoutPresentResult = Layer::ovkQueuePresentKHR(queue, &timeoutPresentInfo);
        if (timeoutPresentResult != VK_SUCCESS && timeoutPresentResult != VK_SUBOPTIMAL_KHR) {
            metrics.windowSourcePresentFailures++;
            metrics.totalSourcePresentFailures++;
            throw LSFG::vulkan_error(timeoutPresentResult,
                "Failed to present source frame after framegen timeout");
        }
        if (adrenoSingleQueueReadinessPoll) {
            if (this->conservativePendingBatchCompletePollFd_ >= 0) {
                ::close(this->conservativePendingBatchCompletePollFd_);
                this->conservativePendingBatchCompletePollFd_ = -1;
            }
            this->conservativePendingBatchCompleteSemaphore_ = {};
            this->conservativePendingBatchCompleteValid_ = true;
            this->sourceHistoryWarmupRemaining_ =
                kConservativeSourceReprimeFrames - 1;
            this->requiresSourceHistoryWarmup_ = true;
            return finishSourcePresent(
                timeoutPresentResult, "framegen-timeout-source-only");
        }
        return finishSourcePresent(VK_ERROR_OUT_OF_DATE_KHR, "pre-copy-timeout");
    }
    if (firstPresentDiagnostic)
        std::cerr << "lsfg-vk: runtime stage=framegen-idle-ready"
                  << " host_wait=" << (requireHostCompletionWait ? 1 : 0)
                  << "\n";
    updateAdaptiveFlowGovernor();

    const bool useAdrenoSyntheticQueue =
        this->conservativeCrossDeviceSync_ && info.adrenoSyntheticQueueAvailable
        && this->syntheticQueue_ != VK_NULL_HANDLE;
    const VkQueue generatedWorkQueue = useAdrenoSyntheticQueue
        ? this->syntheticQueue_
        : info.queue.second;
    const VkQueue generatedPresentQueue = useAdrenoSyntheticQueue
        ? this->syntheticQueue_
        : queue;

    // 4. Generated presentation is opportunistic. Never wait for a synthetic
    // swapchain image: if WSI has no image immediately available, drop this and
    // the remaining synthetic opportunities so the real source present can be
    // queued without generated-frame backpressure.
    size_t queuedGeneratedFrameCount = 0;
    size_t generatedWsiRejectedFrameCount = 0;
    bool generatedDeadlineObservationEligible = true;
    for (size_t i = 0; i < generatedFrameCount; i++) {
        const auto generatedPresentStart = RuntimeMetrics::Clock::now();
        const double syntheticFraction =
            static_cast<double>(i + 1)
            / static_cast<double>(interpolationGenerationCount + 1);
        const uint64_t syntheticDesiredTimeNs =
            this->sourceTimeline_.syntheticDesiredTimeNs(
                this->currentSourceTimeline_, syntheticFraction);
        const uint64_t syntheticAdmissionNowNs = monotonicNowNs();
        // Post-dispatch rejection is source-safe whenever completion is GPU-side.
        // Xclipse retains its existing path. Adreno joins it only while the r12
        // SYNC_FD completion chain is active; host-fallback Adreno keeps the
        // conservative pre-admission behavior.
        const bool enforcePostDispatchSyntheticDeadline =
            conf.adaptiveFramegen
            && (!this->conservativeCrossDeviceSync_ || this->asyncFramegenCompletionEnabled_);
        if (enforcePostDispatchSyntheticDeadline
                && syntheticDesiredTimeNs > 0
                && syntheticAdmissionNowNs >= syntheticDesiredTimeNs) {
            const double deliveryLatenessMs =
                static_cast<double>(
                    syntheticAdmissionNowNs - syntheticDesiredTimeNs)
                / 1'000'000.0;
            this->deadlineAdmissionPredictor_.observeDeliveryMiss(
                deliveryLatenessMs);
            generatedDeadlineObservationEligible = false;
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
        const uint64_t generatedAcquireTimeoutNs =
            !conf.adaptiveFramegen && this->conservativeCrossDeviceSync_ && !this->asyncFramegenCompletionEnabled_
                ? runtimeWaitTimeoutNs()
                : 0;
        auto res = Layer::ovkAcquireNextImageKHR(
            info.device, this->swapchain, generatedAcquireTimeoutNs,
            pass.acquireSemaphores.at(i).handle(), VK_NULL_HANDLE, &imageIdx);
        if (res == VK_NOT_READY || res == VK_TIMEOUT) {
            // Swapchain-image availability is downstream presentation
            // capacity, not evidence that framegen compute missed its source
            // deadline. Keep the deadline predictor trained only on actual
            // submit-to-deadline lateness.
            const size_t droppedGeneratedFrames = generatedFrameCount - i;
            generatedWsiRejectedFrameCount = droppedGeneratedFrames;
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
        if (this->conservativeCrossDeviceSync_
                && isAdrenoWsiRetirementResult(res))
            return res;
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
            metrics.windowGeneratedPresentFailures++;
            metrics.totalGeneratedPresentFailures++;
            throw LSFG::vulkan_error(res, "Failed to acquire next swapchain image");
        }
        this->releasePresentWaitRetirements(imageIdx);

        pass.postCopySemaphores.at(i) = Mini::Semaphore(info.device);
        pass.prevPostCopySemaphores.at(i) = Mini::Semaphore(info.device);
        auto& postCopyBuf = pass.postCopyBufs.at(i);
        if (this->conservativeCrossDeviceSync_) {
            if (postCopyBuf.getState() == Mini::CommandBufferState::Submitted)
                postCopyBuf.reset();
        } else {
            postCopyBuf = Mini::CommandBuffer(info.device, this->cmdPool);
        }
        postCopyBuf.begin();

        copyExternalAhbToSwapchain(postCopyBuf.handle(),
            this->out_n.at(i).handle(),
            this->swapchainImages.at(imageIdx),
            this->extent.width, this->extent.height,
            info.queue.first);

        postCopyBuf.end();
        std::vector<VkSemaphore> generatedCopyWaits{
            pass.acquireSemaphores.at(i).handle()
        };
        if (outputReadyWaitValid.at(i))
            generatedCopyWaits.emplace_back(pass.renderSemaphores.at(i).handle());
        postCopyBuf.submit(generatedWorkQueue,
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
        this->retainPresentWait(imageIdx, pass.postCopySemaphores.at(i));
        if (i != 0)
            this->retainPresentWait(
                imageIdx, pass.prevPostCopySemaphores.at(i - 1));
        res = Layer::ovkQueuePresentKHR(generatedPresentQueue, &presentInfo);
        if (this->conservativeCrossDeviceSync_
                && isAdrenoWsiRetirementResult(res))
            return res;
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

    if (conf.adaptiveFramegen
            && generatedFrameCount > 0
            && generatedDeadlineObservationEligible) {
        this->generatedPresentationCapacityTracker_.observe(
            generatedFrameCount,
            queuedGeneratedFrameCount,
            generatedWsiRejectedFrameCount,
            presentationCapacityContext);
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
    if (queuedGeneratedFrameCount > 0)
        armPassGpuRetirement(generatedWorkQueue);
    else
        armPassGpuRetirement();
    if (queuedGeneratedFrameCount > 0) {
        this->retainPresentWait(
            presentIdx,
            pass.prevPostCopySemaphores.at(queuedGeneratedFrameCount - 1));
    } else {
        this->retainPresentWait(presentIdx, pass.preCopySemaphores.at(0));
    }
    const VkQueue finalSourcePresentQueue =
        queuedGeneratedFrameCount > 0 && useAdrenoSyntheticQueue
            ? generatedPresentQueue
            : queue;
    auto res = Layer::ovkQueuePresentKHR(
        finalSourcePresentQueue, &finalPresentInfo);
    if (this->conservativeCrossDeviceSync_
            && isAdrenoWsiRetirementResult(res))
        return res;
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
    if (this->frameIdx > 0) {
        pass.crossFrameWaitRetentions.emplace_back(
            this->passInfos.at((this->frameIdx - 1) % 8).preCopySemaphores.at(1));
        gameRenderSemaphores2.emplace_back(
            pass.crossFrameWaitRetentions.back().handle());
    }
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
        this->releasePresentWaitRetirements(imageIdx);

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
        this->retainPresentWait(imageIdx, pass.postCopySemaphores.at(i));
        if (i != 0)
            this->retainPresentWait(
                imageIdx, pass.prevPostCopySemaphores.at(i - 1));
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
    this->submitPassCompletionFence(pass, info.queue.second);
    this->retainPresentWait(
        presentIdx,
        pass.prevPostCopySemaphores.at(conf.multiplier - 1 - 1));
    auto res = Layer::ovkQueuePresentKHR(queue, &presentInfo);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
        throw LSFG::vulkan_error(res, "Failed to present swapchain image");

    this->frameIdx++;
    return res;
#endif
}

#ifdef __ANDROID__
void LsContext::advanceAdaptiveFlowTimingEpoch() {
    ++this->adaptiveFlowTimingEpoch_;
    if (this->adaptiveFlowTimingEpoch_ == 0)
        this->adaptiveFlowTimingEpoch_ = 1;
    this->adaptiveFlowNextBatchId_ = 0;
    this->adaptiveFlowLastObservedBatchId_ = 0;
    this->adaptiveFlowGeneratedTimingValid_ = false;
    this->adaptiveFlowRetainedMipmapsMs_ = 0.0;
    this->adaptiveFlowRetainedWorkMs_ = 0.0;
    this->adaptiveFlowRetainedTotalLsfgMs_ = 0.0;
    this->adaptiveFlowRetainedBudgetMs_ = 0.0;
    this->adaptiveFlowRetainedGenerationCount_ = 0;
    this->adaptiveFlowTimingValid_ = false;
    this->adaptiveFlowMipmapsMs_ = 0.0;
    this->adaptiveFlowWorkMs_ = 0.0;
    this->adaptiveFlowTotalLsfgMs_ = 0.0;
    this->adaptiveFlowBudgetMs_ = 0.0;
    this->adaptiveFlowGenerationCount_ = 0;
}

void LsContext::resetAdaptiveSourceEpoch(bool resetScheduler) {
    if (resetScheduler)
        this->adaptiveScheduler_.reset();
    this->advanceAdaptiveFlowTimingEpoch();
    this->deadlineAdmissionPredictor_.reset();
    this->generatedPresentationCapacityTracker_.reset();
    this->lsfgOutputCadenceTracker_.reset();
    this->deadlineBatchDecision_ = {};
    this->sourceTimeline_.reset();
    this->currentSourceTimeline_ = {};
    this->adaptivePresentPeriodNs_ = 0;

    this->sourceHistoryWarmupRemaining_ =
        this->conservativeCrossDeviceSync_ ? kConservativeSourceReprimeFrames
                                           : kSourceHistoryWarmupFrames;
    this->requiresSourceHistoryWarmup_ =
        this->sourceHistoryWarmupRemaining_ > 0;
    this->lastGeneratedFrameCount_ = 0;
    this->lastDispatchedGeneratedFrameCount_ = 0;

    this->adaptiveFlowController_.reset();
    this->adaptiveFlowNextPressureRead_ = {};
    this->adaptiveFlowGlobalPressureValid_ = false;
    this->adaptiveFlowGlobalGpuUsagePercent_ = 0.0;
    this->adaptiveFlowGlobalOutputFps_ = 0.0;
    this->adaptiveFlowGlobalFrameTimeP95Ms_ = 0.0;
    this->adaptiveFlowGlobalSlowFrameRatio_ = 0.0;
    this->adaptiveFlowLastObservedComputeDrops_ =
        this->runtimeMetrics.totalAdmissionRejects
        + this->runtimeMetrics.totalGeneratedDeadlineDrops;
    this->adaptiveFlowLastObservedWsiDrops_ =
        this->runtimeMetrics.totalGeneratedWsiDrops;
    this->adaptiveFlowComputePressure_ = false;
    this->adaptiveFlowWsiPressure_ = false;
    this->adaptiveFlowRequestedScale_ =
        this->adaptiveFlowController_.currentScale();
    this->adaptiveFlowActiveScale_ =
        this->adaptiveFlowController_.currentScale();
    this->adaptiveFlowWarmupRemaining_ = 0;
    this->adaptiveFlowTransitionPending_ = false;
    this->adaptiveFlowReason_ =
        this->adaptiveFlowController_.telemetry().reason;
}

void LsContext::enterSourceOnlyBypass() {
    this->advanceAdaptiveFlowTimingEpoch();
    this->adaptiveScheduler_.reset();
    this->deadlineAdmissionPredictor_.reset();
    this->adaptiveFlowController_.reset();
    this->sourceTimeline_.reset();
    this->currentSourceTimeline_ = {};
    this->deadlineBatchDecision_ = {};
    this->adaptiveFlowNextPressureRead_ = {};
    this->adaptiveFlowGlobalPressureValid_ = false;
    this->adaptiveFlowGlobalGpuUsagePercent_ = 0.0;
    this->adaptiveFlowGlobalOutputFps_ = 0.0;
    this->adaptiveFlowGlobalFrameTimeP95Ms_ = 0.0;
    this->adaptiveFlowGlobalSlowFrameRatio_ = 0.0;
    this->lastGeneratedFrameCount_ = 0;
    this->sourceHistoryWarmupRemaining_ =
        this->conservativeCrossDeviceSync_ ? kConservativeSourceReprimeFrames
                                           : kSourceHistoryWarmupFrames;
    this->requiresSourceHistoryWarmup_ =
        this->sourceHistoryWarmupRemaining_ > 0;
    this->lastDispatchedGeneratedFrameCount_ = 0;
    this->previousSourceCopySignalValid_ = false;
    this->generatedPresentationCapacityTracker_.reset();
    this->lsfgOutputCadenceTracker_.reset();
    this->adaptiveFlowLastObservedComputeDrops_ =
        this->runtimeMetrics.totalAdmissionRejects
        + this->runtimeMetrics.totalGeneratedDeadlineDrops;
    this->adaptiveFlowLastObservedWsiDrops_ =
        this->runtimeMetrics.totalGeneratedWsiDrops;
    this->adaptiveFlowComputePressure_ = false;
    this->adaptiveFlowWsiPressure_ = false;
    this->adaptiveFlowRequestedScale_ =
        this->adaptiveFlowController_.currentScale();
    this->adaptiveFlowActiveScale_ =
        this->adaptiveFlowController_.currentScale();
    this->adaptiveFlowWarmupRemaining_ = 0;
    this->adaptiveFlowTransitionPending_ = false;
    this->adaptiveFlowReason_ = adaptiveFlowController_.telemetry().reason;
    this->runtimeMetrics.hasLastSourcePresent = false;
    this->runtimeMetrics.lastSourcePresent = {};
    this->adaptivePresentPeriodNs_ = 0;
}
#endif

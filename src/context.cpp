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
#include <string_view>
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

const char* sourceHistoryInvalidationReasonName(
        SourceHistoryInvalidationReason reason) noexcept {
    switch (reason) {
        case SourceHistoryInvalidationReason::None: return "none";
        case SourceHistoryInvalidationReason::Startup: return "startup";
        case SourceHistoryInvalidationReason::TimelineDiscontinuity:
            return "timeline_discontinuity";
        case SourceHistoryInvalidationReason::RuntimeConfigChange:
            return "runtime_config_change";
        case SourceHistoryInvalidationReason::SuspendResume:
            return "suspend_resume";
        case SourceHistoryInvalidationReason::SyncExportFailure:
            return "sync_export_failure";
        case SourceHistoryInvalidationReason::SyncImportFailure:
            return "sync_import_failure";
        case SourceHistoryInvalidationReason::ContextRecreate:
            return "context_recreate";
        case SourceHistoryInvalidationReason::SourcePairMismatch:
            return "source_pair_mismatch";
        case SourceHistoryInvalidationReason::AdmissionBypass:
            return "admission_bypass";
        case SourceHistoryInvalidationReason::OverloadBypass:
            return "overload_bypass";
        case SourceHistoryInvalidationReason::RetirementBackpressure:
            return "retirement_backpressure";
        case SourceHistoryInvalidationReason::FractionalGap:
            return "fractional_gap";
        case SourceHistoryInvalidationReason::AbandonedBatch:
            return "abandoned_batch";
        case SourceHistoryInvalidationReason::TrueOwnershipFailure:
            return "true_ownership_failure";
    }
    return "unknown";
}

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

bool shouldTraceBatch(uint64_t batchId) {
    static const bool traceEveryBatch = [] {
        const char* value = std::getenv("LSFG_VK_BATCH_TRACE");
        return value != nullptr && std::string_view(value) == "1";
    }();
    return traceEveryBatch || batchId <= 8 || batchId % 120 == 0;
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
    VkPhysicalDeviceProperties gameDeviceProperties{};
    Layer::ovkGetPhysicalDeviceProperties(
        info.physicalDevice, &gameDeviceProperties);
    this->compatibilityPath_ =
        AndroidSyncPolicy::selectFramegenCompatibilityPath(
            backendDiagnostics.driverId,
            backendDiagnostics.driverName,
            gameDeviceProperties.deviceName);
    this->conservativeCrossDeviceSync_ =
        this->compatibilityPath_
            == AndroidSyncPolicy::FramegenCompatibilityPath::AdrenoLatestKnownGood;

    // Presentation-engine confirmation is telemetry-only. VK_GOOGLE_display_timing
    // was already enabled opportunistically at device creation; on Xclipse/generic
    // paths resolve its asynchronous history query and leave desiredPresentTime=0
    // so this cannot change present cadence. Keep the device-proven Adreno path
    // completely untouched.
    if (info.androidDisplayTimingSupported
            && !this->conservativeCrossDeviceSync_) {
        this->getPastPresentationTimingGoogle_ =
            reinterpret_cast<PFN_vkGetPastPresentationTimingGOOGLE>(
                Layer::ovkGetDeviceProcAddr(
                    info.device, "vkGetPastPresentationTimingGOOGLE"));
    }
    this->generatedDisplayConfirmationEnabled_ =
        this->getPastPresentationTimingGoogle_ != nullptr;
    std::cerr << "lsfg-vk: display-confirmation"
              << " capability=" << (info.androidDisplayTimingSupported ? 1 : 0)
              << " protected_adreno="
              << (this->conservativeCrossDeviceSync_ ? 1 : 0)
              << " backend="
              << (this->generatedDisplayConfirmationEnabled_
                    ? "google-display-timing"
                    : "unavailable")
              << " pacing_changed=0\n";

    // The device-proven Adreno path never selects a synthetic queue.
    // Ordinary generated cycles use one-shot OPAQUE_FD source handoff and keep
    // completion on the bounded private-device host wait below.
    if (this->conservativeCrossDeviceSync_)
        this->syntheticQueue_ = VK_NULL_HANDLE;
    // Source ownership and framegen completion are independent policies.
    // The validated September 18 Android artifact composed the SYNC_FD source
    // handoff transform on top of the 364178af execution topology. On the
    // S20+/Turnip path OPAQUE_FD was unavailable while SYNC_FD was available;
    // ordinary generated cycles therefore used a GPU SYNC_FD handoff, followed
    // by the same bounded private-device host completion before generated AHBs
    // were consumed. Warmup and zero-generation/history cycles remain on the
    // conservative host-fence path below.
    //
    // Prefer SYNC_FD when both devices expose it. OPAQUE_FD remains a valid
    // fallback for drivers that genuinely support it. This selection is the
    // same capability order already used by the non-conservative/Xclipse path,
    // so Xclipse routing and completion behavior are unchanged.
    this->asyncAhbHandoffEnabled_ =
        gameGetSemaphoreFd != nullptr
        && (syncFdHandoffSupported || opaqueFdHandoffSupported);
    this->asyncAhbHandoffHandleType_ =
        syncFdHandoffSupported
            ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
            : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    // September 18's proven Adreno 6xx topology keeps zero-generation
    // temporal preprocessing conservative: host-fence the source copy, run the
    // private zero-count pass, and return only after its bounded private-device
    // completion. Do not carry a private SYNC_FD release into the next game
    // source copy; enabling that path caused the S20+ Fixed-mode r28 regression.
    // Xclipse/generic asynchronous completion remains selected independently
    // below through asyncFramegenCompletionEnabled_.
    this->asyncHistoryCompletionEnabled_ = false;
    this->asyncFramegenCompletionEnabled_ =
        !this->conservativeCrossDeviceSync_
        && syncFdHandoffSupported
        && gameImportSemaphoreFd != nullptr;
    // Rejected Adreno experiment: never select deferred completion/source
    // buffering on the device-proven compatibility path. The dormant code stays
    // isolated for lifetime-audit reference, but no compatibility selector can
    // route Qualcomm/Turnip into it.
    this->deferredAdrenoCompletionEnabled_ = false;

    const bool xclipseCompatibilityPath =
        this->compatibilityPath_
            == AndroidSyncPolicy::FramegenCompatibilityPath::XclipseCurrent;
    const char* compatibilityCompletion =
        this->asyncFramegenCompletionEnabled_
            ? "sync-fd"
            : (this->deferredAdrenoCompletionEnabled_
                ? "deferred-sync-fd"
                : "host-wait");
    const char* compatibilityPresentation =
        this->compatibilityPath_
                == AndroidSyncPolicy::FramegenCompatibilityPath::AdrenoLatestKnownGood
            ? "generated-before-source-same-call"
            : (xclipseCompatibilityPath ? "xclipse-current" : "capability-current");
    const char* compatibilityRetirement =
        this->compatibilityPath_
                == AndroidSyncPolicy::FramegenCompatibilityPath::AdrenoLatestKnownGood
            ? "host-completion+real-copy-fence+wsi-reacquire"
            : (xclipseCompatibilityPath ? "xclipse-current" : "capability-current");

    std::cerr << "lsfg-vk: LSFG compatibility path:"
              << " gpu=\"" << gameDeviceProperties.deviceName << "\""
              << " vendor="
              << AndroidSyncPolicy::compatibilityVendorName(
                    this->compatibilityPath_)
              << " path="
              << AndroidSyncPolicy::compatibilityPathName(
                    this->compatibilityPath_)
              << " completion=" << compatibilityCompletion
              << " history_completion="
              << (this->conservativeCrossDeviceSync_
                    ? (this->asyncHistoryCompletionEnabled_
                        ? "sync-fd-release"
                        : "host-wait")
                    : (this->asyncFramegenCompletionEnabled_
                        ? "sync-fd"
                        : "host-wait"))
              << " handoff="
              << (this->asyncAhbHandoffEnabled_
                    ? handoffTypeName(this->asyncAhbHandoffHandleType_)
                    : "host-fence")
              << " presentation=" << compatibilityPresentation
              << " retirement=" << compatibilityRetirement
              << " queue_topology="
              << (this->syntheticQueue_ != VK_NULL_HANDLE
                    ? "split-graphics"
                    : "single-graphics")
              << " source_queue=application-present"
              << " generated_queue="
              << (this->syntheticQueue_ != VK_NULL_HANDLE
                    ? "synthetic-graphics"
                    : "application-present")
              << " synthetic_queue="
              << (this->syntheticQueue_ != VK_NULL_HANDLE ? 1 : 0)
              << " deadline_semantics="
              << (this->conservativeCrossDeviceSync_
                    ? "source-protection"
                    : "synthetic-slot")
              << " execution_reference="
              << (this->conservativeCrossDeviceSync_
                    ? "364178af-sep18"
                    : "current-capability")
              << " governor_adapter="
              << (this->conservativeCrossDeviceSync_
                    ? "source-protected"
                    : "native-current")
              << " fixed_generation="
              << (this->conservativeCrossDeviceSync_
                    ? "source-cadence-governed"
                    : "native-current")
              << " behavior_changed=" << (this->compatibilityPath_ == AndroidSyncPolicy::FramegenCompatibilityPath::AdrenoLatestKnownGood ? 1 : 0)
              << '\n';

    // September 18 starts the protected Adreno context immediately: the
    // first generated cycle initializes frame_0/frame_1 through wrapper frameIdx
    // parity and does not insert a synthetic startup warmup. A real Off/reset
    // transition below still requests exactly one source-only warmup.
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
              << (this->asyncFramegenCompletionEnabled_
                    ? "sync-fd"
                    : (this->deferredAdrenoCompletionEnabled_
                        ? "deferred-sync-fd"
                        : "host-wait"))
              << ", historyCompletion="
              << (this->conservativeCrossDeviceSync_
                    ? (this->asyncHistoryCompletionEnabled_
                        ? "sync-fd-release"
                        : "host-wait")
                    : (this->asyncFramegenCompletionEnabled_
                        ? "sync-fd"
                        : "host-wait"))
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
        const auto createOwnedCompletionFence = [&]() {
            VkFence fence = VK_NULL_HANDLE;
            const auto fenceResult = createCompletionFence(
                info.device, &fenceInfo, nullptr, &fence);
            if (fenceResult != VK_SUCCESS || fence == VK_NULL_HANDLE) {
                throw LSFG::vulkan_error(
                    fenceResult == VK_SUCCESS
                        ? VK_ERROR_INITIALIZATION_FAILED
                        : fenceResult,
                    "Failed to create pass-retirement fence");
            }
            return std::shared_ptr<VkFence>(
                new VkFence(fence),
                [device = info.device, destroyCompletionFence](
                        VkFence* ownedFence) {
                    if (ownedFence != nullptr) {
                        if (*ownedFence != VK_NULL_HANDLE)
                            destroyCompletionFence(
                                device, *ownedFence, nullptr);
                        delete ownedFence;
                    }
                });
        };

        pass.completionFence = createOwnedCompletionFence();
#ifdef __ANDROID__
        if (this->deferredAdrenoCompletionEnabled_) {
            pass.postCopyCompletionFences.resize(runtimeMultiplier - 1);
            pass.postCopyCompletionFenceSubmitted.assign(
                runtimeMultiplier - 1, false);
            for (auto& postCopyFence : pass.postCopyCompletionFences)
                postCopyFence = createOwnedCompletionFence();
        }
#endif
    }
}

LsContext::~LsContext() {
#ifdef __ANDROID__
    for (const int fd : this->deferredAdrenoOutputReadyFds_) {
        if (fd >= 0)
            ::close(fd);
    }
    this->deferredAdrenoOutputReadyFds_.clear();
    if (this->deferredAdrenoBatchCompleteFd_ >= 0) {
        ::close(this->deferredAdrenoBatchCompleteFd_);
        this->deferredAdrenoBatchCompleteFd_ = -1;
    }
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
#ifdef __ANDROID__
    if (pass.deferredAdrenoOwned)
        return false;
#endif
    if (pass.completionFenceFailed
            || this->completionWaitFences_ == nullptr
            || this->completionResetFences_ == nullptr)
        return false;

    const auto waitAndResetFence =
        [&](const std::shared_ptr<VkFence>& owner) -> bool {
            if (owner == nullptr) {
                pass.completionFenceFailed = true;
                return false;
            }

            const VkFence fence = *owner;
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
            return true;
        };

    if (pass.completionFenceSubmitted) {
        if (!waitAndResetFence(pass.completionFence))
            return false;
        pass.completionFenceSubmitted = false;
    }

#ifdef __ANDROID__
    for (size_t i = 0;
            i < pass.postCopyCompletionFenceSubmitted.size(); ++i) {
        if (!pass.postCopyCompletionFenceSubmitted.at(i))
            continue;
        if (i >= pass.postCopyCompletionFences.size()
                || !waitAndResetFence(pass.postCopyCompletionFences.at(i))) {
            return false;
        }
        pass.postCopyCompletionFenceSubmitted.at(i) = false;
    }
#endif

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
    const bool adrenoHostCompletionFallback =
        this->conservativeCrossDeviceSync_
        && this->syntheticQueue_ == VK_NULL_HANDLE
        && !this->asyncFramegenCompletionEnabled_
        && !this->deferredAdrenoCompletionEnabled_;
#else
    constexpr bool adrenoHostCompletionFallback = false;
#endif
    size_t deferredDeliveredGeneratedFrameCount = 0;
    bool deferredAdrenoBatchQueuedThisCycle = false;
    bool conservativeBatchStillInFlight = false;
    this->releasePresentWaitRetirements(presentIdx);

#ifdef __ANDROID__
    const auto deferredBoundaryNow = RuntimeMetrics::Clock::now();
    const bool deferredAdrenoBoundaryDiscontinuity =
        this->deferredAdrenoBatchValid_
        && this->runtimeMetrics.hasLastSourcePresent
        && std::chrono::duration<double, std::milli>(
            deferredBoundaryNow - this->runtimeMetrics.lastSourcePresent).count()
            >= 250.0;
    const bool deferredAdrenoConfigBoundary =
        this->deferredAdrenoBatchValid_
        && this->runtimeConfigSignatureValid_
        && this->runtimeConfigSignature_
            != runtimeDiagnosticConfigSignature(conf);
    if ((deferredAdrenoBoundaryDiscontinuity || deferredAdrenoConfigBoundary)
            && this->deferredAdrenoOutputEligible_) {
        this->deferredAdrenoOutputEligible_ = false;
        for (int& fd : this->deferredAdrenoOutputReadyFds_) {
            if (fd >= 0)
                ::close(fd);
            fd = -1;
        }
    }

    if (this->deferredAdrenoBatchValid_) {
        const uint64_t deferredBatchId = this->deferredAdrenoBatchId_;
        const uint32_t deferredSourceAge = this->deferredAdrenoSourceAge_;
        auto& deferredPass =
            this->passInfos.at(this->deferredAdrenoPassIndex_ % this->passInfos.size());
        bool batchReady = this->deferredAdrenoBatchCompleteReady_;

        if (!batchReady && this->deferredAdrenoBatchCompleteFd_ >= 0) {
            pollfd batchPoll{
                .fd = this->deferredAdrenoBatchCompleteFd_,
                .events = POLLIN,
                .revents = 0,
            };
            const int batchPollResult = ::poll(&batchPoll, 1, 0);
            if (batchPollResult > 0 && (batchPoll.revents & POLLIN) != 0) {
                const int batchFd = this->deferredAdrenoBatchCompleteFd_;
                this->deferredAdrenoBatchCompleteFd_ = -1;
                try {
                    this->deferredAdrenoBatchCompleteSemaphore_ = Mini::Semaphore(
                        info.device, batchFd,
                        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
                    this->deferredAdrenoBatchCompleteReady_ = true;
                    batchReady = true;
                } catch (const std::exception& e) {
                    // Mini::Semaphore consumed the fd. Keep source-safe polling
                    // through the private context without a blocking wait.
                    std::cerr << "lsfg-vk: deferred Adreno batch SYNC_FD import failed: "
                              << e.what() << "\n";
                }
            } else if (batchPollResult < 0
                    || (batchPollResult > 0
                        && (batchPoll.revents
                            & (POLLERR | POLLHUP | POLLNVAL)) != 0)) {
                ::close(this->deferredAdrenoBatchCompleteFd_);
                this->deferredAdrenoBatchCompleteFd_ = -1;
            }
        }

        if (!batchReady && this->deferredAdrenoBatchCompleteFd_ < 0) {
            batchReady = conf.performance
                ? LSFG_3_1P::waitContext(*this->lsfgCtxId, 0)
                : LSFG_3_1::waitContext(*this->lsfgCtxId, 0);
            if (batchReady) {
                this->deferredAdrenoBatchCompleteReady_ = true;
                this->deferredAdrenoBatchCompleteSemaphore_ = {};
            }
        }

        size_t deferredReadyOutputPrefix = 0;
        bool deferredOutputPollingValid = this->deferredAdrenoOutputEligible_;
        if (this->deferredAdrenoOutputEligible_) {
            for (; deferredReadyOutputPrefix < this->deferredAdrenoGeneratedCount_;
                    ++deferredReadyOutputPrefix) {
                const size_t i = deferredReadyOutputPrefix;
                int& outputFd = this->deferredAdrenoOutputReadyFds_.at(i);
                if (outputFd < 0)
                    continue;

                pollfd outputPoll{
                    .fd = outputFd,
                    .events = POLLIN,
                    .revents = 0,
                };
                const int outputPollResult = ::poll(&outputPoll, 1, 0);
                if (outputPollResult > 0 && (outputPoll.revents & POLLIN) != 0) {
                    const int readyFd = outputFd;
                    outputFd = -1;
                    try {
                        deferredPass.renderSemaphores.at(i) = Mini::Semaphore(
                            info.device, readyFd,
                            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
                    } catch (const std::exception& e) {
                        this->deferredAdrenoOutputEligible_ = false;
                        deferredOutputPollingValid = false;
                        std::cerr << "lsfg-vk: deferred Adreno output SYNC_FD import failed: "
                                  << e.what() << "\n";
                        break;
                    }
                } else if (outputPollResult == 0) {
                    // Preserve temporal order: a later synthetic must never be
                    // delivered when an earlier interpolation slot is not ready.
                    break;
                } else {
                    ::close(outputFd);
                    outputFd = -1;
                    this->deferredAdrenoOutputEligible_ = false;
                    deferredOutputPollingValid = false;
                    break;
                }
            }
        }

        const size_t deferredUnreadyOutputCount =
            deferredOutputPollingValid
                ? this->deferredAdrenoGeneratedCount_ - deferredReadyOutputPrefix
                : 0;
        if (deferredReadyOutputPrefix > 0) {
            this->runtimeMetrics.windowGeneratedCompleted +=
                deferredReadyOutputPrefix;
            this->runtimeMetrics.totalGeneratedCompleted +=
                deferredReadyOutputPrefix;
        }
        if (!batchReady
                || deferredReadyOutputPrefix < this->deferredAdrenoGeneratedCount_) {
            ++this->deferredAdrenoSourceAge_;
        }

        if (deferredOutputPollingValid && deferredUnreadyOutputCount > 0) {
            this->runtimeMetrics.windowGeneratedLateDrops +=
                deferredUnreadyOutputCount;
            this->runtimeMetrics.totalGeneratedLateDrops +=
                deferredUnreadyOutputCount;
            if (conf.adaptiveFramegen) {
                // The unready temporal suffix missed this real-source boundary.
                // Train only Adreno's deferred admission feedback; the ready
                // prefix below remains deliverable without blocking the source.
                this->runtimeMetrics.windowGeneratedDeadlineDrops +=
                    deferredUnreadyOutputCount;
                this->runtimeMetrics.totalGeneratedDeadlineDrops +=
                    deferredUnreadyOutputCount;
                double deliveryMissMs = 0.5;
                if (this->runtimeMetrics.hasLastSourcePresent) {
                    const double sourceBoundaryMs =
                        std::chrono::duration<double, std::milli>(
                            deferredBoundaryNow
                                - this->runtimeMetrics.lastSourcePresent).count();
                    if (std::isfinite(sourceBoundaryMs)
                            && sourceBoundaryMs > 0.0) {
                        deliveryMissMs = sourceBoundaryMs;
                    }
                }
                this->deadlineAdmissionPredictor_.observeDeliveryMiss(
                    deliveryMissMs);
            }
        }

        size_t deferredWsiDrops = 0;
        const bool deferredPresentationAttempted =
            this->deferredAdrenoOutputEligible_
            && deferredReadyOutputPrefix > 0;
        const size_t deferredPresentationAttemptedCount =
            deferredPresentationAttempted ? deferredReadyOutputPrefix : 0;
        if (deferredReadyOutputPrefix > 0 && deferredPresentationAttempted) {
            for (size_t i = 0; i < deferredReadyOutputPrefix; ++i) {
                const auto generatedPresentStart = RuntimeMetrics::Clock::now();
                deferredPass.acquireSemaphores.at(i) = Mini::Semaphore(info.device);
                uint32_t imageIdx{};
                auto acquireResult = Layer::ovkAcquireNextImageKHR(
                    info.device, this->swapchain, 0,
                    deferredPass.acquireSemaphores.at(i).handle(),
                    VK_NULL_HANDLE, &imageIdx);
                if (acquireResult == VK_NOT_READY || acquireResult == VK_TIMEOUT) {
                    deferredWsiDrops = deferredReadyOutputPrefix - i;
                    this->runtimeMetrics.windowGeneratedLateDrops += deferredWsiDrops;
                    this->runtimeMetrics.totalGeneratedLateDrops += deferredWsiDrops;
                    this->runtimeMetrics.windowGeneratedWsiDrops += deferredWsiDrops;
                    this->runtimeMetrics.totalGeneratedWsiDrops += deferredWsiDrops;
                    break;
                }
                if (isAdrenoWsiRetirementResult(acquireResult))
                    return acquireResult;
                if (acquireResult != VK_SUCCESS
                        && acquireResult != VK_SUBOPTIMAL_KHR) {
                    this->runtimeMetrics.windowGeneratedPresentFailures++;
                    this->runtimeMetrics.totalGeneratedPresentFailures++;
                    throw LSFG::vulkan_error(
                        acquireResult,
                        "Failed to acquire swapchain image for deferred Adreno frame");
                }
                this->releasePresentWaitRetirements(imageIdx);

                deferredPass.postCopySemaphores.at(i) = Mini::Semaphore(info.device);
                deferredPass.prevPostCopySemaphores.at(i) = Mini::Semaphore(info.device);
                auto& postCopyBuf = deferredPass.postCopyBufs.at(i);
                if (postCopyBuf.getState() == Mini::CommandBufferState::Submitted)
                    postCopyBuf.reset();
                postCopyBuf.begin();
                copyExternalAhbToSwapchain(
                    postCopyBuf.handle(),
                    this->out_n.at(i).handle(),
                    this->swapchainImages.at(imageIdx),
                    this->extent.width, this->extent.height,
                    info.queue.first);
                postCopyBuf.end();

                std::vector<VkSemaphore> generatedCopyWaits{
                    deferredPass.acquireSemaphores.at(i).handle()
                };
                if (deferredPass.renderSemaphores.at(i).handle() != VK_NULL_HANDLE) {
                    generatedCopyWaits.emplace_back(
                        deferredPass.renderSemaphores.at(i).handle());
                }
                if (i >= deferredPass.postCopyCompletionFences.size()
                        || deferredPass.postCopyCompletionFences.at(i) == nullptr
                        || deferredPass.postCopyCompletionFenceSubmitted.at(i)) {
                    this->deferredAdrenoOutputEligible_ = false;
                    this->lastHistoryInvalidationReason_ =
                        SourceHistoryInvalidationReason::TrueOwnershipFailure;
                    this->lastHistoryReprimeReason_ =
                        SourceHistoryInvalidationReason::TrueOwnershipFailure;
                    throw LSFG::vulkan_error(
                        VK_ERROR_INITIALIZATION_FAILED,
                        "Deferred Adreno post-copy retirement fence unavailable");
                }
                postCopyBuf.submit(
                    info.queue.second,
                    generatedCopyWaits,
                    { deferredPass.postCopySemaphores.at(i).handle(),
                      deferredPass.prevPostCopySemaphores.at(i).handle() },
                    *deferredPass.postCopyCompletionFences.at(i));
                deferredPass.postCopyCompletionFenceSubmitted.at(i) = true;
                this->runtimeMetrics.windowGeneratedCopySubmitted++;
                this->runtimeMetrics.totalGeneratedCopySubmitted++;

                std::vector<VkSemaphore> deferredPresentWaits{
                    deferredPass.postCopySemaphores.at(i).handle()
                };
                if (i != 0) {
                    deferredPresentWaits.emplace_back(
                        deferredPass.prevPostCopySemaphores.at(i - 1).handle());
                }
                const VkPresentInfoKHR deferredPresentInfo{
                    .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                    .pNext = nullptr,
                    .waitSemaphoreCount =
                        static_cast<uint32_t>(deferredPresentWaits.size()),
                    .pWaitSemaphores = deferredPresentWaits.data(),
                    .swapchainCount = 1,
                    .pSwapchains = &this->swapchain,
                    .pImageIndices = &imageIdx,
                };
                this->retainPresentWait(
                    imageIdx, deferredPass.postCopySemaphores.at(i));
                if (i != 0) {
                    this->retainPresentWait(
                        imageIdx,
                        deferredPass.prevPostCopySemaphores.at(i - 1));
                }
                this->runtimeMetrics.windowGeneratedWsiSubmitted++;
                this->runtimeMetrics.totalGeneratedWsiSubmitted++;
                const auto deferredPresentResult =
                    Layer::ovkQueuePresentKHR(queue, &deferredPresentInfo);
                if (isAdrenoWsiRetirementResult(deferredPresentResult))
                    return deferredPresentResult;
                if (deferredPresentResult != VK_SUCCESS
                        && deferredPresentResult != VK_SUBOPTIMAL_KHR) {
                    this->runtimeMetrics.windowGeneratedPresentFailures++;
                    this->runtimeMetrics.totalGeneratedPresentFailures++;
                    throw LSFG::vulkan_error(
                        deferredPresentResult,
                        "Failed to present deferred Adreno generated frame");
                }

                ++deferredDeliveredGeneratedFrameCount;
                ++this->runtimeMetrics.windowGeneratedFrames;
                ++this->runtimeMetrics.totalGeneratedFrames;
                ++this->runtimeMetrics.windowGeneratedWsiAccepted;
                ++this->runtimeMetrics.totalGeneratedWsiAccepted;
                ++this->runtimeMetrics.windowGeneratedDisplayUnknown;
                ++this->runtimeMetrics.totalGeneratedDisplayUnknown;
                this->runtimeMetrics.windowGeneratedPresentMs +=
                    std::chrono::duration<double, std::milli>(
                        RuntimeMetrics::Clock::now()
                            - generatedPresentStart).count();
            }
        }

        if (conf.adaptiveFramegen && deferredPresentationAttempted) {
            this->generatedPresentationCapacityTracker_.observe(
                deferredPresentationAttemptedCount,
                deferredWsiDrops);
        }

        if (deferredDeliveredGeneratedFrameCount
                == this->deferredAdrenoGeneratedCount_) {
            this->deadlineAdmissionPredictor_.observeDeliverySuccess();
        }

        // Deferred output delivery is a one-source-boundary opportunity. Keep
        // the batch ownership/release state until batchComplete, but never
        // retry a stale suffix on a later source frame.
        this->deferredAdrenoOutputEligible_ = false;
        for (int& fd : this->deferredAdrenoOutputReadyFds_) {
            if (fd >= 0)
                ::close(fd);
            fd = -1;
        }

        conservativeBatchStillInFlight = !batchReady;
        if (batchReady) {
            this->deferredAdrenoOutputReadyFds_.clear();
            this->deferredAdrenoBatchValid_ = false;
            this->deferredAdrenoGeneratedCount_ = 0;
            this->deferredAdrenoSourceAge_ = 0;
            deferredPass.deferredAdrenoOwned = false;
        }

        if (shouldTraceBatch(deferredBatchId)) {
            std::cerr << "lsfg-vk: runtime stage=adreno-batch-delivery"
                      << " batch_id=" << deferredBatchId
                      << " private_complete=" << (batchReady ? 1 : 0)
                      << " output_ready=" << deferredReadyOutputPrefix
                      << " copy_submitted=" << deferredDeliveredGeneratedFrameCount
                      << " wsi_submitted=" << deferredDeliveredGeneratedFrameCount
                      << " wsi_accepted=" << deferredDeliveredGeneratedFrameCount
                      << " late_dropped=" << deferredUnreadyOutputCount
                      << " source_age=" << deferredSourceAge
                      << '\n';
        }
        if (batchReady)
            this->deferredAdrenoBatchId_ = 0;
    }

    if (this->pendingSourceValid_) {
        const VkSemaphore pendingSourceReady =
            this->pendingSourceReady_.handle();
        const uint32_t pendingSourceImage =
            this->pendingSourceImage_;
        const VkPresentInfoKHR pendingSourcePresentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            // The original application's pNext chain is not retained across
            // calls. Deferred source buffering is therefore enabled only when
            // the source call had no downstream chain.
            .pNext = nullptr,
            .waitSemaphoreCount =
                pendingSourceReady != VK_NULL_HANDLE ? 1U : 0U,
            .pWaitSemaphores =
                pendingSourceReady != VK_NULL_HANDLE
                    ? &pendingSourceReady
                    : nullptr,
            .swapchainCount = 1,
            .pSwapchains = &this->swapchain,
            .pImageIndices = &pendingSourceImage,
        };
        if (pendingSourceReady != VK_NULL_HANDLE) {
            this->retainPresentWait(
                pendingSourceImage, this->pendingSourceReady_);
        }
        const auto pendingSourceResult =
            Layer::ovkQueuePresentKHR(queue, &pendingSourcePresentInfo);
        if (isAdrenoWsiRetirementResult(pendingSourceResult))
            return pendingSourceResult;
        if (pendingSourceResult != VK_SUCCESS
                && pendingSourceResult != VK_SUBOPTIMAL_KHR) {
            this->runtimeMetrics.windowSourcePresentFailures++;
            this->runtimeMetrics.totalSourcePresentFailures++;
            throw LSFG::vulkan_error(
                pendingSourceResult,
                "Failed presenting buffered Adreno source frame");
        }

        if (this->frameIdx < 4) {
            std::cerr << "lsfg-vk: runtime stage=adreno-pending-source-present"
                      << " image=" << pendingSourceImage
                      << " pending_pass=" << this->pendingPassIndex_
                      << " planned_generated="
                      << this->pendingGeneratedCount_
                      << "\n";
        }
        this->pendingSourceValid_ = false;
        this->pendingSourceReady_ = {};
        this->pendingGeneratedCount_ = 0;
        this->framegenInFlight_ = false;
        this->framegenOutputEligible_ = false;
    }
#endif

    auto& pass = this->passInfos.at(this->frameIdx % 8);
#ifdef __ANDROID__
    const bool shouldRecyclePass = !this->conservativeCrossDeviceSync_;
#else
    const bool shouldRecyclePass = true;
#endif
    if (shouldRecyclePass && !this->tryRecyclePass(pass)) {
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
            if (this->conservativeCrossDeviceSync_) {
                // A busy pass ring is retirement backpressure, not a source
                // cadence discontinuity. Resetting the whole Adreno epoch here
                // restarts the two-source reprime every passthrough and can pin
                // sourceHistoryWarmupRemaining_ at one forever while deferred
                // framegen work retires. Preserve cadence/controller state and
                // request only the single copy needed to reprime the skipped
                // source before the next generated batch.
                this->lastDispatchedGeneratedFrameCount_ = 0;
                this->lastSourceCadenceObservation_ =
                    SourceCadenceObservation::SourceOnly;
                this->previousSourceCopySignalValid_ = false;
                this->sourceHistoryWarmupRemaining_ = std::max(
                    this->sourceHistoryWarmupRemaining_,
                    kConservativeSourceReprimeFrames - 1);
                this->requiresSourceHistoryWarmup_ =
                    this->sourceHistoryWarmupRemaining_ > 0;
                this->lastHistoryInvalidationReason_ =
                    SourceHistoryInvalidationReason::None;
                this->lastHistoryReprimeReason_ =
                    SourceHistoryInvalidationReason::RetirementBackpressure;
                this->deadlineBatchDecision_ = {};
            } else {
                this->resetAdaptiveSourceEpoch(
                true, SourceHistoryInvalidationReason::TimelineDiscontinuity);
            }
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
        // Resident hot reloads deliberately preserve the swapchain/AHB
        // allocation, but their generated cadence cannot inherit temporal
        // history from the pre-menu configuration. Start a fresh source epoch
        // before the new scheduler settings are allowed to generate.
        this->resetAdaptiveSourceEpoch(
            true, SourceHistoryInvalidationReason::RuntimeConfigChange);
        std::cerr << "lsfg-vk: runtime stage=temporal-epoch-reset"
                  << " reason=runtime-config-change"
                  << " config_revision=" << this->configRevision_
                  << "\n";
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
    const bool sourceProtectionBatchAdmission =
        this->conservativeCrossDeviceSync_;
    const char* deadlineSemantics =
        sourceProtectionBatchAdmission
            ? "source-protection"
            : "synthetic-slot";
    double computeReadyBudgetMs = 0.0;
    double presentationSlotBudgetMs = 0.0;
    double sourceBudgetRawMs = 0.0;
    double sourceBudgetEffectiveMs = 0.0;
    const SourceCadenceObservation previousSourceCadenceObservation =
        this->conservativeCrossDeviceSync_
            ? this->lastSourceCadenceObservation_
            : (this->lastDispatchedGeneratedFrameCount_ > 0
                ? SourceCadenceObservation::Generated
                : SourceCadenceObservation::SourceOnly);

    // Learn source cadence before target planning. Only the previous cycle's
    // classification may authorize baseline growth, so generated/history work
    // cannot teach Adaptive that LSFG's own slowdown is a larger budget.
    if (sourceProtectionBatchAdmission && sourceInterval.count() > 0) {
        const double observedSourceIntervalMs =
            std::chrono::duration<double, std::milli>(sourceInterval).count();
        if (observedSourceIntervalMs < kRuntimeTimingDiscontinuityMs) {
            this->sourceProtectionBudgetTracker_.observeSource(
                sourceInterval, previousSourceCadenceObservation);
        }
    }
    const auto& sourceProtectionBeforePlan =
        this->sourceProtectionBudgetTracker_.telemetry();
    this->adaptiveScheduler_.setSourceProtectionBaseline(
        sourceProtectionBeforePlan.protectedSourceIntervalMs,
        sourceProtectionBatchAdmission
            && sourceProtectionBeforePlan.baselineValid);

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
    const double protectedCapacityIntervalMs =
        sourceProtectionBatchAdmission
            ? this->sourceProtectionBudgetTracker_.clampTimelineBudget(
                capacityIntervalMs)
            : capacityIntervalMs;
    const bool safeGenerationHintValid =
        conf.adaptiveFramegen
        && maxAdaptiveGeneratedFrames > 0
        && protectedCapacityIntervalMs > 0.0
        && (!sourceProtectionBatchAdmission
            || this->sourceProtectionBudgetTracker_.telemetry().baselineValid)
        && this->deadlineAdmissionPredictor_.hasEstimate();
    const size_t safeGenerationHint = safeGenerationHintValid
        ? (sourceProtectionBatchAdmission
            ? this->deadlineAdmissionPredictor_.safeBatchGenerationHint(
                maxAdaptiveGeneratedFrames, protectedCapacityIntervalMs)
            : this->deadlineAdmissionPredictor_.safeGenerationHint(
                maxAdaptiveGeneratedFrames, capacityIntervalMs))
        : 0;
    this->adaptiveScheduler_.setSafeGenerationHint(
        safeGenerationHint,
        safeGenerationHintValid);

    if (conf.adaptiveFramegen)
        this->fixedSourceCadenceGovernor_.reset();
    size_t plannedGeneratedFrameCount = conf.adaptiveFramegen
        ? this->adaptiveScheduler_.plan(sourceInterval)
        : this->fixedSourceCadenceGovernor_.plan(
            sourceInterval,
            requestedFixedGeneratedFrameCount,
            this->lastDispatchedGeneratedFrameCount_,
            !this->requiresSourceHistoryWarmup_,
            previousSourceCadenceObservation);
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
        this->resetAdaptiveSourceEpoch(
            false, SourceHistoryInvalidationReason::TimelineDiscontinuity);
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
            this->resetAdaptiveSourceEpoch(
                true, SourceHistoryInvalidationReason::TimelineDiscontinuity);
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

    // Warmup is a genuine history/lifecycle condition, not a function of
    // current fractional demand. Advance it through the conservative source
    // path even when Adaptive plans zero generated frames; otherwise a rejected
    // or zero-demand cadence gap can leave history_warmup_remaining=1 forever.
    const bool sourceHistoryWarmupActive =
        this->requiresSourceHistoryWarmup_
        && this->sourceHistoryWarmupRemaining_ > 0;
    const bool sourceProtectionBaselineValid =
        !sourceProtectionBatchAdmission
        || this->sourceProtectionBudgetTracker_.telemetry().baselineValid;

    // Active deadline admission remains Adaptive-only. Fixed mode protects the
    // real-source cadence through FixedSourceCadenceGovernor before entering
    // the compatibility island; it does not use Adaptive's GPU-cost predictor.
    // Adaptive Adreno tests a complete candidate batch against the protected
    // real-source boundary; Xclipse/generic Adaptive keeps ideal-slot admission.
    // A rejected opportunity is dropped, never accumulated as catch-up debt.
    this->deadlineBatchDecision_ = {};
    // Protected Adreno must not bootstrap from an LSFG-active interval. Until
    // one genuine source-only interval establishes the source cadence, reject
    // synthetic work and let the existing admission source-escape path collect
    // uncontaminated baseline evidence.
    if (conf.adaptiveFramegen
            && sourceProtectionBatchAdmission && !sourceProtectionBaselineValid
            && !sourceHistoryWarmupActive && generatedFrameCount > 0) {
        metrics.windowAdmissionRejects += generatedFrameCount;
        metrics.totalAdmissionRejects += generatedFrameCount;
        generatedFrameCount = 0;
    }

    // Cost estimates are learned from completed batches below only after the
    // protected source cadence is known.
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
                const double rawSourceBudgetMs =
                    static_cast<double>(
                        this->currentSourceTimeline_.sourceDesiredTimeNs - admissionNowNs)
                    / 1'000'000.0;
                const double sourceBudgetMs =
                    sourceProtectionBaselineValid && sourceProtectionBatchAdmission
                        ? this->sourceProtectionBudgetTracker_.clampTimelineBudget(
                            rawSourceBudgetMs)
                        : rawSourceBudgetMs;
                sourceBudgetRawMs = rawSourceBudgetMs;
                sourceBudgetEffectiveMs = sourceBudgetMs;
                computeReadyBudgetMs = sourceBudgetMs;
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
                        if (slot == 0)
                            presentationSlotBudgetMs = slotBudgetMs;
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
                    if (sourceProtectionBatchAdmission) {
                        // Deferred single-queue Adreno protects the next real
                        // source boundary. Ideal interpolation timestamps are
                        // presentation slots, not private-device completion
                        // deadlines. Test complete candidate batches against the
                        // remaining source-owned budget.
                        for (size_t candidate = generatedFrameCount;
                                candidate > 0; --candidate) {
                            const auto batchDecision =
                                this->deadlineAdmissionPredictor_.predict(
                                    candidate, sourceBudgetMs);
                            if (batchDecision.valid
                                    && batchDecision.wouldAdmit) {
                                admittedGeneratedFrameCount = candidate;
                                break;
                            }
                        }
                    } else {
                        for (size_t candidate = generatedFrameCount;
                                candidate > 0; --candidate) {
                            bool candidateFits = true;
                            for (size_t slot = 0; slot < candidate; ++slot) {
                                // Existing Xclipse/capability-async policy:
                                // every generated prefix must still meet the
                                // ideal slot it owns.
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
                                if (!slotDecision.valid
                                        || !slotDecision.wouldAdmit) {
                                    candidateFits = false;
                                    break;
                                }
                            }
                            if (candidateFits) {
                                admittedGeneratedFrameCount = candidate;
                                break;
                            }
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
    if (conf.adaptiveFramegen && generatedFrameCount > 0) {
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

        if (this->conservativeCrossDeviceSync_ && budgetMs > 0.0) {
            budgetMs =
                this->sourceProtectionBudgetTracker_.clampTimelineBudget(
                    budgetMs);
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

    // Fixed mode also needs uncontaminated source-only evidence. Immediately
    // after a temporal/config reset there is no protected baseline, and after a
    // governor backoff a zero generated count means synthetic work has already
    // harmed source cadence. In both cases bypass AHB/private-framegen work for
    // this cycle so the next interval measures the game itself.
    const bool conservativeFixedSourceProtectionGap =
        this->conservativeCrossDeviceSync_
        && !conf.adaptiveFramegen
        && requestedFixedGeneratedFrameCount > 0
        && !sourceTimelineDiscontinuity
        && (!sourceProtectionBaselineValid
            || (!sourceHistoryWarmupActive
                && this->fixedSourceCadenceGovernor_.telemetry().baselineValid
                && plannedGeneratedFrameCount == 0));

    // Ordinary fractional/zero-demand Adaptive gaps preserve temporal
    // history. A rejected synthetic opportunity is different: on protected
    // Adreno it is an emergency source-protection escape and must return a
    // real source frame without entering AHB/private-framegen maintenance.
    const bool conservativeAdmissionRejectedHistoryGap =
        this->conservativeCrossDeviceSync_
        && conf.adaptiveFramegen
        && !sourceHistoryWarmupActive
        && !sourceTimelineDiscontinuity
        && plannedGeneratedFrameCount > 0
        && generatedFrameCount == 0;
    const bool conservativeFractionalHistoryGap =
        this->conservativeCrossDeviceSync_
        && conf.adaptiveFramegen
        && !sourceHistoryWarmupActive
        && !sourceTimelineDiscontinuity
        && plannedGeneratedFrameCount == 0
        && adaptiveTelemetry.wantedGeneratedFrames > 0.0;
    const bool conservativeZeroDemandHistoryGap =
        this->conservativeCrossDeviceSync_
        && conf.adaptiveFramegen
        && !sourceHistoryWarmupActive
        && !sourceTimelineDiscontinuity
        && plannedGeneratedFrameCount == 0
        && adaptiveTelemetry.wantedGeneratedFrames <= 0.0;
    const bool conservativeAdaptiveHistoryGap =
        this->conservativeCrossDeviceSync_
        && conf.adaptiveFramegen
        && !sourceHistoryWarmupActive
        && !sourceTimelineDiscontinuity
        && generatedFrameCount == 0
        && (conservativeFractionalHistoryGap
            || conservativeZeroDemandHistoryGap);
    // Fixed source-protection rejection is still a history-maintenance
    // cycle. Whether the governor planned zero work or deadline admission
    // reduced planned work to zero, keep the source pair coherent instead of
    // reclassifying the cycle as a true source-only bypass.
    const bool conservativeFixedHistoryGap =
        this->conservativeCrossDeviceSync_
        && !conf.adaptiveFramegen
        && !sourceHistoryWarmupActive
        && !sourceTimelineDiscontinuity
        && generatedFrameCount == 0;
    const bool conservativeHistoryGap =
        this->conservativeCrossDeviceSync_
        && !sourceHistoryWarmupActive
        && !sourceTimelineDiscontinuity
        && generatedFrameCount == 0
        && (conservativeAdaptiveHistoryGap || conservativeFixedHistoryGap);
    const bool conservativeSourceOnlyWarmup =
        this->conservativeCrossDeviceSync_
        && sourceHistoryWarmupActive;
    const bool conservativeTrueSourceOnlyCycle =
        this->conservativeCrossDeviceSync_
        && historyOnly
        && !conservativeSourceOnlyWarmup
        && !conservativeHistoryGap;

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
        if (timingFresh) {
            this->adaptiveFlowLastObservedBatchId_ = timing.batchId;
            if (shouldTraceBatch(timing.batchId)) {
                std::cerr << "lsfg-vk: runtime stage=framegen-gpu-timing"
                          << " batch_id=" << timing.batchId
                          << " mipmaps_ms=" << timing.mipmapsMs
                          << " flow_end_ms=" << timing.opticalFlowMs
                          << " frame_interpolation_end_ms=" << timing.totalLsfgMs
                          << " generation_count=" << timing.generationCount
                          << '\n';
            }
        }
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
        const bool retainedTimingUsable =
            this->adaptiveFlowGeneratedTimingValid_
            && this->adaptiveFlowRetainedGenerationCount_ > 0;
        const double observationMipmapsMs = generatedWorkSample
            ? timing.mipmapsMs : this->adaptiveFlowRetainedMipmapsMs_;
        const double observationFlowEndMs = generatedWorkSample
            ? timing.opticalFlowMs : this->adaptiveFlowRetainedWorkMs_;
        const double observationScaleSensitiveFlowMs = std::max(
            0.0, observationFlowEndMs - observationMipmapsMs);
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
            .flowMs = observationScaleSensitiveFlowMs,
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
                !this->conservativeCrossDeviceSync_
                && this->adaptiveFlowGlobalPressureValid_,
            .outputDeficit = outputDeficit,
            .syntheticDropPressure = false,
            .generatedWorkSample = generatedWorkSample,
            .retainedGeneratedTimingSample =
                !generatedWorkSample && retainedTimingUsable,
            .schedulerTransition = schedulerTransition,
            .valid = observationBudgetValid
                && sourceInterval.count() > 0
                && retainedTimingUsable
                && (!cadenceDiscontinuity || generatedWorkSample),
        };

        const float previousScale = this->adaptiveFlowController_.currentScale();
        const float selectedScale =
            this->adaptiveFlowController_.observe(observation);
        const auto& flowTelemetry = this->adaptiveFlowController_.telemetry();
        // Diagnostics must report the controller's actual direct-timing
        // pressure decision, not only event-counter deltas. On Adreno the
        // system GPU-utilization sidecar can stay flat while LSFG GPU time is
        // grossly over budget.
        this->adaptiveFlowComputePressure_ = flowTelemetry.computePressure;
        this->adaptiveFlowWsiPressure_ = flowTelemetry.wsiPressure;

        if (flowTelemetry.changed
                && std::fabs(selectedScale - previousScale) > 0.0005F) {
            // One retained real-batch timing sample may bridge source-only
            // protection cycles to authorize this downstep. Once the actuator
            // changes scale, that timing no longer describes the active Flow
            // state and must not authorize another step without fresh GPU work.
            this->adaptiveFlowGeneratedTimingValid_ = false;
            // Cost measurements are scale-specific. Never admit a new batch
            // using an estimate learned at the previous Flow Scale.
            this->deadlineAdmissionPredictor_.reset();
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
                      << " retained_timing_sample="
                      << (observation.retainedGeneratedTimingSample ? 1 : 0)
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
                      << (flowTelemetry.computePressure ? 1 : 0)
                      << " wsi_pressure="
                      << (flowTelemetry.wsiPressure ? 1 : 0)
                      << " wsi_loss_rate=" << observation.wsiLossRate
                      << '\n';
#ifdef __ANDROID__
            __android_log_print(
                ANDROID_LOG_INFO,
                "LSFG_FLOW",
                "runtime_session_id=%llu config_revision=%llu "
                "previous=%.3f requested=%.3f reason=%s flow_ms=%.3f lsfg_ms=%.3f "
                "budget_ms=%.3f generation_count=%zu generated_work=%d retained_timing=%d "
                "gpu=%.1f pressure_valid=%d output_fps=%.3f output_deficit=%d "
                "output_satisfied=%d compute_pressure=%d wsi_pressure=%d wsi_loss_rate=%.3f",
                static_cast<unsigned long long>(this->runtimeSessionId_),
                static_cast<unsigned long long>(this->configRevision_),
                static_cast<double>(previousScale),
                static_cast<double>(selectedScale),
                AdaptiveFlowController::reasonName(flowTelemetry.reason),
                observation.flowMs,
                observation.totalLsfgMs,
                observation.frameBudgetMs,
                observation.generationCount,
                observation.generatedWorkSample ? 1 : 0,
                observation.retainedGeneratedTimingSample ? 1 : 0,
                observation.globalGpuUsagePercent,
                observation.globalPressureValid ? 1 : 0,
                observation.outputFps,
                observation.outputDeficit ? 1 : 0,
                observation.outputTargetSatisfied ? 1 : 0,
                flowTelemetry.computePressure ? 1 : 0,
                flowTelemetry.wsiPressure ? 1 : 0,
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
            retainedTimingUsable ? observationScaleSensitiveFlowMs : 0.0;
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

    const auto chainHasGooglePresentTimes = [](const void* downstream) {
        auto* node = reinterpret_cast<const VkBaseInStructure*>(downstream);
        while (node != nullptr) {
            if (node->sType == VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE)
                return true;
            node = node->pNext;
        }
        return false;
    };

    const auto nextGeneratedDisplayTimingPresentId = [&]() {
        uint32_t presentId = this->generatedDisplayPresentId_++;
        // Keep generated telemetry in the high-half namespace so a future
        // source-pacing ID stream remains trivially distinguishable.
        if (presentId == 0 || (presentId & 0x80000000U) == 0) {
            presentId = 0x80000001U;
            this->generatedDisplayPresentId_ = 0x80000002U;
        }
        return presentId;
    };

    const auto generatedDisplayConfirmationPNext = [&](
            const void* downstream,
            uint64_t desiredPresentTimeNs,
            VkPresentTimeGOOGLE& presentTime,
            VkPresentTimesInfoGOOGLE& presentTimes) -> const void* {
        const bool pacingRequested =
            this->adaptiveDisplayTimingEnabled_ && desiredPresentTimeNs != 0;
        if (!this->generatedDisplayConfirmationEnabled_ && !pacingRequested)
            return downstream;

        // A pNext chain may already contain the extension structure supplied by
        // the application. Never duplicate an sType in that case; that present
        // remains WSI-only telemetry rather than mutating application metadata.
        if (chainHasGooglePresentTimes(downstream))
            return downstream;

        uint64_t effectiveDesiredTimeNs = 0;
        if (pacingRequested) {
            const uint64_t nowNs = monotonicNowNs();
            if (nowNs != 0 && desiredPresentTimeNs > nowNs)
                effectiveDesiredTimeNs = desiredPresentTimeNs;
        }

        presentTime = VkPresentTimeGOOGLE{
            .presentID = nextGeneratedDisplayTimingPresentId(),
            // Zero is explicitly telemetry-only: the presentation engine may
            // display at any time, so confirmation does not change cadence.
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

    const auto trackGeneratedDisplayPresent = [&](uint32_t presentId) {
        if (!this->generatedDisplayConfirmationEnabled_ || presentId == 0) {
            metrics.windowGeneratedDisplayUnknown++;
            metrics.totalGeneratedDisplayUnknown++;
            return;
        }

        const auto [_, inserted] =
            this->generatedDisplayPendingSet_.insert(presentId);
        if (!inserted)
            return;
        this->generatedDisplayPendingIds_.push_back(presentId);

        while (this->generatedDisplayPendingSet_.size()
                > kGeneratedDisplayPendingLimit) {
            while (!this->generatedDisplayPendingIds_.empty()
                    && this->generatedDisplayPendingSet_.find(
                        this->generatedDisplayPendingIds_.front())
                        == this->generatedDisplayPendingSet_.end()) {
                this->generatedDisplayPendingIds_.pop_front();
            }
            if (this->generatedDisplayPendingIds_.empty())
                break;
            const uint32_t expiredId =
                this->generatedDisplayPendingIds_.front();
            this->generatedDisplayPendingIds_.pop_front();
            if (this->generatedDisplayPendingSet_.erase(expiredId) != 0) {
                metrics.windowGeneratedDisplayUnknown++;
                metrics.totalGeneratedDisplayUnknown++;
            }
        }
    };

    const auto pollGeneratedDisplayConfirmations = [&]() {
        if (!this->generatedDisplayConfirmationEnabled_
                || this->getPastPresentationTimingGoogle_ == nullptr)
            return;

        uint32_t timingCount = 0;
        VkResult timingResult = this->getPastPresentationTimingGoogle_(
            this->device_, this->swapchain, &timingCount, nullptr);
        if (timingResult != VK_SUCCESS && timingResult != VK_INCOMPLETE) {
            metrics.windowDisplayTimingQueryFailures++;
            metrics.totalDisplayTimingQueryFailures++;
            Utils::logLimitN(
                "displayTimingQuery",
                5,
                "vkGetPastPresentationTimingGOOGLE count query failed: "
                    + std::to_string(static_cast<int>(timingResult)));
            return;
        }
        if (timingCount == 0)
            return;

        std::vector<VkPastPresentationTimingGOOGLE> timings(timingCount);
        timingResult = this->getPastPresentationTimingGoogle_(
            this->device_, this->swapchain, &timingCount, timings.data());
        if (timingResult != VK_SUCCESS && timingResult != VK_INCOMPLETE) {
            metrics.windowDisplayTimingQueryFailures++;
            metrics.totalDisplayTimingQueryFailures++;
            Utils::logLimitN(
                "displayTimingQuery",
                5,
                "vkGetPastPresentationTimingGOOGLE data query failed: "
                    + std::to_string(static_cast<int>(timingResult)));
            return;
        }

        timings.resize(timingCount);
        for (const auto& timing : timings) {
            if (this->generatedDisplayPendingSet_.erase(timing.presentID) == 0)
                continue;
            if (timing.actualPresentTime != 0) {
                metrics.windowGeneratedDisplayConfirmed++;
                metrics.totalGeneratedDisplayConfirmed++;
            } else {
                // MAILBOX is allowed to report a replaced/not-displayed image
                // with zero actualPresentTime. Keep it distinct from "unknown".
                metrics.windowGeneratedDisplayNotShown++;
                metrics.totalGeneratedDisplayNotShown++;
            }
        }

        while (!this->generatedDisplayPendingIds_.empty()
                && this->generatedDisplayPendingSet_.find(
                    this->generatedDisplayPendingIds_.front())
                    == this->generatedDisplayPendingSet_.end()) {
            this->generatedDisplayPendingIds_.pop_front();
        }
    };

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
        // Nonblocking presentation-engine feedback for previously submitted
        // generated frames. No queue/fence waits are introduced here.
        pollGeneratedDisplayConfirmations();
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
                      << " action=reset-temporal-epoch\n";
            // A suspend can happen after sourceInterval was sampled, so the
            // scheduler may never see the multi-second gap. Metrics-only reset
            // leaves the private framegen/source pair armed across that gap and
            // can persistently alternate stale temporal output. Preserve GPU
            // ownership releases, but invalidate cadence/history exactly as an
            // explicit Off -> On transition does.
            this->resetAdaptiveSourceEpoch(
                true, SourceHistoryInvalidationReason::SuspendResume);
            metrics.windowStart = cycleEnd;
            metrics.windowSourceFrames = 0;
            metrics.windowGeneratedFrames = 0;
            metrics.windowGeneratedDispatched = 0;
            metrics.windowGeneratedCompleted = 0;
            metrics.windowGeneratedCopySubmitted = 0;
            metrics.windowGeneratedWsiSubmitted = 0;
            metrics.windowGeneratedWsiAccepted = 0;
            metrics.windowGeneratedDisplayConfirmed = 0;
            metrics.windowGeneratedDisplayNotShown = 0;
            metrics.windowGeneratedDisplayUnknown = 0;
            metrics.windowDisplayTimingQueryFailures = 0;
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
            metrics.windowHistoryAsyncReleases = 0;
            metrics.windowHistoryHostCompletions = 0;
            metrics.windowDeadlineShadowOpportunities = 0;
            metrics.windowDeadlineShadowWouldAdmit = 0;
            metrics.windowDeadlineShadowWouldReject = 0;
            metrics.windowCycleMs = 0.0;
            metrics.windowCycleMaxMs = 0.0;
            metrics.windowHandoffMs = 0.0;
            metrics.windowHandoffSubmitMs = 0.0;
            metrics.windowHandoffFenceWaitMs = 0.0;
            metrics.windowHistoryPreprocessSubmitMs = 0.0;
            metrics.windowHistoryPreprocessHostWaitMs = 0.0;
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
            const size_t cadenceGeneratedFrames =
                this->deferredAdrenoCompletionEnabled_
                    ? deferredDeliveredGeneratedFrameCount
                    : this->lastGeneratedFrameCount_;
            this->lsfgOutputCadenceTracker_.observe(
                sourceInterval, 1, cadenceGeneratedFrames);
        }

        const double elapsedSeconds = std::chrono::duration<double>(
            cycleEnd - metrics.windowStart).count();
        if (elapsedSeconds >= 1.0) {
            const double sourceCount = static_cast<double>(metrics.windowSourceFrames);
            const double generatedCount = static_cast<double>(metrics.windowGeneratedFrames);
            const double sourceFps = sourceCount / elapsedSeconds;
            const double generatedFps = generatedCount / elapsedSeconds;
            const double outputFps = (sourceCount + generatedCount) / elapsedSeconds;
            const uint64_t generatedDisplayPending =
                static_cast<uint64_t>(this->generatedDisplayPendingSet_.size());
            const char* generatedDeliveryConfidence =
                metrics.windowGeneratedDisplayConfirmed > 0
                    ? "display-timing-confirmed"
                    : (this->generatedDisplayConfirmationEnabled_
                        && generatedDisplayPending > 0
                            ? "display-timing-pending"
                            : (this->generatedDisplayConfirmationEnabled_
                                && metrics.windowGeneratedDisplayNotShown > 0
                                    ? "display-timing-no-visible-confirmation"
                                    : (metrics.windowGeneratedWsiAccepted > 0
                                        ? "wsi-accepted-only"
                                        : "none")));
            std::cerr << "lsfg-vk: delivery-metrics"
                      << " generated_dispatched=" << metrics.windowGeneratedDispatched
                      << " generated_completed=" << metrics.windowGeneratedCompleted
                      << " generated_copy_submitted=" << metrics.windowGeneratedCopySubmitted
                      << " generated_wsi_submitted=" << metrics.windowGeneratedWsiSubmitted
                      << " generated_wsi_accepted=" << metrics.windowGeneratedWsiAccepted
                      << " generated_display_confirmed=" << metrics.windowGeneratedDisplayConfirmed
                      << " generated_display_not_shown=" << metrics.windowGeneratedDisplayNotShown
                      << " generated_display_pending=" << generatedDisplayPending
                      << " generated_display_unknown=" << metrics.windowGeneratedDisplayUnknown
                      << " generated_display_confirmed_total="
                      << metrics.totalGeneratedDisplayConfirmed
                      << " generated_display_not_shown_total="
                      << metrics.totalGeneratedDisplayNotShown
                      << " generated_display_unknown_total="
                      << metrics.totalGeneratedDisplayUnknown
                      << " display_timing_query_failures="
                      << metrics.windowDisplayTimingQueryFailures
                      << " generated_delivery_backend="
                      << (this->generatedDisplayConfirmationEnabled_
                            ? "google-display-timing"
                            : "none")
                      << " generated_delivery_confidence=" << generatedDeliveryConfidence
                      << " history_invalidation_reason="
                      << sourceHistoryInvalidationReasonName(
                            this->lastHistoryInvalidationReason_)
                      << " history_reprime_reason="
                      << sourceHistoryInvalidationReasonName(
                            this->lastHistoryReprimeReason_)
                      << "\n";
            __android_log_print(
                ANDROID_LOG_INFO,
                "LSFG_DELIVERY",
                "generated_dispatched=%llu generated_completed=%llu "
                "generated_copy_submitted=%llu generated_wsi_submitted=%llu "
                "generated_wsi_accepted=%llu generated_display_confirmed=%llu "
                "generated_display_not_shown=%llu generated_display_pending=%llu "
                "generated_display_unknown=%llu generated_display_confirmed_total=%llu "
                "generated_display_not_shown_total=%llu generated_display_unknown_total=%llu "
                "display_timing_query_failures=%llu "
                "generated_delivery_backend=%s generated_delivery_confidence=%s "
                "history_invalidation_reason=%s history_reprime_reason=%s",
                static_cast<unsigned long long>(metrics.windowGeneratedDispatched),
                static_cast<unsigned long long>(metrics.windowGeneratedCompleted),
                static_cast<unsigned long long>(metrics.windowGeneratedCopySubmitted),
                static_cast<unsigned long long>(metrics.windowGeneratedWsiSubmitted),
                static_cast<unsigned long long>(metrics.windowGeneratedWsiAccepted),
                static_cast<unsigned long long>(metrics.windowGeneratedDisplayConfirmed),
                static_cast<unsigned long long>(metrics.windowGeneratedDisplayNotShown),
                static_cast<unsigned long long>(generatedDisplayPending),
                static_cast<unsigned long long>(metrics.windowGeneratedDisplayUnknown),
                static_cast<unsigned long long>(metrics.totalGeneratedDisplayConfirmed),
                static_cast<unsigned long long>(metrics.totalGeneratedDisplayNotShown),
                static_cast<unsigned long long>(metrics.totalGeneratedDisplayUnknown),
                static_cast<unsigned long long>(metrics.windowDisplayTimingQueryFailures),
                this->generatedDisplayConfirmationEnabled_
                    ? "google-display-timing" : "none",
                generatedDeliveryConfidence,
                sourceHistoryInvalidationReasonName(
                    this->lastHistoryInvalidationReason_),
                sourceHistoryInvalidationReasonName(
                    this->lastHistoryReprimeReason_));
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
            const double historySamples = static_cast<double>(
                metrics.windowHistoryAsyncReleases
                + metrics.windowHistoryHostCompletions);
            const double historyPreprocessSubmitAvgMs = historySamples > 0.0
                ? metrics.windowHistoryPreprocessSubmitMs / historySamples
                : 0.0;
            const double historyPreprocessHostWaitAvgMs = historySamples > 0.0
                ? metrics.windowHistoryPreprocessHostWaitMs / historySamples
                : 0.0;
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
            const auto& sourceProtectionTelemetry =
                this->sourceProtectionBudgetTracker_.telemetry();

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
                      << " history_preprocess_submit_avg_ms="
                      << historyPreprocessSubmitAvgMs
                      << " history_preprocess_host_wait_avg_ms="
                      << historyPreprocessHostWaitAvgMs
                      << " history_async_releases="
                      << metrics.windowHistoryAsyncReleases
                      << " history_async_releases_total="
                      << metrics.totalHistoryAsyncReleases
                      << " history_host_completions="
                      << metrics.windowHistoryHostCompletions
                      << " history_host_completions_total="
                      << metrics.totalHistoryHostCompletions
                      << " framegen_dispatch_avg_ms=" << dispatchAvgMs
                      << " framegen_wait_avg_ms=" << waitIdleAvgMs
                      << " generated_present_avg_ms=" << generatedPresentAvgMs
                      << " source_interval_avg_ms=" << sourceIntervalAvgMs
                      << " source_interval_max_ms=" << metrics.windowSourceIntervalMaxMs
                      << " source_protected_interval_ms="
                      << sourceProtectionTelemetry.protectedSourceIntervalMs
                      << " source_budget_raw_ms=" << sourceBudgetRawMs
                      << " source_budget_effective_ms=" << sourceBudgetEffectiveMs
                      << " source_budget_copy_reserve_ms="
                      << sourceProtectionTelemetry.serializedCopyReserveMs
                      << " source_budget_observation="
                      << sourceCadenceObservationName(
                            sourceProtectionTelemetry.lastObservation)
                      << " source_protection_baseline_valid="
                      << (sourceProtectionTelemetry.baselineValid ? 1 : 0)
                      << " source_protection_copy_cost_valid="
                      << (sourceProtectionTelemetry.copyCostValid ? 1 : 0)
                      << " source_deadline_error_avg_ms=" << sourceDeadlineErrorAvgMs
                      << " source_deadline_error_max_ms="
                      << metrics.windowSourceDeadlineErrorMaxMs
                      << " source_timeline_rebases="
                      << metrics.windowSourceTimelineRebases
                      << " source_timeline_index="
                      << this->currentSourceTimeline_.sourceIndex
                      << " deadline_admission_valid="
                      << (this->deadlineBatchDecision_.valid ? 1 : 0)
                      << " deadline_semantics=" << deadlineSemantics
                      << " compute_ready_budget_ms=" << computeReadyBudgetMs
                      << " presentation_slot_budget_ms="
                      << presentationSlotBudgetMs
                      << " deadline_planned_generated="
                      << plannedGeneratedFrameCount
                      << " deadline_admitted_generated="
                      << generatedFrameCount
                      << " deadline_bootstrap_probe=0"
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
                      << " fixed_source_baseline_fps="
                      << this->fixedSourceCadenceGovernor_.telemetry().baselineSourceFps
                      << " fixed_source_interval_ratio="
                      << this->fixedSourceCadenceGovernor_.telemetry().intervalRatio
                      << " fixed_generation_limit="
                      << this->fixedSourceCadenceGovernor_.telemetry().generationLimit
                      << " fixed_source_backoff="
                      << (this->fixedSourceCadenceGovernor_.telemetry().backedOff ? 1 : 0)
                      << " fixed_source_raise="
                      << (this->fixedSourceCadenceGovernor_.telemetry().raised ? 1 : 0)
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
                "deadline_error_ms=%.3f rebases=%llu "
                "deadline_semantics=%s compute_ready_budget_ms=%.3f "
                "presentation_slot_budget_ms=%.3f planned=%zu admitted=%zu "
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
                deadlineSemantics,
                computeReadyBudgetMs,
                presentationSlotBudgetMs,
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
                "source_budget_raw_ms=%.3f source_budget_effective_ms=%.3f "
                "source_budget_copy_reserve_ms=%.3f source_budget_observation=%s "
                "history_preprocess_submit_avg_ms=%.3f "
                "history_preprocess_host_wait_avg_ms=%.3f "
                "history_async_releases=%llu history_host_completions=%llu",
                sourceBudgetRawMs,
                sourceBudgetEffectiveMs,
                sourceProtectionTelemetry.serializedCopyReserveMs,
                sourceCadenceObservationName(
                    sourceProtectionTelemetry.lastObservation),
                historyPreprocessSubmitAvgMs,
                historyPreprocessHostWaitAvgMs,
                static_cast<unsigned long long>(
                    metrics.windowHistoryAsyncReleases),
                static_cast<unsigned long long>(
                    metrics.windowHistoryHostCompletions));
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
            metrics.windowGeneratedDispatched = 0;
            metrics.windowGeneratedCompleted = 0;
            metrics.windowGeneratedCopySubmitted = 0;
            metrics.windowGeneratedWsiSubmitted = 0;
            metrics.windowGeneratedWsiAccepted = 0;
            metrics.windowGeneratedDisplayConfirmed = 0;
            metrics.windowGeneratedDisplayNotShown = 0;
            metrics.windowGeneratedDisplayUnknown = 0;
            metrics.windowDisplayTimingQueryFailures = 0;
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
            metrics.windowHistoryAsyncReleases = 0;
            metrics.windowHistoryHostCompletions = 0;
            metrics.windowDeadlineShadowOpportunities = 0;
            metrics.windowDeadlineShadowWouldAdmit = 0;
            metrics.windowDeadlineShadowWouldReject = 0;
            metrics.windowCycleMs = 0.0;
            metrics.windowCycleMaxMs = 0.0;
            metrics.windowHandoffMs = 0.0;
            metrics.windowHandoffSubmitMs = 0.0;
            metrics.windowHandoffFenceWaitMs = 0.0;
            metrics.windowHistoryPreprocessSubmitMs = 0.0;
            metrics.windowHistoryPreprocessHostWaitMs = 0.0;
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

    const bool protectedAdrenoPriorComputeOverBudget =
        this->conservativeCrossDeviceSync_
        && !sourceHistoryWarmupActive
        && this->lastSourceCadenceObservation_
            == SourceCadenceObservation::Generated
        && this->adaptiveFlowGeneratedTimingValid_
        && this->adaptiveFlowRetainedBudgetMs_ > 0.0
        && this->adaptiveFlowRetainedTotalLsfgMs_
            > this->adaptiveFlowRetainedBudgetMs_;

    // BEGIN ADRENO_364178AF_EXECUTION
    // The Qualcomm/Turnip execution path below intentionally preserves the
    // September 18 364178af transport/presentation topology. Modern scheduling
    // is allowed to choose generatedFrameCount, interpolationGenerationCount,
    // and Adaptive Flow metadata before entering this block; it may not change
    // source ownership, cross-device completion, WSI acquisition, queue choice,
    // or generated-before-source ordering inside it.
    if (this->conservativeCrossDeviceSync_) {
        // The host-fence path necessarily waits through the application's
        // render dependency before the AHB copy can complete. If the previous
        // real generated cycle already proved that private LSFG compute alone
        // exceeded the protected source budget, do not submit another AHB copy
        // and turn that producer dependency into another blocking CPU wait.
        // Escape exactly one cycle, then require one real-source history copy
        // before generation resumes. lastSourceCadenceObservation_ makes this
        // a one-shot circuit breaker rather than a stale-timing loop.
        if (protectedAdrenoPriorComputeOverBudget) {
            this->lastDispatchedGeneratedFrameCount_ = 0;
            this->lastSourceCadenceObservation_ =
                SourceCadenceObservation::SourceOnly;
            this->lastGeneratedFrameCount_ = 0;
            this->previousSourceCopySignalValid_ = false;
            this->sourceHistoryWarmupRemaining_ = 1;
            this->requiresSourceHistoryWarmup_ = true;
            this->lastHistoryInvalidationReason_ =
                SourceHistoryInvalidationReason::None;
            this->lastHistoryReprimeReason_ =
                SourceHistoryInvalidationReason::OverloadBypass;
            this->deadlineBatchDecision_ = {};
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
                    bypassPresentTime,
                    bypassPresentTimes),
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
            if (isAdrenoWsiRetirementResult(bypassResult))
                return bypassResult;
            if (bypassResult != VK_SUCCESS
                    && bypassResult != VK_SUBOPTIMAL_KHR) {
                metrics.windowSourcePresentFailures++;
                metrics.totalSourcePresentFailures++;
                throw LSFG::vulkan_error(
                    bypassResult,
                    "Failed protected Adreno overload source-only present");
            }
            if (conf.adaptiveFramegen)
                this->deadlineAdmissionPredictor_.observeSourceOnlyRecovery();
            if (firstPresentDiagnostic) {
                std::cerr
                    << "lsfg-vk: runtime stage=adreno-overload-source-bypass"
                    << " prior_lsfg_ms="
                    << this->adaptiveFlowRetainedTotalLsfgMs_
                    << " source_budget_ms="
                    << this->adaptiveFlowRetainedBudgetMs_
                    << " reprime=1\n";
            }
            return finishSourcePresent(
                bypassResult, "game-render-overload-bypass");
        }

        if (conservativeFixedSourceProtectionGap) {
            this->lastDispatchedGeneratedFrameCount_ = 0;
            this->lastSourceCadenceObservation_ =
                SourceCadenceObservation::SourceOnly;
            this->lastGeneratedFrameCount_ = 0;
            this->previousSourceCopySignalValid_ = false;
            // Direct presentation does not refresh the private AHB history.
            // Require exactly one copy-only source warmup before a later
            // generated probe is allowed back into the September 18 island.
            this->sourceHistoryWarmupRemaining_ = 1;
            this->requiresSourceHistoryWarmup_ = true;
            this->lastHistoryInvalidationReason_ =
                SourceHistoryInvalidationReason::None;
            this->lastHistoryReprimeReason_ =
                SourceHistoryInvalidationReason::AdmissionBypass;
            this->deadlineBatchDecision_ = {};
            updateAdaptiveFlowGovernor();

            VkPresentTimeGOOGLE bypassPresentTime{};
            VkPresentTimesInfoGOOGLE bypassPresentTimes{};
            const VkPresentInfoKHR bypassPresentInfo{
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .pNext = adaptivePresentPNext(
                    pNext,
                    this->currentSourceTimeline_.sourceDesiredTimeNs,
                    bypassPresentTime,
                    bypassPresentTimes),
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
            if (isAdrenoWsiRetirementResult(bypassResult))
                return bypassResult;
            if (bypassResult != VK_SUCCESS
                    && bypassResult != VK_SUBOPTIMAL_KHR) {
                metrics.windowSourcePresentFailures++;
                metrics.totalSourcePresentFailures++;
                throw LSFG::vulkan_error(
                    bypassResult,
                    "Failed protected Adreno Fixed source-only present");
            }
            if (firstPresentDiagnostic) {
                std::cerr
                    << "lsfg-vk: runtime stage=adreno-fixed-source-protection"
                    << " baseline_valid="
                    << (sourceProtectionBaselineValid ? 1 : 0)
                    << " fixed_limit="
                    << this->fixedSourceCadenceGovernor_.telemetry().generationLimit
                    << " reprime=1\n";
            }
            return finishSourcePresent(
                bypassResult, "game-render-fixed-source-protection");
        }

        // A rejected synthetic opportunity is source protection, not temporal
        // maintenance. Do not copy into the AHB pair and then host-wait through
        // a zero-count private framegen pass: that was the feedback loop that
        // collapsed the S20+ source cadence. Present the real source directly,
        // mark this as an authoritative source-only observation, and request
        // exactly one real-source reprime before generation resumes.
        if (conservativeAdmissionRejectedHistoryGap) {
            this->lastDispatchedGeneratedFrameCount_ = 0;
            this->lastSourceCadenceObservation_ =
                SourceCadenceObservation::SourceOnly;
            this->lastGeneratedFrameCount_ = 0;
            this->previousSourceCopySignalValid_ = false;
            this->sourceHistoryWarmupRemaining_ = 1;
            this->requiresSourceHistoryWarmup_ = true;
            this->lastHistoryInvalidationReason_ =
                SourceHistoryInvalidationReason::None;
            this->lastHistoryReprimeReason_ =
                SourceHistoryInvalidationReason::AdmissionBypass;
            this->deadlineBatchDecision_ = {};
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
                    bypassPresentTime,
                    bypassPresentTimes),
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
            if (isAdrenoWsiRetirementResult(bypassResult))
                return bypassResult;
            if (bypassResult != VK_SUCCESS
                    && bypassResult != VK_SUBOPTIMAL_KHR) {
                metrics.windowSourcePresentFailures++;
                metrics.totalSourcePresentFailures++;
                throw LSFG::vulkan_error(
                    bypassResult,
                    "Failed protected Adreno admission source-only present");
            }
            this->deadlineAdmissionPredictor_.observeSourceOnlyRecovery();
            if (firstPresentDiagnostic) {
                std::cerr
                    << "lsfg-vk: runtime stage=adreno-admission-source-bypass"
                    << " planned=" << plannedGeneratedFrameCount
                    << " admitted=" << generatedFrameCount
                    << " reprime=1\n";
            }
            return finishSourcePresent(
                bypassResult, "game-render-admission-bypass");
        }

        pass.preCopySemaphores.at(0) = Mini::Semaphore(info.device);
        pass.preCopySemaphores.at(1) = Mini::Semaphore(info.device);
        pass.preCopyBuf = Mini::CommandBuffer(info.device, this->cmdPool);
        pass.preCopyBuf.begin();

        copySwapchainToExternalAhb(
            pass.preCopyBuf.handle(),
            this->swapchainImages.at(presentIdx),
            this->frameIdx % 2 == 0
                ? this->frame_0.handle()
                : this->frame_1.handle(),
            this->extent.width,
            this->extent.height,
            info.queue.first,
            this->frameIdx < 2);

        pass.preCopyBuf.end();

        std::vector<VkSemaphore> gameRenderSemaphores2 = gameRenderSemaphores;
        if (this->previousSourceCopySignalValid_) {
            gameRenderSemaphores2.emplace_back(
                this->passInfos.at((this->frameIdx - 1) % 8)
                    .preCopySemaphores.at(1).handle());
        }

        const auto handoffStart = RuntimeMetrics::Clock::now();
        std::vector<VkSemaphore> preCopySignals{
            pass.preCopySemaphores.at(0).handle(),
            pass.preCopySemaphores.at(1).handle(),
        };

        // September 18 built transaction: ordinary generated cycles use the
        // selected GPU semaphore handoff (SYNC_FD on the S20+/Turnip build
        // composition, OPAQUE_FD where that payload is supported). Warmup and
        // zero-generation/history cycles keep the bounded host-fence handoff.
        bool useAsyncHandoff = this->asyncAhbHandoffEnabled_
            && generatedFrameCount > 0
            && !sourceHistoryWarmupActive;
        int framegenInputSemaphoreFd = -1;
        if (useAsyncHandoff) {
            try {
                pass.framegenInputSemaphore = Mini::Semaphore(
                    info.device, this->asyncAhbHandoffHandleType_);
                preCopySignals.emplace_back(
                    pass.framegenInputSemaphore.handle());
            } catch (const std::exception& e) {
                this->asyncAhbHandoffEnabled_ = false;
                useAsyncHandoff = false;
                metrics.totalAsyncFallbacks++;
                std::cerr
                    << "lsfg-vk: Android async AHB handoff disabled after "
                    << handoffTypeName(this->asyncAhbHandoffHandleType_)
                    << " semaphore creation failure: " << e.what()
                    << "; falling back to host fence\n";
            }
        }

        if (useAsyncHandoff) {
            const auto submitStart = RuntimeMetrics::Clock::now();
            submitAhbHandoff(
                info.device,
                pass.preCopyBuf,
                info.queue.second,
                gameRenderSemaphores2,
                preCopySignals,
                *this->ahbHandoffFence,
                this->resetHandoffFences);
            metrics.windowHandoffSubmitMs +=
                std::chrono::duration<double, std::milli>(
                    RuntimeMetrics::Clock::now() - submitStart).count();

            try {
                // SYNC_FD has copy-transference semantics. Export only after
                // the source-copy signal operation has been submitted so the
                // descriptor represents that exact GPU completion point.
                framegenInputSemaphoreFd = pass.framegenInputSemaphore.exportFd(
                    info.device, this->asyncAhbHandoffHandleType_);
                metrics.windowAsyncHandoffs++;
                metrics.totalAsyncHandoffs++;
            } catch (const std::exception& e) {
                // The copy is already queued with the reusable handoff fence.
                // Complete it before falling back so framegen never consumes
                // the shared AHB unsynchronized.
                const auto fallbackWaitStart = RuntimeMetrics::Clock::now();
                waitForAhbHandoff(
                    info.device,
                    *this->ahbHandoffFence,
                    this->waitHandoffFences);
                metrics.windowHandoffFenceWaitMs +=
                    std::chrono::duration<double, std::milli>(
                        RuntimeMetrics::Clock::now() - fallbackWaitStart).count();
                this->asyncAhbHandoffEnabled_ = false;
                useAsyncHandoff = false;
                framegenInputSemaphoreFd = -1;
                metrics.totalAsyncFallbacks++;
                metrics.windowSyncHandoffs++;
                metrics.totalSyncHandoffs++;
                std::cerr
                    << "lsfg-vk: Android async AHB handoff disabled after "
                    << handoffTypeName(this->asyncAhbHandoffHandleType_)
                    << " fd export failure: " << e.what()
                    << "; completed source copy with host-fence fallback\n";
            }
        } else {
            submitAndWaitForAhbHandoff(
                info.device,
                pass.preCopyBuf,
                info.queue.second,
                gameRenderSemaphores2,
                preCopySignals,
                *this->ahbHandoffFence,
                this->resetHandoffFences,
                this->waitHandoffFences,
                &metrics.windowHandoffSubmitMs,
                &metrics.windowHandoffFenceWaitMs);
            metrics.windowSyncHandoffs++;
            metrics.totalSyncHandoffs++;
        }
        this->previousSourceCopySignalValid_ = true;
        metrics.windowHandoffMs += std::chrono::duration<double, std::milli>(
            RuntimeMetrics::Clock::now() - handoffStart).count();

        if (firstPresentDiagnostic) {
            std::cerr << "lsfg-vk: runtime stage=source-ahb-handoff-ready mode="
                      << (useAsyncHandoff
                            ? (this->asyncAhbHandoffHandleType_
                                    == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
                                ? "gpu-sync-fd"
                                : "gpu-opaque-fd")
                            : "host-fence")
                      << " adreno_execution=sep18-built\n";
        }

        // The newer governors may create a zero-generation Fixed or Adaptive
        // cadence gap. Execute it through the same September 18 zero-count
        // private preprocessing route: source handoff is already host-complete,
        // and the backend's preprocessingFence wait completes before return.
        if (generatedFrameCount == 0 && !sourceHistoryWarmupActive) {
            this->lastDispatchedGeneratedFrameCount_ = 0;
            this->lastSourceCadenceObservation_ =
                SourceCadenceObservation::HistoryMaintenance;
            const auto adaptiveFlowBatch = nextAdaptiveFlowBatch();
            std::vector<int> noOutSems;
            const auto historyAdvanceStart = RuntimeMetrics::Clock::now();
            if (conf.performance) {
                LSFG_3_1P::presentContextWithCount(
                    *this->lsfgCtxId,
                    -1,
                    noOutSems,
                    0,
                    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
                    0,
                    adaptiveFlowBatch);
            } else {
                LSFG_3_1::presentContextWithCount(
                    *this->lsfgCtxId,
                    -1,
                    noOutSems,
                    0,
                    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
                    0,
                    adaptiveFlowBatch);
            }
            const double historyMs =
                std::chrono::duration<double, std::milli>(
                    RuntimeMetrics::Clock::now() - historyAdvanceStart).count();
            metrics.windowDispatchMs += historyMs;
            metrics.windowHistoryPreprocessHostWaitMs += historyMs;
            metrics.windowHistoryHostCompletions++;
            metrics.totalHistoryHostCompletions++;
            metrics.windowAdaptiveZeroGenerationCycles++;
            metrics.totalAdaptiveZeroGenerationCycles++;
            this->sourceHistoryWarmupRemaining_ = 0;
            this->requiresSourceHistoryWarmup_ = false;
            this->lastGeneratedFrameCount_ = 0;
            updateAdaptiveFlowGovernor();

            const VkSemaphore sourceReady =
                pass.preCopySemaphores.at(0).handle();
            VkPresentTimeGOOGLE sourcePresentTime{};
            VkPresentTimesInfoGOOGLE sourcePresentTimes{};
            const VkPresentInfoKHR sourcePresentInfo{
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .pNext = adaptivePresentPNext(
                    pNext,
                    this->currentSourceTimeline_.sourceDesiredTimeNs,
                    sourcePresentTime,
                    sourcePresentTimes),
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &sourceReady,
                .swapchainCount = 1,
                .pSwapchains = &this->swapchain,
                .pImageIndices = &presentIdx,
            };
            const auto sourceResult =
                Layer::ovkQueuePresentKHR(queue, &sourcePresentInfo);
            if (sourceResult != VK_SUCCESS
                    && sourceResult != VK_SUBOPTIMAL_KHR) {
                metrics.windowSourcePresentFailures++;
                metrics.totalSourcePresentFailures++;
                throw LSFG::vulkan_error(
                    sourceResult,
                    "Failed September 18 Adreno zero-generation source present");
            }
            return finishSourcePresent(
                sourceResult, "pre-copy-adreno-364178af-zero");
        }

        // September 18 performs one real-source copy/present warmup before the
        // first generated batch after lifecycle/history invalidation.
        if (sourceHistoryWarmupActive) {
            this->lastDispatchedGeneratedFrameCount_ = 0;
            this->lastSourceCadenceObservation_ =
                SourceCadenceObservation::HistoryMaintenance;
            this->sourceHistoryWarmupRemaining_ = 0;
            this->requiresSourceHistoryWarmup_ = false;
            this->lastGeneratedFrameCount_ = 0;
            metrics.windowAdaptiveZeroGenerationCycles++;
            metrics.totalAdaptiveZeroGenerationCycles++;
            updateAdaptiveFlowGovernor();

            const VkSemaphore sourceReady =
                pass.preCopySemaphores.at(0).handle();
            VkPresentTimeGOOGLE warmupPresentTime{};
            VkPresentTimesInfoGOOGLE warmupPresentTimes{};
            const VkPresentInfoKHR warmupPresentInfo{
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .pNext = adaptivePresentPNext(
                    pNext,
                    this->currentSourceTimeline_.sourceDesiredTimeNs,
                    warmupPresentTime,
                    warmupPresentTimes),
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &sourceReady,
                .swapchainCount = 1,
                .pSwapchains = &this->swapchain,
                .pImageIndices = &presentIdx,
            };
            const auto warmupResult =
                Layer::ovkQueuePresentKHR(queue, &warmupPresentInfo);
            if (warmupResult != VK_SUCCESS
                    && warmupResult != VK_SUBOPTIMAL_KHR) {
                metrics.windowSourcePresentFailures++;
                metrics.totalSourcePresentFailures++;
                throw LSFG::vulkan_error(
                    warmupResult,
                    "Failed September 18 Adreno source-history warmup");
            }
            if (firstPresentDiagnostic)
                std::cerr
                    << "lsfg-vk: runtime stage=source-history-warmup"
                    << " adreno_execution=364178af\n";
            return finishSourcePresent(
                warmupResult, "pre-copy-adreno-364178af-warmup");
        }

        this->lastDispatchedGeneratedFrameCount_ = generatedFrameCount;
        this->lastSourceCadenceObservation_ =
            SourceCadenceObservation::Generated;
        const auto adaptiveFlowBatch = nextAdaptiveFlowBatch();
        std::vector<int> noOutSems;

        if (firstPresentDiagnostic) {
            std::cerr << "lsfg-vk: runtime stage=framegen-dispatch-begin"
                      << " mode=" << (conf.performance ? "performance" : "quality")
                      << " generated=" << generatedFrameCount
                      << " handoff="
                      << (useAsyncHandoff
                            ? handoffTypeName(this->asyncAhbHandoffHandleType_)
                            : "host-fence")
                      << " adreno_execution=sep18-built\n";
        }

        const auto dispatchStart = RuntimeMetrics::Clock::now();
        if (conf.performance) {
            LSFG_3_1P::presentContextWithCount(
                *this->lsfgCtxId,
                framegenInputSemaphoreFd,
                noOutSems,
                generatedFrameCount,
                this->asyncAhbHandoffHandleType_,
                interpolationGenerationCount,
                adaptiveFlowBatch);
        } else {
            LSFG_3_1::presentContextWithCount(
                *this->lsfgCtxId,
                framegenInputSemaphoreFd,
                noOutSems,
                generatedFrameCount,
                this->asyncAhbHandoffHandleType_,
                interpolationGenerationCount,
                adaptiveFlowBatch);
        }
        metrics.windowGeneratedDispatched += generatedFrameCount;
        metrics.totalGeneratedDispatched += generatedFrameCount;
        metrics.windowDispatchMs += std::chrono::duration<double, std::milli>(
            RuntimeMetrics::Clock::now() - dispatchStart).count();

        // 364178af blocks only at this private-device completion boundary before
        // the game device reads generated AHBs.
        const auto waitIdleStart = RuntimeMetrics::Clock::now();
        const uint64_t framegenCompletionTimeoutNs = runtimeWaitTimeoutNs();
        const bool framegenReady = conf.performance
            ? LSFG_3_1P::waitContext(*this->lsfgCtxId, framegenCompletionTimeoutNs)
            : LSFG_3_1::waitContext(*this->lsfgCtxId, framegenCompletionTimeoutNs);
        const double framegenBlockingCompletionMs =
            std::chrono::duration<double, std::milli>(
                RuntimeMetrics::Clock::now() - waitIdleStart).count();
        metrics.windowWaitIdleMs += framegenBlockingCompletionMs;
        if (framegenReady && generatedFrameCount > 0) {
            // This is the cost that actually blocks the matching source present
            // on protected Adreno. GPU timestamps omit queue residency and were
            // admitting ~23 ms batches that took ~69 ms to reach this boundary.
            this->deadlineAdmissionPredictor_.observeBlockingCompletion(
                generatedFrameCount, framegenBlockingCompletionMs);
        }

        if (!framegenReady) {
            this->lastGeneratedFrameCount_ = 0;
            const VkSemaphore sourceReady =
                pass.preCopySemaphores.at(0).handle();
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
                throw LSFG::vulkan_error(
                    timeoutPresentResult,
                    "Failed September 18 Adreno source present after framegen timeout");
            }
            return finishSourcePresent(
                VK_ERROR_OUT_OF_DATE_KHR, "pre-copy-adreno-364178af-timeout");
        }

        metrics.windowGeneratedCompleted += generatedFrameCount;
        metrics.totalGeneratedCompleted += generatedFrameCount;
        updateAdaptiveFlowGovernor();

        // 364178af acquires each synthetic image with a bounded blocking timeout,
        // then presents every admitted synthetic on the application's queue.
        for (size_t i = 0; i < generatedFrameCount; ++i) {
            const auto generatedPresentStart = RuntimeMetrics::Clock::now();
            pass.acquireSemaphores.at(i) = Mini::Semaphore(info.device);
            uint32_t imageIdx{};
            auto res = Layer::ovkAcquireNextImageKHR(
                info.device,
                this->swapchain,
                runtimeWaitTimeoutNs(),
                pass.acquireSemaphores.at(i).handle(),
                VK_NULL_HANDLE,
                &imageIdx);
            if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
                metrics.windowGeneratedPresentFailures++;
                metrics.totalGeneratedPresentFailures++;
                throw LSFG::vulkan_error(
                    res,
                    "Failed September 18 Adreno generated swapchain acquire");
            }

            pass.postCopySemaphores.at(i) = Mini::Semaphore(info.device);
            pass.prevPostCopySemaphores.at(i) = Mini::Semaphore(info.device);
            pass.postCopyBufs.at(i) =
                Mini::CommandBuffer(info.device, this->cmdPool);
            pass.postCopyBufs.at(i).begin();

            copyExternalAhbToSwapchain(
                pass.postCopyBufs.at(i).handle(),
                this->out_n.at(i).handle(),
                this->swapchainImages.at(imageIdx),
                this->extent.width,
                this->extent.height,
                info.queue.first);

            pass.postCopyBufs.at(i).end();
            pass.postCopyBufs.at(i).submit(
                info.queue.second,
                { pass.acquireSemaphores.at(i).handle() },
                { pass.postCopySemaphores.at(i).handle(),
                  pass.prevPostCopySemaphores.at(i).handle() });
            metrics.windowGeneratedCopySubmitted++;
            metrics.totalGeneratedCopySubmitted++;

            std::vector<VkSemaphore> waitSemaphores{
                pass.postCopySemaphores.at(i).handle()
            };
            if (i != 0) {
                waitSemaphores.emplace_back(
                    pass.prevPostCopySemaphores.at(i - 1).handle());
            }

            const double syntheticFraction =
                static_cast<double>(i + 1)
                / static_cast<double>(interpolationGenerationCount + 1);
            const uint64_t syntheticDesiredTimeNs =
                this->sourceTimeline_.syntheticDesiredTimeNs(
                    this->currentSourceTimeline_,
                    syntheticFraction);
            VkPresentTimeGOOGLE generatedPresentTime{};
            VkPresentTimesInfoGOOGLE generatedPresentTimes{};
            const void* generatedDownstreamPNext = i == 0 ? pNext : nullptr;
            const VkPresentInfoKHR presentInfo{
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .pNext = generatedDisplayConfirmationPNext(
                    generatedDownstreamPNext,
                    syntheticDesiredTimeNs,
                    generatedPresentTime,
                    generatedPresentTimes),
                .waitSemaphoreCount =
                    static_cast<uint32_t>(waitSemaphores.size()),
                .pWaitSemaphores = waitSemaphores.data(),
                .swapchainCount = 1,
                .pSwapchains = &this->swapchain,
                .pImageIndices = &imageIdx,
            };
            metrics.windowGeneratedWsiSubmitted++;
            metrics.totalGeneratedWsiSubmitted++;
            res = Layer::ovkQueuePresentKHR(queue, &presentInfo);
            if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
                metrics.windowGeneratedPresentFailures++;
                metrics.totalGeneratedPresentFailures++;
                throw LSFG::vulkan_error(
                    res,
                    "Failed September 18 Adreno generated present");
            }
            metrics.windowGeneratedFrames++;
            metrics.totalGeneratedFrames++;
            metrics.windowGeneratedWsiAccepted++;
            metrics.totalGeneratedWsiAccepted++;
            trackGeneratedDisplayPresent(generatedPresentTime.presentID);
            metrics.windowGeneratedPresentMs +=
                std::chrono::duration<double, std::milli>(
                    RuntimeMetrics::Clock::now()
                    - generatedPresentStart).count();
        }

        // The source is queued after the admitted generated prefix, using the
        // second signal from the final generated post-copy exactly as 364178af.
        VkSemaphore lastPrevPostCopySemaphore =
            generatedFrameCount > 0
                ? pass.prevPostCopySemaphores
                    .at(generatedFrameCount - 1).handle()
                : pass.preCopySemaphores.at(0).handle();
        VkPresentTimeGOOGLE finalSourcePresentTime{};
        VkPresentTimesInfoGOOGLE finalSourcePresentTimes{};
        const void* finalSourceDownstreamPNext =
            generatedFrameCount == 0 ? pNext : nullptr;
        const VkPresentInfoKHR finalPresentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = adaptivePresentPNext(
                finalSourceDownstreamPNext,
                this->currentSourceTimeline_.sourceDesiredTimeNs,
                finalSourcePresentTime,
                finalSourcePresentTimes),
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &lastPrevPostCopySemaphore,
            .swapchainCount = 1,
            .pSwapchains = &this->swapchain,
            .pImageIndices = &presentIdx,
        };
        auto res =
            Layer::ovkQueuePresentKHR(queue, &finalPresentInfo);
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
            metrics.windowSourcePresentFailures++;
            metrics.totalSourcePresentFailures++;
            throw LSFG::vulkan_error(
                res,
                "Failed September 18 Adreno source present");
        }

        this->lastGeneratedFrameCount_ = generatedFrameCount;
        if (generatedFrameCount > 0) {
            this->deadlineAdmissionPredictor_.observeDeliverySuccess();
        }
        return finishSourcePresent(
            res, "prev-post-copy-adreno-364178af");
    }
    // END ADRENO_364178AF_EXECUTION

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
        this->lastSourceCadenceObservation_ =
            SourceCadenceObservation::SourceOnly;
        this->lastGeneratedFrameCount_ = 0;
        this->previousSourceCopySignalValid_ = false;
        this->sourceHistoryWarmupRemaining_ =
            kConservativeSourceReprimeFrames - 1;
        this->requiresSourceHistoryWarmup_ = true;
        this->lastHistoryInvalidationReason_ =
            SourceHistoryInvalidationReason::None;
        this->lastHistoryReprimeReason_ =
            conservativeBatchStillInFlight
                ? SourceHistoryInvalidationReason::RetirementBackpressure
                : SourceHistoryInvalidationReason::FractionalGap;
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
    bool consumePendingHistoryComplete = false;
    if (this->frameIdx > 0)
        previousPass = &this->passInfos.at((this->frameIdx - 1) % 8);
    if (this->conservativeCrossDeviceSync_
            && this->conservativePendingHistoryCompleteValid_) {
        // A protected zero-generation pass may be followed by one or more
        // source-only bypasses. Carry its private-device release independently
        // of the pass ring until an actual source AHB copy consumes it.
        metrics.windowHandoffBatchDeps++;
        pass.crossFrameWaitRetentions.emplace_back(
            this->conservativePendingHistoryCompleteSemaphore_);
        gameRenderSemaphores2.emplace_back(
            pass.crossFrameWaitRetentions.back().handle());
        consumePendingHistoryComplete = true;
    }
    if (this->previousSourceCopySignalValid_ && previousPass != nullptr) {
        metrics.windowHandoffPrevSourceDeps++;
        pass.crossFrameWaitRetentions.emplace_back(
            previousPass->preCopySemaphores.at(1));
        gameRenderSemaphores2.emplace_back(
            pass.crossFrameWaitRetentions.back().handle());
    }
    bool consumeDeferredAdrenoBatchComplete = false;
    if (this->deferredAdrenoBatchCompleteReady_) {
        metrics.windowHandoffBatchDeps++;
        if (this->deferredAdrenoBatchCompleteSemaphore_.handle() != VK_NULL_HANDLE) {
            pass.crossFrameWaitRetentions.emplace_back(
                this->deferredAdrenoBatchCompleteSemaphore_);
            gameRenderSemaphores2.emplace_back(
                pass.crossFrameWaitRetentions.back().handle());
        }
        consumeDeferredAdrenoBatchComplete = true;
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
    // cycle. Ordinary generated Adreno cycles use the validated OPAQUE_FD
    // GPU-semaphore handoff; warmup and zero-generation history cycles use the
    // conservative host-fence path. No generated completion is deferred.
    bool useAsyncHandoff =
        this->asyncAhbHandoffEnabled_
        && (!this->conservativeCrossDeviceSync_
            || (!conservativeSourceOnlyWarmup
                && !conservativeHistoryGap
                && !conservativeTrueSourceOnlyCycle));
    bool asyncSubmissionIssued = false;
    bool asyncExportFailed = false;
    int framegenInputSemaphoreFd = -1;

    if (useAsyncHandoff) {
        try {
            if (this->conservativeCrossDeviceSync_) {
                // September 18 Adreno path: OPAQUE_FD is a reusable handle, so
                // export it before the source-copy submission. This keeps export
                // failure recoverable by the same host-fence fallback used by
                // 364178af instead of stranding an already-submitted copy.
                pass.framegenInputSemaphore =
                    Mini::Semaphore(info.device, &framegenInputSemaphoreFd);
            } else {
                // Generic/Xclipse may use SYNC_FD, which must be exported only
                // after its signal operation has been submitted.
                pass.framegenInputSemaphore = Mini::Semaphore(
                    info.device, this->asyncAhbHandoffHandleType_);
            }
            preCopySignals.emplace_back(pass.framegenInputSemaphore.handle());
        } catch (const std::exception& e) {
            this->asyncAhbHandoffEnabled_ = false;
            useAsyncHandoff = false;
            framegenInputSemaphoreFd = -1;
            metrics.totalAsyncFallbacks++;
            std::cerr << "lsfg-vk: Android async AHB handoff disabled after "
                      << handoffTypeName(this->asyncAhbHandoffHandleType_)
                      << " semaphore creation/export failure: " << e.what()
                      << "; falling back to host fence\n";
        }
    }

    if (useAsyncHandoff) {
        const auto asyncSubmitStart = RuntimeMetrics::Clock::now();
        submitAhbHandoff(info.device, pass.preCopyBuf, info.queue.second,
            gameRenderSemaphores2, preCopySignals,
            this->conservativeCrossDeviceSync_
                ? *this->ahbHandoffFence
                : VK_NULL_HANDLE,
            this->conservativeCrossDeviceSync_
                ? this->resetHandoffFences
                : nullptr);
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
        if (consumePendingHistoryComplete) {
            this->conservativePendingHistoryCompleteValid_ = false;
            this->conservativePendingHistoryCompleteSemaphore_ = {};
        }

        if (this->conservativeCrossDeviceSync_) {
            metrics.windowAsyncHandoffs++;
            metrics.totalAsyncHandoffs++;
        } else {
            try {
                // SYNC_FD copy transference requires its signal operation to be
                // submitted before vkGetSemaphoreFdKHR.
                framegenInputSemaphoreFd = pass.framegenInputSemaphore.exportFd(
                    info.device, this->asyncAhbHandoffHandleType_);
                metrics.windowAsyncHandoffs++;
                metrics.totalAsyncHandoffs++;
            } catch (const std::exception& e) {
                // The generic/SYNC_FD copy was already submitted without a
                // reusable fence. Fail open rather than dispatch unsynchronized.
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
    }

    bool queuedCopyWithoutHostWait = false;
    if (!useAsyncHandoff && !asyncSubmissionIssued) {
        // Warmup/history and capability-limited Adreno still use the proven
        // host-fence source handoff. The fence wall time includes the game's
        // render dependency, so it is diagnostics only and must never be
        // charged wholesale as LSFG source-copy cost.
        submitAndWaitForAhbHandoff(
            info.device, pass.preCopyBuf, info.queue.second,
            gameRenderSemaphores2, preCopySignals,
            *this->ahbHandoffFence, this->resetHandoffFences,
            this->waitHandoffFences,
            &metrics.windowHandoffSubmitMs,
            &metrics.windowHandoffFenceWaitMs);
        metrics.windowSyncHandoffs++;
        metrics.totalSyncHandoffs++;
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
        if (consumePendingHistoryComplete) {
            this->conservativePendingHistoryCompleteValid_ = false;
            this->conservativePendingHistoryCompleteSemaphore_ = {};
        }
    }
    if (consumeDeferredAdrenoBatchComplete) {
        this->deferredAdrenoBatchCompleteReady_ = false;
        this->deferredAdrenoBatchCompleteSemaphore_ = {};
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
        this->lastSourceCadenceObservation_ =
            SourceCadenceObservation::HistoryMaintenance;
        this->sourceHistoryWarmupRemaining_ =
            this->conservativeCrossDeviceSync_
                ? kConservativeSourceReprimeFrames
                : kSourceHistoryWarmupFrames;
        this->requiresSourceHistoryWarmup_ = true;
        this->lastHistoryInvalidationReason_ =
            SourceHistoryInvalidationReason::SyncExportFailure;
        this->lastHistoryReprimeReason_ =
            SourceHistoryInvalidationReason::SyncExportFailure;
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

    // Adaptive/fixed zero-generation gaps are ordinary temporal-history
    // cycles. They continue through the zero-count framegen path below so the
    // alternating source AHB pair and private temporal index stay coherent.
    // Only genuine warmup/discontinuity/ownership cases use source-only bypass.
    if (conservativeSourceOnlyWarmup) {
        this->lastDispatchedGeneratedFrameCount_ = 0;
        this->lastSourceCadenceObservation_ =
            SourceCadenceObservation::HistoryMaintenance;
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
        this->lastSourceCadenceObservation_ =
            SourceCadenceObservation::HistoryMaintenance;
        const auto adaptiveFlowBatch = nextAdaptiveFlowBatch();

        // Zero-generation cadence still refreshes Mipmaps/Alpha temporal state.
        // Protected Adreno deliberately uses the September 18 host-complete
        // zero-count path before presenting the matching source. Xclipse/generic
        // capability paths may keep exporting their existing asynchronous
        // history release. Generated output is never deferred.
        std::vector<int> noOutSems;
        LSFG::AndroidFrameSyncFds historySync{};
        bool historyRequiresHostCompletionWait = false;
        bool historyReleaseImported = false;
        const bool exportExistingAsyncHistory =
            this->asyncFramegenCompletionEnabled_ && useAsyncHandoff;
        const bool exportHistoryRelease = exportExistingAsyncHistory;
        const auto historyAdvanceStart = RuntimeMetrics::Clock::now();

        if (exportHistoryRelease) {
            const int historyInputFd =
                useAsyncHandoff ? framegenInputSemaphoreFd : -1;
            const auto historyInputHandleType =
                useAsyncHandoff
                    ? this->asyncAhbHandoffHandleType_
                    : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
            historySync = conf.performance
                ? LSFG_3_1P::presentContextWithCountExportSyncFd(
                    *this->lsfgCtxId, historyInputFd, 0,
                    historyInputHandleType, 0, adaptiveFlowBatch)
                : LSFG_3_1::presentContextWithCountExportSyncFd(
                    *this->lsfgCtxId, historyInputFd, 0,
                    historyInputHandleType, 0, adaptiveFlowBatch);

            for (const int fd : historySync.outputReadyFds)
                if (fd >= 0) ::close(fd);

            if (historySync.gpuDependenciesExported
                    && historySync.batchCompleteFd >= 0) {
                try {
                    if (this->conservativeCrossDeviceSync_) {
                        this->conservativePendingHistoryCompleteSemaphore_ =
                            Mini::Semaphore(
                                info.device, historySync.batchCompleteFd,
                                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
                        this->conservativePendingHistoryCompleteValid_ = true;
                    } else {
                        // Preserve the existing Xclipse/generic history release
                        // ownership exactly as before.
                        pass.framegenBatchCompleteSemaphore = Mini::Semaphore(
                            info.device, historySync.batchCompleteFd,
                            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
                        pass.framegenBatchCompleteValid = true;
                    }
                    historySync.batchCompleteFd = -1;
                    historyReleaseImported = true;
                    metrics.windowHistoryAsyncReleases++;
                    metrics.totalHistoryAsyncReleases++;
                } catch (const std::exception& e) {
                    if (historySync.batchCompleteFd >= 0)
                        ::close(historySync.batchCompleteFd);
                    historySync.batchCompleteFd = -1;
                    if (this->conservativeCrossDeviceSync_) {
                        this->conservativePendingHistoryCompleteValid_ = false;
                        this->conservativePendingHistoryCompleteSemaphore_ = {};
                        this->asyncHistoryCompletionEnabled_ = false;
                    } else {
                        pass.framegenBatchCompleteValid = false;
                        this->asyncFramegenCompletionEnabled_ = false;
                    }
                    historyRequiresHostCompletionWait = true;
                    std::cerr
                        << "lsfg-vk: zero-history completion SYNC_FD import failed: "
                        << e.what() << "; using bounded host fallback\n";
                }
            } else if (historySync.hostWaitFallback) {
                // The framegen backend already completed the zero-count pass
                // synchronously after export/setup failure.
                if (this->conservativeCrossDeviceSync_) {
                    this->conservativePendingHistoryCompleteValid_ = false;
                    this->conservativePendingHistoryCompleteSemaphore_ = {};
                } else {
                    pass.framegenBatchCompleteValid = false;
                }
                metrics.windowHistoryHostCompletions++;
                metrics.totalHistoryHostCompletions++;
            } else {
                if (this->conservativeCrossDeviceSync_) {
                    this->conservativePendingHistoryCompleteValid_ = false;
                    this->conservativePendingHistoryCompleteSemaphore_ = {};
                } else {
                    pass.framegenBatchCompleteValid = false;
                }
                historyRequiresHostCompletionWait = true;
                if (this->conservativeCrossDeviceSync_)
                    this->asyncHistoryCompletionEnabled_ = false;
                else
                    this->asyncFramegenCompletionEnabled_ = false;
            }
        } else {
            if (this->conservativeCrossDeviceSync_) {
                this->conservativePendingHistoryCompleteValid_ = false;
                this->conservativePendingHistoryCompleteSemaphore_ = {};
            } else {
                pass.framegenBatchCompleteValid = false;
            }
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

            if (this->conservativeCrossDeviceSync_) {
                // Protected Adreno's non-export API already completes the
                // zero-count private preprocessing before returning.
                metrics.windowHistoryHostCompletions++;
                metrics.totalHistoryHostCompletions++;
            } else {
                // Preserve the pre-repair Xclipse/generic fallback behavior:
                // retain the explicit bounded completion check after the
                // non-export zero-count API returns.
                historyRequiresHostCompletionWait = true;
            }
        }

        const double historyCallMs = std::chrono::duration<double, std::milli>(
            RuntimeMetrics::Clock::now() - historyAdvanceStart).count();
        if (historyReleaseImported) {
            metrics.windowHistoryPreprocessSubmitMs += historyCallMs;
        } else if (!historyRequiresHostCompletionWait) {
            // hostWaitFallback and the non-export API both return only after
            // private preprocessing completed.
            metrics.windowHistoryPreprocessHostWaitMs += historyCallMs;
        } else {
            // Export succeeded but game-device import failed; this portion is
            // submission/export time. The explicit fallback wait is timed below.
            metrics.windowHistoryPreprocessSubmitMs += historyCallMs;
        }
        metrics.windowDispatchMs += historyCallMs;

        // A zero-count framegen submission still advances the private temporal
        // source pair. Keep the Adreno source-copy index in lockstep with that
        // private history; genuine source-only warmup never enters this block.
        if (this->conservativeCrossDeviceSync_)
            ++this->conservativeFramegenSourceIndex_;

        if (historyRequiresHostCompletionWait) {
            const auto historyWaitStart = RuntimeMetrics::Clock::now();
            const uint64_t historyTimeoutNs = runtimeWaitTimeoutNs();
            const bool historyReady = conf.performance
                ? LSFG_3_1P::waitContext(*this->lsfgCtxId, historyTimeoutNs)
                : LSFG_3_1::waitContext(*this->lsfgCtxId, historyTimeoutNs);
            const double historyWaitMs =
                std::chrono::duration<double, std::milli>(
                    RuntimeMetrics::Clock::now() - historyWaitStart).count();
            metrics.windowWaitIdleMs += historyWaitMs;
            metrics.windowHistoryPreprocessHostWaitMs += historyWaitMs;
            metrics.windowHistoryHostCompletions++;
            metrics.totalHistoryHostCompletions++;
            if (!historyReady) {
                this->sourceHistoryWarmupRemaining_ =
                    kSourceHistoryWarmupFrames;
                this->requiresSourceHistoryWarmup_ = true;
                this->lastGeneratedFrameCount_ = 0;
                const VkSemaphore sourceReady =
                    pass.preCopySemaphores.at(0).handle();
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
                this->retainPresentWait(
                    presentIdx, pass.preCopySemaphores.at(0));
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
                      << (historyReleaseImported ? 1 : 0)
                      << " history_release="
                      << (historyReleaseImported
                            ? "sync-fd-next-source-copy"
                            : (historySync.hostWaitFallback
                                ? "backend-host-fallback"
                                : "host-complete"))
                      << " history_warmup_remaining="
                      << this->sourceHistoryWarmupRemaining_
                      << " discontinuity="
                      << (adaptiveTelemetry.discontinuityReset ? 1 : 0)
                      << "\n";
        }
        return finishSourcePresent(
            adaptiveSourceResult, "pre-copy-history-only");
    }


    this->lastDispatchedGeneratedFrameCount_ = generatedFrameCount;
    this->lastSourceCadenceObservation_ =
        SourceCadenceObservation::Generated;
    const auto adaptiveFlowBatch = nextAdaptiveFlowBatch();

    // 2. Tell framegen to generate intermediary frames. Xclipse/generic
    //    capability paths may export output-ready and batch-complete SYNC_FDs
    //    for asynchronous completion. Protected Adreno deliberately does not:
    //    its OPAQUE_FD source handoff is followed by the bounded host completion
    //    wait below before generated AHBs are consumed on the game device.
    std::vector<int> noOutSems;
    std::vector<bool> outputReadyWaitValid(generatedFrameCount, false);
    LSFG::AndroidFrameSyncFds framegenSync{};
    if (firstPresentDiagnostic) {
        std::cerr << "lsfg-vk: runtime stage=framegen-dispatch-begin mode="
                  << (conf.performance ? "performance" : "quality")
                  << " batch_id=" << adaptiveFlowBatch.batchId
                  << " generated=" << generatedFrameCount
                  << " handoff=" << (useAsyncHandoff
                        ? handoffTypeName(this->asyncAhbHandoffHandleType_)
                        : "host-fence")
                  << "\n";
    }
    const auto dispatchStart = RuntimeMetrics::Clock::now();
    if (this->asyncFramegenCompletionEnabled_
            || this->deferredAdrenoCompletionEnabled_) {
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
    metrics.windowGeneratedDispatched += generatedFrameCount;
    metrics.totalGeneratedDispatched += generatedFrameCount;
    metrics.windowDispatchMs += std::chrono::duration<double, std::milli>(
        RuntimeMetrics::Clock::now() - dispatchStart).count();
    if (firstPresentDiagnostic)
        std::cerr << "lsfg-vk: runtime stage=framegen-dispatch-returned"
                  << " completion="
                  << (framegenSync.gpuDependenciesExported ? "sync-fd"
                      : (framegenSync.hostWaitFallback ? "host-fallback"
                          : "host-wait"))
                  << "\n";

    bool requireHostCompletionWait =
        !this->asyncFramegenCompletionEnabled_
        && !this->deferredAdrenoCompletionEnabled_;

    if (this->deferredAdrenoCompletionEnabled_) {
        const bool deferredExportValid =
            framegenSync.gpuDependenciesExported
            && framegenSync.outputReadyFds.size() == generatedFrameCount;
        if (deferredExportValid) {
            for (int& oldFd : this->deferredAdrenoOutputReadyFds_) {
                if (oldFd >= 0)
                    ::close(oldFd);
            }
            this->deferredAdrenoOutputReadyFds_ =
                std::move(framegenSync.outputReadyFds);
            this->deferredAdrenoBatchCompleteFd_ =
                framegenSync.batchCompleteFd;
            framegenSync.batchCompleteFd = -1;
            this->deferredAdrenoPassIndex_ = this->frameIdx % this->passInfos.size();
            this->deferredAdrenoGeneratedCount_ = generatedFrameCount;
            this->deferredAdrenoBatchId_ = framegenSync.batchId;
            this->deferredAdrenoSourceAge_ = 0;
            this->deferredAdrenoOutputEligible_ = true;
            this->deferredAdrenoBatchCompleteReady_ =
                this->deferredAdrenoBatchCompleteFd_ < 0;
            this->deferredAdrenoBatchCompleteSemaphore_ = {};
            this->deferredAdrenoBatchValid_ = true;
            pass.deferredAdrenoOwned = true;
            pass.framegenBatchCompleteValid = false;
            deferredAdrenoBatchQueuedThisCycle = true;
            requireHostCompletionWait = false;

            if (firstPresentDiagnostic) {
                std::cerr << "lsfg-vk: runtime stage=adreno-deferred-batch-queued"
                          << " batch_id=" << this->deferredAdrenoBatchId_
                          << " generated=" << generatedFrameCount
                          << " pass=" << this->deferredAdrenoPassIndex_
                          << "\n";
            }

            // The generated outputs interpolate into this real source, so
            // presenting the source now would put any deferred synthetic after
            // the frame it should precede. Buffer one real source boundary while
            // returning to the guest immediately; the next source call first
            // gives ready synthetics one opportunity, then presents this source.
            updateAdaptiveFlowGovernor();
            this->lastGeneratedFrameCount_ =
                deferredDeliveredGeneratedFrameCount;

            if (pNext != nullptr) {
                // An opaque application present chain cannot be retained safely
                // across calls. Fail open to the real source and abandon only
                // this batch's synthetic delivery; keep its batch-complete
                // lifetime state until the private device releases the AHB pair.
                this->deferredAdrenoOutputEligible_ = false;
                this->framegenOutputEligible_ = false;
                for (int& fd : this->deferredAdrenoOutputReadyFds_) {
                    if (fd >= 0)
                        ::close(fd);
                    fd = -1;
                }

                const VkSemaphore sourceReady =
                    pass.preCopySemaphores.at(0).handle();
                const VkPresentInfoKHR deferredSourcePresentInfo{
                    .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                    .pNext = pNext,
                    .waitSemaphoreCount = 1,
                    .pWaitSemaphores = &sourceReady,
                    .swapchainCount = 1,
                    .pSwapchains = &this->swapchain,
                    .pImageIndices = &presentIdx,
                };
                this->retainPresentWait(
                    presentIdx, pass.preCopySemaphores.at(0));
                const auto deferredSourceResult =
                    Layer::ovkQueuePresentKHR(
                        queue, &deferredSourcePresentInfo);
                if (isAdrenoWsiRetirementResult(deferredSourceResult))
                    return deferredSourceResult;
                if (deferredSourceResult != VK_SUCCESS
                        && deferredSourceResult != VK_SUBOPTIMAL_KHR) {
                    metrics.windowSourcePresentFailures++;
                    metrics.totalSourcePresentFailures++;
                    throw LSFG::vulkan_error(
                        deferredSourceResult,
                        "Failed source present after deferred Adreno pNext fail-open");
                }
                return finishSourcePresent(
                    deferredSourceResult,
                    "pre-copy-adreno-deferred-pnext-fail-open");
            }

            this->pendingSourceValid_ = true;
            this->pendingSourceImage_ = presentIdx;
            this->pendingSourceReady_ = pass.preCopySemaphores.at(0);
            this->pendingPassIndex_ =
                this->frameIdx % this->passInfos.size();
            this->pendingGeneratedCount_ = generatedFrameCount;
            this->framegenInFlight_ = true;
            this->framegenOutputEligible_ = true;
            if (firstPresentDiagnostic) {
                std::cerr
                    << "lsfg-vk: runtime stage=adreno-deferred-source-buffered"
                    << " image=" << presentIdx
                    << " generated=" << generatedFrameCount
                    << "\n";
            }
            return finishSourcePresent(
                VK_SUCCESS, "pre-copy-adreno-deferred-buffered");
        }

        for (const int fd : framegenSync.outputReadyFds) {
            if (fd >= 0)
                ::close(fd);
        }
        framegenSync.outputReadyFds.clear();
        if (framegenSync.batchCompleteFd >= 0) {
            ::close(framegenSync.batchCompleteFd);
            framegenSync.batchCompleteFd = -1;
        }
        if (framegenSync.hostWaitFallback) {
            // Backend already completed the batch synchronously.
            requireHostCompletionWait = false;
        } else {
            // Export failed before a safe dependency could be retained.
            requireHostCompletionWait = true;
        }
    }

    if (this->asyncFramegenCompletionEnabled_
            && framegenSync.gpuDependenciesExported) {
        bool importFailed = framegenSync.outputReadyFds.size() != generatedFrameCount;
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
    if (requireHostCompletionWait && framegenReady) {
        metrics.windowGeneratedCompleted += generatedFrameCount;
        metrics.totalGeneratedCompleted += generatedFrameCount;
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
        if (adrenoHostCompletionFallback) {
            // Preserve the last valid source pair and request normal context
            // recreation on a genuine bounded completion timeout. Do not enter
            // the broken single-queue "batch still in flight" history loop.
            this->conservativePendingBatchCompleteValid_ = false;
            this->conservativePendingBatchCompleteSemaphore_ = {};
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
        // Preserve the September 18 Adreno WSI transaction: once the
        // source-protection governors admit a generated batch, generated-image
        // acquisition uses the same bounded wait as 364178af. The newer
        // nonblocking/drop policy remains isolated to Xclipse/generic.
        const uint64_t generatedAcquireTimeoutNs =
            this->conservativeCrossDeviceSync_
                ? runtimeWaitTimeoutNs()
                : 0;
        auto res = Layer::ovkAcquireNextImageKHR(
            info.device, this->swapchain, generatedAcquireTimeoutNs,
            pass.acquireSemaphores.at(i).handle(), VK_NULL_HANDLE, &imageIdx);
        if (!this->conservativeCrossDeviceSync_
                && (res == VK_NOT_READY || res == VK_TIMEOUT)) {
            // Generic/Xclipse keeps opportunistic downstream-capacity drops.
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
        metrics.windowGeneratedCopySubmitted++;
        metrics.totalGeneratedCopySubmitted++;

        std::vector<VkSemaphore> waitSemaphores{ pass.postCopySemaphores.at(i).handle() };
        if (i != 0) waitSemaphores.emplace_back(pass.prevPostCopySemaphores.at(i - 1).handle());

        VkPresentTimeGOOGLE generatedPresentTime{};
        VkPresentTimesInfoGOOGLE generatedPresentTimes{};
        const void* generatedDownstreamPNext =
            this->conservativeCrossDeviceSync_ && i == 0
                ? pNext
                : nullptr;
        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            // September 18 attached the application's present chain to the first
            // generated Adreno present. Keep that ownership contract on the
            // protected path; Xclipse/generic retains the newer source-owned
            // downstream chain.
            .pNext = generatedDisplayConfirmationPNext(
                generatedDownstreamPNext,
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
        metrics.windowGeneratedWsiSubmitted++;
        metrics.totalGeneratedWsiSubmitted++;
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
        metrics.windowGeneratedWsiAccepted++;
        metrics.totalGeneratedWsiAccepted++;
        trackGeneratedDisplayPresent(generatedPresentTime.presentID);
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
    if (this->deferredAdrenoCompletionEnabled_
            && !deferredAdrenoBatchQueuedThisCycle) {
        deferredDeliveredGeneratedFrameCount += queuedGeneratedFrameCount;
    }

    // 5. Present the real game frame after only the synthetic frames that were
    // actually queued. A WSI drop therefore shortens this cycle instead of
    // making the source wait for an unavailable synthetic swapchain image.
    VkSemaphore lastPrevPostCopySemaphore = queuedGeneratedFrameCount > 0
        ? pass.prevPostCopySemaphores.at(queuedGeneratedFrameCount - 1).handle()
        : pass.preCopySemaphores.at(0).handle();
    VkPresentTimeGOOGLE finalSourcePresentTime{};
    VkPresentTimesInfoGOOGLE finalSourcePresentTimes{};
    const void* finalSourceDownstreamPNext =
        this->conservativeCrossDeviceSync_
            ? (queuedGeneratedFrameCount == 0 ? pNext : nullptr)
            : pNext;
    const VkPresentInfoKHR finalPresentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = adaptivePresentPNext(
            finalSourceDownstreamPNext,
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

void LsContext::resetAdaptiveSourceEpoch(
        bool resetScheduler,
        SourceHistoryInvalidationReason reason) {
    // Any source-timeline epoch change invalidates synthetic pixels produced
    // against the previous cadence/history. Keep the private-device batch
    // release alive until it retires so shared AHB reuse remains ordered.
    if (this->deferredAdrenoBatchValid_) {
        this->deferredAdrenoOutputEligible_ = false;
        for (int& fd : this->deferredAdrenoOutputReadyFds_) {
            if (fd >= 0)
                ::close(fd);
            fd = -1;
        }
    }

    if (resetScheduler)
        this->adaptiveScheduler_.reset();
    this->fixedSourceCadenceGovernor_.reset();
    this->sourceProtectionBudgetTracker_.reset();
    this->advanceAdaptiveFlowTimingEpoch();
    this->deadlineAdmissionPredictor_.reset();
    this->generatedPresentationCapacityTracker_.reset();
    this->lsfgOutputCadenceTracker_.reset();
    this->deadlineBatchDecision_ = {};
    this->sourceTimeline_.reset();
    this->currentSourceTimeline_ = {};
    this->adaptivePresentPeriodNs_ = 0;

    this->sourceHistoryWarmupRemaining_ =
        this->conservativeCrossDeviceSync_ ? 1U : kSourceHistoryWarmupFrames;
    this->requiresSourceHistoryWarmup_ =
        this->sourceHistoryWarmupRemaining_ > 0;
    this->lastHistoryInvalidationReason_ = reason;
    this->lastHistoryReprimeReason_ = reason;
    this->lastGeneratedFrameCount_ = 0;
    this->lastDispatchedGeneratedFrameCount_ = 0;
    this->lastSourceCadenceObservation_ =
        SourceCadenceObservation::SourceOnly;
    // A new temporal epoch must never chain source-copy parity or cadence
    // timestamps from the previous epoch. Private-device batch releases remain
    // retained above and are still consumed before shared AHB reuse.
    this->previousSourceCopySignalValid_ = false;
    this->runtimeMetrics.hasLastSourcePresent = false;
    this->runtimeMetrics.lastSourcePresent = {};

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
    // Explicit Off/source-only transitions invalidate deferred synthetic output,
    // but never discard the private-device batch release itself. The latter must
    // still retire before either shared input AHB can be reused.
    if (this->deferredAdrenoBatchValid_) {
        this->deferredAdrenoOutputEligible_ = false;
        for (int& fd : this->deferredAdrenoOutputReadyFds_) {
            if (fd >= 0)
                ::close(fd);
            fd = -1;
        }
    }

    this->advanceAdaptiveFlowTimingEpoch();
    this->adaptiveScheduler_.reset();
    this->fixedSourceCadenceGovernor_.reset();
    this->sourceProtectionBudgetTracker_.reset();
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
        this->conservativeCrossDeviceSync_ ? 1U : kSourceHistoryWarmupFrames;
    this->requiresSourceHistoryWarmup_ =
        this->sourceHistoryWarmupRemaining_ > 0;
    this->lastHistoryInvalidationReason_ =
        SourceHistoryInvalidationReason::ContextRecreate;
    this->lastHistoryReprimeReason_ =
        SourceHistoryInvalidationReason::ContextRecreate;
    this->lastDispatchedGeneratedFrameCount_ = 0;
    this->lastSourceCadenceObservation_ =
        SourceCadenceObservation::SourceOnly;
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

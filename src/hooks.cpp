#include "hooks.hpp"
#include "common/exception.hpp"
#include "config/config.hpp"
#include "utils/utils.hpp"
#include "context.hpp"
#include "layer.hpp"
#include "source_frame_pacer.hpp"

#include <vulkan/vulkan_core.h>

#include <unordered_map>
#include <filesystem>
#include <stdexcept>
#include <algorithm>
#include <exception>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>

using namespace Hooks;

static VkInstance layerInstance{};

namespace Layer {
    VkResult ovkEnumerateDeviceExtensionProperties(
            VkPhysicalDevice physicalDevice,
            uint32_t* pPropertyCount,
            VkExtensionProperties* pProperties) {
        if (layerInstance == VK_NULL_HANDLE)
            return VK_ERROR_INITIALIZATION_FAILED;

        auto enumerateDeviceExtensions =
            reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                ovkGetInstanceProcAddr(layerInstance,
                    "vkEnumerateDeviceExtensionProperties"));
        if (enumerateDeviceExtensions == nullptr)
            return VK_ERROR_INITIALIZATION_FAILED;

        return enumerateDeviceExtensions(
            physicalDevice, nullptr, pPropertyCount, pProperties);
    }
}

namespace {

    bool requiresSwapchainRecreation(
            const Config::Configuration& previous,
            const Config::Configuration& next) {
        const bool previousNeedsFgWsi = previous.enable && previous.multiplier > 1;
        const bool nextNeedsFgWsi = next.enable && next.multiplier > 1;
        if (previousNeedsFgWsi != nextNeedsFgWsi)
            return true;
        if (!nextNeedsFgWsi)
            return false;
        return previous.dll != next.dll
            || previous.hdr != next.hdr
            || previous.multiplier != next.multiplier
            || previous.e_present != next.e_present;
    }

    bool supportsDeviceExtension(VkPhysicalDevice physicalDevice, const char* extensionName) {
        uint32_t count{};
        auto res = Layer::ovkEnumerateDeviceExtensionProperties(
            physicalDevice, &count, nullptr);
        if (res != VK_SUCCESS || count == 0)
            return false;

        std::vector<VkExtensionProperties> extensions(count);
        res = Layer::ovkEnumerateDeviceExtensionProperties(
            physicalDevice, &count, extensions.data());
        if (res != VK_SUCCESS)
            return false;
        extensions.resize(count);

        return std::any_of(extensions.begin(), extensions.end(),
            [extensionName](const VkExtensionProperties& extension) {
                return std::string(extension.extensionName) == extensionName;
            });
    }

    VkResult myvkCreateInstance(
            const VkInstanceCreateInfo* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkInstance* pInstance) {
#ifdef __ANDROID__
        // The Android game-side AHB path does not consume any of the desktop
        // external-memory capability instance extensions. Preserve the game's
        // instance extension list exactly so promoted/omitted KHR aliases on a
        // stock ICD cannot make an otherwise valid instance creation fail.
        auto res = Layer::ovkCreateInstance(pCreateInfo, pAllocator, pInstance);
#else
        auto extensions = Utils::addExtensions(
            pCreateInfo->ppEnabledExtensionNames,
            pCreateInfo->enabledExtensionCount,
            {
                "VK_KHR_get_physical_device_properties2",
                "VK_KHR_external_memory_capabilities",
                "VK_KHR_external_semaphore_capabilities"
            }
        );
        VkInstanceCreateInfo createInfo = *pCreateInfo;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        auto res = Layer::ovkCreateInstance(&createInfo, pAllocator, pInstance);
        if (res == VK_ERROR_EXTENSION_NOT_PRESENT)
            throw std::runtime_error(
                "Required Vulkan instance extensions are not present."
                "Your GPU driver is not supported.");
#endif
        if (res == VK_SUCCESS)
            layerInstance = *pInstance;
        return res;
    }

    std::unordered_map<VkDevice, DeviceInfo> deviceToInfo;

    VkResult myvkCreateDevicePre(
            VkPhysicalDevice physicalDevice,
            const VkDeviceCreateInfo* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkDevice* pDevice) {
#ifdef __ANDROID__
        // AHB is required by this Android exchange path, but LSFG must never
        // turn a missing optional capability into failure of the game's own
        // Vulkan device. Probe first and fail open to the unmodified create info.
        const bool ahbSupported = supportsDeviceExtension(physicalDevice,
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
        if (!ahbSupported) {
            std::cerr << "lsfg-vk: init stage=ahb-extension-unavailable; "
                         "creating game device without LSFG AHB augmentation\n";
            return Layer::ovkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
        }
        auto extensions = Utils::addExtensions(
            pCreateInfo->ppEnabledExtensionNames,
            pCreateInfo->enabledExtensionCount,
            { VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME }
        );
#else
        auto extensions = Utils::addExtensions(
            pCreateInfo->ppEnabledExtensionNames,
            pCreateInfo->enabledExtensionCount,
            {
                VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
                VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
                VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME
            }
        );
#endif
        VkDeviceCreateInfo createInfo = *pCreateInfo;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        auto res = Layer::ovkCreateDevice(physicalDevice, &createInfo, pAllocator, pDevice);
        if (res == VK_ERROR_EXTENSION_NOT_PRESENT)
            throw std::runtime_error(
                "Required Vulkan device extensions are not present."
                "Your GPU driver is not supported.");
        return res;
    }

    VkResult myvkCreateDevicePost(
            VkPhysicalDevice physicalDevice,
            VkDeviceCreateInfo* pCreateInfo,
            const VkAllocationCallbacks*,
            VkDevice* pDevice) {
#ifdef __ANDROID__
        const bool androidAhbSupported = supportsDeviceExtension(physicalDevice,
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
#else
        const bool androidAhbSupported = true;
#endif
        auto getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            Layer::ovkGetInstanceProcAddr(layerInstance, "vkGetPhysicalDeviceProperties2"));
        if (getProperties2 == nullptr) {
            getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                Layer::ovkGetInstanceProcAddr(layerInstance, "vkGetPhysicalDeviceProperties2KHR"));
        }
        const auto identity = Utils::getDeviceIdentity(physicalDevice, getProperties2);
        if (!identity.has_value()) {
            Utils::logLimitN("deviceIdentity", 1,
                "Physical-device ID properties unavailable; LSFG will fail open for this device.");
        }
        deviceToInfo.emplace(*pDevice, DeviceInfo {
            .device = *pDevice,
            .physicalDevice = physicalDevice,
            .identity = identity.value_or(LSFG::DeviceIdentity{}),
            .identityValid = identity.has_value(),
            .queue = Utils::findQueue(*pDevice, physicalDevice, pCreateInfo, VK_QUEUE_GRAPHICS_BIT),
            .androidAhbSupported = androidAhbSupported
        });
#ifdef __ANDROID__
        std::cerr << "lsfg-vk: capability_matrix stage=device"
                  << " identity_valid=" << (identity.has_value() ? 1 : 0)
                  << " ahb_external_memory=" << (androidAhbSupported ? 1 : 0)
                  << " sync_fallback=fence+legacy-barrier"
                  << " fail_open=1\n";
#endif
        return VK_SUCCESS;
    }

    VkPresentModeKHR choosePresentMode(
            VkPhysicalDevice physicalDevice,
            VkSurfaceKHR surface,
            VkPresentModeKHR gamePresentMode,
            VkPresentModeKHR configuredPresentMode);

    std::unordered_map<VkDevice, DeviceInfo> dummyDeviceToInfoDeclarationGuard;

    void myvkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) noexcept {
        deviceToInfo.erase(device);
        Layer::ovkDestroyDevice(device, pAllocator);
    }

    std::unordered_map<VkSwapchainKHR, LsContext> swapchains;
    std::unordered_map<VkSwapchainKHR, VkDevice> swapchainToDeviceTable;
    std::unordered_map<VkSwapchainKHR, VkPresentModeKHR> swapchainToPresent;
    std::unordered_map<VkSwapchainKHR, VkPresentModeKHR> swapchainToConfiguredPresent;

    struct SwapchainWsiProvenance {
        uint32_t gameMinImageCount{};
        uint32_t effectiveMinImageCount{};
        VkPresentModeKHR gamePresentMode{VK_PRESENT_MODE_FIFO_KHR};
        VkPresentModeKHR effectivePresentMode{VK_PRESENT_MODE_FIFO_KHR};
        bool fgModified{};
    };
    std::unordered_map<VkSwapchainKHR, SwapchainWsiProvenance> swapchainWsiProvenance;

#ifdef __ANDROID__
    struct RuntimeOutputStats {
        using Clock = std::chrono::steady_clock;
        Clock::time_point windowStart{Clock::now()};
        std::vector<VkSemaphore> presentWaitSemaphores;
        SourceFramePacer sourceFramePacer;
        uint64_t windowSourceFrames{0};
        uint64_t windowGeneratedFrames{0};
        uint64_t totalSourceFrames{0};
        uint64_t totalGeneratedFrames{0};
        uint64_t presentFailures{0};
    };

    struct RuntimeStatsSnapshot {
        std::string configFile;
        bool active{};
        bool generationReady{};
        double outputFps{};
        double sourceFps{};
        double generatedFps{};
        uint64_t totalSourceFrames{};
        uint64_t totalGeneratedFrames{};
        uint64_t presentFailures{};
        int multiplier{};
        bool adaptive{};
        uint32_t targetFps{};
        uint32_t sourceLimitFps{};
        bool performance{};
    };

    struct RuntimeIoState {
        std::mutex mutex;
        std::condition_variable cv;
        std::string watchedConfigFile;
        std::filesystem::file_time_type observedTimestamp{};
        bool observedExists{};
        bool watcherArmed{};
        Config::Configuration appliedConf{};
        std::optional<Config::Configuration> pendingConfig;
        bool pendingRequiresRecreate{};
        std::atomic<bool> configPending{false};
        std::optional<RuntimeStatsSnapshot> pendingStats;
    };

    std::unordered_map<VkSwapchainKHR, RuntimeOutputStats> runtimeOutputStats;

    RuntimeIoState& runtimeIoState() {
        // Process-lifetime allocation deliberately avoids detached-thread/static
        // teardown ordering hazards during loader shutdown.
        static RuntimeIoState* state = new RuntimeIoState();
        return *state;
    }

    void writeRuntimeStatsSnapshotNow(const RuntimeStatsSnapshot& snapshot) {
        if (snapshot.configFile.empty())
            return;

        const std::filesystem::path statsPath =
            std::filesystem::path(snapshot.configFile).parent_path() / "stats.txt";
        const std::filesystem::path tempPath = statsPath.string() + ".tmp";
        try {
            std::ofstream out(tempPath, std::ios::trunc);
            if (!out)
                throw std::runtime_error("unable to open temporary stats file");
            out << std::fixed << std::setprecision(3)
                << "active=" << (snapshot.active ? 1 : 0) << '\n'
                << "generation_ready=" << (snapshot.generationReady ? 1 : 0) << '\n'
                << "fps=" << snapshot.outputFps << '\n'
                << "source_fps=" << snapshot.sourceFps << '\n'
                << "generated_fps=" << snapshot.generatedFps << '\n'
                << "source_frames_total=" << snapshot.totalSourceFrames << '\n'
                << "generated_frames_total=" << snapshot.totalGeneratedFrames << '\n'
                << "present_failures=" << snapshot.presentFailures << '\n'
                << "multiplier=" << snapshot.multiplier << '\n'
                << "adaptive=" << (snapshot.adaptive ? 1 : 0) << '\n'
                << "target_fps=" << snapshot.targetFps << '\n'
                << "source_limit_fps=" << snapshot.sourceLimitFps << '\n'
                << "performance=" << (snapshot.performance ? 1 : 0) << '\n';
            out.close();
            if (!out)
                throw std::runtime_error("failed to flush temporary stats file");

            std::error_code ec;
            std::filesystem::rename(tempPath, statsPath, ec);
            if (ec) {
                std::filesystem::remove(statsPath, ec);
                ec.clear();
                std::filesystem::rename(tempPath, statsPath, ec);
            }
            if (ec)
                throw std::runtime_error("failed to publish stats.txt: " + ec.message());
            Utils::resetLimitN("statsWrite");
        } catch (const std::exception& e) {
            std::error_code ignored;
            std::filesystem::remove(tempPath, ignored);
            Utils::logLimitN("statsWrite", 5,
                "Failed to publish Android runtime stats: " + std::string(e.what()));
        }
    }

    void runtimeIoWorker() {
        auto& state = runtimeIoState();
        for (;;) {
            std::string configFile;
            bool watcherArmed = false;
            bool observedExists = false;
            std::filesystem::file_time_type observedTimestamp{};
            Config::Configuration appliedConf{};
            std::optional<RuntimeStatsSnapshot> statsSnapshot;
            {
                std::unique_lock lock(state.mutex);
                state.cv.wait_for(lock, std::chrono::milliseconds(250), [&state] {
                    return state.pendingStats.has_value();
                });
                statsSnapshot = std::move(state.pendingStats);
                state.pendingStats.reset();
                configFile = state.watchedConfigFile;
                watcherArmed = state.watcherArmed;
                observedExists = state.observedExists;
                observedTimestamp = state.observedTimestamp;
                appliedConf = state.appliedConf;
            }

            if (statsSnapshot.has_value())
                writeRuntimeStatsSnapshotNow(*statsSnapshot);

            if (!watcherArmed || configFile.empty())
                continue;

            std::error_code ec;
            const bool exists = std::filesystem::exists(configFile, ec) && !ec;
            std::filesystem::file_time_type timestamp{};
            if (exists) {
                ec.clear();
                timestamp = std::filesystem::last_write_time(configFile, ec);
                if (ec)
                    continue;
            }
            const bool changed = exists != observedExists
                || (exists && observedExists && timestamp != observedTimestamp);
            if (!changed)
                continue;

            std::optional<Config::Configuration> nextConf;
            if (exists) {
                try {
                    Config::updateConfig(configFile);
                    nextConf = Config::getConfig(Utils::getProcessName());
                    Utils::resetLimitN("configReload");
                } catch (const std::exception& e) {
                    Utils::logLimitN("configReload", 5,
                        "Failed to hot-reload configuration; preserving the active runtime:\n- "
                        + std::string(e.what()));
                }
            } else {
                auto disabledConf = appliedConf;
                disabledConf.enable = false;
                disabledConf.config_file = configFile;
                disabledConf.timestamp = {};
                nextConf = std::move(disabledConf);
            }

            {
                std::lock_guard lock(state.mutex);
                if (state.watchedConfigFile != configFile)
                    continue;
                state.observedExists = exists;
                state.observedTimestamp = timestamp;
                if (nextConf.has_value()) {
                    state.pendingRequiresRecreate = requiresSwapchainRecreation(
                        state.appliedConf, *nextConf);
                    state.pendingConfig = std::move(nextConf);
                    state.configPending.store(true, std::memory_order_release);
                }
            }
        }
    }

    void ensureRuntimeIoThread() {
        static std::once_flag startFlag;
        std::call_once(startFlag, [] {
            std::thread(runtimeIoWorker).detach();
        });
    }

    void armConfigWatcher(const Config::Configuration& conf) {
        ensureRuntimeIoThread();
        auto& state = runtimeIoState();
        {
            std::lock_guard lock(state.mutex);
            state.watchedConfigFile = conf.config_file;
            state.observedExists = !conf.config_file.empty();
            state.observedTimestamp = conf.timestamp;
            state.watcherArmed = !conf.config_file.empty();
            state.appliedConf = conf;
            state.pendingConfig.reset();
            state.pendingRequiresRecreate = false;
            state.configPending.store(false, std::memory_order_release);
        }
        state.cv.notify_one();
    }

    enum class PendingConfigAction {
        None,
        Applied,
        Recreate
    };

    PendingConfigAction applyPendingRuntimeConfig() {
        auto& state = runtimeIoState();
        if (!state.configPending.load(std::memory_order_acquire))
            return PendingConfigAction::None;

        std::lock_guard lock(state.mutex);
        if (!state.pendingConfig.has_value()) {
            state.configPending.store(false, std::memory_order_release);
            return PendingConfigAction::None;
        }
        if (state.pendingRequiresRecreate)
            return PendingConfigAction::Recreate;

        Config::activeConf = *state.pendingConfig;
        state.appliedConf = Config::activeConf;
        state.pendingConfig.reset();
        state.pendingRequiresRecreate = false;
        state.configPending.store(false, std::memory_order_release);
        std::cerr << "lsfg-vk: runtime stage=config-applied-no-wsi-recreate"
                  << " adaptive=" << (Config::activeConf.adaptiveFramegen ? 1 : 0)
                  << " targetFps=" << Config::activeConf.fpsLimit
                  << " sourceFpsLimit=" << Config::activeConf.sourceFpsLimit << "\n";
        return PendingConfigAction::Applied;
    }

    bool applyPendingConfigForSwapchainCreation() {
        auto& state = runtimeIoState();
        if (!state.configPending.load(std::memory_order_acquire))
            return false;

        std::lock_guard lock(state.mutex);
        if (!state.pendingConfig.has_value()) {
            state.configPending.store(false, std::memory_order_release);
            return false;
        }
        const auto previousConf = Config::activeConf;
        Config::activeConf = *state.pendingConfig;
        const bool requiredRecreate = requiresSwapchainRecreation(
            previousConf, Config::activeConf);
        state.appliedConf = Config::activeConf;
        state.pendingConfig.reset();
        state.pendingRequiresRecreate = false;
        state.configPending.store(false, std::memory_order_release);
        std::cerr << "lsfg-vk: init stage=config-applied-for-swapchain"
                  << " multiplier=" << Config::activeConf.multiplier
                  << " adaptive=" << (Config::activeConf.adaptiveFramegen ? 1 : 0)
                  << " targetFps=" << Config::activeConf.fpsLimit
                  << " sourceFpsLimit=" << Config::activeConf.sourceFpsLimit
                  << " presentMode=" << Config::activeConf.e_present
                  << " enabled=" << (Config::activeConf.enable ? 1 : 0)
                  << " wsiChanged=" << (requiredRecreate ? 1 : 0) << "\n";
        return true;
    }

    void queueRuntimeStats(RuntimeStatsSnapshot snapshot) {
        ensureRuntimeIoThread();
        auto& state = runtimeIoState();
        {
            std::lock_guard lock(state.mutex);
            // Stats are telemetry, so keeping only the newest pending sample
            // prevents file I/O from ever creating backpressure on presentation.
            state.pendingStats = std::move(snapshot);
        }
        state.cv.notify_one();
    }

    void publishRuntimeState(const std::string& configFile,
            bool active, bool generationReady, int multiplier,
            bool performance, bool adaptive, uint32_t targetFps) {
        queueRuntimeStats(RuntimeStatsSnapshot {
            .configFile = configFile,
            .active = active,
            .generationReady = generationReady,
            .multiplier = multiplier,
            .adaptive = adaptive,
            .targetFps = targetFps,
            .sourceLimitFps = Config::activeConf.sourceFpsLimit,
            .performance = performance,
        });
    }

    void recordSuccessfulOutputCycle(VkSwapchainKHR swapchain,
            const std::string& configFile, uint64_t generated,
            int multiplier, bool performance, bool adaptive, uint32_t targetFps,
            uint32_t sourceLimitFps) {
        auto& stats = runtimeOutputStats[swapchain];
        stats.windowSourceFrames++;
        stats.totalSourceFrames++;
        stats.windowGeneratedFrames += generated;
        stats.totalGeneratedFrames += generated;

        const auto now = RuntimeOutputStats::Clock::now();
        const double elapsedSeconds = std::chrono::duration<double>(
            now - stats.windowStart).count();
        if (elapsedSeconds < 1.0)
            return;

        const double sourceFps = static_cast<double>(stats.windowSourceFrames) / elapsedSeconds;
        const double generatedFps = static_cast<double>(stats.windowGeneratedFrames) / elapsedSeconds;
        const double outputFps = sourceFps + generatedFps;
        queueRuntimeStats(RuntimeStatsSnapshot {
            .configFile = configFile,
            .active = true,
            .generationReady = true,
            .outputFps = outputFps,
            .sourceFps = sourceFps,
            .generatedFps = generatedFps,
            .totalSourceFrames = stats.totalSourceFrames,
            .totalGeneratedFrames = stats.totalGeneratedFrames,
            .presentFailures = stats.presentFailures,
            .multiplier = multiplier,
            .adaptive = adaptive,
            .targetFps = targetFps,
            .sourceLimitFps = sourceLimitFps,
            .performance = performance,
        });

        stats.windowStart = now;
        stats.windowSourceFrames = 0;
        stats.windowGeneratedFrames = 0;
    }

    void recordOutputFailure(VkSwapchainKHR swapchain) {
        runtimeOutputStats[swapchain].presentFailures++;
    }
#endif

    void eraseSwapchainState(VkSwapchainKHR swapchain) {
        if (swapchain == VK_NULL_HANDLE)
            return;
        swapchains.erase(swapchain);
        swapchainToDeviceTable.erase(swapchain);
        swapchainToPresent.erase(swapchain);
        swapchainToConfiguredPresent.erase(swapchain);
        swapchainWsiProvenance.erase(swapchain);
#ifdef __ANDROID__
        runtimeOutputStats.erase(swapchain);
#endif
    }

    VkPresentModeKHR choosePresentMode(
            VkPhysicalDevice physicalDevice,
            VkSurfaceKHR surface,
            VkPresentModeKHR gamePresentMode,
            VkPresentModeKHR configuredPresentMode) {
        if (layerInstance == VK_NULL_HANDLE)
            return gamePresentMode;

        auto getPresentModes = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
            Layer::ovkGetInstanceProcAddr(layerInstance,
                "vkGetPhysicalDeviceSurfacePresentModesKHR"));
        if (getPresentModes == nullptr) {
            Utils::logLimitN("presentModes", 5,
                "vkGetPhysicalDeviceSurfacePresentModesKHR unavailable; preserving game present mode");
            return gamePresentMode;
        }

        uint32_t count{};
        auto res = getPresentModes(physicalDevice, surface, &count, nullptr);
        if (res != VK_SUCCESS || count == 0) {
            Utils::logLimitN("presentModes", 5,
                "Could not enumerate surface present modes; preserving game present mode");
            return gamePresentMode;
        }

        std::vector<VkPresentModeKHR> modes(count);
        res = getPresentModes(physicalDevice, surface, &count, modes.data());
        if (res != VK_SUCCESS || count == 0) {
            Utils::logLimitN("presentModes", 5,
                "Could not read surface present modes; preserving game present mode");
            return gamePresentMode;
        }
        modes.resize(count);

        const auto supports = [&modes](VkPresentModeKHR mode) {
            return std::find(modes.begin(), modes.end(), mode) != modes.end();
        };
        if (supports(configuredPresentMode)) {
            Utils::resetLimitN("presentModes");
            return configuredPresentMode;
        }

        if (supports(gamePresentMode)) {
            Utils::logLimitN("presentModes", 5,
                "Configured present mode " + std::to_string(configuredPresentMode) +
                " is unsupported by this surface; preserving game mode " +
                std::to_string(gamePresentMode));
            return gamePresentMode;
        }

        if (supports(VK_PRESENT_MODE_FIFO_KHR)) {
            Utils::logLimitN("presentModes", 5,
                "Configured and game present modes are unavailable; falling back to FIFO");
            return VK_PRESENT_MODE_FIFO_KHR;
        }

        Utils::logLimitN("presentModes", 5,
            "Configured and game present modes are unavailable; using first enumerated surface mode");
        return modes.front();
    }

    bool supportsBidirectionalBlit(VkPhysicalDevice physicalDevice,
            VkFormat sharedFormat, VkFormat swapchainFormat) {
        auto getFormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
            Layer::ovkGetInstanceProcAddr(layerInstance, "vkGetPhysicalDeviceFormatProperties"));
        if (getFormatProperties == nullptr)
            return false;

        VkFormatProperties sharedProperties{};
        VkFormatProperties swapchainProperties{};
        getFormatProperties(physicalDevice, sharedFormat, &sharedProperties);
        getFormatProperties(physicalDevice, swapchainFormat, &swapchainProperties);
        constexpr VkFormatFeatureFlags required =
            VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        return (sharedProperties.optimalTilingFeatures & required) == required
            && (swapchainProperties.optimalTilingFeatures & required) == required;
    }

    VkResult myvkCreateSwapchainKHR(
            VkDevice device,
            const VkSwapchainCreateInfoKHR* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkSwapchainKHR* pSwapchain) noexcept {
#ifdef __ANDROID__
        applyPendingConfigForSwapchainCreation();
        armConfigWatcher(Config::activeConf);
#endif
        std::cerr << "lsfg-vk: init stage=swapchain-hook-enter requestedImages="
                  << pCreateInfo->minImageCount
                  << " extent=" << pCreateInfo->imageExtent.width << "x"
                  << pCreateInfo->imageExtent.height
                  << " presentMode=" << pCreateInfo->presentMode << "\n";

        auto it = deviceToInfo.find(device);
        if (it == deviceToInfo.end()) {
            Utils::logLimitN("swapMap", 5, "Device not found in map");
            return Layer::ovkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        }
        Utils::resetLimitN("swapMap");
        auto& deviceInfo = it->second;
        const auto activeConf = Config::activeConf;

        const auto createPassThrough = [&](const char* reason) -> VkResult {
            const auto res = Layer::ovkCreateSwapchainKHR(
                device, pCreateInfo, pAllocator, pSwapchain);
            if (res == VK_SUCCESS) {
                if (pCreateInfo->oldSwapchain)
                    eraseSwapchainState(pCreateInfo->oldSwapchain);
                swapchainToDeviceTable.emplace(*pSwapchain, device);
                swapchainWsiProvenance.emplace(*pSwapchain, SwapchainWsiProvenance {
                    .gameMinImageCount = pCreateInfo->minImageCount,
                    .effectiveMinImageCount = pCreateInfo->minImageCount,
                    .gamePresentMode = pCreateInfo->presentMode,
                    .effectivePresentMode = pCreateInfo->presentMode,
                    .fgModified = false,
                });
#ifdef __ANDROID__
                publishRuntimeState(activeConf.config_file, false, false,
                    static_cast<int>(activeConf.multiplier), activeConf.performance,
                    activeConf.adaptiveFramegen, activeConf.fpsLimit);
#endif
                std::cerr << "lsfg-vk: init stage=swapchain-pass-through reason="
                          << reason
                          << " enabled=" << (activeConf.enable ? 1 : 0)
                          << " multiplier=" << activeConf.multiplier
                          << " gameImages=" << pCreateInfo->minImageCount
                          << " effectiveImages=" << pCreateInfo->minImageCount
                          << " gamePresentMode=" << pCreateInfo->presentMode
                          << " effectivePresentMode=" << pCreateInfo->presentMode << "\n";
            }
            return res;
        };

        if (!activeConf.enable || activeConf.multiplier <= 1)
            return createPassThrough("disabled");

#ifdef __ANDROID__
        if (!deviceInfo.androidAhbSupported) {
            Utils::logLimitN("swapAhb", 5,
                "init stage=ahb-extension-unavailable; preserving original swapchain");
            return createPassThrough("ahb-unavailable");
        }
#endif

        VkSurfaceCapabilitiesKHR surfaceCapabilities{};
        auto surfaceRes = Layer::ovkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            deviceInfo.physicalDevice, pCreateInfo->surface, &surfaceCapabilities);
        if (surfaceRes != VK_SUCCESS) {
            Utils::logLimitN("swapCaps", 5,
                "init stage=swapchain-capabilities-unavailable; preserving original swapchain");
            return createPassThrough("capabilities-unavailable");
        }

        constexpr VkImageUsageFlags requiredTransferUsage =
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if ((surfaceCapabilities.supportedUsageFlags & requiredTransferUsage)
                != requiredTransferUsage) {
            std::cerr << "lsfg-vk: init stage=swapchain-unsupported-usage supportedUsage="
                      << surfaceCapabilities.supportedUsageFlags
                      << " requiredUsage=" << requiredTransferUsage
                      << "; preserving original swapchain\n";
            return createPassThrough("unsupported-usage");
        }

        VkSwapchainCreateInfoKHR createInfo = *pCreateInfo;
        const uint32_t requiredHeadroom = static_cast<uint32_t>(
            std::max<size_t>(1, activeConf.multiplier - 1));
        const uint32_t maxImageCount = surfaceCapabilities.maxImageCount;
        if (pCreateInfo->minImageCount > UINT32_MAX - requiredHeadroom) {
            std::cerr << "lsfg-vk: init stage=swapchain-insufficient-headroom minImageCount="
                      << pCreateInfo->minImageCount
                      << " maxImageCount=" << maxImageCount
                      << " requiredHeadroom=" << requiredHeadroom
                      << "; preserving original swapchain\n";
            return createPassThrough("headroom-overflow");
        }
        const uint32_t requiredImageCount = pCreateInfo->minImageCount + requiredHeadroom;
        std::cerr << "lsfg-vk: init stage=swapchain-capacity minImageCount="
                  << pCreateInfo->minImageCount
                  << " maxImageCount=" << maxImageCount
                  << " requiredHeadroom=" << requiredHeadroom
                  << " multiplier=" << activeConf.multiplier << "\n";
        if (maxImageCount != 0 && requiredImageCount > maxImageCount) {
            std::cerr << "lsfg-vk: init stage=swapchain-insufficient-headroom minImageCount="
                      << pCreateInfo->minImageCount
                      << " maxImageCount=" << maxImageCount
                      << " requiredHeadroom=" << requiredHeadroom
                      << "; preserving original swapchain\n";
            return createPassThrough("insufficient-headroom");
        }
        createInfo.minImageCount = requiredImageCount;
        Utils::resetLimitN("swapCount");

        const VkFormat sharedFormat = activeConf.hdr
            ? VK_FORMAT_R8G8B8A8_UNORM
            : VK_FORMAT_R16G16B16A16_SFLOAT;
        std::cerr << "lsfg-vk: init stage=swapchain-blit-check-begin sharedFormat="
                  << sharedFormat << " swapchainFormat=" << pCreateInfo->imageFormat << "\n";
        if (!supportsBidirectionalBlit(
                deviceInfo.physicalDevice, sharedFormat, pCreateInfo->imageFormat)) {
            std::cerr << "lsfg-vk: init stage=blit-format-unsupported sharedFormat="
                      << sharedFormat << " swapchainFormat=" << pCreateInfo->imageFormat
                      << "; preserving original swapchain\n";
            return createPassThrough("blit-unsupported");
        }
        std::cerr << "lsfg-vk: init stage=swapchain-blit-check-ready\n";

        createInfo.imageUsage |= requiredTransferUsage;

        const auto configuredPresentMode = activeConf.e_present;
        const bool recreatingExistingSwapchain = pCreateInfo->oldSwapchain != VK_NULL_HANDLE;
        createInfo.presentMode = recreatingExistingSwapchain
            ? pCreateInfo->presentMode
            : choosePresentMode(
                deviceInfo.physicalDevice, pCreateInfo->surface,
                pCreateInfo->presentMode, configuredPresentMode);
        if (recreatingExistingSwapchain) {
            std::cerr << "lsfg-vk: init stage=swapchain-hot-recreate-present-mode"
                         " preservingGameMode=" << pCreateInfo->presentMode
                      << " configuredMode=" << configuredPresentMode << "\n";
        }

        std::cerr << "lsfg-vk: capability_matrix stage=swapchain"
                  << " ahb_external_memory=" << (deviceInfo.androidAhbSupported ? 1 : 0)
                  << " bidirectional_blit=1"
                  << " game_present_mode=" << pCreateInfo->presentMode
                  << " configured_present_mode=" << configuredPresentMode
                  << " selected_present_mode=" << createInfo.presentMode
                  << " min_images_game=" << pCreateInfo->minImageCount
                  << " min_images_fg=" << createInfo.minImageCount
                  << " fail_open=1\n";

        std::cerr << "lsfg-vk: init stage=swapchain-downstream-create-begin images="
                  << createInfo.minImageCount
                  << " presentMode=" << createInfo.presentMode
                  << " oldSwapchain=" << (createInfo.oldSwapchain != VK_NULL_HANDLE ? 1 : 0)
                  << "\n";
        auto res = Layer::ovkCreateSwapchainKHR(device, &createInfo, pAllocator, pSwapchain);
        std::cerr << "lsfg-vk: init stage=swapchain-downstream-create-return result="
                  << res << "\n";
        if (res != VK_SUCCESS) {
            std::cerr << "lsfg-vk: init stage=swapchain-modified-create-failed result="
                      << res << "; retrying original parameters\n";
            return createPassThrough("modified-create-failed");
        }

        try {
            swapchainToPresent.emplace(*pSwapchain, createInfo.presentMode);
            swapchainToConfiguredPresent.emplace(*pSwapchain, configuredPresentMode);
            swapchainWsiProvenance.emplace(*pSwapchain, SwapchainWsiProvenance {
                .gameMinImageCount = pCreateInfo->minImageCount,
                .effectiveMinImageCount = createInfo.minImageCount,
                .gamePresentMode = pCreateInfo->presentMode,
                .effectivePresentMode = createInfo.presentMode,
                .fgModified = true,
            });

            uint32_t imageCount{};
            res = Layer::ovkGetSwapchainImagesKHR(device, *pSwapchain, &imageCount, nullptr);
            if (res != VK_SUCCESS || imageCount == 0)
                throw LSFG::vulkan_error(res, "Failed to get swapchain image count");

            std::vector<VkImage> swapchainImages(imageCount);
            res = Layer::ovkGetSwapchainImagesKHR(device, *pSwapchain,
                &imageCount, swapchainImages.data());
            if (res != VK_SUCCESS)
                throw LSFG::vulkan_error(res, "Failed to get swapchain images");

            // Retire the old LSFG bookkeeping only after the replacement Vulkan
            // swapchain is known-good. If downstream creation fails, the old
            // swapchain remains usable and its context remains intact.
            if (pCreateInfo->oldSwapchain)
                eraseSwapchainState(pCreateInfo->oldSwapchain);

            swapchainToDeviceTable.emplace(*pSwapchain, device);
            std::cerr << "lsfg-vk: init stage=ls-context-begin images=" << imageCount
                      << " selectedPresentMode=" << createInfo.presentMode << "\n";
            swapchains.emplace(*pSwapchain, LsContext(
                deviceInfo, *pSwapchain, pCreateInfo->imageExtent,
                swapchainImages
            ));
            std::cerr << "lsfg-vk: init stage=ls-context-ready images=" << imageCount << "\n";
#ifdef __ANDROID__
            publishRuntimeState(activeConf.config_file, true, true,
                static_cast<int>(activeConf.multiplier), activeConf.performance,
                activeConf.adaptiveFramegen, activeConf.fpsLimit);
#endif

            std::cerr << "lsfg-vk: Swapchain context " <<
                    (createInfo.oldSwapchain ? "recreated" : "created")
                << " (using " << imageCount << " images, present mode "
                << createInfo.presentMode << ").\n";

            Utils::resetLimitN("swapCtxCreate");
        } catch (const std::exception& e) {
            std::cerr << "lsfg-vk: init stage=ls-context-failed error=" << e.what() << "\n";
            Utils::logLimitN("swapCtxCreate", 5,
                "An error occurred while creating the swapchain wrapper:\n"
                "- " + std::string(e.what()));

            // The modified swapchain has already retired pCreateInfo->oldSwapchain.
            // Use the modified handle as oldSwapchain for a replacement created
            // with the application's untouched parameters, and only destroy it
            // after the fallback has been created successfully.
            const VkSwapchainKHR failedSwapchain = *pSwapchain;
            VkSwapchainCreateInfoKHR fallbackCreateInfo = *pCreateInfo;
            fallbackCreateInfo.oldSwapchain = failedSwapchain;
            VkSwapchainKHR fallbackSwapchain = VK_NULL_HANDLE;
            const auto fallbackRes = Layer::ovkCreateSwapchainKHR(
                device, &fallbackCreateInfo, pAllocator, &fallbackSwapchain);
            if (fallbackRes == VK_SUCCESS) {
                eraseSwapchainState(failedSwapchain);
                Layer::ovkDestroySwapchainKHR(device, failedSwapchain, pAllocator);
                *pSwapchain = fallbackSwapchain;
                swapchainToDeviceTable.emplace(*pSwapchain, device);
                swapchainWsiProvenance.emplace(*pSwapchain, SwapchainWsiProvenance {
                    .gameMinImageCount = pCreateInfo->minImageCount,
                    .effectiveMinImageCount = pCreateInfo->minImageCount,
                    .gamePresentMode = pCreateInfo->presentMode,
                    .effectivePresentMode = pCreateInfo->presentMode,
                    .fgModified = false,
                });
#ifdef __ANDROID__
                publishRuntimeState(activeConf.config_file, false, false,
                    static_cast<int>(activeConf.multiplier), activeConf.performance,
                    activeConf.adaptiveFramegen, activeConf.fpsLimit);
#endif
                std::cerr << "lsfg-vk: init stage=swapchain-fallback-pass-through"
                             " reason=ls-context-failed\n";
                return VK_SUCCESS;
            }
            eraseSwapchainState(failedSwapchain);
            Layer::ovkDestroySwapchainKHR(device, failedSwapchain, pAllocator);
            *pSwapchain = VK_NULL_HANDLE;
            std::cerr << "lsfg-vk: init stage=swapchain-fallback-failed result="
                      << fallbackRes << "\n";
            return fallbackRes;
        }
        return VK_SUCCESS;
    }

    VkResult myvkQueuePresentKHR(
            VkQueue queue,
            const VkPresentInfoKHR* pPresentInfo) noexcept {
        auto it = swapchainToDeviceTable.find(*pPresentInfo->pSwapchains);
        if (it == swapchainToDeviceTable.end()) {
            Utils::logLimitN("swapMap", 5,
                "Swapchain not found in map");
            return Layer::ovkQueuePresentKHR(queue, pPresentInfo);
        }

        auto it2 = deviceToInfo.find(it->second);
        if (it2 == deviceToInfo.end()) {
            Utils::logLimitN("swapMap", 5,
                "Device not found in map");
            return Layer::ovkQueuePresentKHR(queue, pPresentInfo);
        }
        auto& deviceInfo = it2->second;

#ifdef __ANDROID__
        const auto pendingAction = applyPendingRuntimeConfig();
        if (pendingAction == PendingConfigAction::Recreate) {
            // Transition frames are native. Returning OUT_OF_DATE after the
            // native present asks the application to rebuild WSI from its own
            // untouched create parameters; sustained Off then has no LSFG WSI.
            Layer::ovkQueuePresentKHR(queue, pPresentInfo);
            std::cerr << "lsfg-vk: runtime stage=wsi-restore-requested reason=config-transition\n";
            return VK_ERROR_OUT_OF_DATE_KHR;
        }
#endif

        const auto conf = Config::activeConf;
        // Canonical disabled path: no LsContext, adaptive plan, pacer, telemetry
        // file I/O or generated semaphore handling. The resident hook exists
        // only so a background config change can request a future recreation.
        if (!conf.enable || conf.multiplier <= 1)
            return Layer::ovkQueuePresentKHR(queue, pPresentInfo);

#ifdef __ANDROID__
        auto& runtimeStats = runtimeOutputStats[*pPresentInfo->pSwapchains];
#endif

        auto it3 = swapchains.find(*pPresentInfo->pSwapchains);
        if (it3 == swapchains.end()) {
            Utils::logLimitN("swapMap", 5,
                "Swapchain context not found in map");
            return Layer::ovkQueuePresentKHR(queue, pPresentInfo);
        }
        auto& swapchain = it3->second;

        auto it4 = swapchainToPresent.find(*pPresentInfo->pSwapchains);
        auto it5 = swapchainToConfiguredPresent.find(*pPresentInfo->pSwapchains);
        if (it4 == swapchainToPresent.end() || it5 == swapchainToConfiguredPresent.end()) {
            Utils::logLimitN("swapMap", 5,
                "Swapchain present mode not found in map");
            return Layer::ovkQueuePresentKHR(queue, pPresentInfo);
        }
        auto& present = it4->second;
        auto& configuredPresent = it5->second;

        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
        const VkSwapchainPresentModeInfoEXT* presentModeInfo =
            reinterpret_cast<const VkSwapchainPresentModeInfoEXT*>(pPresentInfo->pNext);
        while (presentModeInfo) {
            if (presentModeInfo->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT) {
                for (size_t i = 0; i < presentModeInfo->swapchainCount; i++)
                    const_cast<VkPresentModeKHR*>(presentModeInfo->pPresentModes)[i] =
                        present;
            }
            presentModeInfo =
                reinterpret_cast<const VkSwapchainPresentModeInfoEXT*>(presentModeInfo->pNext);
        }
        #pragma clang diagnostic pop

        const bool presentModeChangePending = configuredPresent != conf.e_present;
        if (presentModeChangePending)
            Utils::logLimitN("presentModeDeferred", 1,
                "Present-mode change awaits the config-triggered swapchain recreation");
        else
            Utils::resetLimitN("presentModeDeferred");

#ifdef __ANDROID__
        auto& sourceFramePacer = runtimeStats.sourceFramePacer;
        sourceFramePacer.configure(conf.sourceFpsLimit);
        const auto sourcePacingDelay = sourceFramePacer.delayUntilNext(
            SourceFramePacer::Clock::now());
        if (sourcePacingDelay > std::chrono::nanoseconds::zero())
            std::this_thread::sleep_for(sourcePacingDelay);
#endif

        try {
#ifdef __ANDROID__
            auto& semaphores = runtimeStats.presentWaitSemaphores;
            semaphores.resize(pPresentInfo->waitSemaphoreCount);
#else
            std::vector<VkSemaphore> semaphores(pPresentInfo->waitSemaphoreCount);
#endif
            if (!semaphores.empty())
                std::copy_n(pPresentInfo->pWaitSemaphores, semaphores.size(), semaphores.data());

            const auto res = swapchain.present(deviceInfo, pPresentInfo->pNext,
                queue, semaphores, *pPresentInfo->pImageIndices);

#ifdef __ANDROID__
            recordSuccessfulOutputCycle(*pPresentInfo->pSwapchains,
                conf.config_file, swapchain.lastGeneratedFrameCount(),
                conf.multiplier, conf.performance,
                conf.adaptiveFramegen, conf.fpsLimit, conf.sourceFpsLimit);
#endif
            Utils::resetLimitN("swapPresent");
            return res;
        } catch (const std::exception& e) {
#ifdef __ANDROID__
            recordOutputFailure(*pPresentInfo->pSwapchains);
            publishRuntimeState(conf.config_file, false, false,
                static_cast<int>(conf.multiplier), conf.performance,
                conf.adaptiveFramegen, conf.fpsLimit);
#endif
            Utils::logLimitN("swapPresent", 5,
                "An error occurred while presenting the swapchain; degrading to native presentation:\n"
                "- " + std::string(e.what()));
            swapchains.erase(*pPresentInfo->pSwapchains);
            std::cerr << "lsfg-vk: runtime stage=context-degraded-bypass reason=present-error\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    void myvkDestroySwapchainKHR(
            VkDevice device,
            VkSwapchainKHR swapchain,
            const VkAllocationCallbacks* pAllocator) noexcept {
        eraseSwapchainState(swapchain);
        Layer::ovkDestroySwapchainKHR(device, swapchain, pAllocator);
    }
}

std::unordered_map<std::string, PFN_vkVoidFunction> Hooks::hooks = {
    {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(myvkCreateInstance)},
    {"vkCreateDevicePre", reinterpret_cast<PFN_vkVoidFunction>(myvkCreateDevicePre)},
    {"vkCreateDevicePost", reinterpret_cast<PFN_vkVoidFunction>(myvkCreateDevicePost)},
    {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(myvkDestroyDevice)},
    {"vkCreateSwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(myvkCreateSwapchainKHR)},
    {"vkQueuePresentKHR", reinterpret_cast<PFN_vkVoidFunction>(myvkQueuePresentKHR)},
    {"vkDestroySwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(myvkDestroySwapchainKHR)}
};
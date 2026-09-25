#include "hooks.hpp"
#include "common/exception.hpp"
#include "config/config.hpp"
#include "utils/utils.hpp"
#include "context.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>

#include <unordered_map>
#include <filesystem>
#include <stdexcept>
#include <algorithm>
#include <atomic>
#include <exception>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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

    constexpr size_t kAndroidResidentMaxMultiplier = 4;
    std::atomic<uint64_t> nextSwapchainGeneration{1};

    size_t residentCapacityMultiplier(const Config::Configuration& conf) {
#ifdef __ANDROID__
        if (conf.targeted)
            return std::max(conf.multiplier, kAndroidResidentMaxMultiplier);
#endif
        return conf.multiplier;
    }

    bool adaptivePresentationPacing(const Config::Configuration& conf) {
        // Adaptive FG historically ran smoothly with the same WSI present-mode
        // selection as Fixed FG. Do not change the swapchain contract merely
        // because generation density is adaptive.
        (void)conf;
        return false;
    }

    bool requiresSwapchainRecreation(
            const Config::Configuration& previous,
            const Config::Configuration& next) {
#ifdef __ANDROID__
        const bool residentTarget = previous.targeted && next.targeted;
        if (residentTarget) {
            // Off is layer-resident, not framegen-context-resident. Crossing
            // the generation boundary must rebuild the real swapchain so an
            // inactive FIFO path cannot inherit LSFG image-count/usage state.
            const bool generationActivityChanged =
                (previous.multiplier > 1) != (next.multiplier > 1);
            const bool adaptiveFlowModeChanged =
                previous.adaptiveFlowScale != next.adaptiveFlowScale;
            const bool adaptiveFlowPresetChanged =
                previous.adaptiveFlowScale && next.adaptiveFlowScale
                && previous.adaptiveFlowPreset != next.adaptiveFlowPreset;
            const bool fixedFlowScaleChanged =
                !previous.adaptiveFlowScale && !next.adaptiveFlowScale
                && previous.flowScale != next.flowScale;
            // A resident context is allocated for at least four
            // generated outputs. A larger hot-reloaded multiplier needs a new
            // swapchain/context before present can index those outputs.
            return generationActivityChanged
                || next.multiplier > residentCapacityMultiplier(previous)
                || previous.dll != next.dll
                || adaptiveFlowModeChanged
                || adaptiveFlowPresetChanged
                || fixedFlowScaleChanged
                || previous.performance != next.performance
                || previous.hdr != next.hdr
                || previous.e_present != next.e_present;
        }
#endif
        return previous.enable != next.enable
            || previous.dll != next.dll
            || previous.multiplier != next.multiplier
            || previous.flowScale != next.flowScale
            || previous.performance != next.performance
            || previous.hdr != next.hdr
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

#ifdef __ANDROID__
    struct ExternalSemaphoreProbe {
        bool extensionPresent{false};
        bool queryAvailable{false};
        VkExternalSemaphoreFeatureFlags features{};
        VkExternalSemaphoreHandleTypeFlags compatibleHandleTypes{};
        VkExternalSemaphoreHandleTypeFlags exportFromImportedHandleTypes{};
        bool supported{false};
    };

    const char* externalSemaphoreHandleName(
            VkExternalSemaphoreHandleTypeFlagBits handleType) {
        switch (handleType) {
            case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT:
                return "opaque-fd";
            case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT:
                return "sync-fd";
            default:
                return "other";
        }
    }

    ExternalSemaphoreProbe probeFdSemaphore(VkPhysicalDevice physicalDevice,
            VkExternalSemaphoreHandleTypeFlagBits handleType) {
        ExternalSemaphoreProbe probe{};
        probe.extensionPresent = supportsDeviceExtension(
            physicalDevice, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
        if (!probe.extensionPresent)
            return probe;

        auto getExternalSemaphoreProperties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(
                Layer::ovkGetInstanceProcAddr(
                    layerInstance, "vkGetPhysicalDeviceExternalSemaphoreProperties"));
        if (getExternalSemaphoreProperties == nullptr) {
            getExternalSemaphoreProperties =
                reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(
                    Layer::ovkGetInstanceProcAddr(
                        layerInstance, "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR"));
        }
        probe.queryAvailable = getExternalSemaphoreProperties != nullptr;
        if (!probe.queryAvailable)
            return probe;

        const VkPhysicalDeviceExternalSemaphoreInfo info{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
            .handleType = handleType,
        };
        VkExternalSemaphoreProperties properties{
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
        };
        getExternalSemaphoreProperties(physicalDevice, &info, &properties);
        probe.features = properties.externalSemaphoreFeatures;
        probe.compatibleHandleTypes = properties.compatibleHandleTypes;
        probe.exportFromImportedHandleTypes = properties.exportFromImportedHandleTypes;

        constexpr VkExternalSemaphoreFeatureFlags required =
            VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT
            | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
        probe.supported =
            (probe.features & required) == required
            && (probe.compatibleHandleTypes & handleType) != 0;
        return probe;
    }

    void logGameExternalSemaphoreProbe(
            VkExternalSemaphoreHandleTypeFlagBits handleType,
            const ExternalSemaphoreProbe& probe) {
        std::cerr << "lsfg-vk: external-semaphore-probe scope=game"
                  << " handle=" << externalSemaphoreHandleName(handleType)
                  << " extension=" << (probe.extensionPresent ? 1 : 0)
                  << " query=" << (probe.queryAvailable ? 1 : 0)
                  << " features=0x" << std::hex
                  << static_cast<uint32_t>(probe.features)
                  << " compatible=0x"
                  << static_cast<uint32_t>(probe.compatibleHandleTypes)
                  << " export_from_imported=0x"
                  << static_cast<uint32_t>(probe.exportFromImportedHandleTypes)
                  << std::dec
                  << " supported=" << (probe.supported ? 1 : 0)
                  << '\n';
    }

#endif

    VkResult myvkCreateInstance(
            const VkInstanceCreateInfo* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkInstance* pInstance) {
#ifdef __ANDROID__
        // The Android game-side AHB path does not require any additional
        // instance extensions. Preserve the game's instance extension list
        // exactly; optional external-semaphore support is Vulkan 1.1 core and
        // is probed after instance creation.
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

    std::mutex hookStateMutex;
    std::unordered_map<VkDevice, std::shared_ptr<DeviceInfo>> deviceToInfo;

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

        std::vector<const char*> requestedExtensions{
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
        };
        const auto opaqueFdSemaphoreProbe = probeFdSemaphore(
            physicalDevice, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
        const auto syncFdSemaphoreProbe = probeFdSemaphore(
            physicalDevice, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
        logGameExternalSemaphoreProbe(
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, opaqueFdSemaphoreProbe);
        logGameExternalSemaphoreProbe(
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, syncFdSemaphoreProbe);
        const bool opaqueFdSemaphoreSupported = opaqueFdSemaphoreProbe.supported;
        const bool syncFdSemaphoreSupported = syncFdSemaphoreProbe.supported;
        if (opaqueFdSemaphoreSupported || syncFdSemaphoreSupported)
            requestedExtensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
        const bool displayTimingSupported = supportsDeviceExtension(
            physicalDevice, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
        if (displayTimingSupported)
            requestedExtensions.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);

        auto extensions = Utils::addExtensions(
            pCreateInfo->ppEnabledExtensionNames,
            pCreateInfo->enabledExtensionCount,
            requestedExtensions
        );
        std::cerr << "lsfg-vk: init stage=android-sync-capability opaqueFdSemaphore="
                  << (opaqueFdSemaphoreSupported ? 1 : 0)
                  << " syncFdSemaphore=" << (syncFdSemaphoreSupported ? 1 : 0)
                  << " fallback=host-fence\n";
        std::cerr << "lsfg-vk: init stage=android-display-timing capability="
                  << (displayTimingSupported ? 1 : 0) << "\n";
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
        const bool androidOpaqueFdSemaphoreSupported = probeFdSemaphore(
            physicalDevice, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT).supported;
        const bool androidSyncFdSemaphoreSupported = probeFdSemaphore(
            physicalDevice, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT).supported;
        const bool androidDisplayTimingSupported = supportsDeviceExtension(
            physicalDevice, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
#else
        const bool androidAhbSupported = true;
        const bool androidOpaqueFdSemaphoreSupported = false;
        const bool androidSyncFdSemaphoreSupported = false;
        const bool androidDisplayTimingSupported = false;
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
        try {
            auto deviceInfo = std::make_shared<DeviceInfo>(DeviceInfo {
                .device = *pDevice,
                .physicalDevice = physicalDevice,
                .identity = identity.value_or(LSFG::DeviceIdentity{}),
                .identityValid = identity.has_value(),
                .queue = Utils::findQueue(*pDevice, physicalDevice, pCreateInfo, VK_QUEUE_GRAPHICS_BIT),
                .androidAhbSupported = androidAhbSupported,
                .androidOpaqueFdSemaphoreSupported = androidOpaqueFdSemaphoreSupported,
                .androidSyncFdSemaphoreSupported = androidSyncFdSemaphoreSupported,
                .androidDisplayTimingSupported = androidDisplayTimingSupported,
            });
            std::lock_guard lock(hookStateMutex);
            deviceToInfo.insert_or_assign(*pDevice, std::move(deviceInfo));
        } catch (const std::exception& e) {
            // The Vulkan device already exists. If bookkeeping allocation
            // fails, leave the application device native instead of allowing
            // an exception to escape the layer callback boundary.
            Utils::logLimitN("deviceMap", 3,
                "Could not retain device bookkeeping; continuing natively:\n- "
                + std::string(e.what()));
        }
        return VK_SUCCESS;
    }

    VkPresentModeKHR choosePresentMode(
            VkPhysicalDevice physicalDevice,
            VkSurfaceKHR surface,
            VkPresentModeKHR gamePresentMode,
            VkPresentModeKHR configuredPresentMode);

    void myvkDestroyDevice(VkDevice device,
            const VkAllocationCallbacks* pAllocator) noexcept;

#ifdef __ANDROID__
    struct RuntimeOutputStats {
        using Clock = std::chrono::steady_clock;
        Clock::time_point windowStart{Clock::now()};
        Clock::time_point nextConfigPoll{};
        std::vector<VkSemaphore> presentWaitSemaphores;
        uint64_t windowSourceFrames{0};
        uint64_t windowGeneratedFrames{0};
        uint64_t totalSourceFrames{0};
        uint64_t totalGeneratedFrames{0};
        uint64_t presentFailures{0};
    };

    std::mutex runtimeStatsFileMutex;
#endif

    struct SwapchainState {
        VkDevice device{VK_NULL_HANDLE};
        std::shared_ptr<DeviceInfo> deviceInfo;
        VkPresentModeKHR present{VK_PRESENT_MODE_FIFO_KHR};
        VkPresentModeKHR configuredPresent{VK_PRESENT_MODE_FIFO_KHR};
        std::shared_ptr<LsContext> context;
        // Serializes a present against swapchain retirement and protects the
        // per-swapchain runtime counters. The global map lock is never held
        // while frame generation or downstream Vulkan calls run.
        std::mutex presentMutex;
#ifdef __ANDROID__
        RuntimeOutputStats runtimeStats;
#endif
    };

    std::unordered_map<VkSwapchainKHR, std::shared_ptr<SwapchainState>> swapchains;

    std::shared_ptr<DeviceInfo> findDeviceInfo(VkDevice device) {
        std::lock_guard lock(hookStateMutex);
        const auto it = deviceToInfo.find(device);
        return it == deviceToInfo.end() ? nullptr : it->second;
    }

    std::shared_ptr<SwapchainState> findSwapchainState(VkSwapchainKHR swapchain) {
        std::lock_guard lock(hookStateMutex);
        const auto it = swapchains.find(swapchain);
        return it == swapchains.end() ? nullptr : it->second;
    }

    void publishSwapchainState(VkSwapchainKHR swapchain,
            std::shared_ptr<SwapchainState> state) {
        std::lock_guard lock(hookStateMutex);
        swapchains.insert_or_assign(swapchain, std::move(state));
    }

    std::shared_ptr<SwapchainState> detachSwapchainState(VkSwapchainKHR swapchain) {
        if (swapchain == VK_NULL_HANDLE)
            return nullptr;
        std::shared_ptr<SwapchainState> state;
        {
            std::lock_guard lock(hookStateMutex);
            const auto it = swapchains.find(swapchain);
            if (it == swapchains.end())
                return nullptr;
            state = std::move(it->second);
            swapchains.erase(it);
        }
        return state;
    }

    void retireSwapchainState(VkSwapchainKHR swapchain) {
        auto state = detachSwapchainState(swapchain);
        if (!state)
            return;
        std::lock_guard presentLock(state->presentMutex);
        state->context.reset();
    }

    void myvkDestroyDevice(VkDevice device,
            const VkAllocationCallbacks* pAllocator) noexcept {
        // Retire one state at a time so device destruction cannot fail with a
        // bad_alloc while building a temporary retirement vector. The map lock
        // is released before waiting for a present already in progress.
        while (true) {
            std::shared_ptr<SwapchainState> state;
            {
                std::lock_guard lock(hookStateMutex);
                const auto it = std::find_if(
                    swapchains.begin(), swapchains.end(),
                    [device](const auto& entry) {
                        return entry.second->device == device;
                    });
                if (it == swapchains.end()) {
                    deviceToInfo.erase(device);
                    break;
                }
                state = std::move(it->second);
                swapchains.erase(it);
            }
            std::lock_guard presentLock(state->presentMutex);
            state->context.reset();
        }
        Layer::ovkDestroyDevice(device, pAllocator);
    }

#ifdef __ANDROID__
    void publishRuntimeState(const std::string& configFile, const char* state,
            bool active, bool generationReady, bool resident, bool sourceOnly,
            bool generationInitialized, bool generatedPresented, bool degraded, int multiplier,
            bool performance, bool adaptive, uint32_t targetFps) {
        if (configFile.empty())
            return;
        std::lock_guard fileLock(runtimeStatsFileMutex);

        std::filesystem::path tempPath;
        try {
            const std::filesystem::path statsPath =
                std::filesystem::path(configFile).parent_path() / "stats.txt";
            tempPath = statsPath.string() + ".tmp";
            std::ofstream out(tempPath, std::ios::trunc);
            if (!out)
                throw std::runtime_error("unable to open temporary stats file");
            out << "state=" << state << '\n'
                << "active=" << (active ? 1 : 0) << '\n'
                << "generation_ready=" << (generationReady ? 1 : 0) << '\n'
                << "resident=" << (resident ? 1 : 0) << '\n'
                << "source_only=" << (sourceOnly ? 1 : 0) << '\n'
                << "generation_initialized=" << (generationInitialized ? 1 : 0) << '\n'
                << "generated_presented=" << (generatedPresented ? 1 : 0) << '\n'
                << "degraded=" << (degraded ? 1 : 0) << '\n'
                << "fps=0.000\n"
                << "source_fps=0.000\n"
                << "generated_fps=0.000\n"
                << "source_frames_total=0\n"
                << "generated_frames_total=0\n"
                << "present_failures=0\n"
                << "multiplier=" << multiplier << '\n'
                << "adaptive=" << (adaptive ? 1 : 0) << '\n'
                << "target_fps=" << targetFps << '\n'
                << "performance=" << (performance ? 1 : 0) << '\n';
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
                "Failed to publish Android runtime state: " + std::string(e.what()));
        }
    }

    void writeRuntimeStatsFile(const std::string& configFile, const char* state,
            bool active, bool generationReady, bool resident, bool sourceOnly,
            bool generationInitialized, bool generatedPresented, bool degraded,
            double outputFps, double sourceFps, double generatedFps,
            const RuntimeOutputStats& stats, int multiplier, bool performance,
            bool adaptive, uint32_t targetFps,
            const AdaptiveFlowRuntimeSnapshot& adaptiveFlow) {
        if (configFile.empty())
            return;
        std::lock_guard fileLock(runtimeStatsFileMutex);

        std::filesystem::path tempPath;
        try {
            const std::filesystem::path statsPath =
                std::filesystem::path(configFile).parent_path() / "stats.txt";
            tempPath = statsPath.string() + ".tmp";
            std::ofstream out(tempPath, std::ios::trunc);
            if (!out)
                throw std::runtime_error("unable to open temporary stats file");
            out << std::fixed << std::setprecision(3)
                << "state=" << state << '\n'
                << "active=" << (active ? 1 : 0) << '\n'
                << "generation_ready=" << (generationReady ? 1 : 0) << '\n'
                << "resident=" << (resident ? 1 : 0) << '\n'
                << "source_only=" << (sourceOnly ? 1 : 0) << '\n'
                << "generation_initialized=" << (generationInitialized ? 1 : 0) << '\n'
                << "generated_presented=" << (generatedPresented ? 1 : 0) << '\n'
                << "degraded=" << (degraded ? 1 : 0) << '\n'
                << "fps=" << outputFps << '\n'
                << "source_fps=" << sourceFps << '\n'
                << "generated_fps=" << generatedFps << '\n'
                << "source_frames_total=" << stats.totalSourceFrames << '\n'
                << "generated_frames_total=" << stats.totalGeneratedFrames << '\n'
                << "present_failures=" << stats.presentFailures << '\n'
                << "multiplier=" << multiplier << '\n'
                << "adaptive=" << (adaptive ? 1 : 0) << '\n'
                << "target_fps=" << targetFps << '\n'
                << "performance=" << (performance ? 1 : 0) << '\n'
                << "adaptive_flow_enabled=" << (adaptiveFlow.enabled ? 1 : 0) << '\n'
                << "adaptive_flow_preset=" << adaptiveFlow.preset << '\n'
                << "adaptive_flow_target=" << adaptiveFlow.targetScale << '\n'
                << "adaptive_flow_minimum=" << adaptiveFlow.minimumScale << '\n'
                << "adaptive_flow_requested=" << adaptiveFlow.requestedScale << '\n'
                << "adaptive_flow_active=" << adaptiveFlow.activeScale << '\n'
                << "adaptive_flow_transition=" << (adaptiveFlow.transitionPending ? 1 : 0) << '\n'
                << "adaptive_flow_warmup_remaining=" << adaptiveFlow.warmupRemaining << '\n'
                << "adaptive_flow_timing_valid=" << (adaptiveFlow.timingValid ? 1 : 0) << '\n'
                << "adaptive_flow_mipmaps_ms=" << adaptiveFlow.mipmapsMs << '\n'
                << "adaptive_flow_work_ms=" << adaptiveFlow.flowMs << '\n'
                << "adaptive_flow_lsfg_ms=" << adaptiveFlow.totalLsfgMs << '\n'
                << "adaptive_flow_budget_ms=" << adaptiveFlow.budgetMs << '\n'
                << "adaptive_flow_generation_count=" << adaptiveFlow.generationCount << '\n'
                << "adaptive_flow_global_pressure_valid="
                << (adaptiveFlow.globalPressureValid ? 1 : 0) << '\n'
                << "adaptive_flow_global_gpu_percent="
                << adaptiveFlow.globalGpuUsagePercent << '\n'
                << "adaptive_flow_global_output_fps="
                << adaptiveFlow.globalOutputFps << '\n'
                << "adaptive_flow_lsfg_output_valid="
                << (adaptiveFlow.lsfgOutputValid ? 1 : 0) << '\n'
                << "adaptive_flow_lsfg_output_fps="
                << adaptiveFlow.lsfgOutputFps << '\n'
                << "adaptive_flow_global_p95_ms="
                << adaptiveFlow.globalFrameTimeP95Ms << '\n'
                << "adaptive_flow_global_slow_ratio="
                << adaptiveFlow.globalSlowFrameRatio << '\n'
                << "adaptive_flow_global_pressure="
                << (adaptiveFlow.globalPressure ? 1 : 0) << '\n'
                << "adaptive_flow_compute_pressure="
                << (adaptiveFlow.computePressure ? 1 : 0) << '\n'
                << "adaptive_flow_wsi_pressure="
                << (adaptiveFlow.wsiPressure ? 1 : 0) << '\n'
                << "adaptive_flow_wsi_loss_rate="
                << adaptiveFlow.wsiLossRate << '\n'
                << "adaptive_flow_presentation_cap="
                << adaptiveFlow.presentationGenerationCap << '\n'
                << "adaptive_flow_presentation_duty="
                << adaptiveFlow.presentationDuty << '\n'
                << "adaptive_flow_presentation_rejection_evidence="
                << adaptiveFlow.presentationRejectionEvidence << '\n'
                << "adaptive_flow_presentation_recovery_evidence="
                << adaptiveFlow.presentationRecoveryEvidence << '\n'
                << "adaptive_flow_presentation_attempted_generated="
                << adaptiveFlow.presentationAttemptedGeneratedFrames << '\n'
                << "adaptive_flow_presentation_accepted_generated="
                << adaptiveFlow.presentationAcceptedGeneratedFrames << '\n'
                << "adaptive_flow_presentation_delivered_efficiency="
                << adaptiveFlow.presentationDeliveredEfficiency << '\n'
                << "adaptive_flow_presentation_last_change_reason="
                << adaptiveFlow.presentationLastChangeReason << '\n'
                << "adaptive_flow_presentation_last_change_output_deficit="
                << (adaptiveFlow.presentationLastChangeOutputDeficit ? 1 : 0) << '\n'
                << "adaptive_flow_presentation_provisional_lower="
                << (adaptiveFlow.presentationProvisionalLowerActive ? 1 : 0) << '\n'
                << "adaptive_flow_presentation_upward_probe="
                << (adaptiveFlow.presentationUpwardProbePending ? 1 : 0) << '\n'
                << "adaptive_flow_output_deficit="
                << (adaptiveFlow.outputDeficit ? 1 : 0) << '\n'
                << "adaptive_flow_synthetic_drop_pressure="
                << (adaptiveFlow.syntheticDropPressure ? 1 : 0) << '\n'
                << "adaptive_flow_reason=" << adaptiveFlow.reason << '\n';
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

    void recordSuccessfulOutputCycle(SwapchainState& state,
            const LsContext& context, const std::string& configFile, uint64_t generated,
            int multiplier, bool performance, bool adaptive, uint32_t targetFps) {
        auto& stats = state.runtimeStats;
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
        const bool generationActive = multiplier > 1;
        const bool generatedPresented = generationActive && stats.totalGeneratedFrames > 0;
        const auto adaptiveFlow = context.adaptiveFlowRuntimeSnapshot();
        writeRuntimeStatsFile(configFile,
            generationActive ? "generating" : "source_only",
            generationActive, generationActive, true, !generationActive, true,
            generatedPresented, false,
            outputFps, sourceFps, generatedFps, stats, multiplier, performance,
            adaptive, targetFps, adaptiveFlow);

        stats.windowStart = now;
        stats.windowSourceFrames = 0;
        stats.windowGeneratedFrames = 0;
    }

    void recordOutputFailure(SwapchainState& state) {
        state.runtimeStats.presentFailures++;
    }
#endif

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

    bool configurationFileChanged(const Config::Configuration& conf) noexcept {
        if (conf.config_file.empty())
            return false;
        try {
            std::error_code ec;
            const std::filesystem::path path(conf.config_file);
            if (!std::filesystem::exists(path, ec))
                return !ec;
            if (ec)
                return false;
            const auto modified = std::filesystem::last_write_time(path, ec);
            return !ec && modified != conf.timestamp;
        } catch (...) {
            // This is called from a Vulkan noexcept hook. A transient file
            // system error must preserve the current runtime, not terminate
            // the process while checking whether a hot reload is needed.
            return false;
        }
    }

    VkResult myvkCreateSwapchainKHR(
            VkDevice device,
            const VkSwapchainCreateInfoKHR* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkSwapchainKHR* pSwapchain) noexcept {
        std::cerr << "lsfg-vk: init stage=swapchain-hook-enter requestedImages="
                  << pCreateInfo->minImageCount
                  << " extent=" << pCreateInfo->imageExtent.width << "x"
                  << pCreateInfo->imageExtent.height
                  << " presentMode=" << pCreateInfo->presentMode << "\n";

        const auto deviceInfo = findDeviceInfo(device);
        if (!deviceInfo) {
            Utils::logLimitN("swapMap", 5, "Device not found in map");
            return Layer::ovkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        }
        Utils::resetLimitN("swapMap");
        const auto activeConf = Config::snapshot();

        const auto createPassThrough = [&](const char* reason) -> VkResult {
            const auto res = Layer::ovkCreateSwapchainKHR(
                device, pCreateInfo, pAllocator, pSwapchain);
            if (res == VK_SUCCESS) {
                if (pCreateInfo->oldSwapchain)
                    retireSwapchainState(pCreateInfo->oldSwapchain);
                try {
                    auto state = std::make_shared<SwapchainState>();
                    state->device = device;
                    state->deviceInfo = deviceInfo;
                    publishSwapchainState(*pSwapchain, std::move(state));
                } catch (const std::exception& e) {
                    Utils::logLimitN("swapMap", 5,
                        "Could not retain pass-through swapchain state; continuing natively:\n- "
                        + std::string(e.what()));
                }
#ifdef __ANDROID__
                publishRuntimeState(activeConf.config_file, "pass_through",
                    false, false, false, false, false, false, false,
                    static_cast<int>(activeConf.multiplier), activeConf.performance,
                    activeConf.adaptiveFramegen, activeConf.fpsLimit);
#endif
                std::cerr << "lsfg-vk: init stage=swapchain-pass-through reason="
                          << reason
                          << " enabled=" << (activeConf.enable ? 1 : 0)
                          << " multiplier=" << activeConf.multiplier << "\n";
            }
            return res;
        };

        const auto createSourceOnly = [&](const char* reason) -> VkResult {
            VkSwapchainCreateInfoKHR sourceOnlyCreateInfo = *pCreateInfo;
            const auto configuredPresentMode = activeConf.e_present;
            sourceOnlyCreateInfo.presentMode = choosePresentMode(
                deviceInfo->physicalDevice,
                pCreateInfo->surface,
                pCreateInfo->presentMode,
                activeConf.e_present);

            // Source-only is a true WSI passthrough state: retain the selected
            // present mode, but do not inflate image count, add transfer usage,
            // or instantiate the private LSFG/AHB context.
            const auto res = Layer::ovkCreateSwapchainKHR(
                device, &sourceOnlyCreateInfo, pAllocator, pSwapchain);
            if (res != VK_SUCCESS)
                return res;

            if (pCreateInfo->oldSwapchain)
                retireSwapchainState(pCreateInfo->oldSwapchain);

            try {
                auto state = std::make_shared<SwapchainState>();
                state->device = device;
                state->deviceInfo = deviceInfo;
                state->present = sourceOnlyCreateInfo.presentMode;
                state->configuredPresent = configuredPresentMode;
                publishSwapchainState(*pSwapchain, std::move(state));
            } catch (const std::exception& e) {
                Utils::logLimitN("swapMap", 5,
                    "Could not retain source-only swapchain state; continuing natively:\n- "
                    + std::string(e.what()));
            }
#ifdef __ANDROID__
            publishRuntimeState(activeConf.config_file, "source_only",
                false, false, true, true, false, false, false,
                static_cast<int>(activeConf.multiplier), activeConf.performance,
                activeConf.adaptiveFramegen, activeConf.fpsLimit);
#endif
            std::cerr << "lsfg-vk: init stage=swapchain-source-only-pass-through"
                      << " reason=" << reason
                      << " requestedImages=" << pCreateInfo->minImageCount
                      << " requestedPresentMode=" << pCreateInfo->presentMode
                      << " configuredPresentMode=" << configuredPresentMode
                      << " chosenPresentMode=" << sourceOnlyCreateInfo.presentMode
                      << "\n";
            return VK_SUCCESS;
        };

        if (!activeConf.enable)
            return createPassThrough("disabled");

#ifdef __ANDROID__
        if (activeConf.targeted && activeConf.multiplier <= 1)
            return createSourceOnly("generation-off");
        if (activeConf.multiplier <= 1 && !activeConf.targeted)
            return createPassThrough("disabled");
#else
        if (activeConf.multiplier <= 1 && !activeConf.targeted)
            return createPassThrough("disabled");
#endif

#ifdef __ANDROID__
        if (!deviceInfo->androidAhbSupported) {
            Utils::logLimitN("swapAhb", 5,
                "init stage=ahb-extension-unavailable; preserving original swapchain");
            return createPassThrough("ahb-unavailable");
        }
#endif

        VkSurfaceCapabilitiesKHR surfaceCapabilities{};
        auto surfaceRes = Layer::ovkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            deviceInfo->physicalDevice, pCreateInfo->surface, &surfaceCapabilities);
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
        const size_t residentMultiplier = residentCapacityMultiplier(activeConf);
        const uint32_t requiredHeadroom = static_cast<uint32_t>(
            std::max<size_t>(1, residentMultiplier - 1));
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
                  << " multiplier=" << activeConf.multiplier
                  << " residentMultiplier=" << residentMultiplier << "\n";
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
                deviceInfo->physicalDevice, sharedFormat, pCreateInfo->imageFormat)) {
            std::cerr << "lsfg-vk: init stage=blit-format-unsupported sharedFormat="
                      << sharedFormat << " swapchainFormat=" << pCreateInfo->imageFormat
                      << "; preserving original swapchain\n";
            return createPassThrough("blit-unsupported");
        }
        std::cerr << "lsfg-vk: init stage=swapchain-blit-check-ready\n";

        createInfo.imageUsage |= requiredTransferUsage;

        const auto configuredPresentMode = activeConf.e_present;
        const bool recreatingExistingSwapchain = pCreateInfo->oldSwapchain != VK_NULL_HANDLE;
        // Adaptive and Fixed FG share the same WSI contract. This restores the
        // proven MAILBOX-capable path instead of forcing Adaptive onto FIFO.
        createInfo.presentMode = recreatingExistingSwapchain
            ? pCreateInfo->presentMode
            : choosePresentMode(
                deviceInfo->physicalDevice, pCreateInfo->surface,
                pCreateInfo->presentMode, configuredPresentMode);
        if (recreatingExistingSwapchain) {
            std::cerr << "lsfg-vk: init stage=swapchain-hot-recreate-present-mode"
                      << " adaptivePacing=0"
                      << " gameMode=" << pCreateInfo->presentMode
                      << " configuredMode=" << configuredPresentMode
                      << " effectiveMode=" << createInfo.presentMode << "\n";
        }

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
            uint32_t imageCount{};
            res = Layer::ovkGetSwapchainImagesKHR(device, *pSwapchain, &imageCount, nullptr);
            if (res != VK_SUCCESS || imageCount == 0)
                throw LSFG::vulkan_error(res, "Failed to get swapchain image count");

            std::vector<VkImage> swapchainImages(imageCount);
            res = Layer::ovkGetSwapchainImagesKHR(device, *pSwapchain,
                &imageCount, swapchainImages.data());
            if (res != VK_SUCCESS)
                throw LSFG::vulkan_error(res, "Failed to get swapchain images");

            const uint64_t swapchainGeneration =
                nextSwapchainGeneration.fetch_add(1, std::memory_order_relaxed);
            std::cerr << "lsfg-vk: init stage=present-contract"
                      << " swapchain_generation=" << swapchainGeneration
                      << " requested_present_mode=" << pCreateInfo->presentMode
                      << " wrapper_override_present_mode=" << configuredPresentMode
                      << " chosen_present_mode=" << createInfo.presentMode
                      << " actual_create_info_present_mode=" << createInfo.presentMode
                      << " image_count=" << imageCount
                      << " source_queue=application-present"
                      << " generated_queue=compatibility-selected"
                      << '\n';

            // Retire the old LSFG bookkeeping only after the replacement Vulkan
            // swapchain is known-good. If downstream creation fails, the old
            // swapchain remains usable and its context remains intact.
            std::cerr << "lsfg-vk: init stage=ls-context-begin images=" << imageCount
                      << " selectedPresentMode=" << createInfo.presentMode << "\n";
            auto state = std::make_shared<SwapchainState>();
            state->device = device;
            state->deviceInfo = deviceInfo;
            state->present = createInfo.presentMode;
            state->configuredPresent = configuredPresentMode;
            state->context = std::make_shared<LsContext>(
                *deviceInfo, *pSwapchain, pCreateInfo->imageExtent, swapchainImages);
            if (pCreateInfo->oldSwapchain)
                retireSwapchainState(pCreateInfo->oldSwapchain);
            publishSwapchainState(*pSwapchain, std::move(state));
            std::cerr << "lsfg-vk: init stage=ls-context-ready images=" << imageCount << "\n";
#ifdef __ANDROID__
            const bool generationActive = activeConf.multiplier > 1;
            publishRuntimeState(activeConf.config_file,
                generationActive ? "generating" : "source_only",
                generationActive, generationActive, true, !generationActive, true,
                false, false,
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
            // The downstream fallback has consumed the old handle as well.
            // Retire its wrapper now so a later present cannot use a context
            // whose real swapchain has already been replaced.
            if (pCreateInfo->oldSwapchain)
                retireSwapchainState(pCreateInfo->oldSwapchain);
            if (fallbackRes == VK_SUCCESS) {
                retireSwapchainState(failedSwapchain);
                Layer::ovkDestroySwapchainKHR(device, failedSwapchain, pAllocator);
                *pSwapchain = fallbackSwapchain;
                try {
                    auto state = std::make_shared<SwapchainState>();
                    state->device = device;
                    state->deviceInfo = deviceInfo;
                    publishSwapchainState(*pSwapchain, std::move(state));
                } catch (const std::exception& e) {
                    Utils::logLimitN("swapMap", 5,
                        "Could not retain fallback swapchain state; continuing natively:\n- "
                        + std::string(e.what()));
                }
#ifdef __ANDROID__
                publishRuntimeState(activeConf.config_file, "degraded",
                    false, false, false, false, false, false, true,
                    static_cast<int>(activeConf.multiplier), activeConf.performance,
                    activeConf.adaptiveFramegen, activeConf.fpsLimit);
#endif
                std::cerr << "lsfg-vk: init stage=swapchain-fallback-pass-through"
                             " reason=ls-context-failed\n";
                return VK_SUCCESS;
            }
            retireSwapchainState(failedSwapchain);
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
        const VkSwapchainKHR swapchainHandle = *pPresentInfo->pSwapchains;
        auto state = findSwapchainState(swapchainHandle);
        if (!state) {
            Utils::logLimitN("swapMap", 5,
                "Swapchain not found in map");
            return Layer::ovkQueuePresentKHR(queue, pPresentInfo);
        }
        std::unique_lock presentLock(state->presentMutex);
        const auto deviceInfo = state->deviceInfo;
        if (!deviceInfo)
            return Layer::ovkQueuePresentKHR(queue, pPresentInfo);

        auto conf = Config::snapshot();
#ifdef __ANDROID__
        auto& runtimeStats = state->runtimeStats;
        const auto configPollNow = RuntimeOutputStats::Clock::now();
        const bool shouldPollConfig = configPollNow >= runtimeStats.nextConfigPoll;
        if (shouldPollConfig)
            runtimeStats.nextConfigPoll = configPollNow + std::chrono::milliseconds(250);
#else
        const bool shouldPollConfig = true;
#endif
        if (shouldPollConfig && configurationFileChanged(conf)) {
            const std::string configFile = conf.config_file;
            const auto previousConf = conf;
            bool recreateSwapchain = false;
            std::error_code configError;
            const bool configExists = std::filesystem::exists(configFile, configError);
            if (configExists && !configError) {
                try {
                    Config::updateConfig(configFile);
                    Config::setActive(Config::getConfig(Utils::getProcessName()));
                    conf = Config::snapshot();
                    recreateSwapchain = requiresSwapchainRecreation(
                        previousConf, conf);
                    std::cerr << "lsfg-vk: init stage=config-reloaded multiplier="
                              << conf.multiplier
                              << " adaptive=" << (conf.adaptiveFramegen ? 1 : 0)
                              << " targetFps=" << conf.fpsLimit
                              << " adaptiveFlow="
                              << (conf.adaptiveFlowScale ? 1 : 0)
                              << " adaptiveFlowPreset="
                              << conf.adaptiveFlowPreset
                              << " fixedFlowScale=" << conf.flowScale
                              << " presentMode=" << conf.e_present
                              << " enabled=" << (conf.enable ? 1 : 0)
                              << " recreateSwapchain=" << (recreateSwapchain ? 1 : 0)
                              << "\n";
                    if (!recreateSwapchain) {
                        const bool generationActive = conf.multiplier > 1;
                        std::cerr << "lsfg-vk: runtime stage=config-reload-soft-toggle"
                                  << " oldEnabled=" << (previousConf.enable ? 1 : 0)
                                  << " newEnabled=" << (conf.enable ? 1 : 0)
                                  << " oldMultiplier=" << previousConf.multiplier
                                  << " newMultiplier=" << conf.multiplier
                                  << " resident=1"
                                  << " source_only=" << (generationActive ? 0 : 1)
                                  << " generation_ready=" << (generationActive ? 1 : 0)
                                  << " recreateSwapchain=0"
                                  << "\n";
                    }
                } catch (const std::exception& e) {
                    Utils::logLimitN("configReload", 5,
                        "Failed to hot-reload configuration; preserving the active runtime:\n- "
                        + std::string(e.what()));
                }
            } else {
                recreateSwapchain = true;
            }
            if (recreateSwapchain) {
#ifdef __ANDROID__
                publishRuntimeState(configFile, "degraded",
                    false, false, false, false, false, false, true,
                    static_cast<int>(conf.multiplier), conf.performance,
                    conf.adaptiveFramegen, conf.fpsLimit);
#endif
                Layer::ovkQueuePresentKHR(queue, pPresentInfo);
                return VK_ERROR_OUT_OF_DATE_KHR;
            }
        }

        if (!state->context) {
            // A missing LSFG wrapper is expected for disabled/degraded
            // pass-through swapchains. Creation already logs the reason, so do
            // not add repeated work or noise on every present.
            return Layer::ovkQueuePresentKHR(queue, pPresentInfo);
        }
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
        const VkSwapchainPresentModeInfoEXT* presentModeInfo =
            reinterpret_cast<const VkSwapchainPresentModeInfoEXT*>(pPresentInfo->pNext);
        while (presentModeInfo) {
            if (presentModeInfo->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT) {
                for (size_t i = 0; i < presentModeInfo->swapchainCount; i++)
                    const_cast<VkPresentModeKHR*>(presentModeInfo->pPresentModes)[i] =
                        state->present;
            }
            presentModeInfo =
                reinterpret_cast<const VkSwapchainPresentModeInfoEXT*>(presentModeInfo->pNext);
        }
        #pragma clang diagnostic pop

        const VkPresentModeKHR desiredPresentMode = conf.e_present;
        if (state->configuredPresent != desiredPresentMode) {
            Layer::ovkQueuePresentKHR(queue, pPresentInfo);
            return VK_ERROR_OUT_OF_DATE_KHR;
        }

        try {
#ifdef __ANDROID__
            auto& semaphores = runtimeStats.presentWaitSemaphores;
            semaphores.resize(pPresentInfo->waitSemaphoreCount);
#else
            std::vector<VkSemaphore> semaphores(pPresentInfo->waitSemaphoreCount);
#endif
            if (!semaphores.empty())
                std::copy_n(pPresentInfo->pWaitSemaphores, semaphores.size(), semaphores.data());

            const auto res = state->context->present(*deviceInfo, pPresentInfo->pNext,
                queue, semaphores, *pPresentInfo->pImageIndices);

#ifdef __ANDROID__
            recordSuccessfulOutputCycle(*state, *state->context,
                conf.config_file, state->context->lastGeneratedFrameCount(),
                conf.multiplier, conf.performance,
                conf.adaptiveFramegen, conf.fpsLimit);
#endif
            Utils::resetLimitN("swapPresent");
            return res;
        } catch (const std::exception& e) {
#ifdef __ANDROID__
            recordOutputFailure(*state);
            publishRuntimeState(conf.config_file, "degraded",
                false, false, false, false, false, false, true,
                static_cast<int>(conf.multiplier), conf.performance,
                conf.adaptiveFramegen, conf.fpsLimit);
#endif
            Utils::logLimitN("swapPresent", 5,
                "An error occurred while presenting the swapchain; degrading to native presentation:\n"
                "- " + std::string(e.what()));
            state->context.reset();
            std::cerr << "lsfg-vk: runtime stage=context-degraded-bypass reason=present-error\n";
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    void myvkDestroySwapchainKHR(
            VkDevice device,
            VkSwapchainKHR swapchain,
            const VkAllocationCallbacks* pAllocator) noexcept {
        retireSwapchainState(swapchain);
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

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
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifdef __ANDROID__
#include <sys/inotify.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#endif

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

    VkExternalSemaphoreProperties queryExternalSemaphoreProperties(
            VkPhysicalDevice physicalDevice, VkExternalSemaphoreHandleTypeFlagBits handleType) {
        VkExternalSemaphoreProperties properties{
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
        };
        auto getExternalSemaphoreProperties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(
                Layer::ovkGetInstanceProcAddr(layerInstance,
                    "vkGetPhysicalDeviceExternalSemaphoreProperties"));
        if (getExternalSemaphoreProperties == nullptr) {
            getExternalSemaphoreProperties =
                reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(
                    Layer::ovkGetInstanceProcAddr(layerInstance,
                        "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR"));
        }
        if (getExternalSemaphoreProperties == nullptr)
            return properties;
        const VkPhysicalDeviceExternalSemaphoreInfo info{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
            .handleType = handleType,
        };
        getExternalSemaphoreProperties(physicalDevice, &info, &properties);
        return properties;
    }

    bool supportsOpaqueFdExternalSemaphore(VkPhysicalDevice physicalDevice) {
        if (!supportsDeviceExtension(physicalDevice,
                VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME))
            return false;

        const auto properties = queryExternalSemaphoreProperties(
            physicalDevice, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
        constexpr VkExternalSemaphoreFeatureFlags required =
            VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
            VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
        return (properties.externalSemaphoreFeatures & required) == required;
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
        std::vector<const char*> requiredExtensions{
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
        };
        if (supportsOpaqueFdExternalSemaphore(physicalDevice))
            requiredExtensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
        auto extensions = Utils::addExtensions(
            pCreateInfo->ppEnabledExtensionNames,
            pCreateInfo->enabledExtensionCount,
            requiredExtensions);
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
        const bool androidExternalSemaphoreFdSupported = androidAhbSupported
            && supportsOpaqueFdExternalSemaphore(physicalDevice);
#else
        const bool androidAhbSupported = true;
        const bool androidExternalSemaphoreFdSupported = false;
#endif
#ifdef __ANDROID__
        const bool externalSemaphoreFdExtension = supportsDeviceExtension(physicalDevice,
            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
        const auto opaqueFdProperties = queryExternalSemaphoreProperties(physicalDevice,
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
        const auto syncFdProperties = queryExternalSemaphoreProperties(physicalDevice,
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
        const bool getSemaphoreFdProc = Layer::ovkGetDeviceProcAddr(*pDevice, "vkGetSemaphoreFdKHR") != nullptr;
        const bool importSemaphoreFdProc = Layer::ovkGetDeviceProcAddr(*pDevice, "vkImportSemaphoreFdKHR") != nullptr;
        std::cerr << "lsfg-vk: game-external-semaphore-fd"
                  << " extension=" << (externalSemaphoreFdExtension ? 1 : 0)
                  << " opaqueFeatures=0x" << std::hex << opaqueFdProperties.externalSemaphoreFeatures
                  << " opaqueCompatible=0x" << opaqueFdProperties.compatibleHandleTypes
                  << " opaqueExportFromImported=0x" << opaqueFdProperties.exportFromImportedHandleTypes
                  << " syncFeatures=0x" << syncFdProperties.externalSemaphoreFeatures
                  << " syncCompatible=0x" << syncFdProperties.compatibleHandleTypes
                  << " syncExportFromImported=0x" << syncFdProperties.exportFromImportedHandleTypes << std::dec
                  << " getSemaphoreFdKHR=" << (getSemaphoreFdProc ? 1 : 0)
                  << " importSemaphoreFdKHR=" << (importSemaphoreFdProc ? 1 : 0) << '\n';
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
            .androidAhbSupported = androidAhbSupported,
            .androidExternalSemaphoreFdSupported = androidExternalSemaphoreFdSupported
        });
        return VK_SUCCESS;
    }

    VkPresentModeKHR choosePresentMode(
            VkPhysicalDevice physicalDevice,
            VkSurfaceKHR surface,
            VkPresentModeKHR gamePresentMode,
            VkPresentModeKHR configuredPresentMode);

    void myvkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) noexcept {
        deviceToInfo.erase(device);
        Layer::ovkDestroyDevice(device, pAllocator);
    }

    std::unordered_map<VkSwapchainKHR, LsContext> swapchains;
    std::unordered_map<VkSwapchainKHR, VkDevice> swapchainToDeviceTable;
    std::unordered_map<VkSwapchainKHR, VkPresentModeKHR> swapchainToPresent;
    std::unordered_map<VkSwapchainKHR, VkPresentModeKHR> swapchainToConfiguredPresent;

#ifdef __ANDROID__
    struct RuntimeOutputStats {
        using Clock = std::chrono::steady_clock;
        Clock::time_point windowStart{Clock::now()};
        std::vector<VkSemaphore> presentWaitSemaphores;
        uint64_t windowSourceFrames{0};
        uint64_t windowGeneratedFrames{0};
        uint64_t totalSourceFrames{0};
        uint64_t totalGeneratedFrames{0};
        uint64_t presentFailures{0};
    };

    class AndroidConfigWatcher {
    public:
        ~AndroidConfigWatcher() {
            stop();
        }

        bool changed(const std::string& configFile) noexcept {
            if (configFile.empty())
                return false;
            if (configFile != configFile_)
                arm(configFile);
            return changed_.exchange(false, std::memory_order_acq_rel);
        }

    private:
        void arm(const std::string& configFile) noexcept {
            stop();
            configFile_ = configFile;
            const std::filesystem::path path(configFile_);
            directory_ = path.parent_path().string();
            filename_ = path.filename().string();

            fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
            if (fd_ >= 0) {
                wd_ = ::inotify_add_watch(fd_, directory_.c_str(),
                    IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_ATTRIB |
                    IN_MOVE_SELF | IN_DELETE_SELF);
            }

            std::error_code ec;
            fallbackTimestamp_ = std::filesystem::last_write_time(configFile_, ec);
            fallbackTimestampValid_ = !ec;
            stopRequested_.store(false, std::memory_order_release);
            worker_ = std::thread(&AndroidConfigWatcher::run, this);
        }

        void run() noexcept {
            bool useInotify = fd_ >= 0 && wd_ >= 0;
            auto nextFallbackPoll = RuntimeOutputStats::Clock::now() + std::chrono::seconds(1);
            alignas(struct inotify_event) char buffer[4096];

            while (!stopRequested_.load(std::memory_order_acquire)) {
                if (useInotify) {
                    struct pollfd pfd { fd_, POLLIN, 0 };
                    const int pollResult = ::poll(&pfd, 1, 250);
                    if (pollResult > 0 && (pfd.revents & POLLIN)) {
                        for (;;) {
                            const ssize_t length = ::read(fd_, buffer, sizeof(buffer));
                            if (length < 0) {
                                if (errno == EAGAIN || errno == EWOULDBLOCK)
                                    break;
                                useInotify = false;
                                break;
                            }
                            if (length == 0)
                                break;

                            size_t offset = 0;
                            while (offset < static_cast<size_t>(length)) {
                                const auto* event = reinterpret_cast<const struct inotify_event*>(buffer + offset);
                                const bool matchingName = event->len > 0 && filename_ == event->name;
                                if (matchingName && (event->mask & (
                                        IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE |
                                        IN_ATTRIB | IN_MOVE_SELF | IN_DELETE_SELF))) {
                                    changed_.store(true, std::memory_order_release);
                                }
                                if (event->mask & IN_IGNORED)
                                    useInotify = false;
                                offset += sizeof(struct inotify_event) + event->len;
                            }
                        }
                    } else if (pollResult < 0 && errno != EINTR) {
                        useInotify = false;
                    }
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }

                if (!useInotify && RuntimeOutputStats::Clock::now() >= nextFallbackPoll) {
                    nextFallbackPoll = RuntimeOutputStats::Clock::now() + std::chrono::seconds(1);
                    std::error_code ec;
                    const auto timestamp = std::filesystem::last_write_time(configFile_, ec);
                    const bool valid = !ec;
                    if (valid != fallbackTimestampValid_
                            || (valid && timestamp != fallbackTimestamp_)) {
                        changed_.store(true, std::memory_order_release);
                    }
                    fallbackTimestamp_ = timestamp;
                    fallbackTimestampValid_ = valid;
                }
            }
        }

        void stop() noexcept {
            stopRequested_.store(true, std::memory_order_release);
            if (worker_.joinable())
                worker_.join();
            if (fd_ >= 0 && wd_ >= 0)
                ::inotify_rm_watch(fd_, wd_);
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = -1;
            wd_ = -1;
            configFile_.clear();
            directory_.clear();
            filename_.clear();
            changed_.store(false, std::memory_order_release);
        }

        int fd_{-1};
        int wd_{-1};
        std::string configFile_;
        std::string directory_;
        std::string filename_;
        std::filesystem::file_time_type fallbackTimestamp_{};
        bool fallbackTimestampValid_{false};
        std::atomic<bool> changed_{false};
        std::atomic<bool> stopRequested_{false};
        std::thread worker_;
    };

    AndroidConfigWatcher androidConfigWatcher;
    std::unordered_map<VkSwapchainKHR, RuntimeOutputStats> runtimeOutputStats;

    class AndroidStatsPublisher {
    public:
        ~AndroidStatsPublisher() {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stopping_ = true;
            }
            cv_.notify_one();
            if (worker_.joinable())
                worker_.join();
        }

        void publish(std::filesystem::path path, std::string payload) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!worker_.joinable())
                    worker_ = std::thread(&AndroidStatsPublisher::run, this);
                pending_ = Pending{std::move(path), std::move(payload)};
            }
            cv_.notify_one();
        }

    private:
        struct Pending {
            std::filesystem::path path;
            std::string payload;
        };

        static void write(const Pending& pending) noexcept {
            const std::filesystem::path tempPath = pending.path.string() + ".tmp";
            try {
                std::ofstream out(tempPath, std::ios::trunc);
                if (!out)
                    throw std::runtime_error("unable to open temporary stats file");
                out << pending.payload;
                out.close();
                if (!out)
                    throw std::runtime_error("failed to flush temporary stats file");

                std::error_code ec;
                std::filesystem::rename(tempPath, pending.path, ec);
                if (ec) {
                    std::filesystem::remove(pending.path, ec);
                    ec.clear();
                    std::filesystem::rename(tempPath, pending.path, ec);
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

        void run() noexcept {
            for (;;) {
                std::optional<Pending> pending;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
                    if (stopping_ && !pending_.has_value())
                        return;
                    pending = std::move(pending_);
                    pending_.reset();
                }
                write(*pending);
            }
        }

        std::mutex mutex_;
        std::condition_variable cv_;
        std::optional<Pending> pending_;
        std::thread worker_;
        bool stopping_{false};
    };

    AndroidStatsPublisher androidStatsPublisher;

    std::filesystem::path runtimeStatsPath(const std::string& configFile) {
        return std::filesystem::path(configFile).parent_path() / "stats.txt";
    }

    void publishRuntimeState(const std::string& configFile,
            bool active, bool generationReady, int multiplier,
            bool performance, bool adaptive, uint32_t targetFps) {
        if (configFile.empty())
            return;

        std::ostringstream out;
        out << "active=" << (active ? 1 : 0) << '\n'
            << "generation_ready=" << (generationReady ? 1 : 0) << '\n'
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
        androidStatsPublisher.publish(runtimeStatsPath(configFile), out.str());
    }

    void writeRuntimeStatsFile(const std::string& configFile,
            double outputFps, double sourceFps, double generatedFps,
            const RuntimeOutputStats& stats, int multiplier, bool performance,
            bool adaptive, uint32_t targetFps) {
        if (configFile.empty())
            return;

        std::ostringstream out;
        out << std::fixed << std::setprecision(3)
            << "active=1\n"
            << "generation_ready=1\n"
            << "fps=" << outputFps << '\n'
            << "source_fps=" << sourceFps << '\n'
            << "generated_fps=" << generatedFps << '\n'
            << "source_frames_total=" << stats.totalSourceFrames << '\n'
            << "generated_frames_total=" << stats.totalGeneratedFrames << '\n'
            << "present_failures=" << stats.presentFailures << '\n'
            << "multiplier=" << multiplier << '\n'
            << "adaptive=" << (adaptive ? 1 : 0) << '\n'
            << "target_fps=" << targetFps << '\n'
            << "performance=" << (performance ? 1 : 0) << '\n';
        androidStatsPublisher.publish(runtimeStatsPath(configFile), out.str());
    }

    void recordSuccessfulOutputCycle(VkSwapchainKHR swapchain,
            const std::string& configFile, uint64_t generated,
            int multiplier, bool performance, bool adaptive, uint32_t targetFps) {
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
        writeRuntimeStatsFile(configFile, outputFps, sourceFps, generatedFps,
            stats, multiplier, performance, adaptive, targetFps);

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
        const auto& activeConf = Config::activeConf;

        const auto createPassThrough = [&](const char* reason) -> VkResult {
            const auto res = Layer::ovkCreateSwapchainKHR(
                device, pCreateInfo, pAllocator, pSwapchain);
            if (res == VK_SUCCESS) {
                if (pCreateInfo->oldSwapchain)
                    eraseSwapchainState(pCreateInfo->oldSwapchain);
                swapchainToDeviceTable.emplace(*pSwapchain, device);
#ifdef __ANDROID__
                publishRuntimeState(activeConf.config_file, false, false,
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

        const auto configuredPresentMode = Config::activeConf.e_present;
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

        auto& conf = Config::activeConf;
#ifdef __ANDROID__
        auto& runtimeStats = runtimeOutputStats[*pPresentInfo->pSwapchains];
        const bool shouldPollConfig = androidConfigWatcher.changed(conf.config_file);
#else
        const bool shouldPollConfig = true;
#endif
        if (shouldPollConfig && !conf.config_file.empty()) {
            const std::string configFile = conf.config_file;
            const auto previousConf = conf;
            bool recreateSwapchain = false;
            std::error_code configEc;
            const bool configExists = std::filesystem::exists(configFile, configEc) && !configEc;
            if (configExists) {
                try {
                    Config::updateConfig(configFile);
                    Config::activeConf = Config::getConfig(Utils::getProcessName());
                    recreateSwapchain = requiresSwapchainRecreation(
                        previousConf, Config::activeConf);
                    std::cerr << "lsfg-vk: init stage=config-reloaded multiplier="
                              << Config::activeConf.multiplier
                              << " adaptive=" << (Config::activeConf.adaptiveFramegen ? 1 : 0)
                              << " targetFps=" << Config::activeConf.fpsLimit
                              << " presentMode=" << Config::activeConf.e_present
                              << " enabled=" << (Config::activeConf.enable ? 1 : 0)
                              << " recreateSwapchain=" << (recreateSwapchain ? 1 : 0)
                              << "\n";
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
                publishRuntimeState(configFile, false, false,
                    static_cast<int>(Config::activeConf.multiplier),
                    Config::activeConf.performance,
                    Config::activeConf.adaptiveFramegen,
                    Config::activeConf.fpsLimit);
#endif
                Layer::ovkQueuePresentKHR(queue, pPresentInfo);
                return VK_ERROR_OUT_OF_DATE_KHR;
            }
        }

        if (!conf.enable || conf.multiplier <= 1)
            return Layer::ovkQueuePresentKHR(queue, pPresentInfo);

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

        if (configuredPresent != conf.e_present) {
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

            const auto res = swapchain.present(deviceInfo, pPresentInfo->pNext,
                queue, semaphores, *pPresentInfo->pImageIndices);

#ifdef __ANDROID__
            recordSuccessfulOutputCycle(*pPresentInfo->pSwapchains,
                conf.config_file, swapchain.lastGeneratedFrameCount(),
                conf.multiplier, conf.performance,
                conf.adaptiveFramegen, conf.fpsLimit);
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
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
#include <exception>
#include <iostream>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Hooks;

namespace {

    VkInstance layerInstance{};

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
        deviceToInfo.emplace(*pDevice, DeviceInfo {
            .device = *pDevice,
            .physicalDevice = physicalDevice,
            .queue = Utils::findQueue(*pDevice, physicalDevice, pCreateInfo, VK_QUEUE_GRAPHICS_BIT),
            .androidAhbSupported = androidAhbSupported
        });
        return VK_SUCCESS;
    }

    void myvkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) noexcept {
        deviceToInfo.erase(device);
        Layer::ovkDestroyDevice(device, pAllocator);
    }

    std::unordered_map<VkSwapchainKHR, LsContext> swapchains;
    std::unordered_map<VkSwapchainKHR, VkDevice> swapchainToDeviceTable;
    std::unordered_map<VkSwapchainKHR, VkPresentModeKHR> swapchainToPresent;
    std::unordered_map<VkSwapchainKHR, VkPresentModeKHR> swapchainToConfiguredPresent;

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

#ifdef __ANDROID__
        if (!deviceInfo.androidAhbSupported) {
            Utils::logLimitN("swapAhb", 5,
                "init stage=ahb-extension-unavailable; preserving original swapchain");
            return Layer::ovkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        }
#endif

        VkSurfaceCapabilitiesKHR surfaceCapabilities{};
        auto surfaceRes = Layer::ovkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            deviceInfo.physicalDevice, pCreateInfo->surface, &surfaceCapabilities);
        if (surfaceRes != VK_SUCCESS) {
            Utils::logLimitN("swapCaps", 5,
                "init stage=swapchain-capabilities-unavailable; preserving original swapchain");
            return Layer::ovkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        }

        constexpr VkImageUsageFlags requiredTransferUsage =
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if ((surfaceCapabilities.supportedUsageFlags & requiredTransferUsage)
                != requiredTransferUsage) {
            std::cerr << "lsfg-vk: init stage=swapchain-unsupported-usage supportedUsage="
                      << surfaceCapabilities.supportedUsageFlags
                      << " requiredUsage=" << requiredTransferUsage
                      << "; preserving original swapchain\n";
            return Layer::ovkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        }

        VkSwapchainCreateInfoKHR createInfo = *pCreateInfo;
        const uint32_t maxImages = surfaceCapabilities.maxImageCount == 0
            ? UINT32_MAX : surfaceCapabilities.maxImageCount;
        // LSFG needs one additional image available while the game's acquired
        // image is being transformed. Queue-family numbering is unrelated and
        // is vendor-defined, so it must not influence swapchain sizing.
        createInfo.minImageCount = pCreateInfo->minImageCount + 1;
        if (createInfo.minImageCount > maxImages) {
            createInfo.minImageCount = maxImages;
            Utils::logLimitN("swapCount", 10,
                "Requested image count (" +
                    std::to_string(pCreateInfo->minImageCount) + ") "
                "exceeds maximum allowed (" +
                    std::to_string(maxImages) + "). "
                "Continuing with maximum allowed image count. "
                "This might lead to performance degradation.");
        } else {
            Utils::resetLimitN("swapCount");
        }

        createInfo.imageUsage |= requiredTransferUsage;

        const auto configuredPresentMode = Config::activeConf.e_present;
        createInfo.presentMode = choosePresentMode(
            deviceInfo.physicalDevice, pCreateInfo->surface,
            pCreateInfo->presentMode, configuredPresentMode);

        if (pCreateInfo->oldSwapchain) {
            swapchains.erase(pCreateInfo->oldSwapchain);
            swapchainToDeviceTable.erase(pCreateInfo->oldSwapchain);
            swapchainToPresent.erase(pCreateInfo->oldSwapchain);
            swapchainToConfiguredPresent.erase(pCreateInfo->oldSwapchain);
        }

        auto res = Layer::ovkCreateSwapchainKHR(device, &createInfo, pAllocator, pSwapchain);
        if (res != VK_SUCCESS)
            return res;

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

            swapchainToDeviceTable.emplace(*pSwapchain, device);
            std::cerr << "lsfg-vk: init stage=ls-context-begin images=" << imageCount
                      << " selectedPresentMode=" << createInfo.presentMode << "\n";
            swapchains.emplace(*pSwapchain, LsContext(
                deviceInfo, *pSwapchain, pCreateInfo->imageExtent,
                swapchainImages
            ));
            std::cerr << "lsfg-vk: init stage=ls-context-ready images=" << imageCount << "\n";

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
            return VK_SUCCESS;
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

        VkResult res{};
        try {
            auto& conf = Config::activeConf;
            if (!conf.config_file.empty()
                    && (
                            !std::filesystem::exists(conf.config_file)
                          || conf.timestamp != std::filesystem::last_write_time(conf.config_file)
                    )) {
                const std::string configFile = conf.config_file;
                if (std::filesystem::exists(configFile)) {
                    try {
                        Config::updateConfig(configFile);
                        Config::activeConf = Config::getConfig(Utils::getProcessName());
                        std::cerr << "lsfg-vk: init stage=config-reloaded multiplier="
                                  << Config::activeConf.multiplier
                                  << " presentMode=" << Config::activeConf.e_present
                                  << " enabled=" << (Config::activeConf.enable ? 1 : 0)
                                  << "\n";
                    } catch (const std::exception& e) {
                        Utils::logLimitN("configReload", 5,
                            "Failed to hot-reload configuration; requesting swapchain recreation:\n- "
                            + std::string(e.what()));
                    }
                }
                Layer::ovkQueuePresentKHR(queue, pPresentInfo);
                return VK_ERROR_OUT_OF_DATE_KHR;
            }

            if (configuredPresent != conf.e_present) {
                Layer::ovkQueuePresentKHR(queue, pPresentInfo);
                return VK_ERROR_OUT_OF_DATE_KHR;
            }

            if (conf.multiplier <= 1)
                return Layer::ovkQueuePresentKHR(queue, pPresentInfo);

            std::vector<VkSemaphore> semaphores(pPresentInfo->waitSemaphoreCount);
            std::copy_n(pPresentInfo->pWaitSemaphores, semaphores.size(), semaphores.data());

            res = swapchain.present(deviceInfo, pPresentInfo->pNext,
                queue, semaphores, *pPresentInfo->pImageIndices);

            Utils::resetLimitN("swapPresent");
        } catch (const std::exception& e) {
            Utils::logLimitN("swapPresent", 5,
                "An error occurred while presenting the swapchain:\n"
                "- " + std::string(e.what()));
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return res;
    }

    void myvkDestroySwapchainKHR(
            VkDevice device,
            VkSwapchainKHR swapchain,
            const VkAllocationCallbacks* pAllocator) noexcept {
        swapchains.erase(swapchain);
        swapchainToDeviceTable.erase(swapchain);
        swapchainToPresent.erase(swapchain);
        swapchainToConfiguredPresent.erase(swapchain);
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

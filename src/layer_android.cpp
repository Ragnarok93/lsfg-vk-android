#ifdef __ANDROID__

#include "layer.hpp"
#include "common/exception.hpp"
#include "config/config.hpp"
#include "hooks.hpp"

#include <android/hardware_buffer.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace {
PFN_vkCreateInstance next_vkCreateInstance{};
PFN_vkDestroyInstance next_vkDestroyInstance{};
PFN_vkCreateDevice next_vkCreateDevice{};
PFN_vkGetInstanceProcAddr next_vkGetInstanceProcAddr{};
thread_local PFN_vkGetDeviceProcAddr next_vkGetDeviceProcAddr{};
struct DeviceConstructionScope {
    PFN_vkGetDeviceProcAddr previous;
    explicit DeviceConstructionScope(PFN_vkGetDeviceProcAddr gdpa)
        : previous(next_vkGetDeviceProcAddr) { next_vkGetDeviceProcAddr = gdpa; }
    ~DeviceConstructionScope() { next_vkGetDeviceProcAddr = previous; }
};
PFN_vkGetPhysicalDeviceQueueFamilyProperties next_vkGetPhysicalDeviceQueueFamilyProperties{};
PFN_vkGetPhysicalDeviceMemoryProperties next_vkGetPhysicalDeviceMemoryProperties{};
PFN_vkGetPhysicalDeviceProperties next_vkGetPhysicalDeviceProperties{};
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR next_vkGetPhysicalDeviceSurfaceCapabilitiesKHR{};

struct PrivateInstanceDispatch {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr{};
    PFN_vkDestroyInstance DestroyInstance{};
};

std::unordered_map<VkInstance, PrivateInstanceDispatch> privateInstanceDispatchTables;
std::shared_mutex privateInstanceDispatchMutex;

bool loadPrivateInstanceDispatch(VkInstance instance, PrivateInstanceDispatch* dispatch) {
    if (instance == VK_NULL_HANDLE || !dispatch) return false;
    std::shared_lock lock(privateInstanceDispatchMutex);
    const auto it = privateInstanceDispatchTables.find(instance);
    if (it == privateInstanceDispatchTables.end()) return false;
    *dispatch = it->second;
    return true;
}

void storePrivateInstanceDispatch(VkInstance instance, PFN_vkGetInstanceProcAddr getInstanceProcAddr) {
    if (instance == VK_NULL_HANDLE || !getInstanceProcAddr) return;
    PrivateInstanceDispatch dispatch{
        .GetInstanceProcAddr = getInstanceProcAddr,
        .DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
            getInstanceProcAddr(instance, "vkDestroyInstance")),
    };
    std::unique_lock lock(privateInstanceDispatchMutex);
    privateInstanceDispatchTables[instance] = dispatch;
}

void erasePrivateInstanceDispatch(VkInstance instance) {
    if (instance == VK_NULL_HANDLE) return;
    std::unique_lock lock(privateInstanceDispatchMutex);
    privateInstanceDispatchTables.erase(instance);
}

// The Vulkan loader contract is distributed-dispatch: device entry points
// returned by vkGetDeviceProcAddr belong to the queried logical device. Some
// Android vendor wrappers return device-specific thunks, so a table keyed only
// by the loader dispatch pointer is not sufficient: two logical devices from
// the same driver commonly share that pointer. Keep the device handle as the
// owner key and explicitly associate queues/command buffers with that owner.
struct DeviceDispatch {
    VkDevice device{VK_NULL_HANDLE};
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr{};
    PFN_vkSetDeviceLoaderData SetDeviceLoaderData{};
    PFN_vkDestroyDevice DestroyDevice{};
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR{};
    PFN_vkQueuePresentKHR QueuePresentKHR{};
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR{};
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR{};
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers{};
    PFN_vkFreeCommandBuffers FreeCommandBuffers{};
    PFN_vkBeginCommandBuffer BeginCommandBuffer{};
    PFN_vkEndCommandBuffer EndCommandBuffer{};
    PFN_vkCreateCommandPool CreateCommandPool{};
    PFN_vkDestroyCommandPool DestroyCommandPool{};
    PFN_vkCreateImage CreateImage{};
    PFN_vkDestroyImage DestroyImage{};
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements{};
    PFN_vkBindImageMemory BindImageMemory{};
    PFN_vkAllocateMemory AllocateMemory{};
    PFN_vkFreeMemory FreeMemory{};
    PFN_vkCreateSemaphore CreateSemaphore{};
    PFN_vkDestroySemaphore DestroySemaphore{};
    PFN_vkGetMemoryFdKHR GetMemoryFdKHR{};
    PFN_vkGetSemaphoreFdKHR GetSemaphoreFdKHR{};
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID GetAndroidHardwareBufferPropertiesANDROID{};
    PFN_vkGetDeviceQueue GetDeviceQueue{};
    PFN_vkGetDeviceQueue2 GetDeviceQueue2{};
    PFN_vkQueueSubmit QueueSubmit{};
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier{};
    PFN_vkCmdBlitImage CmdBlitImage{};
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR{};
    bool presentationDevice{false};
};

std::unordered_map<VkDevice, DeviceDispatch> deviceDispatchTables;
std::unordered_map<VkQueue, DeviceDispatch> queueDispatchTables;
std::unordered_map<VkCommandBuffer, DeviceDispatch> commandBufferDispatchTables;
std::shared_mutex deviceDispatchMutex;

bool loadDeviceDispatch(VkDevice device, DeviceDispatch* dispatch) {
    if (device == VK_NULL_HANDLE || !dispatch) return false;
    std::shared_lock lock(deviceDispatchMutex);
    const auto it = deviceDispatchTables.find(device);
    if (it == deviceDispatchTables.end()) return false;
    *dispatch = it->second;
    return true;
}

bool loadQueueDispatch(VkQueue queue, DeviceDispatch* dispatch) {
    if (queue == VK_NULL_HANDLE || !dispatch) return false;
    std::shared_lock lock(deviceDispatchMutex);
    const auto it = queueDispatchTables.find(queue);
    if (it == queueDispatchTables.end()) return false;
    *dispatch = it->second;
    return true;
}

bool loadCommandBufferDispatch(VkCommandBuffer commandBuffer,
        DeviceDispatch* dispatch) {
    if (commandBuffer == VK_NULL_HANDLE || !dispatch) return false;
    std::shared_lock lock(deviceDispatchMutex);
    const auto it = commandBufferDispatchTables.find(commandBuffer);
    if (it == commandBufferDispatchTables.end()) return false;
    *dispatch = it->second;
    return true;
}

void storeDeviceDispatch(VkDevice device, const DeviceDispatch& dispatch) {
    if (device == VK_NULL_HANDLE) return;
    auto ownedDispatch = dispatch;
    ownedDispatch.device = device;
    std::unique_lock lock(deviceDispatchMutex);
    deviceDispatchTables[device] = std::move(ownedDispatch);
}

void storeQueueDispatch(VkQueue queue, const DeviceDispatch& dispatch) {
    if (queue == VK_NULL_HANDLE || dispatch.device == VK_NULL_HANDLE) return;
    std::unique_lock lock(deviceDispatchMutex);
    const auto previous = queueDispatchTables.find(queue);
    if (previous == queueDispatchTables.end()) {
        std::cerr << "lsfg-vk: dispatch queue=" << queue << " owner=" << dispatch.device
                  << " policy=exact-device-queue submit=" << reinterpret_cast<void*>(dispatch.QueueSubmit)
                  << " present=" << reinterpret_cast<void*>(dispatch.QueuePresentKHR) << "\n";
    } else if (previous->second.device != dispatch.device) {
        // Two live devices cannot own the same dispatchable handle. Poison the
        // entry instead of replacing a still-live owner with a guessed one.
        previous->second = DeviceDispatch{};
        std::cerr << "lsfg-vk: dispatch queue ownership collision queue=" << queue << "\n";
        return;
    }
    queueDispatchTables[queue] = dispatch;
}

void storeCommandBufferDispatch(
        VkCommandBuffer commandBuffer, const DeviceDispatch& dispatch) {
    if (commandBuffer == VK_NULL_HANDLE || dispatch.device == VK_NULL_HANDLE)
        return;
    std::unique_lock lock(deviceDispatchMutex);
    commandBufferDispatchTables[commandBuffer] = dispatch;
}

void eraseDeviceDispatch(VkDevice device) {
    if (device == VK_NULL_HANDLE) return;
    std::unique_lock lock(deviceDispatchMutex);
    deviceDispatchTables.erase(device);
    for (auto it = queueDispatchTables.begin(); it != queueDispatchTables.end();) {
        if (it->second.device == device)
            it = queueDispatchTables.erase(it);
        else
            ++it;
    }
    for (auto it = commandBufferDispatchTables.begin();
            it != commandBufferDispatchTables.end();) {
        if (it->second.device == device)
            it = commandBufferDispatchTables.erase(it);
        else
            ++it;
    }
}

template <typename T>
bool initInstanceFunc(VkInstance instance, const char* name, T* func) {
    *func = reinterpret_cast<T>(next_vkGetInstanceProcAddr(instance, name));
    if (!*func) {
        std::cerr << "(no function pointer for " << name << ")\n";
        return false;
    }
    return true;
}

template <typename T>
bool initDeviceFunc(VkDevice device, PFN_vkGetDeviceProcAddr gdpa,
        const char* name, T* func, bool required = true) {
    *func = gdpa ? reinterpret_cast<T>(gdpa(device, name)) : nullptr;
    if (!*func && required)
        std::cerr << "lsfg-vk: missing device function " << name << " owner=" << device << "\n";
    return *func != nullptr || !required;
}

void registerPassthroughDevice(VkDevice device, DeviceDispatch dispatch) {
    const auto gdpa = dispatch.GetDeviceProcAddr;
    initDeviceFunc(device, gdpa, "vkDestroyDevice", &dispatch.DestroyDevice);
    initDeviceFunc(device, gdpa, "vkGetDeviceQueue", &dispatch.GetDeviceQueue);
    initDeviceFunc(device, gdpa, "vkGetDeviceQueue2", &dispatch.GetDeviceQueue2, false);
    initDeviceFunc(device, gdpa, "vkQueueSubmit", &dispatch.QueueSubmit);
    initDeviceFunc(device, gdpa, "vkQueuePresentKHR", &dispatch.QueuePresentKHR, false);
    storeDeviceDispatch(device, dispatch);
}

bool deviceExtensionEnabled(const VkDeviceCreateInfo* createInfo, const char* extension) {
    if (!createInfo || !createInfo->ppEnabledExtensionNames) return false;
    for (uint32_t i = 0; i < createInfo->enabledExtensionCount; ++i) {
        const char* name = createInfo->ppEnabledExtensionNames[i];
        if (name && std::strcmp(name, extension) == 0) return true;
    }
    return false;
}

bool isDeviceWsiHook(const std::string& name) {
    return name == "vkCreateSwapchainKHR"
        || name == "vkQueuePresentKHR"
        || name == "vkDestroySwapchainKHR";
}


void layer_vkDestroyPrivateInstance(
        VkInstance instance,
        const VkAllocationCallbacks* pAllocator) {
    PrivateInstanceDispatch dispatch{};
    if (!loadPrivateInstanceDispatch(instance, &dispatch))
        return;
    erasePrivateInstanceDispatch(instance);
    if (dispatch.DestroyInstance) {
        dispatch.DestroyInstance(instance, pAllocator);
    }
}

VkResult layer_vkCreateInstance(
        const VkInstanceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkInstance* pInstance) {
    try {
        auto* layerDesc = const_cast<VkLayerInstanceCreateInfo*>(
            reinterpret_cast<const VkLayerInstanceCreateInfo*>(pCreateInfo->pNext));
        while (layerDesc && (layerDesc->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO
                || layerDesc->function != VK_LAYER_LINK_INFO)) {
            layerDesc = const_cast<VkLayerInstanceCreateInfo*>(
                reinterpret_cast<const VkLayerInstanceCreateInfo*>(layerDesc->pNext));
        }
        if (!layerDesc)
            throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
                "No layer creation info found in pNext chain");

        const auto downstreamGipa = layerDesc->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        layerDesc->u.pLayerInfo = layerDesc->u.pLayerInfo->pNext;

        const auto* appInfo = pCreateInfo ? pCreateInfo->pApplicationInfo : nullptr;
        const bool isPrivateFramegenInstance = appInfo
            && appInfo->pApplicationName
            && appInfo->pEngineName
            && std::strcmp(appInfo->pApplicationName, "lsfg-vk-base") == 0
            && std::strcmp(appInfo->pEngineName, "lsfg-vk-base") == 0;
        const auto downstreamCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
            downstreamGipa(nullptr, "vkCreateInstance"));
        if (!downstreamCreateInstance)
            throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
                "Failed to get instance function pointer for vkCreateInstance");

        // LsContext owns a private Vulkan instance for the frame-generation
        // backend. GameNative force-enables this layer, so that private
        // vkCreateInstance re-enters us. It must not replace the game
        // instance's process-global compatibility dispatch or run the
        // active game-instance hook a second time.
        if (isPrivateFramegenInstance) {
            const auto res = downstreamCreateInstance(pCreateInfo, pAllocator, pInstance);
            if (res == VK_SUCCESS && pInstance && *pInstance != VK_NULL_HANDLE)
                storePrivateInstanceDispatch(*pInstance, downstreamGipa);
            return res;
        }

        next_vkGetInstanceProcAddr = downstreamGipa;
        next_vkCreateInstance = downstreamCreateInstance;

        if (!Config::snapshot().enable) {
            auto res = next_vkCreateInstance(pCreateInfo, pAllocator, pInstance);
            if (res == VK_SUCCESS)
                initInstanceFunc(*pInstance, "vkCreateDevice", &next_vkCreateDevice);
            return res;
        }

        auto* createInstanceHook = reinterpret_cast<PFN_vkCreateInstance>(
            Hooks::hooks["vkCreateInstance"]);
        auto res = createInstanceHook(pCreateInfo, pAllocator, pInstance);
        if (res != VK_SUCCESS)
            throw LSFG::vulkan_error(res, "Failed to create Vulkan instance");

        bool success = true;
        success &= initInstanceFunc(*pInstance, "vkDestroyInstance", &next_vkDestroyInstance);
        success &= initInstanceFunc(*pInstance, "vkCreateDevice", &next_vkCreateDevice);
        success &= initInstanceFunc(*pInstance, "vkGetPhysicalDeviceQueueFamilyProperties", &next_vkGetPhysicalDeviceQueueFamilyProperties);
        success &= initInstanceFunc(*pInstance, "vkGetPhysicalDeviceMemoryProperties", &next_vkGetPhysicalDeviceMemoryProperties);
        success &= initInstanceFunc(*pInstance, "vkGetPhysicalDeviceProperties", &next_vkGetPhysicalDeviceProperties);
        success &= initInstanceFunc(*pInstance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", &next_vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
        if (!success)
            throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
                "Failed to get instance function pointers");

        return VK_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "lsfg-vk: An error occurred while initializing the Vulkan instance layer:\n- "
                  << e.what() << '\n';
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

VkResult layer_vkCreateDevice(
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDevice* pDevice) {
    try {
        auto* layerDesc = const_cast<VkLayerDeviceCreateInfo*>(
            reinterpret_cast<const VkLayerDeviceCreateInfo*>(pCreateInfo->pNext));
        while (layerDesc && (layerDesc->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
                || layerDesc->function != VK_LAYER_LINK_INFO)) {
            layerDesc = const_cast<VkLayerDeviceCreateInfo*>(
                reinterpret_cast<const VkLayerDeviceCreateInfo*>(layerDesc->pNext));
        }
        if (!layerDesc)
            throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
                "No layer creation info found in pNext chain");

        DeviceDispatch dispatch{};
        dispatch.GetDeviceProcAddr = layerDesc->u.pLayerInfo->pfnNextGetDeviceProcAddr;
        // Used only while the loader is resolving this construction thread.
        DeviceConstructionScope construction(dispatch.GetDeviceProcAddr);
        layerDesc->u.pLayerInfo = layerDesc->u.pLayerInfo->pNext;

        auto* loaderData = const_cast<VkLayerDeviceCreateInfo*>(
            reinterpret_cast<const VkLayerDeviceCreateInfo*>(pCreateInfo->pNext));
        while (loaderData && (loaderData->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
                || loaderData->function != VK_LOADER_DATA_CALLBACK)) {
            loaderData = const_cast<VkLayerDeviceCreateInfo*>(
                reinterpret_cast<const VkLayerDeviceCreateInfo*>(loaderData->pNext));
        }
        if (!loaderData)
            throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
                "No layer device loader data found in pNext chain");
        dispatch.SetDeviceLoaderData = loaderData->u.pfnSetDeviceLoaderData;

        if (!Config::snapshot().enable) {
            auto res = next_vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
            if (res == VK_SUCCESS) registerPassthroughDevice(*pDevice, dispatch);
            return res;
        }

        // vkGetDeviceProcAddr is required to return NULL for extension commands
        // that were not enabled on this logical device. GameNative/Proton creates
        // helper Vulkan devices that do not enable VK_KHR_swapchain. Those devices
        // can never present frames and therefore must not be treated as an LSFG
        // capability failure.
        if (!deviceExtensionEnabled(pCreateInfo, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            auto res = next_vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
            if (res == VK_SUCCESS) {
                registerPassthroughDevice(*pDevice, dispatch);
            }
            return res;
        }

        auto* createDeviceHook = reinterpret_cast<PFN_vkCreateDevice>(
            Hooks::hooks["vkCreateDevicePre"]);
        auto res = createDeviceHook(physicalDevice, pCreateInfo, pAllocator, pDevice);
        if (res != VK_SUCCESS)
            throw LSFG::vulkan_error(res, "Failed to create Vulkan device");

        bool success = true;
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkDestroyDevice", &dispatch.DestroyDevice);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkCreateSwapchainKHR", &dispatch.CreateSwapchainKHR);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkQueuePresentKHR", &dispatch.QueuePresentKHR);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkDestroySwapchainKHR", &dispatch.DestroySwapchainKHR);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkGetSwapchainImagesKHR", &dispatch.GetSwapchainImagesKHR);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkAllocateCommandBuffers", &dispatch.AllocateCommandBuffers);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkFreeCommandBuffers", &dispatch.FreeCommandBuffers);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkBeginCommandBuffer", &dispatch.BeginCommandBuffer);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkEndCommandBuffer", &dispatch.EndCommandBuffer);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkCreateCommandPool", &dispatch.CreateCommandPool);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkDestroyCommandPool", &dispatch.DestroyCommandPool);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkCreateImage", &dispatch.CreateImage);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkDestroyImage", &dispatch.DestroyImage);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkGetImageMemoryRequirements", &dispatch.GetImageMemoryRequirements);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkBindImageMemory", &dispatch.BindImageMemory);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkAllocateMemory", &dispatch.AllocateMemory);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkFreeMemory", &dispatch.FreeMemory);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkCreateSemaphore", &dispatch.CreateSemaphore);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkDestroySemaphore", &dispatch.DestroySemaphore);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkGetDeviceQueue", &dispatch.GetDeviceQueue);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkQueueSubmit", &dispatch.QueueSubmit);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkCmdPipelineBarrier", &dispatch.CmdPipelineBarrier);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkCmdBlitImage", &dispatch.CmdBlitImage);
        success &= initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkAcquireNextImageKHR", &dispatch.AcquireNextImageKHR);

        // Desktop FD export is not part of the Android AHardwareBuffer exchange.
        // Keep these pointers when the driver exposes them, but never reject an
        // otherwise valid Android presentation device because they are absent.
        initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkGetMemoryFdKHR", &dispatch.GetMemoryFdKHR, false);
        initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkGetSemaphoreFdKHR", &dispatch.GetSemaphoreFdKHR, false);
        initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr, "vkGetAndroidHardwareBufferPropertiesANDROID",
            &dispatch.GetAndroidHardwareBufferPropertiesANDROID, false);

        if (!success)
            throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
                "Failed to get required device function pointers");

        initDeviceFunc(*pDevice, dispatch.GetDeviceProcAddr,
            "vkGetDeviceQueue2", &dispatch.GetDeviceQueue2, false);
        dispatch.presentationDevice = true;
        storeDeviceDispatch(*pDevice, dispatch);

        auto* postCreateDeviceHook = reinterpret_cast<PFN_vkCreateDevice>(
            Hooks::hooks["vkCreateDevicePost"]);
        res = postCreateDeviceHook(physicalDevice,
            const_cast<VkDeviceCreateInfo*>(pCreateInfo), pAllocator, pDevice);
        if (res != VK_SUCCESS) {
            eraseDeviceDispatch(*pDevice);
            throw LSFG::vulkan_error(res, "Failed to register Vulkan device");
        }

        return VK_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "lsfg-vk: An error occurred while initializing the Vulkan device layer:\n- "
                  << e.what() << '\n';
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}
} // namespace

const std::unordered_map<std::string, PFN_vkVoidFunction> layerFunctions = {
    {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(&layer_vkCreateInstance)},
    {"vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(&layer_vkCreateDevice)},
    {"vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(&layer_vkGetInstanceProcAddr)},
    {"vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(&layer_vkGetDeviceProcAddr)},
    // Track every application-created queue so a present queue is resolved by
    // its exact logical-device owner, even when two devices share a loader
    // dispatch pointer.
    {"vkGetDeviceQueue", reinterpret_cast<PFN_vkVoidFunction>(&Layer::ovkGetDeviceQueue)},
    {"vkGetDeviceQueue2", reinterpret_cast<PFN_vkVoidFunction>(&Layer::ovkGetDeviceQueue2)},
};

PFN_vkVoidFunction layer_vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    const std::string name(pName);
    PrivateInstanceDispatch privateDispatch{};
    if (instance != VK_NULL_HANDLE
            && loadPrivateInstanceDispatch(instance, &privateDispatch)) {
        if (name == "vkDestroyInstance")
            return reinterpret_cast<PFN_vkVoidFunction>(&layer_vkDestroyPrivateInstance);
        return privateDispatch.GetInstanceProcAddr
            ? privateDispatch.GetInstanceProcAddr(instance, pName)
            : nullptr;
    }

    if (name == "vkDestroyDevice") return Hooks::hooks.at(name);
    auto it = layerFunctions.find(name);
    if (it != layerFunctions.end()) return it->second;
    it = Hooks::hooks.find(name);
    if (it != Hooks::hooks.end() && Config::snapshot().enable) {
        return it->second;
    }
    return next_vkGetInstanceProcAddr(instance, pName);
}

PFN_vkVoidFunction layer_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    const std::string name(pName);
    if (name == "vkDestroyDevice") return Hooks::hooks.at(name);
    if (name == "vkGetDeviceQueue2") {
        DeviceDispatch owner{};
        if (loadDeviceDispatch(device, &owner))
            return owner.GetDeviceQueue2
                ? reinterpret_cast<PFN_vkVoidFunction>(&Layer::ovkGetDeviceQueue2) : nullptr;
        return next_vkGetDeviceProcAddr && next_vkGetDeviceProcAddr(device, pName)
            ? reinterpret_cast<PFN_vkVoidFunction>(&Layer::ovkGetDeviceQueue2) : nullptr;
    }
    auto it = layerFunctions.find(name);
    if (it != layerFunctions.end()) return it->second;

    DeviceDispatch dispatch{};
    const bool tracked = loadDeviceDispatch(device, &dispatch);
    PFN_vkGetDeviceProcAddr downstream =
        tracked && dispatch.GetDeviceProcAddr ? dispatch.GetDeviceProcAddr : next_vkGetDeviceProcAddr;

    it = Hooks::hooks.find(name);
    if (it != Hooks::hooks.end() && Config::snapshot().enable) {
        // The loader may query GDPA while it is still constructing a logical
        // device's dispatch table, before storeDeviceDispatch() has published
        // our per-device snapshot.  Do not hand it a permanent downstream WSI
        // bypass in that window.  The exact device's downstream GDPA remains
        // the capability gate: helper devices without VK_KHR_swapchain return
        // NULL here, while presentation devices retain LSFG interception.
        if (!tracked && isDeviceWsiHook(name)) {
            if (!downstream || !downstream(device, pName))
                return nullptr;
            return it->second;
        }

        // Never advertise LSFG hooks on a helper device once its dispatch
        // identity is known, and never advertise an extension command that
        // the next entity reports as unavailable for this exact device.
        if (!tracked || (!dispatch.presentationDevice && name != "vkDestroyDevice"))
            return downstream ? downstream(device, pName) : nullptr;
        if (!downstream || !downstream(device, pName))
            return nullptr;
        return it->second;
    }
    return downstream ? downstream(device, pName) : nullptr;
}

namespace Layer {
VkResult ovkCreateInstance(const VkInstanceCreateInfo* a, const VkAllocationCallbacks* b, VkInstance* c) { return next_vkCreateInstance(a, b, c); }
void ovkDestroyInstance(VkInstance a, const VkAllocationCallbacks* b) { next_vkDestroyInstance(a, b); }
VkResult ovkCreateDevice(VkPhysicalDevice a, const VkDeviceCreateInfo* b, const VkAllocationCallbacks* c, VkDevice* d) { return next_vkCreateDevice(a, b, c, d); }
void ovkDestroyDevice(VkDevice a, const VkAllocationCallbacks* b) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.DestroyDevice)
        dispatch.DestroyDevice(a, b);
    else
        return;
    eraseDeviceDispatch(a);
}
VkResult ovkSetDeviceLoaderData(VkDevice a, void* b) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.SetDeviceLoaderData)
        return dispatch.SetDeviceLoaderData(a, b);
    return VK_ERROR_DEVICE_LOST;
}
PFN_vkVoidFunction ovkGetInstanceProcAddr(VkInstance a, const char* b) { return next_vkGetInstanceProcAddr(a, b); }
PFN_vkVoidFunction ovkGetDeviceProcAddr(VkDevice a, const char* b) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.GetDeviceProcAddr)
        return dispatch.GetDeviceProcAddr(a, b);
    return nullptr;
}
void ovkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice a, uint32_t* b, VkQueueFamilyProperties* c) { next_vkGetPhysicalDeviceQueueFamilyProperties(a, b, c); }
void ovkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice a, VkPhysicalDeviceMemoryProperties* b) { next_vkGetPhysicalDeviceMemoryProperties(a, b); }
void ovkGetPhysicalDeviceProperties(VkPhysicalDevice a, VkPhysicalDeviceProperties* b) { next_vkGetPhysicalDeviceProperties(a, b); }
VkResult ovkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice a, VkSurfaceKHR b, VkSurfaceCapabilitiesKHR* c) { return next_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(a, b, c); }
VkResult ovkCreateSwapchainKHR(VkDevice a, const VkSwapchainCreateInfoKHR* b, const VkAllocationCallbacks* c, VkSwapchainKHR* d) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.CreateSwapchainKHR)
        return dispatch.CreateSwapchainKHR(a, b, c, d);
    return VK_ERROR_DEVICE_LOST;
}
VkResult ovkQueuePresentKHR(VkQueue a, const VkPresentInfoKHR* b) {
    DeviceDispatch dispatch{};
    if (loadQueueDispatch(a, &dispatch) && dispatch.QueuePresentKHR)
        return dispatch.QueuePresentKHR(a, b);
    std::cerr << "lsfg-vk: dispatch rejected present queue=" << a << " owner=unknown-or-unavailable\n";
    return VK_ERROR_DEVICE_LOST;
}
void ovkDestroySwapchainKHR(VkDevice a, VkSwapchainKHR b, const VkAllocationCallbacks* c) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.DestroySwapchainKHR) {
        dispatch.DestroySwapchainKHR(a, b, c);
        return;
    }
    return;
}
VkResult ovkGetSwapchainImagesKHR(VkDevice a, VkSwapchainKHR b, uint32_t* c, VkImage* d) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.GetSwapchainImagesKHR)
        return dispatch.GetSwapchainImagesKHR(a, b, c, d);
    return VK_ERROR_DEVICE_LOST;
}
VkResult ovkAllocateCommandBuffers(VkDevice a, const VkCommandBufferAllocateInfo* b, VkCommandBuffer* c) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.AllocateCommandBuffers) {
        const auto result = dispatch.AllocateCommandBuffers(a, b, c);
        if (result == VK_SUCCESS) {
            for (uint32_t i = 0; i < b->commandBufferCount; ++i)
                storeCommandBufferDispatch(c[i], dispatch);
        }
        return result;
    }
    return VK_ERROR_DEVICE_LOST;
}
void ovkFreeCommandBuffers(VkDevice a, VkCommandPool b, uint32_t c, const VkCommandBuffer* d) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.FreeCommandBuffers) {
        dispatch.FreeCommandBuffers(a, b, c, d);
        std::unique_lock lock(deviceDispatchMutex);
        for (uint32_t i = 0; i < c; ++i)
            commandBufferDispatchTables.erase(d[i]);
        return;
    }
    return;
}
VkResult ovkBeginCommandBuffer(VkCommandBuffer a, const VkCommandBufferBeginInfo* b) {
    DeviceDispatch dispatch{};
    if (loadCommandBufferDispatch(a, &dispatch) && dispatch.BeginCommandBuffer)
        return dispatch.BeginCommandBuffer(a, b);
    return VK_ERROR_DEVICE_LOST;
}
VkResult ovkEndCommandBuffer(VkCommandBuffer a) {
    DeviceDispatch dispatch{};
    if (loadCommandBufferDispatch(a, &dispatch) && dispatch.EndCommandBuffer)
        return dispatch.EndCommandBuffer(a);
    return VK_ERROR_DEVICE_LOST;
}
VkResult ovkCreateCommandPool(VkDevice a, const VkCommandPoolCreateInfo* b, const VkAllocationCallbacks* c, VkCommandPool* d) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.CreateCommandPool)
        return dispatch.CreateCommandPool(a, b, c, d);
    return VK_ERROR_DEVICE_LOST;
}
void ovkDestroyCommandPool(VkDevice a, VkCommandPool b, const VkAllocationCallbacks* c) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.DestroyCommandPool) {
        dispatch.DestroyCommandPool(a, b, c);
        return;
    }
    return;
}
VkResult ovkCreateImage(VkDevice a, const VkImageCreateInfo* b, const VkAllocationCallbacks* c, VkImage* d) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.CreateImage)
        return dispatch.CreateImage(a, b, c, d);
    return VK_ERROR_DEVICE_LOST;
}
void ovkDestroyImage(VkDevice a, VkImage b, const VkAllocationCallbacks* c) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.DestroyImage) {
        dispatch.DestroyImage(a, b, c);
        return;
    }
    return;
}
void ovkGetImageMemoryRequirements(VkDevice a, VkImage b, VkMemoryRequirements* c) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.GetImageMemoryRequirements) {
        dispatch.GetImageMemoryRequirements(a, b, c);
        return;
    }
    return;
}
VkResult ovkBindImageMemory(VkDevice a, VkImage b, VkDeviceMemory c, VkDeviceSize d) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.BindImageMemory)
        return dispatch.BindImageMemory(a, b, c, d);
    return VK_ERROR_DEVICE_LOST;
}
VkResult ovkAllocateMemory(VkDevice a, const VkMemoryAllocateInfo* b, const VkAllocationCallbacks* c, VkDeviceMemory* d) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.AllocateMemory)
        return dispatch.AllocateMemory(a, b, c, d);
    return VK_ERROR_DEVICE_LOST;
}
void ovkFreeMemory(VkDevice a, VkDeviceMemory b, const VkAllocationCallbacks* c) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.FreeMemory) {
        dispatch.FreeMemory(a, b, c);
        return;
    }
    return;
}
VkResult ovkCreateSemaphore(VkDevice a, const VkSemaphoreCreateInfo* b, const VkAllocationCallbacks* c, VkSemaphore* d) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.CreateSemaphore)
        return dispatch.CreateSemaphore(a, b, c, d);
    return VK_ERROR_DEVICE_LOST;
}
void ovkDestroySemaphore(VkDevice a, VkSemaphore b, const VkAllocationCallbacks* c) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.DestroySemaphore) {
        dispatch.DestroySemaphore(a, b, c);
        return;
    }
    return;
}
VkResult ovkGetMemoryFdKHR(VkDevice a, const VkMemoryGetFdInfoKHR* b, int* c) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch))
        return dispatch.GetMemoryFdKHR ? dispatch.GetMemoryFdKHR(a, b, c) : VK_ERROR_EXTENSION_NOT_PRESENT;
    return VK_ERROR_DEVICE_LOST;
}
VkResult ovkGetSemaphoreFdKHR(VkDevice a, const VkSemaphoreGetFdInfoKHR* b, int* c) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch))
        return dispatch.GetSemaphoreFdKHR ? dispatch.GetSemaphoreFdKHR(a, b, c) : VK_ERROR_EXTENSION_NOT_PRESENT;
    return VK_ERROR_DEVICE_LOST;
}
VkResult ovkGetAndroidHardwareBufferPropertiesANDROID(VkDevice a, const AHardwareBuffer* b, VkAndroidHardwareBufferPropertiesANDROID* c) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch))
        return dispatch.GetAndroidHardwareBufferPropertiesANDROID
            ? dispatch.GetAndroidHardwareBufferPropertiesANDROID(a, b, c)
            : VK_ERROR_EXTENSION_NOT_PRESENT;
    return VK_ERROR_DEVICE_LOST;
}
void ovkGetDeviceQueue(VkDevice a, uint32_t b, uint32_t c, VkQueue* d) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.GetDeviceQueue) {
        dispatch.GetDeviceQueue(a, b, c, d);
        storeQueueDispatch(*d, dispatch);
        return;
    }
    if (d) *d = VK_NULL_HANDLE;
    std::cerr << "lsfg-vk: dispatch rejected queue acquisition device=" << a << "\n";
}
void ovkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(device, &dispatch) && dispatch.GetDeviceQueue2) {
        dispatch.GetDeviceQueue2(device, info, queue);
        storeQueueDispatch(*queue, dispatch);
        return;
    }
    if (queue) *queue = VK_NULL_HANDLE;
    std::cerr << "lsfg-vk: dispatch rejected queue2 acquisition device=" << device << "\n";
}
VkDevice queueOwner(VkQueue queue) {
    DeviceDispatch dispatch{};
    return loadQueueDispatch(queue, &dispatch) ? dispatch.device : VK_NULL_HANDLE;
}
VkResult ovkQueueSubmit(VkQueue a, uint32_t b, const VkSubmitInfo* c, VkFence d) {
    DeviceDispatch dispatch{};
    if (loadQueueDispatch(a, &dispatch) && dispatch.QueueSubmit)
        return dispatch.QueueSubmit(a, b, c, d);
    std::cerr << "lsfg-vk: dispatch rejected submit queue=" << a << " owner=unknown-or-unavailable\n";
    return VK_ERROR_DEVICE_LOST;
}
void ovkCmdPipelineBarrier(VkCommandBuffer a, VkPipelineStageFlags b, VkPipelineStageFlags c, VkDependencyFlags d, uint32_t e, const VkMemoryBarrier* f, uint32_t g, const VkBufferMemoryBarrier* h, uint32_t i, const VkImageMemoryBarrier* j) {
    DeviceDispatch dispatch{};
    if (loadCommandBufferDispatch(a, &dispatch) && dispatch.CmdPipelineBarrier) {
        dispatch.CmdPipelineBarrier(a, b, c, d, e, f, g, h, i, j);
        return;
    }
    return;
}
void ovkCmdBlitImage(VkCommandBuffer a, VkImage b, VkImageLayout c, VkImage d, VkImageLayout e, uint32_t f, const VkImageBlit* g, VkFilter h) {
    DeviceDispatch dispatch{};
    if (loadCommandBufferDispatch(a, &dispatch) && dispatch.CmdBlitImage) {
        dispatch.CmdBlitImage(a, b, c, d, e, f, g, h);
        return;
    }
    return;
}
VkResult ovkAcquireNextImageKHR(VkDevice a, VkSwapchainKHR b, uint64_t c, VkSemaphore d, VkFence e, uint32_t* f) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.AcquireNextImageKHR)
        return dispatch.AcquireNextImageKHR(a, b, c, d, e, f);
    return VK_ERROR_DEVICE_LOST;
}
} // namespace Layer

#endif // __ANDROID__

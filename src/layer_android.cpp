#ifdef __ANDROID__

#include "layer.hpp"
#include "common/exception.hpp"
#include "config/config.hpp"
#include "hooks.hpp"

#include <android/hardware_buffer.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan_core.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace {

bool shouldInterceptTarget() {
    // `targeted` is immutable process residency. Runtime `enable` may change
    // after the application has cached Vulkan WSI entrypoints, so it must not
    // decide whether those entrypoints are intercepted.
    return Config::activeConf.targeted || Config::activeConf.enable;
}

PFN_vkCreateInstance next_vkCreateInstance{};
PFN_vkDestroyInstance next_vkDestroyInstance{};
PFN_vkCreateDevice next_vkCreateDevice{};
PFN_vkDestroyDevice next_vkDestroyDevice{};
PFN_vkSetDeviceLoaderData next_vSetDeviceLoaderData{};
PFN_vkGetInstanceProcAddr next_vkGetInstanceProcAddr{};
PFN_vkGetDeviceProcAddr next_vkGetDeviceProcAddr{};
PFN_vkGetPhysicalDeviceQueueFamilyProperties next_vkGetPhysicalDeviceQueueFamilyProperties{};
PFN_vkGetPhysicalDeviceMemoryProperties next_vkGetPhysicalDeviceMemoryProperties{};
PFN_vkGetPhysicalDeviceProperties next_vkGetPhysicalDeviceProperties{};
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR next_vkGetPhysicalDeviceSurfaceCapabilitiesKHR{};
PFN_vkCreateSwapchainKHR next_vkCreateSwapchainKHR{};
PFN_vkQueuePresentKHR next_vkQueuePresentKHR{};
PFN_vkDestroySwapchainKHR next_vkDestroySwapchainKHR{};
PFN_vkGetSwapchainImagesKHR next_vkGetSwapchainImagesKHR{};
PFN_vkAllocateCommandBuffers next_vkAllocateCommandBuffers{};
PFN_vkFreeCommandBuffers next_vkFreeCommandBuffers{};
PFN_vkBeginCommandBuffer next_vkBeginCommandBuffer{};
PFN_vkEndCommandBuffer next_vkEndCommandBuffer{};
PFN_vkCreateCommandPool next_vkCreateCommandPool{};
PFN_vkDestroyCommandPool next_vkDestroyCommandPool{};
PFN_vkCreateImage next_vkCreateImage{};
PFN_vkDestroyImage next_vkDestroyImage{};
PFN_vkGetImageMemoryRequirements next_vkGetImageMemoryRequirements{};
PFN_vkBindImageMemory next_vkBindImageMemory{};
PFN_vkAllocateMemory next_vkAllocateMemory{};
PFN_vkFreeMemory next_vkFreeMemory{};
PFN_vkCreateSemaphore next_vkCreateSemaphore{};
PFN_vkDestroySemaphore next_vkDestroySemaphore{};
PFN_vkGetMemoryFdKHR next_vkGetMemoryFdKHR{};
PFN_vkGetSemaphoreFdKHR next_vkGetSemaphoreFdKHR{};
PFN_vkGetAndroidHardwareBufferPropertiesANDROID next_vkGetAndroidHardwareBufferPropertiesANDROID{};
PFN_vkGetDeviceQueue next_vkGetDeviceQueue{};
PFN_vkQueueSubmit next_vkQueueSubmit{};
PFN_vkCmdPipelineBarrier next_vkCmdPipelineBarrier{};
PFN_vkCmdBlitImage next_vkCmdBlitImage{};
PFN_vkAcquireNextImageKHR next_vkAcquireNextImageKHR{};

// The Vulkan loader contract is distributed-dispatch: device entry points
// returned by vkGetDeviceProcAddr belong to the queried logical device.  Some
// Android vendor wrappers return device-specific thunks, so sharing one global
// table across every logical device is not valid.  Key the table by the loader
// dispatch pointer stored in the first word of every device-level dispatchable
// handle; VkDevice, VkQueue and VkCommandBuffer from one logical device share
// that key.
struct DeviceDispatch {
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
    PFN_vkQueueSubmit QueueSubmit{};
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier{};
    PFN_vkCmdBlitImage CmdBlitImage{};
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR{};
    bool presentationDevice{false};
};

std::unordered_map<const void*, DeviceDispatch> deviceDispatchTables;
std::shared_mutex deviceDispatchMutex;

template <typename Handle>
const void* deviceDispatchKey(Handle handle) {
    if (handle == VK_NULL_HANDLE) return nullptr;
    return *reinterpret_cast<void* const*>(handle);
}

template <typename Handle>
bool loadDeviceDispatch(Handle handle, DeviceDispatch* dispatch) {
    const void* key = deviceDispatchKey(handle);
    if (!key || !dispatch) return false;
    std::shared_lock lock(deviceDispatchMutex);
    auto it = deviceDispatchTables.find(key);
    if (it == deviceDispatchTables.end()) return false;
    *dispatch = it->second;
    return true;
}

void storeDeviceDispatch(VkDevice device, const DeviceDispatch& dispatch) {
    const void* key = deviceDispatchKey(device);
    if (!key) return;
    std::unique_lock lock(deviceDispatchMutex);
    deviceDispatchTables[key] = dispatch;
}

void eraseDeviceDispatchKey(const void* key) {
    if (!key) return;
    std::unique_lock lock(deviceDispatchMutex);
    deviceDispatchTables.erase(key);
}

DeviceDispatch snapshotPresentationDispatch() {
    return DeviceDispatch {
        .GetDeviceProcAddr = next_vkGetDeviceProcAddr,
        .SetDeviceLoaderData = next_vSetDeviceLoaderData,
        .DestroyDevice = next_vkDestroyDevice,
        .CreateSwapchainKHR = next_vkCreateSwapchainKHR,
        .QueuePresentKHR = next_vkQueuePresentKHR,
        .DestroySwapchainKHR = next_vkDestroySwapchainKHR,
        .GetSwapchainImagesKHR = next_vkGetSwapchainImagesKHR,
        .AllocateCommandBuffers = next_vkAllocateCommandBuffers,
        .FreeCommandBuffers = next_vkFreeCommandBuffers,
        .BeginCommandBuffer = next_vkBeginCommandBuffer,
        .EndCommandBuffer = next_vkEndCommandBuffer,
        .CreateCommandPool = next_vkCreateCommandPool,
        .DestroyCommandPool = next_vkDestroyCommandPool,
        .CreateImage = next_vkCreateImage,
        .DestroyImage = next_vkDestroyImage,
        .GetImageMemoryRequirements = next_vkGetImageMemoryRequirements,
        .BindImageMemory = next_vkBindImageMemory,
        .AllocateMemory = next_vkAllocateMemory,
        .FreeMemory = next_vkFreeMemory,
        .CreateSemaphore = next_vkCreateSemaphore,
        .DestroySemaphore = next_vkDestroySemaphore,
        .GetMemoryFdKHR = next_vkGetMemoryFdKHR,
        .GetSemaphoreFdKHR = next_vkGetSemaphoreFdKHR,
        .GetAndroidHardwareBufferPropertiesANDROID = next_vkGetAndroidHardwareBufferPropertiesANDROID,
        .GetDeviceQueue = next_vkGetDeviceQueue,
        .QueueSubmit = next_vkQueueSubmit,
        .CmdPipelineBarrier = next_vkCmdPipelineBarrier,
        .CmdBlitImage = next_vkCmdBlitImage,
        .AcquireNextImageKHR = next_vkAcquireNextImageKHR,
        .presentationDevice = true,
    };
}

void registerPassthroughDevice(VkDevice device) {
    DeviceDispatch dispatch{};
    dispatch.GetDeviceProcAddr = next_vkGetDeviceProcAddr;
    dispatch.SetDeviceLoaderData = next_vSetDeviceLoaderData;
    if (dispatch.GetDeviceProcAddr) {
        dispatch.DestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(
            dispatch.GetDeviceProcAddr(device, "vkDestroyDevice"));
    }
    storeDeviceDispatch(device, dispatch);
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
bool initDeviceFunc(VkDevice device, const char* name, T* func, bool required = true) {
    *func = reinterpret_cast<T>(next_vkGetDeviceProcAddr(device, name));
    if (!*func) {
        std::cerr << (required ? "(no function pointer for " : "(optional function pointer unavailable for ")
                  << name << ")\n";
        return !required;
    }
    return true;
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

void logPresentationHookResolution(const char* resolver, const std::string& name) {
    if (name != "vkCreateSwapchainKHR" && name != "vkQueuePresentKHR") return;

    static std::atomic_bool gipaCreateLogged{false};
    static std::atomic_bool gipaPresentLogged{false};
    static std::atomic_bool gdpaCreateLogged{false};
    static std::atomic_bool gdpaPresentLogged{false};

    const bool isGipa = std::strcmp(resolver, "gipa") == 0;
    std::atomic_bool* logged = nullptr;
    if (name == "vkCreateSwapchainKHR")
        logged = isGipa ? &gipaCreateLogged : &gdpaCreateLogged;
    else
        logged = isGipa ? &gipaPresentLogged : &gdpaPresentLogged;

    if (!logged->exchange(true, std::memory_order_relaxed)) {
        std::cerr << "lsfg-vk: runtime stage=" << resolver
                  << "-hook-resolved name=" << name << "\n";
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

        next_vkGetInstanceProcAddr = layerDesc->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        layerDesc->u.pLayerInfo = layerDesc->u.pLayerInfo->pNext;

        if (!initInstanceFunc(nullptr, "vkCreateInstance", &next_vkCreateInstance))
            throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
                "Failed to get instance function pointer for vkCreateInstance");

        if (!shouldInterceptTarget()) {
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

        std::cerr << "lsfg-vk: Vulkan instance layer initialized successfully.\n";
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

        next_vkGetDeviceProcAddr = layerDesc->u.pLayerInfo->pfnNextGetDeviceProcAddr;
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
        next_vSetDeviceLoaderData = loaderData->u.pfnSetDeviceLoaderData;

        if (!shouldInterceptTarget()) {
            auto res = next_vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
            if (res == VK_SUCCESS) registerPassthroughDevice(*pDevice);
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
                registerPassthroughDevice(*pDevice);
                std::cerr << "lsfg-vk: Vulkan device has no VK_KHR_swapchain; passing through without frame generation.\n";
            }
            return res;
        }

        auto* createDeviceHook = reinterpret_cast<PFN_vkCreateDevice>(
            Hooks::hooks["vkCreateDevicePre"]);
        auto res = createDeviceHook(physicalDevice, pCreateInfo, pAllocator, pDevice);
        if (res != VK_SUCCESS)
            throw LSFG::vulkan_error(res, "Failed to create Vulkan device");

        bool success = true;
        success &= initDeviceFunc(*pDevice, "vkDestroyDevice", &next_vkDestroyDevice);
        success &= initDeviceFunc(*pDevice, "vkCreateSwapchainKHR", &next_vkCreateSwapchainKHR);
        success &= initDeviceFunc(*pDevice, "vkQueuePresentKHR", &next_vkQueuePresentKHR);
        success &= initDeviceFunc(*pDevice, "vkDestroySwapchainKHR", &next_vkDestroySwapchainKHR);
        success &= initDeviceFunc(*pDevice, "vkGetSwapchainImagesKHR", &next_vkGetSwapchainImagesKHR);
        success &= initDeviceFunc(*pDevice, "vkAllocateCommandBuffers", &next_vkAllocateCommandBuffers);
        success &= initDeviceFunc(*pDevice, "vkFreeCommandBuffers", &next_vkFreeCommandBuffers);
        success &= initDeviceFunc(*pDevice, "vkBeginCommandBuffer", &next_vkBeginCommandBuffer);
        success &= initDeviceFunc(*pDevice, "vkEndCommandBuffer", &next_vkEndCommandBuffer);
        success &= initDeviceFunc(*pDevice, "vkCreateCommandPool", &next_vkCreateCommandPool);
        success &= initDeviceFunc(*pDevice, "vkDestroyCommandPool", &next_vkDestroyCommandPool);
        success &= initDeviceFunc(*pDevice, "vkCreateImage", &next_vkCreateImage);
        success &= initDeviceFunc(*pDevice, "vkDestroyImage", &next_vkDestroyImage);
        success &= initDeviceFunc(*pDevice, "vkGetImageMemoryRequirements", &next_vkGetImageMemoryRequirements);
        success &= initDeviceFunc(*pDevice, "vkBindImageMemory", &next_vkBindImageMemory);
        success &= initDeviceFunc(*pDevice, "vkAllocateMemory", &next_vkAllocateMemory);
        success &= initDeviceFunc(*pDevice, "vkFreeMemory", &next_vkFreeMemory);
        success &= initDeviceFunc(*pDevice, "vkCreateSemaphore", &next_vkCreateSemaphore);
        success &= initDeviceFunc(*pDevice, "vkDestroySemaphore", &next_vkDestroySemaphore);
        success &= initDeviceFunc(*pDevice, "vkGetDeviceQueue", &next_vkGetDeviceQueue);
        success &= initDeviceFunc(*pDevice, "vkQueueSubmit", &next_vkQueueSubmit);
        success &= initDeviceFunc(*pDevice, "vkCmdPipelineBarrier", &next_vkCmdPipelineBarrier);
        success &= initDeviceFunc(*pDevice, "vkCmdBlitImage", &next_vkCmdBlitImage);
        success &= initDeviceFunc(*pDevice, "vkAcquireNextImageKHR", &next_vkAcquireNextImageKHR);

        // Desktop FD export is not part of the Android AHardwareBuffer exchange.
        // Keep these pointers when the driver exposes them, but never reject an
        // otherwise valid Android presentation device because they are absent.
        initDeviceFunc(*pDevice, "vkGetMemoryFdKHR", &next_vkGetMemoryFdKHR, false);
        initDeviceFunc(*pDevice, "vkGetSemaphoreFdKHR", &next_vkGetSemaphoreFdKHR, false);
        initDeviceFunc(*pDevice, "vkGetAndroidHardwareBufferPropertiesANDROID",
            &next_vkGetAndroidHardwareBufferPropertiesANDROID, false);

        if (!success)
            throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
                "Failed to get required device function pointers");

        // Snapshot the exact downstream function pointers returned for this
        // logical device before any later helper/presentation device can replace
        // the compatibility globals.  Register before the post hook because the
        // post hook obtains VkQueue and allocates device-level objects.
        storeDeviceDispatch(*pDevice, snapshotPresentationDispatch());

        auto* postCreateDeviceHook = reinterpret_cast<PFN_vkCreateDevice>(
            Hooks::hooks["vkCreateDevicePost"]);
        res = postCreateDeviceHook(physicalDevice,
            const_cast<VkDeviceCreateInfo*>(pCreateInfo), pAllocator, pDevice);
        if (res != VK_SUCCESS) {
            eraseDeviceDispatchKey(deviceDispatchKey(*pDevice));
            throw LSFG::vulkan_error(res, "Failed to register Vulkan device");
        }

        std::cerr << "lsfg-vk: runtime stage=device-dispatch-ready presentation=1\n";
        std::cerr << "lsfg-vk: Vulkan device layer initialized successfully.\n";
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
};

PFN_vkVoidFunction layer_vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    const std::string name(pName);
    auto it = layerFunctions.find(name);
    if (it != layerFunctions.end()) return it->second;
    it = Hooks::hooks.find(name);
    if (it != Hooks::hooks.end() && shouldInterceptTarget()) {
        logPresentationHookResolution("gipa", name);
        return it->second;
    }
    return next_vkGetInstanceProcAddr(instance, pName);
}

PFN_vkVoidFunction layer_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    const std::string name(pName);
    auto it = layerFunctions.find(name);
    if (it != layerFunctions.end()) return it->second;

    DeviceDispatch dispatch{};
    const bool tracked = loadDeviceDispatch(device, &dispatch);
    PFN_vkGetDeviceProcAddr downstream =
        tracked && dispatch.GetDeviceProcAddr ? dispatch.GetDeviceProcAddr : next_vkGetDeviceProcAddr;

    it = Hooks::hooks.find(name);
    if (it != Hooks::hooks.end() && shouldInterceptTarget()) {
        // The loader may query GDPA while it is still constructing a logical
        // device's dispatch table, before storeDeviceDispatch() has published
        // our per-device snapshot.  Do not hand it a permanent downstream WSI
        // bypass in that window.  The exact device's downstream GDPA remains
        // the capability gate: helper devices without VK_KHR_swapchain return
        // NULL here, while presentation devices retain LSFG interception.
        if (!tracked && isDeviceWsiHook(name)) {
            if (!downstream || !downstream(device, pName))
                return nullptr;
            std::cerr << "lsfg-vk: runtime stage=gdpa-untracked-wsi-hook-resolved name="
                      << name << "\n";
            return it->second;
        }

        // Never advertise LSFG hooks on a helper device once its dispatch
        // identity is known, and never advertise an extension command that
        // the next entity reports as unavailable for this exact device.
        if (!tracked || (!dispatch.presentationDevice && name != "vkDestroyDevice"))
            return downstream ? downstream(device, pName) : nullptr;
        if (!downstream || !downstream(device, pName))
            return nullptr;
        logPresentationHookResolution("gdpa", name);
        return it->second;
    }
    return downstream ? downstream(device, pName) : nullptr;
}

namespace Layer {
VkResult ovkCreateInstance(const VkInstanceCreateInfo* a, const VkAllocationCallbacks* b, VkInstance* c) { return next_vkCreateInstance(a, b, c); }
void ovkDestroyInstance(VkInstance a, const VkAllocationCallbacks* b) { next_vkDestroyInstance(a, b); }
VkResult ovkCreateDevice(VkPhysicalDevice a, const VkDeviceCreateInfo* b, const VkAllocationCallbacks* c, VkDevice* d) { return next_vkCreateDevice(a, b, c, d); }
void ovkDestroyDevice(VkDevice a, const VkAllocationCallbacks* b) {
    const void* key = deviceDispatchKey(a);
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.DestroyDevice)
        dispatch.DestroyDevice(a, b);
    else
        next_vkDestroyDevice(a, b);
    eraseDeviceDispatchKey(key);
}
VkResult ovkSetDeviceLoaderData(VkDevice a, void* b) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.SetDeviceLoaderData)
        return dispatch.SetDeviceLoaderData(a, b);
    return next_vSetDeviceLoaderData(a, b);
}
PFN_vkVoidFunction ovkGetInstanceProcAddr(VkInstance a, const char* b) { return next_vkGetInstanceProcAddr(a, b); }
PFN_vkVoidFunction ovkGetDeviceProcAddr(VkDevice a, const char* b) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.GetDeviceProcAddr)
        return dispatch.GetDeviceProcAddr(a, b);
    return next_vkGetDeviceProcAddr(a, b);
}
void ovkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice a, uint32_t* b, VkQueueFamilyProperties* c) { next_vkGetPhysicalDeviceQueueFamilyProperties(a, b, c); }
void ovkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice a, VkPhysicalDeviceMemoryProperties* b) { next_vkGetPhysicalDeviceMemoryProperties(a, b); }
void ovkGetPhysicalDeviceProperties(VkPhysicalDevice a, VkPhysicalDeviceProperties* b) { next_vkGetPhysicalDeviceProperties(a, b); }
VkResult ovkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice a, VkSurfaceKHR b, VkSurfaceCapabilitiesKHR* c) { return next_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(a, b, c); }
VkResult ovkCreateSwapchainKHR(VkDevice a, const VkSwapchainCreateInfoKHR* b, const VkAllocationCallbacks* c, VkSwapchainKHR* d) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.CreateSwapchainKHR)
        return dispatch.CreateSwapchainKHR(a, b, c, d);
    return next_vkCreateSwapchainKHR(a, b, c, d);
}
VkResult ovkQueuePresentKHR(VkQueue a, const VkPresentInfoKHR* b) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.QueuePresentKHR)
        return dispatch.QueuePresentKHR(a, b);
    return next_vkQueuePresentKHR(a, b);
}
void ovkDestroySwapchainKHR(VkDevice a, VkSwapchainKHR b, const VkAllocationCallbacks* c) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.DestroySwapchainKHR) {
        dispatch.DestroySwapchainKHR(a, b, c);
        return;
    }
    next_vkDestroySwapchainKHR(a, b, c);
}
VkResult ovkGetSwapchainImagesKHR(VkDevice a, VkSwapchainKHR b, uint32_t* c, VkImage* d) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.GetSwapchainImagesKHR)
        return dispatch.GetSwapchainImagesKHR(a, b, c, d);
    return next_vkGetSwapchainImagesKHR(a, b, c, d);
}
VkResult ovkAllocateCommandBuffers(VkDevice a, const VkCommandBufferAllocateInfo* b, VkCommandBuffer* c) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.AllocateCommandBuffers)
        return dispatch.AllocateCommandBuffers(a, b, c);
    return next_vkAllocateCommandBuffers(a, b, c);
}
void ovkFreeCommandBuffers(VkDevice a, VkCommandPool b, uint32_t c, const VkCommandBuffer* d) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.FreeCommandBuffers) {
        dispatch.FreeCommandBuffers(a, b, c, d);
        return;
    }
    next_vkFreeCommandBuffers(a, b, c, d);
}
VkResult ovkBeginCommandBuffer(VkCommandBuffer a, const VkCommandBufferBeginInfo* b) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.BeginCommandBuffer)
        return dispatch.BeginCommandBuffer(a, b);
    return next_vkBeginCommandBuffer(a, b);
}
VkResult ovkEndCommandBuffer(VkCommandBuffer a) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.EndCommandBuffer)
        return dispatch.EndCommandBuffer(a);
    return next_vkEndCommandBuffer(a);
}
VkResult ovkCreateCommandPool(VkDevice a, const VkCommandPoolCreateInfo* b, const VkAllocationCallbacks* c, VkCommandPool* d) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.CreateCommandPool)
        return dispatch.CreateCommandPool(a, b, c, d);
    return next_vkCreateCommandPool(a, b, c, d);
}
void ovkDestroyCommandPool(VkDevice a, VkCommandPool b, const VkAllocationCallbacks* c) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.DestroyCommandPool) {
        dispatch.DestroyCommandPool(a, b, c);
        return;
    }
    next_vkDestroyCommandPool(a, b, c);
}
VkResult ovkCreateImage(VkDevice a, const VkImageCreateInfo* b, const VkAllocationCallbacks* c, VkImage* d) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.CreateImage)
        return dispatch.CreateImage(a, b, c, d);
    return next_vkCreateImage(a, b, c, d);
}
void ovkDestroyImage(VkDevice a, VkImage b, const VkAllocationCallbacks* c) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.DestroyImage) {
        dispatch.DestroyImage(a, b, c);
        return;
    }
    next_vkDestroyImage(a, b, c);
}
void ovkGetImageMemoryRequirements(VkDevice a, VkImage b, VkMemoryRequirements* c) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.GetImageMemoryRequirements) {
        dispatch.GetImageMemoryRequirements(a, b, c);
        return;
    }
    next_vkGetImageMemoryRequirements(a, b, c);
}
VkResult ovkBindImageMemory(VkDevice a, VkImage b, VkDeviceMemory c, VkDeviceSize d) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.BindImageMemory)
        return dispatch.BindImageMemory(a, b, c, d);
    return next_vkBindImageMemory(a, b, c, d);
}
VkResult ovkAllocateMemory(VkDevice a, const VkMemoryAllocateInfo* b, const VkAllocationCallbacks* c, VkDeviceMemory* d) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.AllocateMemory)
        return dispatch.AllocateMemory(a, b, c, d);
    return next_vkAllocateMemory(a, b, c, d);
}
void ovkFreeMemory(VkDevice a, VkDeviceMemory b, const VkAllocationCallbacks* c) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.FreeMemory) {
        dispatch.FreeMemory(a, b, c);
        return;
    }
    next_vkFreeMemory(a, b, c);
}
VkResult ovkCreateSemaphore(VkDevice a, const VkSemaphoreCreateInfo* b, const VkAllocationCallbacks* c, VkSemaphore* d) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.CreateSemaphore)
        return dispatch.CreateSemaphore(a, b, c, d);
    return next_vkCreateSemaphore(a, b, c, d);
}
void ovkDestroySemaphore(VkDevice a, VkSemaphore b, const VkAllocationCallbacks* c) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.DestroySemaphore) {
        dispatch.DestroySemaphore(a, b, c);
        return;
    }
    next_vkDestroySemaphore(a, b, c);
}
VkResult ovkGetMemoryFdKHR(VkDevice a, const VkMemoryGetFdInfoKHR* b, int* c) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch))
        return dispatch.GetMemoryFdKHR ? dispatch.GetMemoryFdKHR(a, b, c) : VK_ERROR_EXTENSION_NOT_PRESENT;
    return next_vkGetMemoryFdKHR ? next_vkGetMemoryFdKHR(a, b, c) : VK_ERROR_EXTENSION_NOT_PRESENT;
}
VkResult ovkGetSemaphoreFdKHR(VkDevice a, const VkSemaphoreGetFdInfoKHR* b, int* c) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch))
        return dispatch.GetSemaphoreFdKHR ? dispatch.GetSemaphoreFdKHR(a, b, c) : VK_ERROR_EXTENSION_NOT_PRESENT;
    return next_vkGetSemaphoreFdKHR ? next_vkGetSemaphoreFdKHR(a, b, c) : VK_ERROR_EXTENSION_NOT_PRESENT;
}
VkResult ovkGetAndroidHardwareBufferPropertiesANDROID(VkDevice a, const AHardwareBuffer* b, VkAndroidHardwareBufferPropertiesANDROID* c) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch))
        return dispatch.GetAndroidHardwareBufferPropertiesANDROID
            ? dispatch.GetAndroidHardwareBufferPropertiesANDROID(a, b, c)
            : VK_ERROR_EXTENSION_NOT_PRESENT;
    return next_vkGetAndroidHardwareBufferPropertiesANDROID
        ? next_vkGetAndroidHardwareBufferPropertiesANDROID(a, b, c)
        : VK_ERROR_EXTENSION_NOT_PRESENT;
}
void ovkGetDeviceQueue(VkDevice a, uint32_t b, uint32_t c, VkQueue* d) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.GetDeviceQueue) {
        dispatch.GetDeviceQueue(a, b, c, d);
        return;
    }
    next_vkGetDeviceQueue(a, b, c, d);
}
VkResult ovkQueueSubmit(VkQueue a, uint32_t b, const VkSubmitInfo* c, VkFence d) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.QueueSubmit)
        return dispatch.QueueSubmit(a, b, c, d);
    return next_vkQueueSubmit(a, b, c, d);
}
void ovkCmdPipelineBarrier(VkCommandBuffer a, VkPipelineStageFlags b, VkPipelineStageFlags c, VkDependencyFlags d, uint32_t e, const VkMemoryBarrier* f, uint32_t g, const VkBufferMemoryBarrier* h, uint32_t i, const VkImageMemoryBarrier* j) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.CmdPipelineBarrier) {
        dispatch.CmdPipelineBarrier(a, b, c, d, e, f, g, h, i, j);
        return;
    }
    next_vkCmdPipelineBarrier(a, b, c, d, e, f, g, h, i, j);
}
void ovkCmdBlitImage(VkCommandBuffer a, VkImage b, VkImageLayout c, VkImage d, VkImageLayout e, uint32_t f, const VkImageBlit* g, VkFilter h) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.CmdBlitImage) {
        dispatch.CmdBlitImage(a, b, c, d, e, f, g, h);
        return;
    }
    next_vkCmdBlitImage(a, b, c, d, e, f, g, h);
}
VkResult ovkAcquireNextImageKHR(VkDevice a, VkSwapchainKHR b, uint64_t c, VkSemaphore d, VkFence e, uint32_t* f) {
    DeviceDispatch dispatch{};
    if (loadDeviceDispatch(a, &dispatch) && dispatch.AcquireNextImageKHR)
        return dispatch.AcquireNextImageKHR(a, b, c, d, e, f);
    return next_vkAcquireNextImageKHR(a, b, c, d, e, f);
}
} // namespace Layer

#endif // __ANDROID__

#include "layer.hpp"
#include "hooks.hpp"

#include <vulkan/vulkan_core.h>

#include <atomic>
#include <cstring>
#include <iostream>

#ifdef __ANDROID__
namespace {
std::atomic<PFN_vkCreateSwapchainKHR> diagnosticCreateSwapchainTarget{nullptr};
std::atomic<PFN_vkQueuePresentKHR> diagnosticQueuePresentTarget{nullptr};
std::atomic<bool> swapchainDispatchLogged{false};
std::atomic<bool> presentDispatchLogged{false};

VkResult diagnostic_vkCreateSwapchainKHR(
        VkDevice device,
        const VkSwapchainCreateInfoKHR* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSwapchainKHR* pSwapchain) {
    bool expected = false;
    if (swapchainDispatchLogged.compare_exchange_strong(expected, true))
        std::cerr << "lsfg-vk: runtime stage=swapchain-dispatch-enter\n";

    const auto target = diagnosticCreateSwapchainTarget.load(std::memory_order_acquire);
    if (target == nullptr)
        return VK_ERROR_INITIALIZATION_FAILED;
    return target(device, pCreateInfo, pAllocator, pSwapchain);
}

VkResult diagnostic_vkQueuePresentKHR(
        VkQueue queue,
        const VkPresentInfoKHR* pPresentInfo) {
    bool expected = false;
    if (presentDispatchLogged.compare_exchange_strong(expected, true))
        std::cerr << "lsfg-vk: runtime stage=present-hook-enter\n";

    const auto target = diagnosticQueuePresentTarget.load(std::memory_order_acquire);
    if (target == nullptr)
        return VK_ERROR_INITIALIZATION_FAILED;
    return target(queue, pPresentInfo);
}

PFN_vkVoidFunction wrapGipaWsiHook(const char* pName, PFN_vkVoidFunction resolved) {
    if (pName == nullptr || resolved == nullptr)
        return resolved;

    const auto swapchainHook = Hooks::hooks.find("vkCreateSwapchainKHR");
    if (swapchainHook != Hooks::hooks.end()
            && resolved == swapchainHook->second
            && std::strcmp(pName, "vkCreateSwapchainKHR") == 0) {
        diagnosticCreateSwapchainTarget.store(
            reinterpret_cast<PFN_vkCreateSwapchainKHR>(resolved), std::memory_order_release);
        std::cerr << "lsfg-vk: runtime stage=wsi-hook-resolved resolver=gipa command=vkCreateSwapchainKHR\n";
        return reinterpret_cast<PFN_vkVoidFunction>(&diagnostic_vkCreateSwapchainKHR);
    }

    const auto presentHook = Hooks::hooks.find("vkQueuePresentKHR");
    if (presentHook != Hooks::hooks.end()
            && resolved == presentHook->second
            && std::strcmp(pName, "vkQueuePresentKHR") == 0) {
        diagnosticQueuePresentTarget.store(
            reinterpret_cast<PFN_vkQueuePresentKHR>(resolved), std::memory_order_release);
        std::cerr << "lsfg-vk: runtime stage=wsi-hook-resolved resolver=gipa command=vkQueuePresentKHR\n";
        return reinterpret_cast<PFN_vkVoidFunction>(&diagnostic_vkQueuePresentKHR);
    }

    return resolved;
}

PFN_vkVoidFunction wrapGdpaWsiHook(const char* pName, PFN_vkVoidFunction resolved) {
    if (pName == nullptr || resolved == nullptr)
        return resolved;

    const auto swapchainHook = Hooks::hooks.find("vkCreateSwapchainKHR");
    if (swapchainHook != Hooks::hooks.end()
            && resolved == swapchainHook->second
            && std::strcmp(pName, "vkCreateSwapchainKHR") == 0) {
        diagnosticCreateSwapchainTarget.store(
            reinterpret_cast<PFN_vkCreateSwapchainKHR>(resolved), std::memory_order_release);
        std::cerr << "lsfg-vk: runtime stage=wsi-hook-resolved resolver=gdpa command=vkCreateSwapchainKHR\n";
        return reinterpret_cast<PFN_vkVoidFunction>(&diagnostic_vkCreateSwapchainKHR);
    }

    const auto presentHook = Hooks::hooks.find("vkQueuePresentKHR");
    if (presentHook != Hooks::hooks.end()
            && resolved == presentHook->second
            && std::strcmp(pName, "vkQueuePresentKHR") == 0) {
        diagnosticQueuePresentTarget.store(
            reinterpret_cast<PFN_vkQueuePresentKHR>(resolved), std::memory_order_release);
        std::cerr << "lsfg-vk: runtime stage=wsi-hook-resolved resolver=gdpa command=vkQueuePresentKHR\n";
        return reinterpret_cast<PFN_vkVoidFunction>(&diagnostic_vkQueuePresentKHR);
    }

    return resolved;
}
} // namespace
#endif

extern "C" __attribute__((visibility("default")))
PFN_vkVoidFunction lsfg_vkGetInstanceProcAddrDiagnostic(VkInstance instance, const char* pName) {
    const auto resolved = layer_vkGetInstanceProcAddr(instance, pName);
#ifdef __ANDROID__
    return wrapGipaWsiHook(pName, resolved);
#else
    return resolved;
#endif
}

extern "C" __attribute__((visibility("default")))
PFN_vkVoidFunction lsfg_vkGetDeviceProcAddrDiagnostic(VkDevice device, const char* pName) {
    const auto resolved = layer_vkGetDeviceProcAddr(device, pName);
#ifdef __ANDROID__
    return wrapGdpaWsiHook(pName, resolved);
#else
    return resolved;
#endif
}

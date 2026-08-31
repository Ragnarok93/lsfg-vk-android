#include "layer.hpp"
#include "hooks.hpp"

#include <vulkan/vulkan_core.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>

#ifdef __ANDROID__
#include <dlfcn.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifdef __ANDROID__
extern "C" PFN_vkVoidFunction lsfg_layer_vkGetInstanceProcAddr_impl(
    VkInstance instance, const char* pName);
extern "C" PFN_vkVoidFunction lsfg_layer_vkGetDeviceProcAddr_impl(
    VkDevice device, const char* pName);

namespace {
std::atomic<PFN_vkCreateSwapchainKHR> diagnosticCreateSwapchainTarget{nullptr};
std::atomic<PFN_vkQueuePresentKHR> diagnosticQueuePresentTarget{nullptr};
std::atomic<PFN_vkDestroySwapchainKHR> diagnosticDestroySwapchainTarget{nullptr};
std::atomic<uint64_t> presentSequence{0};
std::mutex provenanceMutex;
std::unordered_set<std::string> acquisitionKeys;

long currentTid() {
    return static_cast<long>(syscall(SYS_gettid));
}

std::string processCmdline() {
    std::ifstream in("/proc/self/cmdline", std::ios::binary);
    std::string value((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (char& c : value) {
        if (c == '\0') c = ' ';
    }
    while (!value.empty() && value.back() == ' ') value.pop_back();
    return value.empty() ? std::string("unknown") : value;
}

template <typename Handle>
const void* dispatchKey(Handle handle) {
    if (handle == VK_NULL_HANDLE) return nullptr;
    return *reinterpret_cast<void* const*>(handle);
}

template <typename Fn>
uintptr_t pointerValue(Fn fn) {
    return reinterpret_cast<uintptr_t>(fn);
}

std::string moduleFor(uintptr_t address) {
    if (address == 0) return "null";
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(address), &info) != 0 && info.dli_fname)
        return info.dli_fname;
    return "unknown";
}

template <typename Fn>
std::string moduleFor(Fn fn) {
    return moduleFor(pointerValue(fn));
}

bool shouldLogAcquisition(const char* resolver, const char* command, const void* key) {
    std::ostringstream id;
    id << resolver << ':' << command << ':' << key;
    std::lock_guard lock(provenanceMutex);
    return acquisitionKeys.insert(id.str()).second;
}

void logAcquire(const char* resolver, const char* command, VkDevice device,
        PFN_vkVoidFunction returned, PFN_vkVoidFunction target,
        PFN_vkVoidFunction downstream) {
    const void* key = dispatchKey(device);
    if (!shouldLogAcquisition(resolver, command, key)) return;
    std::cerr << "LSFG_PROVENANCE acquire"
              << " source=" << resolver
              << " name=" << command
              << " device=" << device
              << " dispatchKey=" << key
              << " returned=0x" << std::hex << pointerValue(returned)
              << " returnedModule=" << moduleFor(returned)
              << " target=0x" << pointerValue(target)
              << " targetModule=" << moduleFor(target)
              << " downstream=0x" << pointerValue(downstream)
              << " downstreamModule=" << moduleFor(downstream) << std::dec
              << " exe=" << processCmdline()
              << " pid=" << getpid()
              << " tid=" << currentTid()
              << '\n';
}

void logAcquireGipa(const char* command, PFN_vkVoidFunction returned,
        PFN_vkVoidFunction target, PFN_vkVoidFunction downstream) {
    if (!shouldLogAcquisition("gipa", command, nullptr)) return;
    std::cerr << "LSFG_PROVENANCE acquire"
              << " source=gipa"
              << " name=" << command
              << " device=0"
              << " dispatchKey=0"
              << " returned=0x" << std::hex << pointerValue(returned)
              << " returnedModule=" << moduleFor(returned)
              << " target=0x" << pointerValue(target)
              << " targetModule=" << moduleFor(target)
              << " downstream=0x" << pointerValue(downstream)
              << " downstreamModule=" << moduleFor(downstream) << std::dec
              << " exe=" << processCmdline()
              << " pid=" << getpid()
              << " tid=" << currentTid()
              << '\n';
}

VkResult diagnostic_vkCreateSwapchainKHR(
        VkDevice device,
        const VkSwapchainCreateInfoKHR* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSwapchainKHR* pSwapchain) {
    const auto target = diagnosticCreateSwapchainTarget.load(std::memory_order_acquire);
    std::cerr << "LSFG_PROVENANCE invoke name=vkCreateSwapchainKHR"
              << " device=" << device
              << " dispatchKey=" << dispatchKey(device)
              << " target=0x" << std::hex << pointerValue(target)
              << " module=" << moduleFor(target) << std::dec
              << " exe=" << processCmdline()
              << " pid=" << getpid()
              << " tid=" << currentTid() << '\n';

    if (pCreateInfo) {
        std::cerr << "LSFG_PROVENANCE swapchain-create"
                  << " surface=" << pCreateInfo->surface
                  << " minImages=" << pCreateInfo->minImageCount
                  << " extent=" << pCreateInfo->imageExtent.width << 'x' << pCreateInfo->imageExtent.height
                  << " format=" << pCreateInfo->imageFormat
                  << " colorSpace=" << pCreateInfo->imageColorSpace
                  << " usage=0x" << std::hex << pCreateInfo->imageUsage << std::dec
                  << " sharing=" << pCreateInfo->imageSharingMode
                  << " presentMode=" << pCreateInfo->presentMode
                  << " oldSwapchain=" << pCreateInfo->oldSwapchain
                  << '\n';
    }

    if (target == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = target(device, pCreateInfo, pAllocator, pSwapchain);
    std::cerr << "LSFG_PROVENANCE swapchain-create-result"
              << " result=" << result
              << " swapchain=" << ((pSwapchain && result == VK_SUCCESS) ? *pSwapchain : VK_NULL_HANDLE)
              << '\n';
    return result;
}

VkResult diagnostic_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    const auto target = diagnosticQueuePresentTarget.load(std::memory_order_acquire);
    const uint64_t sequence = presentSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool detailed = sequence <= 12 || (sequence % 120) == 0;

    if (sequence == 1) {
        std::cerr << "LSFG_PROVENANCE invoke name=vkQueuePresentKHR"
                  << " queue=" << queue
                  << " dispatchKey=" << dispatchKey(queue)
                  << " target=0x" << std::hex << pointerValue(target)
                  << " module=" << moduleFor(target) << std::dec
                  << " exe=" << processCmdline()
                  << " pid=" << getpid()
                  << " tid=" << currentTid() << '\n';
    }
    if (detailed) {
        std::cerr << "LSFG_PROVENANCE present"
                  << " seq=" << sequence
                  << " queue=" << queue
                  << " dispatchKey=" << dispatchKey(queue)
                  << " waits=" << (pPresentInfo ? pPresentInfo->waitSemaphoreCount : 0)
                  << " swapchains=" << (pPresentInfo ? pPresentInfo->swapchainCount : 0);
        if (pPresentInfo && pPresentInfo->swapchainCount && pPresentInfo->pSwapchains) {
            std::cerr << " firstSwapchain=" << pPresentInfo->pSwapchains[0];
            if (pPresentInfo->pImageIndices)
                std::cerr << " firstImage=" << pPresentInfo->pImageIndices[0];
        }
        std::cerr << " target=0x" << std::hex << pointerValue(target)
                  << " module=" << moduleFor(target) << std::dec
                  << '\n';
    }

    if (target == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = target(queue, pPresentInfo);
    if (detailed)
        std::cerr << "LSFG_PROVENANCE present-result seq=" << sequence << " result=" << result << '\n';
    return result;
}

void diagnostic_vkDestroySwapchainKHR(
        VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator) {
    const auto target = diagnosticDestroySwapchainTarget.load(std::memory_order_acquire);
    std::cerr << "LSFG_PROVENANCE invoke name=vkDestroySwapchainKHR"
              << " device=" << device
              << " dispatchKey=" << dispatchKey(device)
              << " swapchain=" << swapchain
              << " target=0x" << std::hex << pointerValue(target)
              << " module=" << moduleFor(target) << std::dec
              << " exe=" << processCmdline()
              << " pid=" << getpid()
              << " tid=" << currentTid() << '\n';
    std::cerr << "LSFG_PROVENANCE destroy swapchain=" << swapchain << '\n';
    if (target) target(device, swapchain, pAllocator);
}

PFN_vkVoidFunction wrapWsiHook(const char* resolver, VkInstance instance,
        VkDevice device, const char* pName, PFN_vkVoidFunction resolved) {
    if (pName == nullptr || resolved == nullptr) return resolved;

    const auto createHook = Hooks::hooks.find("vkCreateSwapchainKHR");
    if (createHook != Hooks::hooks.end() && resolved == createHook->second
            && std::strcmp(pName, "vkCreateSwapchainKHR") == 0) {
        diagnosticCreateSwapchainTarget.store(
            reinterpret_cast<PFN_vkCreateSwapchainKHR>(resolved), std::memory_order_release);
        auto wrapper = reinterpret_cast<PFN_vkVoidFunction>(&diagnostic_vkCreateSwapchainKHR);
        PFN_vkVoidFunction downstream = device != VK_NULL_HANDLE
            ? Layer::ovkGetDeviceProcAddr(device, pName)
            : Layer::ovkGetInstanceProcAddr(instance, pName);
        std::cerr << "lsfg-vk: runtime stage=wsi-hook-resolved source=" << resolver
                  << " name=" << pName << '\n';
        if (std::strcmp(resolver, "gipa") == 0)
            logAcquireGipa(pName, wrapper, resolved, downstream);
        else
            logAcquire(resolver, pName, device, wrapper, resolved, downstream);
        return wrapper;
    }

    const auto presentHook = Hooks::hooks.find("vkQueuePresentKHR");
    if (presentHook != Hooks::hooks.end() && resolved == presentHook->second
            && std::strcmp(pName, "vkQueuePresentKHR") == 0) {
        diagnosticQueuePresentTarget.store(
            reinterpret_cast<PFN_vkQueuePresentKHR>(resolved), std::memory_order_release);
        auto wrapper = reinterpret_cast<PFN_vkVoidFunction>(&diagnostic_vkQueuePresentKHR);
        PFN_vkVoidFunction downstream = device != VK_NULL_HANDLE
            ? Layer::ovkGetDeviceProcAddr(device, pName)
            : Layer::ovkGetInstanceProcAddr(instance, pName);
        std::cerr << "lsfg-vk: runtime stage=wsi-hook-resolved source=" << resolver
                  << " name=" << pName << '\n';
        if (std::strcmp(resolver, "gipa") == 0)
            logAcquireGipa(pName, wrapper, resolved, downstream);
        else
            logAcquire(resolver, pName, device, wrapper, resolved, downstream);
        return wrapper;
    }

    const auto destroyHook = Hooks::hooks.find("vkDestroySwapchainKHR");
    if (destroyHook != Hooks::hooks.end() && resolved == destroyHook->second
            && std::strcmp(pName, "vkDestroySwapchainKHR") == 0) {
        diagnosticDestroySwapchainTarget.store(
            reinterpret_cast<PFN_vkDestroySwapchainKHR>(resolved), std::memory_order_release);
        auto wrapper = reinterpret_cast<PFN_vkVoidFunction>(&diagnostic_vkDestroySwapchainKHR);
        PFN_vkVoidFunction downstream = device != VK_NULL_HANDLE
            ? Layer::ovkGetDeviceProcAddr(device, pName)
            : Layer::ovkGetInstanceProcAddr(instance, pName);
        std::cerr << "lsfg-vk: runtime stage=wsi-hook-resolved source=" << resolver
                  << " name=" << pName << '\n';
        if (std::strcmp(resolver, "gipa") == 0)
            logAcquireGipa(pName, wrapper, resolved, downstream);
        else
            logAcquire(resolver, pName, device, wrapper, resolved, downstream);
        return wrapper;
    }

    return resolved;
}
} // namespace
#endif

extern "C" __attribute__((visibility("default")))
PFN_vkVoidFunction layer_vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
#ifdef __ANDROID__
    const auto resolved = lsfg_layer_vkGetInstanceProcAddr_impl(instance, pName);
    return wrapWsiHook("gipa", instance, VK_NULL_HANDLE, pName, resolved);
#else
    return nullptr;
#endif
}

extern "C" __attribute__((visibility("default")))
PFN_vkVoidFunction layer_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
#ifdef __ANDROID__
    const auto resolved = lsfg_layer_vkGetDeviceProcAddr_impl(device, pName);
    return wrapWsiHook("gdpa", VK_NULL_HANDLE, device, pName, resolved);
#else
    return nullptr;
#endif
}

// Keep explicitly named diagnostic aliases available for standalone loader
// experiments, but the manifest intentionally remains on the production names.
extern "C" __attribute__((visibility("default")))
PFN_vkVoidFunction lsfg_vkGetInstanceProcAddrDiagnostic(VkInstance instance, const char* pName) {
    return layer_vkGetInstanceProcAddr(instance, pName);
}

extern "C" __attribute__((visibility("default")))
PFN_vkVoidFunction lsfg_vkGetDeviceProcAddrDiagnostic(VkDevice device, const char* pName) {
    return layer_vkGetDeviceProcAddr(device, pName);
}

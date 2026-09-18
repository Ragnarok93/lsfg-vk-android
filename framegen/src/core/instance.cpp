#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/instance.hpp"
#include "common/exception.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

using namespace LSFG::Core;

namespace {

std::mutex privateInstanceEnvironmentMutex;

std::optional<std::string> readEnvironment(const char* name) {
    const char* value = std::getenv(name);
    if (!value)
        return std::nullopt;
    return std::string(value);
}

void restoreEnvironment(const char* name, const std::optional<std::string>& value) {
    if (value.has_value())
        setenv(name, value->c_str(), 1);
    else
        unsetenv(name);
}

class ScopedPrivateInstanceLayerSuppression {
public:
    ScopedPrivateInstanceLayerSuppression()
        : lock(privateInstanceEnvironmentMutex),
          previousDisable(readEnvironment("DISABLE_LSFG")),
          previousInstanceLayers(readEnvironment("VK_INSTANCE_LAYERS")),
          previousLoaderLayersEnable(readEnvironment("VK_LOADER_LAYERS_ENABLE")),
          previousLoaderLayersDisable(readEnvironment("VK_LOADER_LAYERS_DISABLE")) {
        // GameNative force-enables the LSFG implicit layer for the game's
        // presentation instance. The framegen backend owns a separate compute
        // instance and must never recursively load LSFG into itself: destroying
        // such a nested instance can unload a second copy of this library while
        // the outer layer is still executing.
        setenv("DISABLE_LSFG", "1", 1);

        // GameNative currently force-enables LSFG through both the legacy
        // VK_INSTANCE_LAYERS path and the modern loader filter. The private
        // compute instance must not inherit either one. Disable all application
        // layers for this one vkCreateInstance call; the wrapper remains an ICD.
        unsetenv("VK_INSTANCE_LAYERS");
        unsetenv("VK_LOADER_LAYERS_ENABLE");
        setenv("VK_LOADER_LAYERS_DISABLE", "*", 1);
    }

    ~ScopedPrivateInstanceLayerSuppression() {
        restoreEnvironment("VK_LOADER_LAYERS_DISABLE", previousLoaderLayersDisable);
        restoreEnvironment("VK_LOADER_LAYERS_ENABLE", previousLoaderLayersEnable);
        restoreEnvironment("VK_INSTANCE_LAYERS", previousInstanceLayers);
        restoreEnvironment("DISABLE_LSFG", previousDisable);
    }

    ScopedPrivateInstanceLayerSuppression(
        const ScopedPrivateInstanceLayerSuppression&) = delete;
    ScopedPrivateInstanceLayerSuppression& operator=(
        const ScopedPrivateInstanceLayerSuppression&) = delete;

private:
    std::unique_lock<std::mutex> lock;
    std::optional<std::string> previousDisable;
    std::optional<std::string> previousInstanceLayers;
    std::optional<std::string> previousLoaderLayersEnable;
    std::optional<std::string> previousLoaderLayersDisable;
};

} // namespace

Instance::Instance() {
    if (volkInitialize() != VK_SUCCESS)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "Failed to initialize Vulkan loader");

    uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion)
        vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < VK_API_VERSION_1_1)
        throw LSFG::vulkan_error(VK_ERROR_INCOMPATIBLE_DRIVER, "LSFG requires Vulkan 1.1 or newer");

    const uint32_t requestedVersion = loaderVersion >= VK_API_VERSION_1_2
        ? VK_API_VERSION_1_2
        : VK_API_VERSION_1_1;
    const VkApplicationInfo appInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "lsfg-vk-base",
        .applicationVersion = VK_MAKE_VERSION(0, 0, 1),
        .pEngineName = "lsfg-vk-base",
        .engineVersion = VK_MAKE_VERSION(0, 0, 1),
        .apiVersion = requestedVersion,
    };
    const VkInstanceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
    };
    VkInstance instanceHandle{};
    VkResult res = VK_ERROR_INITIALIZATION_FAILED;
    {
        ScopedPrivateInstanceLayerSuppression suppressRecursiveLsfgLayer;
        res = vkCreateInstance(&createInfo, nullptr, &instanceHandle);
    }
    if (res != VK_SUCCESS || instanceHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create capability-selected Vulkan instance");

    volkLoadInstance(instanceHandle);
    this->instance = std::shared_ptr<VkInstance>(
        new VkInstance(instanceHandle),
        [](VkInstance* instance) { vkDestroyInstance(*instance, nullptr); });
}

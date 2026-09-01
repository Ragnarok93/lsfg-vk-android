#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/instance.hpp"
#include "common/exception.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>

using namespace LSFG::Core;

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
    const auto res = vkCreateInstance(&createInfo, nullptr, &instanceHandle);
    if (res != VK_SUCCESS || instanceHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create capability-selected Vulkan instance");

    volkLoadInstance(instanceHandle);
    this->instance = std::shared_ptr<VkInstance>(
        new VkInstance(instanceHandle),
        [](VkInstance* instance) { vkDestroyInstance(*instance, nullptr); });
}

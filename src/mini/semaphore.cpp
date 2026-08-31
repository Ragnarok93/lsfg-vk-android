#include "mini/semaphore.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>

#include <memory>

using namespace Mini;

namespace {

struct SemaphoreOwner {
    VkDevice device{};
    VkSemaphore handle{};

    SemaphoreOwner(VkDevice device, VkSemaphore handle)
        : device(device), handle(handle) {}

    SemaphoreOwner(const SemaphoreOwner&) = delete;
    SemaphoreOwner& operator=(const SemaphoreOwner&) = delete;

    ~SemaphoreOwner() {
        if (handle != VK_NULL_HANDLE)
            Layer::ovkDestroySemaphore(device, handle, nullptr);
    }
};

std::shared_ptr<VkSemaphore> ownSemaphore(VkDevice device, VkSemaphore handle) {
    // Aliasing shared_ptr keeps the Vulkan handle inside the same allocation as
    // its lifetime owner. LSFG creates several binary semaphores per generated
    // frame, so avoiding the old separate handle allocation materially reduces
    // allocator traffic while preserving the wrapper's copy semantics.
    auto owner = std::make_shared<SemaphoreOwner>(device, handle);
    return std::shared_ptr<VkSemaphore>(owner, &owner->handle);
}

} // namespace

Semaphore::Semaphore(VkDevice device) {
    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    VkSemaphore semaphoreHandle{};
    auto res = Layer::ovkCreateSemaphore(device, &desc, nullptr, &semaphoreHandle);
    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Unable to create semaphore");

    this->semaphore = ownSemaphore(device, semaphoreHandle);
}

Semaphore::Semaphore(VkDevice device, int* fd) {
    const VkExportSemaphoreCreateInfo exportInfo{
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT
    };
    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &exportInfo
    };
    VkSemaphore semaphoreHandle{};
    auto res = Layer::ovkCreateSemaphore(device, &desc, nullptr, &semaphoreHandle);
    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Unable to create semaphore");

    const VkSemaphoreGetFdInfoKHR fdInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore = semaphoreHandle,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT
    };
    res = Layer::ovkGetSemaphoreFdKHR(device, &fdInfo, fd);
    if (res != VK_SUCCESS || *fd < 0) {
        Layer::ovkDestroySemaphore(device, semaphoreHandle, nullptr);
        throw LSFG::vulkan_error(res, "Unable to export semaphore to fd");
    }

    this->semaphore = ownSemaphore(device, semaphoreHandle);
}

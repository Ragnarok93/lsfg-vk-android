#include "mini/semaphore.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>

#include <memory>
#include <unistd.h>

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

Semaphore::Semaphore(
        VkDevice device, VkExternalSemaphoreHandleTypeFlagBits handleType) {
    const VkExportSemaphoreCreateInfo exportInfo{
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = handleType,
    };
    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &exportInfo,
    };
    VkSemaphore semaphoreHandle{};
    const auto res = Layer::ovkCreateSemaphore(
        device, &desc, nullptr, &semaphoreHandle);
    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Unable to create exportable semaphore");

    this->semaphore = ownSemaphore(device, semaphoreHandle);
}

Semaphore::Semaphore(
        VkDevice device, int fd,
        VkExternalSemaphoreHandleTypeFlagBits handleType) {
    if (fd < 0)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Invalid semaphore import fd");

    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    VkSemaphore semaphoreHandle{};
    auto res = Layer::ovkCreateSemaphore(device, &desc, nullptr, &semaphoreHandle);
    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE) {
        ::close(fd);
        throw LSFG::vulkan_error(res, "Unable to create imported semaphore");
    }

    const auto importSemaphoreFd = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
        Layer::ovkGetDeviceProcAddr(device, "vkImportSemaphoreFdKHR"));
    if (importSemaphoreFd == nullptr) {
        Layer::ovkDestroySemaphore(device, semaphoreHandle, nullptr);
        ::close(fd);
        throw LSFG::vulkan_error(VK_ERROR_EXTENSION_NOT_PRESENT,
            "External semaphore fd import is unavailable");
    }

    const bool syncFd =
        handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    const VkImportSemaphoreFdInfoKHR importInfo{
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .semaphore = semaphoreHandle,
        .flags = syncFd ? VK_SEMAPHORE_IMPORT_TEMPORARY_BIT : 0U,
        .handleType = handleType,
        .fd = fd,
    };
    res = importSemaphoreFd(device, &importInfo);
    if (res != VK_SUCCESS) {
        Layer::ovkDestroySemaphore(device, semaphoreHandle, nullptr);
        ::close(fd);
        throw LSFG::vulkan_error(res, "Unable to import semaphore from fd");
    }

    this->semaphore = ownSemaphore(device, semaphoreHandle);
}

Semaphore::Semaphore(VkDevice device, int* fd)
    : Semaphore(device, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT) {
    if (fd == nullptr)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Semaphore export fd pointer is null");
    *fd = this->exportFd(
        device, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
}

int Semaphore::exportFd(
        VkDevice device, VkExternalSemaphoreHandleTypeFlagBits handleType) const {
    if (!this->semaphore)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Cannot export an uninitialized semaphore");

    const auto getSemaphoreFd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        Layer::ovkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR"));
    if (getSemaphoreFd == nullptr)
        throw LSFG::vulkan_error(VK_ERROR_EXTENSION_NOT_PRESENT,
            "External semaphore fd export is unavailable");

    const VkSemaphoreGetFdInfoKHR fdInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore = this->handle(),
        .handleType = handleType,
    };
    int fd = -1;
    const auto res = getSemaphoreFd(device, &fdInfo, &fd);
    const bool completedSyncFd =
        handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT && fd == -1;
    if (res != VK_SUCCESS || (fd < 0 && !completedSyncFd))
        throw LSFG::vulkan_error(res, "Unable to export semaphore to fd");
    return fd;
}

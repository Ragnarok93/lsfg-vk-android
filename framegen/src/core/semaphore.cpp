#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/semaphore.hpp"
#include "core/device.hpp"
#include "common/exception.hpp"

#include <optional>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <unistd.h>

using namespace LSFG::Core;

Semaphore::Semaphore(const Core::Device& device, std::optional<uint32_t> initial) {
    // create semaphore
    const VkSemaphoreTypeCreateInfo typeInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = initial.value_or(0)
    };
    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = initial.has_value() ? &typeInfo : nullptr,
    };
    VkSemaphore semaphoreHandle{};
    auto res = vkCreateSemaphore(device.handle(), &desc, nullptr, &semaphoreHandle);
    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Unable to create semaphore");

    // store semaphore in shared ptr
    this->isTimeline = initial.has_value();
    this->semaphore = std::shared_ptr<VkSemaphore>(
        new VkSemaphore(semaphoreHandle),
        [dev = device.handle()](VkSemaphore* semaphoreHandle) {
            vkDestroySemaphore(dev, *semaphoreHandle, nullptr);
        }
    );
}

Semaphore::Semaphore(const Core::Device& device, int fd) {
    if (fd < 0)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Invalid semaphore fd");

    // Imported OPAQUE_FD semaphores do not need to be exportable from the
    // framegen device. Keeping this object import-only avoids requesting more
    // external-semaphore capability than the actual cross-device wait needs.
    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    VkSemaphore semaphoreHandle{};
    auto res = vkCreateSemaphore(device.handle(), &desc, nullptr, &semaphoreHandle);
    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE) {
        ::close(fd);
        throw LSFG::vulkan_error(res, "Unable to create imported semaphore");
    }

    const auto importSemaphoreFd = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
        vkGetDeviceProcAddr(device.handle(), "vkImportSemaphoreFdKHR"));
    if (importSemaphoreFd == nullptr) {
        vkDestroySemaphore(device.handle(), semaphoreHandle, nullptr);
        ::close(fd);
        throw LSFG::vulkan_error(VK_ERROR_EXTENSION_NOT_PRESENT,
            "External semaphore fd import is unavailable");
    }

    const VkImportSemaphoreFdInfoKHR importInfo{
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .semaphore = semaphoreHandle,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
        .fd = fd
    };
    res = importSemaphoreFd(device.handle(), &importInfo);
    if (res != VK_SUCCESS) {
        // Vulkan only takes ownership of an OPAQUE_FD after a successful import.
        // Close it here on failure so an optional async handoff cannot leak FDs.
        vkDestroySemaphore(device.handle(), semaphoreHandle, nullptr);
        ::close(fd);
        throw LSFG::vulkan_error(res, "Unable to import semaphore from fd");
    }

    // Successful import transfers ownership of fd to Vulkan.
    this->isTimeline = false;
    this->semaphore = std::shared_ptr<VkSemaphore>(
        new VkSemaphore(semaphoreHandle),
        [dev = device.handle()](VkSemaphore* ownedSemaphore) {
            vkDestroySemaphore(dev, *ownedSemaphore, nullptr);
        }
    );
}

void Semaphore::signal(const Core::Device& device, uint64_t value) const {
    if (!this->isTimeline)
        throw std::logic_error("Invalid timeline semaphore");

    const VkSemaphoreSignalInfo signalInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
        .semaphore = this->handle(),
        .value = value
    };
    auto res = vkSignalSemaphore(device.handle(), &signalInfo);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Unable to signal semaphore");
}

bool Semaphore::wait(const Core::Device& device, uint64_t value, uint64_t timeout) const {
    if (!this->isTimeline)
        throw std::logic_error("Invalid timeline semaphore");

    VkSemaphore semaphore = this->handle();
    const VkSemaphoreWaitInfo waitInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &semaphore,
        .pValues = &value
    };
    auto res = vkWaitSemaphores(device.handle(), &waitInfo, timeout);
    if (res != VK_SUCCESS && res != VK_TIMEOUT)
        throw LSFG::vulkan_error(res, "Unable to wait for semaphore");

    return res == VK_SUCCESS;
}
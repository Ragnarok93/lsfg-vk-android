#pragma once

#include <vulkan/vulkan_core.h>

#include "common/exception.hpp"
#include "layer.hpp"

#include <memory>

namespace Mini {
    class Semaphore {
    public:
        Semaphore() = default;
        explicit Semaphore(VkDevice device);
        Semaphore(VkDevice device, int* fd);

        [[nodiscard]] VkSemaphore handle() const {
            return this->semaphore ? *this->semaphore : VK_NULL_HANDLE;
        }

        Semaphore(const Semaphore&) noexcept = default;
        Semaphore& operator=(const Semaphore&) noexcept = default;
        Semaphore(Semaphore&&) noexcept = default;
        Semaphore& operator=(Semaphore&&) noexcept = default;
        ~Semaphore() = default;
    private:
        std::shared_ptr<VkSemaphore> semaphore;
    };
}
#pragma once

#include "core/instance.hpp"
#include "lsfg_backend.hpp"

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <memory>

namespace LSFG::Core {

    class Image;

    class Device {
    public:
        Device(const Instance& instance, const LSFG::DeviceIdentity& identity, VkFormat sharedFormat);

        [[nodiscard]] auto handle() const { return *this->device; }
        [[nodiscard]] VkPhysicalDevice getPhysicalDevice() const { return this->physicalDevice; }
        [[nodiscard]] uint32_t getComputeFamilyIdx() const { return this->computeFamilyIdx; }
        [[nodiscard]] VkQueue getComputeQueue() const { return this->computeQueue; }
        [[nodiscard]] bool supportsNullDescriptor() const { return this->nullDescriptorSupported; }
        [[nodiscard]] const Image& getFallbackDescriptorImage() const;
        [[nodiscard]] LSFG::AhbTransportMode getAhbTransportMode() const {
            return this->diagnostics.ahbTransportMode;
        }
        [[nodiscard]] const LSFG::BackendDiagnostics& getDiagnostics() const {
            return this->diagnostics;
        }

        Device(const Core::Device&) noexcept = default;
        Device& operator=(const Core::Device&) noexcept = default;
        Device(Device&&) noexcept = default;
        Device& operator=(Device&&) noexcept = default;
        ~Device() = default;
    private:
        std::shared_ptr<VkDevice> device;
        VkPhysicalDevice physicalDevice{};
        uint32_t computeFamilyIdx{0};
        VkQueue computeQueue{};
        bool nullDescriptorSupported{false};
        std::shared_ptr<Image> fallbackDescriptorImage;
        LSFG::BackendDiagnostics diagnostics{};
    };

}

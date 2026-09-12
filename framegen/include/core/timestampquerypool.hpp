#pragma once

#include "core/device.hpp"

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace LSFG::Core {

    class TimestampQueryPool {
    public:
        TimestampQueryPool() noexcept = default;
        TimestampQueryPool(const Core::Device& device, uint32_t queryCount);

        [[nodiscard]] bool supported() const noexcept {
            return this->queryPool != nullptr
                && this->queryCount_ > 1
                && this->timestampValidBits_ > 0
                && this->timestampPeriodNs_ > 0.0f;
        }

        void reset(VkCommandBuffer commandBuffer) const;
        void write(VkCommandBuffer commandBuffer, uint32_t queryIndex) const;

        [[nodiscard]] std::vector<double> durationsMs(const Core::Device& device) const;

        TimestampQueryPool(const TimestampQueryPool&) noexcept = default;
        TimestampQueryPool& operator=(const TimestampQueryPool&) noexcept = default;
        TimestampQueryPool(TimestampQueryPool&&) noexcept = default;
        TimestampQueryPool& operator=(TimestampQueryPool&&) noexcept = default;
        ~TimestampQueryPool() = default;

    private:
        std::shared_ptr<VkQueryPool> queryPool;
        uint32_t queryCount_{0};
        uint32_t timestampValidBits_{0};
        float timestampPeriodNs_{0.0f};
    };

}

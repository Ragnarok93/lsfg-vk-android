#pragma once

#include "core/commandpool.hpp"
#include "core/fence.hpp"
#include "core/semaphore.hpp"
#include "core/device.hpp"

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <optional>
#include <vector>
#include <memory>

namespace LSFG::Core {

    enum class CommandBufferState {
        Invalid,
        Empty,
        Recording,
        Full,
        Submitted
    };

    class CommandBuffer {
    public:
        CommandBuffer() noexcept = default;
        CommandBuffer(const Core::Device& device, const CommandPool& pool);

        /// Reset a completed submitted command buffer for re-recording.
        void reset();
        void begin();
        void dispatch(uint32_t x, uint32_t y, uint32_t z) const;
        void end();
        void submit(VkQueue queue, std::optional<Fence> fence,
            const std::vector<Semaphore>& waitSemaphores = {},
            std::optional<std::vector<uint64_t>> waitSemaphoreValues = std::nullopt,
            const std::vector<Semaphore>& signalSemaphores = {},
            std::optional<std::vector<uint64_t>> signalSemaphoreValues = std::nullopt);

        [[nodiscard]] CommandBufferState getState() const { return *this->state; }
        [[nodiscard]] auto handle() const { return *this->commandBuffer; }

        CommandBuffer(const CommandBuffer&) noexcept = default;
        CommandBuffer& operator=(const CommandBuffer&) noexcept = default;
        CommandBuffer(CommandBuffer&&) noexcept = default;
        CommandBuffer& operator=(CommandBuffer&&) noexcept = default;
        ~CommandBuffer() = default;
    private:
        std::shared_ptr<CommandBufferState> state;
        std::shared_ptr<VkCommandBuffer> commandBuffer;
    };

}

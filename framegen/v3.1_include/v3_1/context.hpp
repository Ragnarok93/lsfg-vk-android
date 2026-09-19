#pragma once

#include "core/image.hpp"
#include "core/semaphore.hpp"
#include "core/fence.hpp"
#include "core/commandbuffer.hpp"
#include "core/timestampquerypool.hpp"
#include "core/descriptorpool.hpp"
#include "shaders/alpha.hpp"
#include "shaders/beta.hpp"
#include "shaders/delta.hpp"
#include "shaders/gamma.hpp"
#include "shaders/generate.hpp"
#include "shaders/mipmaps.hpp"
#include "common/utils.hpp"
#include "lsfg_backend.hpp"

#include <vulkan/vulkan_core.h>

#include <vector>
#include <cstdint>
#include <array>
#include <optional>
#include <cstddef>

#ifdef __ANDROID__
struct AHardwareBuffer;
#endif

namespace LSFG_3_1 {

    using namespace LSFG;

    class Context {
    public:
        ///
        /// Create a context
        ///
        /// @param vk The Vulkan instance to use.
        /// @param in0 File descriptor for the first input image.
        /// @param in1 File descriptor for the second input image.
        /// @param outN File descriptors for the output images.
        /// @param extent The size of the images.
        /// @param format The format of the images.
        ///
        /// @throws LSFG::vulkan_error if the context fails to initialize.
        ///
        Context(Vulkan& vk,
            int in0, int in1, const std::vector<int>& outN,
            VkExtent2D extent, VkFormat format);

#ifdef __ANDROID__
        ///
        /// Android-specific constructor: input/output images come from
        /// AHardwareBuffer instead of opaque FDs. See LSFG_3_1::createContextFromAHB.
        ///
        Context(Vulkan& vk,
            AHardwareBuffer* in0, AHardwareBuffer* in1,
            const std::vector<AHardwareBuffer*>& outN,
            VkExtent2D extent, VkFormat format);

        Context(Vulkan& vk,
            AHardwareBuffer* in0, AHardwareBuffer* in1,
            const std::vector<AHardwareBuffer*>& outN,
            VkExtent2D extent, VkFormat format,
            const std::vector<float>& adaptiveFlowScales);

        void requestFlowScale(float flowScale);
        [[nodiscard]] LSFG::AdaptiveFlowContextState flowScaleState() const;
        [[nodiscard]] LSFG::AdaptiveFlowGpuTiming gpuTiming() const;
#endif

        ///
        /// Present on the context.
        ///
        /// @param inSem Semaphore to wait on before starting the generation.
        /// @param outSem Semaphores to signal after each generation is done.
        ///
        /// @throws LSFG::vulkan_error if the context fails to present.
        ///
        LSFG::AndroidFrameSyncFds present(Vulkan& vk,
            int inSem, const std::vector<int>& outSem,
            size_t activeGenerationCount,
            VkExternalSemaphoreHandleTypeFlagBits inSemHandleType =
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
            bool exportAndroidSyncFdOutputs = false);

        [[nodiscard]] bool waitForLastPresent(Vulkan& vk, uint64_t timeoutNs);

        /// Wait only for this context's submitted fences, using the bounded
        /// LSFG_VK_WAIT_TIMEOUT_MS budget. Returns false on timeout.
        bool waitForCompletion(Vulkan& vk);

        // Trivially copyable, moveable and destructible
        Context(const Context&) = default;
        Context& operator=(const Context&) = default;
        Context(Context&&) = default;
        Context& operator=(Context&&) = default;
        ~Context() = default;
    private:
        Core::Image inImg_0, inImg_1; // private shader images in transport-only mode
#ifdef __ANDROID__
        bool inputCopyRequired{false};
        bool outputCopyRequired{false};
        Core::Image sharedInImg_0, sharedInImg_1;
        std::vector<Core::Image> sharedOutImages;
#endif
        uint64_t frameIdx{0};

        struct RenderData {
            Core::Semaphore inSemaphore; // signaled when input is ready
            std::vector<Core::Semaphore> internalSemaphores; // signaled when first step is done
            std::vector<Core::Semaphore> outSemaphores; // signaled when each pass is done
            Core::Semaphore batchCompleteSemaphore; // separate SYNC_FD batch dependency
            std::vector<Core::Fence> completionFences; // fence for completion of each pass
            Core::Fence preprocessingFence; // reused for zero-generation temporal preprocessing
#ifdef __ANDROID__
            Core::TimestampQueryPool adaptiveFlowTimingQueryPool;
            bool adaptiveFlowTransitionCycle{false};
#endif

            Core::CommandBuffer cmdBuffer1;
            std::vector<Core::CommandBuffer> cmdBuffers2; // command buffers for second step

            bool shouldWait{false};
            size_t generationCount{0};
        };
        std::array<RenderData, 8> data;

#ifdef __ANDROID__
        struct AdaptiveFlowGraph {
            float userFlowScale{0.0f};
            // Keep each prepared Flow Scale's descriptors in its own pool.
            // A complete LSFG graph can consume enough sampled/storage-image
            // descriptors that four preset states overflow the legacy shared
            // 4096-descriptor pool even though every state is valid alone.
            Core::DescriptorPool descriptorPool;
            Shaders::Mipmaps mipmaps;
            std::array<Shaders::Alpha, 7> alpha;
            Shaders::Beta beta;
            std::array<Shaders::Gamma, 7> gamma;
            std::array<Shaders::Delta, 3> delta;
            Shaders::Generate generate;
        };

        struct FlowGraphRef {
            float userFlowScale{0.0f};
            Shaders::Mipmaps* mipmaps{nullptr};
            std::array<Shaders::Alpha, 7>* alpha{nullptr};
            Shaders::Beta* beta{nullptr};
            std::array<Shaders::Gamma, 7>* gamma{nullptr};
            std::array<Shaders::Delta, 3>* delta{nullptr};
            Shaders::Generate* generate{nullptr};
        };

        static constexpr uint32_t kAdaptiveFlowHistoryFrames = 3;
        std::vector<AdaptiveFlowGraph> adaptiveFlowGraphs_;
        std::vector<float> adaptiveFlowScales_;
        size_t activeFlowGraphIndex_{0};
        std::optional<size_t> pendingFlowGraphIndex_;
        uint32_t pendingFlowWarmupFrames_{0};
        float requestedFlowScale_{0.0f};
        LSFG::AdaptiveFlowGpuTiming lastAdaptiveFlowGpuTiming_{};

        [[nodiscard]] AdaptiveFlowGraph buildAdaptiveFlowGraph(
            Vulkan& vk, float userFlowScale,
            const std::vector<Core::Image>& outImgs);
        [[nodiscard]] FlowGraphRef flowGraph(size_t index);
        void dispatchAdaptiveFlowPreprocess(
            const Core::CommandBuffer& buffer, FlowGraphRef graph,
            Core::TimestampQueryPool* timingPool = nullptr);
        void recordAdaptiveFlowGpuTiming(Vulkan& vk, RenderData& renderData);
        void commitAdaptiveFlowTransition(size_t index);
#endif

        Shaders::Mipmaps mipmaps;
        std::array<Shaders::Alpha, 7> alpha;
        Shaders::Beta beta;
        std::array<Shaders::Gamma, 7> gamma;
        std::array<Shaders::Delta, 3> delta;
        Shaders::Generate generate;
    };

}

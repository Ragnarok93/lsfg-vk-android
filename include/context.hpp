#pragma once

#include <vulkan/vulkan_core.h>

#ifdef __ANDROID__
#include <vulkan/vulkan_android.h>
#include <android/hardware_buffer.h>
#endif

#include "hooks.hpp"
#include "fixed_frame_governor.hpp"
#include "mini/commandbuffer.hpp"
#include "mini/commandpool.hpp"
#include "mini/image.hpp"
#include "mini/semaphore.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

///
/// This class is the frame generation context. There should be one instance per swapchain.
///
class LsContext {
public:
    ///
    /// Create the swapchain context.
    ///
    /// @param info The device information to use.
    /// @param swapchain The Vulkan swapchain to use.
    /// @param extent The extent of the swapchain images.
    /// @param swapchainImages The swapchain images to use.
    ///
    /// @throws LSFG::vulkan_error if any Vulkan call fails.
    ///
    LsContext(const Hooks::DeviceInfo& info, VkSwapchainKHR swapchain,
        VkExtent2D extent, const std::vector<VkImage>& swapchainImages);

    ///
    /// Custom present logic.
    ///
    /// @param info The device information to use.
    /// @param pNext Unknown pointer set in the present info structure.
    /// @param queue The Vulkan queue to present the frame on.
    /// @param gameRenderSemaphores The semaphores to wait on before presenting.
    /// @param presentIdx The index of the swapchain image to present.
    /// @return The result of the Vulkan present operation, which can be VK_SUCCESS or VK_SUBOPTIMAL_KHR.
    ///
    /// @throws LSFG::vulkan_error if any Vulkan call fails.
    ///
    VkResult present(const Hooks::DeviceInfo& info, const void* pNext, VkQueue queue,
        const std::vector<VkSemaphore>& gameRenderSemaphores, uint32_t presentIdx);

    [[nodiscard]] size_t lastGeneratedFrameCount() const {
        return lastGeneratedFrameCount_;
    }

#ifdef __ANDROID__
    void enterSourceOnlyBypass();
#endif

    // Non-copyable, trivially moveable and destructible
    LsContext(const LsContext&) = delete;
    LsContext& operator=(const LsContext&) = delete;
    LsContext(LsContext&&) = default;
    LsContext& operator=(LsContext&&) = default;
    ~LsContext() = default;
private:
    VkSwapchainKHR swapchain;
    std::vector<VkImage> swapchainImages;
    VkExtent2D extent;

    std::shared_ptr<int32_t> lsfgCtxId; // lsfg context id
    Mini::Image frame_0, frame_1; // frames shared with lsfg. write to frame_0 when fc % 2 == 0
    std::vector<Mini::Image> out_n; // output images shared with lsfg, indexed by framegen id

    Mini::CommandPool cmdPool;
    uint64_t frameIdx{0};
    size_t lastGeneratedFrameCount_{0};

#ifdef __ANDROID__
    FixedFrameGovernor fixedGovernor_;
    bool requiresSourceHistoryWarmup_{false};
    bool previousSourceCopySignalValid_{false};
    // Optional fast path only. If either logical device cannot share an
    // OPAQUE_FD semaphore, or an export fails at runtime, this is disabled for
    // the life of the context and the established synchronous fence path wins.
    bool asyncAhbHandoffEnabled_{false};
    struct RuntimeMetrics {
        using Clock = std::chrono::steady_clock;

        Clock::time_point windowStart{Clock::now()};
        Clock::time_point lastSourcePresent{};
        bool hasLastSourcePresent{false};

        uint64_t windowSourceFrames{0};
        uint64_t windowGeneratedFrames{0};
        uint64_t windowSourcePresentFailures{0};
        uint64_t windowGeneratedPresentFailures{0};
        uint64_t totalSourceFrames{0};
        uint64_t totalGeneratedFrames{0};
        uint64_t totalSourcePresentFailures{0};
        uint64_t totalGeneratedPresentFailures{0};

        uint64_t windowGovernorRateSnaps{0};
        uint64_t totalGovernorRateSnaps{0};
        uint64_t windowGovernorCostRaises{0};
        uint64_t totalGovernorCostRaises{0};
        uint64_t windowGovernorCostBackoffs{0};
        uint64_t totalGovernorCostBackoffs{0};
        uint64_t windowGovernorCostProbes{0};
        uint64_t totalGovernorCostProbes{0};
        uint64_t windowGovernorRefreshLimits{0};
        uint64_t totalGovernorRefreshLimits{0};
        uint64_t windowGovernorDiscontinuities{0};
        uint64_t totalGovernorDiscontinuities{0};
        uint64_t windowAsyncHandoffs{0};
        uint64_t totalAsyncHandoffs{0};
        uint64_t windowSyncHandoffs{0};
        uint64_t totalSyncHandoffs{0};
        uint64_t totalAsyncFallbacks{0};

        double windowCycleMs{0.0};
        double windowCycleMaxMs{0.0};
        double windowHandoffMs{0.0};
        double windowDispatchMs{0.0};
        double windowWaitIdleMs{0.0};
        double windowGeneratedPresentMs{0.0};
        double windowSourceIntervalMs{0.0};
        double windowSourceIntervalMaxMs{0.0};
        uint64_t windowSourceIntervals{0};
    } runtimeMetrics;

    // Reused for the game-device -> framegen AHB handoff. Async generated
    // cycles still attach this fence to the source-copy submit; by the time the
    // framegen completion wait returns, that submit has necessarily completed,
    // so the fence is safe to reset on the next cycle. Warm-up/fallback cycles
    // continue to wait it synchronously exactly as before.
    std::shared_ptr<VkFence> ahbHandoffFence;
    PFN_vkResetFences resetHandoffFences{nullptr};
    PFN_vkWaitForFences waitHandoffFences{nullptr};
#endif

    struct RenderPassInfo {
        Mini::CommandBuffer preCopyBuf; // copy from swapchain image to frame_0/frame_1
        std::array<Mini::Semaphore, 2> preCopySemaphores; // signal when preCopyBuf is done
#ifdef __ANDROID__
        // Dedicated cross-device signal. It is never shared with source-present
        // or next-source-copy waits, so each binary semaphore has one consumer.
        Mini::Semaphore framegenInputSemaphore;
#endif

        std::vector<Mini::Semaphore> renderSemaphores; // signal when lsfg is done with frame n

        std::vector<Mini::Semaphore> acquireSemaphores; // signal for swapchain image n

        std::vector<Mini::CommandBuffer> postCopyBufs; // copy from out_n to swapchain image
        std::vector<Mini::Semaphore> postCopySemaphores; // signal when postCopyBuf is done
        std::vector<Mini::Semaphore> prevPostCopySemaphores; // signal for previous postCopyBuf
    }; // data for a single render pass
    std::array<RenderPassInfo, 8> passInfos; // allocate 8 because why not
};
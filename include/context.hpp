#pragma once

#include <vulkan/vulkan_core.h>

#ifdef __ANDROID__
#include <vulkan/vulkan_android.h>
#include <android/hardware_buffer.h>
#endif

#include "hooks.hpp"
#include "adaptive_scheduler.hpp"
#include "output_frame_pacer.hpp"
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
    LsContext(const Hooks::DeviceInfo& info, VkSwapchainKHR swapchain,
        VkExtent2D extent, const std::vector<VkImage>& swapchainImages,
        VkFormat sharedFormat);

    VkResult present(const Hooks::DeviceInfo& info, const void* pNext, VkQueue queue,
        const std::vector<VkSemaphore>& gameRenderSemaphores, uint32_t presentIdx);

    [[nodiscard]] size_t lastGeneratedFrameCount() const {
        return lastGeneratedFrameCount_;
    }

    LsContext(const LsContext&) = delete;
    LsContext& operator=(const LsContext&) = delete;
    LsContext(LsContext&&) = default;
    LsContext& operator=(LsContext&&) = default;
    ~LsContext() = default;
private:
    VkSwapchainKHR swapchain;
    std::vector<VkImage> swapchainImages;
    VkExtent2D extent;

    std::shared_ptr<int32_t> lsfgCtxId;
    Mini::Image frame_0, frame_1;
    std::vector<Mini::Image> out_n;

    Mini::CommandPool cmdPool;
    uint64_t frameIdx{0};
    size_t lastGeneratedFrameCount_{0};
    bool performanceMode_{false};

#ifdef __ANDROID__
    AdaptiveFrameScheduler adaptiveScheduler_;
    AdaptiveFrameScheduler::StageCosts lastStageCosts_{};
    OutputFramePacer outputFramePacer_;
    bool requiresSourceHistoryWarmup_{false};
    bool previousSourceCopySignalValid_{false};
    // Direct adaptive-zero presents can advance frameIdx without touching the
    // AHB inputs. Track layout initialization per shared source image instead
    // of inferring first-use from the global frame counter.
    std::array<bool, 2> sourceAhbInitialized_{false, false};
    struct RuntimeMetrics {
        using Clock = std::chrono::steady_clock;

        Clock::time_point windowStart{Clock::now()};
        Clock::time_point lastPresentEntry{};
        Clock::time_point lastCycleEnd{};
        bool hasLastPresentEntry{false};
        bool hasLastCycleEnd{false};

        uint64_t windowSourceFrames{0};
        uint64_t windowGeneratedFrames{0};
        uint64_t windowSourcePresentFailures{0};
        uint64_t windowGeneratedPresentFailures{0};
        uint64_t totalSourceFrames{0};
        uint64_t totalGeneratedFrames{0};
        uint64_t totalSourcePresentFailures{0};
        uint64_t totalGeneratedPresentFailures{0};

        double windowCycleMs{0.0};
        double windowCycleMaxMs{0.0};
        double windowHandoffMs{0.0};
        double windowDispatchMs{0.0};
        double windowWaitIdleMs{0.0};
        double windowGeneratedPresentMs{0.0};
        double windowSourceIntervalMs{0.0};
        double windowSourceIntervalMaxMs{0.0};
        uint64_t windowSourceIntervals{0};
        double windowPresentEntryIntervalMs{0.0};
        double windowPresentEntryIntervalMaxMs{0.0};
        uint64_t windowPresentEntryIntervals{0};
    } runtimeMetrics;

    std::shared_ptr<VkFence> ahbHandoffFence;
    PFN_vkResetFences resetHandoffFences{nullptr};
    PFN_vkWaitForFences waitHandoffFences{nullptr};
#endif

    struct RenderPassInfo {
        Mini::CommandBuffer preCopyBuf;
        std::array<Mini::Semaphore, 2> preCopySemaphores;
        std::vector<Mini::Semaphore> renderSemaphores;
        std::vector<Mini::Semaphore> acquireSemaphores;
        std::vector<Mini::CommandBuffer> postCopyBufs;
        std::vector<Mini::Semaphore> postCopySemaphores;
        std::vector<Mini::Semaphore> prevPostCopySemaphores;
    };
    std::array<RenderPassInfo, 8> passInfos;
};

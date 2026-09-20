#pragma once

#include <vulkan/vulkan_core.h>

#ifdef __ANDROID__
#include <vulkan/vulkan_android.h>
#include <android/hardware_buffer.h>
#endif

#include "hooks.hpp"
#include "adaptive_scheduler.hpp"
#include "adaptive_flow_controller.hpp"
#include "mini/commandbuffer.hpp"
#include "mini/commandpool.hpp"
#include "mini/image.hpp"
#include "mini/semaphore.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#ifdef __ANDROID__
struct AdaptiveFlowRuntimeSnapshot {
    bool enabled{false};
    const char* preset{"quality"};
    float targetScale{0.0F};
    float minimumScale{0.0F};
    float requestedScale{0.0F};
    float activeScale{0.0F};
    bool transitionPending{false};
    uint32_t warmupRemaining{0};
    bool timingValid{false};
    double mipmapsMs{0.0};
    double flowMs{0.0};
    double totalLsfgMs{0.0};
    double budgetMs{0.0};
    size_t generationCount{0};
    bool globalPressureValid{false};
    double globalGpuUsagePercent{0.0};
    double globalOutputFps{0.0};
    bool lsfgOutputValid{false};
    double lsfgOutputFps{0.0};
    double globalFrameTimeP95Ms{0.0};
    double globalSlowFrameRatio{0.0};
    bool globalPressure{false};
    bool computePressure{false};
    bool wsiPressure{false};
    double wsiLossRate{0.0};
    size_t presentationGenerationCap{0};
    bool outputDeficit{false};
    bool syntheticDropPressure{false};
    const char* reason{"none"};
};
#endif

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

    [[nodiscard]] AdaptiveFlowRuntimeSnapshot adaptiveFlowRuntimeSnapshot() const {
        const auto& telemetry = adaptiveFlowController_.telemetry();
        const auto& outputCadence = lsfgOutputCadenceTracker_.snapshot();
        const auto& presentationCapacity =
            generatedPresentationCapacityTracker_.telemetry();
        return AdaptiveFlowRuntimeSnapshot{
            .enabled = adaptiveFlowController_.enabled(),
            .preset = AdaptiveFlowController::presetName(adaptiveFlowPreset_),
            .targetScale = telemetry.targetScale,
            .minimumScale = telemetry.minimumScale,
            .requestedScale = adaptiveFlowRequestedScale_,
            .activeScale = adaptiveFlowActiveScale_,
            .transitionPending = adaptiveFlowTransitionPending_,
            .warmupRemaining = adaptiveFlowWarmupRemaining_,
            .timingValid = adaptiveFlowTimingValid_,
            .mipmapsMs = adaptiveFlowMipmapsMs_,
            .flowMs = adaptiveFlowWorkMs_,
            .totalLsfgMs = adaptiveFlowTotalLsfgMs_,
            .budgetMs = adaptiveFlowBudgetMs_,
            .generationCount = adaptiveFlowGenerationCount_,
            .globalPressureValid = adaptiveFlowGlobalPressureValid_,
            .globalGpuUsagePercent = adaptiveFlowGlobalGpuUsagePercent_,
            .globalOutputFps = adaptiveFlowGlobalOutputFps_,
            .lsfgOutputValid = outputCadence.valid,
            .lsfgOutputFps = outputCadence.outputFps,
            .globalFrameTimeP95Ms = adaptiveFlowGlobalFrameTimeP95Ms_,
            .globalSlowFrameRatio = adaptiveFlowGlobalSlowFrameRatio_,
            .globalPressure = telemetry.globalPressure,
            .computePressure = adaptiveFlowComputePressure_,
            .wsiPressure = adaptiveFlowWsiPressure_,
            .wsiLossRate = presentationCapacity.wsiRejectionRatio,
            .presentationGenerationCap = presentationCapacity.generationCap,
            .outputDeficit = telemetry.outputDeficit,
            .syntheticDropPressure =
                adaptiveFlowComputePressure_ || adaptiveFlowWsiPressure_,
            .reason = AdaptiveFlowController::reasonName(adaptiveFlowReason_),
        };
    }
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
    AdaptiveFrameScheduler adaptiveScheduler_;
    std::size_t lastDispatchedGeneratedFrameCount_{0};
    AdaptiveFlowController adaptiveFlowController_;
    AdaptiveFlowPreset adaptiveFlowPreset_{AdaptiveFlowPreset::Quality};
    bool adaptiveFlowRuntimeAvailable_{false};
    float adaptiveFlowRequestedScale_{1.0F};
    float adaptiveFlowActiveScale_{1.0F};
    uint32_t adaptiveFlowWarmupRemaining_{0};
    bool adaptiveFlowTransitionPending_{false};
    bool adaptiveFlowTimingValid_{false};
    double adaptiveFlowMipmapsMs_{0.0};
    double adaptiveFlowWorkMs_{0.0};
    double adaptiveFlowTotalLsfgMs_{0.0};
    double adaptiveFlowBudgetMs_{0.0};
    size_t adaptiveFlowGenerationCount_{0};
    AdaptiveFlowDecisionReason adaptiveFlowReason_{AdaptiveFlowDecisionReason::None};

    // Whole-device pressure is sampled by GameNative at 500 ms and published
    // out-of-band from conf.toml so pressure updates never rebuild the LSFG
    // context. Generated-work timing is retained across history-only cycles:
    // those cycles may confirm pressure, but can never prove recovery headroom.
    std::chrono::steady_clock::time_point adaptiveFlowNextPressureRead_{};
    bool adaptiveFlowGlobalPressureValid_{false};
    double adaptiveFlowGlobalGpuUsagePercent_{0.0};
    double adaptiveFlowGlobalOutputFps_{0.0};
    double adaptiveFlowGlobalFrameTimeP95Ms_{0.0};
    double adaptiveFlowGlobalSlowFrameRatio_{0.0};
    bool adaptiveFlowGeneratedTimingValid_{false};
    double adaptiveFlowRetainedMipmapsMs_{0.0};
    double adaptiveFlowRetainedWorkMs_{0.0};
    double adaptiveFlowRetainedTotalLsfgMs_{0.0};
    size_t adaptiveFlowRetainedGenerationCount_{0};
    uint64_t adaptiveFlowLastObservedComputeDrops_{0};
    uint64_t adaptiveFlowLastObservedWsiDrops_{0};
    bool adaptiveFlowComputePressure_{false};
    bool adaptiveFlowWsiPressure_{false};

    DeadlineAdmissionPredictor deadlineAdmissionPredictor_;
    GeneratedPresentationCapacityTracker generatedPresentationCapacityTracker_;
    LsfgOutputCadenceTracker lsfgOutputCadenceTracker_;
    DeadlineAdmissionDecision deadlineBatchDecision_{};

    // Adaptive presentation pacing. VK_GOOGLE_display_timing is optional; the
    // swapchain still uses FIFO ordering when this capability is unavailable.
    bool adaptiveDisplayTimingEnabled_{false};
    uint64_t adaptivePresentPeriodNs_{0};
    uint32_t adaptivePresentId_{1};
    SourceProtectedTimeline sourceTimeline_;
    SourceTimelineSample currentSourceTimeline_;

    static constexpr uint32_t kSourceHistoryWarmupFrames = 4;
    uint32_t sourceHistoryWarmupRemaining_{kSourceHistoryWarmupFrames};
    bool requiresSourceHistoryWarmup_{true};
    bool previousSourceCopySignalValid_{false};

    // Queue-target 1 delivery for Android: the application's real frame is
    // acknowledged this call, but displayed on the next source boundary so the
    // preceding interpolation batch can be inserted in temporal order. Synthetic
    // work that is not complete by that next boundary is dropped rather than
    // delaying the real frame.
    bool pendingSourceValid_{false};
    uint32_t pendingSourceImage_{0};
    Mini::Semaphore pendingSourceReady_;
    size_t pendingPassIndex_{0};
    size_t pendingGeneratedCount_{0};
    bool framegenInFlight_{false};
    bool framegenOutputEligible_{false};

    // Optional fast path only. Prefer one-shot SYNC_FD on Android, retain
    // OPAQUE_FD compatibility, and fall back to the established host fence.
    bool asyncAhbHandoffEnabled_{false};
    bool asyncFramegenCompletionEnabled_{false};
    VkExternalSemaphoreHandleTypeFlagBits asyncAhbHandoffHandleType_{
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
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

        uint64_t windowAdaptiveZeroGenerationCycles{0};
        uint64_t totalAdaptiveZeroGenerationCycles{0};
        uint64_t windowAdaptiveRateSnaps{0};
        uint64_t totalAdaptiveRateSnaps{0};
        uint64_t windowAdaptiveCostRaises{0};
        uint64_t totalAdaptiveCostRaises{0};
        uint64_t windowAdaptiveCostBackoffs{0};
        uint64_t totalAdaptiveCostBackoffs{0};
        uint64_t windowAdaptiveCostProbes{0};
        uint64_t totalAdaptiveCostProbes{0};
        uint64_t windowAdaptiveDiscontinuities{0};
        uint64_t totalAdaptiveDiscontinuities{0};
        uint64_t windowAsyncHandoffs{0};
        uint64_t totalAsyncHandoffs{0};
        uint64_t windowSyncHandoffs{0};
        uint64_t totalSyncHandoffs{0};
        uint64_t totalAsyncFallbacks{0};
        uint64_t windowGeneratedLateDrops{0};
        uint64_t totalGeneratedLateDrops{0};
        uint64_t windowAdmissionRejects{0};
        uint64_t totalAdmissionRejects{0};
        uint64_t windowGeneratedDeadlineDrops{0};
        uint64_t totalGeneratedDeadlineDrops{0};
        uint64_t windowGeneratedWsiDrops{0};
        uint64_t totalGeneratedWsiDrops{0};
        uint64_t windowGeneratedPresentationCapDrops{0};
        uint64_t totalGeneratedPresentationCapDrops{0};
        uint64_t windowDeadlineShadowOpportunities{0};
        uint64_t windowDeadlineShadowWouldAdmit{0};
        uint64_t windowDeadlineShadowWouldReject{0};
        uint64_t totalDeadlineShadowOpportunities{0};
        uint64_t totalDeadlineShadowWouldReject{0};

        double windowCycleMs{0.0};
        double windowCycleMaxMs{0.0};
        double windowHandoffMs{0.0};
        double windowDispatchMs{0.0};
        double windowWaitIdleMs{0.0};
        double windowGeneratedPresentMs{0.0};
        double windowSourceIntervalMs{0.0};
        double windowSourceIntervalMaxMs{0.0};
        double windowSourceDeadlineErrorAbsMs{0.0};
        double windowSourceDeadlineErrorMaxMs{0.0};
        double windowDeadlinePredictionAbsErrorMs{0.0};
        uint64_t windowDeadlinePredictionSamples{0};
        uint64_t windowSourceIntervals{0};
        uint64_t windowSourceDeadlineSamples{0};
        uint64_t windowSourceTimelineRebases{0};

        // Previous completed one-second LSFG metrics window. It is retained
        // for diagnostics only; Adaptive Flow control uses the shorter rolling
        // LsfgOutputCadenceTracker instead.
        bool lastWindowOutputFpsValid{false};
        bool lastWindowAdaptiveFramegen{false};
        uint32_t lastWindowTargetFps{0};
        double lastWindowOutputFps{0.0};
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
        Mini::Semaphore framegenBatchCompleteSemaphore;
        bool framegenBatchCompleteValid{false};
#endif

        std::vector<Mini::Semaphore> renderSemaphores; // signal when lsfg is done with frame n

        std::vector<Mini::Semaphore> acquireSemaphores; // signal for swapchain image n

        std::vector<Mini::CommandBuffer> postCopyBufs; // copy from out_n to swapchain image
        std::vector<Mini::Semaphore> postCopySemaphores; // signal when postCopyBuf is done
        std::vector<Mini::Semaphore> prevPostCopySemaphores; // signal for previous postCopyBuf
    }; // data for a single render pass
    std::array<RenderPassInfo, 8> passInfos; // allocate 8 because why not
};
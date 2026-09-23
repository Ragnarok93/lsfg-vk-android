#pragma once

#include <vulkan/vulkan_core.h>

#ifdef __ANDROID__
#include <vulkan/vulkan_android.h>
#include <android/hardware_buffer.h>
#endif

#include "hooks.hpp"
#include "android_sync_policy.hpp"
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
enum class SourceHistoryInvalidationReason {
    None,
    Startup,
    TimelineDiscontinuity,
    SyncExportFailure,
    SyncImportFailure,
    ContextRecreate,
    SourcePairMismatch,
    AbandonedBatch,
    TrueOwnershipFailure,
};

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
    double presentationDuty{1.0};
    unsigned presentationRejectionEvidence{0};
    double presentationRecoveryEvidence{0.0};
    uint64_t presentationAttemptedGeneratedFrames{0};
    uint64_t presentationAcceptedGeneratedFrames{0};
    double presentationDeliveredEfficiency{0.0};
    const char* presentationLastChangeReason{"none"};
    bool presentationLastChangeOutputDeficit{false};
    bool presentationProvisionalLowerActive{false};
    bool presentationUpwardProbePending{false};
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
            .presentationDuty = presentationCapacity.singleFrameDuty,
            .presentationRejectionEvidence = presentationCapacity.rejectionEvidence,
            .presentationRecoveryEvidence = presentationCapacity.recoveryEvidence,
            .presentationAttemptedGeneratedFrames =
                presentationCapacity.attemptedGeneratedFrames,
            .presentationAcceptedGeneratedFrames =
                presentationCapacity.acceptedGeneratedFrames,
            .presentationDeliveredEfficiency =
                presentationCapacity.deliveredEfficiency,
            .presentationLastChangeReason = generatedPresentationCapChangeReasonName(
                presentationCapacity.lastChangeReason),
            .presentationLastChangeOutputDeficit =
                presentationCapacity.lastChangeOutputDeficit,
            .presentationProvisionalLowerActive =
                presentationCapacity.provisionalLowerActive,
            .presentationUpwardProbePending =
                presentationCapacity.upwardProbePending,
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
    ~LsContext();
private:
    void releasePresentWaitRetirements(uint32_t imageIdx);
    void retainPresentWait(uint32_t imageIdx, const Mini::Semaphore& semaphore);
#ifdef __ANDROID__
    void advanceAdaptiveFlowTimingEpoch();
    void resetAdaptiveSourceEpoch(
        bool resetScheduler,
        SourceHistoryInvalidationReason reason =
            SourceHistoryInvalidationReason::TimelineDiscontinuity);
#endif
    VkSwapchainKHR swapchain;
    std::vector<VkImage> swapchainImages;
    // Binary semaphores passed to vkQueuePresentKHR remain owned here until
    // the associated swapchain image is acquired again. A later queue-submit
    // fence does not prove that the presentation engine has released them.
    std::vector<std::vector<Mini::Semaphore>> presentWaitRetirements_;
    VkExtent2D extent;

    // The pass ring owns resources submitted to the game's queue. Retain the
    // queue/device dispatch until every in-flight pass has retired.
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue queue_{VK_NULL_HANDLE};
    PFN_vkWaitForFences completionWaitFences_{nullptr};
    PFN_vkResetFences completionResetFences_{nullptr};
    PFN_vkQueueWaitIdle waitQueueIdle_{nullptr};

    Mini::Image frame_0, frame_1; // frames shared with lsfg. write to frame_0 when fc % 2 == 0
    std::vector<Mini::Image> out_n; // output images shared with lsfg, indexed by framegen id
    // Declared after the imported images so exceptional construction and
    // normal destruction release the framegen context before those images.
    std::shared_ptr<int32_t> lsfgCtxId; // lsfg context id

    Mini::CommandPool cmdPool;
    uint64_t frameIdx{0};
    size_t lastGeneratedFrameCount_{0};

#ifdef __ANDROID__
    uint64_t runtimeSessionId_{0};
    // Adreno source-present count can advance while a private framegen batch
    // remains in flight. Track the input slot consumed by framegen separately
    // so source-only bypasses cannot flip the shared AHB pair out of parity.
    uint64_t conservativeFramegenSourceIndex_{0};
    uint64_t runtimeConfigSignature_{0};
    uint64_t configRevision_{0};
    bool runtimeConfigSignatureValid_{false};

    AdaptiveFrameScheduler adaptiveScheduler_;
    FixedSourceCadenceGovernor fixedSourceCadenceGovernor_;
    SourceProtectionBudgetTracker sourceProtectionBudgetTracker_;
    std::size_t lastDispatchedGeneratedFrameCount_{0};
    // Classifies the interval observed at the next intercepted source present.
    // Only a true LSFG-bypass source interval may expand the protected baseline.
    SourceCadenceObservation lastSourceCadenceObservation_{
        SourceCadenceObservation::SourceOnly};
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
    double adaptiveFlowRetainedBudgetMs_{0.0};
    size_t adaptiveFlowRetainedGenerationCount_{0};
    uint64_t adaptiveFlowTimingEpoch_{1};
    uint64_t adaptiveFlowNextBatchId_{0};
    uint64_t adaptiveFlowLastObservedBatchId_{0};
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
    SourceHistoryInvalidationReason lastHistoryInvalidationReason_{
        SourceHistoryInvalidationReason::Startup};
    SourceHistoryInvalidationReason lastHistoryReprimeReason_{
        SourceHistoryInvalidationReason::Startup};

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

    // Qualcomm/Adreno keeps protected source/history handling for zero,
    // reprime, and source-only cycles.
    AndroidSyncPolicy::FramegenCompatibilityPath compatibilityPath_{
        AndroidSyncPolicy::FramegenCompatibilityPath::Generic};
    bool conservativeCrossDeviceSync_{false};

    // Single-queue Adreno cannot safely queue an unsignaled framegen completion
    // wait ahead of the application's next source work. Export completion FDs,
    // keep exactly one batch deferred, and consume it only at a later source
    // boundary after nonblocking readiness proves it cannot head-of-line block.
    bool deferredAdrenoCompletionEnabled_{false};
    bool deferredAdrenoBatchValid_{false};
    std::vector<int> deferredAdrenoOutputReadyFds_;
    int deferredAdrenoBatchCompleteFd_{-1};
    size_t deferredAdrenoPassIndex_{0};
    size_t deferredAdrenoGeneratedCount_{0};
    uint64_t deferredAdrenoBatchId_{0};
    uint32_t deferredAdrenoSourceAge_{0};
    bool deferredAdrenoOutputEligible_{false};
    Mini::Semaphore deferredAdrenoBatchCompleteSemaphore_;
    bool deferredAdrenoBatchCompleteReady_{false};

    // Adreno can intentionally skip private-device work for one or more
    // source-only cycles. Carry the most recent framegen release dependency
    // until the next source copy actually reuses the shared AHB pair.
    Mini::Semaphore conservativePendingBatchCompleteSemaphore_;
    bool conservativePendingBatchCompleteValid_{false};
    int conservativePendingBatchCompletePollFd_{-1};
    VkQueue syntheticQueue_{VK_NULL_HANDLE};

    // Optional fast path only. Prefer one-shot SYNC_FD on Android, retain
    // OPAQUE_FD compatibility, and fall back to the established host fence.
    bool asyncAhbHandoffEnabled_{false};
    // Protected Adreno may export only zero-generation history release as a
    // SYNC_FD. Generated-frame completion remains on the proven bounded host
    // wait; Xclipse/generic keep their existing async generated path.
    bool asyncHistoryCompletionEnabled_{false};
    bool asyncFramegenCompletionEnabled_{false};
    VkExternalSemaphoreHandleTypeFlagBits asyncAhbHandoffHandleType_{
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
    struct RuntimeMetrics {
        using Clock = std::chrono::steady_clock;

        Clock::time_point windowStart{Clock::now()};
        Clock::time_point lastSourcePresent{};
        bool hasLastSourcePresent{false};

        uint64_t windowSourceFrames{0};
        // Legacy generated-frame counters remain WSI-accepted counts for
        // compatibility. The stage counters below make that distinction explicit.
        uint64_t windowGeneratedFrames{0};
        uint64_t windowGeneratedDispatched{0};
        uint64_t windowGeneratedCompleted{0};
        uint64_t windowGeneratedCopySubmitted{0};
        uint64_t windowGeneratedWsiSubmitted{0};
        uint64_t windowGeneratedWsiAccepted{0};
        uint64_t windowGeneratedDisplayConfirmed{0};
        uint64_t windowGeneratedDisplayUnknown{0};
        uint64_t windowSourcePresentFailures{0};
        uint64_t windowGeneratedPresentFailures{0};
        uint64_t totalSourceFrames{0};
        uint64_t totalGeneratedFrames{0};
        uint64_t totalGeneratedDispatched{0};
        uint64_t totalGeneratedCompleted{0};
        uint64_t totalGeneratedCopySubmitted{0};
        uint64_t totalGeneratedWsiSubmitted{0};
        uint64_t totalGeneratedWsiAccepted{0};
        uint64_t totalGeneratedDisplayConfirmed{0};
        uint64_t totalGeneratedDisplayUnknown{0};
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
        uint64_t windowHandoffPrevSourceDeps{0};
        uint64_t windowHandoffBatchDeps{0};
        uint64_t windowHistoryAsyncReleases{0};
        uint64_t totalHistoryAsyncReleases{0};
        uint64_t windowHistoryHostCompletions{0};
        uint64_t totalHistoryHostCompletions{0};
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
        double windowHandoffSubmitMs{0.0};
        double windowHandoffFenceWaitMs{0.0};
        // Zero-generation preprocessing is measured separately from generated
        // dispatch/completion so an S20+ dump can distinguish history cost from
        // source-copy and generated-frame work.
        double windowHistoryPreprocessSubmitMs{0.0};
        double windowHistoryPreprocessHostWaitMs{0.0};
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
        // Zero-generation private preprocessing may release the shared source
        // pair asynchronously. Keep this dependency separate so it can only be
        // consumed by the next source-copy submit, never by generated delivery.
        Mini::Semaphore historyBatchCompleteSemaphore;
        bool historyBatchCompleteValid{false};
        bool deferredAdrenoOwned{false};
#endif

        std::vector<Mini::Semaphore> renderSemaphores; // signal when lsfg is done with frame n

        std::vector<Mini::Semaphore> acquireSemaphores; // signal for swapchain image n

        std::vector<Mini::CommandBuffer> postCopyBufs; // copy from out_n to swapchain image
        std::vector<Mini::Semaphore> postCopySemaphores; // signal when postCopyBuf is done
        std::vector<Mini::Semaphore> prevPostCopySemaphores; // signal for previous postCopyBuf

        // Copies of producer-owned semaphores consumed by this pass's queue
        // submissions. They stay alive until this pass's GPU completion fence
        // proves those waits have executed.
        std::vector<Mini::Semaphore> crossFrameWaitRetentions;

        // GPU-submit retirement only. Present-wait semaphore lifetime is
        // tracked separately by swapchain-image reacquisition.
        std::shared_ptr<VkFence> completionFence;
        bool completionFenceSubmitted{false};

#ifdef __ANDROID__
        // Single-queue Adreno deferred output must retire on the real post-copy
        // submissions that consume output-ready dependencies. An empty submit
        // after vkQueuePresentKHR is not a valid lifetime anchor for this path.
        std::vector<std::shared_ptr<VkFence>> postCopyCompletionFences;
        std::vector<bool> postCopyCompletionFenceSubmitted;
#endif
        bool completionFenceFailed{false};
    }; // data for a single render pass

    bool tryRecyclePass(RenderPassInfo& pass);
    bool submitPassCompletionFence(RenderPassInfo& pass, VkQueue queue);

    std::array<RenderPassInfo, 8> passInfos; // allocate 8 because why not
};

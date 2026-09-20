from __future__ import annotations

import argparse
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one anchor, found {count}")
    return text.replace(old, new, 1)


def patch_context_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "enterSourceOnlyBypass(VkQueue queue)" in text:
        return
    text = replace_once(
        text,
        "    void enterSourceOnlyBypass();\n",
        "    void enterSourceOnlyBypass(VkQueue queue);\n",
        f"{path}: nonblocking resident bypass signature",
    )
    path.write_text(text, encoding="utf-8")


def patch_hooks_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "swapchain.enterSourceOnlyBypass(queue)" in text:
        return
    text = replace_once(
        text,
        "            swapchain.enterSourceOnlyBypass();\n",
        "            swapchain.enterSourceOnlyBypass(queue);\n",
        f"{path}: pass present queue into resident bypass",
    )
    path.write_text(text, encoding="utf-8")


def patch_context_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "nonblocking-generated-pipeline active=1" in text:
        return

    # Android now advances display/source state before selecting the render-pass
    # slot for the newly arriving source. Desktop keeps the original binding.
    text = replace_once(
        text,
        "    const auto& conf = Config::activeConf;\n"
        "    auto& pass = this->passInfos.at(this->frameIdx % 8);\n\n"
        "#ifdef __ANDROID__\n",
        "    const auto& conf = Config::activeConf;\n\n"
        "#ifdef __ANDROID__\n",
        f"{path}: defer Android pass selection",
    )

    # The retained deferred-zero transform retires the current ring-slot handoff
    # fence at function entry. Keep that exact synchronization point, but scope
    # its binding separately so the incoming source can select a new slot after
    # the previously buffered source advances frameIdx.
    early_retire = (
        "    if (pass.handoffFencePending) { waitForAhbHandoff(info.device,*pass.handoffFence,"
        "this->waitHandoffFences); pass.handoffFencePending=false; }\n"
    )
    if early_retire in text:
        text = text.replace(
            early_retire,
            "    {\n"
            "        auto& retiringPass = this->passInfos.at(this->frameIdx % 8);\n"
            "        if (retiringPass.handoffFencePending) {\n"
            "            waitForAhbHandoff(info.device, *retiringPass.handoffFence, "
            "this->waitHandoffFences);\n"
            "            retiringPass.handoffFencePending = false;\n"
            "        }\n"
            "    }\n",
            1,
        )

    text = replace_once(
        text,
        "    const bool warmupSourceHistory =\n"
        "        generatedFrameCount > 0 && this->requiresSourceHistoryWarmup_;\n"
        "    this->lastGeneratedFrameCount_ = generatedFrameCount;\n",
        "    bool warmupSourceHistory =\n"
        "        generatedFrameCount > 0 && this->requiresSourceHistoryWarmup_;\n"
        "    this->lastGeneratedFrameCount_ = 0;\n",
        f"{path}: mutable warmup and delivered-count telemetry",
    )

    text = text.replace(
        '                      << generatedFrameCount << " sourceWait=" << sourceWait << "\\n";',
        '                      << this->lastGeneratedFrameCount_ << " sourceWait=" << sourceWait << "\\n";',
        1,
    )

    # Publish late-drop counters with the normal one-second runtime metrics.
    metrics_anchor = (
        '                      << " ahb_async_fallbacks_total=" << metrics.totalAsyncFallbacks\n'
        '                      << " framegen_dispatch_avg_ms=" << dispatchAvgMs\n'
    )
    if metrics_anchor in text:
        text = text.replace(
            metrics_anchor,
            '                      << " ahb_async_fallbacks_total=" << metrics.totalAsyncFallbacks\n'
            '                      << " generated_late_drops=" << metrics.windowGeneratedLateDrops\n'
            '                      << " generated_late_drops_total=" << metrics.totalGeneratedLateDrops\n'
            '                      << " framegen_dispatch_avg_ms=" << dispatchAvgMs\n',
            1,
        )

    for reset_anchor in (
        "            metrics.windowAsyncHandoffs = 0;\n"
        "            metrics.windowSyncHandoffs = 0;\n",
    ):
        if reset_anchor in text:
            text = text.replace(
                reset_anchor,
                reset_anchor + "            metrics.windowGeneratedLateDrops = 0;\n",
            )

    finish_end = (
        "        this->frameIdx++;\n"
        "        return result;\n"
        "    };\n\n"
    )
    if finish_end not in text:
        raise RuntimeError(f"{path}: finishSourcePresent anchor missing")

    buffered = r'''        this->frameIdx++;
        return result;
    };

    // nonblocking-generated-pipeline active=1 queue_target=1 sourceWait=buffered-source
    //
    // Every generated batch gets one real-source interval to finish. Polling
    // waitContext with timeout=0 records completion/timing when ready but never
    // stalls the game. A missed batch is discarded before the buffered real
    // frame advances, so interpolation cannot reduce source throughput.
    VkResult bufferedPresentResult = VK_SUCCESS;
    size_t deliveredGeneratedCount = 0;
    if (this->framegenInFlight_) {
        const bool framegenReady = conf.performance
            ? LSFG_3_1P::waitContext(*this->lsfgCtxId, 0)
            : LSFG_3_1::waitContext(*this->lsfgCtxId, 0);
        if (framegenReady) {
            this->framegenInFlight_ = false;
            updateAdaptiveFlowGovernor();
        } else if (this->pendingSourceValid_ && this->framegenOutputEligible_) {
            metrics.windowGeneratedLateDrops += this->pendingGeneratedCount_;
            metrics.totalGeneratedLateDrops += this->pendingGeneratedCount_;
            std::cerr << "lsfg-vk: framegen-late-drop generated="
                      << this->pendingGeneratedCount_
                      << " action=preserve-source-cadence\n";
            this->framegenOutputEligible_ = false;
            this->requiresSourceHistoryWarmup_ = true;
            this->previousSourceCopySignalValid_ = false;
        }
    }

    if (this->pendingSourceValid_) {
        auto& pendingPass = this->passInfos.at(this->pendingPassIndex_);

        if (!this->framegenInFlight_
                && this->framegenOutputEligible_
                && this->pendingGeneratedCount_ > 0) {
            for (size_t i = 0; i < this->pendingGeneratedCount_; ++i) {
                const auto generatedPresentStart = RuntimeMetrics::Clock::now();
                pendingPass.acquireSemaphores.at(i) = Mini::Semaphore(info.device);
                uint32_t imageIdx{};
                auto generatedResult = Layer::ovkAcquireNextImageKHR(
                    info.device, this->swapchain, runtimeWaitTimeoutNs(),
                    pendingPass.acquireSemaphores.at(i).handle(),
                    VK_NULL_HANDLE, &imageIdx);
                if (generatedResult != VK_SUCCESS
                        && generatedResult != VK_SUBOPTIMAL_KHR) {
                    metrics.windowGeneratedPresentFailures++;
                    metrics.totalGeneratedPresentFailures++;
                    throw LSFG::vulkan_error(
                        generatedResult, "Failed to acquire generated swapchain image");
                }

                pendingPass.postCopySemaphores.at(i) = Mini::Semaphore(info.device);
                pendingPass.prevPostCopySemaphores.at(i) = Mini::Semaphore(info.device);
                pendingPass.postCopyBufs.at(i) =
                    Mini::CommandBuffer(info.device, this->cmdPool);
                pendingPass.postCopyBufs.at(i).begin();
                copyExternalAhbToSwapchain(
                    pendingPass.postCopyBufs.at(i).handle(),
                    this->out_n.at(i).handle(),
                    this->swapchainImages.at(imageIdx),
                    this->extent.width, this->extent.height,
                    info.queue.first);
                pendingPass.postCopyBufs.at(i).end();
                pendingPass.postCopyBufs.at(i).submit(
                    info.queue.second,
                    { pendingPass.acquireSemaphores.at(i).handle() },
                    { pendingPass.postCopySemaphores.at(i).handle(),
                      pendingPass.prevPostCopySemaphores.at(i).handle() });

                std::vector<VkSemaphore> generatedWaits{
                    pendingPass.postCopySemaphores.at(i).handle()
                };
                if (i != 0) {
                    generatedWaits.emplace_back(
                        pendingPass.prevPostCopySemaphores.at(i - 1).handle());
                }

                VkPresentTimeGOOGLE generatedPresentTime{};
                VkPresentTimesInfoGOOGLE generatedPresentTimes{};
                const VkPresentInfoKHR generatedPresentInfo{
                    .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                    .pNext = adaptivePresentPNext(
                        i == 0 ? pNext : nullptr,
                        generatedPresentTime,
                        generatedPresentTimes),
                    .waitSemaphoreCount =
                        static_cast<uint32_t>(generatedWaits.size()),
                    .pWaitSemaphores = generatedWaits.data(),
                    .swapchainCount = 1,
                    .pSwapchains = &this->swapchain,
                    .pImageIndices = &imageIdx,
                };
                generatedResult =
                    Layer::ovkQueuePresentKHR(queue, &generatedPresentInfo);
                if (generatedResult != VK_SUCCESS
                        && generatedResult != VK_SUBOPTIMAL_KHR) {
                    metrics.windowGeneratedPresentFailures++;
                    metrics.totalGeneratedPresentFailures++;
                    throw LSFG::vulkan_error(
                        generatedResult, "Failed to present generated swapchain image");
                }

                metrics.windowGeneratedFrames++;
                metrics.totalGeneratedFrames++;
                metrics.windowGeneratedPresentMs +=
                    std::chrono::duration<double, std::milli>(
                        RuntimeMetrics::Clock::now()
                        - generatedPresentStart).count();
                ++deliveredGeneratedCount;
            }
        }

        VkSemaphore pendingSourceWait = this->pendingSourceReady_.handle();
        if (deliveredGeneratedCount > 0) {
            pendingSourceWait = pendingPass.prevPostCopySemaphores
                .at(deliveredGeneratedCount - 1).handle();
        }

        VkPresentTimeGOOGLE bufferedSourcePresentTime{};
        VkPresentTimesInfoGOOGLE bufferedSourcePresentTimes{};
        const VkPresentInfoKHR bufferedSourcePresentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = adaptivePresentPNext(
                deliveredGeneratedCount == 0 ? pNext : nullptr,
                bufferedSourcePresentTime,
                bufferedSourcePresentTimes),
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &pendingSourceWait,
            .swapchainCount = 1,
            .pSwapchains = &this->swapchain,
            .pImageIndices = &this->pendingSourceImage_,
        };
        bufferedPresentResult =
            Layer::ovkQueuePresentKHR(queue, &bufferedSourcePresentInfo);
        if (bufferedPresentResult != VK_SUCCESS
                && bufferedPresentResult != VK_SUBOPTIMAL_KHR) {
            metrics.windowSourcePresentFailures++;
            metrics.totalSourcePresentFailures++;
            throw LSFG::vulkan_error(
                bufferedPresentResult, "Failed to present buffered source frame");
        }

        this->lastGeneratedFrameCount_ = deliveredGeneratedCount;
        this->pendingSourceValid_ = false;
        this->pendingGeneratedCount_ = 0;
        this->framegenOutputEligible_ = false;
        (void)finishSourcePresent(bufferedPresentResult, "buffered-source");
    }

    auto& pass = this->passInfos.at(this->frameIdx % 8);
    warmupSourceHistory =
        generatedFrameCount > 0 && this->requiresSourceHistoryWarmup_;

    const auto bufferSourceOnly = [&]() -> VkResult {
        pass.preCopySemaphores.at(0) = Mini::Semaphore(info.device);
        pass.preCopyBuf = Mini::CommandBuffer(info.device, this->cmdPool);
        pass.preCopyBuf.begin();
        pass.preCopyBuf.end();
        pass.preCopyBuf.submit(
            info.queue.second,
            gameRenderSemaphores,
            { pass.preCopySemaphores.at(0).handle() });

        this->pendingSourceValid_ = true;
        this->pendingSourceImage_ = presentIdx;
        this->pendingSourceReady_ = pass.preCopySemaphores.at(0);
        this->pendingPassIndex_ = this->frameIdx % 8;
        this->pendingGeneratedCount_ = 0;
        this->lastGeneratedFrameCount_ = deliveredGeneratedCount;
        this->requiresSourceHistoryWarmup_ = true;
        this->previousSourceCopySignalValid_ = false;
        return bufferedPresentResult;
    };

    // Below the AFG 10 FPS cutoff no interpolation/preprocessing work is
    // submitted. Likewise, an older framegen batch that is still executing may
    // never make the new real frame wait for shared AHB ownership.
    if ((conf.adaptiveFramegen && adaptiveTelemetry.lowFpsCutoff)
            || this->framegenInFlight_) {
        return bufferSourceOnly();
    }

'''
    text = text.replace(finish_end, buffered, 1)

    # Buffer fractional-zero source frames after their existing history refresh.
    zero_return = '        return finishSourcePresent(adaptiveSourceResult, "pre-copy-adaptive-zero");\n'
    if zero_return not in text:
        raise RuntimeError(f"{path}: adaptive zero source-return anchor missing")
    zero_start = text.rfind(
        "        const VkSemaphore sourceReady = pass.preCopySemaphores.at(0).handle();",
        0, text.index(zero_return) + len(zero_return))
    if zero_start < 0:
        raise RuntimeError(f"{path}: adaptive zero source-present start missing")
    zero_end = text.index(zero_return, zero_start) + len(zero_return)
    zero_tail = r'''        this->pendingSourceValid_ = true;
        this->pendingSourceImage_ = presentIdx;
        this->pendingSourceReady_ = pass.preCopySemaphores.at(0);
        this->pendingPassIndex_ = this->frameIdx % 8;
        this->pendingGeneratedCount_ = 0;
        this->lastGeneratedFrameCount_ = deliveredGeneratedCount;
        return bufferedPresentResult;
'''
    text = text[:zero_start] + zero_tail + text[zero_end:]

    # Warm-up is also a buffered real frame; keeping queue depth constant avoids
    # a two-source burst when generation resumes.
    warm_return = '        return finishSourcePresent(warmupResult, "pre-copy-warmup");\n'
    if warm_return not in text:
        raise RuntimeError(f"{path}: warmup source-return anchor missing")
    warm_start = text.rfind(
        "        const VkSemaphore sourceReady = pass.preCopySemaphores.at(0).handle();",
        0, text.index(warm_return) + len(warm_return))
    if warm_start < 0:
        raise RuntimeError(f"{path}: warmup source-present start missing")
    warm_end = text.index(warm_return, warm_start) + len(warm_return)
    warm_tail = r'''        this->pendingSourceValid_ = true;
        this->pendingSourceImage_ = presentIdx;
        this->pendingSourceReady_ = pass.preCopySemaphores.at(0);
        this->pendingPassIndex_ = this->frameIdx % 8;
        this->pendingGeneratedCount_ = 0;
        this->lastGeneratedFrameCount_ = deliveredGeneratedCount;
        std::cerr << "lsfg-vk: runtime stage=source-history-warmup-buffered\n";
        return bufferedPresentResult;
'''
    text = text[:warm_start] + warm_tail + text[warm_end:]

    # Replace the blocking completion/presentation tail. Completion is polled on
    # the next source boundary by the queue-target block above.
    wait_start_marker = (
        "    // 3. Ensure framegen's separate VkDevice has completed its release barriers\n"
    )
    wait_start = text.find(wait_start_marker)
    if wait_start < 0:
        raise RuntimeError(f"{path}: blocking framegen wait start missing")
    tail_marker = '    return finishSourcePresent(res, "prev-post-copy");\n'
    wait_end_pos = text.find(tail_marker, wait_start)
    if wait_end_pos < 0:
        raise RuntimeError(f"{path}: blocking generated-present tail missing")
    wait_end = wait_end_pos + len(tail_marker)

    deferred = r'''    // The framegen job is intentionally left in flight. The next real-frame
    // boundary polls it with timeout=0; ready output is inserted before this
    // buffered source, while late output is dropped without delaying source FPS.
    this->framegenInFlight_ = true;
    this->framegenOutputEligible_ = true;
    this->pendingSourceValid_ = true;
    this->pendingSourceImage_ = presentIdx;
    this->pendingSourceReady_ = pass.preCopySemaphores.at(0);
    this->pendingPassIndex_ = this->frameIdx % 8;
    this->pendingGeneratedCount_ = generatedFrameCount;
    this->lastGeneratedFrameCount_ = deliveredGeneratedCount;
    std::cerr << "lsfg-vk: nonblocking-generated-pipeline active=1"
              << " generated=" << generatedFrameCount
              << " queue_target=1\n";
    return bufferedPresentResult;
'''
    text = text[:wait_start] + deferred + text[wait_end:]

    # Desktop retains the original immediate path and therefore still needs its
    # local render-pass binding after the Android-only block.
    desktop_marker = "#else\n    // Desktop Linux path:"
    if desktop_marker not in text:
        raise RuntimeError(f"{path}: desktop branch anchor missing")
    text = text.replace(
        desktop_marker,
        "#else\n"
        "    auto& pass = this->passInfos.at(this->frameIdx % 8);\n"
        "    // Desktop Linux path:",
        1,
    )

    # A resident Off/On toggle is not destruction. The previous lifecycle
    # hardening deliberately drained outstanding Android work here, but that is a
    # host wait and can recreate the user-visible hitch this pipeline removes.
    # soft resident bypass never drains framegen synchronously: enqueue the one
    # buffered real image first, invalidate synthetic output, and leave any
    # framegen/zero-history work resident for later nonblocking retirement.
    hardened_bypass = """void LsContext::enterSourceOnlyBypass() {
    this->flushPendingAndroidWork(true);
    this->lastGeneratedFrameCount_ = 0;
    this->requiresSourceHistoryWarmup_ = true;
    this->previousSourceCopySignalValid_ = false;
}
"""
    deferred_hardened_bypass = """void LsContext::enterSourceOnlyBypass() {
    this->flushPendingAndroidWork(true);
    this->invalidateDeferredZeroHistory();
    this->lastGeneratedFrameCount_ = 0;
    this->requiresSourceHistoryWarmup_ = true;
    this->previousSourceCopySignalValid_ = false;
}
"""
    base_bypass = """void LsContext::enterSourceOnlyBypass() {
    this->lastGeneratedFrameCount_ = 0;
    this->requiresSourceHistoryWarmup_ = true;
    this->previousSourceCopySignalValid_ = false;
}
"""
    nonblocking_bypass = """void LsContext::enterSourceOnlyBypass(VkQueue queue) {
    if (this->pendingSourceValid_) {
        const VkSemaphore sourceReady = this->pendingSourceReady_.handle();
        const VkPresentInfoKHR pendingSourceInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = nullptr,
            .waitSemaphoreCount = sourceReady != VK_NULL_HANDLE ? 1U : 0U,
            .pWaitSemaphores = sourceReady != VK_NULL_HANDLE ? &sourceReady : nullptr,
            .swapchainCount = 1,
            .pSwapchains = &this->swapchain,
            .pImageIndices = &this->pendingSourceImage_,
        };
        const auto result = Layer::ovkQueuePresentKHR(queue, &pendingSourceInfo);
        std::cerr << "lsfg-vk: soft-bypass-buffered-source result="
                  << result << "\\n";
        this->pendingSourceValid_ = false;
    }

    this->invalidateDeferredZeroHistory();
    this->framegenOutputEligible_ = false;
    this->pendingGeneratedCount_ = 0;
    this->lastGeneratedFrameCount_ = 0;
    this->requiresSourceHistoryWarmup_ = true;
    this->previousSourceCopySignalValid_ = false;
}
"""
    if deferred_hardened_bypass in text:
        text = text.replace(deferred_hardened_bypass, nonblocking_bypass, 1)
    elif hardened_bypass in text:
        text = text.replace(hardened_bypass, nonblocking_bypass, 1)
    elif base_bypass in text:
        text = text.replace(base_bypass, nonblocking_bypass, 1)
    elif "soft-bypass-buffered-source" not in text:
        raise RuntimeError(f"{path}: source-only bypass anchor missing")

    path.write_text(text, encoding="utf-8")


def patch_build(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    invocation = 'python3 "${REPO_ROOT}/scripts/adreno_nonblocking_generated_pipeline.py" --root "${REPO_ROOT}"\n'
    if "adreno_nonblocking_generated_pipeline.py" in text:
        return
    anchor = 'python3 "${REPO_ROOT}/scripts/apply-adreno-evidence-profile.py" --root "${REPO_ROOT}"\n'
    if anchor not in text:
        raise RuntimeError(f"{path}: evidence-profile build anchor missing")
    text = text.replace(anchor, anchor + invocation, 1)
    path.write_text(text, encoding="utf-8")


def apply(root: Path) -> None:
    patch_context_header(root / "include/context.hpp")
    patch_context_source(root / "src/context.cpp")
    patch_hooks_source(root / "src/hooks.cpp")
    patch_build(root / "scripts/build/android.sh")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    patch_context_header(root / "include/context.hpp")
    patch_context_source(root / "src/context.cpp")
    patch_hooks_source(root / "src/hooks.cpp")


if __name__ == "__main__":
    main()

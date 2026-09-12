from __future__ import annotations

from pathlib import Path
from adreno_evidence_common import replace_exact


def patch_outer_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if ("windowHandoffHostWaitMs" in text and "lastDiagnosticStage()" in text
            and "gameSourceCopyQueryPool" in text):
        return

    text = replace_exact(
        text,
        "#ifdef __ANDROID__\n"
        "    void enterSourceOnlyBypass();\n"
        "#endif\n",
        "#ifdef __ANDROID__\n"
        "    [[nodiscard]] const char* lastDiagnosticStage() const noexcept {\n"
        "        return diagnosticStage_;\n"
        "    }\n"
        "    void enterSourceOnlyBypass();\n"
        "#endif\n",
        count=1,
        label=f"{path}: diagnostic stage accessor",
    )
    text = replace_exact(
        text,
        "    AdaptiveFrameScheduler adaptiveScheduler_;\n",
        "    const char* diagnosticStage_{\"idle\"};\n"
        "    AdaptiveFrameScheduler adaptiveScheduler_;\n",
        count=1,
        label=f"{path}: diagnostic stage member",
    )
    text = replace_exact(
        text,
        "        double windowHandoffMs{0.0};\n",
        "        double windowHandoffMs{0.0};\n"
        "        double windowHandoffSubmitCpuMs{0.0};\n"
        "        double windowHandoffHostWaitMs{0.0};\n"
        "        double windowHandoffAsyncSubmitCpuMs{0.0};\n"
        "        double windowSourceCopyGpuMs{0.0};\n"
        "        uint64_t windowSourceCopyGpuSamples{0};\n",
        count=1,
        label=f"{path}: split handoff/source-copy metrics",
    )
    text = replace_exact(
        text,
        "    PFN_vkWaitForFences waitHandoffFences{nullptr};\n",
        "    PFN_vkWaitForFences waitHandoffFences{nullptr};\n\n"
        "    // Diagnostic-only timestamp pool on the game VkDevice. It is used\n"
        "    // only on the established synchronous host-fence path, so query\n"
        "    // results are read after the existing fence wait with no new sync.\n"
        "    std::shared_ptr<VkQueryPool> gameSourceCopyQueryPool;\n"
        "    PFN_vkCmdResetQueryPool gameSourceCopyCmdResetQueryPool{nullptr};\n"
        "    PFN_vkCmdWriteTimestamp gameSourceCopyCmdWriteTimestamp{nullptr};\n"
        "    PFN_vkGetQueryPoolResults gameSourceCopyGetQueryPoolResults{nullptr};\n"
        "    uint32_t gameSourceCopyTimestampValidBits{0};\n"
        "    float gameSourceCopyTimestampPeriodNs{0.0f};\n"
        "    bool gameSourceCopyGpuTimingEnabled_{false};\n",
        count=1,
        label=f"{path}: source-copy query state",
    )
    path.write_text(text, encoding="utf-8")


def patch_outer_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if ("ahb_host_wait_avg_ms=" in text and "diagnosticStage_ = \"source-handoff\"" in text
            and "source_copy_gpu_avg_ms=" in text):
        return

    text = replace_exact(
        text,
        "    this->asyncAhbHandoffEnabled_ =\n"
        "        info.androidOpaqueFdSemaphoreSupported\n"
        "        && backendDiagnostics.externalSemaphoreOpaqueFd\n"
        "        && gameGetSemaphoreFd != nullptr;\n\n"
        "    std::cerr << \"lsfg-vk: Android AHB context created (id=\" << ctxId\n",
        "    this->asyncAhbHandoffEnabled_ =\n"
        "        info.androidOpaqueFdSemaphoreSupported\n"
        "        && backendDiagnostics.externalSemaphoreOpaqueFd\n"
        "        && gameGetSemaphoreFd != nullptr;\n\n"
        "    const auto createSourceCopyQueryPool = reinterpret_cast<PFN_vkCreateQueryPool>(\n"
        "        Layer::ovkGetDeviceProcAddr(info.device, \"vkCreateQueryPool\"));\n"
        "    const auto destroySourceCopyQueryPool = reinterpret_cast<PFN_vkDestroyQueryPool>(\n"
        "        Layer::ovkGetDeviceProcAddr(info.device, \"vkDestroyQueryPool\"));\n"
        "    this->gameSourceCopyCmdResetQueryPool = reinterpret_cast<PFN_vkCmdResetQueryPool>(\n"
        "        Layer::ovkGetDeviceProcAddr(info.device, \"vkCmdResetQueryPool\"));\n"
        "    this->gameSourceCopyCmdWriteTimestamp = reinterpret_cast<PFN_vkCmdWriteTimestamp>(\n"
        "        Layer::ovkGetDeviceProcAddr(info.device, \"vkCmdWriteTimestamp\"));\n"
        "    this->gameSourceCopyGetQueryPoolResults = reinterpret_cast<PFN_vkGetQueryPoolResults>(\n"
        "        Layer::ovkGetDeviceProcAddr(info.device, \"vkGetQueryPoolResults\"));\n"
        "    uint32_t sourceCopyFamilyCount = 0;\n"
        "    Layer::ovkGetPhysicalDeviceQueueFamilyProperties(\n"
        "        info.physicalDevice, &sourceCopyFamilyCount, nullptr);\n"
        "    if (!this->asyncAhbHandoffEnabled_\n"
        "            && info.queue.first < sourceCopyFamilyCount\n"
        "            && createSourceCopyQueryPool != nullptr\n"
        "            && destroySourceCopyQueryPool != nullptr\n"
        "            && this->gameSourceCopyCmdResetQueryPool != nullptr\n"
        "            && this->gameSourceCopyCmdWriteTimestamp != nullptr\n"
        "            && this->gameSourceCopyGetQueryPoolResults != nullptr) {\n"
        "        std::vector<VkQueueFamilyProperties> sourceCopyFamilies(sourceCopyFamilyCount);\n"
        "        Layer::ovkGetPhysicalDeviceQueueFamilyProperties(\n"
        "            info.physicalDevice, &sourceCopyFamilyCount, sourceCopyFamilies.data());\n"
        "        this->gameSourceCopyTimestampValidBits =\n"
        "            sourceCopyFamilies.at(info.queue.first).timestampValidBits;\n"
        "        VkPhysicalDeviceProperties sourceCopyProperties{};\n"
        "        Layer::ovkGetPhysicalDeviceProperties(info.physicalDevice, &sourceCopyProperties);\n"
        "        this->gameSourceCopyTimestampPeriodNs = sourceCopyProperties.limits.timestampPeriod;\n"
        "        if (this->gameSourceCopyTimestampValidBits > 0\n"
        "                && this->gameSourceCopyTimestampPeriodNs > 0.0f) {\n"
        "            const VkQueryPoolCreateInfo queryPoolInfo{\n"
        "                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,\n"
        "                .queryType = VK_QUERY_TYPE_TIMESTAMP,\n"
        "                .queryCount = 2,\n"
        "            };\n"
        "            VkQueryPool queryPool = VK_NULL_HANDLE;\n"
        "            if (createSourceCopyQueryPool(\n"
        "                    info.device, &queryPoolInfo, nullptr, &queryPool) == VK_SUCCESS\n"
        "                    && queryPool != VK_NULL_HANDLE) {\n"
        "                this->gameSourceCopyQueryPool = std::shared_ptr<VkQueryPool>(\n"
        "                    new VkQueryPool(queryPool),\n"
        "                    [device = info.device, destroySourceCopyQueryPool](VkQueryPool* ownedPool) {\n"
        "                        if (ownedPool != nullptr) {\n"
        "                            if (*ownedPool != VK_NULL_HANDLE)\n"
        "                                destroySourceCopyQueryPool(device, *ownedPool, nullptr);\n"
        "                            delete ownedPool;\n"
        "                        }\n"
        "                    });\n"
        "                this->gameSourceCopyGpuTimingEnabled_ = true;\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    std::cerr << \"lsfg-vk: game-source-copy-profile supported=\"\n"
        "              << (this->gameSourceCopyGpuTimingEnabled_ ? 1 : 0)\n"
        "              << \" timestampValidBits=\" << this->gameSourceCopyTimestampValidBits\n"
        "              << \" timestampPeriodNs=\" << this->gameSourceCopyTimestampPeriodNs\n"
        "              << \" asyncHandoff=\" << (this->asyncAhbHandoffEnabled_ ? 1 : 0)\n"
        "              << \"\\n\";\n\n"
        "    std::cerr << \"lsfg-vk: Android AHB context created (id=\" << ctxId\n",
        count=1,
        label=f"{path}: source-copy query pool setup",
    )

    text = replace_exact(
        text,
        "    const auto cycleStart = RuntimeMetrics::Clock::now();\n",
        "    const auto cycleStart = RuntimeMetrics::Clock::now();\n"
        "    this->diagnosticStage_ = \"cycle-start\";\n",
        count=1,
        label=f"{path}: cycle diagnostic stage",
    )
    text = replace_exact(
        text,
        "        this->frameIdx++;\n"
        "        return result;\n"
        "    };\n",
        "        this->diagnosticStage_ = \"idle\";\n"
        "        this->frameIdx++;\n"
        "        return result;\n"
        "    };\n",
        count=1,
        label=f"{path}: successful stage reset",
    )
    text = replace_exact(
        text,
        "    pass.preCopySemaphores.at(0) = Mini::Semaphore(info.device);\n",
        "    this->diagnosticStage_ = \"source-copy-record\";\n"
        "    pass.preCopySemaphores.at(0) = Mini::Semaphore(info.device);\n",
        count=1,
        label=f"{path}: source-copy stage",
    )
    text = replace_exact(
        text,
        "    pass.preCopyBuf.begin();\n\n"
        "    copySwapchainToExternalAhb(pass.preCopyBuf.handle(),\n",
        "    pass.preCopyBuf.begin();\n"
        "    const bool profileGameSourceCopyGpu = this->gameSourceCopyGpuTimingEnabled_\n"
        "        && this->gameSourceCopyQueryPool != nullptr;\n"
        "    if (profileGameSourceCopyGpu) {\n"
        "        this->gameSourceCopyCmdResetQueryPool(\n"
        "            pass.preCopyBuf.handle(), *this->gameSourceCopyQueryPool, 0, 2);\n"
        "        this->gameSourceCopyCmdWriteTimestamp(\n"
        "            pass.preCopyBuf.handle(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,\n"
        "            *this->gameSourceCopyQueryPool, 0);\n"
        "    }\n\n"
        "    copySwapchainToExternalAhb(pass.preCopyBuf.handle(),\n",
        count=1,
        label=f"{path}: source-copy GPU profile begin",
    )
    text = replace_exact(
        text,
        "        info.queue.first, this->frameIdx < 2);\n\n"
        "    pass.preCopyBuf.end();\n",
        "        info.queue.first, this->frameIdx < 2);\n"
        "    if (profileGameSourceCopyGpu)\n"
        "        this->gameSourceCopyCmdWriteTimestamp(\n"
        "            pass.preCopyBuf.handle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,\n"
        "            *this->gameSourceCopyQueryPool, 1);\n\n"
        "    pass.preCopyBuf.end();\n",
        count=1,
        label=f"{path}: source-copy GPU profile end",
    )
    text = replace_exact(
        text,
        "    const auto handoffStart = RuntimeMetrics::Clock::now();\n",
        "    this->diagnosticStage_ = \"source-handoff\";\n"
        "    const auto handoffStart = RuntimeMetrics::Clock::now();\n",
        count=1,
        label=f"{path}: source handoff stage",
    )

    old_handoff = (
        "    if (useAsyncHandoff) {\n"
        "        submitAhbHandoff(info.device, pass.preCopyBuf, info.queue.second,\n"
        "            gameRenderSemaphores2, preCopySignals,\n"
        "            *this->ahbHandoffFence, this->resetHandoffFences);\n"
        "        metrics.windowAsyncHandoffs++;\n"
        "        metrics.totalAsyncHandoffs++;\n"
        "    } else {\n"
        "        submitAndWaitForAhbHandoff(info.device, pass.preCopyBuf, info.queue.second,\n"
        "            gameRenderSemaphores2, preCopySignals,\n"
        "            *this->ahbHandoffFence, this->resetHandoffFences,\n"
        "            this->waitHandoffFences);\n"
        "        metrics.windowSyncHandoffs++;\n"
        "        metrics.totalSyncHandoffs++;\n"
        "    }\n"
    )
    new_handoff = (
        "    if (useAsyncHandoff) {\n"
        "        const auto asyncSubmitStart = RuntimeMetrics::Clock::now();\n"
        "        submitAhbHandoff(info.device, pass.preCopyBuf, info.queue.second,\n"
        "            gameRenderSemaphores2, preCopySignals,\n"
        "            *this->ahbHandoffFence, this->resetHandoffFences);\n"
        "        metrics.windowHandoffAsyncSubmitCpuMs +=\n"
        "            std::chrono::duration<double, std::milli>(\n"
        "                RuntimeMetrics::Clock::now() - asyncSubmitStart).count();\n"
        "        metrics.windowAsyncHandoffs++;\n"
        "        metrics.totalAsyncHandoffs++;\n"
        "    } else {\n"
        "        const auto submitStart = RuntimeMetrics::Clock::now();\n"
        "        submitAhbHandoff(info.device, pass.preCopyBuf, info.queue.second,\n"
        "            gameRenderSemaphores2, preCopySignals,\n"
        "            *this->ahbHandoffFence, this->resetHandoffFences);\n"
        "        metrics.windowHandoffSubmitCpuMs += std::chrono::duration<double, std::milli>(\n"
        "            RuntimeMetrics::Clock::now() - submitStart).count();\n"
        "        const auto hostWaitStart = RuntimeMetrics::Clock::now();\n"
        "        waitForAhbHandoff(info.device, *this->ahbHandoffFence, this->waitHandoffFences);\n"
        "        metrics.windowHandoffHostWaitMs += std::chrono::duration<double, std::milli>(\n"
        "            RuntimeMetrics::Clock::now() - hostWaitStart).count();\n"
        "        if (profileGameSourceCopyGpu) {\n"
        "            std::array<uint64_t, 2> sourceCopyTimestamps{};\n"
        "            const auto queryResult = this->gameSourceCopyGetQueryPoolResults(\n"
        "                info.device, *this->gameSourceCopyQueryPool, 0, 2,\n"
        "                sizeof(sourceCopyTimestamps), sourceCopyTimestamps.data(),\n"
        "                sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);\n"
        "            if (queryResult == VK_SUCCESS) {\n"
        "                const uint32_t validBits = std::min<uint32_t>(\n"
        "                    this->gameSourceCopyTimestampValidBits, 64);\n"
        "                const uint64_t mask = validBits == 64\n"
        "                    ? ~uint64_t{0}\n"
        "                    : ((uint64_t{1} << validBits) - 1);\n"
        "                const uint64_t delta =\n"
        "                    ((sourceCopyTimestamps.at(1) & mask)\n"
        "                        - (sourceCopyTimestamps.at(0) & mask)) & mask;\n"
        "                metrics.windowSourceCopyGpuMs +=\n"
        "                    (static_cast<double>(delta)\n"
        "                        * static_cast<double>(this->gameSourceCopyTimestampPeriodNs))\n"
        "                    / 1000000.0;\n"
        "                metrics.windowSourceCopyGpuSamples++;\n"
        "            }\n"
        "        }\n"
        "        metrics.windowSyncHandoffs++;\n"
        "        metrics.totalSyncHandoffs++;\n"
        "    }\n"
    )
    text = replace_exact(text, old_handoff, new_handoff, count=1, label=f"{path}: split handoff timing")

    text = replace_exact(
        text,
        "            const double handoffAvgMs = sourceCount > 0.0 ? metrics.windowHandoffMs / sourceCount : 0.0;\n",
        "            const double handoffAvgMs = sourceCount > 0.0 ? metrics.windowHandoffMs / sourceCount : 0.0;\n"
        "            const double handoffSubmitCpuAvgMs = metrics.windowSyncHandoffs > 0\n"
        "                ? metrics.windowHandoffSubmitCpuMs / static_cast<double>(metrics.windowSyncHandoffs) : 0.0;\n"
        "            const double handoffHostWaitAvgMs = metrics.windowSyncHandoffs > 0\n"
        "                ? metrics.windowHandoffHostWaitMs / static_cast<double>(metrics.windowSyncHandoffs) : 0.0;\n"
        "            const double handoffAsyncSubmitCpuAvgMs = metrics.windowAsyncHandoffs > 0\n"
        "                ? metrics.windowHandoffAsyncSubmitCpuMs / static_cast<double>(metrics.windowAsyncHandoffs) : 0.0;\n"
        "            const double sourceCopyGpuAvgMs = metrics.windowSourceCopyGpuSamples > 0\n"
        "                ? metrics.windowSourceCopyGpuMs / static_cast<double>(metrics.windowSourceCopyGpuSamples) : 0.0;\n",
        count=1,
        label=f"{path}: handoff/source-copy averages",
    )
    text = replace_exact(
        text,
        "                      << \" ahb_handoff_avg_ms=\" << handoffAvgMs\n",
        "                      << \" ahb_handoff_avg_ms=\" << handoffAvgMs\n"
        "                      << \" ahb_submit_cpu_avg_ms=\" << handoffSubmitCpuAvgMs\n"
        "                      << \" ahb_host_wait_avg_ms=\" << handoffHostWaitAvgMs\n"
        "                      << \" ahb_async_submit_cpu_avg_ms=\" << handoffAsyncSubmitCpuAvgMs\n"
        "                      << \" source_copy_gpu_avg_ms=\" << sourceCopyGpuAvgMs\n"
        "                      << \" source_copy_gpu_samples=\" << metrics.windowSourceCopyGpuSamples\n",
        count=1,
        label=f"{path}: handoff/source-copy metric log",
    )
    text = replace_exact(
        text,
        "            metrics.windowHandoffMs = 0.0;\n",
        "            metrics.windowHandoffMs = 0.0;\n"
        "            metrics.windowHandoffSubmitCpuMs = 0.0;\n"
        "            metrics.windowHandoffHostWaitMs = 0.0;\n"
        "            metrics.windowHandoffAsyncSubmitCpuMs = 0.0;\n"
        "            metrics.windowSourceCopyGpuMs = 0.0;\n"
        "            metrics.windowSourceCopyGpuSamples = 0;\n",
        count=1,
        label=f"{path}: handoff/source-copy metric reset",
    )

    stage_markers = (
        ("    if (adaptiveZeroGeneration) {\n", "adaptive-zero"),
        ("    if (warmupSourceHistory) {\n", "source-history-warmup"),
        ("    // 2. Tell framegen to generate intermediary frames.", "framegen-dispatch"),
        ("    // 3. Ensure framegen's separate VkDevice has completed its release barriers", "framegen-wait"),
    )
    for marker, stage in stage_markers:
        if marker.startswith("    //"):
            text = replace_exact(
                text, marker,
                f"    this->diagnosticStage_ = \"{stage}\";\n\n" + marker,
                count=1, label=f"{path}: {stage} stage",
            )
        else:
            text = replace_exact(
                text, marker,
                f"    this->diagnosticStage_ = \"{stage}\";\n" + marker,
                count=1, label=f"{path}: {stage} stage",
            )

    text = replace_exact(
        text,
        "    // 4. Copy generated frames to swapchain images and present them. Each\n",
        "    this->diagnosticStage_ = \"generated-present\";\n"
        "    // 4. Copy generated frames to swapchain images and present them. Each\n",
        count=1,
        label=f"{path}: generated present stage",
    )
    path.write_text(text, encoding="utf-8")

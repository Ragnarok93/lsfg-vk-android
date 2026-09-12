from __future__ import annotations

from pathlib import Path
from adreno_evidence_common import replace_exact

def patch_outer_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "windowHandoffHostWaitMs" in text and "lastDiagnosticStage()" in text:
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
        "        double windowHandoffAsyncSubmitCpuMs{0.0};\n",
        count=1,
        label=f"{path}: split handoff metrics",
    )
    path.write_text(text, encoding="utf-8")

def patch_outer_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "ahb_host_wait_avg_ms=" in text and "diagnosticStage_ = \"source-handoff\"" in text:
        return

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
        "                ? metrics.windowHandoffAsyncSubmitCpuMs / static_cast<double>(metrics.windowAsyncHandoffs) : 0.0;\n",
        count=1,
        label=f"{path}: handoff averages",
    )
    text = replace_exact(
        text,
        "                      << \" ahb_handoff_avg_ms=\" << handoffAvgMs\n",
        "                      << \" ahb_handoff_avg_ms=\" << handoffAvgMs\n"
        "                      << \" ahb_submit_cpu_avg_ms=\" << handoffSubmitCpuAvgMs\n"
        "                      << \" ahb_host_wait_avg_ms=\" << handoffHostWaitAvgMs\n"
        "                      << \" ahb_async_submit_cpu_avg_ms=\" << handoffAsyncSubmitCpuAvgMs\n",
        count=1,
        label=f"{path}: handoff metric log",
    )
    text = replace_exact(
        text,
        "            metrics.windowHandoffMs = 0.0;\n",
        "            metrics.windowHandoffMs = 0.0;\n"
        "            metrics.windowHandoffSubmitCpuMs = 0.0;\n"
        "            metrics.windowHandoffHostWaitMs = 0.0;\n"
        "            metrics.windowHandoffAsyncSubmitCpuMs = 0.0;\n",
        count=1,
        label=f"{path}: handoff metric reset",
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

    # Mark the generated-frame copy/present section using a stable comment that
    # follows the completion wait in the current Android path.
    text = replace_exact(
        text,
        "    // 4. Copy generated frames to swapchain images and present them. Each\n",
        "    this->diagnosticStage_ = \"generated-present\";\n"
        "    // 4. Copy generated frames to swapchain images and present them. Each\n",
        count=1,
        label=f"{path}: generated present stage",
    )
    path.write_text(text, encoding="utf-8")

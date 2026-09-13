from __future__ import annotations

from pathlib import Path

from adreno_evidence_common import replace_exact


def once(text: str, old: str, new: str, label: str) -> str:
    return replace_exact(text, old, new, count=1, label=label)


def patch_framegen_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "zeroGenerationDirectStorage" in text:
        return

    text = once(
        text,
        "#ifdef __ANDROID__\n    if (this->transportOnly) {\n",
        "#ifdef __ANDROID__\n"
        "    const bool zeroGenerationDirectStorage = generationCount == 0 && !this->transportOnly;\n"
        "    const bool zeroGenerationTransportOnly = generationCount == 0 && this->transportOnly;\n"
        "    Core::Image& activeHistoryInput = (this->frameIdx % 2 == 0)\n"
        "        ? this->inImg_0 : this->inImg_1;\n"
        "    Core::Image& activeSharedHistoryInput = (this->frameIdx % 2 == 0)\n"
        "        ? this->sharedInImg_0 : this->sharedInImg_1;\n"
        "    Core::Image& activePrivateHistoryInput = (this->frameIdx % 2 == 0)\n"
        "        ? this->inImg_0 : this->inImg_1;\n"
        "    if (this->transportOnly) {\n",
        f"{path}: identify active zero-history inputs",
    )

    old_transport = """    if (this->transportOnly) {
        std::vector<VkImageMemoryBarrier2> barriers;
        barriers.reserve(4);
        add_external_transfer_acquire(barriers, vk, this->sharedInImg_0,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);
        add_external_transfer_acquire(barriers, vk, this->sharedInImg_1,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);
        add_local_transition(barriers, this->inImg_0, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        add_local_transition(barriers, this->inImg_1, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        emit_external_barriers(data.cmdBuffer1, barriers);
        copy_same_format(data.cmdBuffer1, this->sharedInImg_0, this->inImg_0);
        copy_same_format(data.cmdBuffer1, this->sharedInImg_1, this->inImg_1);
        barriers.clear();
        add_local_transition(barriers, this->inImg_0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);
        add_local_transition(barriers, this->inImg_1, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);
        add_external_transfer_release(barriers, vk, this->sharedInImg_0,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);
        add_external_transfer_release(barriers, vk, this->sharedInImg_1,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);
        emit_external_barriers(data.cmdBuffer1, barriers);
    } else {
"""
    new_transport = """    if (this->transportOnly) {
        std::vector<VkImageMemoryBarrier2> barriers;
        barriers.reserve(4);
        if (zeroGenerationTransportOnly) {
            // zero-generation transport-only active input acquire
            add_external_transfer_acquire(barriers, vk, activeSharedHistoryInput,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);
            add_local_transition(barriers, activePrivateHistoryInput,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        } else {
            add_external_transfer_acquire(barriers, vk, this->sharedInImg_0,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);
            add_external_transfer_acquire(barriers, vk, this->sharedInImg_1,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);
            add_local_transition(barriers, this->inImg_0, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            add_local_transition(barriers, this->inImg_1, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        }
        emit_external_barriers(data.cmdBuffer1, barriers);
        if (zeroGenerationTransportOnly) {
            copy_same_format(data.cmdBuffer1, activeSharedHistoryInput, activePrivateHistoryInput);
        } else {
            copy_same_format(data.cmdBuffer1, this->sharedInImg_0, this->inImg_0);
            copy_same_format(data.cmdBuffer1, this->sharedInImg_1, this->inImg_1);
        }
        barriers.clear();
        if (zeroGenerationTransportOnly) {
            add_local_transition(barriers, activePrivateHistoryInput,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                VK_IMAGE_LAYOUT_GENERAL);
            // zero-generation transport-only active input release
            add_external_transfer_release(barriers, vk, activeSharedHistoryInput,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);
        } else {
            add_local_transition(barriers, this->inImg_0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);
            add_local_transition(barriers, this->inImg_1, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);
            add_external_transfer_release(barriers, vk, this->sharedInImg_0,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);
            add_external_transfer_release(barriers, vk, this->sharedInImg_1,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);
        }
        emit_external_barriers(data.cmdBuffer1, barriers);
    } else {
"""
    text = once(
        text,
        old_transport,
        new_transport,
        f"{path}: zero-generation transport-only active input acquire/release",
    )

    old_acquire = """        std::vector<VkImageMemoryBarrier2> acquireBarriers;
        acquireBarriers.reserve(2);
        add_external_acquire(acquireBarriers, vk, this->inImg_0, VK_ACCESS_2_SHADER_READ_BIT);
        add_external_acquire(acquireBarriers, vk, this->inImg_1, VK_ACCESS_2_SHADER_READ_BIT);
        emit_external_barriers(data.cmdBuffer1, acquireBarriers);
"""
    new_acquire = """        std::vector<VkImageMemoryBarrier2> acquireBarriers;
        acquireBarriers.reserve(2);
        if (zeroGenerationDirectStorage) {
            add_external_acquire(acquireBarriers, vk, activeHistoryInput,
                VK_ACCESS_2_SHADER_READ_BIT);
        } else {
            add_external_acquire(acquireBarriers, vk, this->inImg_0,
                VK_ACCESS_2_SHADER_READ_BIT);
            add_external_acquire(acquireBarriers, vk, this->inImg_1,
                VK_ACCESS_2_SHADER_READ_BIT);
        }
        emit_external_barriers(data.cmdBuffer1, acquireBarriers);
"""
    text = once(text, old_acquire, new_acquire, f"{path}: active input acquire")

    old_release = """    if (generationCount == 0 && !this->transportOnly) {
        std::vector<VkImageMemoryBarrier2> releaseBarriers;
        releaseBarriers.reserve(2);
        add_external_release(releaseBarriers, vk, this->inImg_0,
            VK_ACCESS_2_SHADER_READ_BIT);
        add_external_release(releaseBarriers, vk, this->inImg_1,
            VK_ACCESS_2_SHADER_READ_BIT);
        emit_external_barriers(data.cmdBuffer1, releaseBarriers);
    }
"""
    new_release = """    if (zeroGenerationDirectStorage) {
        std::vector<VkImageMemoryBarrier2> releaseBarriers;
        releaseBarriers.reserve(1);
        add_external_release(releaseBarriers, vk, activeHistoryInput,
            VK_ACCESS_2_SHADER_READ_BIT);
        emit_external_barriers(data.cmdBuffer1, releaseBarriers);
    }
"""
    text = once(text, old_release, new_release, f"{path}: active input release")
    path.write_text(text, encoding="utf-8")


def patch_outer_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "pendingHistoryCompletionFds_" in text:
        return

    old_state = """    int pendingHistoryCompletionFd_{-1};
    bool pendingHistoryCompletionValid_{false};
    VkDevice androidDevice_{VK_NULL_HANDLE};
    void waitPendingHistoryCompletionFd(bool throwOnTimeout);
"""
    new_state = """    std::array<int, 2> pendingHistoryCompletionFds_{{-1, -1}};
    std::array<bool, 2> pendingHistoryCompletionValid_{{false, false}};
    VkDevice androidDevice_{VK_NULL_HANDLE};
    void waitPendingHistoryCompletionFd(size_t historySlot, bool throwOnTimeout);
"""
    text = once(text, old_state, new_state, f"{path}: slot-aware completion state")
    path.write_text(text, encoding="utf-8")


def patch_outer_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "pendingHistoryCompletionFds_" in text:
        return

    old_enable = (
        "    this->asyncZeroHistoryEnabled_=this->asyncAhbHandoffEnabled_ && "
        "this->asyncAhbHandoffHandleType_==VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;\n"
    )
    new_enable = (
        "    this->asyncZeroHistoryEnabled_="
        "this->asyncAhbHandoffEnabled_ && "
        "this->asyncAhbHandoffHandleType_==VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT && "
        "(ahbTransportMode==LSFG::AhbTransportMode::DirectStorage || "
        "ahbTransportMode==LSFG::AhbTransportMode::TransportOnly);\n"
    )
    text = once(text, old_enable, new_enable, f"{path}: supported transport zero-history gate")

    old_helper = r'''#ifdef __ANDROID__
void LsContext::waitPendingHistoryCompletionFd(bool throwOnTimeout) {
    if (!this->pendingHistoryCompletionValid_)
        return;
    const int fd = this->pendingHistoryCompletionFd_;
    if (fd < 0) {
        this->pendingHistoryCompletionValid_ = false;
        return;
    }

    pollfd descriptor{.fd = fd, .events = POLLIN, .revents = 0};
    const auto pollCompletion = [&descriptor](int timeoutMs) {
        int result = -1;
        do {
            result = ::poll(&descriptor, 1, timeoutMs);
        } while (result < 0 && errno == EINTR);
        return result;
    };

    int result = pollCompletion(0);
    const bool blocked = result == 0;
    const auto waitStart = RuntimeMetrics::Clock::now();
    if (blocked) {
        const int timeoutMs = static_cast<int>(
            (runtimeWaitTimeoutNs() + 999999ULL) / 1000000ULL);
        result = pollCompletion(timeoutMs);
    }
    const bool ready = result > 0
        && (descriptor.revents & POLLIN) != 0
        && (descriptor.revents & (POLLERR | POLLNVAL)) == 0;
    if (!ready) {
        if (throwOnTimeout) {
            if (result == 0)
                throw LSFG::vulkan_error(VK_TIMEOUT,
                    "Timed out waiting for deferred zero-generation history completion");
            throw LSFG::vulkan_error(VK_ERROR_INVALID_EXTERNAL_HANDLE,
                "Deferred zero-generation history sync fd became invalid");
        }
        ::close(fd);
        this->pendingHistoryCompletionFd_ = -1;
        this->pendingHistoryCompletionValid_ = false;
        return;
    }

    ::close(fd);
    this->pendingHistoryCompletionFd_ = -1;
    this->pendingHistoryCompletionValid_ = false;
    if (blocked) {
        const double waitMs = std::chrono::duration<double, std::milli>(
            RuntimeMetrics::Clock::now() - waitStart).count();
        std::cerr << "lsfg-vk: zero-history-sync-fd host-retire wait_ms="
                  << waitMs << '\n';
    }
}
#endif

'''
    new_helper = r'''#ifdef __ANDROID__
void LsContext::waitPendingHistoryCompletionFd(size_t historySlot, bool throwOnTimeout) {
    if (historySlot >= this->pendingHistoryCompletionFds_.size())
        throw LSFG::vulkan_error(VK_ERROR_UNKNOWN,
            "Invalid zero-generation history completion slot");
    if (!this->pendingHistoryCompletionValid_.at(historySlot))
        return;
    const int fd = this->pendingHistoryCompletionFds_.at(historySlot);
    if (fd < 0) {
        this->pendingHistoryCompletionValid_.at(historySlot) = false;
        return;
    }

    pollfd descriptor{.fd = fd, .events = POLLIN, .revents = 0};
    const auto pollCompletion = [&descriptor](int timeoutMs) {
        int result = -1;
        do {
            result = ::poll(&descriptor, 1, timeoutMs);
        } while (result < 0 && errno == EINTR);
        return result;
    };

    int result = pollCompletion(0);
    const bool blocked = result == 0;
    const auto waitStart = RuntimeMetrics::Clock::now();
    if (blocked) {
        const int timeoutMs = static_cast<int>(
            (runtimeWaitTimeoutNs() + 999999ULL) / 1000000ULL);
        result = pollCompletion(timeoutMs);
    }
    const bool ready = result > 0
        && (descriptor.revents & POLLIN) != 0
        && (descriptor.revents & (POLLERR | POLLNVAL)) == 0;
    if (!ready) {
        if (throwOnTimeout) {
            if (result == 0)
                throw LSFG::vulkan_error(VK_TIMEOUT,
                    "Timed out waiting for deferred zero-generation history completion");
            throw LSFG::vulkan_error(VK_ERROR_INVALID_EXTERNAL_HANDLE,
                "Deferred zero-generation history sync fd became invalid");
        }
        ::close(fd);
        this->pendingHistoryCompletionFds_.at(historySlot) = -1;
        this->pendingHistoryCompletionValid_.at(historySlot) = false;
        return;
    }

    ::close(fd);
    this->pendingHistoryCompletionFds_.at(historySlot) = -1;
    this->pendingHistoryCompletionValid_.at(historySlot) = false;
    if (blocked) {
        const double waitMs = std::chrono::duration<double, std::milli>(
            RuntimeMetrics::Clock::now() - waitStart).count();
        std::cerr << "lsfg-vk: zero-history-sync-fd host-retire slot="
                  << historySlot << " wait_ms=" << waitMs << '\n';
    } else {
        std::cerr << "lsfg-vk: zero-history-sync-fd retire-ready slot="
                  << historySlot << '\n';
    }
}
#endif

'''
    text = once(text, old_helper, new_helper, f"{path}: slot-aware completion wait")

    old_consume = """    std::vector<VkSemaphore> gameRenderSemaphores2 = gameRenderSemaphores;
    if (this->pendingHistoryCompletionValid_) this->waitPendingHistoryCompletionFd(true);
    if (this->previousSourceCopySignalValid_)
"""
    new_consume = """    const size_t historySlot = static_cast<size_t>(this->frameIdx % 2);
    std::vector<VkSemaphore> gameRenderSemaphores2 = gameRenderSemaphores;
    if (this->pendingHistoryCompletionValid_.at(historySlot))
        this->waitPendingHistoryCompletionFd(historySlot, true);
    if (this->previousSourceCopySignalValid_)
"""
    text = once(text, old_consume, new_consume, f"{path}: retire only reused slot")

    old_store = r'''                if (this->pendingHistoryCompletionValid_) { ::close(historyCompletionFd); throw LSFG::vulkan_error(VK_ERROR_UNKNOWN,"Overlapping zero-generation history completion fd"); }
                this->pendingHistoryCompletionFd_=historyCompletionFd;
                this->pendingHistoryCompletionValid_=true;
                if(firstPresentDiagnostic||adaptiveTelemetry.discontinuityReset) std::cerr << "lsfg-vk: zero-history-sync-fd deferred=1 fd=" << historyCompletionFd << '\n';
'''
    new_store = r'''                if (this->pendingHistoryCompletionValid_.at(historySlot)) { ::close(historyCompletionFd); throw LSFG::vulkan_error(VK_ERROR_UNKNOWN,"Overlapping zero-generation history completion fd for slot"); }
                this->pendingHistoryCompletionFds_.at(historySlot)=historyCompletionFd;
                this->pendingHistoryCompletionValid_.at(historySlot)=true;
                if(firstPresentDiagnostic||adaptiveTelemetry.discontinuityReset) std::cerr << "lsfg-vk: zero-history-sync-fd deferred=1 slot=" << historySlot << " fd=" << historyCompletionFd << '\n';
'''
    text = once(text, old_store, new_store, f"{path}: store completion by slot")

    text = once(
        text,
        "    bool needsDrain = this->pendingHistoryCompletionValid_;\n",
        "    bool needsDrain = this->pendingHistoryCompletionValid_.at(0)\n"
        "        || this->pendingHistoryCompletionValid_.at(1);\n",
        f"{path}: lifecycle slot detection",
    )
    text = once(
        text,
        "    if (this->pendingHistoryCompletionValid_)\n"
        "        this->waitPendingHistoryCompletionFd(throwOnTimeout);\n",
        "    for (size_t historySlot = 0; historySlot < this->pendingHistoryCompletionValid_.size(); ++historySlot)\n"
        "        if (this->pendingHistoryCompletionValid_.at(historySlot))\n"
        "            this->waitPendingHistoryCompletionFd(historySlot, throwOnTimeout);\n",
        f"{path}: lifecycle slot drain",
    )
    text = once(
        text,
        "    this->pendingHistoryCompletionFd_ = -1;\n"
        "    this->pendingHistoryCompletionValid_ = false;\n",
        "    this->pendingHistoryCompletionFds_.fill(-1);\n"
        "    this->pendingHistoryCompletionValid_.fill(false);\n",
        f"{path}: lifecycle slot reset",
    )

    path.write_text(text, encoding="utf-8")


def apply(root: Path) -> None:
    for relative in (
        "framegen/v3.1_src/context.cpp",
        "framegen/v3.1p_src/context.cpp",
    ):
        patch_framegen_source(root / relative)
    patch_outer_header(root / "include/context.hpp")
    patch_outer_source(root / "src/context.cpp")

#!/usr/bin/env python3
"""Fix Candidate A re-prime to use the proven game->framegen SYNC_FD handoff.

DeferredZero itself stays game-device-local.  When generation resumes, each
retained source replay crosses into the private framegen VkDevice, so it must
carry the same one-way execution dependency as the normal Android source path.
"""
from __future__ import annotations

from pathlib import Path

from adreno_evidence_common import replace_exact


def once(text: str, old: str, new: str, label: str) -> str:
    return replace_exact(text, old, new, count=1, label=label)


def patch_outer_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "deferred-zero reprime input-sync-fd" in text:
        return

    old_gate = '''        if (this->historyMaintenanceState_ == HistoryMaintenanceState::LiveHistory
                && this->deferredZeroRawHistoryAllocated_
                && !this->deferredZeroDisabledForContext_
                && this->rawSourceHistoryCount_ >= 3
'''
    new_gate = '''        if (this->historyMaintenanceState_ == HistoryMaintenanceState::LiveHistory
                && this->deferredZeroRawHistoryAllocated_
                && !this->deferredZeroDisabledForContext_
                && this->asyncZeroHistoryEnabled_
                && this->asyncAhbHandoffHandleType_
                    == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
                && this->rawSourceHistoryCount_ >= 3
'''
    text = once(
        text,
        old_gate,
        new_gate,
        f"{path}: gate DeferredZero on safe replay SYNC_FD support",
    )

    old_replay = '''                Mini::CommandBuffer replayBuffer(info.device, this->cmdPool);
                replayBuffer.begin();
                Mini::Image& targetInput = historySlot == 0 ? this->frame_0 : this->frame_1;
                copyRawHistoryToExternalAhb(replayBuffer.handle(),
                    this->rawSourceHistory_.at(replaySlot).handle(), targetInput.handle(),
                    this->extent.width, this->extent.height, info.queue.first);
                replayBuffer.end();
                submitAndWaitForAhbHandoff(info.device, replayBuffer, info.queue.second,
                    {}, {}, *this->ahbHandoffFence, this->resetHandoffFences,
                    this->waitHandoffFences);
                int historyCompletionFd = conf.performance
                    ? LSFG_3_1P::presentContextWithCountAndHistoryFd(
                        *this->lsfgCtxId, -1, noOutSems, 0)
                    : LSFG_3_1::presentContextWithCountAndHistoryFd(
                        *this->lsfgCtxId, -1, noOutSems, 0);
                if (historyCompletionFd >= 0) {
                    this->pendingHistoryCompletionFds_.at(historySlot) = historyCompletionFd;
                    this->pendingHistoryCompletionValid_.at(historySlot) = true;
                    this->waitPendingHistoryCompletionFd(historySlot, true);
                }
'''
    new_replay = '''                if (!this->asyncZeroHistoryEnabled_
                        || this->asyncAhbHandoffHandleType_
                            != VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT) {
                    throw LSFG::vulkan_error(VK_ERROR_FEATURE_NOT_PRESENT,
                        "DeferredZero re-prime requires game-to-framegen SYNC_FD handoff");
                }

                Mini::Semaphore replayInputSemaphore(
                    info.device, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
                Mini::CommandBuffer replayBuffer(info.device, this->cmdPool);
                replayBuffer.begin();
                Mini::Image& targetInput = historySlot == 0 ? this->frame_0 : this->frame_1;
                copyRawHistoryToExternalAhb(replayBuffer.handle(),
                    this->rawSourceHistory_.at(replaySlot).handle(), targetInput.handle(),
                    this->extent.width, this->extent.height, info.queue.first);
                replayBuffer.end();

                // Keep the existing host fence only as a conservative lifetime guard for
                // this infrequent replay command buffer.  The private framegen device
                // still receives an explicit one-way SYNC_FD execution dependency.
                submitAndWaitForAhbHandoff(info.device, replayBuffer, info.queue.second,
                    {}, { replayInputSemaphore.handle() }, *this->ahbHandoffFence,
                    this->resetHandoffFences, this->waitHandoffFences);
                const int replayInputSemaphoreFd = replayInputSemaphore.exportFd(
                    info.device, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
                std::cerr << "lsfg-vk: deferred-zero reprime input-sync-fd replay="
                          << replayIndex << " fd=" << replayInputSemaphoreFd << '\\n';

                const int historyCompletionFd = conf.performance
                    ? LSFG_3_1P::presentContextWithCountAndHistoryFd(
                        *this->lsfgCtxId, replayInputSemaphoreFd, noOutSems, 0)
                    : LSFG_3_1::presentContextWithCountAndHistoryFd(
                        *this->lsfgCtxId, replayInputSemaphoreFd, noOutSems, 0);
                if (historyCompletionFd < 0)
                    throw LSFG::vulkan_error(VK_ERROR_INVALID_EXTERNAL_HANDLE,
                        "DeferredZero re-prime did not return a history completion SYNC_FD");
                this->pendingHistoryCompletionFds_.at(historySlot) = historyCompletionFd;
                this->pendingHistoryCompletionValid_.at(historySlot) = true;
                this->waitPendingHistoryCompletionFd(historySlot, true);
'''
    text = once(
        text,
        old_replay,
        new_replay,
        f"{path}: synchronized DeferredZero re-prime replay",
    )
    path.write_text(text, encoding="utf-8")


def apply(root: Path) -> None:
    patch_outer_source(root / "src/context.cpp")

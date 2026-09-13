#!/usr/bin/env python3
"""Keep DeferredZero re-prime capture game-local and inside the fallback guard.

The current source frame is copied only into the game-device-local raw-history
ring.  That copy does not cross VkDevice ownership and therefore must not use
the AHB host-fence handoff helper.  The later raw-history -> shared AHB replay
continues to use the proven one-way SYNC_FD path.
"""
from __future__ import annotations

from pathlib import Path

from adreno_evidence_common import replace_exact


def once(text: str, old: str, new: str, label: str) -> str:
    return replace_exact(text, old, new, count=1, label=label)


def patch_outer_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "deferred-zero reprime capture-submitted" in text:
        return

    old_transition = '''        std::cerr << "lsfg-vk: deferred-zero reprime-begin history_count="
                  << this->rawSourceHistoryCount_ << '\\n';
        this->diagnosticStage_ = "deferred-zero-reprime-source-copy";
        pass.preCopySemaphores.at(0) = Mini::Semaphore(info.device);
        pass.preCopySemaphores.at(1) = Mini::Semaphore(info.device);
        pass.preCopyBuf = Mini::CommandBuffer(info.device, this->cmdPool);
        pass.preCopyBuf.begin();
        const size_t currentRawSlot = this->rawSourceHistoryNext_;
        copySwapchainToRawHistory(pass.preCopyBuf.handle(),
            this->swapchainImages.at(presentIdx),
            this->rawSourceHistory_.at(currentRawSlot).handle(),
            this->extent.width, this->extent.height,
            !this->rawSourceHistoryInitialized_.at(currentRawSlot));
        pass.preCopyBuf.end();
        std::vector<VkSemaphore> currentSourceWaits = gameRenderSemaphores;
        if (this->previousSourceCopySignalValid_)
            currentSourceWaits.emplace_back(this->passInfos.at((this->frameIdx - 1) % 8)
                .preCopySemaphores.at(1).handle());
        submitAndWaitForAhbHandoff(info.device, pass.preCopyBuf, info.queue.second,
            currentSourceWaits,
            { pass.preCopySemaphores.at(0).handle(), pass.preCopySemaphores.at(1).handle() },
            *this->ahbHandoffFence, this->resetHandoffFences, this->waitHandoffFences);
        this->previousSourceCopySignalValid_ = true;
        this->rawSourceHistoryInitialized_.at(currentRawSlot) = true;
        this->rawSourceHistoryNext_ = (currentRawSlot + 1) % 3;
        this->rawSourceHistoryCount_ = std::min<size_t>(3, this->rawSourceHistoryCount_ + 1);

        bool reprimeSucceeded = this->rawSourceHistoryCount_ == 3;
        try {
            if (!reprimeSucceeded)
                throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
                    "DeferredZero re-prime has fewer than three raw source frames");
            const auto orderedRawHistorySlots = this->orderedRawHistorySlots();
'''

    new_transition = '''        std::cerr << "lsfg-vk: deferred-zero reprime-begin history_count="
                  << this->rawSourceHistoryCount_ << '\\n';
        bool reprimeSucceeded = true;
        try {
            // This is a game-VkDevice-local source retention copy.  Keep it on the
            // same asynchronous semaphore chain used by steady DeferredZero and do
            // not involve the shared-AHB host handoff fence.
            this->diagnosticStage_ = "deferred-zero-reprime-capture-record";
            std::cerr << "lsfg-vk: deferred-zero reprime capture-record\\n";
            pass.preCopySemaphores.at(0) = Mini::Semaphore(info.device);
            pass.preCopySemaphores.at(1) = Mini::Semaphore(info.device);
            pass.preCopyBuf = Mini::CommandBuffer(info.device, this->cmdPool);
            pass.preCopyBuf.begin();
            const size_t currentRawSlot = this->rawSourceHistoryNext_;
            copySwapchainToRawHistory(pass.preCopyBuf.handle(),
                this->swapchainImages.at(presentIdx),
                this->rawSourceHistory_.at(currentRawSlot).handle(),
                this->extent.width, this->extent.height,
                !this->rawSourceHistoryInitialized_.at(currentRawSlot));
            pass.preCopyBuf.end();
            std::vector<VkSemaphore> currentSourceWaits = gameRenderSemaphores;
            if (this->previousSourceCopySignalValid_)
                currentSourceWaits.emplace_back(this->passInfos.at((this->frameIdx - 1) % 8)
                    .preCopySemaphores.at(1).handle());
            this->diagnosticStage_ = "deferred-zero-reprime-capture-submit";
            std::cerr << "lsfg-vk: deferred-zero reprime capture-submit raw_slot="
                      << currentRawSlot << '\\n';
            pass.preCopyBuf.submit(info.queue.second, currentSourceWaits,
                { pass.preCopySemaphores.at(0).handle(), pass.preCopySemaphores.at(1).handle() });
            this->previousSourceCopySignalValid_ = true;
            this->rawSourceHistoryInitialized_.at(currentRawSlot) = true;
            this->rawSourceHistoryNext_ = (currentRawSlot + 1) % 3;
            this->rawSourceHistoryCount_ = std::min<size_t>(3, this->rawSourceHistoryCount_ + 1);
            std::cerr << "lsfg-vk: deferred-zero reprime capture-submitted raw_slot="
                      << currentRawSlot << '\\n';

            if (this->rawSourceHistoryCount_ != 3)
                throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
                    "DeferredZero re-prime has fewer than three raw source frames");
            this->diagnosticStage_ = "deferred-zero-reprime-replay-enter";
            std::cerr << "lsfg-vk: deferred-zero reprime replay-enter history_count="
                      << this->rawSourceHistoryCount_ << '\\n';
            const auto orderedRawHistorySlots = this->orderedRawHistorySlots();
'''

    text = once(
        text,
        old_transition,
        new_transition,
        f"{path}: guard and de-fence DeferredZero re-prime source capture",
    )

    old_fallback = '''            std::cerr << "lsfg-vk: deferred-zero reprime-fallback reason="
                      << error.what() << '\\n';
'''
    new_fallback = '''            std::cerr << "lsfg-vk: deferred-zero reprime-fallback stage="
                      << this->diagnosticStage_ << " reason=" << error.what() << '\\n';
'''
    text = once(
        text,
        old_fallback,
        new_fallback,
        f"{path}: report guarded re-prime failure stage",
    )
    path.write_text(text, encoding="utf-8")


def apply(root: Path) -> None:
    patch_outer_source(root / "src/context.cpp")

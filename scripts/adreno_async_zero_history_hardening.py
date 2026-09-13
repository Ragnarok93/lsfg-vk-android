from __future__ import annotations

from pathlib import Path

from adreno_evidence_common import replace_exact


def once(text: str, old: str, new: str, label: str) -> str:
    return replace_exact(text, old, new, count=1, label=label)


def patch_public_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    correct = (
        '    __attribute__((visibility("default")))\n'
        '    int presentContextWithCountAndHistoryFd(int32_t id,int inSem,'
        'const std::vector<int>& outSem,size_t activeGenerationCount);\n'
        '    __attribute__((visibility("default")))\n'
        '    bool waitContext(int32_t id, uint64_t timeoutNs);\n'
    )
    if correct in text:
        return

    malformed = (
        '    __attribute__((visibility("default")))\n'
        '    __attribute__((visibility("default")))\n'
        '    int presentContextWithCountAndHistoryFd(int32_t id,int inSem,'
        'const std::vector<int>& outSem,size_t activeGenerationCount);\n'
        '    bool waitContext(int32_t id, uint64_t timeoutNs);\n'
    )
    text = once(text, malformed, correct, f"{path}: preserve waitContext visibility")
    path.write_text(text, encoding="utf-8")


def patch_outer_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "flushPendingAndroidWork" in text:
        return

    text = once(
        text,
        "    VkDevice androidDevice_{VK_NULL_HANDLE};\n",
        "    VkDevice androidDevice_{VK_NULL_HANDLE};\n"
        "    bool performanceBackend_{false};\n"
        "    void flushPendingAndroidWork(bool throwOnTimeout);\n",
        f"{path}: lifecycle drain declaration",
    )
    path.write_text(text, encoding="utf-8")


def patch_outer_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "zero-history-lifecycle-drain ready=1" in text:
        return

    text = once(
        text,
        "    this->androidDevice_=info.device;\n",
        "    this->androidDevice_=info.device;\n"
        "    this->performanceBackend_=conf.performance;\n",
        f"{path}: remember framegen backend",
    )

    old_destructor = """LsContext::~LsContext() {
#ifdef __ANDROID__
    if (this->androidDevice_!=VK_NULL_HANDLE && this->waitHandoffFences) for (auto& pass:this->passInfos) if (pass.handoffFencePending && pass.handoffFence) { const VkFence f=*pass.handoffFence; (void)this->waitHandoffFences(this->androidDevice_,1,&f,VK_TRUE,runtimeWaitTimeoutNs()); pass.handoffFencePending=false; }
#endif
}

"""
    hardened = """#ifdef __ANDROID__
void LsContext::flushPendingAndroidWork(bool throwOnTimeout) {
    bool needsDrain = this->pendingHistoryCompletionValid_;
    for (const auto& pass : this->passInfos)
        needsDrain = needsDrain || pass.handoffFencePending;
    if (!needsDrain)
        return;

    // The reverse completion stays a Linux sync_file fd. Retire it at the CPU
    // boundary instead of importing it into the wrapper/game VkDevice. This
    // keeps the zero-history submission overlapped with the frame interval while
    // avoiding the unstable framegen -> wrapper Vulkan semaphore import path.
    if (this->pendingHistoryCompletionValid_)
        this->waitPendingHistoryCompletionFd(throwOnTimeout);

    // Retire every game-device source-copy submission before command buffers,
    // export semaphores, or AHB-backed images can be destroyed.
    if (this->androidDevice_ != VK_NULL_HANDLE && this->waitHandoffFences != nullptr) {
        for (auto& pass : this->passInfos) {
            if (!pass.handoffFencePending || !pass.handoffFence)
                continue;
            const VkFence fence = *pass.handoffFence;
            const auto result = this->waitHandoffFences(
                this->androidDevice_, 1, &fence, VK_TRUE, runtimeWaitTimeoutNs());
            if (result != VK_SUCCESS) {
                if (throwOnTimeout)
                    throw LSFG::vulkan_error(result,
                        "Timed out draining Android AHB handoff before lifecycle reset");
                std::cerr << "lsfg-vk: zero-history-lifecycle-drain game-fence result="
                          << result << "\\n";
                return;
            }
            pass.handoffFencePending = false;
        }
    }

    // The framegen zero-count path retains its private preprocessing fence and
    // export semaphore until waitContext observes completion. Drain those
    // private-device objects before temporal state or the context is reset.
    if (this->lsfgCtxId) {
        const bool framegenReady = this->performanceBackend_
            ? LSFG_3_1P::waitContext(*this->lsfgCtxId, runtimeWaitTimeoutNs())
            : LSFG_3_1::waitContext(*this->lsfgCtxId, runtimeWaitTimeoutNs());
        if (!framegenReady) {
            if (throwOnTimeout)
                throw LSFG::vulkan_error(VK_TIMEOUT,
                    "Timed out draining zero-generation preprocessing before lifecycle reset");
            std::cerr << "lsfg-vk: zero-history-lifecycle-drain framegen-timeout=1\\n";
            return;
        }
    }

    this->pendingHistoryCompletionFd_ = -1;
    this->pendingHistoryCompletionValid_ = false;
    std::cerr << "lsfg-vk: zero-history-lifecycle-drain ready=1\\n";
}
#endif

LsContext::~LsContext() {
#ifdef __ANDROID__
    try {
        this->flushPendingAndroidWork(false);
    } catch (const std::exception& error) {
        std::cerr << "lsfg-vk: zero-history-lifecycle-drain destructor-error: "
                  << error.what() << "\\n";
    }
#endif
}

"""
    text = once(text, old_destructor, hardened, f"{path}: lifecycle-safe destructor")

    old_bypass = """void LsContext::enterSourceOnlyBypass() {
    this->lastGeneratedFrameCount_ = 0;
    this->requiresSourceHistoryWarmup_ = true;
    this->previousSourceCopySignalValid_ = false;
}
"""
    new_bypass = """void LsContext::enterSourceOnlyBypass() {
    this->flushPendingAndroidWork(true);
    this->lastGeneratedFrameCount_ = 0;
    this->requiresSourceHistoryWarmup_ = true;
    this->previousSourceCopySignalValid_ = false;
}
"""
    text = once(text, old_bypass, new_bypass, f"{path}: source-only lifecycle drain")
    path.write_text(text, encoding="utf-8")


def apply(root: Path) -> None:
    patch_public_header(root / "framegen/public/lsfg_3_1.hpp")
    patch_public_header(root / "framegen/public/lsfg_3_1p.hpp")
    patch_outer_header(root / "include/context.hpp")
    patch_outer_source(root / "src/context.cpp")

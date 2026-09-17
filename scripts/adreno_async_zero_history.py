from __future__ import annotations
from pathlib import Path
from adreno_evidence_common import replace_exact


def once(text: str, old: str, new: str, label: str) -> str:
    return replace_exact(text, old, new, count=1, label=label)


def insert_before(text: str, marker: str, block: str, label: str) -> str:
    pos = text.find(marker)
    if pos < 0:
        raise RuntimeError(f"{label}: marker not found")
    return text[:pos] + block + text[pos:]


def patch_mini(h: Path, s: Path) -> None:
    t = h.read_text()
    if "static Semaphore importFd" not in t:
        t = once(t,
            "        [[nodiscard]] VkSemaphore handle() const {\n",
            "        [[nodiscard]] static Semaphore importFd(VkDevice device, int fd,\n"
            "            VkExternalSemaphoreHandleTypeFlagBits handleType);\n\n"
            "        [[nodiscard]] VkSemaphore handle() const {\n", f"{h}: import api")
        h.write_text(t)
    t = s.read_text()
    if "Semaphore::importFd(" in t: return
    if "#include <unistd.h>" not in t:
        t = once(t, "#include <memory>\n", "#include <memory>\n#include <unistd.h>\n", f"{s}: unistd")
    block = r'''
Semaphore Semaphore::importFd(VkDevice device, int fd,
        VkExternalSemaphoreHandleTypeFlagBits handleType) {
    if (fd < 0) throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "Invalid external semaphore fd");
    const auto importFd = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
        Layer::ovkGetDeviceProcAddr(device, "vkImportSemaphoreFdKHR"));
    if (!importFd) { ::close(fd); throw LSFG::vulkan_error(VK_ERROR_EXTENSION_NOT_PRESENT, "Semaphore fd import unavailable"); }
    VkSemaphore sem{};
    const VkSemaphoreCreateInfo create{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    auto res = Layer::ovkCreateSemaphore(device, &create, nullptr, &sem);
    if (res != VK_SUCCESS || sem == VK_NULL_HANDLE) { ::close(fd); throw LSFG::vulkan_error(res, "Unable to create imported semaphore"); }
    const VkImportSemaphoreFdInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .semaphore = sem,
        .flags = handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
            ? static_cast<VkSemaphoreImportFlags>(VK_SEMAPHORE_IMPORT_TEMPORARY_BIT) : VkSemaphoreImportFlags{0},
        .handleType = handleType, .fd = fd};
    res = importFd(device, &info);
    if (res != VK_SUCCESS) { Layer::ovkDestroySemaphore(device, sem, nullptr); ::close(fd); throw LSFG::vulkan_error(res, "Unable to import semaphore fd"); }
    Semaphore out; out.semaphore = ownSemaphore(device, sem); return out;
}

'''
    t = insert_before(t, "int Semaphore::exportFd(\n", block, f"{s}: import impl")
    s.write_text(t)


def patch_core(h: Path, s: Path) -> None:
    t = h.read_text()
    if "int exportFd(const Core::Device&" not in t:
        t = once(t, "        Semaphore(const Core::Device& device, int fd);\n",
            "        Semaphore(const Core::Device& device, int fd);\n"
            "        Semaphore(const Core::Device& device, VkExternalSemaphoreHandleTypeFlagBits handleType);\n"
            "        [[nodiscard]] int exportFd(const Core::Device& device, VkExternalSemaphoreHandleTypeFlagBits handleType) const;\n",
            f"{h}: export api")
        h.write_text(t)
    t = s.read_text()
    if "Semaphore::exportFd(const Core::Device&" in t: return
    block = r'''Semaphore::Semaphore(const Core::Device& device, VkExternalSemaphoreHandleTypeFlagBits type) {
    const VkExportSemaphoreCreateInfo ex{.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, .handleTypes = type};
    const VkSemaphoreCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ex};
    VkSemaphore sem{}; const auto res = vkCreateSemaphore(device.handle(), &ci, nullptr, &sem);
    if (res != VK_SUCCESS || sem == VK_NULL_HANDLE) throw LSFG::vulkan_error(res, "Unable to create exportable semaphore");
    this->semaphore = std::shared_ptr<VkSemaphore>(new VkSemaphore(sem), [dev=device.handle()](VkSemaphore* p){ vkDestroySemaphore(dev,*p,nullptr); delete p; });
}
int Semaphore::exportFd(const Core::Device& device, VkExternalSemaphoreHandleTypeFlagBits type) const {
    const auto getFd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(device.handle(), "vkGetSemaphoreFdKHR"));
    if (!getFd || !this->semaphore) throw LSFG::vulkan_error(VK_ERROR_EXTENSION_NOT_PRESENT, "Semaphore fd export unavailable");
    const VkSemaphoreGetFdInfoKHR info{.sType=VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,.semaphore=*this->semaphore,.handleType=type};
    int fd=-1; const auto res=getFd(device.handle(),&info,&fd);
    if (res != VK_SUCCESS || fd < 0) throw LSFG::vulkan_error(res,"Unable to export semaphore fd");
    return fd;
}

'''
    t = insert_before(t, "Semaphore::Semaphore(const Core::Device& device, int fd) {\n", block, f"{s}: export impl")
    s.write_text(t)


def patch_fg_header(p: Path) -> None:
    t=p.read_text()
    if "historyCompletionFd" not in t:
        t=once(t,"            size_t activeGenerationCount);\n","            size_t activeGenerationCount, int* historyCompletionFd = nullptr);\n",f"{p}: present arg")
    if "preprocessingPending" not in t:
        t=once(t,"            Core::Fence preprocessingFence; // reused for zero-generation temporal preprocessing\n",
            "            Core::Fence preprocessingFence; // reused for zero-generation temporal preprocessing\n"
            "            Core::Semaphore historyCompletionSemaphore;\n            bool preprocessingPending{false};\n",f"{p}: pending state")
    p.write_text(t)


def patch_fg_source(p: Path) -> None:
    t=p.read_text()
    if "zero-history-sync-fd framegen-submit" in t: return
    if "#include <iostream>" not in t:
        if "#include <cstdlib>\n" in t:
            t=once(t, "#include <cstdlib>\n", "#include <cstdlib>\n#include <iostream>\n", f"{p}: iostream")
        else:
            t=once(t, "#include <cstdint>\n", "#include <cstdint>\n#include <iostream>\n", f"{p}: iostream")
    t=once(t,"        size_t activeGenerationCount) {\n","        size_t activeGenerationCount, int* historyCompletionFd) {\n",f"{p}: present sig")
    t=once(t,"    auto& data = this->data.at(this->frameIdx % 8);\n\n",
        "    auto& data = this->data.at(this->frameIdx % 8);\n"
        "    if (data.preprocessingPending) {\n"
        "        if (!data.preprocessingFence.wait(vk.device, framegenWaitTimeoutNs())) throw LSFG::vulkan_error(VK_TIMEOUT, \"Deferred preprocessing wait timed out\");\n"
        "        data.preprocessingPending=false; data.historyCompletionSemaphore=Core::Semaphore{};\n    }\n\n",f"{p}: retire preprocess")
    t=t.replace("    const bool profileZeroStage = generationCount == 0\n        && this->zeroStageQueryPool.supported();\n",
        "    const bool profileZeroStage = generationCount == 0\n        && historyCompletionFd == nullptr\n        && this->zeroStageQueryPool.supported();\n",1)
    marker="    if (generationCount == 0) {\n"; pos=t.find(marker)
    if pos<0: raise RuntimeError(f"{p}: zero block")
    pos+=len(marker)
    block=r'''        if (historyCompletionFd != nullptr) {
            *historyCompletionFd=-1;
            try {
                data.historyCompletionSemaphore=Core::Semaphore(vk.device,VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
                data.preprocessingFence.reset(vk.device);
                data.cmdBuffer1.submit(vk.device.getComputeQueue(),data.preprocessingFence,waits,std::nullopt,{data.historyCompletionSemaphore},std::nullopt);
                data.preprocessingPending=true;
                try {
                    *historyCompletionFd=data.historyCompletionSemaphore.exportFd(vk.device,VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
                    std::cerr << "lsfg-vk: zero-history-sync-fd framegen-submit fd=" << *historyCompletionFd << '\n';
                    this->frameIdx++; return;
                } catch (const std::exception& e) {
                    if (!data.preprocessingFence.wait(vk.device,framegenWaitTimeoutNs())) throw LSFG::vulkan_error(VK_TIMEOUT,"Temporal preprocessing fallback wait timed out");
                    data.preprocessingPending=false; data.historyCompletionSemaphore=Core::Semaphore{};
                    std::cerr << "lsfg-vk: zero-history-sync-fd export fallback: " << e.what() << '\n';
                    this->frameIdx++; return;
                }
            } catch (const std::exception& e) {
                data.preprocessingPending=false; data.historyCompletionSemaphore=Core::Semaphore{};
                std::cerr << "lsfg-vk: zero-history-sync-fd setup fallback: " << e.what() << '\n';
            }
        }
'''
    t=t[:pos]+block+t[pos:]
    old="    auto& renderData = this->data.at((this->frameIdx - 1) % this->data.size());\n    if (!renderData.shouldWait)\n        return true;\n"
    new="    auto& renderData = this->data.at((this->frameIdx - 1) % this->data.size());\n    if (renderData.preprocessingPending) {\n        if (!renderData.preprocessingFence.wait(vk.device, timeoutNs)) return false;\n        renderData.preprocessingPending=false; renderData.historyCompletionSemaphore=Core::Semaphore{};\n    }\n    if (!renderData.shouldWait)\n        return true;\n"
    t=once(t,old,new,f"{p}: wait last")
    old="    for (auto& renderData : this->data) {\n        if (!renderData.shouldWait)\n            continue;\n"
    new="    for (auto& renderData : this->data) {\n        if (renderData.preprocessingPending) {\n            if (!renderData.preprocessingFence.wait(vk.device, framegenWaitTimeoutNs())) return false;\n            renderData.preprocessingPending=false; renderData.historyCompletionSemaphore=Core::Semaphore{};\n        }\n        if (!renderData.shouldWait)\n            continue;\n"
    t=once(t,old,new,f"{p}: wait all")
    p.write_text(t)


def patch_public(h: Path, s: Path, ns: str) -> None:
    if not h.exists() or not s.exists(): return
    t=h.read_text()
    if "presentContextWithCountAndHistoryFd" not in t:
        marker = (
            "    __attribute__((visibility(\"default\")))\n"
            "    bool waitContext(int32_t id, uint64_t timeoutNs);\n"
        )
        declaration = (
            "    __attribute__((visibility(\"default\")))\n"
            "    int presentContextWithCountAndHistoryFd(int32_t id,int inSem,"
            "const std::vector<int>& outSem,size_t activeGenerationCount);\n"
        )
        t=insert_before(t, marker, declaration, f"{h}: history api")
        h.write_text(t)
    t=s.read_text()
    if "presentContextWithCountAndHistoryFd" in t: return
    block=f'''#ifdef __ANDROID__\nint {ns}::presentContextWithCountAndHistoryFd(int32_t id,int inSem,const std::vector<int>& outSem,size_t count) {{\n    const std::scoped_lock lock(runtimeMutex);\n    if (!instance.has_value() || !device.has_value()) throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,"LSFG not initialized");\n    if (count > device->generationCount) throw std::runtime_error("LSFG active generation count exceeds runtime capacity");\n    auto it=contexts.find(id); if (it==contexts.end()) throw LSFG::vulkan_error(VK_ERROR_UNKNOWN,"Context not found");\n    int historyCompletionFd=-1; it->second.present(*device,inSem,outSem,count,&historyCompletionFd); return historyCompletionFd;\n}}\n#endif\n\n'''
    t=insert_before(t,f"void {ns}::deleteContext(int32_t id) {{\n",block,f"{s}: history api impl")
    s.write_text(t)


def patch_outer_header(p: Path) -> None:
    t=p.read_text()
    if "pendingHistoryCompletionFd_" not in t:
        t=once(t,"    bool asyncAhbHandoffEnabled_{false};\n",
            "    bool asyncAhbHandoffEnabled_{false};\n    bool asyncZeroHistoryEnabled_{false};\n    int pendingHistoryCompletionFd_{-1};\n    bool pendingHistoryCompletionValid_{false};\n    VkDevice androidDevice_{VK_NULL_HANDLE};\n    void waitPendingHistoryCompletionFd(bool throwOnTimeout);\n",f"{p}: history state")
    if "handoffFencePending" not in t:
        t=once(t,"        Mini::Semaphore framegenInputSemaphore;\n",
            "        Mini::Semaphore framegenInputSemaphore;\n        std::shared_ptr<VkFence> handoffFence;\n        bool handoffFencePending{false};\n",f"{p}: pass lifetime")
    if "~LsContext();" not in t: t=once(t,"    ~LsContext() = default;\n","    ~LsContext();\n",f"{p}: destructor")
    p.write_text(t)


def patch_outer(p: Path) -> None:
    t=p.read_text()
    if "zero-history-sync-fd deferred=1" in t: return
    if "#include <poll.h>" not in t:
        t=once(t,"#include <android/log.h>\n",
            "#include <android/log.h>\n#include <poll.h>\n#include <unistd.h>\n#include <cerrno>\n",f"{p}: deferred sync-fd poll includes")
    t=once(t,"#ifdef __ANDROID__\n    // Select and validate the exact framegen ICD before allocating any shared AHB.\n",
        "#ifdef __ANDROID__\n    this->androidDevice_=info.device;\n    // Select and validate the exact framegen ICD before allocating any shared AHB.\n",f"{p}: store device")
    t=once(t,"    this->asyncAhbHandoffHandleType_ = syncFdHandoffSupported\n        ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT\n        : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;\n",
        "    this->asyncAhbHandoffHandleType_ = syncFdHandoffSupported\n        ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT\n        : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;\n    this->asyncZeroHistoryEnabled_=this->asyncAhbHandoffEnabled_ && this->asyncAhbHandoffHandleType_==VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;\n",f"{p}: zero enable")
    a=t.find("    const VkFenceCreateInfo handoffFenceInfo{\n"); b=t.find("\n    const auto gameGetSemaphoreFd",a)
    if a<0 or b<0: raise RuntimeError(f"{p}: fence block")
    t=t[:a]+"    const VkFenceCreateInfo handoffFenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};\n"+t[b:]
    old="    for (size_t i = 0; i < 8; i++) {\n        auto& pass = this->passInfos.at(i);\n"
    new=old+"#ifdef __ANDROID__\n        VkFence f{}; const auto fr=createHandoffFence(info.device,&handoffFenceInfo,nullptr,&f);\n        if (fr!=VK_SUCCESS || f==VK_NULL_HANDLE) throw LSFG::vulkan_error(fr,\"Failed to create Android AHB handoff fence\");\n        pass.handoffFence=std::shared_ptr<VkFence>(new VkFence(f),[device=info.device,destroyHandoffFence](VkFence* p){ if(p){ if(*p!=VK_NULL_HANDLE) destroyHandoffFence(device,*p,nullptr); delete p; }});\n#endif\n"
    t=once(t,old,new,f"{p}: fence ring")
    d='''LsContext::~LsContext() {\n#ifdef __ANDROID__\n    if (this->androidDevice_!=VK_NULL_HANDLE && this->waitHandoffFences) for (auto& pass:this->passInfos) if (pass.handoffFencePending && pass.handoffFence) { const VkFence f=*pass.handoffFence; (void)this->waitHandoffFences(this->androidDevice_,1,&f,VK_TRUE,runtimeWaitTimeoutNs()); pass.handoffFencePending=false; }\n#endif\n}\n\n'''
    t=insert_before(t,"VkResult LsContext::present(",d,f"{p}: destructor impl")
    wait_helper=r'''#ifdef __ANDROID__
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
    t=insert_before(t,"LsContext::~LsContext() {",wait_helper,f"{p}: deferred history fd wait")
    t=once(t,"    auto& metrics = this->runtimeMetrics;\n    const auto cycleStart = RuntimeMetrics::Clock::now();\n",
        "    auto& metrics = this->runtimeMetrics;\n    const auto cycleStart = RuntimeMetrics::Clock::now();\n    if (pass.handoffFencePending) { waitForAhbHandoff(info.device,*pass.handoffFence,this->waitHandoffFences); pass.handoffFencePending=false; }\n",f"{p}: retire slot")
    t=once(t,"    std::vector<VkSemaphore> gameRenderSemaphores2 = gameRenderSemaphores;\n    if (this->previousSourceCopySignalValid_)\n",
        "    std::vector<VkSemaphore> gameRenderSemaphores2 = gameRenderSemaphores;\n    if (this->pendingHistoryCompletionValid_) this->waitPendingHistoryCompletionFd(true);\n    if (this->previousSourceCopySignalValid_)\n",f"{p}: consume reverse")
    t=once(t,"    bool useAsyncHandoff = this->asyncAhbHandoffEnabled_\n        && generatedFrameCount > 0\n        && !warmupSourceHistory;\n",
        "    bool useAsyncHandoff = this->asyncAhbHandoffEnabled_ && !warmupSourceHistory && (generatedFrameCount>0 || (adaptiveZeroGeneration && this->asyncZeroHistoryEnabled_));\n",f"{p}: async zero")
    t=t.replace("*this->ahbHandoffFence","*pass.handoffFence")
    t=once(t,"    this->previousSourceCopySignalValid_ = true;\n","    pass.handoffFencePending=useAsyncHandoff;\n    this->previousSourceCopySignalValid_ = true;\n",f"{p}: fence pending")
    zs=t.find("    if (adaptiveZeroGeneration) {\n"); ze=t.find("\n    if (warmupSourceHistory) {\n",zs)
    if zs<0 or ze<0: raise RuntimeError(f"{p}: zero region")
    block=t[zs:ze]; a=block.find("        std::vector<int> noOutSems;\n"); b=block.find("        metrics.windowAdaptiveZeroGenerationCycles++;\n")
    if a<0 or b<0: raise RuntimeError(f"{p}: zero landmarks")
    mid=r'''        std::vector<int> noOutSems;
        const auto historyAdvanceStart=RuntimeMetrics::Clock::now(); int historyCompletionFd=-1;
        if (useAsyncHandoff) historyCompletionFd=conf.performance ? LSFG_3_1P::presentContextWithCountAndHistoryFd(*this->lsfgCtxId,framegenInputSemaphoreFd,noOutSems,0) : LSFG_3_1::presentContextWithCountAndHistoryFd(*this->lsfgCtxId,framegenInputSemaphoreFd,noOutSems,0);
        else if (conf.performance) LSFG_3_1P::presentContextWithCount(*this->lsfgCtxId,-1,noOutSems,0); else LSFG_3_1::presentContextWithCount(*this->lsfgCtxId,-1,noOutSems,0);
        metrics.windowDispatchMs += std::chrono::duration<double,std::milli>(RuntimeMetrics::Clock::now()-historyAdvanceStart).count();
        if (useAsyncHandoff) {
            if (historyCompletionFd>=0) {
                if (this->pendingHistoryCompletionValid_) { ::close(historyCompletionFd); throw LSFG::vulkan_error(VK_ERROR_UNKNOWN,"Overlapping zero-generation history completion fd"); }
                this->pendingHistoryCompletionFd_=historyCompletionFd;
                this->pendingHistoryCompletionValid_=true;
                if(firstPresentDiagnostic||adaptiveTelemetry.discontinuityReset) std::cerr << "lsfg-vk: zero-history-sync-fd deferred=1 fd=" << historyCompletionFd << '\n';
            } else { this->asyncZeroHistoryEnabled_=false; std::cerr << "lsfg-vk: zero-history-sync-fd disabled after framegen fallback\n"; }
        }
'''
    t=t[:zs]+block[:a]+mid+block[b:]+t[ze:]
    p.write_text(t)


def apply(root: Path) -> None:
    patch_mini(root/"include/mini/semaphore.hpp",root/"src/mini/semaphore.cpp")
    patch_core(root/"framegen/include/core/semaphore.hpp",root/"framegen/src/core/semaphore.cpp")
    for x in ("framegen/v3.1_include/v3_1/context.hpp","framegen/v3.1p_include/v3_1p/context.hpp"): patch_fg_header(root/x)
    for x in ("framegen/v3.1_src/context.cpp","framegen/v3.1p_src/context.cpp"): patch_fg_source(root/x)
    patch_public(root/"framegen/public/lsfg_3_1.hpp",root/"framegen/v3.1_src/lsfg.cpp","LSFG_3_1")
    patch_public(root/"framegen/public/lsfg_3_1p.hpp",root/"framegen/v3.1p_src/lsfg.cpp","LSFG_3_1P")
    patch_outer_header(root/"include/context.hpp"); patch_outer(root/"src/context.cpp")

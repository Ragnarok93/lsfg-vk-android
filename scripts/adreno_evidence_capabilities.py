from __future__ import annotations

from pathlib import Path
from adreno_evidence_common import replace_exact

def patch_backend_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "externalSemaphoreSyncFd" in text:
        return
    old = "    bool externalSemaphoreOpaqueFd{false};\n"
    new = (
        old
        + "    // Diagnostic-only probe for the Android/Linux one-shot SYNC_FD\n"
        + "    // semaphore payload. This build does not enable a SYNC_FD handoff;\n"
        + "    // it only records whether both Vulkan devices could support one.\n"
        + "    bool externalSemaphoreSyncFd{false};\n"
    )
    text = replace_exact(text, old, new, count=1, label=f"{path}: sync-fd diagnostic")
    path.write_text(text, encoding="utf-8")

def patch_device_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "externalSemaphoreSyncFd=" in text:
        return

    text = replace_exact(
        text,
        "bool probeOpaqueFdSemaphoreSupport(VkPhysicalDevice physicalDevice,\n"
        "        const std::vector<VkExtensionProperties>& availableExtensions) {\n",
        "bool probeExternalSemaphoreSupport(VkPhysicalDevice physicalDevice,\n"
        "        const std::vector<VkExtensionProperties>& availableExtensions,\n"
        "        VkExternalSemaphoreHandleTypeFlagBits handleType) {\n",
        count=1,
        label=f"{path}: generic external semaphore probe",
    )
    text = replace_exact(
        text,
        "        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,\n",
        "        .handleType = handleType,\n",
        count=1,
        label=f"{path}: probe handle type",
    )
    text = replace_exact(
        text,
        "            & VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT) != 0;\n",
        "            & handleType) != 0;\n",
        count=1,
        label=f"{path}: compatible handle type",
    )
    text = replace_exact(
        text,
        "    this->diagnostics.externalSemaphoreOpaqueFd =\n"
        "        probeOpaqueFdSemaphoreSupport(physicalDevice, availableExtensions);\n",
        "    this->diagnostics.externalSemaphoreOpaqueFd = probeExternalSemaphoreSupport(\n"
        "        physicalDevice, availableExtensions,\n"
        "        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);\n"
        "    this->diagnostics.externalSemaphoreSyncFd = probeExternalSemaphoreSupport(\n"
        "        physicalDevice, availableExtensions,\n"
        "        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);\n",
        count=1,
        label=f"{path}: diagnostics probes",
    )
    text = replace_exact(
    text,
    "              << \" sync=\" << synchronizationPathName(decision.synchronizationPath) << '\\n';\n",
    "              << \" externalSemaphoreOpaqueFd=\"\n"
    "              << (this->diagnostics.externalSemaphoreOpaqueFd ? 1 : 0)\n"
    "              << \" externalSemaphoreSyncFd=\"\n"
    "              << (this->diagnostics.externalSemaphoreSyncFd ? 1 : 0)\n"
    "              << \" subgroupSize=\" << subgroup.subgroupSize\n"
    "              << \" subgroupStages=\" << subgroup.supportedStages\n"
    "              << \" subgroupOperations=\" << subgroup.supportedOperations\n"
    "              << \" sync=\" << synchronizationPathName(decision.synchronizationPath) << '\\n';\n",
    count=1,
    label=f"{path}: capability log",
)
    path.write_text(text, encoding="utf-8")

def patch_hooks_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "androidSyncFdSemaphoreSupported" in text:
        return
    text = replace_exact(
        text,
        "        bool androidOpaqueFdSemaphoreSupported{false};\n",
        "        bool androidOpaqueFdSemaphoreSupported{false};\n"
        "        // Diagnostic-only: capability is logged but not yet selected as\n"
        "        // the cross-device handoff payload.\n"
        "        bool androidSyncFdSemaphoreSupported{false};\n",
        count=1,
        label=f"{path}: sync-fd DeviceInfo field",
    )
    path.write_text(text, encoding="utf-8")

def patch_hooks_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "syncFdSemaphore=" in text and "present-error stage=" in text:
        return

    text = replace_exact(
        text,
        "    bool supportsOpaqueFdSemaphore(VkPhysicalDevice physicalDevice) {\n",
        "    bool supportsExternalSemaphore(VkPhysicalDevice physicalDevice,\n"
        "            VkExternalSemaphoreHandleTypeFlagBits handleType) {\n",
        count=1,
        label=f"{path}: generic game-side external semaphore probe",
    )
    text = replace_exact(
        text,
        "            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,\n",
        "            .handleType = handleType,\n",
        count=1,
        label=f"{path}: game-side handle type",
    )
    text = replace_exact(
        text,
        "                & VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT) != 0;\n",
        "                & handleType) != 0;\n",
        count=1,
        label=f"{path}: game-side compatible handle type",
    )

    text = replace_exact(
        text,
        "        const bool opaqueFdSemaphoreSupported = supportsOpaqueFdSemaphore(physicalDevice);\n"
        "        if (opaqueFdSemaphoreSupported)\n"
        "            requestedExtensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);\n",
        "        const bool opaqueFdSemaphoreSupported = supportsExternalSemaphore(\n"
        "            physicalDevice, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);\n"
        "        const bool syncFdSemaphoreSupported = supportsExternalSemaphore(\n"
        "            physicalDevice, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);\n"
        "        if (opaqueFdSemaphoreSupported)\n"
        "            requestedExtensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);\n",
        count=1,
        label=f"{path}: game-side capability probes",
    )
    text = replace_exact(
        text,
        "        std::cerr << \"lsfg-vk: init stage=android-sync-capability opaqueFdSemaphore=\"\n"
        "                  << (opaqueFdSemaphoreSupported ? 1 : 0)\n"
        "                  << \" fallback=host-fence\\n\";\n",
        "        std::cerr << \"lsfg-vk: init stage=android-sync-capability opaqueFdSemaphore=\"\n"
        "                  << (opaqueFdSemaphoreSupported ? 1 : 0)\n"
        "                  << \" syncFdSemaphore=\" << (syncFdSemaphoreSupported ? 1 : 0)\n"
        "                  << \" selected=\" << (opaqueFdSemaphoreSupported ? \"opaque-fd\" : \"host-fence\")\n"
        "                  << \" fallback=host-fence\\n\";\n",
        count=1,
        label=f"{path}: game-side capability log",
    )
    text = replace_exact(
        text,
        "        const bool androidOpaqueFdSemaphoreSupported = supportsOpaqueFdSemaphore(physicalDevice);\n",
        "        const bool androidOpaqueFdSemaphoreSupported = supportsExternalSemaphore(\n"
        "            physicalDevice, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);\n"
        "        const bool androidSyncFdSemaphoreSupported = supportsExternalSemaphore(\n"
        "            physicalDevice, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);\n",
        count=1,
        label=f"{path}: post-create semaphore probes",
    )
    text = replace_exact(
        text,
        "        const bool androidOpaqueFdSemaphoreSupported = false;\n",
        "        const bool androidOpaqueFdSemaphoreSupported = false;\n"
        "        const bool androidSyncFdSemaphoreSupported = false;\n",
        count=1,
        label=f"{path}: non-Android sync-fd default",
    )
    text = replace_exact(
        text,
        "            .androidOpaqueFdSemaphoreSupported = androidOpaqueFdSemaphoreSupported,\n",
        "            .androidOpaqueFdSemaphoreSupported = androidOpaqueFdSemaphoreSupported,\n"
        "            .androidSyncFdSemaphoreSupported = androidSyncFdSemaphoreSupported,\n",
        count=1,
        label=f"{path}: DeviceInfo sync-fd assignment",
    )

    text = replace_exact(
        text,
        "            Utils::logLimitN(\"swapPresent\", 5,\n"
        "                \"An error occurred while presenting the swapchain; degrading to native presentation:\\n\"\n"
        "                \"- \" + std::string(e.what()));\n"
        "            swapchains.erase(*pPresentInfo->pSwapchains);\n"
        "            std::cerr << \"lsfg-vk: runtime stage=context-degraded-bypass reason=present-error\\n\";\n",
        "            const std::string presentFailureStage = swapchain.lastDiagnosticStage();\n"
        "            Utils::logLimitN(\"swapPresent\", 5,\n"
        "                \"An error occurred while presenting the swapchain; degrading to native presentation:\\n\"\n"
        "                \"- stage=\" + presentFailureStage + \" error=\" + std::string(e.what()));\n"
        "            swapchains.erase(*pPresentInfo->pSwapchains);\n"
        "            std::cerr << \"lsfg-vk: runtime stage=context-degraded-bypass reason=present-error stage=\"\n"
        "                      << presentFailureStage << \"\\n\";\n",
        count=1,
        label=f"{path}: present failure stage context",
    )
    path.write_text(text, encoding="utf-8")

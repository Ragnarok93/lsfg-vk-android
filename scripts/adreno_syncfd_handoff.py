from __future__ import annotations

from pathlib import Path

from adreno_evidence_common import replace_exact


def patch_capability_selection(hooks_path: Path, device_path: Path) -> None:
    hooks = hooks_path.read_text(encoding="utf-8")
    if 'selected=sync-fd' not in hooks:
        hooks = replace_exact(
            hooks,
            "        if (opaqueFdSemaphoreSupported)\n"
            "            requestedExtensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);\n",
            "        if (opaqueFdSemaphoreSupported || syncFdSemaphoreSupported)\n"
            "            requestedExtensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);\n",
            count=1,
            label=f"{hooks_path}: enable external semaphore fd for sync-fd",
        )
        hooks = replace_exact(
            hooks,
            '                  << " selected=" << (opaqueFdSemaphoreSupported ? "opaque-fd" : "host-fence")\n',
            '                  << " selected=" << (syncFdSemaphoreSupported ? "sync-fd"\n'
            '                      : (opaqueFdSemaphoreSupported ? "opaque-fd" : "host-fence"))\n',
            count=1,
            label=f"{hooks_path}: prefer sync-fd selection",
        )
        hooks_path.write_text(hooks, encoding="utf-8")

    device = device_path.read_text(encoding="utf-8")
    if "externalSemaphoreSyncFd" in device and "externalSemaphoreOpaqueFd\n            || this->diagnostics.externalSemaphoreSyncFd" not in device:
        device = replace_exact(
            device,
            "    if (this->diagnostics.externalSemaphoreOpaqueFd)\n"
            "        enabledExtensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);\n",
            "    if (this->diagnostics.externalSemaphoreOpaqueFd\n"
            "            || this->diagnostics.externalSemaphoreSyncFd)\n"
            "        enabledExtensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);\n",
            count=1,
            label=f"{device_path}: enable framegen external semaphore fd for sync-fd",
        )
        device_path.write_text(device, encoding="utf-8")


def patch_mini_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "int exportFd(VkDevice device" in text:
        return
    text = replace_exact(
        text,
        "        explicit Semaphore(VkDevice device);\n"
        "        Semaphore(VkDevice device, int* fd);\n\n"
        "        [[nodiscard]] VkSemaphore handle() const {\n",
        "        explicit Semaphore(VkDevice device);\n"
        "        Semaphore(VkDevice device, int* fd);\n"
        "        Semaphore(VkDevice device, VkExternalSemaphoreHandleTypeFlagBits handleType);\n\n"
        "        [[nodiscard]] int exportFd(\n"
        "            VkDevice device, VkExternalSemaphoreHandleTypeFlagBits handleType) const;\n\n"
        "        [[nodiscard]] VkSemaphore handle() const {\n",
        count=1,
        label=f"{path}: delayed external semaphore export API",
    )
    path.write_text(text, encoding="utf-8")


def patch_mini_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "int Semaphore::exportFd(" in text:
        return
    old = '''Semaphore::Semaphore(VkDevice device, int* fd) {
    if (fd == nullptr)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Semaphore export fd pointer is null");

    // Resolve the extension entrypoint from this logical device instead of the
    // desktop-only cached layer function. On Android the extension is optional:
    // callers catch failure here and keep the proven host-fence AHB handoff.
    const auto getSemaphoreFd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        Layer::ovkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR"));
    if (getSemaphoreFd == nullptr)
        throw LSFG::vulkan_error(VK_ERROR_EXTENSION_NOT_PRESENT,
            "External semaphore fd export is unavailable");

    const VkExportSemaphoreCreateInfo exportInfo{
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT
    };
    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &exportInfo
    };
    VkSemaphore semaphoreHandle{};
    auto res = Layer::ovkCreateSemaphore(device, &desc, nullptr, &semaphoreHandle);
    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Unable to create exportable semaphore");

    const VkSemaphoreGetFdInfoKHR fdInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore = semaphoreHandle,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT
    };
    *fd = -1;
    res = getSemaphoreFd(device, &fdInfo, fd);
    if (res != VK_SUCCESS || *fd < 0) {
        Layer::ovkDestroySemaphore(device, semaphoreHandle, nullptr);
        throw LSFG::vulkan_error(res, "Unable to export semaphore to fd");
    }

    this->semaphore = ownSemaphore(device, semaphoreHandle);
}
'''
    new = '''Semaphore::Semaphore(
        VkDevice device, VkExternalSemaphoreHandleTypeFlagBits handleType) {
    const VkExportSemaphoreCreateInfo exportInfo{
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = handleType,
    };
    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &exportInfo,
    };
    VkSemaphore semaphoreHandle{};
    const auto res = Layer::ovkCreateSemaphore(device, &desc, nullptr, &semaphoreHandle);
    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Unable to create exportable semaphore");
    this->semaphore = ownSemaphore(device, semaphoreHandle);
}

Semaphore::Semaphore(VkDevice device, int* fd)
    : Semaphore(device, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT) {
    if (fd == nullptr)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Semaphore export fd pointer is null");
    *fd = this->exportFd(device, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
}

int Semaphore::exportFd(
        VkDevice device, VkExternalSemaphoreHandleTypeFlagBits handleType) const {
    if (!this->semaphore || *this->semaphore == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "External semaphore handle is unavailable");

    const auto getSemaphoreFd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        Layer::ovkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR"));
    if (getSemaphoreFd == nullptr)
        throw LSFG::vulkan_error(VK_ERROR_EXTENSION_NOT_PRESENT,
            "External semaphore fd export is unavailable");

    const VkSemaphoreGetFdInfoKHR fdInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore = *this->semaphore,
        .handleType = handleType,
    };
    int fd = -1;
    const auto res = getSemaphoreFd(device, &fdInfo, &fd);
    if (res != VK_SUCCESS || fd < 0)
        throw LSFG::vulkan_error(res, "Unable to export semaphore to fd");
    return fd;
}
'''
    text = replace_exact(text, old, new, count=1, label=f"{path}: delayed semaphore fd export")
    path.write_text(text, encoding="utf-8")


def patch_core_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "VK_SEMAPHORE_IMPORT_TEMPORARY_BIT" in text:
        return
    old = '''Semaphore::Semaphore(const Core::Device& device, int fd) {
    if (fd < 0)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Invalid semaphore fd");

    // Imported OPAQUE_FD semaphores do not need to be exportable from the
    // framegen device. Keeping this object import-only avoids requesting more
    // external-semaphore capability than the actual cross-device wait needs.
    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    VkSemaphore semaphoreHandle{};
    auto res = vkCreateSemaphore(device.handle(), &desc, nullptr, &semaphoreHandle);
    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE) {
        ::close(fd);
        throw LSFG::vulkan_error(res, "Unable to create imported semaphore");
    }

    const auto importSemaphoreFd = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
        vkGetDeviceProcAddr(device.handle(), "vkImportSemaphoreFdKHR"));
    if (importSemaphoreFd == nullptr) {
        vkDestroySemaphore(device.handle(), semaphoreHandle, nullptr);
        ::close(fd);
        throw LSFG::vulkan_error(VK_ERROR_EXTENSION_NOT_PRESENT,
            "External semaphore fd import is unavailable");
    }

    const VkImportSemaphoreFdInfoKHR importInfo{
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .semaphore = semaphoreHandle,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
        .fd = fd
    };
    res = importSemaphoreFd(device.handle(), &importInfo);
    if (res != VK_SUCCESS) {
        // Vulkan only takes ownership of an OPAQUE_FD after a successful import.
        // Close it here on failure so an optional async handoff cannot leak FDs.
        vkDestroySemaphore(device.handle(), semaphoreHandle, nullptr);
        ::close(fd);
        throw LSFG::vulkan_error(res, "Unable to import semaphore from fd");
    }

    // Successful import transfers ownership of fd to Vulkan.
    this->isTimeline = false;
    this->semaphore = std::shared_ptr<VkSemaphore>(
        new VkSemaphore(semaphoreHandle),
        [dev = device.handle()](VkSemaphore* ownedSemaphore) {
            vkDestroySemaphore(dev, *ownedSemaphore, nullptr);
        }
    );
}
'''
    new = '''Semaphore::Semaphore(const Core::Device& device, int fd) {
    if (fd < 0)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Invalid semaphore fd");

    const auto importSemaphoreFd = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
        vkGetDeviceProcAddr(device.handle(), "vkImportSemaphoreFdKHR"));
    if (importSemaphoreFd == nullptr) {
        ::close(fd);
        throw LSFG::vulkan_error(VK_ERROR_EXTENSION_NOT_PRESENT,
            "External semaphore fd import is unavailable");
    }

    std::array<VkExternalSemaphoreHandleTypeFlagBits, 2> candidates{
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    size_t candidateCount = 1;
#ifdef __ANDROID__
    const auto& diagnostics = device.getDiagnostics();
    candidateCount = 0;
    if (diagnostics.externalSemaphoreSyncFd)
        candidates.at(candidateCount++) = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    if (diagnostics.externalSemaphoreOpaqueFd)
        candidates.at(candidateCount++) = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    if (candidateCount == 0) {
        ::close(fd);
        throw LSFG::vulkan_error(VK_ERROR_FORMAT_NOT_SUPPORTED,
            "No compatible external semaphore fd payload is available");
    }
#endif

    VkResult lastResult = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    for (size_t candidate = 0; candidate < candidateCount; ++candidate) {
        const auto handleType = candidates.at(candidate);
        const VkSemaphoreCreateInfo desc{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        VkSemaphore semaphoreHandle{};
        auto res = vkCreateSemaphore(device.handle(), &desc, nullptr, &semaphoreHandle);
        if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE) {
            lastResult = res;
            continue;
        }

        const VkImportSemaphoreFdInfoKHR importInfo{
            .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            .flags = handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
                ? VK_SEMAPHORE_IMPORT_TEMPORARY_BIT : 0,
            .semaphore = semaphoreHandle,
            .handleType = handleType,
            .fd = fd,
        };
        res = importSemaphoreFd(device.handle(), &importInfo);
        if (res == VK_SUCCESS) {
            this->isTimeline = false;
            this->semaphore = std::shared_ptr<VkSemaphore>(
                new VkSemaphore(semaphoreHandle),
                [dev = device.handle()](VkSemaphore* ownedSemaphore) {
                    vkDestroySemaphore(dev, *ownedSemaphore, nullptr);
                }
            );
            return;
        }

        // Failed imports do not consume fd, so a compatible fallback payload
        // type may still be attempted on the same descriptor.
        lastResult = res;
        vkDestroySemaphore(device.handle(), semaphoreHandle, nullptr);
    }

    ::close(fd);
    throw LSFG::vulkan_error(lastResult, "Unable to import semaphore from fd");
}
'''
    text = replace_exact(text, old, new, count=1, label=f"{path}: sync-fd temporary import")
    text = replace_exact(
        text,
        "#include <optional>\n#include <cstdint>\n#include <memory>\n",
        "#include <optional>\n#include <array>\n#include <cstddef>\n#include <cstdint>\n#include <memory>\n",
        count=1,
        label=f"{path}: sync-fd import candidate includes",
    )
    path.write_text(text, encoding="utf-8")


def patch_outer_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "asyncAhbHandoffHandleType_" in text:
        return
    text = replace_exact(
        text,
        "    bool asyncAhbHandoffEnabled_{false};\n",
        "    bool asyncAhbHandoffEnabled_{false};\n"
        "    bool generatedAsyncAhbHandoffEnabled_{false};\n"
        "    VkExternalSemaphoreHandleTypeFlagBits asyncAhbHandoffHandleType_{\n"
        "        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};\n",
        count=1,
        label=f"{path}: selected async handoff handle type",
    )
    path.write_text(text, encoding="utf-8")


def patch_outer_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "gpu-sync-fd" in text and ".exportFd(" in text:
        return

    text = replace_exact(
        text,
        "    this->asyncAhbHandoffEnabled_ =\n"
        "        info.androidOpaqueFdSemaphoreSupported\n"
        "        && backendDiagnostics.externalSemaphoreOpaqueFd\n"
        "        && gameGetSemaphoreFd != nullptr;\n",
        "    const bool syncFdHandoffSupported = info.androidSyncFdSemaphoreSupported\n"
        "        && backendDiagnostics.externalSemaphoreSyncFd;\n"
        "    const bool opaqueFdHandoffSupported = info.androidOpaqueFdSemaphoreSupported\n"
        "        && backendDiagnostics.externalSemaphoreOpaqueFd;\n"
        "    this->asyncAhbHandoffEnabled_ =\n"
        "        (syncFdHandoffSupported || opaqueFdHandoffSupported)\n"
        "        && gameGetSemaphoreFd != nullptr;\n"
        "    this->asyncAhbHandoffHandleType_ = syncFdHandoffSupported\n"
        "        ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT\n"
        "        : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;\n",
        count=1,
        label=f"{path}: prefer sync-fd async handoff",
    )
    text = replace_exact(
        text,
        "            pass.framegenInputSemaphore =\n"
        "                Mini::Semaphore(info.device, &framegenInputSemaphoreFd);\n",
        "            pass.framegenInputSemaphore = Mini::Semaphore(\n"
        "                info.device, this->asyncAhbHandoffHandleType_);\n",
        count=1,
        label=f"{path}: defer external semaphore fd export until submit",
    )
    text = replace_exact(
        text,
        "            std::cerr << \"lsfg-vk: Android async AHB handoff disabled after export failure: \"\n",
        "            std::cerr << \"lsfg-vk: Android async AHB handoff disabled after setup failure: \"\n",
        count=1,
        label=f"{path}: pre-submit setup failure log",
    )

    \
    old_async_counts = (
        "        metrics.windowAsyncHandoffs++;\n"
        "        metrics.totalAsyncHandoffs++;\n"
    )
    new_async_counts = (
        "        try {\n"
        "            // SYNC_FD has copy-transference semantics. Export only after\n"
        "            // the queue signal operation is pending so the FD represents\n"
        "            // that exact source-copy completion point without a host wait.\n"
        "            framegenInputSemaphoreFd = pass.framegenInputSemaphore.exportFd(\n"
        "                info.device, this->asyncAhbHandoffHandleType_);\n"
        "        } catch (const std::exception& e) {\n"
        "            waitForAhbHandoff(\n"
        "                info.device, *this->ahbHandoffFence, this->waitHandoffFences);\n"
        "            this->asyncAhbHandoffEnabled_ = false;\n"
        "            useAsyncHandoff = false;\n"
        "            framegenInputSemaphoreFd = -1;\n"
        "            metrics.totalAsyncFallbacks++;\n"
        "            std::cerr << \"lsfg-vk: Android async AHB handoff disabled after fd export failure: \"\n"
        "                      << e.what() << \"; completed source copy with host fence fallback\\n\";\n"
        "        }\n"
        "        if (useAsyncHandoff) {\n"
        "            metrics.windowAsyncHandoffs++;\n"
        "            metrics.totalAsyncHandoffs++;\n"
        "        } else {\n"
        "            metrics.windowSyncHandoffs++;\n"
        "            metrics.totalSyncHandoffs++;\n"
        "        }\n"
    )
    text = replace_exact(
        text, old_async_counts, new_async_counts,
        count=1, label=f"{path}: post-submit sync-fd export and fallback",
    )

    text = replace_exact(
        text,
        "                  << (useAsyncHandoff ? \"gpu-semaphore\" : \"host-fence\") << \"\\n\";\n",
        "                  << (useAsyncHandoff\n"
        "                      ? (this->asyncAhbHandoffHandleType_\n"
        "                          == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT\n"
        "                          ? \"gpu-sync-fd\" : \"gpu-opaque-fd\")\n"
        "                      : \"host-fence\") << \"\\n\";\n",
        count=1,
        label=f"{path}: source handoff mode diagnostic",
    )
    text = replace_exact(
        text,
        "                  << \" handoff=\" << (useAsyncHandoff ? \"gpu-semaphore\" : \"host-fence\")\n",
        "                  << \" handoff=\" << (useAsyncHandoff\n"
        "                      ? (this->asyncAhbHandoffHandleType_\n"
        "                          == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT\n"
        "                          ? \"gpu-sync-fd\" : \"gpu-opaque-fd\")\n"
        "                      : \"host-fence\")\n",
        count=1,
        label=f"{path}: dispatch handoff diagnostic",
    )
    path.write_text(text, encoding="utf-8")


def apply(root: Path) -> None:
    patch_capability_selection(root / "src/hooks.cpp", root / "framegen/src/core/device.cpp")
    patch_mini_header(root / "include/mini/semaphore.hpp")
    patch_mini_source(root / "src/mini/semaphore.cpp")
    patch_core_source(root / "framegen/src/core/semaphore.cpp")
    patch_outer_header(root / "include/context.hpp")
    patch_outer_source(root / "src/context.cpp")

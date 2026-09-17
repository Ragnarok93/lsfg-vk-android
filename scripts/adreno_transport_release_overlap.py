#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path

from adreno_evidence_common import replace_exact


FRAMEGEN_HEADERS = (
    Path("framegen/v3.1_include/v3_1/context.hpp"),
    Path("framegen/v3.1p_include/v3_1p/context.hpp"),
)
FRAMEGEN_SOURCES = (
    Path("framegen/v3.1_src/context.cpp"),
    Path("framegen/v3.1p_src/context.cpp"),
)


def once(text: str, old: str, new: str, label: str) -> str:
    return replace_exact(text, old, new, count=1, label=label)


def patch_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "transportReleaseCommandBuffer" in text:
        return

    old = """            Core::Semaphore historyCompletionSemaphore;\n            bool preprocessingPending{false};\n"""
    new = """            Core::Semaphore historyCompletionSemaphore;\n            Core::Semaphore transportReadySemaphore;\n            Core::Fence transportReleaseFence;\n            Core::CommandBuffer transportReleaseCommandBuffer;\n            bool preprocessingPending{false};\n"""
    text = once(text, old, new, f"{path}: transport release overlap state")
    path.write_text(text, encoding="utf-8")


def patch_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "zero-history-sync-fd transport-release-submit" in text:
        return

    fence_init = "        data.preprocessingFence = Core::Fence(vk.device);\n"
    count = text.count(fence_init)
    if count < 1:
        raise RuntimeError(f"{path}: preprocessing fence initialization not found")
    text = text.replace(
        fence_init,
        fence_init + "        data.transportReleaseFence = Core::Fence(vk.device);\n",
    )

    old_anchor = """    Core::Image& activePrivateHistoryInput = (this->frameIdx % 2 == 0)\n        ? this->inImg_0 : this->inImg_1;\n    if (this->inputCopyRequired) {\n"""
    new_anchor = """    Core::Image& activePrivateHistoryInput = (this->frameIdx % 2 == 0)\n        ? this->inImg_0 : this->inImg_1;\n    bool transportOnlyHistoryOverlap =\n        zeroGenerationTransportOnly && historyCompletionFd != nullptr;\n    if (transportOnlyHistoryOverlap) {\n        try {\n            data.historyCompletionSemaphore = Core::Semaphore(\n                vk.device, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);\n            data.transportReadySemaphore = Core::Semaphore(vk.device);\n            data.transportReleaseFence.reset(vk.device);\n            data.preprocessingFence.reset(vk.device);\n            data.transportReleaseCommandBuffer = Core::CommandBuffer(vk.device, vk.commandPool);\n            data.transportReleaseCommandBuffer.begin();\n            std::vector<VkImageMemoryBarrier2> transportBarriers;\n            transportBarriers.reserve(2);\n            add_external_transfer_acquire(transportBarriers, vk, activeSharedHistoryInput,\n                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);\n            add_local_transition(transportBarriers, activePrivateHistoryInput,\n                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,\n                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,\n                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);\n            emit_external_barriers(data.transportReleaseCommandBuffer, transportBarriers);\n            copy_same_format(data.transportReleaseCommandBuffer,\n                activeSharedHistoryInput, activePrivateHistoryInput);\n            transportBarriers.clear();\n            add_local_transition(transportBarriers, activePrivateHistoryInput,\n                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,\n                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,\n                VK_IMAGE_LAYOUT_GENERAL);\n            add_external_transfer_release(transportBarriers, vk, activeSharedHistoryInput,\n                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);\n            emit_external_barriers(data.transportReleaseCommandBuffer, transportBarriers);\n            data.transportReleaseCommandBuffer.end();\n        } catch (const std::exception& e) {\n            transportOnlyHistoryOverlap = false;\n            data.historyCompletionSemaphore = Core::Semaphore{};\n            data.transportReadySemaphore = Core::Semaphore{};\n            std::cerr << \"lsfg-vk: zero-history transport-release setup fallback: \"\n                      << e.what() << '\\n';\n        }\n    }\n    if (this->inputCopyRequired && !transportOnlyHistoryOverlap) {\n"""
    text = once(text, old_anchor, new_anchor, f"{path}: record early transport release")

    old_else = """        emit_external_barriers(data.cmdBuffer1, barriers);\n    } else {\n        std::vector<VkImageMemoryBarrier2> acquireBarriers;\n"""
    new_else = """        emit_external_barriers(data.cmdBuffer1, barriers);\n    } else if (!this->inputCopyRequired) {\n        std::vector<VkImageMemoryBarrier2> acquireBarriers;\n"""
    text = once(text, old_else, new_else, f"{path}: skip duplicate transport recording")

    old_create = """                data.historyCompletionSemaphore=Core::Semaphore(vk.device,VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);\n                data.preprocessingFence.reset(vk.device);\n                data.cmdBuffer1.submit(vk.device.getComputeQueue(),data.preprocessingFence,waits,std::nullopt,{data.historyCompletionSemaphore},std::nullopt);\n                data.preprocessingPending=true;\n"""
    new_create = """                if (!transportOnlyHistoryOverlap) {\n                    data.historyCompletionSemaphore=Core::Semaphore(vk.device,VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);\n                    data.preprocessingFence.reset(vk.device);\n                    data.cmdBuffer1.submit(vk.device.getComputeQueue(),data.preprocessingFence,waits,std::nullopt,{data.historyCompletionSemaphore},std::nullopt);\n                } else {\n                    bool transportReleaseSubmitted = false;\n                    try {\n                        data.transportReleaseCommandBuffer.submit(\n                            vk.device.getComputeQueue(), data.transportReleaseFence, waits,\n                            std::nullopt, {data.historyCompletionSemaphore,data.transportReadySemaphore},\n                            std::nullopt);\n                        transportReleaseSubmitted = true;\n                        data.cmdBuffer1.submit(\n                            vk.device.getComputeQueue(), data.preprocessingFence,\n                            {data.transportReadySemaphore}, std::nullopt, {}, std::nullopt);\n                        std::cerr << \"lsfg-vk: zero-history-sync-fd transport-release-submit\\n\";\n                    } catch (...) {\n                        if (transportReleaseSubmitted\n                                && !data.transportReleaseFence.wait(vk.device, framegenWaitTimeoutNs()))\n                            throw LSFG::vulkan_error(VK_TIMEOUT,\n                                \"TransportOnly zero-history release wait timed out after submit failure\");\n                        data.transportReadySemaphore = Core::Semaphore{};\n                        data.historyCompletionSemaphore = Core::Semaphore{};\n                        throw;\n                    }\n                }\n                data.preprocessingPending=true;\n"""
    text = once(text, old_create, new_create, f"{path}: split transport and preprocessing submissions")

    old_setup_catch = """            } catch (const std::exception& e) {\n                data.preprocessingPending=false; data.historyCompletionSemaphore=Core::Semaphore{};\n                std::cerr << \"lsfg-vk: zero-history-sync-fd setup fallback: \" << e.what() << '\\n';\n            }\n"""
    new_setup_catch = """            } catch (const std::exception& e) {\n                data.preprocessingPending=false;\n                data.transportReadySemaphore=Core::Semaphore{};\n                data.historyCompletionSemaphore=Core::Semaphore{};\n                if (transportOnlyHistoryOverlap)\n                    throw;\n                std::cerr << \"lsfg-vk: zero-history-sync-fd setup fallback: \" << e.what() << '\\n';\n            }\n"""
    text = once(text, old_setup_catch, new_setup_catch, f"{path}: preserve submitted transport failure")

    cleanup = "data.preprocessingPending=false; data.historyCompletionSemaphore=Core::Semaphore{};"
    text = text.replace(
        cleanup,
        "data.preprocessingPending=false; data.transportReadySemaphore=Core::Semaphore{}; "
        "data.historyCompletionSemaphore=Core::Semaphore{};",
    )

    path.write_text(text, encoding="utf-8")


def apply(root: Path) -> None:
    for relative in FRAMEGEN_HEADERS:
        patch_header(root / relative)
    for relative in FRAMEGEN_SOURCES:
        patch_source(root / relative)

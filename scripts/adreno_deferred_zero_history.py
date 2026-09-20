#!/usr/bin/env python3
"""Candidate A: defer sustained adaptive-zero framegen history on Android.

Applied after the existing SYNC_FD/slot/release-overlap transforms.  The
adaptive scheduler remains authoritative for generation decisions; this patch
only changes how much temporal maintenance LsContext performs after plan()
returns zero for a sustained interval.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from adreno_evidence_common import replace_exact


def once(text: str, old: str, new: str, label: str) -> str:
    return replace_exact(text, old, new, count=1, label=label)


def patch_mini_image_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "DeviceLocalImageTag" in text:
        return
    text = once(
        text,
        "namespace Mini {\n\n",
        "namespace Mini {\n\n"
        "#ifdef __ANDROID__\n"
        "    // Candidate A raw history never crosses a VkDevice boundary.\n"
        "    struct DeviceLocalImageTag {};\n"
        "#endif\n\n",
        f"{path}: device-local image tag",
    )
    text = once(
        text,
        "#ifdef __ANDROID__\n        ///\n        /// Create the image backed by an AHardwareBuffer (Android path).\n",
        "#ifdef __ANDROID__\n"
        "        /// Create a plain game-device-local image with no external-memory contract.\n"
        "        Image(VkDevice device, VkPhysicalDevice physicalDevice, VkExtent2D extent,\n"
        "            VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspectFlags,\n"
        "            DeviceLocalImageTag);\n\n"
        "        ///\n"
        "        /// Create the image backed by an AHardwareBuffer (Android path).\n",
        f"{path}: device-local constructor declaration",
    )
    path.write_text(text, encoding="utf-8")


def patch_mini_image_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "DeviceLocalImageTag)" in text:
        return
    marker = "#ifdef __ANDROID__\nImage::Image(VkDevice device, VkPhysicalDevice physicalDevice,\n"
    block = r'''#ifdef __ANDROID__
Image::Image(VkDevice device, VkPhysicalDevice physicalDevice,
        VkExtent2D extent, VkFormat format,
        VkImageUsageFlags usage, VkImageAspectFlags aspectFlags,
        DeviceLocalImageTag)
        : extent(extent), format(format), aspectFlags(aspectFlags) {
    const VkImageCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { extent.width, extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage imageHandle = VK_NULL_HANDLE;
    auto res = Layer::ovkCreateImage(device, &desc, nullptr, &imageHandle);
    if (res != VK_SUCCESS || imageHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create device-local raw-history image");

    VkMemoryRequirements requirements{};
    Layer::ovkGetImageMemoryRequirements(device, imageHandle, &requirements);
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    Layer::ovkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    uint32_t memoryType = UINT32_MAX;
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) != 0
                && (memoryProperties.memoryTypes[i].propertyFlags
                    & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
            memoryType = i;
            break;
        }
    }
    if (memoryType == UINT32_MAX) {
        Layer::ovkDestroyImage(device, imageHandle, nullptr);
        throw LSFG::vulkan_error(VK_ERROR_OUT_OF_DEVICE_MEMORY,
            "No device-local memory type for raw-history image");
    }

    const VkMemoryAllocateInfo allocation{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memoryType,
    };
    VkDeviceMemory memoryHandle = VK_NULL_HANDLE;
    res = Layer::ovkAllocateMemory(device, &allocation, nullptr, &memoryHandle);
    if (res != VK_SUCCESS || memoryHandle == VK_NULL_HANDLE) {
        Layer::ovkDestroyImage(device, imageHandle, nullptr);
        throw LSFG::vulkan_error(res, "Failed to allocate raw-history image memory");
    }
    res = Layer::ovkBindImageMemory(device, imageHandle, memoryHandle, 0);
    if (res != VK_SUCCESS) {
        Layer::ovkFreeMemory(device, memoryHandle, nullptr);
        Layer::ovkDestroyImage(device, imageHandle, nullptr);
        throw LSFG::vulkan_error(res, "Failed to bind raw-history image memory");
    }

    // One shared owner destroys the image before freeing its bound memory.
    struct LocalAllocation { VkImage image; VkDeviceMemory memory; };
    auto allocationOwner = std::shared_ptr<LocalAllocation>(
        new LocalAllocation{imageHandle, memoryHandle},
        [device](LocalAllocation* allocation) {
            if (allocation != nullptr) {
                if (allocation->image != VK_NULL_HANDLE)
                    Layer::ovkDestroyImage(device, allocation->image, nullptr);
                if (allocation->memory != VK_NULL_HANDLE)
                    Layer::ovkFreeMemory(device, allocation->memory, nullptr);
                delete allocation;
            }
        });
    this->image = std::shared_ptr<VkImage>(allocationOwner, &allocationOwner->image);
    this->memory = std::shared_ptr<VkDeviceMemory>(allocationOwner, &allocationOwner->memory);
}
#endif

'''
    text = once(text, marker, block + marker, f"{path}: device-local constructor implementation")
    path.write_text(text, encoding="utf-8")


def patch_outer_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "kDeferredZeroDecisionThreshold" in text:
        return
    state = r'''    enum class HistoryMaintenanceState {
        LiveHistory,
        DeferredZero,
        ReprimeHistory,
    };
    static constexpr size_t kDeferredZeroDecisionThreshold = 6;
    static constexpr uint64_t kDeferredZeroMinimumDurationMs = 250;
    static constexpr uint64_t kRawHistoryBudgetBytes = 64ULL * 1024ULL * 1024ULL;
    HistoryMaintenanceState historyMaintenanceState_{HistoryMaintenanceState::LiveHistory};
    std::array<Mini::Image, 3> rawSourceHistory_{};
    std::array<bool, 3> rawSourceHistoryInitialized_{{false, false, false}};
    size_t rawSourceHistoryNext_{0};
    size_t rawSourceHistoryCount_{0};
    size_t consecutiveZeroDecisions_{0};
    std::chrono::steady_clock::time_point zeroDemandStart_{};
    bool zeroDemandActive_{false};
    bool deferredZeroRawHistoryAllocated_{false};
    bool deferredZeroDisabledForContext_{false};
    uint64_t framegenHistoryEpoch_{0};
    void invalidateDeferredZeroHistory();
    [[nodiscard]] std::array<size_t, 3> orderedRawHistorySlots() const;
'''
    text = once(
        text,
        "    bool asyncZeroHistoryEnabled_{false};\n",
        "    bool asyncZeroHistoryEnabled_{false};\n" + state,
        f"{path}: Candidate A state",
    )
    text = once(
        text,
        "        uint64_t totalAdaptiveZeroGenerationCycles{0};\n",
        "        uint64_t totalAdaptiveZeroGenerationCycles{0};\n"
        "        uint64_t windowDeferredZeroFrames{0};\n"
        "        uint64_t totalDeferredZeroFrames{0};\n"
        "        uint64_t windowDeferredZeroEntries{0};\n"
        "        uint64_t totalDeferredZeroEntries{0};\n"
        "        uint64_t windowDeferredReprimes{0};\n"
        "        uint64_t totalDeferredReprimes{0};\n"
        "        uint64_t totalDeferredFallbacks{0};\n",
        f"{path}: Candidate A metrics",
    )
    path.write_text(text, encoding="utf-8")


def patch_outer_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "deferred-zero source-only no-framegen" in text:
        return

    helper_marker = "// Acquire a generated AHB from framegen, copy it into an acquired swapchain\n"
    helpers = r'''// Copy a source frame into game-device-local history.  No EXTERNAL queue-family
// transfer occurs here: DeferredZero deliberately remains entirely on the game VkDevice.
void copySwapchainToRawHistory(VkCommandBuffer buf,
        VkImage swapchainImage, VkImage rawHistoryImage,
        uint32_t width, uint32_t height, bool firstUse) {
    const auto range = colorSubresourceRange();
    const VkImageMemoryBarrier acquireBarriers[] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchainImage,
            .subresourceRange = range,
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = firstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = rawHistoryImage,
            .subresourceRange = range,
        },
    };
    Layer::ovkCmdPipelineBarrier(buf,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, nullptr, 0, nullptr, static_cast<uint32_t>(std::size(acquireBarriers)), acquireBarriers);
    const auto blit = fullImageBlit(width, height);
    Layer::ovkCmdBlitImage(buf,
        swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        rawHistoryImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blit, VK_FILTER_NEAREST);
    const VkImageMemoryBarrier releaseBarriers[] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchainImage,
            .subresourceRange = range,
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = rawHistoryImage,
            .subresourceRange = range,
        },
    };
    Layer::ovkCmdPipelineBarrier(buf,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
        0, nullptr, 0, nullptr, static_cast<uint32_t>(std::size(releaseBarriers)), releaseBarriers);
}

// Re-prime copies one retained game-local source into the existing framegen AHB.
void copyRawHistoryToExternalAhb(VkCommandBuffer buf,
        VkImage rawHistoryImage, VkImage ahbImage,
        uint32_t width, uint32_t height, uint32_t graphicsFamily) {
    const auto range = colorSubresourceRange();
    const VkImageMemoryBarrier acquireBarriers[] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = rawHistoryImage,
            .subresourceRange = range,
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
            .dstQueueFamilyIndex = graphicsFamily,
            .image = ahbImage,
            .subresourceRange = range,
        },
    };
    Layer::ovkCmdPipelineBarrier(buf,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, nullptr, 0, nullptr, static_cast<uint32_t>(std::size(acquireBarriers)), acquireBarriers);
    const auto blit = fullImageBlit(width, height);
    Layer::ovkCmdBlitImage(buf,
        rawHistoryImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        ahbImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blit, VK_FILTER_NEAREST);
    const VkImageMemoryBarrier releaseBarriers[] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = rawHistoryImage,
            .subresourceRange = range,
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = 0,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = graphicsFamily,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
            .image = ahbImage,
            .subresourceRange = range,
        },
    };
    Layer::ovkCmdPipelineBarrier(buf,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
        0, nullptr, 0, nullptr, static_cast<uint32_t>(std::size(releaseBarriers)), releaseBarriers);
}

'''
    text = once(text, helper_marker, helpers + helper_marker, f"{path}: raw-history copy helpers")

    allocation_marker = r'''    this->frame_1 = Mini::Image(info.device, info.physicalDevice,
        extent, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
        ahbTransportMode, LSFG::AhbImageRole::Input);

'''
    allocation = r'''    this->frame_1 = Mini::Image(info.device, info.physicalDevice,
        extent, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
        ahbTransportMode, LSFG::AhbImageRole::Input);

    const uint64_t rawHistoryBytesPerPixel = format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8ULL : 4ULL;
    const uint64_t rawHistoryBytes = static_cast<uint64_t>(extent.width)
        * static_cast<uint64_t>(extent.height) * rawHistoryBytesPerPixel * 3ULL;
    if (rawHistoryBytes <= kRawHistoryBudgetBytes) {
        try {
            for (auto& image : this->rawSourceHistory_)
                image = Mini::Image(info.device, info.physicalDevice, extent, format,
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, Mini::DeviceLocalImageTag{});
            this->deferredZeroRawHistoryAllocated_ = true;
            std::cerr << "lsfg-vk: deferred-zero raw-history-ready bytes="
                      << rawHistoryBytes << " budget=" << kRawHistoryBudgetBytes << '\n';
        } catch (const std::exception& error) {
            this->rawSourceHistory_.fill(Mini::Image{});
            this->deferredZeroRawHistoryAllocated_ = false;
            this->deferredZeroDisabledForContext_ = true;
            std::cerr << "lsfg-vk: deferred-zero raw-history-allocation-fallback reason="
                      << error.what() << '\n';
        }
    } else {
        this->deferredZeroDisabledForContext_ = true;
        std::cerr << "lsfg-vk: deferred-zero raw-history-allocation-fallback bytes="
                  << rawHistoryBytes << " budget=" << kRawHistoryBudgetBytes << '\n';
    }

'''
    text = once(text, allocation_marker, allocation, f"{path}: eager raw-history allocation")

    method_marker = "VkResult LsContext::present(const Hooks::DeviceInfo& info, const void* pNext, VkQueue queue,\n"
    methods = r'''#ifdef __ANDROID__
void LsContext::invalidateDeferredZeroHistory() {
    this->historyMaintenanceState_ = HistoryMaintenanceState::LiveHistory;
    this->rawSourceHistoryNext_ = 0;
    this->rawSourceHistoryCount_ = 0;
    this->rawSourceHistoryInitialized_.fill(false);
    this->consecutiveZeroDecisions_ = 0;
    this->zeroDemandActive_ = false;
    this->zeroDemandStart_ = {};
}

std::array<size_t, 3> LsContext::orderedRawHistorySlots() const {
    if (this->rawSourceHistoryCount_ < 3)
        return {0, 1, 2};
    return {
        this->rawSourceHistoryNext_,
        (this->rawSourceHistoryNext_ + 1) % 3,
        (this->rawSourceHistoryNext_ + 2) % 3,
    };
}
#endif

'''
    text = once(text, method_marker, methods + method_marker, f"{path}: Candidate A helper methods")

    decision_marker = "    this->lastGeneratedFrameCount_ = generatedFrameCount;\n\n"
    decision = r'''    this->lastGeneratedFrameCount_ = generatedFrameCount;

    if (adaptiveTelemetry.discontinuityReset) {
        this->invalidateDeferredZeroHistory();
        std::cerr << "lsfg-vk: deferred-zero invalidated reason=adaptive-discontinuity\n";
    }
    if (adaptiveZeroGeneration) {
        if (!this->zeroDemandActive_) {
            this->zeroDemandActive_ = true;
            this->zeroDemandStart_ = cycleStart;
            this->consecutiveZeroDecisions_ = 1;
        } else {
            ++this->consecutiveZeroDecisions_;
        }
        const uint64_t zeroDemandMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                cycleStart - this->zeroDemandStart_).count());
        if (this->historyMaintenanceState_ == HistoryMaintenanceState::LiveHistory
                && this->deferredZeroRawHistoryAllocated_
                && !this->deferredZeroDisabledForContext_
                && this->rawSourceHistoryCount_ >= 3
                && this->consecutiveZeroDecisions_ >= kDeferredZeroDecisionThreshold
                && zeroDemandMs >= kDeferredZeroMinimumDurationMs) {
            try {
                this->flushPendingAndroidWork(true);
                this->historyMaintenanceState_ = HistoryMaintenanceState::DeferredZero;
                metrics.windowDeferredZeroEntries++;
                metrics.totalDeferredZeroEntries++;
                std::cerr << "lsfg-vk: deferred-zero state=enter streak="
                          << this->consecutiveZeroDecisions_
                          << " elapsed_ms=" << zeroDemandMs << '\n';
            } catch (const std::exception& error) {
                this->deferredZeroDisabledForContext_ = true;
                this->invalidateDeferredZeroHistory();
                metrics.totalDeferredFallbacks++;
                std::cerr << "lsfg-vk: deferred-zero entry-fallback reason="
                          << error.what() << '\n';
            }
        }
    } else {
        this->zeroDemandActive_ = false;
        this->consecutiveZeroDecisions_ = 0;
        if (this->historyMaintenanceState_ == HistoryMaintenanceState::DeferredZero) {
            this->historyMaintenanceState_ = HistoryMaintenanceState::ReprimeHistory;
        } else if (this->historyMaintenanceState_ == HistoryMaintenanceState::LiveHistory) {
            this->rawSourceHistoryNext_ = 0;
            this->rawSourceHistoryCount_ = 0;
            this->rawSourceHistoryInitialized_.fill(false);
        }
    }

'''
    text = once(text, decision_marker, decision, f"{path}: Candidate A state decision")

    metrics_marker = "                      << \" adaptive_zero_cycles_total=\" << metrics.totalAdaptiveZeroGenerationCycles\n"
    metrics_extra = metrics_marker + (
        "                      << \" deferred_zero_frames=\" << metrics.windowDeferredZeroFrames\n"
        "                      << \" deferred_zero_frames_total=\" << metrics.totalDeferredZeroFrames\n"
        "                      << \" deferred_zero_entries=\" << metrics.windowDeferredZeroEntries\n"
        "                      << \" deferred_zero_entries_total=\" << metrics.totalDeferredZeroEntries\n"
        "                      << \" deferred_zero_reprime=\" << metrics.windowDeferredReprimes\n"
        "                      << \" deferred_zero_reprime_total=\" << metrics.totalDeferredReprimes\n"
        "                      << \" deferred_zero_fallbacks_total=\" << metrics.totalDeferredFallbacks\n"
    )
    text = once(text, metrics_marker, metrics_extra, f"{path}: Candidate A metric output")
    text = once(
        text,
        "                      << \" source_history_valid=\" << (this->requiresSourceHistoryWarmup_ ? 0 : 1)\n",
        "                      << \" source_history_valid=\"\n"
        "                      << ((this->historyMaintenanceState_ == HistoryMaintenanceState::LiveHistory\n"
        "                           && !this->requiresSourceHistoryWarmup_) ? 1 : 0)\n",
        f"{path}: honest deferred history validity",
    )
    # Production now resets the timing window both after normal reporting and
    # when Android resumes an in-progress present after a long suspend. Candidate
    # A owns window-scoped deferred-zero counters, so extend both reset sites.
    text = replace_exact(
        text,
        "            metrics.windowAdaptiveZeroGenerationCycles = 0;\n",
        "            metrics.windowAdaptiveZeroGenerationCycles = 0;\n"
        "            metrics.windowDeferredZeroFrames = 0;\n"
        "            metrics.windowDeferredZeroEntries = 0;\n"
        "            metrics.windowDeferredReprimes = 0;\n",
        count=2,
        label=f"{path}: Candidate A metric reset",
    )

    early_marker = "    // 1. Copy every active Adaptive source frame into frame_0/frame_1, even on\n"
    early = r'''    if (this->historyMaintenanceState_ == HistoryMaintenanceState::DeferredZero
            && adaptiveZeroGeneration) {
        // deferred-zero source-only no-framegen
        pass.preCopySemaphores.at(0) = Mini::Semaphore(info.device);
        pass.preCopySemaphores.at(1) = Mini::Semaphore(info.device);
        pass.preCopyBuf = Mini::CommandBuffer(info.device, this->cmdPool);
        pass.preCopyBuf.begin();
        const size_t rawSlot = this->rawSourceHistoryNext_;
        copySwapchainToRawHistory(pass.preCopyBuf.handle(),
            this->swapchainImages.at(presentIdx), this->rawSourceHistory_.at(rawSlot).handle(),
            this->extent.width, this->extent.height,
            !this->rawSourceHistoryInitialized_.at(rawSlot));
        pass.preCopyBuf.end();
        std::vector<VkSemaphore> deferredWaits = gameRenderSemaphores;
        if (this->previousSourceCopySignalValid_)
            deferredWaits.emplace_back(this->passInfos.at((this->frameIdx - 1) % 8)
                .preCopySemaphores.at(1).handle());
        pass.preCopyBuf.submit(info.queue.second, deferredWaits,
            { pass.preCopySemaphores.at(0).handle(), pass.preCopySemaphores.at(1).handle() });
        this->previousSourceCopySignalValid_ = true;
        this->rawSourceHistoryInitialized_.at(rawSlot) = true;
        this->rawSourceHistoryNext_ = (rawSlot + 1) % 3;
        this->rawSourceHistoryCount_ = std::min<size_t>(3, this->rawSourceHistoryCount_ + 1);
        metrics.windowAdaptiveZeroGenerationCycles++;
        metrics.totalAdaptiveZeroGenerationCycles++;
        metrics.windowDeferredZeroFrames++;
        metrics.totalDeferredZeroFrames++;
        this->lastGeneratedFrameCount_ = 0;
        std::cerr << "lsfg-vk: deferred-zero source-only raw_slot=" << rawSlot << '\n';
        const VkSemaphore sourceReady = pass.preCopySemaphores.at(0).handle();
        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = pNext,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &sourceReady,
            .swapchainCount = 1,
            .pSwapchains = &this->swapchain,
            .pImageIndices = &presentIdx,
        };
        const auto result = Layer::ovkQueuePresentKHR(queue, &presentInfo);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            metrics.windowSourcePresentFailures++;
            metrics.totalSourcePresentFailures++;
            throw LSFG::vulkan_error(result, "Failed to present DeferredZero source frame");
        }
        return finishSourcePresent(result, "raw-history-deferred-zero");
    }

    const auto presentDeferredZeroSourceOnly = [&](VkResult returnedResult, const char* waitLabel) {
        const VkSemaphore sourceReady = pass.preCopySemaphores.at(0).handle();
        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = pNext,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &sourceReady,
            .swapchainCount = 1,
            .pSwapchains = &this->swapchain,
            .pImageIndices = &presentIdx,
        };
        const auto result = Layer::ovkQueuePresentKHR(queue, &presentInfo);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            metrics.windowSourcePresentFailures++;
            metrics.totalSourcePresentFailures++;
            throw LSFG::vulkan_error(result, "Failed to present re-prime source frame");
        }
        return finishSourcePresent(returnedResult, waitLabel);
    };

    // deferred-zero reprime-begin
    if (this->historyMaintenanceState_ == HistoryMaintenanceState::ReprimeHistory
            && generatedFrameCount > 0) {
        std::cerr << "lsfg-vk: deferred-zero reprime-begin history_count="
                  << this->rawSourceHistoryCount_ << '\n';
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
            std::vector<int> noOutSems;
            for (size_t replayIndex = 0; replayIndex < 3; ++replayIndex) {
                const size_t replaySlot = orderedRawHistorySlots.at(replayIndex);
                const size_t historySlot = static_cast<size_t>(this->framegenHistoryEpoch_ % 2);
                if (this->pendingHistoryCompletionValid_.at(historySlot))
                    this->waitPendingHistoryCompletionFd(historySlot, true);
                Mini::CommandBuffer replayBuffer(info.device, this->cmdPool);
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
                this->framegenHistoryEpoch_++;
                std::cerr << "lsfg-vk: deferred-zero reprime replay=" << replayIndex
                          << " raw_slot=" << replaySlot
                          << " framegen_slot=" << historySlot << '\n';
            }
            const bool framegenReady = conf.performance
                ? LSFG_3_1P::waitContext(*this->lsfgCtxId, runtimeWaitTimeoutNs())
                : LSFG_3_1::waitContext(*this->lsfgCtxId, runtimeWaitTimeoutNs());
            if (!framegenReady)
                throw LSFG::vulkan_error(VK_TIMEOUT, "DeferredZero re-prime preprocessing timed out");
        } catch (const std::exception& error) {
            reprimeSucceeded = false;
            this->deferredZeroDisabledForContext_ = true;
            metrics.totalDeferredFallbacks++;
            this->historyMaintenanceState_ = HistoryMaintenanceState::LiveHistory;
            this->requiresSourceHistoryWarmup_ = true;
            std::cerr << "lsfg-vk: deferred-zero reprime-fallback reason="
                      << error.what() << '\n';
        }

        this->lastGeneratedFrameCount_ = 0;
        if (!reprimeSucceeded)
            return presentDeferredZeroSourceOnly(VK_ERROR_OUT_OF_DATE_KHR,
                "deferred-reprime-fallback");

        this->historyMaintenanceState_ = HistoryMaintenanceState::LiveHistory;
        this->requiresSourceHistoryWarmup_ = false;
        this->rawSourceHistoryNext_ = 0;
        this->rawSourceHistoryCount_ = 0;
        this->rawSourceHistoryInitialized_.fill(false);
        metrics.windowDeferredReprimes++;
        metrics.totalDeferredReprimes++;
        std::cerr << "lsfg-vk: deferred-zero reprime-complete source_only=1\n";
        return presentDeferredZeroSourceOnly(VK_SUCCESS, "deferred-reprime-source-only");
    }

'''
    text = once(text, early_marker, early + early_marker, f"{path}: DeferredZero and re-prime branches")

    # Framegen input parity follows actual framegen submissions, not outer source presents.
    text = text.replace(
        "this->frameIdx % 2 == 0 ? this->frame_0.handle() : this->frame_1.handle()",
        "this->framegenHistoryEpoch_ % 2 == 0 ? this->frame_0.handle() : this->frame_1.handle()",
        1,
    )
    text = text.replace("info.queue.first, this->frameIdx < 2);",
                        "info.queue.first, this->framegenHistoryEpoch_ < 2);", 1)
    text = text.replace(
        "const size_t historySlot = static_cast<size_t>(this->frameIdx % 2);",
        "const size_t historySlot = static_cast<size_t>(this->framegenHistoryEpoch_ % 2);",
        1,
    )

    # Capture retained raw history while the normal source-copy command
    # buffer is still recording. Candidate A also creates a re-prime
    # preCopyBuf, so include the main-path history-slot declaration to
    # keep this insertion unique after the earlier Candidate A edits.
    raw_capture_marker = (
        "    pass.preCopyBuf.end();\n\n"
        "    const size_t historySlot = static_cast<size_t>(this->framegenHistoryEpoch_ % 2);\n"
    )
    raw_capture = r'''    const bool captureRawHistory = adaptiveZeroGeneration
        && this->historyMaintenanceState_ == HistoryMaintenanceState::LiveHistory
        && this->deferredZeroRawHistoryAllocated_
        && !this->deferredZeroDisabledForContext_;
    if (captureRawHistory) {
        const size_t rawSlot = this->rawSourceHistoryNext_;
        copySwapchainToRawHistory(pass.preCopyBuf.handle(),
            this->swapchainImages.at(presentIdx), this->rawSourceHistory_.at(rawSlot).handle(),
            this->extent.width, this->extent.height,
            !this->rawSourceHistoryInitialized_.at(rawSlot));
        this->rawSourceHistoryInitialized_.at(rawSlot) = true;
        this->rawSourceHistoryNext_ = (rawSlot + 1) % 3;
        this->rawSourceHistoryCount_ = std::min<size_t>(3, this->rawSourceHistoryCount_ + 1);
    }

'''
    text = once(text, raw_capture_marker, raw_capture + raw_capture_marker,
                f"{path}: retain short-zero raw history")

    text = once(
        text,
        "        metrics.windowAdaptiveZeroGenerationCycles++;\n",
        "        this->framegenHistoryEpoch_++;\n"
        "        metrics.windowAdaptiveZeroGenerationCycles++;\n",
        f"{path}: advance framegen history epoch on maintained zero",
    )
    generated_return_marker = (
        "    if (firstPresentDiagnostic)\n"
        "        std::cerr << \"lsfg-vk: runtime stage=framegen-dispatch-returned\\n\";\n"
    )
    text = once(
        text,
        generated_return_marker,
        "    this->framegenHistoryEpoch_++;\n" + generated_return_marker,
        f"{path}: advance framegen history epoch on generated cycle",
    )

    bypass_old = """void LsContext::enterSourceOnlyBypass() {
    this->flushPendingAndroidWork(true);
    this->lastGeneratedFrameCount_ = 0;
"""
    bypass_new = """void LsContext::enterSourceOnlyBypass() {
    this->flushPendingAndroidWork(true);
    this->invalidateDeferredZeroHistory();
    this->lastGeneratedFrameCount_ = 0;
"""
    text = once(text, bypass_old, bypass_new, f"{path}: source-only invalidates deferred history")
    path.write_text(text, encoding="utf-8")


def apply(root: Path) -> None:
    patch_mini_image_header(root / "include/mini/image.hpp")
    patch_mini_image_source(root / "src/mini/image.cpp")
    patch_outer_header(root / "include/context.hpp")
    patch_outer_source(root / "src/context.cpp")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    apply(args.root.resolve())


if __name__ == "__main__":
    main()

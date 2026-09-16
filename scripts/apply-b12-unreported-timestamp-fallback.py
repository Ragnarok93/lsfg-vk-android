#!/usr/bin/env python3
"""Enable a fail-closed timestamp-query probe for B12 evidence builds only.

Some Android Vulkan wrappers report timestampValidBits=0 even when the selected
underlying ICD implements timestamp queries.  The clean runtime must continue to
honor that advertised capability.  B12 evidence builds may instead perform one
small startup self-test on the already-selected compute queue.  The fallback is
kept only when query-pool creation, command recording/submission, fence wait,
and timestamp readback all succeed with an advancing counter.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from adreno_evidence_common import replace_exact

HEADER = Path("framegen/include/core/timestampquerypool.hpp")
SOURCE = Path("framegen/src/core/timestampquerypool.cpp")
CONTEXT_SOURCES = (
    Path("framegen/v3.1_src/context.cpp"),
    Path("framegen/v3.1p_src/context.cpp"),
)


HELPERS = r'''namespace {

uint32_t b12QueueFamilyBits2(VkPhysicalDevice physicalDevice, uint32_t familyIndex) {
    if (vkGetPhysicalDeviceQueueFamilyProperties2 == nullptr)
        return 0;

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties2(physicalDevice, &count, nullptr);
    if (familyIndex >= count)
        return 0;

    std::vector<VkQueueFamilyProperties2> properties(count);
    for (auto& property : properties)
        property.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
    vkGetPhysicalDeviceQueueFamilyProperties2(physicalDevice, &count, properties.data());
    return familyIndex < count
        ? properties.at(familyIndex).queueFamilyProperties.timestampValidBits
        : 0;
}

std::pair<uint32_t, int32_t> b12BestComputeTimestampFamily(
        const std::vector<VkQueueFamilyProperties>& properties) {
    uint32_t bestBits = 0;
    int32_t bestFamily = -1;
    for (uint32_t i = 0; i < properties.size(); ++i) {
        if ((properties.at(i).queueFlags & VK_QUEUE_COMPUTE_BIT) == 0)
            continue;
        if (properties.at(i).timestampValidBits > bestBits) {
            bestBits = properties.at(i).timestampValidBits;
            bestFamily = static_cast<int32_t>(i);
        }
    }
    return {bestBits, bestFamily};
}

bool b12RunUnreportedTimestampProbe(
        const Core::Device& device, VkQueryPool queryPool,
        uint64_t* firstTimestamp, uint64_t* secondTimestamp) {
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    const VkCommandPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = device.getComputeFamilyIdx(),
    };
    if (vkCreateCommandPool(device.handle(), &poolInfo, nullptr, &commandPool) != VK_SUCCESS)
        return false;

    const auto cleanup = [&]() {
        if (fence != VK_NULL_HANDLE)
            vkDestroyFence(device.handle(), fence, nullptr);
        vkDestroyCommandPool(device.handle(), commandPool, nullptr);
    };

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    const VkCommandBufferAllocateInfo allocateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(device.handle(), &allocateInfo, &commandBuffer) != VK_SUCCESS) {
        cleanup();
        return false;
    }

    const VkFenceCreateInfo fenceInfo{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    if (vkCreateFence(device.handle(), &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
        cleanup();
        return false;
    }

    const VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        cleanup();
        return false;
    }

    vkCmdResetQueryPool(commandBuffer, queryPool, 0, 2);
    vkCmdWriteTimestamp(
        commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);
    for (uint32_t i = 0; i < 32; ++i) {
        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 0, nullptr);
    }
    vkCmdWriteTimestamp(
        commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);
    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        cleanup();
        return false;
    }

    const VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffer,
    };
    if (vkQueueSubmit(device.getComputeQueue(), 1, &submitInfo, fence) != VK_SUCCESS) {
        cleanup();
        return false;
    }
    constexpr uint64_t timeoutNs = 1000000000ULL;
    if (vkWaitForFences(device.handle(), 1, &fence, VK_TRUE, timeoutNs) != VK_SUCCESS) {
        cleanup();
        return false;
    }

    std::array<uint64_t, 2> timestamps{};
    const VkResult result = vkGetQueryPoolResults(
        device.handle(), queryPool, 0, 2,
        sizeof(timestamps), timestamps.data(), sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT);
    cleanup();
    if (result != VK_SUCCESS || timestamps.at(0) == timestamps.at(1))
        return false;

    if (firstTimestamp != nullptr)
        *firstTimestamp = timestamps.at(0);
    if (secondTimestamp != nullptr)
        *secondTimestamp = timestamps.at(1);
    return true;
}

bool b12UnreportedTimestampSupported(
        const Core::Device& device, VkQueryPool queryPool,
        uint64_t* firstTimestamp, uint64_t* secondTimestamp) {
    static std::mutex cacheMutex;
    static std::unordered_map<uint64_t, bool> cache;
    const auto physicalKey = static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(device.getPhysicalDevice()));
    const uint64_t key = physicalKey
        ^ (static_cast<uint64_t>(device.getComputeFamilyIdx()) << 48U);

    std::lock_guard<std::mutex> guard(cacheMutex);
    if (const auto found = cache.find(key); found != cache.end())
        return found->second;

    const bool supported = b12RunUnreportedTimestampProbe(
        device, queryPool, firstTimestamp, secondTimestamp);
    cache.emplace(key, supported);
    return supported;
}

} // namespace

'''


def patch_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "allowUnreportedTimestamps" in text:
        return
    text = replace_exact(
        text,
        "        TimestampQueryPool(const Core::Device& device, uint32_t queryCount);\n",
        "        TimestampQueryPool(const Core::Device& device, uint32_t queryCount,\n"
        "            bool allowUnreportedTimestamps = false);\n",
        count=1,
        label=f"{path}: B12 evidence-only constructor option",
    )
    path.write_text(text, encoding="utf-8")


def patch_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "b12-timestamp-fallback" in text:
        return

    text = replace_exact(
        text,
        "#include <algorithm>\n#include <cstdint>\n#include <limits>\n#include <memory>\n#include <vector>\n",
        "#include <algorithm>\n#include <array>\n#include <atomic>\n#include <cstdint>\n"
        "#include <iostream>\n#include <limits>\n#include <memory>\n#include <mutex>\n"
        "#include <unordered_map>\n#include <utility>\n#include <vector>\n",
        count=1,
        label=f"{path}: B12 timestamp probe includes",
    )
    text = replace_exact(
        text,
        "using namespace LSFG::Core;\n\n",
        HELPERS + "using namespace LSFG::Core;\n\n",
        count=1,
        label=f"{path}: B12 timestamp probe helpers",
    )

    old_constructor = '''TimestampQueryPool::TimestampQueryPool(const Core::Device& device, uint32_t queryCount)
    : queryCount_(queryCount) {
    if (queryCount < 2)
        return;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        device.getPhysicalDevice(), &queueFamilyCount, nullptr);
    if (queueFamilyCount == 0 || device.getComputeFamilyIdx() >= queueFamilyCount)
        return;

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(
        device.getPhysicalDevice(), &queueFamilyCount, queueFamilies.data());

    this->timestampValidBits_ =
        queueFamilies.at(device.getComputeFamilyIdx()).timestampValidBits;
    if (this->timestampValidBits_ == 0)
        return;

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.getPhysicalDevice(), &properties);
    this->timestampPeriodNs_ = properties.limits.timestampPeriod;
    if (this->timestampPeriodNs_ <= 0.0f)
        return;

    const VkQueryPoolCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = queryCount,
    };

    VkQueryPool queryPoolHandle = VK_NULL_HANDLE;
    const VkResult result = vkCreateQueryPool(
        device.handle(), &createInfo, nullptr, &queryPoolHandle);
    if (result != VK_SUCCESS || queryPoolHandle == VK_NULL_HANDLE)
        return;

    this->queryPool = std::shared_ptr<VkQueryPool>(
        new VkQueryPool(queryPoolHandle),
        [dev = device.handle()](VkQueryPool* handle) {
            vkDestroyQueryPool(dev, *handle, nullptr);
            delete handle;
        });
}
'''
    new_constructor = '''TimestampQueryPool::TimestampQueryPool(
        const Core::Device& device, uint32_t queryCount,
        bool allowUnreportedTimestamps)
    : queryCount_(queryCount) {
    if (queryCount < 2)
        return;

    const VkPhysicalDevice physicalDevice = device.getPhysicalDevice();
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        physicalDevice, &queueFamilyCount, nullptr);
    if (queueFamilyCount == 0 || device.getComputeFamilyIdx() >= queueFamilyCount)
        return;

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(
        physicalDevice, &queueFamilyCount, queueFamilies.data());

    const uint32_t familyIndex = device.getComputeFamilyIdx();
    const uint32_t legacyBits = queueFamilies.at(familyIndex).timestampValidBits;
    const uint32_t properties2Bits = b12QueueFamilyBits2(physicalDevice, familyIndex);
    const uint32_t reportedBits = std::max(legacyBits, properties2Bits);
    const auto [bestComputeBits, bestComputeFamily] =
        b12BestComputeTimestampFamily(queueFamilies);

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    this->timestampPeriodNs_ = properties.limits.timestampPeriod;

    static std::atomic<bool> capabilityLogged{false};
    if (!capabilityLogged.exchange(true)) {
        std::cerr << "lsfg-vk: b12-timestamp-capability selected_family=" << familyIndex
                  << " queue_flags=0x" << std::hex
                  << queueFamilies.at(familyIndex).queueFlags << std::dec
                  << " legacy_bits=" << legacyBits
                  << " properties2_bits=" << properties2Bits
                  << " best_compute_bits=" << bestComputeBits
                  << " best_compute_family=" << bestComputeFamily
                  << " timestamp_period_ns=" << this->timestampPeriodNs_
                  << " allow_unreported=" << (allowUnreportedTimestamps ? 1 : 0)
                  << '\n';
    }

    if (this->timestampPeriodNs_ <= 0.0f)
        return;
    if (reportedBits == 0 && !allowUnreportedTimestamps)
        return;

    const VkQueryPoolCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = queryCount,
    };

    VkQueryPool queryPoolHandle = VK_NULL_HANDLE;
    const VkResult result = vkCreateQueryPool(
        device.handle(), &createInfo, nullptr, &queryPoolHandle);
    if (result != VK_SUCCESS || queryPoolHandle == VK_NULL_HANDLE)
        return;

    if (reportedBits == 0) {
        uint64_t firstTimestamp = 0;
        uint64_t secondTimestamp = 0;
        const bool probeSupported = b12UnreportedTimestampSupported(
            device, queryPoolHandle, &firstTimestamp, &secondTimestamp);
        static std::atomic<bool> fallbackLogged{false};
        if (!fallbackLogged.exchange(true)) {
            std::cerr << "lsfg-vk: b12-timestamp-fallback mode=unreported-probe result="
                      << (probeSupported ? "enabled" : "disabled")
                      << " first=" << firstTimestamp
                      << " second=" << secondTimestamp << '\n';
        }
        if (!probeSupported) {
            vkDestroyQueryPool(device.handle(), queryPoolHandle, nullptr);
            this->timestampPeriodNs_ = 0.0f;
            return;
        }
        // The wrapper withheld a valid-bit width, but the one-time query probe
        // demonstrated an advancing counter.  B12 intervals are short enough
        // that treating the zero-extended value as 64-bit is safe for evidence;
        // the production path never uses this override.
        this->timestampValidBits_ = 64;
    } else {
        this->timestampValidBits_ = std::min<uint32_t>(reportedBits, 64);
    }

    this->queryPool = std::shared_ptr<VkQueryPool>(
        new VkQueryPool(queryPoolHandle),
        [dev = device.handle()](VkQueryPool* handle) {
            vkDestroyQueryPool(dev, *handle, nullptr);
            delete handle;
        });
}
'''
    text = replace_exact(
        text,
        old_constructor,
        new_constructor,
        count=1,
        label=f"{path}: B12 unreported timestamp constructor",
    )
    path.write_text(text, encoding="utf-8")


def patch_context(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    old = "Core::TimestampQueryPool(vk.device, 2);"
    new = "Core::TimestampQueryPool(vk.device, 2, true);"
    if new in text and old not in text:
        return
    count = text.count(old)
    if count == 0:
        raise RuntimeError(f"{path}: no B12 timestamp-pool constructors found")
    text = text.replace(old, new)
    path.write_text(text, encoding="utf-8")


def apply(root: Path) -> None:
    patch_header(root / HEADER)
    patch_source(root / SOURCE)
    for rel in CONTEXT_SOURCES:
        patch_context(root / rel)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    apply(args.root.resolve())


if __name__ == "__main__":
    main()

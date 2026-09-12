#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/timestampquerypool.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

using namespace LSFG::Core;

TimestampQueryPool::TimestampQueryPool(const Core::Device& device, uint32_t queryCount)
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

void TimestampQueryPool::reset(VkCommandBuffer commandBuffer) const {
    if (!this->supported())
        return;
    vkCmdResetQueryPool(
        commandBuffer, *this->queryPool, 0, this->queryCount_);
}

void TimestampQueryPool::write(
        VkCommandBuffer commandBuffer, uint32_t queryIndex) const {
    if (!this->supported() || queryIndex >= this->queryCount_)
        return;
    vkCmdWriteTimestamp(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        *this->queryPool,
        queryIndex);
}

std::vector<double> TimestampQueryPool::durationsMs(
        const Core::Device& device) const {
    if (!this->supported())
        return {};

    std::vector<uint64_t> timestamps(this->queryCount_);
    const VkResult result = vkGetQueryPoolResults(
        device.handle(),
        *this->queryPool,
        0,
        this->queryCount_,
        timestamps.size() * sizeof(uint64_t),
        timestamps.data(),
        sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS)
        return {};

    const uint32_t validBits = std::min<uint32_t>(this->timestampValidBits_, 64);
    const uint64_t mask = validBits == 64
        ? std::numeric_limits<uint64_t>::max()
        : ((uint64_t{1} << validBits) - 1);

    std::vector<double> durations;
    durations.reserve(this->queryCount_ - 1);
    for (uint32_t i = 0; i + 1 < this->queryCount_; ++i) {
        const uint64_t start = timestamps.at(i) & mask;
        const uint64_t end = timestamps.at(i + 1) & mask;
        const uint64_t delta = (end - start) & mask;
        durations.emplace_back(
            (static_cast<double>(delta) * this->timestampPeriodNs_) / 1000000.0);
    }

    return durations;
}

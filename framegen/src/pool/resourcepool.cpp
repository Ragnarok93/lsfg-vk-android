#include "pool/resourcepool.hpp"
#include "core/buffer.hpp"
#include "core/device.hpp"
#include "core/sampler.hpp"

#include <vulkan/vulkan_core.h>

#include <array>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

using namespace LSFG;
using namespace LSFG::Pool;

struct ConstantBuffer {
    std::array<uint32_t, 2> inputOffset;
    uint32_t firstIter;
    uint32_t firstIterS;
    uint32_t advancedColorKind;
    uint32_t hdrSupport;
    float resolutionInvScale;
    float timestamp;
    float uiThreshold;
    std::array<uint32_t, 3> pad;
};

Core::Buffer ResourcePool::getBuffer(
            const Core::Device& device,
            float timestamp, bool firstIter, bool firstIterS) {
    uint64_t hash = 0;
    const union { float f; uint32_t i; } u{
        .f = timestamp };
    hash |= u.i;
    hash |= static_cast<uint64_t>(firstIter) << 32;
    hash |= static_cast<uint64_t>(firstIterS) << 33;

    auto it = buffers.find(hash);
    if (it != buffers.end())
        return it->second;

    // create the buffer
    const ConstantBuffer data{
        .inputOffset = { 0, 0 },
        .firstIter = firstIter ? 1U : 0U,
        .firstIterS = firstIterS ? 1U : 0U,
        .advancedColorKind = this->isHdr ? 2U : 0U,
        .hdrSupport = this->isHdr,
        .resolutionInvScale = this->flowScale,
        .timestamp = timestamp,
        .uiThreshold = 0.5F,
    };
    Core::Buffer buffer(device, data, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    buffers[hash] = buffer;
    return buffer;
}

Core::Buffer ResourcePool::getTimestampBuffer(
        const Core::Device& device, bool dynamicInterpolationPhases,
        float timestamp, bool firstIter, bool firstIterS) {
    if (!dynamicInterpolationPhases)
        return this->getBuffer(device, timestamp, firstIter, firstIterS);
    return this->createTimestampRing(device, timestamp, firstIter, firstIterS);
}

Core::Buffer ResourcePool::createTimestampRing(
        const Core::Device& device,
        float timestamp, bool firstIter, bool firstIterS) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.getPhysicalDevice(), &properties);
    const size_t alignment = std::max<size_t>(
        1, static_cast<size_t>(properties.limits.minUniformBufferOffsetAlignment));
    const size_t recordSize = sizeof(ConstantBuffer);
    const size_t stride =
        ((recordSize + alignment - 1) / alignment) * alignment;
    if (stride == 0
            || stride > static_cast<size_t>(std::numeric_limits<uint32_t>::max())
                / kTimestampRingSlots)
        throw std::overflow_error("Timestamp ring dynamic offset exceeds Vulkan range");

    const ConstantBuffer record{
        .inputOffset = { 0, 0 },
        .firstIter = firstIter ? 1U : 0U,
        .firstIterS = firstIterS ? 1U : 0U,
        .advancedColorKind = this->isHdr ? 2U : 0U,
        .hdrSupport = this->isHdr,
        .resolutionInvScale = this->flowScale,
        .timestamp = timestamp,
        .uiThreshold = 0.5F,
    };

    std::vector<uint8_t> storage(stride * kTimestampRingSlots, 0);
    for (size_t slot = 0; slot < kTimestampRingSlots; ++slot)
        std::memcpy(storage.data() + slot * stride, &record, recordSize);

    return Core::Buffer(
        device, storage.data(), storage.size(),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
}

VkDeviceSize ResourcePool::timestampRecordSize() noexcept {
    return static_cast<VkDeviceSize>(sizeof(ConstantBuffer));
}

uint32_t ResourcePool::timestampRingOffset(
        const Core::Buffer& buffer, uint64_t frameIndex) {
    if (buffer.getSize() == 0
            || buffer.getSize() % kTimestampRingSlots != 0)
        throw std::logic_error("Invalid timestamp ring allocation");
    const size_t stride = buffer.getSize() / kTimestampRingSlots;
    const size_t slot = static_cast<size_t>(frameIndex % kTimestampRingSlots);
    const size_t offset = stride * slot;
    if (offset > std::numeric_limits<uint32_t>::max())
        throw std::overflow_error("Timestamp ring offset exceeds uint32_t");
    return static_cast<uint32_t>(offset);
}

void ResourcePool::writeTimestamp(
        Core::Buffer& buffer, float timestamp, size_t baseOffset) {
    if (!std::isfinite(timestamp) || timestamp <= 0.0F || timestamp >= 1.0F)
        throw std::invalid_argument("Interpolation timestamp must be within (0, 1)");
    buffer.write(
        &timestamp,
        sizeof(timestamp),
        baseOffset + offsetof(ConstantBuffer, timestamp));
}


Core::Sampler ResourcePool::getSampler(
            const Core::Device& device,
            VkSamplerAddressMode type,
            VkCompareOp compare,
            bool isWhite) {
    uint64_t hash = 0;
    hash |= static_cast<uint64_t>(type) << 0;
    hash |= static_cast<uint64_t>(compare) << 8;
    hash |= static_cast<uint64_t>(isWhite) << 16;

    auto it = samplers.find(hash);
    if (it != samplers.end())
        return it->second;

    // create the sampler
    Core::Sampler sampler(device, type, compare, isWhite);
    samplers[hash] = sampler;
    return sampler;
}

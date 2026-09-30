#pragma once

#include <atomic>
#include <cstdint>

namespace LSFG::Core {

struct ResourceConstructionStats {
    uint64_t images{0};
    uint64_t imageBytes{0};
    uint64_t buffers{0};
    uint64_t bufferBytes{0};
    uint64_t descriptorSets{0};
    uint64_t samplers{0};
    uint64_t pipelines{0};
    uint64_t shaderModules{0};
};

namespace ResourceStatsDetail {
inline std::atomic<uint64_t> images{0};
inline std::atomic<uint64_t> imageBytes{0};
inline std::atomic<uint64_t> buffers{0};
inline std::atomic<uint64_t> bufferBytes{0};
inline std::atomic<uint64_t> descriptorSets{0};
inline std::atomic<uint64_t> samplers{0};
inline std::atomic<uint64_t> pipelines{0};
inline std::atomic<uint64_t> shaderModules{0};
}

inline ResourceConstructionStats snapshotResourceConstructionStats() {
    using namespace ResourceStatsDetail;
    return {
        .images = images.load(std::memory_order_relaxed),
        .imageBytes = imageBytes.load(std::memory_order_relaxed),
        .buffers = buffers.load(std::memory_order_relaxed),
        .bufferBytes = bufferBytes.load(std::memory_order_relaxed),
        .descriptorSets = descriptorSets.load(std::memory_order_relaxed),
        .samplers = samplers.load(std::memory_order_relaxed),
        .pipelines = pipelines.load(std::memory_order_relaxed),
        .shaderModules = shaderModules.load(std::memory_order_relaxed),
    };
}

inline void recordImageConstruction(uint64_t bytes) {
    ResourceStatsDetail::images.fetch_add(1, std::memory_order_relaxed);
    ResourceStatsDetail::imageBytes.fetch_add(bytes, std::memory_order_relaxed);
}
inline void recordBufferConstruction(uint64_t bytes) {
    ResourceStatsDetail::buffers.fetch_add(1, std::memory_order_relaxed);
    ResourceStatsDetail::bufferBytes.fetch_add(bytes, std::memory_order_relaxed);
}
inline void recordDescriptorSetConstruction() {
    ResourceStatsDetail::descriptorSets.fetch_add(1, std::memory_order_relaxed);
}
inline void recordSamplerConstruction() {
    ResourceStatsDetail::samplers.fetch_add(1, std::memory_order_relaxed);
}
inline void recordPipelineConstruction() {
    ResourceStatsDetail::pipelines.fetch_add(1, std::memory_order_relaxed);
}
inline void recordShaderModuleConstruction() {
    ResourceStatsDetail::shaderModules.fetch_add(1, std::memory_order_relaxed);
}

inline ResourceConstructionStats operator-(
        const ResourceConstructionStats& after,
        const ResourceConstructionStats& before) {
    return {
        .images = after.images - before.images,
        .imageBytes = after.imageBytes - before.imageBytes,
        .buffers = after.buffers - before.buffers,
        .bufferBytes = after.bufferBytes - before.bufferBytes,
        .descriptorSets = after.descriptorSets - before.descriptorSets,
        .samplers = after.samplers - before.samplers,
        .pipelines = after.pipelines - before.pipelines,
        .shaderModules = after.shaderModules - before.shaderModules,
    };
}

} // namespace LSFG::Core

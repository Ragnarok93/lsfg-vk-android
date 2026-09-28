#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "lsfg_3_1.hpp"
#include "v3_1/context.hpp"
#include "core/commandpool.hpp"
#include "core/descriptorpool.hpp"
#include "core/instance.hpp"
#include "pool/shaderpool.hpp"
#include "common/exception.hpp"
#include "common/utils.hpp"

#include <cstdint>
#include <optional>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace LSFG;
using namespace LSFG_3_1;

namespace {
    struct RuntimeSignature {
        LSFG::DeviceIdentity identity;
        VkFormat sharedFormat;
        bool isHdr;
        float flowScale;
        uint64_t generationCount;

        bool operator==(const RuntimeSignature&) const = default;
    };

    std::optional<Core::Instance> instance;
    std::optional<Vulkan> device;
    std::optional<RuntimeSignature> activeSignature;
    std::unordered_map<int32_t, Context> contexts;
    std::unordered_set<int32_t> pendingContextDeletes;
    std::mutex runtimeMutex;

    void resetRuntime() {
        contexts.clear();
        pendingContextDeletes.clear();
        device.reset();
        instance.reset();
        activeSignature.reset();
    }

    void collectCompletedContextDeletes() {
        if (!device.has_value() || pendingContextDeletes.empty())
            return;

        for (auto pending = pendingContextDeletes.begin();
                pending != pendingContextDeletes.end();) {
            const auto context = contexts.find(*pending);
            if (context == contexts.end()) {
                pending = pendingContextDeletes.erase(pending);
                continue;
            }
            if (!context->second.waitForCompletion(*device)) {
                ++pending;
                continue;
            }
            contexts.erase(context);
            pending = pendingContextDeletes.erase(pending);
        }

        if (contexts.empty() && pendingContextDeletes.empty())
            resetRuntime();
    }

    void validateOutputCount(size_t outputCount) {
        if (!device.has_value())
            throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "LSFG not initialized");
        if (outputCount != device->generationCount)
            throw std::runtime_error(
                "LSFG output-count mismatch: expected=" +
                std::to_string(device->generationCount) + " actual=" +
                std::to_string(outputCount));
    }
}

void LSFG_3_1::initialize(const LSFG::DeviceIdentity& identity, VkFormat sharedFormat,
        bool isHdr, float flowScale, uint64_t generationCount,
        const std::function<std::vector<uint8_t>(const std::string&)>& loader) {
    const std::scoped_lock lock(runtimeMutex);
    const RuntimeSignature requestedSignature{
        .identity = identity,
        .sharedFormat = sharedFormat,
        .isHdr = isHdr,
        .flowScale = flowScale,
        .generationCount = generationCount,
    };

    // A previous context can outlive its wrapper when the bounded teardown
    // wait expires. Reclaim completed pending contexts before deciding whether
    // a new runtime signature may be installed.
    collectCompletedContextDeletes();

    if (instance.has_value() && device.has_value()
            && activeSignature.has_value()
            && requestedSignature == activeSignature.value())
        return;
    if (!contexts.empty())
        throw std::runtime_error(
            "Cannot reconfigure LSFG runtime while contexts are active");
    resetRuntime();

    try {
        instance.emplace();
        device.emplace(Vulkan {
            .device{*instance, identity, sharedFormat},
            .generationCount = generationCount,
            .flowScale = flowScale,
            .isHdr = isHdr
        });
        activeSignature = requestedSignature;
        contexts = std::unordered_map<int32_t, Context>();

        device->commandPool = Core::CommandPool(device->device);
        device->descriptorPool = Core::DescriptorPool(device->device);

        device->resources = Pool::ResourcePool(device->isHdr, device->flowScale);
        device->shaders = Pool::ShaderPool(loader);
    } catch (...) {
        resetRuntime();
        throw;
    }

    std::srand(static_cast<uint32_t>(std::time(nullptr)));
}

LSFG::BackendDiagnostics LSFG_3_1::getBackendDiagnostics() {
    const std::scoped_lock lock(runtimeMutex);
    if (!device.has_value())
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "LSFG not initialized");
    return device->device.getDiagnostics();
}

int32_t LSFG_3_1::createContext(
        int in0, int in1, const std::vector<int>& outN,
        VkExtent2D extent, VkFormat format) {
    const std::scoped_lock lock(runtimeMutex);
    if (!instance.has_value() || !device.has_value())
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "LSFG not initialized");
    validateOutputCount(outN.size());

    const int32_t id = std::rand();
    contexts.emplace(id, Context(*device, in0, in1, outN, extent, format));
    return id;
}

void LSFG_3_1::presentContext(int32_t id, int inSem, const std::vector<int>& outSem) {
    const std::scoped_lock lock(runtimeMutex);
    if (!instance.has_value() || !device.has_value())
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "LSFG not initialized");
    auto it = contexts.find(id);
    if (it == contexts.end())
        throw LSFG::vulkan_error(VK_ERROR_UNKNOWN, "Context not found");
    it->second.present(*device, inSem, outSem, device->generationCount);
}

void LSFG_3_1::presentContextWithCount(int32_t id, int inSem,
        const std::vector<int>& outSem, size_t activeGenerationCount,
        VkExternalSemaphoreHandleTypeFlagBits inSemHandleType,
        size_t interpolationGenerationCount,
        const LSFG::AdaptiveFlowBatchMetadata& adaptiveFlowBatch) {
    const std::scoped_lock lock(runtimeMutex);
    if (!instance.has_value() || !device.has_value())
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "LSFG not initialized");
    if (activeGenerationCount > device->generationCount)
        throw std::runtime_error("LSFG active generation count exceeds runtime capacity");
    if (interpolationGenerationCount > device->generationCount)
        throw std::runtime_error("LSFG interpolation generation count exceeds runtime capacity");
    if (interpolationGenerationCount != 0
            && interpolationGenerationCount < activeGenerationCount)
        throw std::runtime_error("LSFG interpolation generation count is below active count");

    auto it = contexts.find(id);
    if (it == contexts.end())
        throw LSFG::vulkan_error(VK_ERROR_UNKNOWN, "Context not found");

    it->second.present(
        *device, inSem, outSem, activeGenerationCount, inSemHandleType,
        false, interpolationGenerationCount, adaptiveFlowBatch);
}

#ifdef __ANDROID__
LSFG::AndroidFrameSyncFds LSFG_3_1::presentContextWithCountExportSyncFd(
        int32_t id, int inSem, size_t activeGenerationCount,
        VkExternalSemaphoreHandleTypeFlagBits inSemHandleType,
        size_t interpolationGenerationCount,
        const LSFG::AdaptiveFlowBatchMetadata& adaptiveFlowBatch) {
    const std::scoped_lock lock(runtimeMutex);
    if (!instance.has_value() || !device.has_value())
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "LSFG not initialized");
    if (activeGenerationCount > device->generationCount)
        throw std::runtime_error("LSFG active generation count exceeds runtime capacity");
    if (interpolationGenerationCount > device->generationCount)
        throw std::runtime_error("LSFG interpolation generation count exceeds runtime capacity");
    if (interpolationGenerationCount != 0
            && interpolationGenerationCount < activeGenerationCount)
        throw std::runtime_error("LSFG interpolation generation count is below active count");

    auto it = contexts.find(id);
    if (it == contexts.end())
        throw LSFG::vulkan_error(VK_ERROR_UNKNOWN, "Context not found");

    const std::vector<int> noImportedOutputs;
    return it->second.present(
        *device, inSem, noImportedOutputs, activeGenerationCount,
        inSemHandleType, true, interpolationGenerationCount, adaptiveFlowBatch);
}
#endif

void LSFG_3_1::deleteContext(int32_t id) {
    const std::scoped_lock lock(runtimeMutex);
    if (!instance.has_value() || !device.has_value())
        return;

    auto it = contexts.find(id);
    if (it == contexts.end())
        return;

    if (!it->second.waitForCompletion(*device)) {
        pendingContextDeletes.insert(id);
        std::cerr << "lsfg-vk: framegen teardown timed out; retaining context resources for retry\n";
        return;
    }
    contexts.erase(it);
    pendingContextDeletes.erase(id);
    if (contexts.empty())
        resetRuntime();
}

void LSFG_3_1::finalize() {
    const std::scoped_lock lock(runtimeMutex);
    collectCompletedContextDeletes();
    if (!instance.has_value() || !device.has_value())
        return;

    bool allCompleted = true;
    for (auto& [id, context] : contexts) {
        (void)id;
        if (!context.waitForCompletion(*device))
            allCompleted = false;
    }
    if (!allCompleted) {
        for (const auto& [id, context] : contexts) {
            (void)context;
            pendingContextDeletes.insert(id);
        }
        std::cerr << "lsfg-vk: framegen finalize timed out; retaining Vulkan resources for retry\n";
        return;
    }
    resetRuntime();
}

#ifdef __ANDROID__

#include <android/hardware_buffer.h>

int32_t LSFG_3_1::createContextFromAHB(
        AHardwareBuffer* in0, AHardwareBuffer* in1,
        const std::vector<AHardwareBuffer*>& outN,
        VkExtent2D extent, VkFormat format) {
    const std::scoped_lock lock(runtimeMutex);
    if (!instance.has_value() || !device.has_value())
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "LSFG not initialized");
    validateOutputCount(outN.size());

    const int32_t id = std::rand();
    contexts.emplace(id, Context(*device, in0, in1, outN, extent, format));
    return id;
}

int32_t LSFG_3_1::createAdaptiveContextFromAHB(
        AHardwareBuffer* in0, AHardwareBuffer* in1,
        const std::vector<AHardwareBuffer*>& outN,
        VkExtent2D extent, VkFormat format,
        const std::vector<float>& flowScales) {
    const std::scoped_lock lock(runtimeMutex);
    if (!instance.has_value() || !device.has_value())
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "LSFG not initialized");
    validateOutputCount(outN.size());

    const int32_t id = std::rand();
    contexts.emplace(id, Context(
        *device, in0, in1, outN, extent, format, flowScales));
    return id;
}

void LSFG_3_1::requestContextFlowScale(int32_t id, float flowScale) {
    const std::scoped_lock lock(runtimeMutex);
    if (!instance.has_value() || !device.has_value())
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "LSFG not initialized");
    auto it = contexts.find(id);
    if (it == contexts.end())
        throw LSFG::vulkan_error(VK_ERROR_UNKNOWN, "Context not found");
    it->second.requestFlowScale(flowScale);
}

LSFG::AdaptiveFlowContextState LSFG_3_1::getContextFlowScaleState(int32_t id) {
    const std::scoped_lock lock(runtimeMutex);
    if (!instance.has_value() || !device.has_value())
        return {};
    auto it = contexts.find(id);
    if (it == contexts.end())
        return {};
    return it->second.flowScaleState();
}

LSFG::AdaptiveFlowGpuTiming LSFG_3_1::getContextGpuTiming(int32_t id) {
    const std::scoped_lock lock(runtimeMutex);
    if (!instance.has_value() || !device.has_value())
        return {};
    auto it = contexts.find(id);
    if (it == contexts.end())
        return {};
    return it->second.gpuTiming();
}

#endif // __ANDROID__

#ifdef __ANDROID__
bool LSFG_3_1::waitContext(int32_t id, uint64_t timeoutNs) {
    const std::scoped_lock lock(runtimeMutex);
    if (!instance.has_value() || !device.has_value())
        return false;
    auto it = contexts.find(id);
    if (it == contexts.end())
        return false;
    return it->second.waitForLastPresent(*device, timeoutNs);
}
#endif

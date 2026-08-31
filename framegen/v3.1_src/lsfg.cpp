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
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace LSFG;
using namespace LSFG_3_1;

namespace {
    struct RuntimeSignature {
        uint64_t deviceUUID;
        bool isHdr;
        float flowScale;
        uint64_t generationCount;

        bool operator==(const RuntimeSignature&) const = default;
    };

    std::optional<Core::Instance> instance;
    std::optional<Vulkan> device;
    std::optional<RuntimeSignature> activeSignature;
    std::unordered_map<int32_t, Context> contexts;
    std::mutex runtimeMutex;

    void resetRuntime(bool waitForDevice = true) {
        if (waitForDevice && device.has_value())
            vkDeviceWaitIdle(device->device.handle());
        contexts.clear();
        device.reset();
        instance.reset();
        activeSignature.reset();
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

void LSFG_3_1::initialize(uint64_t deviceUUID,
        bool isHdr, float flowScale, uint64_t generationCount,
        const std::function<std::vector<uint8_t>(const std::string&)>& loader) {
    const std::scoped_lock lock(runtimeMutex);
    const RuntimeSignature requestedSignature{
        .deviceUUID = deviceUUID,
        .isHdr = isHdr,
        .flowScale = flowScale,
        .generationCount = generationCount,
    };
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
            .device{*instance, deviceUUID},
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
        const std::vector<int>& outSem, size_t activeGenerationCount) {
    const std::scoped_lock lock(runtimeMutex);
    if (!instance.has_value() || !device.has_value())
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "LSFG not initialized");
    if (activeGenerationCount > device->generationCount)
        throw std::runtime_error("LSFG active generation count exceeds runtime capacity");

    auto it = contexts.find(id);
    if (it == contexts.end())
        throw LSFG::vulkan_error(VK_ERROR_UNKNOWN, "Context not found");

    it->second.present(*device, inSem, outSem, activeGenerationCount);
}

void LSFG_3_1::deleteContext(int32_t id) {
    const std::scoped_lock lock(runtimeMutex);
    if (!instance.has_value() || !device.has_value())
        return;

    auto it = contexts.find(id);
    if (it == contexts.end())
        return;

    vkDeviceWaitIdle(device->device.handle());
    contexts.erase(it);
    if (contexts.empty())
        resetRuntime(false);
}

void LSFG_3_1::finalize() {
    const std::scoped_lock lock(runtimeMutex);
    if (!instance.has_value() || !device.has_value())
        return;
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

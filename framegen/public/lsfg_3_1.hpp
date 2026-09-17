#pragma once

#include <vulkan/vulkan_core.h>
#include "lsfg_backend.hpp"

#include <functional>
#include <cstdint>
#include <string>
#include <vector>

#ifdef __ANDROID__
struct AHardwareBuffer;
#endif

namespace LSFG_3_1 {

    __attribute__((visibility("default")))
    void initialize(const LSFG::DeviceIdentity& identity, VkFormat sharedFormat,
        bool isHdr, float flowScale, uint64_t generationCount,
        const std::function<std::vector<uint8_t>(const std::string&)>& loader);

    __attribute__((visibility("default")))
    LSFG::BackendDiagnostics getBackendDiagnostics();

    __attribute__((visibility("default")))
    int32_t createContext(
        int in0, int in1, const std::vector<int>& outN,
        VkExtent2D extent, VkFormat format);

#ifdef __ANDROID__
    __attribute__((visibility("default")))
    int32_t createContextFromAHB(
        AHardwareBuffer* in0, AHardwareBuffer* in1,
        const std::vector<AHardwareBuffer*>& outN,
        VkExtent2D extent, VkFormat format);

    /// Create an otherwise ordinary AHB context with a context-specific internal
    /// flow divisor. Adaptive Flow uses this only during swapchain/context setup;
    /// no resource creation is performed when the runtime later switches states.
    __attribute__((visibility("default")))
    int32_t createContextFromAHBAtFlowScale(
        AHardwareBuffer* in0, AHardwareBuffer* in1,
        const std::vector<AHardwareBuffer*>& outN,
        VkExtent2D extent, VkFormat format, float flowScale);
#endif

    __attribute__((visibility("default")))
    void presentContext(int32_t id, int inSem, const std::vector<int>& outSem);

    __attribute__((visibility("default")))
    void presentContextWithCount(int32_t id, int inSem,
        const std::vector<int>& outSem, size_t activeGenerationCount);

#ifdef __ANDROID__
    __attribute__((visibility("default")))
    bool waitContext(int32_t id, uint64_t timeoutNs);
#endif

    __attribute__((visibility("default")))
    void deleteContext(int32_t id);

    __attribute__((visibility("default")))
    void finalize();

}

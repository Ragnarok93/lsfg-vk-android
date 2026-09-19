#pragma once

#include "ahb_transport.hpp"

#include <vulkan/vulkan_core.h>

#include <array>
#include <cstdint>
#include <cstddef>
#include <string>

namespace LSFG {

struct DeviceIdentity {
    std::array<uint8_t, VK_UUID_SIZE> deviceUUID{};
    std::array<uint8_t, VK_UUID_SIZE> driverUUID{};

    [[nodiscard]] bool operator==(const DeviceIdentity& other) const noexcept {
        return deviceUUID == other.deviceUUID && driverUUID == other.driverUUID;
    }
};

struct AdaptiveFlowContextState {
    float requestedScale{0.0f};
    float activeScale{0.0f};
    uint32_t warmupRemaining{0};
    bool transitionPending{false};
};

struct AdaptiveFlowGpuTiming {
    double mipmapsMs{0.0};
    double opticalFlowMs{0.0};
    double totalLsfgMs{0.0};
    size_t generationCount{0};
    bool transitionActive{false};
    bool valid{false};
};

struct BackendDiagnostics {
    uint32_t apiVersion{VK_API_VERSION_1_0};
    uint32_t driverVersion{0};
    std::string driverName;
    std::string driverInfo;
    DeviceIdentity identity{};
    bool ahbR16fStorage{false};
    bool ahbR16fTransferSrc{false};
    bool ahbR16fTransferDst{false};
    bool ahbR8Storage{false};
    bool ahbSampledInput{false};
    bool ahbStorageOutput{false};
    bool ahbTransferInput{false};
    bool ahbTransferOutput{false};
    AhbTransportMode ahbTransportMode{AhbTransportMode::Unsupported};
    // True only when the selected physical device advertises an OPAQUE_FD
    // external semaphore payload that is both exportable and importable. Android
    // uses this solely as an optional GPU-to-GPU AHB handoff optimization; the
    // established host-fence path remains the fallback when it is unavailable.
    bool externalSemaphoreOpaqueFd{false};
    bool externalSemaphoreSyncFd{false};
};

inline constexpr uint64_t DEFAULT_DRIVER_WAIT_TIMEOUT_NS = 500'000'000ULL;

} // namespace LSFG

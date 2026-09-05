#pragma once

#include <vulkan/vulkan_core.h>

#include <array>
#include <cstdint>
#include <string>

namespace LSFG {

struct DeviceIdentity {
    std::array<uint8_t, VK_UUID_SIZE> deviceUUID{};
    std::array<uint8_t, VK_UUID_SIZE> driverUUID{};

    [[nodiscard]] bool operator==(const DeviceIdentity& other) const noexcept {
        return deviceUUID == other.deviceUUID && driverUUID == other.driverUUID;
    }
};

enum class AhbTransportMode {
    Unsupported,
    DirectStorage,
    TransportOnly,
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
    bool externalSemaphoreFd{false};
    AhbTransportMode ahbTransportMode{AhbTransportMode::Unsupported};
};

inline constexpr uint64_t DEFAULT_DRIVER_WAIT_TIMEOUT_NS = 500'000'000ULL;

[[nodiscard]] inline const char* ahbTransportModeName(AhbTransportMode mode) noexcept {
    switch (mode) {
        case AhbTransportMode::DirectStorage: return "direct-storage";
        case AhbTransportMode::TransportOnly: return "transport-only";
        case AhbTransportMode::Unsupported: return "unsupported";
    }
    return "unsupported";
}

} // namespace LSFG

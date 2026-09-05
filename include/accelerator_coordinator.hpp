#pragma once

#include "frame_generation_backend.hpp"
#include "qnn_runtime_probe.hpp"

#include <cstdint>
#include <string>

namespace LSFG::Accelerator {

enum class AcceleratorHealthState : uint8_t {
    Unprobed, Probing, Supported, Compiling, Warming, Ready, Active, Degraded, Quarantined,
};

enum class BackendOverride : uint8_t { Auto, Vulkan, Qnn, Snpe };

struct AcceleratorStatus {
    bool npuSettingEnabled{false};
    bool qnnRuntimeFound{false};
    bool snpeRuntimeFound{false};
    bool qnnProviderQualified{false};
    bool qnnSystemProviderQualified{false};
    bool qnnComputeAttributionQualified{false};
    bool qnnSharedMemoryQualified{false};
    bool qnnGraphExecutionQualified{false};
    bool qnnNumericalSmokeQualified{false};
    bool directAhbInteropQualified{false};
    bool executionEnabled{false};
    uint32_t qnnBackendId{0};
    QnnComputeBackendKind qnnComputeBackend{QnnComputeBackendKind::None};
    std::string qnnProviderName;
    std::string qnnSystemProviderName;
    QnnVersion qnnCoreApiVersion{};
    QnnVersion qnnBackendApiVersion{};
    QnnVersion qnnSystemApiVersion{};
    std::string qnnComputeLibraryPath;
    std::string qnnSystemLibraryPath;
    std::string qnnBackendBuildId;
    BackendKind selectedBackend{BackendKind::Vulkan};
    BackendOverride requestedBackend{BackendOverride::Auto};
    AcceleratorHealthState healthState{AcceleratorHealthState::Unprobed};
    std::string selectionReason{"npu-setting-disabled"};
    std::string fallbackReason;
};

/// Owns optional accelerator discovery and Phase 2 qualification. QNN HTP is
/// attempted first for modern Snapdragon, then QNN DSP for SM8250/Hexagon v66.
/// Even a fully qualified candidate remains diagnostic-only until Phase 3.
class AcceleratorCoordinator final {
public:
    static AcceleratorCoordinator& instance() noexcept;
    AcceleratorCoordinator(const AcceleratorCoordinator&) = delete;
    AcceleratorCoordinator& operator=(const AcceleratorCoordinator&) = delete;
    ~AcceleratorCoordinator();

    void beginEligibleSession();
    void endSession() noexcept;
    [[nodiscard]] const AcceleratorStatus& status() const noexcept { return status_; }

private:
    AcceleratorCoordinator() = default;
    void closeQnnRuntimeHandles() noexcept;
    void closeRuntimeHandles() noexcept;
    void probeQnnRuntime();
    void probeSnpeRuntime();
    void logSnapshot() const;

    AcceleratorStatus status_{};
    void* qnnSystemHandle_{nullptr};
    void* qnnComputeHandle_{nullptr};
    void* snpeHandle_{nullptr};
};

[[nodiscard]] const char* acceleratorHealthStateName(AcceleratorHealthState state) noexcept;
[[nodiscard]] const char* backendOverrideName(BackendOverride backend) noexcept;

} // namespace LSFG::Accelerator

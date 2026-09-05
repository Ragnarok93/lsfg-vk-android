#pragma once

#include "frame_generation_backend.hpp"
#include "qnn_runtime_probe.hpp"

#include <cstdint>
#include <string>

namespace LSFG::Accelerator {

enum class AcceleratorHealthState : uint8_t {
    Unprobed,
    Probing,
    Supported,
    Compiling,
    Warming,
    Ready,
    Active,
    Degraded,
    Quarantined,
};

enum class BackendOverride : uint8_t {
    Auto,
    Vulkan,
    Qnn,
    Snpe,
};

struct AcceleratorStatus {
    bool npuSettingEnabled{false};
    bool qnnRuntimeFound{false};
    bool snpeRuntimeFound{false};
    bool qnnProviderQualified{false};
    bool qnnSystemProviderQualified{false};
    bool qnnHtpAttributionQualified{false};
    bool qnnSharedMemoryQualified{false};
    bool qnnGraphExecutionQualified{false};
    bool qnnNumericalSmokeQualified{false};
    bool directAhbInteropQualified{false};
    bool executionEnabled{false};
    uint32_t qnnBackendId{0};
    std::string qnnProviderName;
    std::string qnnSystemProviderName;
    QnnVersion qnnCoreApiVersion{};
    QnnVersion qnnBackendApiVersion{};
    QnnVersion qnnSystemApiVersion{};
    std::string qnnHtpLibraryPath;
    std::string qnnSystemLibraryPath;
    std::string qnnBackendBuildId;
    BackendKind selectedBackend{BackendKind::Vulkan};
    BackendOverride requestedBackend{BackendOverride::Auto};
    AcceleratorHealthState healthState{AcceleratorHealthState::Unprobed};
    std::string selectionReason{"npu-setting-disabled"};
    std::string fallbackReason;
};

/// Owns optional accelerator runtime discovery and backend health state.
///
/// Phase 2 verifies the QNN HTP/System provider identity, a one-shot HTP graph
/// execution, AHardwareBuffer-backed QNN memory registration, and numerical
/// smoke output. It still does not redirect LSFG stages: Vulkan remains the
/// unconditional execution backend until the Phase 3 dynamic-warp benchmark.
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

    void closeRuntimeHandles() noexcept;
    void probeQnnRuntime();
    void probeSnpeRuntime();
    void logSnapshot() const;

    AcceleratorStatus status_{};
    void* qnnSystemHandle_{nullptr};
    void* qnnHtpHandle_{nullptr};
    void* snpeHandle_{nullptr};
};

[[nodiscard]] const char* acceleratorHealthStateName(AcceleratorHealthState state) noexcept;
[[nodiscard]] const char* backendOverrideName(BackendOverride backend) noexcept;

} // namespace LSFG::Accelerator

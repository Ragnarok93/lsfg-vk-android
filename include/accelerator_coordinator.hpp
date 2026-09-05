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
    BackendKind selectedBackend{BackendKind::Vulkan};
    BackendOverride requestedBackend{BackendOverride::Auto};
    AcceleratorHealthState healthState{AcceleratorHealthState::Unprobed};
    std::string selectionReason{"npu-setting-disabled"};
    std::string fallbackReason;
};

/// Owns optional accelerator runtime discovery and backend health state.
///
/// Phase 2 verifies the actual QNN HTP/System providers and records runtime
/// provenance, but does not create accelerator devices/graphs or redirect frame
/// generation. Vulkan remains the unconditional execution backend until the
/// memory/interoperability and execution path are separately qualified.
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

#pragma once

#include "frame_generation_backend.hpp"

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
    bool executionEnabled{false};
    BackendKind selectedBackend{BackendKind::Vulkan};
    BackendOverride requestedBackend{BackendOverride::Auto};
    AcceleratorHealthState healthState{AcceleratorHealthState::Unprobed};
    std::string selectionReason{"npu-setting-disabled"};
    std::string fallbackReason;
};

/// Owns optional accelerator runtime discovery and backend health state.
///
/// Phase 1 is discovery/scaffolding only: beginEligibleSession() may probe
/// optional Qualcomm libraries when explicitly allowed, but it never hands
/// frame-generation execution away from Vulkan.
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

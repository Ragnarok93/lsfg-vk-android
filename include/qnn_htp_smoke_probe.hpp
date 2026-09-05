#pragma once

#include "qnn_runtime_probe.hpp"

#include <string>

namespace LSFG::Accelerator {

struct QnnComputeSmokeResult {
    bool computeAttributionQualified{false};
    bool sharedMemoryQualified{false};
    bool graphExecutionQualified{false};
    bool numericalSmokeQualified{false};
    std::string backendBuildId;
    std::string failureReason;
};

/// One-shot Phase 2 qualification of a dynamically selected QNN compute
/// provider. The same small signed-INT8 Relu graph is used for modern HTP and
/// SM8250-class DSP-v66 providers, with AHardwareBuffer-backed QNN memory.
/// This is diagnostic qualification only: it does not time or execute LSFG.
QnnComputeSmokeResult probeQnnComputeGraphAndSharedMemory(
    void* qnnComputeHandle, QnnComputeBackendKind computeBackend) noexcept;

} // namespace LSFG::Accelerator

#pragma once

#include <string>

namespace LSFG::Accelerator {

struct QnnHtpSmokeResult {
    bool htpAttributionQualified{false};
    bool sharedMemoryQualified{false};
    bool graphExecutionQualified{false};
    bool numericalSmokeQualified{false};
    std::string backendBuildId;
    std::string failureReason;
};

/// One-shot Phase 2 qualification of the dynamically selected HTP provider.
/// The probe builds a tiny signed-INT8 Relu graph and executes it using
/// AHardwareBuffer-backed memory registered with QNN. It performs no timing
/// loop and never becomes the LSFG execution path.
QnnHtpSmokeResult probeQnnHtpGraphAndSharedMemory(void* qnnHtpHandle) noexcept;

} // namespace LSFG::Accelerator

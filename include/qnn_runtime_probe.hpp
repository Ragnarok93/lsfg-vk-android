#pragma once

#include <cstdint>
#include <string>

namespace LSFG::Accelerator {

enum class QnnComputeBackendKind : uint8_t {
    None,
    Htp,
    Dsp,
};

struct QnnVersion {
    uint32_t major{0};
    uint32_t minor{0};
    uint32_t patch{0};

    [[nodiscard]] bool valid() const noexcept { return major != 0; }
};

struct QnnRuntimeProbeResult {
    bool systemLibraryLoaded{false};
    bool computeLibraryLoaded{false};
    bool qnnInterfaceSymbolFound{false};
    bool qnnSystemInterfaceSymbolFound{false};
    bool qnnProviderEnumerated{false};
    bool qnnSystemProviderEnumerated{false};
    bool qnnProviderQualified{false};
    bool qnnSystemProviderQualified{false};
    uint32_t qnnProviderCount{0};
    uint32_t qnnSystemProviderCount{0};
    uint32_t backendId{0};
    uint32_t systemBackendId{0};
    QnnComputeBackendKind computeBackend{QnnComputeBackendKind::None};
    std::string providerName;
    std::string systemProviderName;
    QnnVersion coreApiVersion{};
    QnnVersion backendApiVersion{};
    QnnVersion systemApiVersion{};
    std::string computeLibraryPath;
    std::string systemLibraryPath;
    std::string systemLoadDiagnostic;
    std::string computeLoadDiagnostic;
    std::string failureReason;
};

/// Inspect the versioned QNN provider prefixes exposed by an optional compute
/// backend and libQnnSystem. Phase 2 supports both the HTP backend used by
/// newer Snapdragon platforms and the DSP backend used by SM8250/Hexagon v66.
/// No backend/device/context/graph/tensor/memory API is called here.
QnnRuntimeProbeResult probeQnnRuntimeMetadata(
    QnnComputeBackendKind computeBackend,
    void*& qnnSystemHandle,
    void*& qnnComputeHandle) noexcept;

[[nodiscard]] const char* qnnComputeBackendName(QnnComputeBackendKind backend) noexcept;

} // namespace LSFG::Accelerator

#pragma once

#include <cstdint>
#include <string>

namespace LSFG::Accelerator {

struct QnnVersion {
    uint32_t major{0};
    uint32_t minor{0};
    uint32_t patch{0};

    [[nodiscard]] bool valid() const noexcept { return major != 0; }
};

struct QnnRuntimeProbeResult {
    bool systemLibraryLoaded{false};
    bool htpLibraryLoaded{false};
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
    std::string providerName;
    std::string systemProviderName;
    QnnVersion coreApiVersion{};
    QnnVersion backendApiVersion{};
    QnnVersion systemApiVersion{};
    std::string htpLibraryPath;
    std::string systemLibraryPath;
    std::string failureReason;
};

/// Inspect the public, versioned QNN provider prefixes exposed by the optional
/// HTP/System runtime libraries. This intentionally does not call backend,
/// device, context, graph, tensor, or memory-registration entry points.
QnnRuntimeProbeResult probeQnnRuntimeMetadata(
    void*& qnnSystemHandle, void*& qnnHtpHandle) noexcept;

} // namespace LSFG::Accelerator

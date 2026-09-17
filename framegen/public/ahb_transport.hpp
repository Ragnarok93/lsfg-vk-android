#pragma once

namespace LSFG {

enum class AhbTransportMode {
    Unsupported,
    DirectStorage,
    DirectInputCopyOutput,
    CopyInputDirectOutput,
    TransportOnly,
};

enum class AhbImageRole {
    Input,
    Output,
};

[[nodiscard]] inline bool ahbStorageUsageRequired(
        AhbTransportMode mode, AhbImageRole role) noexcept {
    return role == AhbImageRole::Output
        && (mode == AhbTransportMode::DirectStorage
            || mode == AhbTransportMode::CopyInputDirectOutput);
}

[[nodiscard]] inline AhbTransportMode selectAhbTransportMode(
        bool sampledInput, bool transferInput,
        bool storageOutput, bool transferOutput) noexcept {
    if (sampledInput && storageOutput)
        return AhbTransportMode::DirectStorage;
    if (sampledInput && transferOutput)
        return AhbTransportMode::DirectInputCopyOutput;
    if (transferInput && storageOutput)
        return AhbTransportMode::CopyInputDirectOutput;
    if (transferInput && transferOutput)
        return AhbTransportMode::TransportOnly;
    return AhbTransportMode::Unsupported;
}

[[nodiscard]] inline bool ahbInputCopyRequired(AhbTransportMode mode) noexcept {
    return mode == AhbTransportMode::CopyInputDirectOutput
        || mode == AhbTransportMode::TransportOnly;
}

[[nodiscard]] inline bool ahbOutputCopyRequired(AhbTransportMode mode) noexcept {
    return mode == AhbTransportMode::DirectInputCopyOutput
        || mode == AhbTransportMode::TransportOnly;
}

[[nodiscard]] inline const char* ahbTransportModeName(AhbTransportMode mode) noexcept {
    switch (mode) {
        case AhbTransportMode::DirectStorage: return "direct-storage";
        case AhbTransportMode::DirectInputCopyOutput: return "direct-input-copy-output";
        case AhbTransportMode::CopyInputDirectOutput: return "copy-input-direct-output";
        case AhbTransportMode::TransportOnly: return "transport-only";
        case AhbTransportMode::Unsupported: return "unsupported";
    }
    return "unsupported";
}

} // namespace LSFG

#pragma once

#include <cstdint>

namespace LSFG::Accelerator {

enum class BackendKind : uint8_t {
    Vulkan,
    Qnn,
    Snpe,
};

/// Execution abstraction for LSFG semantic compute backends.
///
/// Phase 1 intentionally leaves the existing Vulkan frame-generation path
/// wired exactly as it is today. Accelerator implementations will adopt this
/// interface only after their semantic graphs are qualified and validated.
class FrameGenerationComputeBackend {
public:
    virtual ~FrameGenerationComputeBackend() = default;

    [[nodiscard]] virtual BackendKind kind() const noexcept = 0;
    [[nodiscard]] virtual bool available() const noexcept = 0;
};

[[nodiscard]] inline const char* backendKindName(BackendKind kind) noexcept {
    switch (kind) {
        case BackendKind::Vulkan: return "vulkan";
        case BackendKind::Qnn: return "qnn";
        case BackendKind::Snpe: return "snpe";
    }
    return "vulkan";
}

} // namespace LSFG::Accelerator

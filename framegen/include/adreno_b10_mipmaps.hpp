#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace LSFG::Core { class Device; }

namespace LSFG::Optimizations::AdrenoB10 {

struct HeadTransformStats {
    bool applied{false};
    std::string reason{"not-run"};
    uint32_t targetFunction{0};
    uint32_t imageSamplesBefore{0};
    uint32_t imageWritesBefore{0};
    uint32_t barriersBefore{0};
    uint32_t imageSamplesAfter{0};
    uint32_t imageWritesAfter{0};
    uint32_t barriersAfter{0};
    uint32_t workgroupStoresBeforePhase0{0};
    uint32_t workgroupStoresAfter{0};
    bool scratchMirrorsWorkgroupSeed{false};
};

// Fail-closed transform of the translated production p_mipmaps module.
// The original 32x32 phase 0 is retained, its unquantized u1 value is mirrored
// into descriptor binding 5, and the in-shader u2-u6 reduction is removed.
bool transformHead(std::vector<uint8_t>& spirv, HeadTransformStats& stats);

#ifdef LSFGVK_B10_SELF_TEST
bool transformHeadForSelfTest(std::vector<uint8_t>& spirv, HeadTransformStats& stats);
#endif

// Compact 16x16 follow-up pass. Binding 0 is the R32F u1 scratch image;
// bindings 1..5 are the original R8 u2..u6 images.
std::vector<uint8_t> buildTailSpirv();

// Pure predicate so the platform gate is independently testable.
bool isAdreno6xxDevice(uint32_t vendorId, std::string_view deviceName);

// Runtime gate: exact Qualcomm Adreno 6xx plus the storage-image capabilities
// required by the compact R32F scratch/tail path.
bool isRuntimeSupported(const Core::Device& device);

} // namespace LSFG::Optimizations::AdrenoB10

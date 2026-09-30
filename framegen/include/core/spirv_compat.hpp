#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace LSFG::Core {

struct SpirvCompatibilityResult {
    bool supported{false};
    bool lowered{false};
    uint32_t sourceVersion{0};
    uint32_t targetVersion{0};
    std::vector<uint32_t> capabilities;
    std::vector<uint8_t> code;
    std::string rejectionReason;
};

[[nodiscard]] SpirvCompatibilityResult prepareSpirvForTarget(
    const std::vector<uint8_t>& source, uint32_t targetVersion);

} // namespace LSFG::Core

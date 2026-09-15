    words.push_back((wc << 16u) | static_cast<uint32_t>(op));
    words.insert(words.end(), operands.begin(), operands.end());
}

uint32_t extractScalar(dxvk::SpirvModule& module, uint32_t scalarType,
        uint32_t composite, uint32_t index) {
    return module.opCompositeExtract(scalarType, composite, 1, &index);
}

uint32_t vec2u(dxvk::SpirvModule& module, uint32_t type,
        uint32_t x, uint32_t y) {
    const std::array<uint32_t, 2> values{x, y};
    return module.opCompositeConstruct(type,
        static_cast<uint32_t>(values.size()), values.data());
}

uint32_t vec4f(dxvk::SpirvModule& module, uint32_t type, uint32_t x) {
    const std::array<uint32_t, 4> values{x, x, x, x};
    return module.opCompositeConstruct(type,
        static_cast<uint32_t>(values.size()), values.data());
}

uint32_t readScalar(dxvk::SpirvModule& module, uint32_t vec4Type,
        uint32_t scalarType, uint32_t image, uint32_t coord) {
    const dxvk::SpirvImageOperands noOperands{};
    const uint32_t value = module.opImageRead(vec4Type, image, coord, noOperands);
    return extractScalar(module, scalarType, value, 0);
}

void writeScalarBounded(dxvk::SpirvModule& module, uint32_t boolType,
        uint32_t uintType, uint32_t vec2uType, uint32_t vec4fType,
        uint32_t image, uint32_t x, uint32_t y, uint32_t value) {
    const uint32_t extent = module.opImageQuerySize(vec2uType, image);
    const uint32_t width = extractScalar(module, uintType, extent, 0);
    const uint32_t height = extractScalar(module, uintType, extent, 1);
    const uint32_t xInBounds = module.opULessThan(boolType, x, width);
    const uint32_t yInBounds = module.opULessThan(boolType, y, height);
    const uint32_t inBounds = module.opLogicalAnd(boolType, xInBounds, yInBounds);
    const uint32_t writeLabel = module.allocateId();
    const uint32_t mergeLabel = module.allocateId();
    module.opSelectionMerge(mergeLabel, spv::SelectionControlMaskNone);
    module.opBranchConditional(inBounds, writeLabel, mergeLabel);
    module.opLabel(writeLabel);
    const dxvk::SpirvImageOperands noOperands{};
    module.opImageWrite(image, vec2u(module, vec2uType, x, y),
        vec4f(module, vec4fType, value), noOperands);
    module.opBranch(mergeLabel);
    module.opLabel(mergeLabel);
}

uint32_t reduce4(dxvk::SpirvModule& module, uint32_t floatType,
        uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t quarter) {
    const uint32_t ab = module.opFAdd(floatType, a, b);
    const uint32_t abc = module.opFAdd(floatType, ab, c);
    const uint32_t abcd = module.opFAdd(floatType, abc, d);
    return module.opFMul(floatType, abcd, quarter);
}

uint32_t sharedPointer(dxvk::SpirvModule& module, uint32_t ptrFloatWorkgroup,
        uint32_t shared, uint32_t index) {
    return module.opAccessChain(ptrFloatWorkgroup, shared, 1, &index);
}

uint32_t sharedIndex(dxvk::SpirvModule& module, uint32_t uintType,
        uint32_t x, uint32_t y, uint32_t sixteen) {
    return module.opIAdd(uintType,
        module.opIMul(uintType, x, sixteen), y);
}

uint32_t activeMask(dxvk::SpirvModule& module, uint32_t boolType,
        uint32_t uintType, uint32_t x, uint32_t y,
        uint32_t mask, uint32_t zero) {
    const uint32_t xMasked = module.opBitwiseAnd(uintType, x, mask);
    const uint32_t yMasked = module.opBitwiseAnd(uintType, y, mask);
    const uint32_t xOk = module.opIEqual(boolType, xMasked, zero);
    const uint32_t yOk = module.opIEqual(boolType, yMasked, zero);
    return module.opLogicalAnd(boolType, xOk, yOk);
}

void barrier(dxvk::SpirvModule& module, uint32_t scope, uint32_t semantics) {
    module.opControlBarrier(scope, scope, semantics);
}

} // namespace

bool isAdreno6xxDevice(uint32_t vendorId, std::string_view deviceName) {
    if (vendorId != kQualcommVendorId)
        return false;
    const auto adreno = deviceName.find("Adreno");
    if (adreno == std::string_view::npos)
        return false;

    for (size_t i = adreno + 6; i < deviceName.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(deviceName[i])))
            continue;
        uint32_t value = 0;
        size_t digits = 0;
        for (; i < deviceName.size() && std::isdigit(static_cast<unsigned char>(deviceName[i])); ++i) {
            value = value * 10u + static_cast<uint32_t>(deviceName[i] - '0');
            ++digits;
            if (digits == 3)
                break;
        }
        return digits == 3 && value >= 600u && value < 700u;
    }
    return false;
}

#ifndef LSFGVK_B10_SELF_TEST
bool isRuntimeSupported(const Core::Device& device) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.getPhysicalDevice(), &properties);
    if (!isAdreno6xxDevice(properties.vendorID, properties.deviceName))
        return false;

    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(device.getPhysicalDevice(), &features);
    if (features.shaderStorageImageReadWithoutFormat != VK_TRUE
            || features.shaderStorageImageWriteWithoutFormat != VK_TRUE)
        return false;

    VkFormatProperties format{};
    vkGetPhysicalDeviceFormatProperties(
        device.getPhysicalDevice(), VK_FORMAT_R32_SFLOAT, &format);
    return (format.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}
#endif

namespace {

bool transformHeadImpl(std::vector<uint8_t>& spirv, HeadTransformStats& stats,
        bool requireProductionFingerprint) {
    stats = HeadTransformStats{};
    if (spirv.size() < 20 || (spirv.size() % sizeof(uint32_t)) != 0) {
        stats.reason = "invalid-byte-size";
        return false;
    }

    if (requireProductionFingerprint
            && (spirv.size() != kExpectedHeadBytes || fnv1a64(spirv) != kExpectedHeadFnv)) {
        stats.reason = "production-fingerprint";
        return false;
    }

    std::vector<uint32_t> words(spirv.size() / sizeof(uint32_t));
    std::memcpy(words.data(), spirv.data(), spirv.size());
    if (words[0] != kSpirvMagic || words[3] == 0
            || (requireProductionFingerprint && words[3] != kExpectedHeadBound)) {
        stats.reason = "invalid-header";
        return false;
    }

    Parsed parsed;
    if (!parseModule(words, parsed)) {
        stats.reason = "parse-failed";
        return false;
    }
    if (!hasLocalSize32(words, parsed)) {
        stats.reason = "local-size";
        return false;
    }

    const auto shapes = functionShapes(words, parsed);
    std::vector<FunctionShape> candidates;
    for (const auto& shape : shapes) {
        if (shape.samples == 4 && shape.writes == 10 && shape.barriers == 5)
            candidates.push_back(shape);
    }
    if (candidates.size() != 1) {
        stats.reason = candidates.empty() ? "hot-function-not-found" : "hot-function-ambiguous";
        return false;
    }
    const auto target = candidates.front();
    stats.targetFunction = target.id;
    stats.imageSamplesBefore = target.samples;
    stats.imageWritesBefore = target.writes;
    stats.barriersBefore = target.barriers;

    size_t firstBarrierIndex = std::numeric_limits<size_t>::max();
    std::vector<size_t> phase0Writes;
    std::vector<size_t> phase0WorkgroupStores;
    uint32_t phase0Samples = 0;
    for (size_t i = target.beginIndex + 1; i < target.endIndex; ++i) {
        const auto& inst = parsed.instructions[i];
        if (inst.op == spv::OpControlBarrier) {
            firstBarrierIndex = i;
            break;
        }
        if (isImageSample(inst.op))
            ++phase0Samples;
        if (inst.op == spv::OpImageWrite)
            phase0Writes.push_back(i);
        if (inst.op == spv::OpStore && inst.wordCount >= 3) {
            const auto storage = pointerStorage(parsed, words[inst.offset + 1]);
            if (storage && *storage == spv::StorageClassWorkgroup)
                phase0WorkgroupStores.push_back(i);
        }

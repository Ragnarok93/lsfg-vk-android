    }
    if (firstBarrierIndex == std::numeric_limits<size_t>::max()
            || phase0Samples != 4 || phase0Writes.size() != 5) {
        stats.reason = "phase0-shape";
        return false;
    }
    stats.workgroupStoresBeforePhase0 = static_cast<uint32_t>(phase0WorkgroupStores.size());
    if (stats.workgroupStoresBeforePhase0 != 1) {
        stats.reason = "phase0-workgroup-store-count";
        return false;
    }

    for (size_t i = 0; i < 4; ++i) {
        const auto& write = parsed.instructions[phase0Writes[i]];
        if (write.wordCount < 4) {
            stats.reason = "u0-write-shape";
            return false;
        }
        const auto binding = imageBinding(parsed, words[write.offset + 1]);
        if (!binding || *binding != 3) {
            stats.reason = "u0-binding";
            return false;
        }
    }
    const auto& u1Write = parsed.instructions[phase0Writes[4]];
    if (u1Write.wordCount < 4) {
        stats.reason = "u1-write-shape";
        return false;
    }
    const auto u1Binding = imageBinding(parsed, words[u1Write.offset + 1]);
    if (!u1Binding || *u1Binding != 4) {
        stats.reason = "u1-binding";
        return false;
    }

    // Mirror the exact value the production shader stores into TGSM. The real
    // p_mipmaps SSA does not use the same result id for the u1 texel and the
    // later TGSM store: it writes component 1 to u1, mutates another component,
    // reloads component 1, bitcasts it to uint, then stores those bits to TGSM.
    // Reconstructing scratch from the TGSM store itself is both simpler and
    // stronger than trying to prove equivalence through that mutable SSA chain.
    const auto& phase0WorkgroupStore = parsed.instructions[phase0WorkgroupStores.front()];
    const uint32_t workgroupSeedValue = words[phase0WorkgroupStore.offset + 2];
    const uint32_t u1CoordinateValue = words[u1Write.offset + 2];
    const uint32_t u1TexelValue = words[u1Write.offset + 3];

    auto resultType = [&](uint32_t resultId) -> std::optional<uint32_t> {
        for (const auto& inst : parsed.instructions) {
            if (inst.wordCount < 3 || words[inst.offset + 2] != resultId)
                continue;
            switch (inst.op) {
            case spv::OpLoad:
            case spv::OpCompositeConstruct:
            case spv::OpCompositeExtract:
            case spv::OpCompositeInsert:
            case spv::OpVectorShuffle:
            case spv::OpBitcast:
            case spv::OpCopyObject:
                return words[inst.offset + 1];
            default:
                break;
            }
        }
        return std::nullopt;
    };

    const auto u1TexelType = resultType(u1TexelValue);
    const auto workgroupSeedType = resultType(workgroupSeedValue);
    if (!u1TexelType || !workgroupSeedType) {
        stats.reason = "u1-workgroup-seed-mismatch";
        return false;
    }

    uint32_t scratchScalarType = 0;
    for (const auto& inst : parsed.instructions) {
        if (inst.op != spv::OpTypeVector || inst.wordCount < 4
                || words[inst.offset + 1] != *u1TexelType)
            continue;
        if (words[inst.offset + 3] != 4) {
            stats.reason = "u1-texel-vector-width";
            return false;
        }
        scratchScalarType = words[inst.offset + 2];
        break;
    }
    if (scratchScalarType == 0) {
        stats.reason = "u1-texel-type";
        return false;
    }

    bool scalarIsFloat32 = false;
    bool seedIsUint32 = false;
    for (const auto& inst : parsed.instructions) {
        if (inst.op == spv::OpTypeFloat && inst.wordCount >= 3
                && words[inst.offset + 1] == scratchScalarType
                && words[inst.offset + 2] == 32)
            scalarIsFloat32 = true;
        if (inst.op == spv::OpTypeInt && inst.wordCount >= 4
                && words[inst.offset + 1] == *workgroupSeedType
                && words[inst.offset + 2] == 32
                && words[inst.offset + 3] == 0)
            seedIsUint32 = true;
    }
    if (!scalarIsFloat32 || !seedIsUint32) {
        stats.reason = "u1-workgroup-seed-mismatch";
        return false;
    }
    stats.scratchMirrorsWorkgroupSeed = true;

    uint32_t scratchVariable = 0;
    for (const auto& [id, binding] : parsed.bindingById) {
        if (binding != 5)
            continue;
        const auto setIt = parsed.setById.find(id);
        if (setIt != parsed.setById.end() && setIt->second != 0)
            continue;
        if (scratchVariable != 0) {
            stats.reason = "scratch-binding-ambiguous";
            return false;
        }
        scratchVariable = id;
    }
    if (scratchVariable == 0) {
        stats.reason = "scratch-binding-missing";
        return false;
    }
    const auto scratchVarTypeIt = parsed.typeById.find(scratchVariable);
    if (scratchVarTypeIt == parsed.typeById.end()) {
        stats.reason = "scratch-variable-type";
        return false;
    }
    const uint32_t scratchPointerType = scratchVarTypeIt->second;
    const auto scratchStorageIt = parsed.pointerStorageByType.find(scratchPointerType);
    const auto scratchPointeeIt = parsed.pointerPointeeByType.find(scratchPointerType);
    if (scratchStorageIt == parsed.pointerStorageByType.end()
            || scratchPointeeIt == parsed.pointerPointeeByType.end()
            || scratchStorageIt->second != spv::StorageClassUniformConstant) {
        stats.reason = "scratch-pointer-type";
        return false;
    }
    const uint32_t scratchImageType = scratchPointeeIt->second;

    const uint32_t newImageId = words[3];
    if (newImageId > std::numeric_limits<uint32_t>::max() - 3u) {
        stats.reason = "id-bound-overflow";
        return false;
    }
    const uint32_t newScalarId = newImageId + 1u;
    const uint32_t newTexelId = newImageId + 2u;

    std::vector<uint32_t> output;
    output.reserve(words.size());
    output.insert(output.end(), words.begin(), words.begin() + 5);
    output[3] = newImageId + 3u;

    bool inTarget = false;
    bool truncated = false;
    bool scratchWriteInserted = false;
    for (size_t i = 0; i < parsed.instructions.size(); ++i) {
        const auto& inst = parsed.instructions[i];
        if (i == target.beginIndex)
            inTarget = true;

        if (!inTarget) {
            appendInstruction(output, words, inst);
            continue;
        }

        if (truncated) {
            if (inst.op == spv::OpFunctionEnd) {
                putOp(output, spv::OpReturn, {});
                putOp(output, spv::OpFunctionEnd, {});
                inTarget = false;
                truncated = false;
            }
            continue;
        }

        if (inst.op == spv::OpControlBarrier) {
            truncated = true;
            continue;
        }

        if (inst.op == spv::OpStore && inst.wordCount >= 3) {
            const auto storage = pointerStorage(parsed, words[inst.offset + 1]);
            if (storage && *storage == spv::StorageClassWorkgroup) {
                if (i == phase0WorkgroupStores.front()) {
                    putOp(output, spv::OpLoad,
                        {scratchImageType, newImageId, scratchVariable});
                    putOp(output, spv::OpBitcast,
                        {scratchScalarType, newScalarId, workgroupSeedValue});
                    putOp(output, spv::OpCompositeConstruct,
                        {*u1TexelType, newTexelId,
                         newScalarId, newScalarId, newScalarId, newScalarId});
                    putOp(output, spv::OpImageWrite,
                        {newImageId, u1CoordinateValue, newTexelId});
                    scratchWriteInserted = true;
                }
                continue;
            }
        }

        appendInstruction(output, words, inst);
    }

    if (inTarget || truncated || !scratchWriteInserted) {
        stats.reason = "rewrite-incomplete";
        return false;
    }

    Parsed afterParsed;
    if (!parseModule(output, afterParsed)) {
        stats.reason = "post-parse-failed";
        return false;
    }
    const auto afterShapes = functionShapes(output, afterParsed);
    const auto afterIt = std::find_if(afterShapes.begin(), afterShapes.end(),
        [id = target.id](const FunctionShape& shape) { return shape.id == id; });
    if (afterIt == afterShapes.end()) {
        stats.reason = "post-function-missing";
        return false;
    }
    stats.imageSamplesAfter = afterIt->samples;
    stats.imageWritesAfter = afterIt->writes;
    stats.barriersAfter = afterIt->barriers;
    stats.workgroupStoresAfter = countWorkgroupStores(
        output, afterParsed, afterIt->beginIndex + 1, afterIt->endIndex);
    if (stats.imageSamplesAfter != 4 || stats.imageWritesAfter != 6
            || stats.barriersAfter != 0 || stats.workgroupStoresAfter != 0
            || !hasLocalSize32(output, afterParsed)) {
        stats.reason = "post-contract";
        return false;
    }

    spirv.resize(output.size() * sizeof(uint32_t));
    std::memcpy(spirv.data(), output.data(), spirv.size());
    stats.applied = true;
    stats.reason = "ok";
    return true;
}

} // namespace

bool transformHead(std::vector<uint8_t>& spirv, HeadTransformStats& stats) {
    return transformHeadImpl(spirv, stats, true);
}

#ifdef LSFGVK_B10_SELF_TEST
bool transformHeadForSelfTest(std::vector<uint8_t>& spirv, HeadTransformStats& stats) {
    return transformHeadImpl(spirv, stats, false);
}
#endif

std::vector<uint8_t> buildTailSpirv() {
    dxvk::SpirvModule module(dxvk::spvVersion(1, 3));
    module.enableCapability(spv::CapabilityShader);
    module.enableCapability(spv::CapabilityImageQuery);
    module.enableCapability(spv::CapabilityStorageImageReadWithoutFormat);
    module.enableCapability(spv::CapabilityStorageImageWriteWithoutFormat);
    module.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);

    const uint32_t voidType = module.defVoidType();
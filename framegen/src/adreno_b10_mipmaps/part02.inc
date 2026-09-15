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

    // The compact tail must receive the exact value that the original shader
    // seeded into TGSM, including padded edge lanes. The production shader
    // writes u1 as a vec4 replicate of that same scalar. Prove that relation
    // structurally before cloning the u1 image write into the padded R32F
    // scratch image; do not infer it from instruction proximity.
    const auto& phase0WorkgroupStore = parsed.instructions[phase0WorkgroupStores.front()];
    const uint32_t workgroupSeedValue = words[phase0WorkgroupStore.offset + 2];
    const uint32_t u1TexelValue = words[u1Write.offset + 3];
    bool u1ReplicatesWorkgroupSeed = false;
    for (const auto& inst : parsed.instructions) {
        if (inst.op != spv::OpCompositeConstruct || inst.wordCount != 7)
            continue;
        if (words[inst.offset + 2] != u1TexelValue)
            continue;
        u1ReplicatesWorkgroupSeed = true;
        for (size_t operand = 3; operand < 7; ++operand)
            u1ReplicatesWorkgroupSeed = u1ReplicatesWorkgroupSeed
                && words[inst.offset + operand] == workgroupSeedValue;
        break;
    }
    if (!u1ReplicatesWorkgroupSeed) {
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
    if (newImageId == std::numeric_limits<uint32_t>::max()) {
        stats.reason = "id-bound-overflow";
        return false;
    }

    std::vector<uint32_t> output;
    output.reserve(words.size());
    output.insert(output.end(), words.begin(), words.begin() + 5);
    output[3] = newImageId + 1;

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
            if (storage && *storage == spv::StorageClassWorkgroup)
                continue;
        }

        appendInstruction(output, words, inst);
        if (i == phase0Writes[4]) {
            putOp(output, spv::OpLoad,
                {scratchImageType, newImageId, scratchVariable});
            std::vector<uint32_t> scratchWrite(
                words.begin() + static_cast<std::ptrdiff_t>(u1Write.offset),
                words.begin() + static_cast<std::ptrdiff_t>(u1Write.offset + u1Write.wordCount));
            scratchWrite[1] = newImageId;
            output.insert(output.end(), scratchWrite.begin(), scratchWrite.end());
            scratchWriteInserted = true;
        }
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

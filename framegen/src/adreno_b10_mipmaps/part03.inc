    const uint32_t boolType = module.defBoolType();
    const uint32_t uintType = module.defIntType(32, 0);
    const uint32_t floatType = module.defFloatType(32);
    const uint32_t vec2uType = module.defVectorType(uintType, 2);
    const uint32_t vec3uType = module.defVectorType(uintType, 3);
    const uint32_t vec4fType = module.defVectorType(floatType, 4);
    const uint32_t imageType = module.defImageType(floatType, spv::Dim2D,
        0, 0, 0, 2, spv::ImageFormatUnknown);
    const uint32_t imagePtrType = module.defPointerType(imageType, spv::StorageClassUniformConstant);
    const uint32_t inputVec3PtrType = module.defPointerType(vec3uType, spv::StorageClassInput);
    const uint32_t workgroupFloatPtrType = module.defPointerType(floatType, spv::StorageClassWorkgroup);
    const uint32_t array256Type = module.defArrayType(floatType, module.constu32(256));
    const uint32_t workgroupArrayPtrType = module.defPointerType(array256Type, spv::StorageClassWorkgroup);
    const uint32_t functionType = module.defFunctionType(voidType, 0, nullptr);

    const uint32_t localIdVar = module.newVar(inputVec3PtrType, spv::StorageClassInput);
    module.decorateBuiltIn(localIdVar, spv::BuiltInLocalInvocationId);
    const uint32_t groupIdVar = module.newVar(inputVec3PtrType, spv::StorageClassInput);
    module.decorateBuiltIn(groupIdVar, spv::BuiltInWorkgroupId);

    std::array<uint32_t, 6> imageVars{};
    for (uint32_t binding = 0; binding < imageVars.size(); ++binding) {
        imageVars[binding] = module.newVar(imagePtrType, spv::StorageClassUniformConstant);
        module.decorateDescriptorSet(imageVars[binding], 0);
        module.decorateBinding(imageVars[binding], binding);
    }
    const uint32_t shared = module.newVar(workgroupArrayPtrType, spv::StorageClassWorkgroup);

    const uint32_t entry = module.allocateId();
    module.addEntryPoint(entry, spv::ExecutionModelGLCompute, "main");
    module.setLocalSize(entry, 16, 16, 1);
    module.functionBegin(voidType, entry, functionType, spv::FunctionControlMaskNone);
    module.opLabel(module.allocateId());

    const uint32_t zeroU = module.constu32(0);
    const uint32_t oneU = module.constu32(1);
    const uint32_t twoU = module.constu32(2);
    const uint32_t sixteenU = module.constu32(16);
    const uint32_t thirtyTwoU = module.constu32(32);
    const uint32_t scope = module.constu32(static_cast<uint32_t>(spv::ScopeWorkgroup));
    const uint32_t semantics = module.constu32(0x0108u);
    const uint32_t quarter = module.constf32(0.25f);

    const uint32_t local = module.opLoad(vec3uType, localIdVar);
    const uint32_t group = module.opLoad(vec3uType, groupIdVar);
    const uint32_t lx = extractScalar(module, uintType, local, 0);
    const uint32_t ly = extractScalar(module, uintType, local, 1);
    const uint32_t gx = extractScalar(module, uintType, group, 0);
    const uint32_t gy = extractScalar(module, uintType, group, 1);

    std::array<uint32_t, 6> images{};
    for (size_t i = 0; i < images.size(); ++i)
        images[i] = module.opLoad(imageType, imageVars[i]);

    const uint32_t scratchBaseX = module.opIAdd(uintType,
        module.opIMul(uintType, gx, thirtyTwoU), module.opIMul(uintType, lx, twoU));
    const uint32_t scratchBaseY = module.opIAdd(uintType,
        module.opIMul(uintType, gy, thirtyTwoU), module.opIMul(uintType, ly, twoU));
    const uint32_t scratchX1 = module.opIAdd(uintType, scratchBaseX, oneU);
    const uint32_t scratchY1 = module.opIAdd(uintType, scratchBaseY, oneU);

    const uint32_t s00 = readScalar(module, vec4fType, floatType, images[0],
        vec2u(module, vec2uType, scratchBaseX, scratchBaseY));
    const uint32_t s10 = readScalar(module, vec4fType, floatType, images[0],
        vec2u(module, vec2uType, scratchX1, scratchBaseY));
    const uint32_t s01 = readScalar(module, vec4fType, floatType, images[0],
        vec2u(module, vec2uType, scratchBaseX, scratchY1));
    const uint32_t s11 = readScalar(module, vec4fType, floatType, images[0],
        vec2u(module, vec2uType, scratchX1, scratchY1));
    const uint32_t u2 = reduce4(module, floatType, s00, s10, s01, s11, quarter);

    const uint32_t u2x = module.opIAdd(uintType,
        module.opIMul(uintType, gx, sixteenU), lx);
    const uint32_t u2y = module.opIAdd(uintType,
        module.opIMul(uintType, gy, sixteenU), ly);
    writeScalarBounded(module, boolType, uintType, vec2uType, vec4fType,
        images[1], u2x, u2y, u2);

    const uint32_t baseIndex = sharedIndex(module, uintType, lx, ly, sixteenU);
    module.opStore(sharedPointer(module, workgroupFloatPtrType, shared, baseIndex), u2);
    barrier(module, scope, semantics);

    auto emitReductionPhase = [&](uint32_t maskValue, uint32_t neighborOffset,
            uint32_t outputImage, uint32_t groupScale, uint32_t localDivisor,
            bool storeResult) {
        const uint32_t mask = module.constu32(maskValue);
        const uint32_t offset = module.constu32(neighborOffset);
        const uint32_t cond = activeMask(module, boolType, uintType,
            lx, ly, mask, zeroU);
        const uint32_t activeLabel = module.allocateId();
        const uint32_t mergeLabel = module.allocateId();
        module.opSelectionMerge(mergeLabel, spv::SelectionControlMaskNone);
        module.opBranchConditional(cond, activeLabel, mergeLabel);
        module.opLabel(activeLabel);

        const uint32_t idx0 = sharedIndex(module, uintType, lx, ly, sixteenU);
        const uint32_t idxX = sharedIndex(module, uintType,
            module.opIAdd(uintType, lx, offset), ly, sixteenU);
        const uint32_t idxY = sharedIndex(module, uintType,
            lx, module.opIAdd(uintType, ly, offset), sixteenU);
        const uint32_t idxXY = sharedIndex(module, uintType,
            module.opIAdd(uintType, lx, offset),
            module.opIAdd(uintType, ly, offset), sixteenU);
        const uint32_t a = module.opLoad(floatType,
            sharedPointer(module, workgroupFloatPtrType, shared, idx0));
        const uint32_t b = module.opLoad(floatType,
            sharedPointer(module, workgroupFloatPtrType, shared, idxX));
        const uint32_t c = module.opLoad(floatType,
            sharedPointer(module, workgroupFloatPtrType, shared, idxY));
        const uint32_t d = module.opLoad(floatType,
            sharedPointer(module, workgroupFloatPtrType, shared, idxXY));
        const uint32_t reduced = reduce4(module, floatType, a, b, c, d, quarter);

        const uint32_t groupScaleId = module.constu32(groupScale);
        const uint32_t localDivisorId = module.constu32(localDivisor);
        const uint32_t ox = module.opIAdd(uintType,
            module.opIMul(uintType, gx, groupScaleId),
            module.opUDiv(uintType, lx, localDivisorId));
        const uint32_t oy = module.opIAdd(uintType,
            module.opIMul(uintType, gy, groupScaleId),
            module.opUDiv(uintType, ly, localDivisorId));
        writeScalarBounded(module, boolType, uintType, vec2uType, vec4fType,
            images[outputImage], ox, oy, reduced);
        if (storeResult)
            module.opStore(sharedPointer(module, workgroupFloatPtrType, shared, idx0), reduced);

        module.opBranch(mergeLabel);
        module.opLabel(mergeLabel);
    };

    emitReductionPhase(1u, 1u, 2u, 8u, 2u, true);   // u3
    barrier(module, scope, semantics);
    emitReductionPhase(3u, 2u, 3u, 4u, 4u, true);   // u4
    barrier(module, scope, semantics);
    emitReductionPhase(7u, 4u, 4u, 2u, 8u, true);   // u5
    barrier(module, scope, semantics);
    emitReductionPhase(15u, 8u, 5u, 1u, 16u, false); // u6

    module.opReturn();
    module.functionEnd();

    const auto code = module.compile();
    std::vector<uint8_t> result(code.size());
    std::memcpy(result.data(), code.data(), code.size());
    return result;
}

} // namespace LSFG::Optimizations::AdrenoB10
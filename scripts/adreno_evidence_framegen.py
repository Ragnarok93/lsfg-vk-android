from __future__ import annotations

from pathlib import Path
from adreno_evidence_common import replace_exact


def patch_timestamp_query_pool(header_path: Path, source_path: Path) -> None:
    header = header_path.read_text(encoding="utf-8")
    if "writeAtStage" not in header:
        header = replace_exact(
            header,
            "        void write(VkCommandBuffer commandBuffer, uint32_t queryIndex) const;\n",
            "        void write(VkCommandBuffer commandBuffer, uint32_t queryIndex) const;\n"
            "        void writeAtStage(VkCommandBuffer commandBuffer, uint32_t queryIndex,\n"
            "            VkPipelineStageFlagBits stage) const;\n",
            count=1,
            label=f"{header_path}: stage-aware timestamp declaration",
        )
        header_path.write_text(header, encoding="utf-8")

    source = source_path.read_text(encoding="utf-8")
    if "TimestampQueryPool::writeAtStage" not in source:
        source = replace_exact(
            source,
            "void TimestampQueryPool::write(\n"
            "        VkCommandBuffer commandBuffer, uint32_t queryIndex) const {\n"
            "    if (!this->supported() || queryIndex >= this->queryCount_)\n"
            "        return;\n"
            "    vkCmdWriteTimestamp(\n"
            "        commandBuffer,\n"
            "        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,\n"
            "        *this->queryPool,\n"
            "        queryIndex);\n"
            "}\n",
            "void TimestampQueryPool::write(\n"
            "        VkCommandBuffer commandBuffer, uint32_t queryIndex) const {\n"
            "    this->writeAtStage(commandBuffer, queryIndex, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);\n"
            "}\n\n"
            "void TimestampQueryPool::writeAtStage(\n"
            "        VkCommandBuffer commandBuffer, uint32_t queryIndex,\n"
            "        VkPipelineStageFlagBits stage) const {\n"
            "    if (!this->supported() || queryIndex >= this->queryCount_)\n"
            "        return;\n"
            "    vkCmdWriteTimestamp(commandBuffer, stage, *this->queryPool, queryIndex);\n"
            "}\n",
            count=1,
            label=f"{source_path}: stage-aware timestamp implementation",
        )
        source_path.write_text(source, encoding="utf-8")

def patch_framegen_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "generatedPreQueryPool" in text:
        return
    text = replace_exact(
        text,
        "            Core::Fence preprocessingFence; // reused for zero-generation temporal preprocessing\n",
        "            Core::Fence preprocessingFence; // reused for zero-generation temporal preprocessing\n"
        "            Core::TimestampQueryPool generatedPreQueryPool;\n"
        "            std::vector<Core::TimestampQueryPool> generatedPassQueryPools;\n"
        "            bool generatedProfilePending{false};\n"
        "            size_t generatedProfileGenerationCount{0};\n",
        count=1,
        label=f"{path}: generated profile slot state",
    )
    text = replace_exact(
        text,
        "        uint32_t zeroStageProfileSamples{0};\n",
        "        uint32_t zeroStageProfileSamples{0};\n"
        "        std::array<double, 4> generatedPreProfileTotalsMs{};\n"
        "        std::array<double, 12> generatedPassProfileTotalsMs{};\n"
        "        uint32_t generatedProfileSourceSamples{0};\n"
        "        uint32_t generatedProfilePassSamples{0};\n",
        count=1,
        label=f"{path}: generated profile accumulators",
    )
    path.write_text(text, encoding="utf-8")

def generated_collection_block(backend: str) -> str:
    return (
        "    if (data.generatedProfilePending) {\n"
        "        const auto preDurations = data.generatedPreQueryPool.durationsMs(vk.device);\n"
        "        if (preDurations.size() == this->generatedPreProfileTotalsMs.size()) {\n"
        "            for (size_t i = 0; i < preDurations.size(); ++i)\n"
        "                this->generatedPreProfileTotalsMs.at(i) += preDurations.at(i);\n"
        "            ++this->generatedProfileSourceSamples;\n"
        "        }\n"
        "        const size_t profiledPasses = std::min(\n"
        "            data.generatedProfileGenerationCount, data.generatedPassQueryPools.size());\n"
        "        for (size_t pass = 0; pass < profiledPasses; ++pass) {\n"
        "            const auto passDurations =\n"
        "                data.generatedPassQueryPools.at(pass).durationsMs(vk.device);\n"
        "            if (passDurations.size() != this->generatedPassProfileTotalsMs.size())\n"
        "                continue;\n"
        "            for (size_t i = 0; i < passDurations.size(); ++i)\n"
        "                this->generatedPassProfileTotalsMs.at(i) += passDurations.at(i);\n"
        "            ++this->generatedProfilePassSamples;\n"
        "        }\n"
        "        data.generatedProfilePending = false;\n"
        "        if (this->generatedProfileSourceSamples >= 30\n"
        "                && this->generatedProfilePassSamples > 0) {\n"
        "            const double sourceSamples =\n"
        "                static_cast<double>(this->generatedProfileSourceSamples);\n"
        "            const double passSamples =\n"
        "                static_cast<double>(this->generatedProfilePassSamples);\n"
        f"            std::cerr << \"lsfg-vk: generated-stage-profile backend={backend}\"\n"
        "                << \" source_samples=\" << this->generatedProfileSourceSamples\n"
        "                << \" pass_samples=\" << this->generatedProfilePassSamples\n"
        "                << \" input_transport_avg_ms=\" << (this->generatedPreProfileTotalsMs.at(0) / sourceSamples)\n"
        "                << \" mipmaps_avg_ms=\" << (this->generatedPreProfileTotalsMs.at(1) / sourceSamples)\n"
        "                << \" alpha_avg_ms=\" << (this->generatedPreProfileTotalsMs.at(2) / sourceSamples)\n"
        "                << \" beta_avg_ms=\" << (this->generatedPreProfileTotalsMs.at(3) / sourceSamples)\n"
        "                << \" gamma0_avg_ms=\" << (this->generatedPassProfileTotalsMs.at(0) / passSamples)\n"
        "                << \" gamma1_avg_ms=\" << (this->generatedPassProfileTotalsMs.at(1) / passSamples)\n"
        "                << \" gamma2_avg_ms=\" << (this->generatedPassProfileTotalsMs.at(2) / passSamples)\n"
        "                << \" gamma3_avg_ms=\" << (this->generatedPassProfileTotalsMs.at(3) / passSamples)\n"
        "                << \" gamma4_avg_ms=\" << (this->generatedPassProfileTotalsMs.at(4) / passSamples)\n"
        "                << \" delta0_avg_ms=\" << (this->generatedPassProfileTotalsMs.at(5) / passSamples)\n"
        "                << \" gamma5_avg_ms=\" << (this->generatedPassProfileTotalsMs.at(6) / passSamples)\n"
        "                << \" delta1_avg_ms=\" << (this->generatedPassProfileTotalsMs.at(7) / passSamples)\n"
        "                << \" gamma6_avg_ms=\" << (this->generatedPassProfileTotalsMs.at(8) / passSamples)\n"
        "                << \" delta2_avg_ms=\" << (this->generatedPassProfileTotalsMs.at(9) / passSamples)\n"
        "                << \" generate_avg_ms=\" << (this->generatedPassProfileTotalsMs.at(10) / passSamples)\n"
        "                << \" output_transport_avg_ms=\" << (this->generatedPassProfileTotalsMs.at(11) / passSamples)\n"
        "                << '\\n';\n"
        "            this->generatedPreProfileTotalsMs.fill(0.0);\n"
        "            this->generatedPassProfileTotalsMs.fill(0.0);\n"
        "            this->generatedProfileSourceSamples = 0;\n"
        "            this->generatedProfilePassSamples = 0;\n"
        "        }\n"
        "    }\n"
    )

def patch_framegen_source(path: Path, backend: str) -> None:
    text = path.read_text(encoding="utf-8")
    if f"generated-stage-profile backend={backend}" in text:
        return

    init_old = (
        "        data.preprocessingFence = Core::Fence(vk.device);\n"
        "        data.internalSemaphores.resize(vk.generationCount);\n"
    )
    init_new = (
        "        data.preprocessingFence = Core::Fence(vk.device);\n"
        "        data.generatedPreQueryPool = Core::TimestampQueryPool(vk.device, 5);\n"
        "        data.generatedPassQueryPools.resize(vk.generationCount);\n"
        "        for (auto& generatedPassQueryPool : data.generatedPassQueryPools)\n"
        "            generatedPassQueryPool = Core::TimestampQueryPool(vk.device, 13);\n"
        "        data.internalSemaphores.resize(vk.generationCount);\n"
    )
    text = replace_exact(
        text, init_old, init_new, count=2,
        label=f"{path}: generated query-pool initialization",
    )

    wait_old = (
        "    if (data.shouldWait)\n"
        "        for (size_t i = 0; i < data.generationCount; ++i)\n"
        "            if (!data.completionFences.at(i).wait(vk.device, framegenWaitTimeoutNs()))\n"
        "                throw LSFG::vulkan_error(VK_TIMEOUT, \"Fence wait timed out\");\n"
        "    data.shouldWait = generationCount > 0;\n"
    )
    wait_new = wait_old.replace(
        "    data.shouldWait = generationCount > 0;\n",
        generated_collection_block(backend) + "    data.shouldWait = generationCount > 0;\n",
    )
    text = replace_exact(
        text, wait_old, wait_new, count=1,
        label=f"{path}: generated result collection",
    )

    text = replace_exact(
        text,
        "    data.cmdBuffer1 = Core::CommandBuffer(vk.device, vk.commandPool);\n"
        "    data.cmdBuffer1.begin();\n\n",
        "    data.cmdBuffer1 = Core::CommandBuffer(vk.device, vk.commandPool);\n"
        "    data.cmdBuffer1.begin();\n"
        "    bool profileGenerated = generationCount > 0 && data.generatedPreQueryPool.supported();\n"
        "#ifdef __ANDROID__\n"
        "    profileGenerated = profileGenerated && this->adaptiveFlowScales_.empty();\n"
        "#endif\n"
        "    for (size_t pass = 0; profileGenerated && pass < generationCount; ++pass)\n"
        "        profileGenerated = data.generatedPassQueryPools.at(pass).supported();\n"
        "    if (profileGenerated) {\n"
        "        data.generatedPreQueryPool.reset(data.cmdBuffer1.handle());\n"
        "        data.generatedPreQueryPool.writeAtStage(\n"
        "            data.cmdBuffer1.handle(), 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);\n"
        "    }\n\n",
        count=1,
        label=f"{path}: generated first-stage profiling start",
    )

    input_transport_anchor = (
        "#endif\n\n"
        "#ifdef __ANDROID__\n"
        "    Core::TimestampQueryPool* adaptiveFlowTimingPool = nullptr;\n"
    )
    input_transport_profiled = (
        "#endif\n\n"
        "    if (profileGenerated)\n"
        "        data.generatedPreQueryPool.writeAtStage(\n"
        "            data.cmdBuffer1.handle(), 1, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);\n\n"
        "#ifdef __ANDROID__\n"
        "    Core::TimestampQueryPool* adaptiveFlowTimingPool = nullptr;\n"
    )
    if "adaptiveFlowScales_" in text:
        text = replace_exact(
            text, input_transport_anchor, input_transport_profiled, count=1,
            label=f"{path}: input transport timestamp",
        )
    else:
        text = replace_exact(
            text,
            "#endif\n\n    const bool profileZeroStage = generationCount == 0\n",
            "#endif\n\n"
            "    if (profileGenerated)\n"
            "        data.generatedPreQueryPool.writeAtStage(\n"
            "            data.cmdBuffer1.handle(), 1, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);\n\n"
            "    const bool profileZeroStage = generationCount == 0\n",
            count=1,
            label=f"{path}: input transport timestamp",
        )

    fixed_indent = "        " if "adaptiveFlowScales_" in text else "    "
    mipmap_tail = (
        f"{fixed_indent}this->mipmaps.Dispatch(\n"
        f"{fixed_indent}    data.cmdBuffer1, this->frameIdx,\n"
        f"{fixed_indent}    profileZeroStage ? &this->zeroStageQueryPool : nullptr, 1);\n"
        f"{fixed_indent}if (profileZeroStage)\n"
        f"{fixed_indent}    this->zeroStageQueryPool.write(data.cmdBuffer1.handle(), 2);\n"
    )
    mipmap_tail_new = (
        mipmap_tail
        + f"{fixed_indent}if (profileGenerated)\n"
        f"{fixed_indent}    data.generatedPreQueryPool.write(data.cmdBuffer1.handle(), 2);\n"
    )
    text = replace_exact(
        text, mipmap_tail, mipmap_tail_new, count=1,
        label=f"{path}: mipmaps generated timestamp",
    )

    alpha_beta_old = (
        f"{fixed_indent}for (size_t i = 0; i < 7; i++) {{\n"
        f"{fixed_indent}    this->alpha.at(6 - i).Dispatch(data.cmdBuffer1, this->frameIdx);\n"
        f"{fixed_indent}    if (profileZeroStage)\n"
        f"{fixed_indent}        this->zeroStageQueryPool.write(\n"
        f"{fixed_indent}            data.cmdBuffer1.handle(), static_cast<uint32_t>(i + 3));\n"
        f"{fixed_indent}}}\n"
        f"{fixed_indent}if (generationCount > 0)\n"
        f"{fixed_indent}    this->beta.Dispatch(data.cmdBuffer1, this->frameIdx);\n"
    )
    alpha_beta_new = (
        f"{fixed_indent}for (size_t i = 0; i < 7; i++) {{\n"
        f"{fixed_indent}    this->alpha.at(6 - i).Dispatch(data.cmdBuffer1, this->frameIdx);\n"
        f"{fixed_indent}    if (profileZeroStage)\n"
        f"{fixed_indent}        this->zeroStageQueryPool.write(\n"
        f"{fixed_indent}            data.cmdBuffer1.handle(), static_cast<uint32_t>(i + 3));\n"
        f"{fixed_indent}}}\n"
        f"{fixed_indent}if (profileGenerated)\n"
        f"{fixed_indent}    data.generatedPreQueryPool.write(data.cmdBuffer1.handle(), 3);\n"
        f"{fixed_indent}if (generationCount > 0) {{\n"
        f"{fixed_indent}    this->beta.Dispatch(data.cmdBuffer1, this->frameIdx);\n"
        f"{fixed_indent}    if (profileGenerated)\n"
        f"{fixed_indent}        data.generatedPreQueryPool.write(data.cmdBuffer1.handle(), 4);\n"
        f"{fixed_indent}}}\n"
    )
    text = replace_exact(
        text, alpha_beta_old, alpha_beta_new, count=1,
        label=f"{path}: alpha/beta generated timestamps",
    )

    text = replace_exact(
        text,
        "        auto& buf2 = data.cmdBuffers2.at(pass);\n"
        "        buf2 = Core::CommandBuffer(vk.device, vk.commandPool);\n"
        "        buf2.begin();\n\n",
        "        auto& buf2 = data.cmdBuffers2.at(pass);\n"
        "        buf2 = Core::CommandBuffer(vk.device, vk.commandPool);\n"
        "        buf2.begin();\n"
        "        auto* generatedPassProfile = profileGenerated\n"
        "            ? &data.generatedPassQueryPools.at(pass) : nullptr;\n"
        "        uint32_t generatedPassQueryIndex = 1;\n"
        "        if (generatedPassProfile != nullptr) {\n"
        "            generatedPassProfile->reset(buf2.handle());\n"
        "            generatedPassProfile->write(buf2.handle(), 0);\n"
        "        }\n\n",
        count=1,
        label=f"{path}: generated pass profiling start",
    )

    if "adaptiveFlowScales_" in text:
        pass_old = (
            "        } else {\n"
            "            for (size_t i = 0; i < 7; i++) {\n"
            "                this->gamma.at(i).Dispatch(buf2, this->frameIdx, pass, generationCount);\n"
            "                if (i >= 4)\n"
            "                    this->delta.at(i - 4).Dispatch(buf2, this->frameIdx, pass, generationCount);\n"
            "            }\n"
            "            this->generate.Dispatch(buf2, this->frameIdx, pass, generationCount);\n"
            "        }\n"
        )
        pass_new = (
            "        } else {\n"
            "            for (size_t i = 0; i < 7; i++) {\n"
            "                this->gamma.at(i).Dispatch(buf2, this->frameIdx, pass, generationCount);\n"
            "                if (generatedPassProfile != nullptr)\n"
            "                    generatedPassProfile->write(buf2.handle(), generatedPassQueryIndex++);\n"
            "                if (i >= 4) {\n"
            "                    this->delta.at(i - 4).Dispatch(buf2, this->frameIdx, pass, generationCount);\n"
            "                    if (generatedPassProfile != nullptr)\n"
            "                        generatedPassProfile->write(buf2.handle(), generatedPassQueryIndex++);\n"
            "                }\n"
            "            }\n"
            "            this->generate.Dispatch(buf2, this->frameIdx, pass, generationCount);\n"
            "            if (generatedPassProfile != nullptr)\n"
            "                generatedPassProfile->write(buf2.handle(), generatedPassQueryIndex++);\n"
            "        }\n"
        )
    else:
        pass_old = (
            "        for (size_t i = 0; i < 7; i++) {\n"
            "            this->gamma.at(i).Dispatch(buf2, this->frameIdx, pass, generationCount);\n"
            "            if (i >= 4)\n"
            "                this->delta.at(i - 4).Dispatch(buf2, this->frameIdx, pass, generationCount);\n"
            "        }\n"
            "        this->generate.Dispatch(buf2, this->frameIdx, pass, generationCount);\n"
        )
        pass_new = (
            "        for (size_t i = 0; i < 7; i++) {\n"
            "            this->gamma.at(i).Dispatch(buf2, this->frameIdx, pass, generationCount);\n"
            "            if (generatedPassProfile != nullptr)\n"
            "                generatedPassProfile->write(buf2.handle(), generatedPassQueryIndex++);\n"
            "            if (i >= 4) {\n"
            "                this->delta.at(i - 4).Dispatch(buf2, this->frameIdx, pass, generationCount);\n"
            "                if (generatedPassProfile != nullptr)\n"
            "                    generatedPassProfile->write(buf2.handle(), generatedPassQueryIndex++);\n"
            "            }\n"
            "        }\n"
            "        this->generate.Dispatch(buf2, this->frameIdx, pass, generationCount);\n"
            "        if (generatedPassProfile != nullptr)\n"
            "            generatedPassProfile->write(buf2.handle(), generatedPassQueryIndex++);\n"
        )
    text = replace_exact(
        text, pass_old, pass_new, count=1,
        label=f"{path}: generated shader stage timestamps",
    )

    text = replace_exact(
        text,
        "#endif\n\n        buf2.end();\n",
        "#endif\n\n"
        "        if (generatedPassProfile != nullptr)\n"
        "            generatedPassProfile->writeAtStage(\n"
        "                buf2.handle(), generatedPassQueryIndex++, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);\n"
        "        buf2.end();\n",
        count=1,
        label=f"{path}: output transport timestamp",
    )

    text = replace_exact(
        text,
        "    }\n\n    this->frameIdx++;\n}\n\nbool Context::waitForLastPresent",
        "    }\n\n"
        "    data.generatedProfilePending = profileGenerated;\n"
        "    data.generatedProfileGenerationCount = generationCount;\n"
        "    this->frameIdx++;\n"
        "}\n\n"
        "bool Context::waitForLastPresent",
        count=1,
        label=f"{path}: generated profile pending state",
    )

    path.write_text(text, encoding="utf-8")

                tainted = tainted || nodes[dependency].tainted;
            nodes[i].tainted = tainted;
        }

        std::vector<size_t> b9SampleOrder;
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].sample)
                b9SampleOrder.push_back(i);
        }

        std::vector<bool> scheduled(nodes.size(), false);
        std::vector<bool> visiting(nodes.size(), false);
        std::vector<size_t> order;
        order.reserve(nodes.size());

        for (const size_t sample : b9SampleOrder) {
            for (const size_t dependency : nodes[sample].dependencies) {
                if (!scheduled[dependency]
                        && !b9EmitPrerequisites(dependency, sample,
                            nodes, scheduled, visiting, order))
                    return false;
            }
            if (!scheduled[sample]) {
                scheduled[sample] = true;
                order.push_back(sample);
            }

            bool progress = true;
            while (progress) {
                progress = false;
                for (size_t i = 0; i < nodes.size(); ++i) {
                    if (scheduled[i] || nodes[i].sample || !nodes[i].tainted)
                        continue;
                    if (!b9Ready(nodes[i], scheduled))
                        continue;
                    scheduled[i] = true;
                    order.push_back(i);
                    progress = true;
                }
            }
        }

        while (order.size() < nodes.size()) {
            bool progress = false;
            for (size_t i = 0; i < nodes.size(); ++i) {
                if (scheduled[i] || !b9Ready(nodes[i], scheduled))
                    continue;
                scheduled[i] = true;
                order.push_back(i);
                progress = true;
                break;
            }
            if (!progress)
                return false;
        }

        for (size_t newIndex = 0; newIndex < order.size(); ++newIndex) {
            const size_t oldIndex = order[newIndex];
            if (oldIndex != newIndex)
                ++movedInstructions;
            b9AppendInstruction(out, bytecode, nodes[oldIndex].instruction);
        }
        streamedSamples += b9SampleOrder.size();
        return true;
    }

    bool applyCandidateB9Beta4SpillCollapse(
            const std::string& shaderName,
            std::vector<uint8_t>& bytecode) {
        if (shaderName != "p_beta[4]")
            return false;

        std::vector<B9Instruction> instructions;
        if (!b9Parse(bytecode, instructions)
                || !b9Beta4Baseline(bytecode, instructions)) {
            std::cerr << "lsfg-vk: candidate-b9-beta4-spill-collapse shader="
                << shaderName << " applied=0 reason=baseline"
                << " bytes=" << bytecode.size() << std::endl;
            return false;
        }

        const B9Inventory beforeInventory = b9Inventory(bytecode, instructions);

        uint32_t entryPointId = 0U;
        for (const auto& instruction : instructions) {
            if (instruction.opCode == kB9OpEntryPoint && instruction.wordCount >= 3U) {
                entryPointId = b9ReadWord(bytecode, instruction.word + 2U);
                break;
            }
        }
        if (entryPointId == 0U) {
            std::cerr << "lsfg-vk: candidate-b9-beta4-spill-collapse shader="
                << shaderName << " applied=0 reason=entry-point" << std::endl;
            return false;
        }

        std::unordered_map<uint32_t, uint32_t> pointerStorage;
        std::unordered_map<uint32_t, uint32_t> pointerRoot;
        b9PointerMetadata(bytecode, instructions, pointerStorage, pointerRoot);

        std::vector<uint32_t> rewritten;
        rewritten.reserve(bytecode.size() / sizeof(uint32_t));
        for (size_t i = 0; i < 5U; ++i)
            rewritten.push_back(b9ReadWord(bytecode, i));

        std::vector<B9Instruction> segment;
        bool inEntryFunction = false;
        bool phase0 = true;
        size_t phase0Segments = 0U;
        size_t movedInstructions = 0U;
        size_t streamedSamples = 0U;

        const auto flushSegment = [&]() -> bool {
            if (segment.empty())
                return true;
            ++phase0Segments;
            const bool ok = b9ScheduleSegment(
                bytecode, segment, pointerStorage, pointerRoot,
                rewritten, movedInstructions, streamedSamples);
            segment.clear();
            return ok;
        };

        for (const auto& instruction : instructions) {
            if (instruction.opCode == kB9OpFunction && instruction.wordCount >= 3U) {
                if (!flushSegment())
                    return false;
                inEntryFunction =
                    b9ReadWord(bytecode, instruction.word + 2U) == entryPointId;
                b9AppendInstruction(rewritten, bytecode, instruction);
                continue;
            }

            if (instruction.opCode == kB9OpFunctionEnd) {
                if (!flushSegment())
                    return false;
                b9AppendInstruction(rewritten, bytecode, instruction);
                if (inEntryFunction)
                    inEntryFunction = false;
                continue;
            }

            if (!inEntryFunction || !phase0) {
                if (!flushSegment())
                    return false;
                b9AppendInstruction(rewritten, bytecode, instruction);
                continue;
            }

            if (instruction.opCode == kB9OpControlBarrier) {
                if (!flushSegment())
                    return false;
                b9AppendInstruction(rewritten, bytecode, instruction);
                phase0 = false;
                continue;
            }

            if (instruction.opCode == kB9OpLabel
                    || instruction.opCode == kB9OpSelectionMerge
                    || b9IsTerminator(instruction.opCode)
                    || !b9MovableInstruction(bytecode, instruction,
                        pointerStorage, pointerRoot)) {
                if (!flushSegment())
                    return false;
                b9AppendInstruction(rewritten, bytecode, instruction);
                continue;
            }

            segment.push_back(instruction);
        }

        if (!flushSegment())
            return false;

        if (streamedSamples != 18U || movedInstructions == 0U) {
            std::cerr << "lsfg-vk: candidate-b9-beta4-spill-collapse shader="
                << shaderName << " applied=0 reason="
                << (streamedSamples != 18U ? "phase0-sample-count" : "no-reorder")
                << " streamed_samples=" << streamedSamples
                << " moved=" << movedInstructions << std::endl;
            return false;
        }

        std::vector<uint8_t> candidate(rewritten.size() * sizeof(uint32_t));
        std::memcpy(candidate.data(), rewritten.data(), candidate.size());

        std::vector<B9Instruction> candidateInstructions;
        if (candidate.size() != bytecode.size()
                || b9ReadWord(candidate, 3U) != kB9ExpectedBound
                || !b9Parse(candidate, candidateInstructions)
                || !b9Beta4Baseline(candidate, candidateInstructions)
                || !b9SameInventory(beforeInventory,
                    b9Inventory(candidate, candidateInstructions))) {
            std::cerr << "lsfg-vk: candidate-b9-beta4-spill-collapse shader="
                << shaderName << " applied=0 reason=post-verify" << std::endl;
            return false;
        }

        bytecode.swap(candidate);
        std::cerr << "lsfg-vk: candidate-b9-beta4-spill-collapse shader="
            << shaderName
            << " applied=1"
            << " phase0=streaming-ssa"
            << " segments=" << phase0Segments
            << " streamed_samples=" << streamedSamples
            << " moved=" << movedInstructions
            << " samples=18 writes=6 barriers=5"
            << " local_size=32x32x1"
            << " bytes=" << bytecode.size()
            << " bound=" << b9ReadWord(bytecode, 3U)
            << std::endl;
        return true;
    }
}


                const auto rootIt = pointerRoot.find(baseId);
                if (rootIt != pointerRoot.end())
                    pointerRoot[resultId] = rootIt->second;
                continue;
            }

            if (instruction.opCode == kB9OpCopyObject && instruction.wordCount >= 4U) {
                const uint32_t resultType = b9ReadWord(bytecode, instruction.word + 1U);
                const uint32_t resultId = b9ReadWord(bytecode, instruction.word + 2U);
                const uint32_t sourceId = b9ReadWord(bytecode, instruction.word + 3U);
                const auto typeIt = pointerTypeStorage.find(resultType);
                if (typeIt != pointerTypeStorage.end()) {
                    pointerStorage[resultId] = typeIt->second;
                    const auto rootIt = pointerRoot.find(sourceId);
                    if (rootIt != pointerRoot.end())
                        pointerRoot[resultId] = rootIt->second;
                }
            }
        }
    }

    uint32_t b9FunctionPointerRoot(const std::vector<uint8_t>& bytecode,
            const B9Instruction& instruction,
            const std::unordered_map<uint32_t, uint32_t>& pointerStorage,
            const std::unordered_map<uint32_t, uint32_t>& pointerRoot) {
        uint32_t pointerId = 0U;
        if (instruction.opCode == kB9OpLoad && instruction.wordCount >= 4U)
            pointerId = b9ReadWord(bytecode, instruction.word + 3U);
        else if (instruction.opCode == kB9OpStore && instruction.wordCount >= 3U)
            pointerId = b9ReadWord(bytecode, instruction.word + 1U);
        else
            return 0U;

        const auto storageIt = pointerStorage.find(pointerId);
        if (storageIt == pointerStorage.end()
                || storageIt->second != kB9StorageFunction)
            return 0U;
        const auto rootIt = pointerRoot.find(pointerId);
        return rootIt == pointerRoot.end() ? 0U : rootIt->second;
    }

    bool b9MovableInstruction(const std::vector<uint8_t>& bytecode,
            const B9Instruction& instruction,
            const std::unordered_map<uint32_t, uint32_t>& pointerStorage,
            const std::unordered_map<uint32_t, uint32_t>& pointerRoot) {
        if (instruction.opCode == kB9OpExtInst) {
            return instruction.wordCount >= 5U
                && b9ReadWord(bytecode, instruction.word + 3U) == 1U;
        }

        if (instruction.opCode == kB9OpLoad && instruction.wordCount >= 4U) {
            const uint32_t pointerId = b9ReadWord(bytecode, instruction.word + 3U);
            const auto storageIt = pointerStorage.find(pointerId);
            if (storageIt == pointerStorage.end())
                return false;
            if (b9ReadOnlyStorage(storageIt->second))
                return true;
            return storageIt->second == kB9StorageFunction
                && b9FunctionPointerRoot(bytecode, instruction,
                    pointerStorage, pointerRoot) != 0U;
        }

        if (instruction.opCode == kB9OpStore) {
            return b9FunctionPointerRoot(bytecode, instruction,
                pointerStorage, pointerRoot) != 0U;
        }

        if (instruction.opCode == 68U) // OpArrayLength reads storage memory.
            return false;

        return b9PureResultOp(instruction.opCode);
    }

    bool b9Beta4Baseline(const std::vector<uint8_t>& bytecode,
            const std::vector<B9Instruction>& instructions) {
        if (bytecode.size() != kB9ExpectedBytes
                || b9ReadWord(bytecode, 3U) != kB9ExpectedBound)
            return false;

        size_t samples = 0U;
        size_t writes = 0U;
        size_t barriers = 0U;
        size_t local32 = 0U;
        size_t predicates = 0U;

        for (size_t i = 0; i < instructions.size(); ++i) {
            const auto& instruction = instructions[i];
            if (instruction.opCode == kB9OpImageSampleExplicitLod)
                ++samples;
            else if (instruction.opCode == kB9OpImageWrite)
                ++writes;
            else if (instruction.opCode == kB9OpControlBarrier)
                ++barriers;
            else if (instruction.opCode == kB9OpExecutionMode
                    && instruction.wordCount == 6U
                    && b9ReadWord(bytecode, instruction.word + 2U) == kB9ExecutionModeLocalSize
                    && b9ReadWord(bytecode, instruction.word + 3U) == 32U
                    && b9ReadWord(bytecode, instruction.word + 4U) == 32U
                    && b9ReadWord(bytecode, instruction.word + 5U) == 1U)
                ++local32;

            if (instruction.opCode == kB9OpSelectionMerge
                    && instruction.wordCount == 3U
                    && predicates < kB9MergeLabels.size()
                    && b9ReadWord(bytecode, instruction.word + 1U)
                        == kB9MergeLabels[predicates]) {
                if (i + 1U >= instructions.size())
                    return false;
                const auto& branch = instructions[i + 1U];
                if (branch.opCode != kB9OpBranchConditional
                        || branch.wordCount != 4U
                        || b9ReadWord(bytecode, branch.word + 1U)
                            != kB9PredicateConditions[predicates])
                    return false;
                ++predicates;
            }
        }

        return samples == 18U
            && writes == 6U
            && barriers == 5U
            && local32 == 1U
            && predicates == kB9PredicateConditions.size();
    }

    bool b9Ready(const B9Node& node, const std::vector<bool>& scheduled) {
        for (const size_t dependency : node.dependencies) {
            if (!scheduled.at(dependency))
                return false;
        }
        return true;
    }

    bool b9EmitPrerequisites(size_t index, size_t targetSample,
            const std::vector<B9Node>& nodes,
            std::vector<bool>& scheduled,
            std::vector<bool>& visiting,
            std::vector<size_t>& order) {
        if (scheduled.at(index))
            return true;
        if (visiting.at(index))
            return false;
        if (nodes.at(index).sample && index != targetSample)
            return false;

        visiting[index] = true;
        for (const size_t dependency : nodes.at(index).dependencies) {
            if (!b9EmitPrerequisites(dependency, targetSample,
                    nodes, scheduled, visiting, order))
                return false;
        }
        visiting[index] = false;
        scheduled[index] = true;
        order.push_back(index);
        return true;
    }

    bool b9ScheduleSegment(const std::vector<uint8_t>& bytecode,
            const std::vector<B9Instruction>& segment,
            const std::unordered_map<uint32_t, uint32_t>& pointerStorage,
            const std::unordered_map<uint32_t, uint32_t>& pointerRoot,
            std::vector<uint32_t>& out,
            size_t& movedInstructions,
            size_t& streamedSamples) {
        if (segment.empty())
            return true;

        std::vector<B9Node> nodes;
        nodes.reserve(segment.size());
        std::unordered_map<uint32_t, size_t> producerById;
        std::unordered_map<uint32_t, size_t> lastFunctionMemory;
        size_t lastSample = static_cast<size_t>(-1);

        for (const auto& instruction : segment) {
            B9Node node;
            node.instruction = instruction;
            node.resultId = b9ResultId(bytecode, instruction);
            node.sample = instruction.opCode == kB9OpImageSampleExplicitLod;
            node.memoryRoot = b9FunctionPointerRoot(
                bytecode, instruction, pointerStorage, pointerRoot);
            node.functionMemory = node.memoryRoot != 0U;

            const uint32_t operandStart = b9OperandStart(instruction);
            for (uint32_t operand = operandStart;
                    operand < instruction.wordCount; ++operand) {
                const uint32_t candidate =
                    b9ReadWord(bytecode, instruction.word + operand);
                const auto producer = producerById.find(candidate);
                if (producer != producerById.end()
                        && std::find(node.dependencies.begin(), node.dependencies.end(),
                            producer->second) == node.dependencies.end())
                    node.dependencies.push_back(producer->second);
            }

            if (node.sample && lastSample != static_cast<size_t>(-1)
                    && std::find(node.dependencies.begin(), node.dependencies.end(),
                        lastSample) == node.dependencies.end())
                node.dependencies.push_back(lastSample);

            if (node.functionMemory) {
                const auto previous = lastFunctionMemory.find(node.memoryRoot);
                if (previous != lastFunctionMemory.end()
                        && std::find(node.dependencies.begin(), node.dependencies.end(),
                            previous->second) == node.dependencies.end())
                    node.dependencies.push_back(previous->second);
            }

            const size_t index = nodes.size();
            nodes.push_back(std::move(node));
            if (nodes.back().resultId != 0U)
                producerById[nodes.back().resultId] = index;
            if (nodes.back().sample)
                lastSample = index;
            if (nodes.back().functionMemory)
                lastFunctionMemory[nodes.back().memoryRoot] = index;
        }

        for (size_t i = 0; i < nodes.size(); ++i) {
            bool tainted = nodes[i].sample;
            for (const size_t dependency : nodes[i].dependencies)

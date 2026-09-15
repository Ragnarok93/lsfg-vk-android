namespace {
    constexpr size_t kB9ExpectedBytes = 50764U;
    constexpr uint32_t kB9ExpectedBound = 2351U;
    constexpr uint32_t kB9SpirvMagic = 0x07230203U;

    constexpr uint16_t kB9OpExtInst = 12U;
    constexpr uint16_t kB9OpEntryPoint = 15U;
    constexpr uint16_t kB9OpExecutionMode = 16U;
    constexpr uint16_t kB9OpTypePointer = 32U;
    constexpr uint16_t kB9OpFunction = 54U;
    constexpr uint16_t kB9OpFunctionEnd = 56U;
    constexpr uint16_t kB9OpVariable = 59U;
    constexpr uint16_t kB9OpLoad = 61U;
    constexpr uint16_t kB9OpStore = 62U;
    constexpr uint16_t kB9OpAccessChain = 65U;
    constexpr uint16_t kB9OpInBoundsAccessChain = 66U;
    constexpr uint16_t kB9OpPtrAccessChain = 67U;
    constexpr uint16_t kB9OpInBoundsPtrAccessChain = 70U;
    constexpr uint16_t kB9OpCopyObject = 83U;
    constexpr uint16_t kB9OpImageSampleExplicitLod =
        static_cast<uint16_t>(spv::OpImageSampleExplicitLod);
    constexpr uint16_t kB9OpImageWrite = static_cast<uint16_t>(spv::OpImageWrite);
    constexpr uint16_t kB9OpControlBarrier = static_cast<uint16_t>(spv::OpControlBarrier);
    constexpr uint16_t kB9OpSelectionMerge = 247U;
    constexpr uint16_t kB9OpLabel = 248U;
    constexpr uint16_t kB9OpBranch = 249U;
    constexpr uint16_t kB9OpBranchConditional = 250U;
    constexpr uint16_t kB9OpSwitch = 251U;
    constexpr uint16_t kB9OpKill = 252U;
    constexpr uint16_t kB9OpReturn = 253U;
    constexpr uint16_t kB9OpReturnValue = 254U;
    constexpr uint16_t kB9OpUnreachable = 255U;

    constexpr uint32_t kB9ExecutionModeLocalSize = 17U;
    constexpr uint32_t kB9StorageUniformConstant = 0U;
    constexpr uint32_t kB9StorageInput = 1U;
    constexpr uint32_t kB9StorageUniform = 2U;
    constexpr uint32_t kB9StorageWorkgroup = 4U;
    constexpr uint32_t kB9StorageFunction = 7U;
    constexpr uint32_t kB9StoragePushConstant = 9U;

    constexpr std::array<uint32_t, 5> kB9MergeLabels{
        1082U, 1355U, 1619U, 1882U, 2145U,
    };
    constexpr std::array<uint32_t, 5> kB9PredicateConditions{
        2318U, 2326U, 2334U, 2342U, 2350U,
    };

    struct B9Instruction {
        size_t word{};
        uint16_t wordCount{};
        uint16_t opCode{};
        size_t ordinal{};
    };

    struct B9Node {
        B9Instruction instruction{};
        uint32_t resultId{};
        uint32_t memoryRoot{};
        bool sample{};
        bool functionMemory{};
        bool tainted{};
        std::vector<size_t> dependencies;
    };

    struct B9Inventory {
        size_t instructions{};
        uint64_t hashXor{};
        uint64_t hashSum{};
    };

    uint32_t b9ReadWord(const std::vector<uint8_t>& bytecode, size_t wordIndex) {
        uint32_t word = 0;
        std::memcpy(&word, bytecode.data() + wordIndex * sizeof(uint32_t), sizeof(uint32_t));
        return word;
    }

    void b9AppendInstruction(std::vector<uint32_t>& out,
            const std::vector<uint8_t>& bytecode, const B9Instruction& instruction) {
        for (size_t i = 0; i < instruction.wordCount; ++i)
            out.push_back(b9ReadWord(bytecode, instruction.word + i));
    }

    bool b9Parse(const std::vector<uint8_t>& bytecode,
            std::vector<B9Instruction>& instructions) {
        if (bytecode.size() < 5U * sizeof(uint32_t)
                || bytecode.size() % sizeof(uint32_t) != 0U
                || b9ReadWord(bytecode, 0) != kB9SpirvMagic)
            return false;

        const size_t words = bytecode.size() / sizeof(uint32_t);
        instructions.clear();
        size_t ordinal = 0;
        for (size_t cursor = 5; cursor < words;) {
            const uint32_t first = b9ReadWord(bytecode, cursor);
            const uint16_t wc = static_cast<uint16_t>(first >> 16U);
            const uint16_t op = static_cast<uint16_t>(first & 0xffffU);
            if (wc == 0U || cursor + wc > words)
                return false;
            instructions.push_back({cursor, wc, op, ordinal++});
            cursor += wc;
        }
        return true;
    }

    uint64_t b9InstructionHash(const std::vector<uint8_t>& bytecode,
            const B9Instruction& instruction) {
        uint64_t hash = 1469598103934665603ULL;
        for (size_t i = 0; i < instruction.wordCount; ++i) {
            const uint32_t word = b9ReadWord(bytecode, instruction.word + i);
            for (unsigned shift = 0; shift < 32U; shift += 8U) {
                hash ^= static_cast<uint8_t>((word >> shift) & 0xffU);
                hash *= 1099511628211ULL;
            }
        }
        return hash;
    }

    B9Inventory b9Inventory(const std::vector<uint8_t>& bytecode,
            const std::vector<B9Instruction>& instructions) {
        B9Inventory inventory;
        for (const auto& instruction : instructions) {
            const uint64_t hash = b9InstructionHash(bytecode, instruction);
            ++inventory.instructions;
            inventory.hashXor ^= hash;
            inventory.hashSum += hash * 0x9e3779b185ebca87ULL;
        }
        return inventory;
    }

    bool b9SameInventory(const B9Inventory& a, const B9Inventory& b) {
        return a.instructions == b.instructions
            && a.hashXor == b.hashXor
            && a.hashSum == b.hashSum;
    }

    bool b9IsTerminator(uint16_t opCode) {
        return opCode == kB9OpBranch
            || opCode == kB9OpBranchConditional
            || opCode == kB9OpSwitch
            || opCode == kB9OpKill
            || opCode == kB9OpReturn
            || opCode == kB9OpReturnValue
            || opCode == kB9OpUnreachable;
    }

    bool b9ReadOnlyStorage(uint32_t storageClass) {
        return storageClass == kB9StorageUniformConstant
            || storageClass == kB9StorageInput
            || storageClass == kB9StorageUniform
            || storageClass == kB9StoragePushConstant;
    }

    bool b9PureResultOp(uint16_t opCode) {
        if (opCode >= 65U && opCode <= 67U) return true;
        if (opCode >= 70U && opCode <= 84U) return true;
        if (opCode >= 86U && opCode <= 97U) return true;
        if (opCode >= 100U && opCode <= 106U) return true;
        if (opCode >= 109U && opCode <= 199U) return true;
        return false;
    }

    uint32_t b9ResultId(const std::vector<uint8_t>& bytecode,
            const B9Instruction& instruction) {
        if (instruction.wordCount < 3U)
            return 0U;
        if (instruction.opCode == kB9OpLoad
                || instruction.opCode == kB9OpExtInst
                || b9PureResultOp(instruction.opCode))
            return b9ReadWord(bytecode, instruction.word + 2U);
        return 0U;
    }

    uint32_t b9OperandStart(const B9Instruction& instruction) {
        if (instruction.opCode == kB9OpStore)
            return 1U;
        return 3U;
    }

    void b9PointerMetadata(const std::vector<uint8_t>& bytecode,
            const std::vector<B9Instruction>& instructions,
            std::unordered_map<uint32_t, uint32_t>& pointerStorage,
            std::unordered_map<uint32_t, uint32_t>& pointerRoot) {
        std::unordered_map<uint32_t, uint32_t> pointerTypeStorage;
        pointerStorage.clear();
        pointerRoot.clear();

        for (const auto& instruction : instructions) {
            if (instruction.opCode == kB9OpTypePointer && instruction.wordCount >= 4U) {
                pointerTypeStorage[b9ReadWord(bytecode, instruction.word + 1U)] =
                    b9ReadWord(bytecode, instruction.word + 2U);
                continue;
            }

            if (instruction.opCode == kB9OpVariable && instruction.wordCount >= 4U) {
                const uint32_t resultType = b9ReadWord(bytecode, instruction.word + 1U);
                const uint32_t resultId = b9ReadWord(bytecode, instruction.word + 2U);
                const uint32_t storage = b9ReadWord(bytecode, instruction.word + 3U);
                pointerStorage[resultId] = storage;
                if (storage == kB9StorageFunction)
                    pointerRoot[resultId] = resultId;
                else {
                    const auto typeIt = pointerTypeStorage.find(resultType);
                    if (typeIt != pointerTypeStorage.end())
                        pointerStorage[resultId] = typeIt->second;
                }
                continue;
            }

            const bool accessChain = instruction.opCode == kB9OpAccessChain
                || instruction.opCode == kB9OpInBoundsAccessChain
                || instruction.opCode == kB9OpPtrAccessChain
                || instruction.opCode == kB9OpInBoundsPtrAccessChain;
            if (accessChain && instruction.wordCount >= 4U) {
                const uint32_t resultType = b9ReadWord(bytecode, instruction.word + 1U);
                const uint32_t resultId = b9ReadWord(bytecode, instruction.word + 2U);
                const uint32_t baseId = b9ReadWord(bytecode, instruction.word + 3U);
                const auto typeIt = pointerTypeStorage.find(resultType);
                if (typeIt != pointerTypeStorage.end())
                    pointerStorage[resultId] = typeIt->second;

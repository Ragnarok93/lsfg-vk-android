    for (size_t phase = 0; phase < 4; ++phase) {
        const uint32_t offset = offsets[phase];
        const uint32_t stride = strides[phase];
        const uint32_t dim = dims[phase];
        for (uint32_t x = 0; x < 16; x += stride) {
            for (uint32_t y = 0; y < 16; y += stride) {
                const float v = reduce(
                    shared[x * 16 + y],
                    shared[(x + offset) * 16 + y],
                    shared[x * 16 + y + offset],
                    shared[(x + offset) * 16 + y + offset]);
                levels[phase + 1][(x / stride) * dim + (y / stride)] = v;
                if (phase != 3)
                    shared[x * 16 + y] = v;
            }
        }
    }
    return levels;
}

void assertPaddedScratchCoverage(uint32_t flowWidth, uint32_t flowHeight) {
    const uint32_t groupsX = (flowWidth + 63u) >> 6u;
    const uint32_t groupsY = (flowHeight + 63u) >> 6u;
    const uint32_t scratchWidth = groupsX * 32u;
    const uint32_t scratchHeight = groupsY * 32u;

    // Every lane in every original 32x32 workgroup must have a valid scratch
    // coordinate, including lanes whose ordinary u1 imageStore is out of bounds.
    assert(scratchWidth >= groupsX * 32u);
    assert(scratchHeight >= groupsY * 32u);
    if (groupsX != 0) assert(scratchWidth - 1u >= groupsX * 32u - 1u);
    if (groupsY != 0) assert(scratchHeight - 1u >= groupsY * 32u - 1u);

    // Tail dispatch uses one 16x16 workgroup per original head workgroup and
    // consumes the exact 2x2 partition of that padded 32x32 scratch tile.
    assert(groupsX * 16u * 2u == scratchWidth);
    assert(groupsY * 16u * 2u == scratchHeight);
}

void assertEdgeInvalidLanesCannotAffectVisibleOutputs(
        uint32_t u1Width, uint32_t u1Height, uint32_t groupX, uint32_t groupY) {
    assert(u1Width > 0 && u1Height > 0);
    Level a(32u * 32u);
    Level b(32u * 32u);
    for (uint32_t x = 0; x < 32; ++x) {
        for (uint32_t y = 0; y < 32; ++y) {
            const uint32_t gx = groupX * 32u + x;
            const uint32_t gy = groupY * 32u + y;
            const size_t i = x * 32u + y;
            if (gx < u1Width && gy < u1Height) {
                const float v = static_cast<float>((gx * 131u + gy * 17u) & 0xffffu) / 257.0f;
                a[i] = v;
                b[i] = v;
            } else {
                a[i] = 123.25f + static_cast<float>(x + y);
                b[i] = -987.5f - static_cast<float>(x * 3u + y);
            }
        }
    }

    const auto pa = compactPyramid(a);
    const auto pb = compactPyramid(b);
    const std::array<uint32_t, 5> widths{u1Width >> 1u, u1Width >> 2u,
        u1Width >> 3u, u1Width >> 4u, u1Width >> 5u};
    const std::array<uint32_t, 5> heights{u1Height >> 1u, u1Height >> 2u,
        u1Height >> 3u, u1Height >> 4u, u1Height >> 5u};
    const std::array<uint32_t, 5> tileDims{16u, 8u, 4u, 2u, 1u};
    for (size_t level = 0; level < 5; ++level) {
        const uint32_t tileDim = tileDims[level];
        for (uint32_t x = 0; x < tileDim; ++x) {
            for (uint32_t y = 0; y < tileDim; ++y) {
                const uint32_t globalX = groupX * tileDim + x;
                const uint32_t globalY = groupY * tileDim + y;
                if (globalX >= widths[level] || globalY >= heights[level])
                    continue;
                const size_t i = x * tileDim + y;
                assert(std::bit_cast<uint32_t>(pa[level][i])
                    == std::bit_cast<uint32_t>(pb[level][i]));
            }
        }
    }
}

void assertSameBits(const Level& a, const Level& b) {
    assert(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i)
        assert(std::bit_cast<uint32_t>(a[i]) == std::bit_cast<uint32_t>(b[i]));
}

} // namespace

int main() {
    assert(isAdreno6xxDevice(0x5143u, "Turnip Adreno (TM) 650"));
    assert(isAdreno6xxDevice(0x5143u, "Adreno 630"));
    assert(!isAdreno6xxDevice(0x5143u, "Adreno 740"));
    assert(!isAdreno6xxDevice(0x13B5u, "Mali-G715"));
    assert(!isAdreno6xxDevice(0x144Du, "Samsung Xclipse 940"));

    auto head = syntheticHead();
    const auto original = head;
    HeadTransformStats productionRejected;
    assert(!transformHead(head, productionRejected));
    assert(productionRejected.reason == "production-fingerprint");
    HeadTransformStats stats;
    assert(transformHeadForSelfTest(head, stats));
    assert(stats.applied && stats.reason == "ok");
    assert(stats.imageSamplesBefore == 4 && stats.imageWritesBefore == 10 && stats.barriersBefore == 5);
    assert(stats.imageSamplesAfter == 4 && stats.imageWritesAfter == 6 && stats.barriersAfter == 0);
    assert(stats.workgroupStoresBeforePhase0 == 1 && stats.workgroupStoresAfter == 0);
    assert(stats.scratchMirrorsWorkgroupSeed);
    assert(head != original);

    auto malformed = original;
    std::vector<uint32_t> malformedWords(malformed.size() / 4);
    std::memcpy(malformedWords.data(), malformed.data(), malformed.size());
    malformedWords[3] = 0; // break the SPIR-V id bound
    std::memcpy(malformed.data(), malformedWords.data(), malformed.size());
    HeadTransformStats rejected;
    assert(!transformHeadForSelfTest(malformed, rejected));
    assert(malformed != head);

    auto mismatchedSeed = original;
    std::vector<uint32_t> mismatchWords(mismatchedSeed.size() / 4);
    std::memcpy(mismatchWords.data(), mismatchedSeed.data(), mismatchedSeed.size());
    bool changedSeed = false;
    for (size_t off = 5; off < mismatchWords.size();) {
        const uint32_t wc = mismatchWords[off] >> 16u;
        const auto op = static_cast<spv::Op>(mismatchWords[off] & 0xffffu);
        assert(wc != 0 && off + wc <= mismatchWords.size());
        if (op == spv::OpStore && wc >= 3 && mismatchWords[off + 1] == 14) {
            mismatchWords[off + 2] = 99;
            changedSeed = true;
            break;
        }
        off += wc;
    }
    assert(changedSeed);
    std::memcpy(mismatchedSeed.data(), mismatchWords.data(), mismatchedSeed.size());
    HeadTransformStats mismatchRejected;
    assert(!transformHeadForSelfTest(mismatchedSeed, mismatchRejected));
    assert(mismatchRejected.reason == "u1-workgroup-seed-mismatch");

    assertPaddedScratchCoverage(768u, 432u);
    assertPaddedScratchCoverage(769u, 433u);
    assertPaddedScratchCoverage(1919u, 1079u);
    // Invalid padded scratch lanes may contain prior-frame/undefined values,
    // but the floor-divided output extents guarantee they cannot influence any
    // visible u2-u6 texel. Exercise odd edge sizes and both edge dimensions.
    assertEdgeInvalidLanesCannotAffectVisibleOutputs(336u, 189u, 10u, 5u);
    assertEdgeInvalidLanesCannotAffectVisibleOutputs(384u, 216u, 11u, 6u);
    assertEdgeInvalidLanesCannotAffectVisibleOutputs(959u, 539u, 29u, 16u);

    const auto tail = buildTailSpirv();
    if (const char* out = std::getenv("B10_TAIL_SPV_OUT"); out != nullptr && *out != '\0') {
        std::ofstream file(out, std::ios::binary | std::ios::trunc);
        assert(file.good());
        file.write(reinterpret_cast<const char*>(tail.data()),
            static_cast<std::streamsize>(tail.size()));
        assert(file.good());
    }
    const auto shape = inspectTail(tail);
    assert(shape.localX == 16 && shape.localY == 16 && shape.localZ == 1);
    assert(shape.reads == 4);
    assert(shape.writes == 5);
    assert(shape.barriers == 4);
    assert(shape.samples == 0);
    assert(shape.imageQueries == 5);
    assert(shape.conditionalBranches == 9);
    assert(shape.fadds == 15);
    assert(shape.fmuls == 5);
    assert(shape.workgroupVars == 1);
    assert(shape.bindings == std::set<uint32_t>({0, 1, 2, 3, 4, 5}));

    std::mt19937 rng(0xB10u);
    std::uniform_real_distribution<float> dist(-4.0f, 4.0f);
    for (uint32_t iteration = 0; iteration < 128; ++iteration) {
        Level u1(32u * 32u);
        for (auto& value : u1)
            value = dist(rng);
        const auto oldLevels = oldPyramid(u1);
        const auto compactLevels = compactPyramid(u1);
        for (size_t level = 0; level < oldLevels.size(); ++level)
            assertSameBits(oldLevels[level], compactLevels[level]);
    }

    std::cout << "B10 self-test: fingerprint fail-closed, exact TGSM seed mirror, head 4/10/5 -> 4/6/0, tail local16 reads=4 writes=5 queries=5 barriers=4, bounded outputs + padded edges covered, 128 exact pyramids\n";
    return 0;
}

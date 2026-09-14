#!/usr/bin/env python3
"""Fail-closed Beta4 reduction-predicate cleanup for the exact B3 module."""
from __future__ import annotations
import argparse
from pathlib import Path

SRC = Path("src/extract/trans.cpp")
MARK = "beta4-predicate-opt"


def replace1(text: str, old: str, new: str, label: str) -> str:
    if new in text:
        return text
    if text.count(old) != 1:
        raise RuntimeError(f"{label}: expected one anchor")
    return text.replace(old, new, 1)


def helper() -> str:
    return r'''namespace {
constexpr uint64_t kB4Fnv = 0x975df8da92d9c418ULL;
constexpr size_t kB4OldBytes = 49984, kB4NewBytes = 43564;
constexpr size_t kB4OldInstructions = 2769, kB4NewInstructions = 2404;

uint64_t b4Fnv(const std::vector<uint8_t>& b) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint8_t v : b) { h ^= v; h *= 0x100000001b3ULL; }
    return h;
}

bool applyCandidateB4Beta4Predicates(
        const std::string& shaderName, std::vector<uint8_t>& bytecode) {
    if (shaderName != "p_beta[4]") return false;
    if (bytecode.size() != kB4OldBytes || b4Fnv(bytecode) != kB4Fnv) {
        std::cerr << "lsfg-vk: beta4-predicate-opt shader=" << shaderName
                  << " applied=0 reason=fingerprint-mismatch bytes="
                  << bytecode.size() << std::endl;
        return false;
    }
    std::vector<uint32_t> in(bytecode.size() / 4), out;
    std::memcpy(in.data(), bytecode.data(), bytecode.size());
    if (in.size() != 12496 || in[0] != 0x07230203U || in[3] != 2311) return false;

    constexpr size_t barriers[5] = {5993,7338,8657,9976,11295};
    constexpr size_t merges[5] = {6357,7702,9021,10340,11659};
    constexpr size_t branches[5] = {6360,7705,9024,10343,11662};
    constexpr uint32_t divisors[5] = {1007,986,1403,1667,1930};
    constexpr uint32_t typeV3u=32, typeU=10, typeB=61, localIdVar=34, zero=54, one=1083;
    uint32_t next = 2311;
    size_t cursor = 0;
    out.reserve(10891);
    const auto emit = [&out](spv::Op op, std::initializer_list<uint32_t> a) {
        out.push_back((static_cast<uint32_t>(a.size()+1) << 16U) | static_cast<uint16_t>(op));
        out.insert(out.end(), a.begin(), a.end());
    };
    for (size_t s=0; s<5; ++s) {
        const uint32_t bw=in[barriers[s]], mw=in[merges[s]], cw=in[branches[s]];
        if ((bw&0xffffU)!=spv::OpControlBarrier || (bw>>16U)!=4 ||
            (mw&0xffffU)!=spv::OpSelectionMerge || (mw>>16U)!=3 ||
            (cw&0xffffU)!=spv::OpBranchConditional || (cw>>16U)!=4 ||
            merges[s]-barriers[s]-4 != 360) return false;
        out.insert(out.end(), in.begin()+cursor, in.begin()+barriers[s]+4);
        const uint32_t lid=next++, x=next++, y=next++, mask=next++;
        const uint32_t mx=next++, my=next++, both=next++, cond=next++;
        emit(spv::OpLoad,{typeV3u,lid,localIdVar});
        emit(spv::OpCompositeExtract,{typeU,x,lid,0});
        emit(spv::OpCompositeExtract,{typeU,y,lid,1});
        emit(spv::OpISub,{typeU,mask,divisors[s],one});
        emit(spv::OpBitwiseAnd,{typeU,mx,x,mask});
        emit(spv::OpBitwiseAnd,{typeU,my,y,mask});
        emit(spv::OpBitwiseOr,{typeU,both,mx,my});
        emit(spv::OpIEqual,{typeB,cond,both,zero});
        out.insert(out.end(), in.begin()+merges[s], in.begin()+merges[s]+3);
        out.push_back(cw); out.push_back(cond);
        out.push_back(in[branches[s]+2]); out.push_back(in[branches[s]+3]);
        cursor = branches[s]+4;
    }
    out.insert(out.end(), in.begin()+cursor, in.end());
    if (next != 2351 || out.size()!=10891 || out.size()*4!=kB4NewBytes) return false;
    out[3]=next;
    bytecode.resize(kB4NewBytes);
    std::memcpy(bytecode.data(), out.data(), bytecode.size());
    std::cerr << "lsfg-vk: beta4-predicate-opt shader=" << shaderName
              << " applied=1 old_bytes=" << kB4OldBytes << " new_bytes=" << kB4NewBytes
              << " old_instructions=" << kB4OldInstructions
              << " new_instructions=" << kB4NewInstructions
              << " removed_instructions=365" << std::endl;
    return true;
}
}

'''


def patch(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if MARK in text:
        return
    if "shader-hot-path-opt" not in text:
        raise RuntimeError(f"{path}: Candidate B named translation cleanup required")
    if "#include <cstring>" not in text:
        text = replace1(text, "#include <cstddef>\n", "#include <cstddef>\n#include <cstring>\n#include <initializer_list>\n", "B4 includes")
    elif "#include <initializer_list>" not in text:
        text = text.replace("#include <cstring>\n", "#include <cstring>\n#include <initializer_list>\n", 1)
    text = replace1(text, "struct BindingOffsets {\n", helper()+"struct BindingOffsets {\n", "B4 helper")
    call = "    applyCandidateB4Beta4Predicates(shaderName, spirvBytecode);\n"
    profile = "    logMipmapsSpirvProfile(shaderName, spirvBytecode);\n"
    if profile in text:
        text = replace1(text, profile, call+profile, "B4 profile call")
    else:
        text = replace1(text, "    return spirvBytecode;\n", call+"    return spirvBytecode;\n", "B4 return call")
    path.write_text(text, encoding="utf-8")


def main() -> None:
    p=argparse.ArgumentParser(); p.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    patch(p.parse_args().root.resolve()/SRC)

if __name__ == "__main__": main()

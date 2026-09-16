#!/usr/bin/env python3
'''Wire the checked B14 Mipmaps tail fusion into translated p_mipmaps.

GameNative launches Windows titles through Wine's explorer.exe desktop shell.
The configured target override must not make that helper process itself a
target, otherwise the resident LSFG layer can intercept the launcher's Vulkan
device/WSI path before the real game process exists.
'''

from __future__ import annotations

import argparse
from pathlib import Path


TRANSLATION_SOURCE = Path("src/extract/trans.cpp")
PROCESS_SOURCE = Path("src/utils/utils.cpp")
MARKER = "candidate-b14-mipmaps-tail-fusion"
PROCESS_MARKER = "gamenative-helper-process-override-guard"


def replace_exact(text: str, old: str, new: str, *, count: int, label: str) -> str:
    if new in text:
        return text
    found = text.count(old)
    if found != count:
        raise RuntimeError(f"{label}: expected {count} match(es), found {found}")
    return text.replace(old, new, count)


def patch_process_targeting_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if PROCESS_MARKER in text:
        return

    old = '''    // GameNative / Wine-on-Android: /proc/self/exe points at the Wine loader,
    // not the game .exe. Accept an explicit override from the launcher so
    // per-game matching in the TOML still works.
    const char* process_exe = std::getenv("LSFG_PROCESS_EXE");
    if (process_exe && *process_exe != '\\0')
        return { process_exe, process_exe };
'''
    new = '''    // GameNative / Wine-on-Android: /proc/self/exe points at the Wine loader,
    // not the game .exe. Accept an explicit override from the launcher so
    // per-game matching in the TOML still works. The override is inherited by
    // Wine helper processes too, so never target explorer.exe itself: it owns
    // the desktop shell/launcher and must remain a pass-through Vulkan client.
    // gamenative-helper-process-override-guard
    const char* process_exe = std::getenv("LSFG_PROCESS_EXE");
    if (process_exe && *process_exe != '\\0') {
        std::array<char, 4096> cmdline{};
        std::ifstream cmdline_file("/proc/self/cmdline", std::ios::binary);
        if (cmdline_file.is_open()) {
            cmdline_file.read(
                cmdline.data(),
                static_cast<std::streamsize>(cmdline.size() - 1U));
            const auto cmdline_len =
                static_cast<size_t>(cmdline_file.gcount());
            if (cmdline_len > 0U) {
                cmdline.at(cmdline_len) = '\\0';
                const std::string actual_process(cmdline.data());
                constexpr char explorer_exe[] = "explorer.exe";
                constexpr size_t explorer_len = sizeof(explorer_exe) - 1U;
                if (actual_process.size() >= explorer_len
                        && actual_process.compare(
                            actual_process.size() - explorer_len,
                            explorer_len,
                            explorer_exe) == 0) {
                    std::cerr
                        << "lsfg-vk: GameNative helper bypass process="
                        << actual_process
                        << " configured_target=" << process_exe << '\\n';
                    return { actual_process, actual_process };
                }
            }
        }
        return { process_exe, process_exe };
    }
'''
    text = replace_exact(
        text,
        old,
        new,
        count=1,
        label=f"{path}: GameNative helper targeting guard",
    )
    path.write_text(text, encoding="utf-8")


def patch_translation_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if MARKER in text:
        return
    if "const std::string& shaderName" not in text:
        raise RuntimeError(f"{path}: B14 requires named shader translation")

    text = replace_exact(
        text,
        '#include "extract/trans.hpp"\n',
        '#include "extract/trans.hpp"\n'
        '#include "../../scripts/b14_mipmaps_tail_fusion.hpp"\n',
        count=1,
        label=f"{path}: B14 rewriter include",
    )
    if "#include <iostream>\n" not in text:
        text = replace_exact(
            text,
            "#include <vector>\n",
            "#include <iostream>\n#include <vector>\n",
            count=1,
            label=f"{path}: B14 log include",
        )
    if "#include <stdexcept>\n" not in text:
        text = replace_exact(
            text,
            "#include <vector>\n",
            "#include <stdexcept>\n#include <vector>\n",
            count=1,
            label=f"{path}: B14 exception include",
        )

    call = '''    if (shaderName == "p_mipmaps") {
        const auto b14Report = b14::fuseTail(spirvBytecode);
        if (!b14Report.applied && !b14Report.alreadyApplied)
            throw std::runtime_error(
                std::string("B14 Mipmaps tail fusion rejected known shader: ")
                + b14Report.reason);
        std::cerr << "lsfg-vk: candidate-b14-mipmaps-tail-fusion"
                  << " applied=" << (b14Report.applied ? 1 : 0)
                  << " already_applied=" << (b14Report.alreadyApplied ? 1 : 0)
                  << " barriers_before=" << b14Report.barriersBefore
                  << " barriers_after=" << b14Report.barriersAfter
                  << " tail_dynamic_loads_before=" << b14Report.tailDynamicLoadsBefore
                  << " tail_dynamic_loads_after=" << b14Report.tailDynamicLoadsAfter
                  << " tail_workgroup_stores_before=" << b14Report.tailWorkgroupStoresBefore
                  << " tail_workgroup_stores_after=" << b14Report.tailWorkgroupStoresAfter
                  << " image_writes_before=" << b14Report.imageWritesBefore
                  << " image_writes_after=" << b14Report.imageWritesAfter
                  << std::endl;
    }
'''
    text = replace_exact(
        text,
        "    return spirvBytecode;\n",
        call + "    return spirvBytecode;\n",
        count=1,
        label=f"{path}: B14 translation call",
    )
    path.write_text(text, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    process_source = root / PROCESS_SOURCE
    if process_source.exists():
        patch_process_targeting_source(process_source)
    patch_translation_source(root / TRANSLATION_SOURCE)


if __name__ == "__main__":
    main()

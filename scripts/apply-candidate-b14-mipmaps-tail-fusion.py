#!/usr/bin/env python3
'''Wire the checked B14 cooperative Mipmaps tail into translated p_mipmaps.

GameNative launches Windows titles through Wine's explorer.exe desktop shell.
The configured target override must not make that helper process itself a
target, otherwise the resident LSFG layer can intercept the launcher's Vulkan
device/WSI path before the real game process exists.

The cooperative tail is capability-gated from Vulkan subgroup properties. If a
device cannot safely broadcast among at least four compute-subgroup lanes, the
translator leaves p_mipmaps at the B13 bytecode instead of forcing B14.
'''

from __future__ import annotations

import argparse
from pathlib import Path


TRANSLATION_HEADER = Path("include/extract/trans.hpp")
TRANSLATION_SOURCE = Path("src/extract/trans.cpp")
CONTEXT_SOURCE = Path("src/context.cpp")
HOOKS_HEADER = Path("include/hooks.hpp")
HOOKS_SOURCE = Path("src/hooks.cpp")
PROCESS_SOURCE = Path("src/utils/utils.cpp")
MARKER = "candidate-b14-mipmaps-tail-fusion"
PROCESS_MARKER = "gamenative-helper-process-override-guard"
CAPABILITY_MARKER = "b14-mipmaps-subgroup-capability-gate"


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

    old = r'''    // GameNative / Wine-on-Android: /proc/self/exe points at the Wine loader,
    // not the game .exe. Accept an explicit override from the launcher so
    // per-game matching in the TOML still works.
    const char* process_exe = std::getenv("LSFG_PROCESS_EXE");
    if (process_exe && *process_exe != '\0')
        return { process_exe, process_exe };
'''
    new = r'''    // GameNative / Wine-on-Android: /proc/self/exe points at the Wine loader,
    // not the game .exe. Accept an explicit override from the launcher so
    // per-game matching in the TOML still works. The override is inherited by
    // Wine helper processes too, so never target explorer.exe itself: it owns
    // the desktop shell/launcher and must remain a pass-through Vulkan client.
    // gamenative-helper-process-override-guard
    const char* process_exe = std::getenv("LSFG_PROCESS_EXE");
    if (process_exe && *process_exe != '\0') {
        std::array<char, 4096> cmdline{};
        std::ifstream cmdline_file("/proc/self/cmdline", std::ios::binary);
        if (cmdline_file.is_open()) {
            cmdline_file.read(
                cmdline.data(),
                static_cast<std::streamsize>(cmdline.size() - 1U));
            const auto cmdline_len =
                static_cast<size_t>(cmdline_file.gcount());
            if (cmdline_len > 0U) {
                cmdline.at(cmdline_len) = '\0';
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
                        << " configured_target=" << process_exe << '\n';
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


def patch_hooks_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "mipmapsSubgroupBroadcastSupported" in text:
        return
    old = "        bool androidOpaqueFdSemaphoreSupported{false};\n"
    new = (
        old
        + "        // B14 is optional. Unsupported subgroup hardware keeps exact B13\n"
        + "        // p_mipmaps bytecode rather than changing the game's Vulkan contract.\n"
        + "        bool mipmapsSubgroupBroadcastSupported{false};\n"
    )
    text = replace_exact(
        text, old, new, count=1, label=f"{path}: B14 subgroup capability field"
    )
    path.write_text(text, encoding="utf-8")


def patch_hooks_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if CAPABILITY_MARKER in text:
        return

    text = replace_exact(
        text,
        '#include "layer.hpp"\n',
        '#include "layer.hpp"\n'
        '#include "../scripts/b14_subgroup_properties.hpp"\n',
        count=1,
        label=f"{path}: B14 subgroup query helper include",
    )

    old = '''        const auto identity = Utils::getDeviceIdentity(physicalDevice, getProperties2);
'''
    new = r'''        // b14-mipmaps-subgroup-capability-gate
        // Some Android compatibility ICDs expose both aliases but rebuild the
        // core Properties2 pNext chain incompletely. Probe the KHR alias when
        // the core route cannot prove the exact B14 shader contract. Never
        // combine fields from separate calls and never infer capabilities from
        // subgroup size or the GPU identity.
        auto getProperties2Khr =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2KHR>(
                Layer::ovkGetInstanceProcAddr(
                    layerInstance, "vkGetPhysicalDeviceProperties2KHR"));
        const auto subgroupQuery = b14::querySubgroupProperties(
            physicalDevice, getProperties2, getProperties2Khr);
        const auto& subgroupProperties = subgroupQuery.properties;
        const bool mipmapsSubgroupBroadcastSupported =
            b14::supportsCooperativeMipmaps(subgroupProperties);
        std::cerr << "lsfg-vk: init stage=b14-mipmaps-capability"
                  << " query_route="
                  << b14::subgroupQueryRouteName(subgroupQuery.route)
                  << " subgroup_size=" << subgroupProperties.subgroupSize
                  << " supported_stages=0x" << std::hex
                  << subgroupProperties.supportedStages
                  << " supported_operations=0x"
                  << subgroupProperties.supportedOperations << std::dec
                  << " quad_all_stages="
                  << subgroupProperties.quadOperationsInAllStages
                  << " cooperative_tail="
                  << (mipmapsSubgroupBroadcastSupported ? 1 : 0)
                  << '\n';

        const auto identity = Utils::getDeviceIdentity(physicalDevice, getProperties2);
'''
    text = replace_exact(
        text, old, new, count=1, label=f"{path}: B14 subgroup property query"
    )

    old_init = "            .androidOpaqueFdSemaphoreSupported = androidOpaqueFdSemaphoreSupported,\n"
    new_init = (
        old_init
        + "            .mipmapsSubgroupBroadcastSupported = mipmapsSubgroupBroadcastSupported,\n"
    )
    text = replace_exact(
        text, old_init, new_init, count=1, label=f"{path}: B14 capability storage"
    )
    path.write_text(text, encoding="utf-8")


def patch_translation_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "enableCooperativeMipmaps" in text:
        return
    old = '''    std::vector<uint8_t> translateShader(
        std::vector<uint8_t> bytecode, const std::string& shaderName = {});
'''
    new = '''    std::vector<uint8_t> translateShader(
        std::vector<uint8_t> bytecode,
        const std::string& shaderName = {},
        bool enableCooperativeMipmaps = false);
'''
    text = replace_exact(
        text, old, new, count=1, label=f"{path}: B14 translator capability argument"
    )
    path.write_text(text, encoding="utf-8")


def patch_android_context(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if (
        "enableCooperativeMipmaps" in text
        and "mipmapsSubgroupBroadcastSupported" in text
    ):
        return

    # The unified branch owns recursive-interception cleanup with
    # ScopedLsfgDisable. Older candidate snapshots used a raw setenv here.
    # Match the Android-specific multiplier expression so this patch cannot
    # accidentally alter the desktop loader's shader callback.
    old_inits = (
        (
            '''    setenv("DISABLE_LSFG", "1", 1); // NOLINT
    lsfgInitialize(
        info.identity, format,
        conf.hdr, 1.0F / initialFlowScale, runtimeMultiplier - 1,
''',
            '''    setenv("DISABLE_LSFG", "1", 1); // NOLINT
    const bool enableCooperativeMipmaps =
        info.mipmapsSubgroupBroadcastSupported;
    lsfgInitialize(
        info.identity, format,
        conf.hdr, 1.0F / initialFlowScale, runtimeMultiplier - 1,
''',
        ),
        (
            '''    {
        ScopedLsfgDisable disableRecursiveInterception;
        lsfgInitialize(
            info.identity, format,
            conf.hdr, 1.0F / initialFlowScale, runtimeMultiplier - 1,
''',
            '''    {
        ScopedLsfgDisable disableRecursiveInterception;
        const bool enableCooperativeMipmaps =
            info.mipmapsSubgroupBroadcastSupported;
        lsfgInitialize(
            info.identity, format,
            conf.hdr, 1.0F / initialFlowScale, runtimeMultiplier - 1,
''',
        ),
    )
    for old_init, new_init in old_inits:
        if old_init in text:
            text = replace_exact(
                text,
                old_init,
                new_init,
                count=1,
                label=f"{path}: Android B14 capability capture",
            )
            break
    else:
        raise RuntimeError(
            f"{path}: Android B14 capability capture: unsupported LSFG initializer layout"
        )

    old_callbacks = (
        (
            '''        [](const std::string& name) {
            auto dxbc = Extract::getShader(name);
            auto spirv = Extract::translateShader(dxbc, name);
            return spirv;
        }
''',
            '''        [enableCooperativeMipmaps](const std::string& name) {
            auto dxbc = Extract::getShader(name);
            auto spirv = Extract::translateShader(
                dxbc, name, enableCooperativeMipmaps);
            return spirv;
        }
''',
        ),
        (
            '''            [](const std::string& name) {
                auto dxbc = Extract::getShader(name);
                auto spirv = Extract::translateShader(dxbc, name);
                return spirv;
            }
''',
            '''            [enableCooperativeMipmaps](const std::string& name) {
                auto dxbc = Extract::getShader(name);
                auto spirv = Extract::translateShader(
                    dxbc, name, enableCooperativeMipmaps);
                return spirv;
            }
''',
        ),
    )
    for old_callback, new_callback in old_callbacks:
        if old_callback in text:
            text = replace_exact(
                text,
                old_callback,
                new_callback,
                count=1,
                label=f"{path}: B14 capability-aware Android shader callback",
            )
            break
    else:
        raise RuntimeError(
            f"{path}: B14 capability-aware Android shader callback: unsupported layout"
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

    old_signature = '''std::vector<uint8_t> Extract::translateShader(
        std::vector<uint8_t> bytecode, const std::string& shaderName) {
'''
    new_signature = '''std::vector<uint8_t> Extract::translateShader(
        std::vector<uint8_t> bytecode, const std::string& shaderName,
        bool enableCooperativeMipmaps) {
'''
    if old_signature in text:
        text = replace_exact(
            text, old_signature, new_signature, count=1,
            label=f"{path}: B14 translator capability signature",
        )
    elif "Extract::translateShader(" in text and "enableCooperativeMipmaps" not in text:
        raise RuntimeError(f"{path}: B14 translator signature unavailable")

    call = '''    if (shaderName == "p_mipmaps") {
        if (!enableCooperativeMipmaps) {
            std::cerr << "lsfg-vk: candidate-b14-mipmaps-tail-fusion"
                      << " applied=0 already_applied=0 capability_gate=0"
                      << " fallback=b13 reason=subgroup-capability-unavailable"
                      << std::endl;
        } else {
            const auto b14Report = b14::fuseTail(spirvBytecode);
            if (!b14Report.applied && !b14Report.alreadyApplied) {
                std::cerr << "lsfg-vk: candidate-b14-mipmaps-tail-fusion"
                          << " applied=0 already_applied=0 capability_gate=1"
                          << " fallback=b13 reason=" << b14Report.reason
                          << std::endl;
            } else {
                std::cerr << "lsfg-vk: candidate-b14-mipmaps-tail-fusion"
                          << " applied=" << (b14Report.applied ? 1 : 0)
                          << " already_applied="
                          << (b14Report.alreadyApplied ? 1 : 0)
                          << " capability_gate=1 fallback=none"
                          << " barriers_before=" << b14Report.barriersBefore
                          << " barriers_after=" << b14Report.barriersAfter
                          << " tail_dynamic_loads_before="
                          << b14Report.tailDynamicLoadsBefore
                          << " tail_dynamic_loads_after="
                          << b14Report.tailDynamicLoadsAfter
                          << " tail_critical_path_loads_before="
                          << b14Report.tailCriticalPathLoadsBefore
                          << " tail_critical_path_loads_after="
                          << b14Report.tailCriticalPathLoadsAfter
                          << " tail_parallel_lanes_after="
                          << b14Report.tailParallelLanesAfter
                          << " subgroup_broadcasts_after="
                          << b14Report.subgroupBroadcastsAfter
                          << " static_workgroup_loads_before="
                          << b14Report.staticWorkgroupLoadsBefore
                          << " static_workgroup_loads_after="
                          << b14Report.staticWorkgroupLoadsAfter
                          << " tail_workgroup_stores_before="
                          << b14Report.tailWorkgroupStoresBefore
                          << " tail_workgroup_stores_after="
                          << b14Report.tailWorkgroupStoresAfter
                          << " image_writes_before=" << b14Report.imageWritesBefore
                          << " image_writes_after=" << b14Report.imageWritesAfter
                          << std::endl;
            }
        }
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

    optional_patches = (
        (HOOKS_HEADER, patch_hooks_header),
        (HOOKS_SOURCE, patch_hooks_source),
        (TRANSLATION_HEADER, patch_translation_header),
        (CONTEXT_SOURCE, patch_android_context),
    )
    for relative, patch in optional_patches:
        candidate = root / relative
        if candidate.exists():
            patch(candidate)

    patch_translation_source(root / TRANSLATION_SOURCE)


if __name__ == "__main__":
    main()

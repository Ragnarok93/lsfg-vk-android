#!/usr/bin/env python3
from __future__ import annotations

import shutil
import subprocess
import tempfile
import textwrap
import unittest
import runpy
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "scripts/b14_subgroup_properties.hpp"
PATCHER = ROOT / "scripts/apply-candidate-b14-mipmaps-tail-fusion.py"


VULKAN_STUB = r'''
#pragma once
#include <cstdint>

using VkBool32 = uint32_t;
using VkPhysicalDevice = uint64_t;
using VkShaderStageFlags = uint32_t;
using VkSubgroupFeatureFlags = uint32_t;

constexpr VkBool32 VK_FALSE = 0;
constexpr VkBool32 VK_TRUE = 1;
constexpr uint32_t VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 = 1000059001;
constexpr uint32_t VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES = 1000094000;
constexpr VkShaderStageFlags VK_SHADER_STAGE_COMPUTE_BIT = 0x00000020;
constexpr VkShaderStageFlags VK_SHADER_STAGE_FRAGMENT_BIT = 0x00000010;
constexpr VkSubgroupFeatureFlags VK_SUBGROUP_FEATURE_BASIC_BIT = 0x00000001;
constexpr VkSubgroupFeatureFlags VK_SUBGROUP_FEATURE_BALLOT_BIT = 0x00000008;

struct VkPhysicalDeviceSubgroupProperties {
    uint32_t sType{};
    void* pNext{};
    uint32_t subgroupSize{};
    VkShaderStageFlags supportedStages{};
    VkSubgroupFeatureFlags supportedOperations{};
    VkBool32 quadOperationsInAllStages{};
};

struct VkPhysicalDeviceProperties2 {
    uint32_t sType{};
    void* pNext{};
};

using PFN_vkGetPhysicalDeviceProperties2 = void (*)(
    VkPhysicalDevice, VkPhysicalDeviceProperties2*);
using PFN_vkGetPhysicalDeviceProperties2KHR = void (*)(
    VkPhysicalDevice, VkPhysicalDeviceProperties2*);
'''


HARNESS = r'''
#include "b14_subgroup_properties.hpp"

#include <cassert>
#include <cstring>

namespace {
int coreCalls{};
int khrCalls{};

void write(VkPhysicalDeviceProperties2* out, uint32_t size,
        VkShaderStageFlags stages, VkSubgroupFeatureFlags operations,
        VkBool32 quad) {
    auto* subgroup = static_cast<VkPhysicalDeviceSubgroupProperties*>(out->pNext);
    assert(subgroup != nullptr);
    assert(subgroup->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES);
    subgroup->subgroupSize = size;
    subgroup->supportedStages = stages;
    subgroup->supportedOperations = operations;
    subgroup->quadOperationsInAllStages = quad;
}

void validCore(VkPhysicalDevice, VkPhysicalDeviceProperties2* out) {
    ++coreCalls;
    write(out, 32, VK_SHADER_STAGE_COMPUTE_BIT,
        VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT, VK_FALSE);
}

void missingBallot(VkPhysicalDevice, VkPhysicalDeviceProperties2* out) {
    ++coreCalls;
    write(out, 64, VK_SHADER_STAGE_COMPUTE_BIT,
        VK_SUBGROUP_FEATURE_BASIC_BIT, VK_FALSE);
}

void malformedCore(VkPhysicalDevice, VkPhysicalDeviceProperties2* out) {
    ++coreCalls;
    write(out, 128, 0, 0, VK_FALSE);
}

void validKhr(VkPhysicalDevice, VkPhysicalDeviceProperties2* out) {
    ++khrCalls;
    write(out, 64, VK_SHADER_STAGE_COMPUTE_BIT,
        VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT, VK_TRUE);
}

void missingBallotKhr(VkPhysicalDevice, VkPhysicalDeviceProperties2* out) {
    ++khrCalls;
    write(out, 64, VK_SHADER_STAGE_COMPUTE_BIT,
        VK_SUBGROUP_FEATURE_BASIC_BIT, VK_FALSE);
}

void fragmentOnly(VkPhysicalDevice, VkPhysicalDeviceProperties2* out) {
    ++coreCalls;
    write(out, 32, VK_SHADER_STAGE_FRAGMENT_BIT,
        VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT, VK_FALSE);
}
} // namespace

int main() {
    {
        coreCalls = khrCalls = 0;
        const auto result = b14::querySubgroupProperties(1, validCore, validKhr);
        assert(result.route == b14::SubgroupQueryRoute::Core);
        assert(coreCalls == 1 && khrCalls == 0);
        assert(!b14::supportsCooperativeMipmaps(result.properties));
    }
    {
        coreCalls = khrCalls = 0;
        const auto result = b14::querySubgroupProperties(1, missingBallot, nullptr);
        assert(result.route == b14::SubgroupQueryRoute::Core);
        assert(!b14::supportsCooperativeMipmaps(result.properties));
    }
    {
        coreCalls = khrCalls = 0;
        const auto result = b14::querySubgroupProperties(1, malformedCore, validKhr);
        assert(result.route == b14::SubgroupQueryRoute::Khr);
        assert(coreCalls == 1 && khrCalls == 1);
        assert(result.properties.subgroupSize == 64);
        assert(result.properties.quadOperationsInAllStages == VK_TRUE);
        assert(!b14::supportsCooperativeMipmaps(result.properties));
    }
    {
        coreCalls = khrCalls = 0;
        const auto result = b14::querySubgroupProperties(
            1, malformedCore, missingBallotKhr);
        assert(result.route == b14::SubgroupQueryRoute::Khr);
        assert(coreCalls == 1 && khrCalls == 1);
        assert(result.properties.supportedStages == VK_SHADER_STAGE_COMPUTE_BIT);
        assert(result.properties.supportedOperations == VK_SUBGROUP_FEATURE_BASIC_BIT);
        assert(!b14::supportsCooperativeMipmaps(result.properties));
    }
    {
        coreCalls = khrCalls = 0;
        const auto result = b14::querySubgroupProperties(1, fragmentOnly, nullptr);
        assert(result.route == b14::SubgroupQueryRoute::Core);
        assert(!b14::supportsCooperativeMipmaps(result.properties));
    }
    return 0;
}
'''


class AndroidB14SubgroupPropertyQueryTest(unittest.TestCase):
    def test_core_and_khr_routes_fail_closed_without_full_subgroup_mapping(self) -> None:
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as tmp:
            temp = Path(tmp)
            vulkan = temp / "include/vulkan/vulkan_core.h"
            vulkan.parent.mkdir(parents=True)
            vulkan.write_text(textwrap.dedent(VULKAN_STUB), encoding="utf-8")
            harness = temp / "subgroup_query.cpp"
            harness.write_text(textwrap.dedent(HARNESS), encoding="utf-8")
            executable = temp / "subgroup_query"
            subprocess.run(
                [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                 "-I", str(temp / "include"), "-I", str(ROOT / "scripts"),
                 str(harness), "-o", str(executable)],
                check=True,
            )
            subprocess.run([str(executable)], check=True)

    def test_candidate_patcher_queries_both_aliases_and_logs_raw_properties(self) -> None:
        module = runpy.run_path(str(PATCHER))
        with tempfile.TemporaryDirectory() as tmp:
            hooks = Path(tmp) / "hooks.cpp"
            hooks.write_text(
                '''#include "layer.hpp"\n
void probe() {
        auto getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            Layer::ovkGetInstanceProcAddr(layerInstance, "vkGetPhysicalDeviceProperties2"));
        if (getProperties2 == nullptr) {
            getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                Layer::ovkGetInstanceProcAddr(layerInstance, "vkGetPhysicalDeviceProperties2KHR"));
        }
        const auto identity = Utils::getDeviceIdentity(physicalDevice, getProperties2);
            .androidOpaqueFdSemaphoreSupported = androidOpaqueFdSemaphoreSupported,
}
''',
                encoding="utf-8",
            )
            module["patch_hooks_source"](hooks)
            generated = hooks.read_text(encoding="utf-8")

        self.assertIn("b14_subgroup_properties.hpp", generated)
        self.assertIn('"vkGetPhysicalDeviceProperties2"', generated)
        self.assertIn('"vkGetPhysicalDeviceProperties2KHR"', generated)
        self.assertIn("querySubgroupProperties", generated)
        self.assertIn("supportsCooperativeMipmaps", generated)
        for field in (
            "supportedStages", "supportedOperations", "quadOperationsInAllStages",
        ):
            self.assertIn(field, generated)


if __name__ == "__main__":
    unittest.main()

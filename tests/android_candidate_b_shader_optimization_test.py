#!/usr/bin/env python3
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidCandidateBShaderOptimizationContractTest(unittest.TestCase):
    def test_lossless_hot_path_optimizer_is_adreno6xx_scoped_and_fails_open(self) -> None:
        optimizer_hpp = ROOT / "include/extract/shader_opt.hpp"
        optimizer_cpp = ROOT / "src/extract/shader_opt.cpp"
        self.assertTrue(optimizer_hpp.exists(), optimizer_hpp.as_posix())
        self.assertTrue(optimizer_cpp.exists(), optimizer_cpp.as_posix())

        source = optimizer_cpp.read_text(encoding="utf-8")
        for token in (
            '#include <spirv-tools/optimizer.hpp>',
            '#include <spirv-tools/libspirv.hpp>',
            'name == "p_mipmaps" || name == "p_beta[4]"',
            'RegisterPerformancePasses(true)',
            'SPV_ENV_VULKAN_1_3',
            'shader-opt applied=1',
            'shader-opt applied=0',
            'return spirv;',
        ):
            self.assertIn(token, source)
        for forbidden in ('p_beta[0]', 'p_beta[1]', 'p_beta[2]', 'p_beta[3]', 'sleep(', 'vkQueueWaitIdle', 'vkDeviceWaitIdle'):
            self.assertNotIn(forbidden, source)

        context = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        for token in (
            '0x5143',
            'Adreno (TM) 6',
            'Extract::optimizeHotPathShader(name, std::move(spirv), optimizeShaderHotPath)',
        ):
            self.assertIn(token, context)
        self.assertEqual(context.count('Extract::optimizeHotPathShader('), 1)

        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        for token in (
            'SPIRV_SKIP_EXECUTABLES ON',
            'SPIRV-Headers_SOURCE_DIR',
            'add_subdirectory(thirdparty/spirv-tools',
            'SPIRV-Tools-opt',
        ):
            self.assertIn(token, cmake)

        gitmodules = (ROOT / ".gitmodules").read_text(encoding="utf-8")
        self.assertIn('thirdparty/spirv-tools', gitmodules)
        self.assertIn('KhronosGroup/SPIRV-Tools.git', gitmodules)
        self.assertIn('thirdparty/spirv-headers', gitmodules)
        self.assertIn('KhronosGroup/SPIRV-Headers.git', gitmodules)


if __name__ == "__main__":
    unittest.main()

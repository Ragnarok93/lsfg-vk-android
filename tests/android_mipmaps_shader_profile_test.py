#!/usr/bin/env python3
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AndroidMipmapsShaderProfileContractTest(unittest.TestCase):
    def test_android_profile_transform_logs_mipmaps_spirv_metadata_and_effective_scale(self) -> None:
        zero_stage_patcher = ROOT / "scripts/apply-zero-stage-profile.py"
        shader_patcher = ROOT / "scripts/apply-mipmaps-shader-profile.py"
        self.assertTrue(zero_stage_patcher.exists(), zero_stage_patcher.as_posix())
        self.assertTrue(shader_patcher.exists(), shader_patcher.as_posix())

        required_files = (
            Path("framegen/v3.1_include/v3_1/context.hpp"),
            Path("framegen/v3.1_src/context.cpp"),
            Path("framegen/v3.1_include/v3_1/shaders/mipmaps.hpp"),
            Path("framegen/v3.1_src/shaders/mipmaps.cpp"),
            Path("framegen/v3.1p_include/v3_1p/context.hpp"),
            Path("framegen/v3.1p_src/context.cpp"),
            Path("framegen/v3.1p_include/v3_1p/shaders/mipmaps.hpp"),
            Path("framegen/v3.1p_src/shaders/mipmaps.cpp"),
            Path("include/extract/trans.hpp"),
            Path("src/extract/trans.cpp"),
            Path("framegen/v3.1_src/lsfg.cpp"),
            Path("framegen/v3.1p_src/lsfg.cpp"),
        )

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            for rel in required_files:
                target = temp_root / rel
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / rel, target)

            for _ in range(2):
                subprocess.run(
                    [sys.executable, str(zero_stage_patcher), "--root", str(temp_root)],
                    check=True,
                )
                subprocess.run(
                    [sys.executable, str(shader_patcher), "--root", str(temp_root)],
                    check=True,
                )

            trans_header = (temp_root / "include/extract/trans.hpp").read_text(encoding="utf-8")
            trans_source = (temp_root / "src/extract/trans.cpp").read_text(encoding="utf-8")
            quality_loader = (temp_root / "framegen/v3.1_src/lsfg.cpp").read_text(encoding="utf-8")
            performance_loader = (temp_root / "framegen/v3.1p_src/lsfg.cpp").read_text(encoding="utf-8")

            self.assertIn("const std::string& shaderName", trans_header)
            self.assertIn("zero-stage-shader-profile", trans_source)
            for field in (
                "shader=",
                "spirv_bytes=",
                "spirv_words=",
                "instruction_count=",
                "local_size=",
                "workgroup_variables=",
                "valid_spirv=",
            ):
                self.assertIn(field, trans_source)
            self.assertIn("kOpExecutionMode", trans_source)
            self.assertIn("kExecutionModeLocalSize", trans_source)
            self.assertIn("kOpVariable", trans_source)
            self.assertIn("kStorageClassWorkgroup", trans_source)
            self.assertIn("<< std::endl;", trans_source)
            self.assertIn("Extract::translateShader(dxbc, name)", quality_loader)
            self.assertIn("Extract::translateShader(dxbc, name)", performance_loader)

            for rel in (
                Path("framegen/v3.1_src/shaders/mipmaps.cpp"),
                Path("framegen/v3.1p_src/shaders/mipmaps.cpp"),
            ):
                source = (temp_root / rel).read_text(encoding="utf-8")
                self.assertIn("effective_flow_scale=", source, rel.as_posix())


if __name__ == "__main__":
    unittest.main()

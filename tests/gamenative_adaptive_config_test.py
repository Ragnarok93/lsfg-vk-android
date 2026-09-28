#!/usr/bin/env python3
from pathlib import Path

CONFIG = Path(__file__).resolve().parents[1] / "src" / "config" / "config.cpp"
SOURCE = CONFIG.read_text(encoding="utf-8")


def test_gamenative_adaptive_uses_authoritative_conf_toml():
    assert '"adaptive_framegen"' in SOURCE
    assert '"fps_limit"' in SOURCE
    assert '"gamenative-adaptive.toml"' not in SOURCE
    assert "GameNativeAdaptiveOverride" not in SOURCE
    assert "read_gamenative_adaptive_override" not in SOURCE


def test_adaptive_target_is_not_source_pacing():
    assert "GameNative owns source-frame pacing" in SOURCE
    assert ".adaptiveFramegen = toml::find_or(gameTable, \"adaptive_framegen\", false)" in SOURCE
    assert ".fpsLimit = toml::find_or(gameTable, \"fps_limit\", 0U)" in SOURCE
    assert "Adaptive frame generation requires a positive fps_limit" in SOURCE


def test_fixed_and_source_only_do_not_require_target():
    validation = SOURCE.split("// GameNative owns source-frame pacing.", 1)[1].split("games[exe]", 1)[0]
    assert "if (game.adaptiveFramegen && game.fpsLimit == 0)" in validation
    assert "if (!game.adaptiveFramegen && game.fpsLimit == 0)" not in validation


def test_runtime_off_remains_multiplier_one_resident_semantics():
    assert ".enable = true" in SOURCE
    assert ".targeted = true" in SOURCE
    assert "if (game.multiplier < 1)" in SOURCE


def test_auto_preset_is_accepted_by_native_config():
    assert 'preset == "auto"' in SOURCE
    assert "Adaptive Flow preset must be quality, balanced, low, or auto" in SOURCE


if __name__ == "__main__":
    for test in (
        test_gamenative_adaptive_uses_authoritative_conf_toml,
        test_adaptive_target_is_not_source_pacing,
        test_fixed_and_source_only_do_not_require_target,
        test_runtime_off_remains_multiplier_one_resident_semantics,
        test_auto_preset_is_accepted_by_native_config,
    ):
        test()
    print("gamenative adaptive config contract: 5 tests passed")

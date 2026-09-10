#!/usr/bin/env python3
from pathlib import Path

CONFIG = Path(__file__).resolve().parents[1] / "src" / "config" / "config.cpp"
SOURCE = CONFIG.read_text(encoding="utf-8")


def test_gamenative_adaptive_uses_separate_overlay():
    assert '"gamenative-adaptive.toml"' in SOURCE
    assert '"adaptive_framegen"' in SOURCE
    assert '"target_output_fps"' in SOURCE


def test_overlay_overrides_only_adaptive_target_fields():
    assert "game.adaptiveFramegen = gameNativeAdaptive->enabled;" in SOURCE
    assert "game.fpsLimit = gameNativeAdaptive->enabled" in SOURCE
    assert "game.multiplier =" not in SOURCE.split("if (gameNativeAdaptive.has_value())", 1)[1].split("// validate", 1)[0]
    assert "game.flowScale =" not in SOURCE.split("if (gameNativeAdaptive.has_value())", 1)[1].split("// validate", 1)[0]


def test_malformed_overlay_fails_open_to_fixed_mode():
    assert "return GameNativeAdaptiveOverride{};" in SOURCE
    assert "falling back to fixed mode" in SOURCE
    assert "Ignoring malformed GameNative Adaptive overlay" in SOURCE

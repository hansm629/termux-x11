#!/usr/bin/env python3
"""Guard build-only GAME Power HAL experiment and standard APK isolation."""

import importlib.util
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "tools/oneui-game-power/category.py"
spec = importlib.util.spec_from_file_location("game_category_builder", MODULE)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

original = (ROOT / "app/src/main/AndroidManifest.xml").read_text()
assert module.category_from_xml(original) is None, "Standard source manifest must NOT classify app as GAME"
experimental = module.enable_game(original)
assert module.category_from_xml(experimental) == "game"
assert experimental.replace('        android:appCategory="game"\n', '', 1) == original
try:
    module.enable_game(experimental)
except ValueError:
    pass
else:
    raise AssertionError("GAME category build should reject double insertion")

# Verify the exact merged-manifest check in a fresh stand-in build directory.
with tempfile.TemporaryDirectory() as temp:
    merged_dir = Path(temp) / "merged_manifests/debug/mergeDebugManifest"
    merged_dir.mkdir(parents=True)
    manifest = merged_dir / "AndroidManifest.xml"
    old_roots = module.MERGED
    try:
        module.MERGED = [Path(temp) / "merged_manifests"]
        manifest.write_text(original)
        module.verify_merged("none")
        try:
            module.verify_merged("game")
        except ValueError:
            pass
        else:
            raise AssertionError("GAME CI must reject standard-category APK")
        manifest.write_text(experimental)
        module.verify_merged("game")
        try:
            module.verify_merged("none")
        except ValueError:
            pass
        else:
            raise AssertionError("normal CI must reject GAME-category APK")
    finally:
        module.MERGED = old_roots

gradle = (ROOT / "app/build.gradle").read_text()
workflow = (ROOT / ".github/workflows/debug_build.yml").read_text()
activity = (ROOT / "app/src/main/java/com/termux/x11/MainActivity.java").read_text()
assert "tx11GamePowerExperiment" in gradle
assert 'buildConfigField "boolean", "GAME_POWER_EXPERIMENT"' in gradle
assert "game_power_mode" in workflow and "category.py verify game" in workflow
assert "category.py verify none" in workflow
assert "GAME-POWER-EXPERIMENT" in workflow
assert "BuildConfig.GAME_POWER_EXPERIMENT" in activity
assert "gameManager.getGameMode()" in activity
print("TGAMECATEGORY standard manifest isolation, explicit GAME build, runtime diagnostics: PASS")

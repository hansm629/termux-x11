#!/usr/bin/env python3
"""Build-only Android GAME category opt-in; production manifest is untouched.

The official Android GAME power mode is sent to Power HAL for foreground
applications classified as games. It is an OEM policy hint, not a command to
write sysfs clocks or spin a fake CPU/GPU worker.

Enable modifies ONLY the isolated CI runner checkout before Gradle merges
the manifest. Verify game/none inspects Gradle's actual merged manifest.
"""

import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ANDROID = "{http://schemas.android.com/apk/res/android}"
ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "app/src/main/AndroidManifest.xml"
MERGED = [
    ROOT / "app/build/intermediates/merged_manifests/debug",
    ROOT / "app/build/intermediates/merged_manifest/debug",
]


def category_from_xml(source: str) -> str | None:
    root = ET.fromstring(source)
    app = root.find("application")
    if app is None:
        raise ValueError("AndroidManifest has no application element")
    return app.get(ANDROID + "appCategory")


def enable_game(source: str) -> str:
    if category_from_xml(source) is not None:
        raise ValueError("Refusing to override an existing appCategory")
    anchor = "    <application\n"
    if source.count(anchor) != 1:
        raise ValueError("Unexpected application layout; refusing blind insertion")
    result = source.replace(anchor, anchor + '        android:appCategory="game"\n', 1)
    if category_from_xml(result) != "game":
        raise ValueError("Failed to establish Android appCategory=game")
    return result


def verify_merged(expected: str) -> None:
    candidates = [p for base in MERGED if base.is_dir()
                  for p in base.rglob("AndroidManifest.xml")]
    if not candidates:
        raise ValueError("No merged debug manifest; build before verifying")
    checked = 0
    for candidate in candidates:
        root = ET.parse(candidate).getroot()
        app = root.find("application")
        if app is None or not any((act.get(ANDROID + "name") or "").endswith("MainActivity")
                                  for act in app.findall("activity")):
            continue
        found = app.get(ANDROID + "appCategory")
        if found != ("game" if expected == "game" else None):
            raise ValueError(f"{candidate}: appCategory={found!r}, expected {expected}")
        print(f"PASS: {candidate}: category={found!r}, expected={expected}")
        checked += 1
    if not checked:
        raise ValueError("No merged manifest contains Termux-X11 MainActivity")


def main() -> int:
    if len(sys.argv) == 2 and sys.argv[1] == "enable":
        original = SOURCE.read_text()
        SOURCE.write_text(enable_game(original))
        print(f"PASS: explicit experimental GAME category in CI checkout {SOURCE}")
        return 0
    if len(sys.argv) == 3 and sys.argv[1] == "verify" and sys.argv[2] in ("none", "game"):
        verify_merged(sys.argv[2])
        return 0
    print("usage: category.py enable | verify game | verify none", file=sys.stderr)
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, ET.ParseError, OSError) as error:
        sys.exit(f"FAIL: GAME power experiment: {error}")

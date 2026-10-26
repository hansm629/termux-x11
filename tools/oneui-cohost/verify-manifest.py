#!/usr/bin/env python3
"""Check the *merged* debug manifest, not just the unexpanded source XML.

Run after :app:processDebugMainManifest or assembleDebug. The normal build must
stay com.termux.x11; ONLY the explicitly selected cohost variant uses com.termux.
"""
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ANDROID = "{http://schemas.android.com/apk/res/android}"
if len(sys.argv) != 2 or sys.argv[1] not in ("com.termux", "com.termux.x11"):
    sys.exit("usage: verify-manifest.py com.termux|com.termux.x11")
expected = sys.argv[1]
roots = [
    Path("app/build/intermediates/merged_manifests/debug"),
    Path("app/build/intermediates/merged_manifest/debug"),
]
candidates = [p for root in roots if root.is_dir() for p in root.rglob("AndroidManifest.xml")]
if not candidates:
    sys.exit("FAIL: Gradle did not produce a merged debug AndroidManifest.xml")

for path in candidates:
    root = ET.parse(path).getroot()
    app = root.find("application")
    if app is None:
        continue
    process_name = app.get(ANDROID + "process")
    shared_uid = root.get(ANDROID + "sharedUserId")
    main = next((a for a in app.findall("activity") if (a.get(ANDROID + "name") or "").endswith("MainActivity")), None)
    if main is None:
        continue
    if shared_uid != "com.termux" or process_name != expected:
        sys.exit(f"FAIL: {path}: sharedUserId={shared_uid!r}, process={process_name!r}, expected={expected!r}")
    # Activity, Preferences, Accessibility Service and FGS must all share the *same*
    # process. An explicit component-level android:process would break static state
    # lookups such as KeyInterceptor -> MainActivity.getInstance().
    components = [*app.findall("activity"), *app.findall("service"), *app.findall("receiver")]
    overrides = [(c.tag, c.get(ANDROID + "name"), c.get(ANDROID + "process"))
                 for c in components if c.get(ANDROID + "process") not in (None, expected)]
    if overrides:
        sys.exit(f"FAIL: process overrides break shared host: {overrides}")
    print(f"PASS: merged manifest {path} sharedUserId=com.termux, application process={expected}, "
          f"{len(components)} app components agree")
    break
else:
    sys.exit("FAIL: none of the merged manifests contains Termux-X11 MainActivity")

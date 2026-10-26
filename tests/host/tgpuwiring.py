#!/usr/bin/env python3
"""Guard the Java/native/Gradle plumbing of the optional cross-vendor GPU helper."""
from pathlib import Path
from xml.etree import ElementTree as ET

root = Path(__file__).resolve().parents[2]
app = root / "app/src/main"
native = app / "cpp/lorie"
java = app / "java/com/termux/x11"
xml = app / "res/xml/preferences.xml"
strings = app / "res/values/strings.xml"

def contains(filename, tokens):
    text = filename.read_text()
    for token in tokens:
        assert token in text, f"{filename} missing {token!r}"

contains(app / "cpp/recipes/xserver.cmake", ['"lorie/gpu_performance.c"'])
contains(native / "renderer.c",
         ['gpuPerformanceNoteFrame();', 'gpuPerformanceSetMode(mode)',
          'gpuPerformanceSetForeground(', 'gpuPerformanceSetThermal(',
          'gpuPerformanceSetRefreshRate(refreshRate)'])
contains(native / "activity.c",
         ['"setGpuPerformanceModeNative"', '"setGpuPerformanceForegroundNative"',
          '"setGpuPerformanceThermalNative"'])
contains(java / "LorieView.java",
         ['"gpuPerformanceMode"', 'setGpuPerformanceModeNative(gpuModeValue)',
          'setGpuPerformanceForegroundNative(', 'setGpuPerformanceThermalNative('])
contains(java / "MainActivity.java",
         ['gpuView.setGpuPerformanceForeground(true)',
          'gpuView.setGpuPerformanceForeground(false)',
          'GpuThermalApi29.subscribe(this)', 'GpuThermalApi29.unsubscribe(',
          'public void onStart()', 'public void onStop()'])
activity_src = (java / "MainActivity.java").read_text()
start_block = activity_src.split("public void onStart()", 1)[1].split("public void onResume()", 1)[0]
stop_block = activity_src.split("public void onStop()", 1)[1].split("public void onPause()", 1)[0]
assert 'setGpuPerformanceForeground(true)' in start_block
assert 'setGpuPerformanceForeground(false)' in stop_block
assert 'setGpuPerformanceForeground(false)' not in activity_src.split(
    "public void onPause()", 1)[1].split("public LorieView getLorieView()", 1)[0]
contains(native / "gpu_performance.c",
         ['gpuPerfLimitsForRefresh(', 'gpuPerfAsyncPulse(', 'gpu->pendingCount == 2',
          'EGL_SYNC_FENCE_KHR', 'gpuPerformanceSetRefreshRate(',
          'GPU_PERF_PERFORMANCE &&', 'state=idle mode='])

a = "{http://schemas.android.com/apk/res-auto}"
elements = ET.parse(xml).getroot().iter()
prefs = [p for p in elements if p.attrib.get(a + "key") == "gpuPerformanceMode"]
assert len(prefs) == 1, f"Expected exactly one GPU mode preference, found {len(prefs)}"
assert prefs[0].attrib[a + "defaultValue"] == "off"
assert prefs[0].tag == "ListPreference"
surface_prefs = [p for p in ET.parse(xml).getroot().iter()
                 if p.attrib.get(a + "key") == "highRefreshSurfacePolicy"]
assert len(surface_prefs) == 1
assert surface_prefs[0].attrib[a + "defaultValue"] == "default"
values = ET.parse(strings).getroot()
choices = next((e for e in values.findall("string-array")
                if e.attrib.get("name") == "lorie_pref_gpuPerformanceMode_values"), None)
assert choices is not None
assert [c.text for c in choices.findall("item")] == [
    "off", "balanced", "performance", "maximum"
]
surface_choices = next((e for e in values.findall("string-array")
                        if e.attrib.get("name") == "lorie_pref_highRefreshSurfacePolicy_values"), None)
assert surface_choices is not None
assert [c.text for c in surface_choices.findall("item")] == [
    "default", "system", "legacy_fixed"
]
contains(java / "LorieView.java",
         ['"highRefreshSurfacePolicy"', 'requestRendererSurfaceFrameRate(refreshRate)',
          'requestRendererSurfaceFrameRateFixedSource(refreshRate)',
          'clearRendererSurfaceFrameRate();'])
print("TGPUWIRING cross-vendor controller JNI, preference, thermal/foreground and build: PASS")

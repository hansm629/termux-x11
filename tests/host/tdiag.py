#!/usr/bin/env python3
"""Checks tools/diag/renderer-proof.sh and renderer-status.sh against logs written in the X server's format
(InitOutput.c lorieLogRendererDiag): a run that really drew with Vulkan passes --expect vulkan and fails
--expect gles, and the reverse for GLES; and every way a run can fall short of the proof fails it - no
present, no imported-image draw, a GLES frame or swap in the window, a fallback, the backend changing within
the window - and too few reports is "insufficient", never a pass.

And the same against the logs tdiaglog.c wrote with the X server's real formatter, when given its output
directory: the Vulkan run passes --expect vulkan only, the GLES run --expect gles only.

usage: tdiag.py <tools/diag> [<host test output dir>]
"""
import os, subprocess, sys, tempfile

D = sys.argv[1]
fails = 0
def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("  FAIL " + what)

def report(t, active, gles, vk, fb_total_delta=0, totals=None):
    """One 5 s report, as the X server logs it. gles/vk give deltas; totals default to 10x the delta."""
    totals = totals or {}
    def kv(k, d):
        return "%s=%d(+%d)" % (k, totals.get(k, d * 10), d)
    p = "10-07 12:00:%02d.000  100  101 I LorieNative: " % t
    lines = [p + "XlorieBackend: active=%s" % active,
             p + "XlorieGles: " + " ".join(kv(k, gles.get(k, 0)) for k in ("egl_image_imports", "gles_copy_frames", "egl_swap_buffers")),
             p + "XlorieVk: " + " ".join(kv(k, vk.get(k, 0)) for k in (
                 "instance_created", "device_created", "surface_created", "swapchain_created", "ahb_import_attempts",
                 "ahb_import_success", "imported_source_draws", "imported_destination_draws", "copy_commands",
                 "blit_commands", "render_commands", "queue_submits", "queue_present_success", "frames_presented")),
             p + 'XlorieVkTiming: acquire_success=0(+0) selftest=pass device="Adreno (TM) 840" present_mode="FIFO"',
             p + "XlorieFallback: total=%d(+%d) fatal_backend=0 resource_local=0 injected=0 last_class=none last_reason=none "
                 "last_vkresult=0 last_resource_id=0 last_resource_format=0 last_resource_usage=0x0 reasons=none" % (
                     fb_total_delta, fb_total_delta)]
    return lines

VK = {"ahb_import_success": 2, "imported_source_draws": 300, "imported_destination_draws": 40, "render_commands": 340,
      "queue_submits": 300, "queue_present_success": 300, "frames_presented": 300}
GL = {"egl_image_imports": 1, "gles_copy_frames": 300, "egl_swap_buffers": 300}

def run(script, lines, *args):
    with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
        f.write("noise line\n" + "\n".join(lines) + "\n")
        path = f.name
    try:
        r = subprocess.run(["sh", os.path.join(D, script), "--file", path] + list(args), capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr
    finally:
        os.unlink(path)

def reports(n, **kw):
    out = []
    for i in range(n):
        out += report(i * 5, **kw)
    return out

vk_good = reports(3, active="VULKAN", gles={}, vk=VK)
gl_good = reports(3, active="GLES", gles=GL, vk={})
cases = [
    ("vulkan run, --expect vulkan", vk_good, "vulkan", 0),
    ("vulkan run, --expect gles", vk_good, "gles", 1),
    ("gles run, --expect gles", gl_good, "gles", 0),
    ("gles run, --expect vulkan", gl_good, "vulkan", 1),
    ("vulkan claimed with no present", reports(3, active="VULKAN", gles={}, vk=dict(VK, queue_present_success=0, frames_presented=0)), "vulkan", 1),
    ("vulkan with no imported-image draw", reports(3, active="VULKAN", gles={}, vk=dict(VK, imported_source_draws=0)), "vulkan", 1),
    ("vulkan never imported an AHB", reports(3, active="VULKAN", gles={}, vk=dict(VK, ahb_import_success=0)), "vulkan", 1),
    ("vulkan with GLES frames in the window", reports(3, active="VULKAN", gles=GL, vk=VK), "vulkan", 1),
    ("vulkan with a fallback in the window", reports(3, active="VULKAN", gles={}, vk=VK, fb_total_delta=1), "vulkan", 1),
    ("gles with a Vulkan present in the window", reports(3, active="GLES", gles=GL, vk={"queue_present_success": 1, "frames_presented": 1}), "gles", 1),
    ("backend changed within the window", report(0, active="GLES", gles=GL, vk={}) + reports(2, active="VULKAN", gles={}, vk=VK), "vulkan", 1),
    ("too few reports", reports(1, active="VULKAN", gles={}, vk=VK), "vulkan", 2),
    ("a line type missing", [l for l in vk_good if "XlorieFallback" not in l], "vulkan", 2),
]
for name, lines, expect, want in cases:
    code, out = run("renderer-proof.sh", lines, "--expect", expect)
    check(code == want, "%s: exit %d, want %d\n%s" % (name, code, want, out))
    check(("RESULT: PASS" in out) == (want == 0), "%s: result line does not match the exit status" % name)

code, out = run("renderer-status.sh", vk_good)
check(code == 0 and all(k in out for k in ("XlorieBackend: active=VULKAN", "XlorieGles:", "XlorieVk:", "XlorieVkTiming:", "XlorieFallback:")),
      "renderer-status: %d\n%s" % (code, out))
check(out.count("XlorieVk:") == 1, "renderer-status printed more than the latest report")
code, out = run("renderer-status.sh", ["nothing here"])
check(code == 1, "renderer-status with no diagnostics: exit %d" % code)
r = subprocess.run(["sh", os.path.join(D, "renderer-proof.sh"), "--expect", "vulkan", "--reports", "0"], capture_output=True, text=True)
check(r.returncode == 2, "--reports 0 accepted")

if len(sys.argv) > 2:
    real = {"vulkan": os.path.join(sys.argv[2], "diag-vulkan.log"), "gles": os.path.join(sys.argv[2], "diag-gles.log")}
    check(all(os.path.exists(p) for p in real.values()), "no logs from tdiaglog in %s" % sys.argv[2])
    for run_, p in real.items():
        for expect in ("vulkan", "gles"):
            r = subprocess.run(["sh", os.path.join(D, "renderer-proof.sh"), "--file", p, "--expect", expect],
                               capture_output=True, text=True)
            check(r.returncode == (0 if run_ == expect else 1), "real-format %s log, --expect %s: exit %d\n%s" % (
                run_, expect, r.returncode, r.stdout))

print("renderer diagnostics scripts (%d cases): %s (%d failures)" % (len(cases), "FAIL" if fails else "PASS", fails))
sys.exit(1 if fails else 0)

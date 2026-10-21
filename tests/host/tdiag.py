#!/usr/bin/env python3
"""Checks tools/diag/renderer-proof.sh and renderer-status.sh against logs written in the X server's format
(InitOutput.c lorieLogRendererDiag): a run that really drew with Vulkan passes --expect vulkan and fails
--expect gles, and the reverse for GLES; and every way a run can fall short of the proof fails it - no
present, no imported-image draw, a GLES frame or swap in the window, a fallback, the backend changing within
the window - and too few reports is "insufficient", never a pass.

And the same against the logs tdiaglog.c wrote with the X server's real formatter, when given its output
directory: the Vulkan run passes --expect vulkan only, the GLES run --expect gles only; renderer-status.sh prints the
latest of P11-C3's TERMUX_X11_RENDERER_VK_FRAME_DIAG lines there are, none where there are none.

And frame-diag-session.sh (P11-C3) as it runs on a device, its waits and Android's tools faked: the three phases'
marks in order, each in the log (as LorieNative) and in the file, the X server's lines from the session's start after
them, nothing else left behind; a warning when the session has no pacing report.

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

def report(t, active, gles, vk, fb_total_delta=0, totals=None, ownership=None, selftest=None, iterations=1000):
    """One 5 s report, as the X server logs it. gles/vk give deltas; totals default to 10x the delta. The X
    server's AHB ownership defaults to the one that goes with `active`."""
    ownership = ownership or ("per_access" if active == "VULKAN" else "persistent")
    selftest = selftest or ("pass" if active == "VULKAN" else "none")
    totals = totals or {}
    def kv(k, d):
        return "%s=%d(+%d)" % (k, totals.get(k, d * 10), d)
    p = "10-07 12:00:%02d.000  100  101 I LorieNative: " % t
    lines = [p + "XlorieBackend: requested=auto vk_fail_at=none vk_selftest=full active=%s ownership=%s ownership_transitions=1 ownership_refused=0 "
                 "x_ahb_cpu_acquires=40(+4) x_state_lock_wait_us=900(+90)" % (active, ownership),
             p + "XlorieGles: " + " ".join(kv(k, gles.get(k, 0)) for k in ("egl_image_imports", "gles_copy_frames", "egl_swap_buffers")),
             p + "XlorieVk: " + " ".join(kv(k, vk.get(k, 0)) for k in (
                 "instance_created", "device_created", "surface_created", "swapchain_created", "ahb_import_attempts",
                 "ahb_import_success", "imported_source_draws", "imported_destination_draws", "copy_commands",
                 "blit_commands", "render_commands", "queue_submits", "queue_present_success", "frames_presented")),
             p + 'XlorieVkTiming: acquire_success=0(+0) selftest=pass device="Adreno (TM) 840" present_mode="FIFO"',
             p + "XlorieVkSelftest: result=%s compares=%d(+0) iterations=%d formats_passed=0x7 formats_unavailable=0x0 "
                 "us=2100000 fail_step=NONE fail_format=0 fail_direction=0 fail_iteration=0 fail_offset=0 fail_expected=0x00 "
                 "fail_actual=0x00 fail_vkresult=0" % (selftest, iterations * 9, iterations),
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
    ("vulkan while the X server keeps 3c98ffa's CPU locks", reports(3, active="VULKAN", gles={}, vk=VK, ownership="persistent"), "vulkan", 1),
    ("gles while the X server is per-access", reports(3, active="GLES", gles=GL, vk={}, ownership="per_access"), "gles", 1),
    ("vulkan after a failed ownership self-test", reports(3, active="VULKAN", gles={}, vk=VK, selftest="fail"), "vulkan", 1),
    ("vulkan after only the quick self-test", reports(3, active="VULKAN", gles={}, vk=VK, iterations=5), "vulkan", 1),
    ("vulkan without the self-test line", [l for l in vk_good if "XlorieVkSelftest" not in l], "vulkan", 1),
    ("an X server without the ownership field", [l.replace(" ownership=per_access", "") for l in vk_good], "vulkan", 1),
    ("backend changed within the window", report(0, active="GLES", gles=GL, vk={}) + reports(2, active="VULKAN", gles={}, vk=VK), "vulkan", 1),
    ("too few reports", reports(1, active="VULKAN", gles={}, vk=VK), "vulkan", 2),
    ("a line type missing", [l for l in vk_good if "XlorieFallback" not in l], "vulkan", 2),
]
for name, lines, expect, want in cases:
    code, out = run("renderer-proof.sh", lines, "--expect", expect)
    check(code == want, "%s: exit %d, want %d\n%s" % (name, code, want, out))
    check(("RESULT: PASS" in out) == (want == 0), "%s: result line does not match the exit status" % name)

code, out = run("renderer-status.sh", vk_good)
check(code == 0 and all(k in out for k in ("XlorieBackend:", "active=VULKAN", "ownership=per_access", "XlorieVkSelftest: result=pass", "XlorieGles:", "XlorieVk:", "XlorieVkTiming:", "XlorieFallback:")),
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

code, out = run("renderer-status.sh", vk_good)
check(not any(k in out for k in ("XlorieVkFramePacing:", "XlorieVkPipeline:", "XlorieRootCopyTiming:", "XlorieXLatency:")),
      "renderer-status: frame diagnostics lines out of nothing\n" + out)
if len(sys.argv) > 2:
    p = os.path.join(sys.argv[2], "diag-frame.log")
    r = subprocess.run(["sh", os.path.join(D, "renderer-status.sh"), "--file", p], capture_output=True, text=True)
    check(r.returncode == 0 and all(r.stdout.count(k) == 1 for k in ("XlorieVkFramePacing:", "XlorieVkPipeline:",
          "XlorieRootCopyTiming:", "XlorieXLatency:")) and "XlorieVkFramePacing: frame_start_us=0 " in r.stdout,
          "renderer-status on the real frame diagnostics log: %d\n%s" % (r.returncode, r.stdout))

# frame-diag-session.sh, with Android's logcat and log and the waits faked
def session(pacing):
    with tempfile.TemporaryDirectory() as t:
        fake = os.path.join(t, "bin")
        os.makedirs(fake)
        calls = os.path.join(t, "calls")
        tools = {
            "logcat": 'echo "logcat $*" >> %s\necho "10-21 10:00:01.000  9  9 I LorieNative: XlorieBackend: active=VULKAN"\n%s' % (
                calls, 'echo "10-21 10:00:06.000  9  9 I LorieNative: XlorieVkFramePacing: frame_start_us=0"' if pacing else ""),
            "log": 'echo "log $*" >> %s' % calls,
            "sleep": 'echo "sleep $*" >> %s' % calls,
        }
        for name, body in tools.items():
            with open(os.path.join(fake, name), "w") as f:
                f.write("#!/bin/sh\n" + body + "\n")
            os.chmod(os.path.join(fake, name), 0o755)
        out = os.path.join(t, "session.log")
        env = dict(os.environ, PATH=fake + os.pathsep + os.environ.get("PATH", ""), HOME=t)
        r = subprocess.run(["sh", os.path.join(D, "frame-diag-session.sh"), out], input="\n\n", capture_output=True,
                           text=True, env=env, timeout=60)
        saved = open(out).read() if os.path.exists(out) else ""
        made = open(calls).read() if os.path.exists(calls) else ""
        left = sorted(os.listdir(t))
    return r, saved, made, left

r, saved, made, left = session(True)
phases = ["phase=idle start", "phase=idle end", "phase=glmark2 start", "phase=glmark2 end", "phase=glmark2+move start",
          "phase=glmark2+move end"]
logged = [l[len("log -t LorieNative XlorieMark: "):] for l in made.splitlines() if l.startswith("log -t LorieNative XlorieMark: ")]
marked = [l.split("XlorieMark: ", 1)[1].rsplit(" at ", 1)[0] for l in saved.splitlines() if l.startswith("XlorieMark: ")]
sleeps = [l for l in made.splitlines() if l.startswith("sleep ")]
check(r.returncode == 0 and logged == phases and marked == phases, "frame-diag-session: exit %d, marks logged %s, in the file %s\n%s" % (
      r.returncode, logged, marked, r.stdout + r.stderr))
check(sleeps == ["sleep 10", "sleep 10", "sleep 20", "sleep 6"], "frame-diag-session: waits %s" % sleeps)
check("logcat -d -v threadtime -T " in made and " -s LorieNative:I" in made and
      saved.index("XlorieVkFramePacing:") < saved.index("XlorieMark: "), "frame-diag-session: the X server's lines not saved first\n" + saved)
check(left == ["bin", "calls", "session.log"] and "1 pacing reports" in r.stdout and "no XlorieVkFramePacing" not in r.stderr,
      "frame-diag-session: left %s, said %s" % (left, r.stdout + r.stderr))
r, saved, made, left = session(False)
check(r.returncode == 0 and "no XlorieVkFramePacing line" in r.stderr, "frame-diag-session without a pacing report: no warning\n" +
      r.stdout + r.stderr)

print("renderer diagnostics scripts (%d cases): %s (%d failures)" % (len(cases), "FAIL" if fails else "PASS", fails))
sys.exit(1 if fails else 0)

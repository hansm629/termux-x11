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
them, nothing else left behind; a warning when the session has no pacing report. P11-C3-D: the watchdog taken as on from
its armed line, or - that line older than the session - from XlorieBackend's freeze_diag=on or an XlorieXSched line; each
phase summed from the 5-second reports made wholly within it (by the log's marks), none from one that straddles its
start or end.

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
          "XlorieRootCopyTiming:", "XlorieXLatency:", "XlorieFrameGaps:", "XlorieComposite:", "XlorieCompositeClasses:",
          "XlorieRootCopySched:", "XlorieCopyDamage:")) and "XlorieVkFramePacing: frame_start_us=0 " in r.stdout and
          "XlorieContent" not in r.stdout, "renderer-status on the real frame diagnostics log: %d\n%s" % (r.returncode, r.stdout))
    # P11-C3-D0: the content lines, the latest of each, where the X server wrote them
    r = subprocess.run(["sh", os.path.join(D, "renderer-status.sh"), "--file", os.path.join(sys.argv[2], "diag-content.log")],
                       capture_output=True, text=True)
    check(r.returncode == 0 and all(r.stdout.count(k) == 1 for k in ("XlorieContentFrame:", "XlorieContentLatency:",
          "XlorieContentStall:", "XlorieContentClients:")), "renderer-status on the real content log: %d\n%s" % (r.returncode, r.stdout))

# frame-diag-session.sh, with Android's logcat and log and the waits faked; `lines`: more the fake logcat says
P = "10-21 10:00:%02d.000  9  9 I LorieNative: "
FREEZE = [P % 7 + "XlorieFreeze: phase=armed period_us=100000 begin_us=500000 levels_us=1000000,2000000,3000000 main_tid=9 "
          "input_tid=10 vsync_tid=11",
          P % 20 + "XlorieFreeze: phase=begin id=1 side=both stall_us=500000 x_stall_us=500000 r_stall_us=500000 pending=root,x,ticks "
          "stage=x:root_copy_wait+renderer:sleep x_stage=root_copy_wait x_stage_us=500000 x_request=139.7@3 x_main=S,futex:root_done "
          "ticks_posted=60 ticks_run=0 r_stage=sleep r_stage_us=0 surface=1 asks=4 taken=3 ask_wait_us=500000 copies_queued=0 "
          "copy_wait_us=0 copies_done=7 notified=7 acked=7 root=5/4/4/0 root_wait_us=500000 x_waiting_root=5 gpu_busy=0 lock_pid=0",
          P % 23 + "XlorieFreeze: phase=end id=1 duration_us=3100000 side=both stalled_stage=x:root_copy_wait+renderer:sleep "
          "last_stage=x:root_copy_wait+renderer:sleep pending_at_begin=root,x,ticks ended_by=root,ticks levels=3 x_main_run_us=120 "
          "x_main_runq_us=40 input_runq_us=3 vsync_runq_us=5",
          P % 25 + "XlorieFrameGaps: ask_wait_us=0 gaps_idle=4(+1) gaps_x_stalled=1(+1) gaps_unwoken=2(+1) gaps_late=3(+2) "
          "stall_ge_ms=100:6(+4),250:5(+3),500:4(+2),1000:3(+1),2000:2(+1),3000:1(+1) idle_ge_ms=100:4(+1),250:4(+1),500:0(+0),"
          "1000:0(+0),2000:0(+0),3000:0(+0) worst_stall_us=3100000 worst_stall_cause=unwoken worst_stall_ask_wait_us=3100000 "
          "worst_stall_wake_us=3000000 worst_stall_gap_us=3200000 worst_stall_ticks=2",
          P % 30 + "XlorieFrameGaps: ask_wait_us=0 gaps_idle=5(+1) gaps_x_stalled=1(+0) gaps_unwoken=2(+0) gaps_late=4(+1) "
          "stall_ge_ms=100:7(+1),250:5(+0),500:4(+0),1000:3(+0),2000:2(+0),3000:1(+0) idle_ge_ms=100:5(+1),250:4(+0),500:0(+0),"
          "1000:0(+0),2000:0(+0),3000:0(+0) worst_stall_us=150000 worst_stall_cause=late worst_stall_ask_wait_us=150000 "
          "worst_stall_wake_us=1000 worst_stall_gap_us=400000 worst_stall_ticks=40",
          P % 31 + "XlorieXSched: main=S,epoll_pwait,run_us=900000,runq_us=12000 input=S,run_us=3000,runq_us=200 vsync=S,run_us=1000,"
          "runq_us=100 watchdog_run_us=800 report_us=1500 report_max_us=2100 freezes=1"]
def session(pacing, lines=(), args=()):
    with tempfile.TemporaryDirectory() as t:
        fake = os.path.join(t, "bin")
        os.makedirs(fake)
        calls = os.path.join(t, "calls")
        tools = {
            "logcat": 'echo "logcat $*" >> %s\necho "10-21 10:00:01.000  9  9 I LorieNative: XlorieBackend: active=VULKAN"\n%s\n%s' % (
                calls, 'echo "10-21 10:00:06.000  9  9 I LorieNative: XlorieVkFramePacing: frame_start_us=0"' if pacing else "",
                "".join("echo '%s'\n" % l for l in lines)),
            "log": 'echo "log $*" >> %s' % calls,
            "sleep": 'echo "sleep $*" >> %s' % calls,
        }
        for name, body in tools.items():
            with open(os.path.join(fake, name), "w") as f:
                f.write("#!/bin/sh\n" + body + "\n")
            os.chmod(os.path.join(fake, name), 0o755)
        out = os.path.join(t, "session.log")
        env = dict(os.environ, PATH=fake + os.pathsep + os.environ.get("PATH", ""), HOME=t)
        r = subprocess.run(["sh", os.path.join(D, "frame-diag-session.sh")] + list(args) + [out], input="\n\n", capture_output=True,
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
check(sleeps == ["sleep 10", "sleep 20", "sleep 60", "sleep 6"], "frame-diag-session: waits %s" % sleeps)
check("logcat -d -v threadtime -T " in made and " -s LorieNative:I" in made and
      saved.index("XlorieVkFramePacing:") < saved.index("XlorieMark: "), "frame-diag-session: the X server's lines not saved first\n" + saved)
check(left == ["bin", "calls", "session.log"] and "1 pacing reports" in r.stdout and "no XlorieVkFramePacing" not in r.stderr,
      "frame-diag-session: left %s, said %s" % (left, r.stdout + r.stderr))
check("freezes: the watchdog not on" in r.stdout and "frame gaps" not in r.stdout, "frame-diag-session without the "
      "watchdog: said %s" % r.stdout)
r, saved, made, left = session(False)
check(r.returncode == 0 and "no XlorieVkFramePacing line" in r.stderr, "frame-diag-session without a pacing report: no warning\n" +
      r.stdout + r.stderr)
# P11-C3-C: the watchdog's freezes and the frame gaps summed up, the threads' last line; --move, --all
r, saved, made, left = session(True, FREEZE, ["--move", "5", "--all"])
sleeps = [l for l in made.splitlines() if l.startswith("sleep ")]
check(r.returncode == 0 and sleeps == ["sleep 10", "sleep 20", "sleep 5", "sleep 6"] and "logcat -d -v threadtime -s LorieNative:I" in made
      and " -T " not in made, "frame-diag-session --move 5 --all: exit %d, waits %s, logcat %s" % (r.returncode, sleeps, made))
check("freezes (TERMUX_X11_FRAME_FREEZE_DIAG): 1 begun, 1 ended" in r.stdout and
      "  freeze 1: 3100000 us, side both, stalled at x:root_copy_wait+renderer:sleep, last at x:root_copy_wait+renderer:sleep, "
      "waiting root,x,ticks, ended by root,ticks" in r.stdout and
      "    at its begin: x_main=S,futex:root_done x_request=139.7@3 asks=4 taken=3 copies_queued=0 root=5/4/4/0 lock_pid=0" in r.stdout and
      "frame gaps (100 ms or more): unwoken 1, late 3, x_stalled 1, idle 2; stalls >=100/250/500 ms 5/3/2, >=1/2/3 s 1/1/1; "
      "worst 3100000 us (unwoken)" in r.stdout and
      "X server threads (last 5 s): main=S,epoll_pwait,run_us=900000,runq_us=12000 " in r.stdout,
      "frame-diag-session summary:\n" + r.stdout)
r, saved, made, left = session(True, FREEZE[:2])
check("1 begun, 0 ended" in r.stdout and "(a freeze begun and not ended in the log)" in r.stdout, "frame-diag-session, a freeze not "
      "ended: said %s" % r.stdout)
r, saved, made, left = session(True, (), ["--move", "x"])
check(r.returncode == 2, "frame-diag-session --move x: exit %d" % r.returncode)

# P11-C3-D: the watchdog on, its armed line before the session (not in it); each phase from its whole reports
Q = "10-21 10:%s  9  9 I LorieNative: "
def phaseReport(at, scale=1):
    d = lambda n: "%d(+%d)" % (n * 10 * scale, n * scale)
    return [Q % at + l for l in (
        "XlorieBackend: requested=vulkan active=VULKAN x_gpu_wait_us=%s x_state_lock_wait_us=%s freeze_diag=on" % (d(100700), d(150000)),
        "XlorieVk: instance_created=1(+0) frames_presented=%s" % d(596),
        "XlorieRootCopy: root_gpu_copy=on root_gpu_copy_requests=%s root_copy_cpu_block_us=%s root_copy_pending=0" % (d(580), d(159000)),
        "XlorieContentFrame: requests=%s skipped=%s gpu_copies=%s cpu_copies=%s copies_done=%s completes=%s completes_skip=%s "
        "stale_reads=%s pending_reads=%s unknown_reads=%s compositor_updates=%s root_updates=%s root_publishes=%s screen_new=%s "
        "screen_reused=%s" % (d(4900), d(4410), d(490), d(0), d(490), d(490), d(4410), d(0), d(300), d(1), d(490), d(490), d(480),
                              d(480), d(116)),
        "XlorieContentLatency: present_to_copy_us=0 copy_to_complete_us=0 complete_to_root_us=0 root_to_publish_us=0 "
        "publish_to_screen_us=0 content_total_us=480/21000/30000/%s/60000/100/5" % (">100000" if scale > 1 else "40000"),
        "XlorieContentStall: content_interval_us=0 gaps_pending=%s gaps_client=%s max_gap_us=%d worst_content_us=0" % (
            d(1), d(0), 120000 * scale),
        "XlorieComposite: composites=%s px=1 us=%d cpu_us=%d gpu_us=%d root_us=%d lock_us=0 ahb_us=0" % (
            d(6000), 26000 * scale, 15500 * scale, 6000 * scale, 2000 * scale),
        "XlorieCopyDamage: exa_composite_opt=on deferred=%s done=%s early=%s dropped=%s pending=0" % (d(490), d(488), d(1), d(1)),
        "XlorieRootCopySched: root_copy_sched_opt=on vk_schedule_opt=on settled_acquired=%s settled_lock_wait=%s settled_x_waiting=%s" % (
            d(500), d(3), d(40)),
        "XlorieXSched: main=S,epoll_pwait,run_us=2600000,runq_us=%d input=S,run_us=1,runq_us=1 vsync=S,run_us=1,runq_us=1 "
        "watchdog_run_us=1 report_us=1 report_max_us=1 freezes=0" % (1020000 * scale))]
marks = [Q % at + "XlorieMark: phase=" + m for at, m in (
    ("01:00.000", "idle start"), ("01:10.000", "idle end"), ("01:20.000", "glmark2 start"), ("01:40.000", "glmark2 end"),
    ("01:45.000", "glmark2+move start"), ("02:45.000", "glmark2+move end"))]
r, saved, made, left = session(True, marks + [Q % "01:25.000" + "XlorieBackend: active=VULKAN freeze_diag=on",
                                              Q % "01:25.000" + "XlorieVk: frames_presented=5950(+595)"]
                               + [Q % "01:42.000" + "XlorieBackend: active=VULKAN freeze_diag=on"]      # straddles
                               + phaseReport("01:47.000", 7)                                          # straddles
                               + phaseReport("01:50.100") + phaseReport("01:55.100") + phaseReport("02:48.000", 7))
want = ["freezes (TERMUX_X11_FRAME_FREEZE_DIAG): 0 begun, 0 ended",
        "phase idle: no 5-second report wholly within it",
        "phase glmark2: 1 reports (5 s)",
        "  screen frames 119.0/s",
        "phase glmark2+move: 2 reports (10 s)",
        "  screen frames 119.2/s, with new client content 96.0/s, with the last one again 23.2/s",
        "  content: asked 980.0/s (skipped 882.0/s), copied by the GPU 98.0/s (completed 98.0/s), by the CPU 0.0/s; reads of a copy "
        "still queued: stale 0.0/s, pending 60.0/s, unknown 0.2/s; into the compositor 98.0/s, the screen pixmap 98.0/s, published 96.0/s",
        "  content to the screen: mean 21000 us, p99 up to 40000 us, max 60000 us; gaps of 50 ms or more: pending 2, client 0, "
        "longest 120000 us",
        "  X server per second: GPU wait 20.1 ms, lock and GPU wait 30.0 ms, root copy wait 31.8 ms, main thread runnable 204.0 ms; "
        "composites 1200.0/s, 5.2 ms (own 3.1, GPU wait 1.2, root copy wait 0.4)",
        "  root copies 116.0/s; copy damage put off 98.0/s (early 0.2/s, dropped 0.2/s); root copies settled early: once acquired 100.0/s, "
        "waiting for the lock 0.6/s, the X server waiting 8.0/s"]
got = r.stdout.splitlines()
check(r.returncode == 0 and all(w in got for w in want) and "phase glmark2+move: 2 reports (10 s)" in got and
      got.index(want[2]) + 1 == got.index(want[3]) and got.index(want[4]) + 5 == got.index(want[9]) and
      got.index(want[1]) < got.index(want[2]) < got.index(want[4]),
      "frame-diag-session phases:\n" + r.stdout + "\nwant\n" + "\n".join(want))
# the watchdog on from XlorieBackend alone, from XlorieXSched alone
for alone in (Q % "01:00.000" + "XlorieBackend: active=VULKAN freeze_diag=on", phaseReport("01:00.000")[-1]):
    r, saved, made, left = session(True, [alone])
    check("freezes (TERMUX_X11_FRAME_FREEZE_DIAG): 0 begun, 0 ended" in r.stdout, "frame-diag-session, the watchdog on from %s "
          "alone: said %s" % (alone.split("LorieNative: ")[1].split(":")[0], r.stdout))
r, saved, made, left = session(True, marks[4:] + phaseReport("01:50.100", 2)[:6])
check("  content to the screen: mean 21000 us, p99 up to >100000 us, max 60000 us; gaps of 50 ms or more: pending 2, client 0, "
      "longest 240000 us" in r.stdout.splitlines() and "freezes (TERMUX_X11_FRAME_FREEZE_DIAG): 0 begun, 0 ended" in r.stdout and
      "phase glmark2+move: 1 reports (5 s)" in r.stdout, "frame-diag-session, a percentile past the last bucket:\n" + r.stdout)


print("renderer diagnostics scripts (%d cases): %s (%d failures)" % (len(cases), "FAIL" if fails else "PASS", fails))
sys.exit(1 if fails else 0)

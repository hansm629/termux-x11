# Renderer diagnostics

The renderer runs in the Termux:X11 activity's process, whose logcat cannot be read without root or adb.
It therefore records what it actually did in the state it shares with the X server
(`struct lorie_renderer_diag`, `app/src/main/cpp/lorie/lorie.h`), and the X server logs that every 5 seconds
in its own log, which Termux can read with `logcat -d`:

```
XlorieBackend:  requested=auto|vulkan|gles active=GLES|VULKAN|NONE ownership=persistent|per_access
                ownership_transitions ownership_refused x_ahb_cpu_acquires=N(+d) x_state_lock_wait_us=N(+d)
XlorieGles:     egl_image_imports=N(+d) gles_copy_frames=N(+d) egl_swap_buffers=N(+d)
XlorieVk:       instance_created=... ahb_import_success=... imported_source_draws=... queue_submits=...
                queue_present_success=... frames_presented=... (and the rest)
XlorieVkTiming: acquire results, acquire/submit/fence/present times, self-test, device, present mode
XlorieFallback: total, by class (fatal_backend, resource_local, injected), last reason, VkResult, resource
```

`key=N(+d)`: N is the total since the X server started, d what it moved by since the previous report.
Every counter is moved only right after the API call it names returned success (fallback counters right
after the failure they record); none is set from a configuration value.

`requested` is `TERMUX_X11_RENDERER_BACKEND` as the X server read it (`(invalid-value)` appended when it
was set to anything else, which counts as auto). `ownership` is how the X server holds the AHardwareBuffers
at that moment (`struct lorie_backend_control`): `persistent` is 3c98ffa's lifetime CPU locks, `per_access`
the CPU lock only for each access, under the lock shared with the renderer - which the X server moves to only
when the renderer asks for it, and never in a session asked for gles. `x_ahb_cpu_acquires` and
`x_state_lock_wait_us` count those per-access locks and their wait for the shared lock.

## Scripts

`renderer-status.sh [--file LOG]` - prints the latest of those lines (and the GLES driver line).

`renderer-proof.sh --expect vulkan|gles [--reports N] [--file LOG]` - judges the last N reports (default 3,
15 seconds) and prints every check with its numbers; exit 0 PASS, 1 FAIL, 2 not enough reports.

- `--expect vulkan`: `active=VULKAN` and `ownership=per_access` in all N reports; `ahb_import_success`
  total > 0; over the reports
  `imported_source_draws`, `queue_submits`, `queue_present_success`, `frames_presented` > 0;
  `gles_copy_frames`, `egl_swap_buffers` and the fallback total did not move.
- `--expect gles`: `active=GLES` and `ownership=persistent` in all N reports; `gles_copy_frames`,
  `egl_swap_buffers` > 0;
  `queue_present_success`, `frames_presented` and the fallback total did not move.

Run the workload first, then the script while it is still running (or right after), so the last reports
cover it. Both scripts read `logcat -d` by default; `--file` takes a saved log instead.

`tests/host/tdiag.py` checks the scripts, including against logs written by the X server's real formatter.

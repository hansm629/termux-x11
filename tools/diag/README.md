# Renderer diagnostics

The renderer runs in the Termux:X11 activity's process, whose logcat cannot be read without root or adb.
It therefore records what it actually did in the state it shares with the X server
(`struct lorie_renderer_diag`, `app/src/main/cpp/lorie/lorie.h`), and the X server logs that every 5 seconds
in its own log, which Termux can read with `logcat -d`:

```
XlorieBackend:  active=GLES|VULKAN|NONE
XlorieGles:     egl_image_imports=N(+d) gles_copy_frames=N(+d) egl_swap_buffers=N(+d)
XlorieVk:       instance_created=... ahb_import_success=... imported_source_draws=... queue_submits=...
                queue_present_success=... frames_presented=... (and the rest)
XlorieVkTiming: acquire results, acquire/submit/fence/present times, self-test, device, present mode
XlorieFallback: total, by class (fatal_backend, resource_local, injected), last reason, VkResult, resource
```

`key=N(+d)`: N is the total since the X server started, d what it moved by since the previous report.
Every counter is moved only right after the API call it names returned success (fallback counters right
after the failure they record); none is set from a configuration value.

## Scripts

`renderer-status.sh [--file LOG]` - prints the latest of those lines (and the GLES driver line).

`renderer-proof.sh --expect vulkan|gles [--reports N] [--file LOG]` - judges the last N reports (default 3,
15 seconds) and prints every check with its numbers; exit 0 PASS, 1 FAIL, 2 not enough reports.

- `--expect vulkan`: `active=VULKAN` in all N reports; `ahb_import_success` total > 0; over the reports
  `imported_source_draws`, `queue_submits`, `queue_present_success`, `frames_presented` > 0;
  `gles_copy_frames`, `egl_swap_buffers` and the fallback total did not move.
- `--expect gles`: `active=GLES` in all N reports; `gles_copy_frames`, `egl_swap_buffers` > 0;
  `queue_present_success`, `frames_presented` and the fallback total did not move.

Run the workload first, then the script while it is still running (or right after), so the last reports
cover it. Both scripts read `logcat -d` by default; `--file` takes a saved log instead.

`tests/host/tdiag.py` checks the scripts, including against logs written by the X server's real formatter.

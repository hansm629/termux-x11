# Renderer diagnostics

The renderer runs in the Termux:X11 activity's process, whose logcat cannot be read without root or adb.
It therefore records what it actually did in the state it shares with the X server
(`struct lorie_renderer_diag`, `app/src/main/cpp/lorie/lorie.h`), and the X server logs that every 5 seconds
in its own log, which Termux can read with `logcat -d`:

```
XlorieBackend:  requested=auto|vulkan|gles vk_fail_at=none|instance|... active=GLES|VULKAN|NONE ownership=...
                ownership_transitions ownership_refused x_ahb_cpu_acquires=N(+d) x_state_lock_wait_us=N(+d)
                x_gpu_wait_us=N(+d) x_ahb_cpu_lock_us=N(+d)
XlorieGles:     egl_image_imports=N(+d) gles_copy_frames=N(+d) egl_swap_buffers=N(+d)
XlorieVk:       instance_created=... ahb_import_success=... imported_source_draws=... queue_submits=...
                queue_present_success=... frames_presented=... (and the rest)
XlorieVkTiming: acquire results, acquire/submit/fence/present times, self-test, device, present mode
XlorieVkSelftest: result=pass|fail|none compares=N(+d) iterations formats_passed formats_unavailable us
                fail_step fail_format fail_direction fail_iteration fail_offset fail_expected fail_actual
XlorieFallback: total, by class (fatal_backend, resource_local, injected), last reason, VkResult, resource
```

`key=N(+d)`: N is the total since the X server started, d what it moved by since the previous report.
Every counter is moved only right after the API call it names returned success (fallback counters right
after the failure they record); none is set from a configuration value.

`requested` is `TERMUX_X11_RENDERER_BACKEND` as the X server read it (`(invalid-value)` appended when it
was set to anything else, which counts as auto). `vk_fail_at` is `TERMUX_X11_RENDERER_VK_FAIL_AT`, the
point where the Vulkan backend is made to fail on purpose (instance, device, surface, swapchain, import,
submit, present; `none` when unset, and `(invalid-value)` when set to anything else, which injects
nothing): such a failure is counted in the `injected` fallback class. `ownership` is how the X server holds the AHardwareBuffers
at that moment (`struct lorie_backend_control`): `persistent` is 3c98ffa's lifetime CPU locks, `per_access`
the CPU lock only for each access, under the lock shared with the renderer - which the X server moves to only
when the renderer asks for it, and never in a session asked for gles. `x_ahb_cpu_acquires` counts those
per-access locks, `x_state_lock_wait_us` their whole wait before they could take the AHB (for the shared lock,
and for a Vulkan frame still using it), `x_gpu_wait_us` the part of it spent waiting for that frame, and
`x_ahb_cpu_lock_us` the time in the AHB's own CPU lock and unlock calls.

`XlorieVkSelftest` is the device self-test of how the renderer takes the X server's buffers over and gives
them back (vk.h, lorieVkSelftest): RGBA, RGBX and BGRA, CPU -> Vulkan -> CPU and Vulkan -> CPU -> Vulkan,
`iterations` times each, a byte-exact compare each time (RGBX's X byte excepted: undefined to Vulkan after
an external write). The Vulkan backend runs the quick one (5 iterations) when it starts, the full one (1000)
only when the X server is started with `TERMUX_X11_RENDERER_VK_SELFTEST=full`. `compares` counts the compares that passed;
`formats_passed` / `formats_unavailable` are bit masks (1 RGBA, 2 RGBX, 4 BGRA - only BGRA may be
unavailable). On a failure the `fail_*` fields say where: the step, the AHB format, the direction (1 or 2),
the iteration, the byte and its expected and actual value, and the VkResult.

## Scripts

`renderer-status.sh [--file LOG]` - prints the latest of those lines (and the GLES driver line).

`renderer-proof.sh --expect vulkan|gles [--reports N] [--file LOG]` - judges the last N reports (default 3,
15 seconds) and prints every check with its numbers; exit 0 PASS, 1 FAIL, 2 not enough reports.

- `--expect vulkan`: `active=VULKAN`, `ownership=per_access` and the full ownership self-test passed
  (`result=pass`, `iterations` >= 1000 - start the X server with `TERMUX_X11_RENDERER_VK_SELFTEST=full`) in
  all N reports; `ahb_import_success`
  total > 0; over the reports
  `imported_source_draws`, `queue_submits`, `queue_present_success`, `frames_presented` > 0;
  `gles_copy_frames`, `egl_swap_buffers` and the fallback total did not move.
- `--expect gles`: `active=GLES` and `ownership=persistent` in all N reports; `gles_copy_frames`,
  `egl_swap_buffers` > 0;
  `queue_present_success`, `frames_presented` and the fallback total did not move.

Run the workload first, then the script while it is still running (or right after), so the last reports
cover it. Both scripts read `logcat -d` by default; `--file` takes a saved log instead.

`tests/host/tdiag.py` checks the scripts, including against logs written by the X server's real formatter.

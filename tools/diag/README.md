# Renderer diagnostics

The renderer runs in the Termux:X11 activity's process, whose logcat cannot be read without root or adb.
It therefore records what it actually did in the state it shares with the X server
(`struct lorie_renderer_diag`, `app/src/main/cpp/lorie/lorie.h`), and the X server logs that every 5 seconds
in its own log, which Termux can read with `logcat -d`:

```
XlorieBackend:  requested=auto|vulkan|gles vk_fail_at=none|instance|... active=GLES|VULKAN|NONE ownership=...
                ownership_transitions ownership_refused x_ahb_cpu_acquires=N(+d) x_state_lock_wait_us=N(+d)
                x_gpu_wait_us=N(+d) x_ahb_cpu_lock_us=N(+d) root_buffers=1|2 x_root_copy_us=N(+d) vk_transfer_copy=on|off
XlorieGles:     egl_image_imports=N(+d) gles_copy_frames=N(+d) egl_swap_buffers=N(+d)
XlorieVk:       instance_created=... ahb_import_success=... imported_source_draws=... queue_submits=...
                queue_present_success=... frames_presented=... (and the rest)
XlorieVkTiming: acquire results, acquire/submit/fence/present times, self-test, device, present mode
XlorieVkSelftest: result=pass|fail|none compares=N(+d) iterations formats_passed formats_unavailable us
                fail_step fail_format fail_direction fail_iteration fail_offset fail_expected fail_actual
XlorieFallback: total, by class (fatal_backend, resource_local, injected), last reason, VkResult, resource
XlorieVkTransfer: (only with TERMUX_X11_RENDERER_VK_TRANSFER_COPY=1) checked copied not_* usage_missing_* imports_*
                last_not last_src last_dst last_rects last_first last_off
XlorieCopyLatency: copy_standalone_entries copy_frame_entries copy_standalone_submits copy_frame_submits
                copy_queue_us copy_lock_us copy_record_us copy_gpu_wait_us copy_total_us copy_ack_us
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
`x_ahb_cpu_lock_us` the time in the AHB's own CPU lock and unlock calls. `root_buffers` is 2 while root double
buffering is in effect (`TERMUX_X11_RENDERER_ROOT_DOUBLE=1`, experimental, Vulkan only): the X server draws into
one buffer of the screen pixmap while the renderer samples the other, and at each tick with damage the damage is
copied into the one sampled and they swap (`x_root_copy_us` is the time those copies took).
`vk_transfer_copy` is `on` when the X server was started with `TERMUX_X11_RENDERER_VK_TRANSFER_COPY=1`
(experimental, Vulkan only): a Present copy is then made by `vkCmdCopyImage` (counted in `XlorieVk`
`copy_commands`, one per queued copy) where that gives exactly the pixels the shader copy gives - one Vulkan format
and byte order, or a BGRA AHB into an RGBX one of depth 24 (B8G8R8A8_UNORM into R8G8B8A8_UNORM, size-compatible,
copied as raw bits: the shader copy swaps red and blue between the two byte orders, which gives those same bytes)
once this device copied that byte-exact (`bgra_rgbx_check`, below); an RGBX source only into an RGBX destination
below depth 32, both AHBs imported with the transfer
usage (asked for only where the device allows it for that AHB; refused, the AHB is imported without it), every
rectangle inside its source, no two destinations overlapping; every other copy is drawn as before
(`imported_destination_draws`).

`XlorieCopyLatency` times the Vulkan backend's Present copies (GLES's are not timed). Each `*_us` covers the 5
seconds since the previous report as `n:<samples>,avg:<mean>,p95:<bound>,max:<largest>` (`n:0` without samples):
p95 is the upper bound of the bucket the 95th percentile falls in (bounds 250, 500, 750, 1000, 1500, 2000, 3000,
4000, 6000, 8000, 12000, 16000, 25000, 33000, 66000 us; `>66000` past the last). Per queued entry:
`copy_queue_us`, from the X server queuing it to the renderer beginning the submission that records it (asking
for the shared lock; in a frame that is after the swapchain image was acquired), and `copy_total_us`, to its
completed serial published. Per submission that records copies (not per entry, so a submission of several is
timed once): `copy_lock_us`, the shared lock's wait; `copy_record_us`, from the lock to the submission made
(buffer lookups, imports, recording - in a frame the root and the cursor too); `copy_gpu_wait_us`, from the
submission to its fence waited for. `copy_ack_us`: from the renderer publishing a completion to the X server
having re-executed the Present work it completed, once per publish (measured against the latest publish).
`copy_standalone_*` and `copy_frame_*` count entries and submissions made on their own and with a frame.

`XlorieVkTransfer` says why copies were drawn instead. `checked` counts the queued copies looked at, `copied` is
`copy_commands`. Each `not_*` counts the copies one condition kept from `vkCmdCopyImage` (a copy can count under
several): `same` (one buffer), `format` (two Vulkan formats or byte orders, e.g. BGRA into RGBX), `rgbx_alpha`
(RGBX into a buffer with alpha), `depth32` (RGBX into RGBX of depth 32), `src_usage` / `dst_usage` (an image without
the transfer usage), `rect_source` (a rectangle empty or past its source), `rect_overlap` (two destinations
overlapping), `rect_none` (nothing left inside the destination). For each image without the usage,
`usage_missing_*` says what its import got: `unasked` (imported before a copy asked for it - cached without it),
`no_feature` (the format lacks the transfer features), `refused` (the AHB's image-format query refused it),
`failed` (the image made with it failed, made again without it). `imports_*` counts the imports that asked for it
by what they got (`granted` or the same reasons). `last_*` is the last copy drawn instead: its conditions, the
source and destination (AHB format, size, the destination's depth, AHB usage, Vulkan usage), its rectangles, the
first one and the offset. `bgra_rgbx_check` is the device check made once when Vulkan starts with the option on:
a BGRA AHB copied into an RGBX one at an offset, three patterns whose bytes all differ, compared byte by byte (each
pixel's first three bytes, RGBX's 4th being X padding) - `PASS`, `FAIL` (red and blue swapped, or any other wrong
byte: `check_step`, `check_byte` in the destination tightly packed, `check_expected`, `check_actual`,
`check_vkresult`; such copies are then drawn), `UNAVAILABLE` (no BGRA import with the transfer usage) or `NONE`.

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

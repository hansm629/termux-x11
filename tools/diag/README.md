# Renderer diagnostics

The renderer runs in the Termux:X11 activity's process, whose logcat cannot be read without root or adb.
It therefore records what it actually did in the state it shares with the X server
(`struct lorie_renderer_diag`, `app/src/main/cpp/lorie/lorie.h`), and the X server logs that every 5 seconds
in its own log, which Termux can read with `logcat -d`:

```
XlorieBackend:  requested=auto|vulkan|gles vk_fail_at=none|instance|... active=GLES|VULKAN|NONE ownership=...
                ownership_transitions ownership_refused x_ahb_cpu_acquires=N(+d) x_state_lock_wait_us=N(+d)
                x_gpu_wait_us=N(+d) x_ahb_cpu_lock_us=N(+d) root_buffers=1|2 x_root_copy_us=N(+d) vk_transfer_copy=on|off
                x_yield_on_wake=on|off root_copy_opt=on|off|fallback vk_submit_opt=on|off vk_frame_diag=on|off
                exa_lock_opt=on|off
XlorieGles:     egl_image_imports=N(+d) gles_copy_frames=N(+d) egl_swap_buffers=N(+d)
XlorieVk:       instance_created=... ahb_import_success=... imported_source_draws=... queue_submits=...
                queue_present_success=... frames_presented=... (and the rest)
XlorieVkTiming: acquire results, acquire/submit/fence/present times, copies_ahead, early_presents, self-test, device, present mode
XlorieRootCopy: root_gpu_copy=off|on|fallback, the root double buffering copies by the renderer's GPU (P11-C2)
XlorieSchedule: exa_lock_opt=on|off x_access_reserved x_access_reserve_full copies_deferred copies_deferred_wakes (P11-C3-B)
XlorieVkSelftest: result=pass|fail|none compares=N(+d) iterations formats_passed formats_unavailable us
                fail_step fail_format fail_direction fail_iteration fail_offset fail_expected fail_actual
XlorieFallback: total, by class (fatal_backend, resource_local, injected), last reason, VkResult, resource
XlorieVkTransfer: (only with TERMUX_X11_RENDERER_VK_TRANSFER_COPY=1) checked copied not_* usage_missing_* imports_*
                last_not last_src last_dst last_rects last_first last_off
XlorieCopyLatency: copy_standalone_entries copy_frame_entries copy_standalone_submits copy_frame_submits
                copy_queue_us copy_lock_us copy_inflight_wait_us copy_record_us copy_gpu_wait_us
                copy_frame_gpu_wait_us copy_total_us copy_ack_us
XlorieTick:     vsync_to_callback_us callback_to_tick_us ticks ticks_waiting ticks_waiting_max copy_notify_us
                copy_recheck_us copy_done_events copy_done_waiting copy_done_waiting_max stamps_invalid copy_early
                copy_retired copy_retired_live copy_cancelled
(only with TERMUX_X11_RENDERER_VK_FRAME_DIAG=1, P11-C3:)
XlorieVkFramePacing: frame_start_us present_delta_us wake_us idle_gaps stamps_invalid ticks_frame_wanted
                ticks_frame_missed worst_start_us worst_wake_us worst_acquire_us worst_lock_us worst_fence_us
                worst_present_us worst_after_root_us
XlorieVkPipeline: cpu_us acquire_us lock_us record_us submit_us fence_frame_us fence_root_us fence_copy_us present_us
                after_root_us
XlorieRootCopyTiming: take_us record_us resume_us x_block_us worst_total_us worst_take_us worst_record_us worst_fence_us
XlorieXLatency: request_us access_us access_lock_us access_gpu_us tick_delay_us tick_work_us worst_request_us
                worst_request=client:command:extension:major.minor access_hold_us worst_access_us
                worst_access=WxH:dest|src|mask:reserved|locked
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
`root_copy_opt` is `on` when the X server was started with `TERMUX_X11_RENDERER_ROOT_COPY_OPT=1` (experimental, with
root double buffering): its swap then locks the back for CPU reads only and the front for CPU writes only, each only
within the bounds of the damage copied, instead of both for reads and writes, whole. `fallback` once such a lock was
refused: the swaps of the session then take the whole-buffer locks again. Its effect shows in `x_ahb_cpu_lock_us` (all
the X server's AHB locks, not only the swap's), `x_root_copy_us` and `x_state_lock_wait_us`.
`vk_submit_opt` is `on` when the X server was started with `TERMUX_X11_RENDERER_VK_SUBMIT_OPT=1` (experimental,
Vulkan): the copies queued when a frame is drawn go ahead of it in a submission of their own in every present mode (as
under FIFO), published at their own fence; and a frame carrying none of them is presented right after it is
submitted, before its fence is waited for. `XlorieVkTiming` `copies_ahead` counts the frames whose copies went ahead
where they would have gone in the frame's (not under FIFO), `early_presents` the frames presented before their fence.
The frame's fence is still waited for before the next submission (one in flight), and so is the image's acquire fence.
`XlorieRootCopy` (P11-C2) says how root double buffering's copies went with `TERMUX_X11_RENDERER_ROOT_GPU_COPY=1`
(experimental, Vulkan, with `TERMUX_X11_RENDERER_ROOT_DOUBLE=1`): at a swap the copy of the damage into the buffer the
X server is to draw into is asked of the renderer's GPU and the buffers swap at once; the X server waits for the copy
only when it first touches that buffer. `root_gpu_copy_requests` asked, `root_gpu_copy_submits` and
`root_gpu_copy_completions` submitted and completed by the renderer (with their `_rects` and `_bytes`),
`root_copy_completion_us` the time from each request to its completion (not the GPU's time alone),
`root_cpu_fallbacks` the ones the X server copied on its CPU instead (the renderer could not, or the request was taken
back: no renderer to draw, the X server holding the shared lock, an ownership answer asked for), `root_cpu_small` the
swaps copied on the CPU at once being smaller than 1 MiB, `root_copy_cpu_block_us` the X server's time waiting for
them, `root_copy_pending` 1 while one is not settled. `fallback`: this period only the CPU settled them. The CPU
copies themselves still count in `XlorieBackend` `x_root_copy_us`.
`x_yield_on_wake` is `on` when the X server was started with `TERMUX_X11_X_YIELD_ON_WAKE=1` (experimental): a vsync
tick or a GPU copy's completion then breaks off the client dispatch round in progress at its next request boundary,
runs right away, and the round goes on with the rest of its slice - instead of waiting for the round to end. Its
effect shows in `XlorieTick`: `callback_to_tick_us` and `ticks_waiting_max`, `copy_recheck_us` and `copy_done_waiting_max`.

`XlorieCopyLatency` times the Vulkan backend's Present copies (GLES's are not timed). Each `*_us` covers the 5
seconds since the previous report as `n:<samples>,avg:<mean>,p95:<bound>,max:<largest>[,share:<x>%]` (`n:0`
without samples): p95 is the upper bound of the bucket the 95th percentile falls in (bounds 250, 500, 750, 1000,
1500, 2000, 3000, 4000, 6000, 8000, 12000, 16000, 25000, 33000, 66000 us; `>66000` past the last).

Per queued entry: `copy_queue_us`, from the X server queuing it to the renderer beginning the submission that
records it (asking for the shared lock; in a frame that is after the swapchain image was acquired), and
`copy_total_us`, to its completed serial published.

Per submission that records copies (n, avg, p95 and max count a submission of several entries once):
`copy_lock_us`, the shared lock's wait; `copy_inflight_wait_us`, under the lock, the wait for a frame another
renderer process listed in flight to be over (lorieGpuWaitIdle); `copy_record_us`, the rest up to the submission
made (buffer lookups, imports, recording - in a frame the root and the cursor too); from the submission to its
fence waited for, `copy_gpu_wait_us` for the copies submitted alone and `copy_frame_gpu_wait_us` for a frame
that carries copies - that one is the frame's whole GPU time (its copies, the root and the cursor), not the
copies' own, so it says nothing about copy execution by itself.

`share` puts the parts in one unit: entry time. A submission's part counts once for each entry it held up (each
waited through it); `share` is that over the entries' `copy_total_us` added up for the same 5 seconds. The parts
of an entry's time - queue, lock, in flight, record, the fence (one of the two), and a short tail from the fence
to the publish - add up to its total, so the shares of queue, lock, in flight, record and the two fences add up
to a little under 100%. Compare shares, not one part's mean with another's: the means are per entry for queue
and total and per submission for the rest.

`copy_ack_us`: from the renderer publishing a completion to the X server having re-executed the Present work it
completed, once per publish (measured against the latest publish); after the total, so it has no share.
`copy_standalone_*` and `copy_frame_*` count entries and submissions made on their own and with a frame.

`XlorieTick` follows the X server's own ticks and GPU copy completions to its thread (latencies as above, without
shares). A tick: `vsync_to_callback_us`, from the vsync's frame time Choreographer gives to its callback (64-bit
builds only: a 32-bit `long` cannot hold the frame time), and `callback_to_tick_us`, from that callback to the tick
(lorieRedraw) running on the X server's thread. A completion: `copy_notify_us`, from the Vulkan renderer publishing
it to the X server's input thread receiving it, and `copy_recheck_us`, from there to the X server's thread done with
the recheck it caused. Each pair is one event's own two CLOCK_MONOTONIC stamps - the tick and the recheck carry
theirs to the X server's thread - and a pair out of order or over 10 s apart is counted in `stamps_invalid`, not
timed. `ticks` and `copy_done_events` count those posted to the X server's thread; `*_waiting` is how many of them
were posted but had not run when the report was made (waiting, not lost), `*_waiting_max` the most still waiting
behind one when it ran since the previous report. `copy_early` counts GPU-copied presentations that came to an
end, or to a poll that could not be queued, before their copy was done: `scrap` overwritten by a new presentation in
the same frame, `destroy` with their window, `first_poll` / `later_poll` when the poll for the copy could not be
queued (each time), `stalled` when the renderer had no surface or no connection (once per presentation). Before
P10-S1 each of these let the X server's references go and told the client its buffer was idle while the copy could
still read it (`tests/host/tpresentsafety.c`); now they are only attempts. What kept them safe: `copy_retired`, a
presentation's copy retired - its buffers, idle fence and owed idle notification kept past the presentation until the
copy settles - and `copy_retired_live`, those still held now; `copy_cancelled`, copies the X server cancelled before
any renderer recorded them (the renderer skips them, completing their serial): those let go at once. A poll not
queued, or a stalled copy still running, is simply waited for (rechecked at every completion and tick).

### Frame pacing (P11-C3, `TERMUX_X11_RENDERER_VK_FRAME_DIAG=1`)

Diagnostics only, off by default: started with `TERMUX_X11_RENDERER_VK_FRAME_DIAG=1` the X server writes four more
lines every 5 seconds and `XlorieBackend` says `vk_frame_diag=on`. Off, none of these timings reads a clock - the
renderer's frames, the root copies and the X server's requests, ticks and accesses are timed only while it is on; the
existing lines (`XlorieFrames` with its GLES frame statistics included) stay as they are either way. Everything is
CLOCK_MONOTONIC microseconds, gathered into fixed histograms in the shared state (no per-frame log, no lock):

`key=n/avg/p95/p99/max/h16/h33`: the samples of the last 5 seconds - how many, their mean, p95 and p99 (each the bound
of the bucket it falls in; the bounds are 50 us .. 8 ms, 8.5, 9, 10, 12, 14, 16.667, 20, 25, 33.333, 50, 100 ms, and
`>100000` past the last), the largest, and how many were over 16.667 ms (two 120 Hz refreshes) and over 33.333 ms.
`key=0` when there were none. Counters are `N(+d)` as above; `worst_*` belong to one item, the period's largest, and
are taken back to 0 with each report (the renderer may write them while the X server reads: a near-simultaneous
pair can mix two frames).

`XlorieVkFramePacing`, one sample per Vulkan screen frame presented: `frame_start_us`, from the last frame's start to
this one's (the interval a viewer sees as pacing); `present_delta_us`, from the last `vkQueuePresentKHR` call to this
one's; `wake_us`, from the X server's tick asking for a frame to the renderer starting it (only for a request made
after the last frame started - an older one that frame took up). The first frame on a swapchain, window, X server
session or device has no interval (no false hitch); an interval over 250 ms is an idle desktop, counted in
`idle_gaps`, not timed; a stamp before the last is counted in `stamps_invalid`. `ticks_frame_wanted` counts the ticks
that asked the renderer for a frame, `ticks_frame_missed` the ticks that found no frame presented (Vulkan's or
GLES's) since the last one asked - a frame later than one refresh. `worst_start_us` is the period's largest frame
interval, and the frame that ended it spent `worst_wake_us` waking, `worst_acquire_us` in `vkAcquireNextImageKHR`,
`worst_lock_us` waiting for the X server's lock, `worst_fence_us` on its own fence, `worst_present_us` in
`vkQueuePresentKHR`, and `worst_after_root_us` in a root copy made right before it.

`XlorieVkPipeline`, the same frames' parts: `cpu_us` from the frame's start to its end (everything below and the
rest), `acquire_us`, `lock_us` (the X server's shared lock), `record_us` (from holding the lock to submitting: the
copies taken, images imported, the frame recorded), `submit_us` (`vkQueueSubmit`), `present_us` (the present call).
Three fences, each counted once, in its own key: `fence_frame_us` the screen frame's own fence (with the Present
copies it carries, when they go in its submission), `fence_root_us` a root copy's (P11-C2), `fence_copy_us` the Present
copies sent in a submission of their own (P11-C1, FIFO, or no frame); together they make up `vk_fence_wait_us` in
`XlorieVkTiming`. `after_root_us` is a root copy made in the same renderer loop pass right before a frame, the whole
of it (its fence included) - time that frame waited behind it.

`XlorieRootCopyTiming`, the root double buffering copies by the renderer's GPU (`TERMUX_X11_RENDERER_ROOT_GPU_COPY=1`):
`take_us` from the X server's request to the renderer taking it, `record_us` from there to its submission; its fence
is `fence_root_us` above. Where the X server had to wait for one (its next access to the buffer, or its next swap):
`x_block_us` that wait, `resume_us` from the renderer settling the copy to the X server going on. A copy settled
before the X server needed it costs no wait and is in neither. `worst_*`: the period's slowest copy, from its request
to its completion, and its parts.

`XlorieXLatency`, the X server's side: `request_us` each client request's handling in Dispatch, and the period's
slowest as `worst_request=client:command:extension:major.minor` (the client's index and command name, `core` for a
core request); `access_us` how long an EXA access to an AHB under PER_ACCESS (Vulkan) held the shared lock - the
renderer cannot record a frame meanwhile - `access_lock_us` its wait for that lock, `access_gpu_us` its wait for a
Vulkan frame using the AHB; `tick_delay_us` from the vsync callback to the tick running on the X server's thread
(`callback_to_tick_us` of `XlorieTick` with its tail), `tick_work_us` the tick's work up to asking for the frame.

With P11-C3-B1, `access_hold_us` is how long each such access kept the shared lock - all of it, or, reserved (below),
only while reserving and letting go - and `worst_access_us` / `worst_access` the period's slowest access: its pixmap's
size, EXA's index, and whether it was reserved or held the lock.

Reading them: a hitch shows as `frame_start_us` h16/h33; where its time went is in the same period's other keys and
the worst frame's breakdown - waiting for the tick (`wake_us`, `tick_delay_us`, a long `request_us`), for the X
server's lock (`lock_us`, `access_us`), behind a root copy (`after_root_us`, `x_block_us`), on the GPU (the fences), or
in the presentation engine (`acquire_us`, `present_us`). `tools/diag/frame-diag-session.sh` records one session in
three marked phases for this.

### EXA accesses without the shared lock (P11-C3-B1, `TERMUX_X11_EXA_LOCK_OPT=1`)

Experimental, off by default. Under PER_ACCESS (Vulkan) every CPU access to an AHB used to hold the lock shared with
the renderer for the whole access - the software drawing included (EXA has no GPU acceleration here, so an XRender
Composite is pixman on the CPU) - and the renderer's frame waited for it (`lock_us`). With the option an access to
any AHB but the root the X server publishes (the one the renderer samples) holds a reservation instead
(`struct lorie_shared_server_state` `cpuAccess`): under the lock, once no submitted Vulkan frame uses the AHB, the AHB
is reserved and the lock let go; the CPU lock is taken after that, and let go before the reservation is, under the
lock again. A renderer records no work using a reserved AHB: a queued copy that would use one is left queued, with
the copies after it, and the frame goes on without them; the X server wakes the renderer when it lets the
reservation go. Accesses to the published root, and accesses when all 8 reservations are taken, hold the lock as
before.

`XlorieSchedule`: `x_access_reserved` - accesses made under a reservation; `x_access_reserve_full` - those that held
the lock because none was free; `copies_deferred` - renderer passes that left a copy queued for a reservation;
`copies_deferred_wakes` - the X server's wakes of a renderer that did.

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

`frame-diag-session.sh [OUT]` (P11-C3) - with the X server started with `TERMUX_X11_RENDERER_VK_FRAME_DIAG=1`: three
phases marked in the log (`XlorieMark: phase=...`, also in the file with their times) - the screen left alone for 10 s,
glmark2 alone for 10 s, glmark2 while XFCE windows are moved for 20 s - each started when you press Enter, then the
X server's `LorieNative` lines from the session's start saved to OUT (default `~/frame-diag-<date>-<time>.log`).

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

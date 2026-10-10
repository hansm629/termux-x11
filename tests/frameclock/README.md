# Frame clock tests and diagnostics

`app/src/main/cpp/lorie/frameclock.c` drives the X server's frames:

```
VSYNC -> AChoreographer callback (owner thread: the X server process' main Looper)
      -> QueueWorkProc(lorieRedraw), at most one queued    -> X server main thread
      -> lorieRedraw: MSC += ticks, Present vblanks, waitForNextFrame = false
      -> renderer (activity process) draws, sets waitForNextFrame
```

`app/src/main/cpp/lorie/flowstats.c` measures what feeds those frames: input -> X server -> damage ->
draw request -> renderer wakeup (XlorieFlow lines below).

## Host tests

`sh tests/frameclock/run.sh` (Termux: `CC=clang`) builds `tframeclock.c` against test doubles of the
Android APIs and runs it, `tflowstats.c` (flowstats.c, plus the renderer's flow counters cut out of
renderer.c), then `syntax.sh` checks every touched lorie source for Android arm64 and
armv7 with the CI's error flags, against a patched copy of the xserver tree (the submodule must be a
real checkout; it is never patched in place).

## Reading the 5 second lines

The X server logs them with tag `LorieNative` (Termux: `logcat -d -v threadtime -s LorieNative`).
Steady lines only print while a surface is on screen; stalls, replays, re-arms and long X server
busy stretches print regardless. `TERMUX_X11_DEBUG=1` prints them always.

`XlorieFrameClock:` callback and work queue

| field | meaning |
|---|---|
| `cb`, `post` | valid callbacks / callbacks posted in the window |
| `cb_gap_max_us`, `cb_hist` | callback gaps; histogram `<4/<12/<25/<50/<100/>=100 ms` |
| `vsync_gap_min_us`, `vsync_gap_max_us` | gaps between the VSYNC timestamps the callbacks were handed |
| `cb_lat_max_us`, `cb_late` | callback run time minus its VSYNC timestamp; callbacks >= 4 ms late |
| `gen`, `stale_cb` | chain generation; callbacks of an older generation that were dropped |
| `stall`, `rearm`, `forced_rearm`, `resume_check` | watchdog stalls (>= 100 ms quiet), re-arms, re-arms that replaced a still registered callback, resume checks |
| `queue_try`, `queue_actual`, `queue_coalesced`, `requeue` | ticks, lorieRedraws queued, ticks counted onto a queued one, re-queues of a stuck one |
| `redraw`, `replay`, `ticks_max`, `multi_tick` | lorieRedraw runs, runs that found no tick, most ticks taken at once, runs that took > 1 |
| `cb_to_redraw_max_us`/`_avg_us`, `redraw_gap_max_us`, `redraw_hist` | first tick -> lorieRedraw delay, gaps between runs |

`XlorieFrameClockR:` renderer hand-off (shared state)

| field | meaning |
|---|---|
| `render`, `surface`, `connected`, `sgen` | frames drawn, surface/connection state, surfaces applied so far |
| `wait_set`, `wait_clear`, `wait_over_tick` | waitForNextFrame set by frames / cleared by ticks while set / sets that overwrote a tick's clear |
| `handover`, `handover_late`, `handover_late_max_ticks`, `handover_max_us` | frames started for a tick's draw request; those that started one or more ticks late; tick -> frame start otherwise |
| `lock_hold_max_us`, `buf_wait_max_us` | longest state->lock hold by a frame; longest wait for a GPU-copy buffer |

`XlorieFrameClockSched:` the choreographer and X server main threads: `cpu`, `allowed` (affinity),
`prio`, `nice`, `run_ms` / `wait_ms` (on CPU / runnable but waiting, from schedstat), `majflt`; then
`x_busy_max_us` (longest stretch the X server main thread stayed awake), `lock_wait_*` (X server
waiting for state->lock). Cgroup changes of either thread are logged when they happen.

## Which stage a gap is in

| pattern | stage |
|---|---|
| `cb_gap` large, `vsync_gap_max` as large, `cb_lat` small | no VSYNC was delivered to the X server process (SurfaceFlinger side) |
| `cb_gap` large, `vsync_gap_max` ~ one frame, `cb_lat` large | the callback thread did not run: `wait_ms` up = starved of CPU, both flat = blocked/frozen |
| callbacks regular, `cb_to_redraw` large, `queue_coalesced`/`ticks_max` up, `x_busy_max_us` large | X server main thread (`lock_wait_*` up: waiting for the renderer) |
| callbacks and redraws regular, `handover_late` / `wait_over_tick` up | renderer hand-off (waitForNextFrame) |
| all of the above regular, renderer `frame avg` still bad | inside the renderer frame (XloriePerf root_wait/swap) |

## Input -> damage -> draw request -> renderer (XlorieFlow / XlorieFlowR)

Printed right after the frame clock lines, same 5 second window, same gap buckets
(`<4/<12/<25/<50/<100/>=100 ms`), so each stage's cadence can be put next to the next one's.

Note: `XlorieFrames` (frame avg/max/hitches) measures renderer frame to renderer frame. A stretch with
nothing new to draw counts there exactly like a stall; `gap_long_idle` / `gap_long_pending` below
tell the two apart.

`XlorieFlow:` X server side, in pipeline order

| field | stage | meaning |
|---|---|---|
| `in_motion`, `in_drag`, `in_btn`, `in_scroll`, `in_touch`, `in_stylus`, `in_key` | input | events read from the activity's socket (input thread); `in_drag` = motion with a button / touch down |
| `in_gap_max_us`, `in_hist` | input | gaps between motion events (pauses included) |
| `in_drag_gap_max_us` | input | gaps between motion events *within* one drag |
| `inject`, `inject_gap_max_us` | inject | events handed to the X server (QueuePointerEvents / QueueTouchEvents) |
| `cursor`, `cursor_gap_max_us`, `in_to_cursor_max_us` | cursor | lorieMoveCursor runs; first unhandled motion -> sprite moved |
| `dmg_check`, `dmg_on`, `dmg_off`, `dmg_skip` | damage | lorieRedraw ticks that looked at root damage: non-empty / empty / skipped (no connection or surface) |
| `dmg_gap_max_us`, `dmg_hist` | damage | gaps between ticks with damage |
| `in_to_dmg_max_us` | damage | first undrawn drag motion -> the tick that found damage |
| `drag_nodmg_max_us` | damage | longest time drag motion kept waiting with no damage at all (released drags included) |
| `req`, `req_already` | request | drawRequested false -> true / damage while the previous request was still pending |
| `req_gap_max_us`, `req_hist` | request | gaps between draw requests |
| `sig_draw`, `sig_cursor` | request | rendererCond signals from lorieRedraw / lorieMoveCursor |

`XlorieFlowR:` renderer side (shared state)

| field | meaning |
|---|---|
| `frames`, `frame_hist` | frames drawn, gaps between them |
| `f_draw`, `f_cursor`, `f_other` | what each frame was for: a draw request, the cursor only, anything else (GPU copy, surface) |
| `req_to_frame_max_us`/`_avg_us`, `cursor_to_frame_max_us` | request (or first cursor move) -> frame start |
| `req_to_frame_invalid`, `cursor_to_frame_invalid` | stamps newer than the frame's own start (a request made right after it began): left out of the latencies |
| `gap_long_idle`, `gap_idle_max_us` | frame gaps >= 33 ms with no request pending for that long: nothing to draw |
| `gap_long_pending`, `gap_pending_max_us` | frame gaps >= 33 ms with a request pending >= 33 ms: the renderer was late |
| `wait` | pthread_cond_wait calls of the render loop |
| `wake_draw`, `wake_cursor`, `wake_gpucopy`, `wake_state`, `wake_gated`, `wake_none` | what was pending when it returned (`gated`: work, but waitForNextFrame or no surface) |
| `sw_waitframe`, `sw_buffers`, `sw_nosurface`, `sw_idle` | why rendererShouldWait() sent it to sleep (`idle`: nothing to do) |
| `limiter`, `limiter_max_us` | deliberate timed waits of the coalesce / high-refresh limiter |

## Android -> activity -> socket -> X server (XlorieInput)

Printed right after XlorieFlowR, same window. The activity side (`a_*`, inputflow.c) is counted in the
activity and read through the shared state; the socket is timed by matching each pointer event the X
server reads against a log the activity writes right before each `write()` (key over the event's
fields, in order; the socket, its protocol and the event struct are unchanged). All times are
CLOCK_MONOTONIC, which is also the time base of MotionEvent event times (SystemClock.uptimeMillis);
`a_clock_bad` counts event times that do not fit it.

Code path: `MainActivity` touch / hover / generic-motion / captured-pointer listeners ->
`TouchInputHandler.handleTouchEvent` (noted here, once) -> input strategy / `GestureDetector` ->
`onScroll` -> `moveCursorByOffset` / `moveCursorToScreenPoint` -> `InputEventSender.sendCursorMove`
(or `sendTouchEvent` in direct touch mode, `sendStylusEvent`) -> `LorieView.sendMouseEvent` /
`sendTouchEvent` / `sendStylusEvent` (activity.c) -> `write(conn_fd)` -> X server input thread
`handleLorieEvents` -> `QueuePointerEvents` (touch: `QueueWorkProc(handleTouchEvent)` on the main
thread, then `QueueTouchEvents`) -> main thread `ProcessInputEvents` delivers to clients.

| field | stage | meaning |
|---|---|---|
| `a_events`, `a_moves`, `a_drag`, `a_hist` | Android | MotionEvents the activity got, MOVE / HOVER_MOVE among them, drag moves (finger / button down), batched historical samples |
| `a_drag_sample_gap_max_us` | Android | gap between the input samples themselves (event times, historical included) of consecutive drag moves |
| `a_drag_cb_gap_max_us` | Android | gap between the callbacks that delivered drag moves |
| `a_ev_to_cb_max_us`, `a_oldest_to_cb_max_us` | Android | event time (oldest sample) -> callback |
| `a_clock_bad` | Android | event times in the future or > 10 s old |
| `a_send`, `a_send_drag`, `a_send_drag_gap_max_us` | activity | pointer events written to the socket; drag motion among them (same rule as `in_drag`) and its gaps |
| `a_cb_to_send_max_us`, `a_write_max_us`, `a_write_fail` | activity | last MotionEvent callback -> motion write; time in `write()`; short writes |
| `xmit`, `xmit_unmatched`, `xmit_max_us`, `xmit_avg_us` | socket | events matched to the activity's write and their write -> read time; reads it had no entry for |
| `x_rx_drag`, `x_rx_drag_gap_max_us` | X server | the same as XlorieFlow's `in_drag` / `in_drag_gap_max_us`, repeated for the side by side |
| `touch_queue_max_us` | X server | touch read -> handleTouchEvent on the main thread |
| `x_process`, `inject_to_process_max_us` | X server | main thread runs that took up injected input; injection -> that run |

Drag rules differ only where the input does: on a touchscreen in trackpad mode every finger move is an
`a_drag`, but `a_send_drag` / `in_drag` need the X button held (double tap and drag). Compare `a_send_drag`
with `in_drag` (same rule on both ends of the socket), and `a_drag` with both for the Android side.

| pattern (one drag, same window) | stage |
|---|---|
| `a_drag_sample_gap_max_us` ~100 ms | no input samples were produced: the finger stopped, or before Android delivered anything |
| samples regular, `a_drag_cb_gap_max_us` / `a_ev_to_cb_max_us` large | delivered to the activity late (input dispatch / batching / its main thread busy) |
| callbacks regular, `a_send_drag_gap_max_us` or `a_cb_to_send_max_us` large | the activity's conversion / sending |
| sends regular, `xmit_max_us` or `x_rx_drag_gap_max_us` large | socket / X server input thread |
| received regular, `touch_queue_max_us` / `inject_to_process_max_us` large | X server input processing |
| all of the above regular, `drag_nodmg_max_us` large | X clients / compositor (damage) |

None of this follows a frame to the screen: `gap_long_idle` / `gap_long_pending` stop at the renderer's
frame start, not at Android presenting it.

### Where a stutter is, one 5 second window at a time

| pattern | stage |
|---|---|
| `in_drag_gap_max_us` large while dragging | input reached the X server late: XlorieInput says which side of the socket |
| input regular, `in_to_cursor_max_us` large | the X server handled the input late |
| input regular, `drag_nodmg_max_us` / `dmg_gap_max_us` large | nothing was drawn for it: X clients / compositor, or the X server main thread (`x_busy_max_us` in XlorieFrameClockSched) |
| damage regular, `req` regular, `gap_long_pending` > 0 or `req_to_frame_max_us` large | renderer late with work; `wake_*` / `sw_*` / `limiter` say why |
| `gap_long_idle` only, damage gaps match the frame gaps | idle: nothing new to show, not a renderer hitch |

### Tests, each compared stage by stage in the same windows

| test | expect when healthy |
|---|---|
| A. glmark2 only | `in_*` ~ 0; `dmg_on` ~ glmark2's frame rate, `dmg_hist` in its bucket; `req` + `req_already` ~ `dmg_on`; `frames` ~ `req`; `gap_long_pending` = 0 |
| B. window drag only | `in_drag` at the touch/pointer rate, small `in_drag_gap_max_us`; `dmg_on` follows the drag, `drag_nodmg_max_us` a frame or two; `frames` ~ `req` |
| C. glmark2 + window drag | as A and B together; the first stage whose cadence breaks (input, cursor, damage, request, renderer) is where the stutter is |

## Fault injection

Off unless set in the X server's environment; each fault fires once per 10 s, staggered from
start (10 s, 15 s, 20 s, 25 s), and is logged as `XlorieFrameClock: test: ...`:

| variable | fault | expected |
|---|---|---|
| `TERMUX_X11_CHOREO_TEST_DROP=n` | a callback does not re-post (n times) | `re-arming the frame callback (lost)` ~100 ms later, `rearm=1 stale_cb=0`, normal cadence after |
| `TERMUX_X11_CHOREO_TEST_REARM=n` | a healthy chain is re-armed (n times) | `stale_cb=1 forced_rearm=1`, `cb` per window unchanged (no double cadence) |
| `TERMUX_X11_CHOREO_TEST_BLOCK_MS=ms` | the choreographer thread sleeps after a callback | `cb_lat_max_us` ~ ms with `vsync_gap_max_us` ~ one frame |
| `TERMUX_X11_FRAMECLOCK_TEST_XSTALL_MS=ms` | the X server main thread sleeps | callbacks regular, `queue_coalesced` ~ ms / frame, `ticks_max` ~ the same, `replay=0`, `x_busy_max_us` ~ ms |

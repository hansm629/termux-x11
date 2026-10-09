# Frame clock tests and diagnostics

`app/src/main/cpp/lorie/frameclock.c` drives the X server's frames:

```
VSYNC -> AChoreographer callback (owner thread: the X server process' main Looper)
      -> QueueWorkProc(lorieRedraw), at most one queued    -> X server main thread
      -> lorieRedraw: MSC += ticks, Present vblanks, waitForNextFrame = false
      -> renderer (activity process) draws, sets waitForNextFrame
```

## Host tests

`sh tests/frameclock/run.sh` (Termux: `CC=clang`) builds `tframeclock.c` against test doubles of the
Android APIs and runs it, then `syntax.sh` checks every touched lorie source for Android arm64 and
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

## Fault injection

Off unless set in the X server's environment; each fault fires once per 10 s, staggered from
start (10 s, 15 s, 20 s, 25 s), and is logged as `XlorieFrameClock: test: ...`:

| variable | fault | expected |
|---|---|---|
| `TERMUX_X11_CHOREO_TEST_DROP=n` | a callback does not re-post (n times) | `re-arming the frame callback (lost)` ~100 ms later, `rearm=1 stale_cb=0`, normal cadence after |
| `TERMUX_X11_CHOREO_TEST_REARM=n` | a healthy chain is re-armed (n times) | `stale_cb=1 forced_rearm=1`, `cb` per window unchanged (no double cadence) |
| `TERMUX_X11_CHOREO_TEST_BLOCK_MS=ms` | the choreographer thread sleeps after a callback | `cb_lat_max_us` ~ ms with `vsync_gap_max_us` ~ one frame |
| `TERMUX_X11_FRAMECLOCK_TEST_XSTALL_MS=ms` | the X server main thread sleeps | callbacks regular, `queue_coalesced` ~ ms / frame, `ticks_max` ~ the same, `replay=0`, `x_busy_max_us` ~ ms |

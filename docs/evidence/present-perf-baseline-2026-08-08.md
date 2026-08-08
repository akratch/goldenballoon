# Present-path cost baseline and occlusion/frame-latency parity

Date: 2026-08-08
Branch: `worktree-presentation-gold-standard`
Host: Apple M3 Max, macOS arm64, 60 Hz display, AppleClang release build (`build-rel`)
ROM: US v80 Rev 1, validated by the runtime banner
Binary: `build-rel/mdkr64` at commit e9fa33a

This note records two things: the parity answers for the three occlusion and
frame-latency questions, and the fresh present-path cost census that later
presentation work measures against. It replaces the 103 submissions/s WebGPU
figure, which predates the tear-free present-mode policy.

## Decision

Two of the three defenses were already in place and are cited below. The third
— swap-chain depth — was not pinned at all and is now pinned to the backend
minimum (commit e9fa33a), on the latency argument alone and at a documented
throughput trade this session could not measure.

What this note does **not** establish is listed under "Explicitly open" rather
than left to inference: the throughput cost of the depth pin, WebGPU surface
acquire and present cost, and one occlusion gap that is a wasted-work cost
rather than the stall the reference port was defending against. All four share
one cause — no un-occluded foreground window was obtainable here.

## The three questions

### (a) Is acquire+present skipped when the window is occluded or minimized?

**Hidden and minimized: yes, fully — the whole render is skipped, not merely
the clock rebased.**

`platform_surface_visibility_update()` samples the window every frame
(`platform/platform_sdl_min.c:1706-1758`) and publishes the verdict through
`present_sched_set_surface_elided()` (`platform/present_sched.c:419-425`).
Downstream:

- The emulated graphics task completes **without walking the display list and
  without opening a frame transaction** — `gfx_start_frame` is never reached,
  so there is no surface acquire at all (`platform/stubs_dkr.c:490-497`).
- The frame boundary runs `platform_frame_service()` instead of
  `platform_frame_sync()`, both for the authored tick
  (`platform/stubs_dkr.c:768-770`) and for every subloop opportunity
  (`platform/stubs_dkr.c:888-892`). `platform_frame_service()` is
  `platform_frame_sync_impl(0, 0)` (`platform/platform_sdl_min.c:4046-4048`):
  no swap, no present, input and lifecycle only.
- Resume retires the suspension instead of paying it back as catch-up
  (`platform/present_sched.c:359`, `platform/platform_sdl_min.c:3372-3387`).

Gated by `tests/check_surface_suspension.py`, which drives the transition
deterministically through `MDKR_TEST_MINIMIZE_TICKS` and asserts the
`[SURFACE-PACING]` rows plus stream identity across the suspension.

**Occluded but mapped (a window fully covered by another): no.** The
visibility predicate reads only `SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED`
(`platform/platform_sdl_min.c:878-887`), and SDL 2.32.70 exposes no
window-occlusion flag — occlusion detection is an SDL3 feature. So a covered
window keeps walking, encoding and submitting the display list every frame and
only drops the present.

Measured directly, because every window this session creates is reported
occluded: over a 600-tick race run the backend recorded
`submitted=1197 completed=1197 presented=0 unavailable=1197`, and the app-shell
arm recorded `attempts=1440 presented=0 unavailable=1440 lastStatus=196609`.
Status 196609 is `WGPUSurfaceGetCurrentTextureStatus_Occluded`.

**The stall the reference port was defending against does not reach us.**
libultraship skips render+present because Metal's `nextDrawable` blocks ~1 s per
frame while occluded. wgpu-native already interposes that exact defense one
layer down: it checks `NSWindow.occlusionState` and returns `Occluded`
immediately rather than blocking
(`build-rel/_deps/wgpu_native-src/include/webgpu/wgpu.h:60-78`), and the backend
classifies that result as transient rather than fatal
(`platform/fast3d/gfx_webgpu.c:3231-3235`). The measurement agrees: mean tick
wall time on the fully-occluded runs was 31.2–31.4 ms, i.e. exactly the pacing
floor, with no stall anywhere in the distribution (`max` present interval
33.8–46.1 ms over 1199 samples).

So the residual is **wasted render work while covered**, not latency and not a
hang. It is left open deliberately: the only occlusion signal available to this
codebase is the surface's own status, which is reported *after* the walk it
would need to suppress, so acting on it means latching across frames with a
periodic re-probe — and every automated gate in this environment runs with an
occluded surface, so such a latch needs an automation carve-out plus a test seam
before it can be witnessed at all. Shipping it unwitnessed would trade a power
cost for a correctness risk.

### (b) Is the WebGPU swap-chain depth pinned to the backend minimum?

**Was: no. Now: yes.**

Neither configure site chained `WGPUSurfaceConfigurationExtras`, so both ran at
wgpu-native's default `desiredMaximumFrameLatency = 2` — one whole refresh
(16.7 ms at 60 Hz) of input-to-photon latency above the pacer's own spacing.
The reference port pins the same value to 1 (`Fast3dWindow.cpp:1418`).

**This is a trade, and the vendored header names both halves of it**
(`webgpu/wgpu.h`, `WGPUSurfaceConfigurationExtras`): "1: Minimize latency (CPU
and GPU cannot run in parallel)." Depth 1 means the next acquire cannot return
until the previous image is done with, so CPU frame N+1 no longer overlaps GPU
frame N — free on a CPU-bound frame, not free on a GPU-bound one. **The
throughput cost on this workload is unmeasured**, because the runs below
presented zero frames; see the open list. The pin is taken on the latency
argument alone, and the constant is the thing to revisit if a measured run shows
it costs frames on a GPU-bound scene.

Pinned at surface **configure**, not at bring-up, because the depth is a
property of the configuration: a resize, a present-mode re-rank or a surface
recovery that dropped the chain would silently restore the default. Both sites
carry it — the engine's own (`platform/fast3d/gfx_webgpu.c:113`, `1454-1465`)
and the app shell's adopted window (`platform/app/app_host.cpp:486-497`), since
Play continues in the launcher's surface.

Witnessed by two separate rows, because the two configure sites are not the
same event. The engine reports `frameLatency=` on the `[PRESENT-MODE]` row that
already carries the swap chain's other decisions — but that row is emitted by
the present-mode ranking, which only the engine performs. The launcher's
adopted-window configure hardcodes FIFO, ranks nothing, and therefore reports
itself on its own `[SURFACE-CONFIG] owner=app-shell` row. `check_app_adopted_pacing.py`
asserts both; asserting only the first would have checked the engine twice while
reading as though it covered the adopted window too. The web `[PRESENT-MODE]`
row reports `frameLatency=0`: the browser canvas swap chain is the user agent's
to size and emdawnwebgpu has no extras chain to hang this on.

Witnessed after the change: the engine's `[PRESENT-MODE]` row reports
`frameLatency=1` at a 60 Hz cap, and the app shell's `[SURFACE-CONFIG]` row
reports `owner=app-shell presentMode=fifo frameLatency=1` for the adopted
2560x1600 window. Four negative controls confirm the pair of assertions bites:
a missing shell row, a shell row at depth 2, and an engine row at depth 2 each
fail the gate, and only both-pinned passes.

### (c) Does the GL swap-interval fallback busy-wait or sleep when occluded?

**It sleeps. No spin exists on this path.**

The GL policy mirrors the WebGPU one and falls back to swap interval 0 only
when a tearing opt-in is refused (`platform/platform_sdl_min.c:788-810`). With
a non-blocking swap the pacer, not the queue, is what spaces opportunities, and
while the surface is elided the pacer installs a dedicated occluded deadline
clock at **tick rate** rather than at present rate
(`platform/platform_sdl_min.c:3421-3435`). The wait itself is
`pace_sleep_until()` (`platform/platform_sdl_min.c:2712-2766`), which is
`nanosleep` on POSIX; the only spin in the function is the last millisecond of
the Windows high-resolution-timer path, and it is not reachable while elided
because the elided clock's target is a full tick away.

When the presentation subloop is not engaged at all the pacing floor is
`platform_vi_pace_measure()` (`platform/platform_sdl_min.c:2772-2827`), which
sleeps to the same clock. There is no configuration in which an occluded GL
window spins.

The bound is one tick (16.7 ms at a one-field floor, 33.3 ms at two) rather
than the reference port's flat 8 ms. That is deliberate: it is the same grid
the un-occluded session was on, so resume needs no re-derivation.

## Present-path cost baseline

Arm: `MDKR_PRESENT_RATE=60`, `MDKR_PRESENT_SMOOTHING=interpolate`,
`MDKR_PACE_REALTIME=1`, `MDKR_PRESENT_PERF=1`, autopilot on track 5, 600
authored ticks (1200 present opportunities), 640x480, three repetitions per
backend. Means are per-hit, in microseconds, from `[PRESENTPERF] section=`.

| Section | WebGPU rep1/2/3 | GL rep1/2/3 |
|---|---|---|
| `replay` (interpolated re-walk) | 853 / 825 / 801 | 814 / 752 / 848 |
| `present` (authored-tick boundary) | 68 / 69 / 61 | 1431 / 1376 / 1313 |
| `ipresent` (subloop boundary) | 63 / 60 / 56 | 2188 / 2076 / 2205 |
| `freeze` | 1077 / 1033 / 944 | 1046 / 989 / 1130 |
| `interp` (camera view derivation) | 3.2 / 3.0 / 2.7 | 3.3 / 3.1 / 3.2 |
| `snapshot` | 36.5 / 35.6 / 33.7 | 38.5 / 37.2 / 39.5 |
| `tickwall` | 31195 / 31298 / 31416 | 31266 / 31368 / 31156 |

Pacing quality was identical across both backends and all six runs:
`present-interval` p50 16.65–16.90 ms, mean 16.67 ms, `over=0`,
`regressions=0 stalls=0`, 1200 presents with 1197 displayed.

**The headline number: an interpolated replay walk costs ~0.8 ms** — 801–853 us
on WebGPU, 752–848 us on GL. At a 60 Hz cap that is ~5% of a present interval,
and it is the figure Phase 3 must not regress.

**Reading the `present` rows.** The two backends measure different things and
the numbers must not be compared:

- On **GL**, `present`/`ipresent` bracket `platform_frame_sync`, which reaches
  `SDL_GL_SwapWindow` (`platform/platform_sdl_min.c:2186-2208`). Those are true
  end-to-end present costs, and they are the ones to trust: 1.3–1.4 ms at the
  authored tick, 2.1–2.2 ms in the subloop.
- On **WebGPU**, `wgpu_end_frame` already presented inside `gfx_end_frame` and
  `platform_sdl_present` is a no-op, so the acquire and present are **not**
  attributed to these sections at all. The 61–69 us figures are frame-boundary
  bookkeeping only.

**Display-gated arm, recorded rather than faked.** Every window this session
creates is reported occluded by the Metal backend (see (a) above), so the
WebGPU runs presented zero frames: `presented=0 unavailable=1197`. The WebGPU
numbers above are therefore a valid baseline for **CPU-side** present-path cost
(replay, freeze, interp, snapshot, and the pacing distributions, none of which
depend on a drawable) and **not** a baseline for WebGPU surface acquire or
present cost. The same limitation failed the FPS-overlay arm of
`tests/check_app_adopted_pacing.py`:

```
app adopted pacing: FAIL — FPS-only WebGPU overlay:
  missing '[overlay-test] FPS-only WebGPU pass rendered'
[app] autoplay: host surface occluded attempts=1440; continuing with offscreen engine frames
```

That failure is the known display-gated arm and is independent of the changes in
this note — it needs an un-occluded foreground window, which a command-launched
session on this host does not get. Every other arm of that gate, including the
new `frameLatency=1` assertion, passed.

## Explicitly open

Nothing below is a finding this note is making; each is a measurement it could
not take, recorded so that later work does not mistake silence for a result.

1. **The throughput cost of `desiredMaximumFrameLatency = 1` is unmeasured.**
   The header's own words are "CPU and GPU cannot run in parallel", so the pin
   trades throughput on a GPU-bound frame for a refresh of latency on every
   frame. The runs here presented zero images, so they cannot say what that
   costs on a real scene. Needed: the same census on a foreground display
   session, run at depth 1 and depth 2, comparing displayed-interval means and
   the backpressure hold counts. Until then the pin rests on the latency
   argument alone.
2. **WebGPU surface acquire and present cost is unmeasured.** Same cause. The
   CPU-side sections (replay, freeze, interp, snapshot) and the pacing
   distributions are valid; nothing in this note bounds the drawable path.
3. **Wasted render work on an occluded-but-mapped window is unfixed**, for the
   reasons in (a). It needs an automation carve-out and a test seam before it
   can be witnessed at all.
4. **The FPS-overlay arm of `tests/check_app_adopted_pacing.py` has never run
   here.** It needs an un-occluded foreground window. Every other arm of that
   gate, including both frame-latency witnesses, passes.

## Reproducing

```
MDKR_RENDERER=webgpu MDKR_TEST_VISIBLE_HEADLESS=1 MDKR_AUDIO=0 \
MDKR_AUTOPILOT=1 MDKR_LOAD_TRACK=5 \
MDKR_PRESENT_RATE=60 MDKR_PRESENT_SMOOTHING=interpolate \
MDKR_PRESENT_PERF=1 MDKR_PACE_REALTIME=1 \
./build-rel/mdkr64 --headless-ticks 600 --rom baserom.us.v80.z64 \
    --window-size 640x480
```

Swap `MDKR_RENDERER=gl` for the GL column.

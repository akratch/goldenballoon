# Human acceptance: presentation safety and application-overlay pauses

This is the owner-device acceptance protocol for the work integrated after
v1.2.0. It covers motion smoothing, wave deformation, skydome camera locking,
camera-cut handling, projected-shadow correspondence, VRR-aware alpha timing,
and application-overlay pauses in menu, demo, and race-intro scenes.

Automated tests prove invariants and reproduce known failures. Human testing is
still required for display artifacts, real VRR behavior, audio output, input
feel, compositor behavior, and hardware/backend coverage. No finite checklist
can prove software "100% robust"; passing every required row below is the
release bar, and any stop-ship observation overrides a nominal pass.

## Build and evidence rules

Test one binary built from a clean local `main`. Record:

- `git rev-parse HEAD`, build type, package/archive hash, and ROM revision;
- OS version, device, CPU/GPU, graphics driver, renderer, and window mode;
- display model, active refresh rate, VRR on/off, VSync/tearing setting, and
  whether an external dock/display is involved;
- exact `Video.FrameLimit`, `Video.MotionSmoothing`, preset, resolution, render
  scale, widescreen setting, and every `MDKR_*` override;
- track, vehicle, game mode, approximate location/timestamp, reproduction
  count, result, log, and a 60 fps or higher camera recording for visual bugs.

Use an isolated preferences/save directory for the clean pass, then repeat the
save checks with a backed-up copy of a real v1.2.0 profile. Do not use test-only
environment variables in the production-baseline rows. Test with audio enabled;
`MDKR_AUDIO=0` is for automation only.

Every visual A/B must use the same camera path, vehicle, weather, window size,
render scale, and display mode. Change one variable at a time. Rate shimmer on
this fixed scale:

- 0: absent;
- 1: visible only when deliberately hunting for it;
- 2: obvious during ordinary play;
- 3: disruptive, tearing, flashing, or geometry instability.

## Required hardware matrix

The following rows are mandatory unless the target is explicitly unsupported:

| Platform | Display mode | Renderer | Required purpose |
| --- | --- | --- | --- |
| Windows handheld (ROG Ally or equivalent) | 120 Hz, VRR on | WebGPU | Primary waterfall/VRR acceptance |
| Same Windows device | fixed 60 Hz or VRR off | WebGPU | Fixed-refresh comparison |
| Windows desktop or handheld | native refresh | OpenGL | Backend comparison and fallback |
| macOS ProMotion | adaptive refresh | WebGPU and OpenGL | Free-quantum behavior and backend parity |
| Linux | fixed refresh, plus VRR if available | WebGPU and OpenGL | Vulkan/lavapipe-vs-physical-driver parity |
| Browser build | native display refresh | browser WebGPU | Presentation, input, resize, and lifecycle smoke |

At least one physical Windows run must use the packaged application rather than
only a developer executable. If Linux or browser physical VRR is unavailable,
record that limitation; do not silently mark the row passed.

## Universal stop-ship conditions

Stop and retain the log/artifacts if any row produces:

- a crash, abort, assertion, GPU validation error, hang, black/flat scene, or
  unrecoverable renderer/device loss;
- NaN-like exploded geometry, full-screen flash, one-frame camera teleport,
  backward camera sweep, or interpolation through a scene cut;
- gameplay, timer, racer, input, audio, save, RNG, or progression behavior that
  changes when only a presentation option changes;
- a pause overlay that advances simulation, loses the authored camera, corrupts
  a racer, or resumes from a different state;
- persistent shimmer rated 2 or 3 on the primary Windows VRR route;
- an authored 30 Hz endpoint that differs between smoothing on and off;
- a test-only environment variable affecting behavior without the internal
  test token.

## A. Application-overlay pause regression

Run each scene five times. Open the F1 application settings overlay early,
midway, and within a few frames of a scene transition. Hold it for 1, 5, and 15
seconds, close it, and continue until the scene naturally advances.

1. Title-screen flyaround: the entire picture and camera pose remain frozen;
   no flat background appears; close resumes from the held pose.
2. New-game intro: animated objects and scripted camera remain intact and
   stationary; close resumes rather than restarting or skipping the sequence.
3. Attract demo, including Greenwood Village: all racers, effects, and camera
   remain finite and frozen; no abort occurs; close resumes normal racing.
4. Ancient Lake start flyover: the application overlay holds the exact authored
   camera bank, not the behind-kart camera; countdown and control resume once.
5. Repeat the race-intro test with at least one boss, challenge, and hub intro.
6. During ordinary racing, verify lap time, countdown, checkpoint, kart pose,
   AI positions, particles, and audio do not advance while the overlay is open.
7. Open and close with keyboard, controller, and handheld/touch input. No input
   used to close the overlay may leak into gameplay as an unintended action.
8. In three-player mode, exercise the TT-camera viewport and the normal START
   pause. Camera selection must remain correct and neither pause path may break
   the other.

Pass requires a visually exact hold, clean resume, no crash, no duplicated or
lost input, and no scene-specific exception across all repetitions.

## B. Default and authored-frame neutrality

1. Start from fresh preferences. Confirm the shipped defaults remain Original
   frame limit with motion smoothing off.
2. Compare v1.2.0 and current `main` on title, hub, Ancient Lake, Jungle Falls,
   and Whale Bay with Original/off. Camera, gameplay speed, effects, audio, and
   authored-frame appearance must be indistinguishable.
3. With smoothing off, cycle Original, 30, 60, 120, Display, Display minus
   margin, and Uncapped. No intermediate reconstruction may appear.
4. With smoothing interpolated, pause on or capture authored-tick endpoints.
   Those endpoints must exactly match smoothing-off output; only intermediate
   presents may differ.
5. Toggle smoothing live where the UI allows it. The setting must take effect
   once, preserve gameplay state, and persist exactly as the UI reports.

## C. Water, waterfall, and wave deformation

Use Restored visuals, smoothing Interpolated, and Display/120 Hz.

1. Whale Bay: watch water during a standing start, slow steering, fast
   left-right pans, approach/retreat across wave LOD boundaries, and a full lap.
   Deformation and texture motion must read as one surface, with no crawling
   edge, vertex explosion, fold-through, or one-tick shape lag.
2. Jungle Falls: inspect the waterfall sheet, ping-pong water, shoreline, and
   horizon during repeated rapid pans. The reported v1.2.0 shimmer must be 0 or
   1 on the primary VRR device and must not worsen on fixed refresh.
3. Repeat at 60, 90/120, Display, Display minus margin, and Uncapped where the
   hardware supports them. No rate may introduce topology pops.
4. Repeat Whale Bay and Jungle Falls with OpenGL and WebGPU. Minor rasterization
   differences are acceptable; temporal behavior and surface ownership are not.
5. Cross a wave-grid LOD boundary at low speed ten times. A legitimate one-frame
   snap at a topology change is acceptable; interpolating unrelated vertices,
   repeated oscillation, holes, or spikes is a failure.
6. Check lava/void-curtain or other wave-driven surfaces if encountered. They
   must obey the same no-fold/no-explosion rule.

## D. VRR-aware alpha timing and shimmer bisection

On the Windows handheld, use the same Jungle Falls viewpoint and perform ten
slow pans and ten rapid left-right pans per arm:

1. Baseline: VRR on, 120 Hz, WebGPU, `FrameLimit=Display`,
   `MotionSmoothing=Interpolated`, tearing off.
2. Legacy-grid A/B: repeat with `MDKR_PRESENT_QUANTUM_STRICT=1`. This intentionally
   restores fixed-grid quantization. On a genuinely variable interval, current
   production should be equal or better; strict being clearly better is a
   release-blocking investigation.
3. Fixed-refresh control: disable VRR or force 60 Hz, remove the strict override,
   then compare strict and production again. Neither may judder or drift.
4. Repeat baseline with Display minus margin, numeric 60, numeric 120, and
   Uncapped. Check cadence, latency feel, camera motion, and audio continuity.
5. Repeat baseline with the platform tearing option enabled. Tearing may change
   scan-out behavior, but it must not be required to hide simulation/presentation
   shimmer.
6. Repeat with smoothing off. This distinguishes interpolation artifacts from
   texture filtering, panel response, backend, or scan-out artifacts.
7. Repeat on macOS ProMotion and, where available, Linux VRR. Record whether the
   log reports free or grid alpha-quantum mode and whether that matches the
   actual display configuration.
8. Change the display refresh or move the window between unlike-refresh displays
   while running. The game must settle to the new cadence without a persistent
   hitch, runaway speed, stale quantum, or audio underrun.

Optional diagnostic only: run with `MDKR_SMOOTH_PAN_DEMOTE=1`. During a rapid
pan, selected water/scroll/sky surfaces may step to the newest authored state
instead of blending, while racers/ordinary object roots remain smooth. This is
an attribution tool, not an ownership substitute. It is off by default; do not
treat an opt-in result as proof of the shipping baseline.

## E. Camera cuts, skydome, finish cameras, and shadows

1. Title flyaround and hub skies: translate and rotate the camera through the
   horizon. The dome must remain camera-locked with no swimming, parallax,
   translation lag, seam, or black edge.
2. Trigger level loads, respawns, balloon/challenge transitions, boss intros,
   sudden yaw changes, and FOV changes. A cut must be atomic: never blend
   position while snapping rotation, or vice versa.
3. Drive ordinary long bends and small FOV changes. They must remain smooth;
   the new cut thresholds must not over-fire as visible micro-snaps.
4. Finish races in first place and behind, then watch finish, spectate, replay,
   and post-race cameras. They must cut cleanly and never interpolate through an
   unrelated camera or remain stuck after the mode changes.
5. Repeat finish-camera checks in one-player and three-player modes, including
   the TT-camera viewport where available.
6. Inspect kart shadows on slopes, banked turns, jumps, bridges, walls, and
   transitions between receiver triangles. Shadows must remain attached, keep
   plausible shape/height, and never swap corners, spike, pulse severely, or
   interpolate between reordered meshes.
7. Repeat shadow checks with world shadows on/off, Restored/Enhanced visuals,
   both renderers, widescreen on/off, and render scales 1 and 2+.

## F. General regression and soak

1. Complete at least one three-lap race with car, hovercraft, and plane; one boss;
   one battle/challenge; one hub transition; and one multiplayer race.
2. Verify starts, boosts, steering, items, collisions, checkpoints, lap timing,
   AI, finish order, post-race flow, and progression are unchanged.
3. Verify music, vehicle sounds, positional effects, pause/resume, focus loss,
   minimize/restore, and audio-device reconnect. No click, loop, starvation, or
   permanent mute is acceptable.
4. Verify keyboard and controller hotplug, analog steering, rumble, multiplayer
   bindings, overlay input capture, and reconnect after focus loss.
5. Load an existing v1.2.0 save, complete a race, save, relaunch, and verify
   progress. Repeat with fresh save and portable mode if shipped on that target.
6. Exercise launcher restart/apply, renderer switching, fullscreen/windowed,
   resize, DPI/scale changes, minimize/restore, and clean quit/relaunch.
7. Run a 60-minute soak on the primary Windows VRR device while rotating among
   water-heavy tracks, hubs, overlays, pauses, finishes, and display modes.
   There must be no crash, rising stutter, corruption, resource growth symptom,
   device-loss loop, audio degradation, or accumulating visual instability.

## Sign-off

A platform row passes only when all mandatory cases have evidence and zero
stop-ship observations. Record limitations as `NOT RUN`, not `PASS`. The final
owner sign-off should include the exact commit/package hash and explicitly state:

- primary VRR waterfall result and strict-grid A/B result;
- fixed-refresh result;
- GL/WebGPU result;
- all four overlay-pause scenes result;
- wave, sky, camera-cut, finish-camera, and shadow result;
- 60-minute soak result;
- any unsupported or untested platform row.

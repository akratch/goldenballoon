# Device acceptance checklist — everything since 1.0.5

*Candidate build: 1.1.0-RC1, cross-built from the release tree. Work through
this on real hardware; every line traces to a shipped change. The release is
gated on this pass.*

## Reported issues (verify each is gone)

- [ ] **#16/#18 — menu stretch**: Player select → Tracks → back to player
  select, and Track Select → back to title → start again. Both must stay
  widescreen; nothing 4:3-stretched afterward.
- [ ] **#22 — challenge-loss stretch**: lose the Dino Domain Gold Key
  (egg-hatch) challenge, and place 4th in the Jungle Falls coin race. The
  recap stays inside its wooden frame; Try Again and the lobby afterward stay
  widescreen. Also win one normally — the podium is still framed.
- [ ] **#17 — Taj portraits**: play a TT Challenge event as Taj. The big wall
  photo shows Taj (not Diddy); his scoreboard card is the same size and place
  as every other racer's, and slides/fades with the HUD.
- [ ] **#19 — PAL (if you have the EU ROM)**: PAL play has no audio crackle.
  Setting Frame limit to Match Display + Motion smoothing removes the visible
  ripple without changing game speed or music pitch. A PAL launch mentions
  this once.
- [ ] **Language menu**: on any disc, the in-game language selector cycles
  EN → DE → FR. Settings → Presentation → Menu Languages → Authentic restores
  the disc's own retail menu immediately.
- [ ] **#20 — pause menu**: open the launcher pause menu WITH THE CONTROLLER
  (back/select), navigate to Resume WITH THE CONTROLLER, activate it. The
  game accepts pad input immediately after — including while still holding
  the button. Also: keys pressed during the race are not replayed into the
  menu when it opens.
- [ ] **#23 — audio under load**: play several races on Windows. Listen for
  stutter, clicks, or dropouts, especially during loads, alt-tab, and busy
  scenes. Audio should stay clean through frame hitches.
- [ ] **#24 — plane contrails**: fly a plane; wing trails lie flat (--) like
  the original, not rotated (|). Hold R (barrel roll) to see them thicker.

## New behavior to exercise (1.0.6 → 1.1.0)

- [ ] **Camera default**: on a fresh config the camera is **Authored
  (original camera)** — the retail camera, unchanged. Settings → Camera →
  Keep the camera out of walls turns the correction on at the next race, no
  restart (drive at walls/doors in the adventure hub and in races — the camera
  pulls in instead of clipping), and switching back restores the retail camera
  the same way. *This row was run on 2026-08-07 against a build that shipped
  the correction ON by default; it was rejected as too sensitive in play, and
  the default was reverted to Authored. The correction ships as an opt-in.*
- [ ] **Camera comfort**: with Keep the camera out of walls selected, the
  reduced-motion option visibly calms camera shake and recovery; switching
  back restores authored motion. (Nothing to observe on the original camera.)
- [ ] **No tearing**: every Frame limit choice (including 40, 90, 120,
  "Just under display", Uncapped) shows no horizontal seam while panning.
  Allow Tearing ON is the one exception — it may tear, by choice.
- [ ] **Live settings**: change Frame limit, Motion smoothing, and Allow
  tearing mid-race — the picture changes on the next frame, no restart; lap
  time/position/music unaffected. Presentation pace (Original/Smooth) is the
  one-click version.
- [ ] **Smooth mode look**: Presentation pace is **Original** on a fresh
  config; Smooth is the opt-in. With Presentation pace = Smooth, waterfalls,
  water, and lava move fluidly with the world (no stepping/shimmer —
  Jungle Falls waterfall and Hot Top lava are the reference spots). *Run
  2026-08-07: jitter and artifacts remain visible in high-motion areas, which
  is why Smooth stays opt-in and Motion smoothing Off stays the default.*
- [ ] **Ultrawide (your monitor)**: race at 21:9/32:9, wide FOV if you use
  it — the camera should NOT jump to overhead emergency framing in normal
  play (was frequent at wide FOV before).
- [ ] **Fullscreen + two monitors** (#22 reporter's setup): fullscreen on the
  ultrawide, run the #22 routes above, and drag the window between monitors —
  frame limit and pacing adapt to the new display without restart.

## Standard release pass (unchanged from the release guide)

- [ ] Launcher opens via WebGPU; ROM picker loads your dump; Play starts.
- [ ] Controller, rumble, keyboard all respond; remap survives relaunch.
- [ ] Save, quit, relaunch: settings, EEPROM progress, and ghosts persist.
- [ ] One full adventure segment: hub → door → race → win balloon → return.
- [ ] Return to Launcher and Restart & Apply both come back cleanly;
  `mdkr64.log` names itself and the previous run's file on its first lines.

Anything that fails: note the exact route and grab `mdkr64.log` — every line
above has a matching regression gate, so a precise repro turns into a fix
fast.

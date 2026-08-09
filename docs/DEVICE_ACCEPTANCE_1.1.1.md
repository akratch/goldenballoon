# Device acceptance — 1.1.1-RC2

**Candidate:** `Golden-Balloon-1.1.1-RC2-windows-x64.zip` / `-macos-arm64-unsigned.dmg`
**Commit:** `d0f2a4d`, branch `land/rc2` (merge of `integrate/presentation-gold-standard` onto main)
**Save:** a 100%-complete file ships beside the Windows build at
`GoldenBalloon\save\eeprom.bin` — everything below is reachable immediately, no unlocking.

This supersedes nothing: [`DEVICE_ACCEPTANCE_SMOOTHING.md`](DEVICE_ACCEPTANCE_SMOOTHING.md)
remains the protocol for the smoothing verdict specifically, and §2 below is its
short form. This document covers the whole candidate.

---

## 0. Before you start

The launcher's defaults are the shipped defaults; **do not change them for §1 or
§3**. Motion smoothing and the camera correction are OFF and unrecommended, and
this candidate does not change that. §2 asks you to turn smoothing on
deliberately, and it is the only section that does.

Two windows are worth having open: the game, and the log (the launcher's
Diagnostics view, or the console the exe was launched from). Three checks below
read a counter out of the log; every other check is with your eyes.

---

## 1. New in this candidate — high-resolution text

The plain lettering is now drawn from real typefaces instead of being magnified
from small pictures of letters. **The race HUD is not affected** — lap, position
and countdown are the game's own colourful lettering and are deliberately
untouched. Do not evaluate this mid-race.

| # | Where | What "right" looks like |
|---|---|---|
| 1.1 | Main menu, Options, Video | Crisp letters at any window size. Same words, same line breaks, same positions, same widths as before. |
| 1.2 | Save screen (file select) | The clearest place to see it. Filenames and labels sharp; the wooden furniture and layout unmoved. |
| 1.3 | Any lap / finish time | Digits crisp; columns still line up exactly as they did. |
| 1.4 | Character select, world hub signage | DKR's own colourful display lettering must look **exactly as it always has** — that is the game's handwriting and it is not replaced. |
| 1.5 | Taj / T.T. dialogue boxes | Sharp text; the box, the breaks and the paging unchanged. Note this is still *SmallFont* — on a US ROM the dialogue boxes draw with it, not with the second replaced face (see below). |
| 1.6 | Video → **High-resolution text** → Off → restart | Everything returns to the original letters. This is the player's escape hatch; confirm it works. |
| 1.7 | Video → mode **Original** | Original mode is pixel-exact to the console and must ignore this feature entirely, even though the switch exists. |

**Only one of the two replaced faces can appear on a US ROM.** The feature
replaces `SmallFont` and `SubtitleFont`. `SubtitleFont` is never loaded on this
build — the engine's font boot loads FunFont and SmallFont only
(`game/src/font.c`), and both places that would select it (`game/src/menu.c`)
sit inside `#if REGION == REGION_JP`, compiling to SmallFont everywhere else.
So every hi-res glyph you can see is Roboto SemiBold standing in for SmallFont;
the "MDKR Subtitle" face ships but renders nothing until JP support exists.
Do not go hunting for a second typeface.

**The failure to watch for is layout movement, not ugliness.** A word wrapping
differently, a column shifting, or text touching a box edge it never touched is
a real defect. Report it with the screen name.

---

## 2. Motion smoothing — the standing verdict (opt-in, 120 Hz)

Turn on **Frame Rate & Motion → Match Display + Motion smoothing**. This is the
only section where you change a default. The full rationale and the artifact
classes are in [`DEVICE_ACCEPTANCE_SMOOTHING.md`](DEVICE_ACCEPTANCE_SMOOTHING.md).

| # | Do | Right |
|---|---|---|
| 2.1 | Level 29, three laps, 120 Hz, pick up a **shield** | The bubble sits on the kart through corners and boosts. It must never lead or trail. The defect this fixes put it ~51 px ahead on a 640×480 frame. |
| 2.2 | Same, pick up a **magnet** | Shell anchored, no lead, no shear. **Nothing automated has ever measured the magnet** — it shares the fixed code path, and your eyes are the only check that exists. |
| 2.3 | Waterfalls (Crescent Island), lava (Hot Top Volcano), fences at speed | The shimmer/artifacting that caused the 2026-08-07 rejection should be gone. |
| 2.4 | Finish a race; watch the post-race spectate cameras. Also one 3-player Time Trial | Each camera change is an instant **cut**. No sliding between viewpoints. 12 blended cuts per race → 0. |
| 2.5 | At each cut, watch trees and signs | Billboards must not roll or pop against the cut. (Found by review, fixed on top of the branch.) |
| 2.6 | Spin out / take a hairpin hard | The kart turns the short way round, never smearing the long way. |
| 2.7 | Menus → hub → a cutscene → a level transition, then read the log's `[PRESENT-PACKET]` row | `uncapturedext=0 uncapturedrefusals=0`. Non-zero means those screens are quietly running unsmoothed — the failure has **no on-screen signal**, which is why it is a log check. |

**Verdict rule.** Smoothing stays opt-in regardless of the outcome. Accepting it
does not flip a default; that is a separate decision.

---

## 3. Regression sweep — defaults untouched

Roughly fifteen minutes. The scissor fix in this line changed how *every* 2D
rectangle clips, so this section is broader than it looks.

| # | Do | Right |
|---|---|---|
| 3.1 | Smokey Castle, win **and** lose | On the results screen the character portraits are clipped away by the shrinking wooden frame. Slivers *inside* the frame's top edge are correct — hardware does that. Portraits floating over the wood is the bug (#25). |
| 3.2 | Title → character select → track select carousel → credits | Backgrounds still fill the widescreen gutters. No missing panels, no black bars, no clipped art. |
| 3.3 | Character-select bios, credits scroll | Text tucks cleanly into its box instead of leaking past the edge. |
| 3.4 | One 2-player split-screen race | Each player's HUD stays inside its own half. |
| 3.5 | Pause during the opening flight cutscene, then resume | No blue screen; the animation continues. Also try pausing during the title fly-around. |
| 3.6 | One race-start and one race-finish wipe | The transition still covers the screen edge to edge. |
| 3.7 | A lit night track (Star City, Boulder Canyon) | Sane lighting. This path was touched by a compile repair. |
| 3.8 | Alt-tab / drag the window mid-race | No audio clicks or stutter on return. |
| 3.9 | Any Adventure race, start to finish, with defaults | Nothing feels different from 1.1.0. That is the point of this row. |

---

## 4. Things only this session can settle

Automated coverage cannot reach these. If you have appetite, they are worth more
than another lap.

- **The magnet shell** (2.2). Genuinely unmeasured.
- **Real 120 Hz interpolation quality.** Every automated number was taken on a
  60 Hz host, where a 120 Hz request is already physically unpresentable. Your
  panel is the only 120 Hz measurement that exists.
- **The launcher's FPS overlay, foregrounded for a minute.** One arm of
  `check_app_adopted_pacing` has never run to completion anywhere, because it
  needs an un-occluded foreground window.
- **Two gates need an active display session and cannot be run from an
  automated one.** `check_app_adopted_pacing` (the FPS-overlay arm above) and
  `check_pacing_quality` (its realtime arm) both require the window server to
  hand back drawables; without one, every present returns `unavailable` and the
  arms measure nothing. Both now say exactly that instead of blaming a missing
  marker. Run them from a logged-in, unlocked session:

  ```bash
  python3 tests/check_app_adopted_pacing.py --build build-rel --rom baserom.us.v80.z64
  python3 tests/check_pacing_quality.py     --build build-rel --rom baserom.us.v80.z64
  ```

  The realtime arm is the only coverage the alpha-grid projection has anywhere,
  so "it passed on a headless host" is not a substitute.
- **`frameLatency=1` across a resize.** Resize the window and confirm the
  `[SURFACE-CONFIG]` / `[PRESENT-MODE]` rows still report it.

---

## 4b. If the picture shimmers on a VRR handheld — three experiments

Reported on a ROG Ally X: shimmering/blur when the camera moves fast, with
smoothing on. The Ally X is a 120 Hz FreeSync panel, and there is a specific
reason to suspect variable refresh rather than the smoothing work itself.

**Why VRR is suspect.** With Match Display + smoothing, the interpolation phase
is not used raw — `present_sched_alpha` projects it onto a fixed grid one
display refresh wide (`platform_present_display_quantum_units`). On a fixed
panel that is a jitter *filter* and is exactly right: the queue retires one
present per refresh, so rounding recovers the instant the frame is really shown.
A VRR panel does not refresh on a grid — it scans out when the frame arrives —
so the same projection becomes a phase *error* of up to half a refresh (±4.2 ms
at 120 Hz). Nothing in the port detects VRR, and nothing can: SDL reports a flat
120 Hz whether FreeSync is engaged or not.

**The port already ships the VRR-correct mode.** Frame limit **"Just under
display"** (refresh − 3 Hz) installs its own software pacing grid, which
*disables* the projection above, and never blocks the panel because it stays
below max refresh. That is the same "cap a few Hz under the panel" practice the
frame-generation ecosystem converged on independently.

Run these in order; each is a couple of minutes and they discriminate between
three different causes.

| # | Setting | If the shimmer goes away, the cause is |
|---|---|---|
| 4b.1 | Smoothing **on**, Frame limit **Just under display** | Our fixed-grid phase projection under VRR. Fixable in code — either recommend this mode with smoothing, or make the projection stand down when presents stop landing on the grid. |
| 4b.2 | FreeSync **off** in AMD Adrenalin (not just Armoury Crate), fixed 120 Hz, smoothing on Match Display | The panel, not us — VRR overdrive/flicker. The Ally line has a tracked, application-independent VRR flicker issue. Our deliverable is documentation, not code. |
| 4b.3 | Neither helped | Content-level and already known: texture/mip aliasing under smooth eye pursuit (30 Hz stepping hides it; 120 Hz smooth motion reveals it), and UV-scroll phase holds on waterfall/fence surfaces. Not a defect in the shell/camera work. |

**Capture while you do it.** Launch with `MDKR_PRESENT_PERF=1` and keep the log.
The `[PRESENTPERF-HIST] series=displayed-interval` p50/p95 rows and the `gridppm`
row show directly whether presents are landing on an 8.33 ms grid or spreading —
this would be the first measurement of a VRR display this project has ever had.

---

## 5. Known and deliberate

Not defects; do not report them.

- Motion smoothing and the camera correction are **off by default and not
  recommended**. Unchanged by this candidate.
- The race HUD's colourful lettering is **not** replaced by §1. Deliberate.
- macOS is **not notarized**: first launch needs System Settings → Privacy &
  Security → **Open Anyway**. Windows is not code-signed: SmartScreen →
  **More info → Run anyway**.
- No game data ships. Bring your own US 1.1 ROM.
- The RC1 WebGPU frame-admission change was **reverted**. Frame admission in
  this build is identical to shipped 1.1.0 — the behaviour already accepted on
  device. The problem it targeted proved to be an artifact of the test
  harness's synthetic clock: re-measured with a real pacer, interpolation
  happens and no authored frames are dropped
  ([`open-items/renderer.md`](open-items/renderer.md)).

---

## 6. Reporting

For anything wrong: the screen or track, whether smoothing was on, the display
rate, and what you saw versus expected. For §2.7 and §4, paste the log row.

**A pass here is not a release.** Cutting the release is a separate step, gated
on this verdict.

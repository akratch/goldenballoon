# Device acceptance — 1.2.0

**Candidate:** `Golden-Balloon-1.2.0-RC1-windows-x64.zip`
**Commit:** see the RC manifest beside this file — the zip, the manifest and this
guide are staged together and name the same commit
**Save:** a 100%-complete file ships beside the build. Copy `eeprom.bin` to
`GoldenBalloon\save\eeprom.bin`, then **choose ADVENTURE TWO on GAME SELECT** —
the slot is a completed Adventure Two file, and the file-select gate refuses a
slot whose adventure marker disagrees with the option you are under. That
refusal is exactly why an earlier candidate's save "showed up but could not be
clicked into". `tests/check_save_100_entry.py` now drives that route on every
suite run, with the broken variant as a positive control.

**Windows only.** No macOS DMG in this candidate.

---

## 0. Before you start

The launcher's defaults are the shipped defaults. **Do not change them for §1,
§3 or §4.** Motion smoothing and the camera correction are OFF and not
recommended; §2 is the only section that asks you to change a default.

Have the log visible — the launcher's Diagnostics view, or the console the exe
was launched from. Three checks read a counter out of it; everything else is
with your eyes.

---

## 1. Three reported bugs — the fixes to confirm

| # | Do | Right |
|---|---|---|
| 1.1 | **Smokey Castle**, win **and** lose | On the results screen the portraits are clipped away by the shrinking wooden frame. Slivers *inside* the frame's top edge are correct — hardware does that. Portraits floating over the wood is the bug. *(#25)* |
| 1.2 | **Adventure Two → a tournament.** Watch the announcement ("ROUND ONE / ANCIENT LAKE") | The lettering reads normally. The course behind it stays mirrored — that is Adventure Two working. *(#27)* |
| 1.3 | **Adventure Two → pause mid-race** | The pause menu reads normally. **This half never reproduced here** on any machine, mode or window shape. If you still see mirrored text, say so and screenshot it — that is genuinely new information. |
| 1.4 | **Bluey rematch** (2nd Walrus) at 60 FPS with gameplay tick rate **Enhanced** | Winnable. It was not before. See §4 for what is still off. *(#26)* |
| 1.5 | **Bubbler / Kraken** rematch, same settings | Winnable. Bubbler was the worst case — it used to reach roughly three times the speed the game intends. |

---

## 2. Motion smoothing — opt-in, and the only section that changes a default

Turn on **Frame Rate & Motion → Match Display + Motion smoothing**.

| # | Do | Right |
|---|---|---|
| 2.1 | Level 29, three laps, pick up a **shield** | The bubble sits on the kart through corners and boosts. Never leads, never trails. |
| 2.2 | Same, pick up a **magnet** | Shell anchored, no lead, no shear. **Nothing automated has ever measured the magnet** — your eyes are the only check that exists. |
| 2.3 | Waterfalls (Crescent Island), lava (Hot Top Volcano), fences at speed | No shimmer. This is what caused an earlier rejection. |
| 2.4 | A snowy track (Snowball Valley, Frosty Village) and a rainy one | Snow and rain move smoothly with the scene rather than stepping. Precipitation moves further per tick than anything else on screen, so this is the most visible of the recent fixes. |
| 2.5 | A sun/lens flare in view; rain landing on water | Flare and splashes track the camera rather than lagging a tick behind it. |
| 2.6 | Finish a race; watch the post-race spectate cameras. Also one 3-player Time Trial | Every camera change is an instant **cut**. No sliding between viewpoints. |
| 2.7 | At each cut, watch trees and signs | Billboards must not roll or pop against the cut. |
| 2.8 | A cutscene that hard-cuts (race intro fly-in, boss intro) | Instant. An earlier candidate blended across these. |
| 2.9 | Spin out / take a hairpin hard | The kart turns the short way round, never smearing the long way. |
| 2.10 | Menus → hub → cutscene → level transition, then read the log's `[PRESENT-PACKET]` row | `uncapturedext=0 uncapturedrefusals=0`. Non-zero means those screens are quietly running unsmoothed — **the failure has no on-screen signal**, which is why it is a log check. |

**Verdict rule.** Smoothing stays opt-in regardless of the outcome. Accepting it
does not flip a default.

---

## 3. The settings screen — rewritten

| # | Do | Right |
|---|---|---|
| 3.1 | Open Settings and read down it | Settings are grouped by what you are trying to do. Every setting says in one line what it does. |
| 3.2 | Find **Gameplay tick rate** | Marked **Changes gameplay** *and* **Experimental**, with a caution box. It sits in its own group, away from Frame limit. |
| 3.3 | Set it to Enhanced | The caution box changes to say Enhanced is on and races run off pace. |
| 3.4 | Read any setting's description aloud in your head | It should sound like a person wrote it. Report anything that reads like filler — that is a real defect in this release. |
| 3.5 | Change **Interface scale**, then any staged setting | "Next launch" / "Next race" markers behave as labelled. |

The big rearrangement: **Gameplay tick rate used to sit in the same box as
Frame limit**, which is why players reasonably read it as a frame-rate setting.
That is the second cause behind the boss-race reports.

---

## 4. Gameplay tick rate — read before testing Enhanced

**Original is the accurate setting and the default. It is bit-for-bit the game
as it shipped**, and every change in this release leaves it untouched.

**Enhanced is experimental.** It runs DKR's logic at 60 Hz instead of the 30 Hz
it was written for. This release fixes the worst of it — the boss runaway that
made rematches unwinnable, acceleration that was twice as quick, AI laps 3–8%
fast, and bosses launching with half their intended head start. Ordinary racers
now run within **0.1%** of authored pace.

**What is still off, measured:** boss races. On one route a boss finishes about
2% early; on another about 27% early, spending most of the race with wheels off
the ground that should be planted.

**For a smooth 60 FPS picture without changing the game at all, use Frame limit
and Motion smoothing (§2).** Those do not touch the simulation.

Why this is taking a while, plainly: the collision test, the chassis-angle solve
and the drag sample are not approximations of a smooth system — they *are* the
system, defined at 30 steps a second. Halving the step makes them answer
differently, and no scaling constant fixes that.

---

## 5. Regression sweep — defaults untouched

Roughly fifteen minutes. The rectangle-clipping fix in this line changed how
*every* 2D rectangle clips, so this is broader than it looks.

| # | Do | Right |
|---|---|---|
| 5.1 | Title → character select → track select carousel → credits | Backgrounds still fill the widescreen gutters. No missing panels, no black bars, no clipped art. |
| 5.2 | Character-select bios, credits scroll | Text tucks into its box rather than leaking past the edge. |
| 5.3 | One 2-player split-screen race | Each player's HUD stays inside its own half. |
| 5.4 | Pause during the opening flight cutscene, then resume | No blue screen; the animation continues. Also pause during the title fly-around. |
| 5.5 | One race-start and one race-finish wipe | The transition covers the screen edge to edge. |
| 5.6 | A lit night track (Star City, Boulder Canyon) | Sane lighting. |
| 5.7 | **Remastered mode**, a track with flat-filled panels or a fade | No black panels or fills. |
| 5.8 | Hold a direction on track / character / file select | The selection steps at a comfortable rate, not twice as fast. Check this under **Enhanced** too. |
| 5.9 | Alt-tab / drag the window mid-race | No audio clicks or stutter on return. |
| 5.10 | Any Adventure race, start to finish, defaults | Nothing feels different from 1.1.0. That is the point of this row. |

---

## 6. New in this build, no human check needed — but worth knowing

Two changes landed after the last candidate and are covered by automation, so
they need no specific test. Mentioned so that if something feels off in these
areas you know where to look.

- **Renderer state handling.** The function that draws every 2D rectangle now
  saves and restores its borrowed state as one unit. That function caused two
  of the bugs in §1 by forgetting one field each time; it structurally cannot
  now.
- **Input timing instrumentation.** Groundwork for reducing control latency,
  **off by default**. Nothing changes unless it is explicitly enabled.

---

## 6b. The new product surface — first time on a real Windows machine

This candidate is the first with content packs, speech, the enhancements menu
and the developer tools. Automation covers their logic; three of them have
never run on real Windows hardware at all, and one has never been *heard*
anywhere but a Mac.

| # | Do | Right |
|---|---|---|
| 6b.1 | Open Settings and read the page top to bottom | **Accessibility** sits second from the top. **Enhancements** and **Content** exist further down. Every row reads like a person wrote it. |
| 6b.2 | Accessibility → turn **Speech** on, then move focus around the launcher with keyboard or controller | The launcher says what you land on. **This is the first time this feature has ever run on Windows** — on the Mac it works; here it has only ever been built. If you hear nothing, that is a real finding and not your mistake: say so, with your Windows version. |
| 6b.3 | Turn **Race announcements** on, run one race | Position, lap, item and finish are spoken. Same first-time caveat as above. |
| 6b.4 | Settings → Content, with an empty `mods` folder | The section says plainly that nothing is installed and what a pack is. Nothing crashes, `Tab` in-game does nothing. |
| 6b.5 | If you have twenty minutes: follow the quick start in `docs/MODDING.md` and install a one-texture pack | The texture is replaced; `Tab` flicks it off and on; Settings → Content lists the pack. The skip list names anything it refused, with a reason. |
| 6b.6 | App window → **Developer tools** on: open the console and the free camera, then switch it off | Tools work while on; off means gone. |
| 6b.7 | Find **Check for updates** | Its description says nothing checks for updates yet. It must not claim otherwise. |

---

## 7. Known and deliberate

Not defects; do not report them.

- Motion smoothing and the camera correction are **off by default and not
  recommended**.
- The race HUD's colourful lettering is **not** replaced by the high-resolution
  text feature. Deliberate — that is the game's own handwriting.
- Only one replaced typeface is visible on a US ROM. The second is JP-only and
  renders nothing. Do not go hunting for it.
- Windows is not code-signed: SmartScreen → **More info → Run anyway**.
- No game data ships. Bring your own US 1.1 ROM.
- **A content pack applies in every mode, including Original.** Installing the
  pack is the opt-in; Original with a pack installed is deliberately not a
  pixel-exact reference. Do not report it.
- Speech is the app speaking for itself, not a screen-reader tree. VoiceOver,
  Narrator and NVDA still cannot see into the launcher, and nothing claims
  they can.
- There are no save states in this build, and nothing in it claims there are.

---

## 8. If the picture shimmers on a VRR handheld

Reported on a ROG Ally X: shimmer or blur when the camera moves fast, with
smoothing on. Run these in order; each takes a couple of minutes and they
separate three different causes.

| # | Setting | If the shimmer goes away, the cause is |
|---|---|---|
| 8.1 | Smoothing **on**, Frame limit **Just under display** | Our fixed-grid phase projection under variable refresh. Fixable in code. |
| 8.2 | FreeSync **off** in AMD Adrenalin (not just Armoury Crate), fixed 120 Hz, smoothing on Match Display | The panel, not us. The Ally line has a tracked, application-independent VRR flicker issue. |
| 8.3 | Neither helped | Content-level and already known: texture aliasing under smooth eye pursuit, which 30 Hz stepping hides and 120 Hz motion reveals. |

**Capture while you do it.** Launch with `MDKR_PRESENT_PERF=1` and keep the log.
The `[PRESENTPERF-HIST] series=displayed-interval` rows show whether presents are
landing on the display's grid or spreading — this would be the first measurement
of a VRR panel this project has ever had.

---

## 9. Reporting

For anything wrong: the screen or track, whether smoothing was on, the tick rate
setting, the display rate, and what you saw versus expected. For §2.10, paste
the log row.

**A pass here is not a release.** Cutting the release is a separate step, gated
on this verdict.

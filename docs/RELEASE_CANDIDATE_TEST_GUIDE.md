# Golden Balloon 1.0.5 acceptance guide

Use this guide only with artifacts built from the same clean candidate commit.
Do not publish, retag, or substitute a rebuilt file after testing begins.

Use a legally owned US 1.1 or European 1.1 ROM. Share text logs and hashes in a
bug report, not ROMs or ROM-derived captures.

## 1. Record the candidate

For each desktop artifact, record its filename and SHA-256. Verify the adjacent
`.sha256` file where supplied and inspect `.provenance.json`: version must be
`1.0.5`, `commit` must match the candidate commit, and its recorded hash
must match the artifact.

For the browser build, open `build-info.json` and confirm version `1.0.5`, the
same source commit, and `source_dirty: false`. Hard-refresh before testing.

Stop if any identity differs. Do not test an archive in place: extract it to a
writable folder first.

## 2. Launcher and settings

Run these checks on macOS and Windows; repeat the browser-relevant steps on the
web build.

1. Start without a saved ROM. The launcher must remain responsive while a ROM
   is checked, and an invalid file must not replace the last valid selection.
2. Select both supported revisions if available. European 1.1 must offer
   English, German, and French; US 1.1 must offer English and French.
3. Resize down to 640×480 and test UI scaling. The layout must remain usable,
   and dragging the scale slider must not flash or flicker.
4. On a Windows touchscreen or handheld such as ROG Ally, use 1.25× scale or
   larger. Tap every navigation mode, a combo choice, a checkbox, and the
   primary action. Swipe Settings from non-interactive text and confirm it
   follows the finger; a drag beginning on a slider or scrollbar must operate
   that control instead of moving the page. Open the F1 overlay and confirm its
   guidance changes to touch instructions.
5. Change master, music, and effects levels. Previews must be audible and free
   of obvious clicks; active loops must react immediately.
6. On Windows, toggle borderless fullscreen from Settings, **F11**, and
   **Alt+Enter**. Restart and confirm the saved choice returns.
7. Remap a controller button, disable rumble, then test Light, Balanced, and
   Strong. Restore defaults and confirm no stale binding remains.
8. Change a restart-scoped video setting during play and choose **Restart &
   Apply**. The same ROM must reopen. A forced startup failure must return to a
   usable launcher with diagnostics rather than exit.

## 3. Core gameplay and presentation

Use WebGPU with Restored presentation unless a step says otherwise.

1. Watch the opening logos and title sequence at 16:9 and ultrawide sizes. The
   logo animation must fill the horizontal presentation without stretching;
   its authored top and bottom bands and centered copyright text must remain.
2. Open track select and move through its track and vehicle transitions. Live
   previews must remain inside the wooden frame with no side bleed. The
   decorative menu background must reach both display edges without black
   gutters, stretching, or a visible tile seam. Neighboring carousel labels,
   question marks, and navigation arrows must not appear in the widescreen
   side areas before their card enters the authored canvas. Once the expanding frame is
   gone, the vehicle/time-trial setup backdrop must fill the screen in Hor+.
   Back out once: the backdrop must return inside the wooden frame on the first
   frame of the reverse transition, with no flash or one-frame bleed.
3. Finish a single-player race. Check every post-race page, including Race
   Order, times, records, and options. Initial unframed footage must use the
   widescreen presentation; from the first wooden-frame transition onward,
   footage must remain inside its aperture with no one-frame bleed or FOV pop.
4. View animated credits. The moving background must fill the horizontal
   presentation while text remains in the safe reading area. Ordinary races
   must still fill the selected widescreen aspect.
5. Open the F1 overlay during a timed race. The kart, race clock, and world must
   freeze. Engine and ambient effects must fall away while quieter music keeps
   playing; resume must restore the mix cleanly. Escape must back out or request
   quit, never close the desktop app immediately.
6. Complete a three-lap race with music enabled. The background music must
   audibly accelerate when the final lap begins and remain stable at the faster
   tempo through the finish; it must not restart, stutter, or change pitch.
7. Drive a car, hovercraft, and plane. Engine sound must track throttle and
   speed, with no growing delay, breakup, or stuck loop.
8. Test Original cadence first. Then use Match Display or a numeric frame limit
   with Motion smoothing set to Interpolated. Gameplay speed must not change.
   Look closely at kart shadows, wheels, particles, fades, split-screen seams,
   and camera cuts for flicker, doubling, or intermediate-frame artifacts.
   Interpolated is a preview: UV-scrolled static level surfaces such as
   waterfalls still advance on authored ticks and may look stepped or shimmer
   during camera motion. Record that separately; new geometry corruption,
   tearing, or artifacts outside this documented boundary remain failures.
9. Return Motion smoothing to Off and Frame Limit to Original. Confirm the
   authored presentation remains stable.

## 4. Taj

Test once on a fresh save and once on an existing save.

1. Enter `ABRACADABRA`. “Taj has joined the race” must be followed by a visible
   Taj slot in the actual character picker, regardless of which retail
   characters are unlocked.
2. Select Taj with keyboard and controller. His portrait, name, voice, horn,
   HUD, pause screen, and results identity must be his own.
3. Race with car, hovercraft, and plane selections. Taj must use the scaled,
   animated magic carpet; it must not appear as a flat oversized sheet or cast
   a large character-picker shadow.
4. Test two-player selection with Taj in either port. Player ownership and
   results order must remain correct.
5. Complete a Taj Time Trial and confirm it does not replace an original
   character's canonical record or ghost.
6. Relaunch and confirm the unlock persists. Exercise save import and erase;
   the unlock state shown by the UI must match the documented operation.

## 5. Browser custody and mobile controls

1. Import a ROM, reload, and confirm the browser restores it locally. In
   developer tools, no request may contain a ROM filename or bytes.
2. Export a save, erase stored progress, import the backup, reload, and verify
   the preview and progress.
3. Repeat save management with WebGPU unavailable. Play may be blocked, but
   backup, restore, and erase controls must remain usable.
4. On a touch device, steer while holding Go plus Drift or Item. Rotate the
   device and collapse/expand browser chrome; controls must stay reachable and
   no input may remain stuck.
5. Do not rely on a browser-specific site-setting toggle. Serve the candidate
   with a `Permissions-Policy: fullscreen=()` response, then press the
   fullscreen button. Play must continue in the window and a visible
   explanation must appear. Remove the policy, open the same candidate directly
   in a top-level tab, and confirm the next attempt succeeds. The automated
   browser gate also injects a rejected exit request; its message must say that
   fullscreen could not be exited and offer Escape/browser controls.
6. Force browser storage unavailable, import a valid ROM, and start it. The ROM
   must remain usable for the session, the warning must leave save management
   unobscured, and **Retry browser storage** must persist the retained ROM after
   storage is restored without asking for the file again.
7. With keyboard focus on the game canvas, force a WebGPU startup failure. The
   launcher must return, announce a usable error, and move focus to that visible
   recovery message rather than leaving it on the hidden canvas.

## 6. Platform packaging

Know what the hosted workflows already proved for each artifact, so this pass
covers the rest rather than repeating it. `macos-release.yml` runs the packaged
app through LaunchServices on a `macos-14` Apple silicon runner and requires
WebGPU-default startup and successful surface presents there; that smoke runs
with `MDKR_AUDIO=0` and touches no audio device, controller, or hotplug. The
Linux AppImage and tarball are
uploaded only after `release.yml` renders and content-checks them under Xvfb
with Mesa's lavapipe/llvmpipe software stack, both as built and as extracted
through `AppRun` — no hosted Linux job has ever touched a physical GPU. The
Windows zip is built, import-checked, extracted, and launched from an unrelated
directory, but `release.yml` never uploads it; it may be attached only after
this pass succeeds on Windows hardware.

### macOS Apple silicon

Verify the DMG sidecars, mount it read-only, copy `mdkr64.app` to a fresh folder,
and launch from Finder without renderer environment variables. An
unidentified-developer prompt is expected for the unsigned candidate; a
“damaged” warning is a failure. Diagnostics must report WebGPU. Test audio from
physical speakers or headphones, device hotplug, pause/resume, and clean quit.

### Windows 10/11 x64

Extract the whole `GoldenBalloon` folder and launch `GoldenBalloon.exe` from an
unrelated working directory. Test a ROM and save path containing non-ASCII
characters. For the long-path arm, create nested named folders until the full
ROM path is longer than 260 characters, copy the ROM there, and select it
through the native picker. Create a separate >260-character data root. In
Command Prompt, set `MDKR_SAVE_DIR` to that exact full path before running
`GoldenBalloon.exe`; record both full path lengths. Launch a race, save, return
to the launcher, and use Restart & Apply.
Confirm diagnostics, the long save root, and the long ROM remain usable after
the restart. Check WebGPU, fullscreen, controller input/remapping, rumble,
physical audio, saves, diagnostics, and Restart & Apply. Start the extracted
app cold at least five times and watch the full logo/title/character-select
sequence; audio must not begin behind video and then catch up while waiting at
character select. The package must not require a bundled DLL.

### Linux x86-64

Test both the AppImage and extracted tarball on a physical GPU. Check WebGPU and
diagnostic OpenGL under the available X11/Wayland session. ROM selection is by
drag/drop or an absolute typed path. Record the distribution, display server,
GPU/driver, controller, and audio device; Linux remains best effort until this
physical breadth exists.

## 7. Campaign breadth and report

On at least one desktop candidate, play a clean save from start through credits,
including silver-coin races, later boss rematches, both Wizpig races, trophy
series, and save/reload boundaries. This manual route closes the campaign area
that automation deliberately does not claim.

Report:

- PASS or FAIL;
- candidate commit and artifact SHA-256;
- platform, OS, GPU/driver, display, audio device, and controllers;
- ROM revision and renderer;
- which sections above were run; and
- the first failed step with a redacted text log.

Any crash, damaged-app warning, renderer corruption, gameplay-speed change,
save loss, silent relaunch failure, input starvation, sustained audio defect, or
live image escaping a fixed frame is a release blocker.

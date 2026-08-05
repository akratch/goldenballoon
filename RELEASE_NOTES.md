# Golden Balloon 1.0.5

*Unreleased.*

Golden Balloon 1.0.5 is a fix release. It closes the six reported problems
listed below, repairs Return to Launcher and the diagnostic log on Windows,
and corrects launcher layout at small window sizes. No game data is included.

Recommended settings are unchanged from 1.0.4: **WebGPU**, **Restored**, frame
limit **Original**, motion smoothing **Off**, gameplay cadence **Original**.
Existing preferences are preserved when upgrading.

## Reported problems fixed

- **Course names show correctly again.** Fossil Canyon and Pirate Lagoon
  rendered as a single letter or as nothing in Track Select and Adventure, and
  the track names in the post-Wizpig 2 credits were affected the same way. The
  level-name table was allocated at half the size it needed on 64-bit builds.
  (#10)
- **The giant character portraits are back in Smokey Castle and Fire
  Mountain**, so you can tell which corner is yours without trial and error.
  The same fix restores the boost shockwave plume and Star City's rainfall,
  which were also drawing nothing. (#9)
- **Intro shrubs reach the ground** instead of floating with a repeated top.
  (#11)
- **PAL: the strip along the right edge of the screen is gone, and the track
  map is no longer cut off at the bottom.** European ROMs compose a taller
  surface than the renderer was assuming, and the port was clipping the world
  at the viewport edge where the console clips further out. NTSC output is
  unchanged, byte for byte. (#12)
- **Taj no longer freezes during his own balloon award ceremony.** Playing as
  Taj had truncated the animation set the ceremony actor shares with him. Taj
  also keeps his boost on zip pads now, and his Time Trial finishes are no
  longer discarded silently. (#13)
- **Return to Launcher returns to the launcher on Windows** instead of closing
  the whole application, including when the install path contains a space.
  (#14)

## Other fixes

- `mdkr64.log` is no longer empty. Launched from Explorer the app has no
  standard output to capture, but the file was rotated and truncated anyway,
  so every run left an empty log and destroyed the previous one. There is now
  something to attach to a bug report.
- The launcher's About destination is no longer clipped at common window
  heights, and the gold Play button keeps its lower edge at 800x600.
- Changing an unrelated setting no longer rebases a cadence or presentation
  choice that came from a preset or from the launcher.
- Audio recovery after a stall no longer clicks, and a stall part-way through
  a fade no longer leaves the fade stuck.
- A restart-scoped settings change that fails to stage or start now says why.
- Track Select no longer exposes a bare column at the right of its backdrop on
  wide displays.
- In the browser, two tabs open at once no longer overwrite each other's saves.
  The first tab owns the save store; a second tab plays as a spectator and says
  so, with import, edit, restore, erase, and drag-and-drop disabled. Exports
  still work in every tab. The page also works offline once loaded, and a
  cached page can no longer mix an old and a new build.
- Bounds and lifetime fixes across level text, audio, collision, ghosts,
  Controller Pak note names, particles, and the decompressor. Eighth place's
  racing line now matches what the original hardware actually did, wall recoil
  works again for planes and bosses, and a computer-carried egg is no longer
  drawn frozen.

## Not enabled in this release

A modern camera obstruction subsystem is present in the build but does not
change any camera. It defaults to an observe-only policy that measures and
records; the corrective policies are explicit opt-ins behind the
`MDKR_CAMERA_OBSTRUCTION` environment variable and are not supported in 1.0.5.

## Known limitations

- WebGPU with Restored presentation remains the qualified native and browser
  path. OpenGL is a diagnostic option, and Remastered is still work in
  progress.
- Motion smoothing remains a preview. Interpolated draws presentation-only
  in-between images; it does not run the game more often. UV-scrolled surfaces
  such as waterfalls, water, and lava still update on authored game ticks and
  may shimmer or step during camera motion. Motion smoothing Off is unaffected.
- The Windows archive is not published automatically. Hosted runners do not
  guarantee a qualifying GPU, so the Windows package is attached only after a
  human runs it on real Windows hardware and records the result.
- The desktop app supports keyboard and gamepad navigation, visible focus,
  scaling, contrast, and reduced motion. It does not claim a VoiceOver, UI
  Automation, or other screen-reader semantic tree.
- Linux still uses drag and drop or a typed ROM path instead of a native file
  picker.
- The macOS download is ad-hoc integrity sealed but not Developer ID signed or
  notarized. macOS may show an unidentified-developer warning on first launch;
  use **System Settings → Privacy & Security → Open Anyway** if needed.

For implementation details, see [CHANGELOG.md](CHANGELOG.md). Open work is
listed in [ROADMAP.md](ROADMAP.md).

---

# Golden Balloon 1.0.4

*Released 2026-08-04.*

Golden Balloon 1.0.4 adds playable Taj, smoother high-refresh presentation,
and a much better desktop launcher. It also fixes several audio, widescreen,
controller, and recovery problems found during extended play. No game data is
included.

## Recommended setup

| Setting | Recommended value | Why |
|---|---|---|
| Renderer | **WebGPU** | Qualified native and browser rendering path |
| Presentation | **Restored** | Widescreen and modern image quality without changing the original art direction |
| Frame limit | **Original** | Presents the game's authored motion and uses the least extra CPU/GPU time |
| Motion smoothing | **Off** | Stable default; avoids the known scrolling-surface interpolation limit |
| Gameplay cadence | **Original** | Preserves the original physics, AI, timers, audio service, and race pace |

These are the defaults for a fresh configuration. Upgrades retain existing
preferences, so check Settings if you previously selected an experimental
mode.

For smoother motion, try **Match Display** with **Motion smoothing:
Interpolated**. Interpolated draws presentation-only in-between images; it does
not run the game more often. **Enhanced** cadence is a separate compatibility
option that changes gameplay and ran the measured Bluey 2 route about 14%
faster. It is not recommended for accurate play. **Remastered** remains a
visual preview, and **OpenGL** remains a diagnostic backend.

## Playable Taj

- Enter `ABRACADABRA` or complete all three Taj challenges to unlock Taj.
- Taj appears as a normal character-select choice regardless of how many retail
  characters are unlocked. He has his own portrait, voice, horn, HUD, results
  identity, and handling.
- His magic carpet is scaled to the playable model and has its own animation;
  the oversized picker shadow and flat-carpet presentation have been removed.
- Taj works with car, hovercraft, and plane selections and in multiplayer.
- Taj Time Trials cannot overwrite the original roster's records or ghosts.
  Unlock state is preserved across relaunches and supported save operations.

## Frame rate, widescreen, and presentation

- Added safe presentation-only interpolation for cameras, racers, supported
  objects and deformations, particles, fades, and projected shadows. Gameplay,
  input, timers, RNG, audio, and saves remain on the original fixed tick.
- Fixed projected kart and character shadows flickering or snapping on
  interpolated frames.
- Fixed framed widescreen scenes. Track-select previews and results footage now
  stay inside their wooden frames instead of bleeding into the side areas.
- Restored full-width Hor+ presentation where no physical frame is present,
  including the opening logos, animated credits, Track Select setup, and the
  initial post-race footage.
- Track Select now extends its background cleanly on ultrawide displays without
  exposing incomplete neighboring labels, question marks, or navigation arrows.
- Transitions between contained and full-width views are treated as camera cuts,
  preventing one-frame blends between incompatible projections.
- Fixed high-refresh held-frame pacing that could consume a CPU core and cause
  audio or frame-time hitches when motion smoothing was Off.

## Audio

- Restored engine sound response to throttle and speed for cars, hovercraft,
  and planes without changing the gameplay RNG sequence.
- Fixed final-lap music on native builds. The sequencer now applies the faster
  tempo instead of changing only the displayed diagnostic value.
- Added persistent master, music, and sound-effects levels to native Settings.
  They stay synchronized with the original Audio Options menu, preview with
  click-free ramps, and report save failures instead of silently reverting.
- Bounded the native PCM backlog and added smooth recovery after rejected or
  dropped buffers.
- Fixed a Windows startup timing fault that could leave audio behind the video
  through the opening sequence and catch up at character select.
- F1 now applies a real pause: simulation and race effects stop while the
  quieter authored music mix continues.

## Launcher, controls, and platform fixes

- Reworked native Settings around Presentation, Frame Rate & Motion, Audio,
  Controller, and advanced graphics. Play remains visible, compact windows and
  high UI scales no longer overlap, and dragging UI scale no longer makes the
  interface flash. Larger controls and direct touch scrolling improve use on
  handheld PCs and touchscreens.
- ROM validation runs on a cancellable worker. Invalid or unsaved replacements
  do not displace the last playable ROM.
- Added **Restart & Apply** for restart-scoped settings. If relaunch staging or
  engine startup fails, the app returns to the launcher with diagnostics rather
  than exiting.
- Escape now follows the overlay's back and quit-confirmation flow. Controller
  state is cleared when the native window loses focus.
- Windows gains borderless fullscreen, **F11** and **Alt+Enter** shortcuts,
  controller remapping, selectable rumble strength, and Unicode and
  extended-length path handling.
- European 1.1 ROMs can select English, German, or French. The US 1.1 language
  menu remains English/French.
- Browser ROM and save replacement, local-storage failures, fullscreen denial,
  keyboard focus, and document navigation now have visible recovery paths.

## Known limitations

- WebGPU with Restored presentation remains the qualified native and browser
  path. OpenGL is a diagnostic option, and Remastered is still work in progress.
- Motion smoothing remains a preview. UV-scrolled surfaces such as waterfalls,
  water, and lava still update on authored game ticks and may shimmer or step
  during camera motion. Motion smoothing Off is unaffected.
- The desktop app supports keyboard and gamepad navigation, visible focus,
  scaling, contrast, and reduced motion. It does not claim a
  VoiceOver, UI Automation, or other screen-reader semantic tree.
- Linux still uses drag and drop or a typed ROM path instead of a native file
  picker.
- The macOS download is ad-hoc integrity sealed but not Developer ID signed or
  notarized. macOS may show an unidentified-developer warning on first launch;
  use **System Settings → Privacy & Security → Open Anyway** if needed.

For implementation details, see [CHANGELOG.md](CHANGELOG.md). Open work is
listed in [ROADMAP.md](ROADMAP.md).

---

# Golden Balloon 1.0.3

*Released 2026-08-02.* 1.0.2 released the same day and is superseded by this
release; see [CHANGELOG.md](CHANGELOG.md) for its notes.

- Fixed texture-cache identity and stale WebGPU binding state that could show
  old textures or filtering after menus and levels changed.
- Kept Remastered grading on the 3D scene instead of applying it to HUD text.
- Matched cutout mip coverage to the alpha threshold used by the renderers.
- Made Restored the default everywhere. Remastered remains opt-in.

WebGPU with Restored presentation remains the supported visual path for this
release. OpenGL and Remastered were not promoted by 1.0.3.

---

# Golden Balloon 1.0.2

*Released 2026-08-02.*

- Fixed Dino Domain door numerals changing or repeating as the camera moved.
- Fixed Windows save migration and save tools opening binary data with text-mode
  newline conversion.

---

# Golden Balloon 1.0.1

*Released 2026-08-01.*

- Replaced the broken macOS package with a self-contained SDL2 build and
  corrected its integrity sealing.
- Made WebGPU the native default after finding an opening-sequence rendering
  issue in OpenGL.
- Removed a blocking WebGPU queue wait that slowed Windows audio and menus.
- Fixed crashes when selecting numeric or Uncapped frame limits.
- Disabled the original motion-smoothing experiment because its retained data
  could be rewritten by the next game task.
- Fixed magic-code table loading and validation.
- Improved the launcher, in-game overlay, compact-window layout, diagnostics,
  preference recovery, and Return to Launcher behavior.

The frame-rate choices in 1.0.1 changed host presentation opportunities only;
they did not create additional unique game images. Original was the recommended
setting for that release.

## macOS first launch

The 1.0.1 macOS download is not notarized. If macOS blocks it as an unidentified
developer, open **System Settings → Privacy & Security**, choose **Open Anyway**,
and confirm. A “damaged and can't be opened” message is not expected from the
replacement package.

---

# Golden Balloon 1.0.0

*Released 2026-07-30.*

The first public release added the native launcher, browser build, WebGPU and
OpenGL renderers, audio, controls, saves, widescreen modes, and portable build
support around the decompiled game.

## ROM requirements

Golden Balloon does not include game data. It reads assets at runtime from a
ROM you provide. The 1.0.x releases support verified US 1.1 and European 1.1
images; unsupported revisions are rejected instead of being loaded.

## Project scope

This is an unofficial fan project. It is not affiliated with Nintendo, Rare,
Microsoft, or the original game's developers. See [DISCLAIMER.md](DISCLAIMER.md)
and [NOTICE.md](NOTICE.md) for licensing and provenance.

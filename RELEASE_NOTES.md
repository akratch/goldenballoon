# Golden Balloon 1.4.0

*Released 2026-08-18.*

This release fixes the graphical flickering reported with Motion smoothing,
the Adventure door launch, steering sensitivity, Wizpig's character select,
Taj's theme music, and texture pack sizing. It also adds an optional
widescreen HUD layout.

Recommended settings: **WebGPU**, **Restored**, frame limit **Original**,
Motion smoothing **Interpolated** on 120 Hz displays or **Off** elsewhere,
gameplay tick rate **Original**, camera **Authored**.

WebGPU with Restored presentation remains the qualified native and browser
visual path. Interpolated draws presentation-only in-between images from
adjacent game ticks; your inputs and the simulation always run at the
authored rate.

## Motion smoothing

- Fixed: world geometry — most visibly the Timber's Island waterfalls —
  could flash, vanish for a frame, or shear while the camera moves with
  Motion smoothing on (issue #36). The cause was in how level geometry was
  matched between game ticks for smoothing; mismatched geometry could be
  drawn blended toward the wrong shape on in-between frames.
- Frame pacing now measures your display's actual refresh rate instead of
  assuming the nominal one, which removes slow drift during long sessions.
- Scrolling textures (rivers, falls, lava) move smoothly between ticks
  instead of stepping. Menu and cutscene cameras are smoothed.

## Gameplay and content fixes

- Exiting a race in Adventure mode no longer launches the kart upward off
  the rising door (issue #41). This applies to quitting mid-race, quitting
  during the starting camera pan, and returning after a win. The 1.3.x
  workaround that briefly disabled door collision has been removed.
- Steering strength follows the original handling curve, including with the
  60 FPS gameplay option (issue #37).
- Selecting Wizpig no longer starts the race as Krunch (issue #40).
- Taj's theme plays with all of its instrument channels (issue #39).
- Texture packs: replacement textures are matched by their logical size, so
  packs fit correctly (issue #34).

## Added

- Widescreen HUD (issue #38): a new toggle in Video settings anchors the
  balloon count, lap counter, and minimap to the edges of a widescreen
  display instead of the 4:3 center. Off by default.

## Compatibility

Save data, settings, unlocked Magic Codes, and Time Trial ghosts from 1.3.0
carry over unchanged. Phone Party and Online Room remain out of the
player-facing build, exactly as in 1.3.0. The launcher is keyboard and
gamepad operable, but does not claim a
VoiceOver, UI Automation, or other screen-reader semantic tree.

## Known reports under investigation

One Windows report of characters briefly appearing unanimated ("T-posing")
at race start (issue #35) could not be reproduced from this release's
source code, including when building and testing the exact 1.3.0 code. If
you saw this on 1.3.0, please retest on 1.4.0 and report either way in the
issue.

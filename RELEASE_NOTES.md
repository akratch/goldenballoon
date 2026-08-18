# Golden Balloon 1.4.0

*Released 2026-08-18.*

This release makes **Motion smoothing** a first-class, qualified way to play.
The interpolation engine was rebuilt to pace itself against your display's
real refresh rate, and the one artifact that had haunted smoothed play since
it shipped — world geometry (most visibly the Timber's Island waterfalls)
flashing or breaking apart while you drive — was traced to its root cause and
fixed. If you tried smoothing before and turned it off, this is the release
to try it again.

Recommended settings: **WebGPU**, **Restored**, frame limit **Original**,
Motion smoothing **Interpolated** on high-refresh (120 Hz) displays or
**Off** elsewhere, gameplay tick rate **Original**, camera **Authored**.

WebGPU with Restored presentation remains the qualified native and browser
visual path. Interpolated draws presentation-only in-between images from
adjacent game ticks — your inputs and the simulation still run at the
authored rate, and nothing interpolation draws is ever fed back into
gameplay.

## Smooth motion, without the glitches

- The waterfalls, cliffs, and world geometry no longer flash, vanish, or
  shear while the camera moves with Motion smoothing on. Waves, scrolling
  water, characters, and effects all keep their full smoothing.
- Frame pacing locks onto your panel's measured refresh instead of assuming
  a nominal one, so smoothed play holds an even cadence over long sessions.
- Scrolling textures (rivers, falls, lava) glide between game ticks instead
  of stepping; menu and cutscene cameras are smoothed too.

## Fixes you asked for

- **Exiting a race in Adventure no longer flings your kart** off the rising
  door — quit mid-race, quit during the starting pan, or win and return; you
  land by the door and drive away normally (issue #41).
- **Steering feels like the original again**, especially with the 60 FPS
  gameplay option: turning strength follows the authored handling curve
  instead of biting twice as fast (issue #37).
- **Wizpig is actually playable** in Adventure and Tracks — no more silent
  swap to Krunch (issue #40).
- **Taj's theme** plays with all of its instruments again (issue #39).
- **Texture packs fit**: packs are matched by each texture's logical size,
  so replacement textures land where they should (issue #34).
- **Widescreen HUD** (new toggle): anchor the balloon count, lap counter,
  and minimap to the edges of a widescreen display instead of the 4:3
  center (issue #38). Off by default.

## Compatibility

Save data, settings, unlocked Magic Codes, and Time Trial ghosts from 1.3.0
carry over unchanged. Phone Party and Online Room remain out of the
player-facing build, exactly as in 1.3.0. The launcher is keyboard and
gamepad operable, but does not claim a
VoiceOver, UI Automation, or other screen-reader semantic tree.

## Known reports under investigation

One Windows report of characters briefly appearing unanimated ("T-posing")
at race start (issue #35) could not be reproduced from this release's source
on any tested configuration, including the exact 1.3.0 code. The 1.4.0
Windows package is produced by the validated CI lane; if you saw this on
1.3.0, please retest on 1.4.0 and let us know either way in the issue.

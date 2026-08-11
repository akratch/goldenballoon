# Golden Balloon 1.2.1

*Released 2026-08-11.*

This is a focused patch for the in-game Settings overlay and Motion smoothing.
No game data is included, and existing preferences carry over unchanged.

Recommended settings are unchanged: **WebGPU**, **Restored**, frame limit
**Original**, Motion smoothing **Off**, gameplay tick rate **Original**, camera
**Authored**.

## Fixed

**Settings no longer crashes an attract demo.** The demo racers were being
asked to update with a zero-length game tick while the overlay held the scene.
Vehicle code cannot safely run that way. The overlay now freezes the demo at
the subsystem boundary and resumes it normally when closed. *(#28)*

**Pausing during the start-line flyover no longer delays player control.** The
camera used to keep advancing behind Settings even though the rest of the race
was frozen. Closing the overlay could therefore leave the player waiting for
the flyover to catch up. The exact camera pose is now held with the countdown,
then both resume together. *(#29)*

## Motion smoothing

Water and lava now move as a single picture: their wave deformation advances
with the scrolling texture and presented camera. The sky follows the camera
used for the in-between frame instead of the previous game tick, and a shadow
whose receiver triangles change order snaps to the new shape rather than
twisting between two different meshes.

Abrupt yaw and field-of-view changes, finish cameras, and spectator cameras now
cut cleanly. On adaptive-refresh displays, Golden Balloon also stops forcing
in-between frames onto a fixed timing grid when the display is not following
one.

These changes affect presentation only. Physics, input, timers, audio, and
saves still advance at the game's authored rate. Motion smoothing remains off
by default.

Interpolated draws presentation-only in-between images from adjacent authored
frames; they do not advance the game a second time.

## Acknowledgement

The correspondence rules behind this work were informed by
[DKR-R](https://github.com/ThatGuyMcd/DKR-R), an MIT-licensed recompilation by
[ThatGuyMcd](https://github.com/ThatGuyMcd). We adapted the ideas to Golden
Balloon's renderer and wrote the implementation and tests here; no DKR-R source
code was copied.

## Support boundaries

WebGPU with Restored presentation remains the qualified native and browser
configuration; other combinations run but are not what we test against.

The launcher is keyboard and gamepad navigable, and does not claim a
  VoiceOver, UI Automation, or other screen-reader semantic tree.

## First launch

- macOS is not notarized. If macOS blocks the app, use **System Settings →
  Privacy & Security → Open Anyway**. A “damaged” warning is not expected.
- Windows is not code-signed. If SmartScreen intervenes, choose **More info →
  Run anyway**.
- Bring your own legally acquired US 1.1 or European 1.1 ROM.

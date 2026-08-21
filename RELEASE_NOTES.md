# Golden Balloon 1.5.1

*Released 2026-08-21.*

A bug-fix release: it restores the cave echo, fixes the widescreen HUD
layout, and lets the game build from source on stock Ubuntu again. There
are no new features and no changes to gameplay.

Recommended settings: **WebGPU**, **Restored**, frame limit **Original**,
Motion smoothing **Interpolated** on 120 Hz displays or **Off** elsewhere,
gameplay tick rate **Original**, camera **Authored**.

WebGPU with Restored presentation remains the qualified native and browser
visual path. Interpolated draws presentation-only in-between images from
adjacent game ticks; your inputs and the simulation always run at the
authored rate.

## Fixes

- The echo in Treasure Caves and other tunnel levels is back. Sound
  effects were being sent to the wrong reverb, so deep in a cave they thinned
  out to nothing instead of echoing; they now use the proper big-room reverb,
  and the music keeps its own reverb (issue #49).
- The Widescreen HUD no longer pushes the pause menu and Taj's dialog boxes
  off-center, and the banana counter no longer overlaps the lap counter
  (issue #50). The Widescreen HUD is still off by default; turning it off was
  never affected.
- Building from source works again on stock Ubuntu 22.04.

## Compatibility

Save data, settings, unlocked Magic Codes, and Time Trial ghosts from 1.5.0
carry over unchanged. Phone Party and Online Room remain out of the
player-facing build, exactly as in 1.5.0. The launcher is keyboard and
gamepad operable, but does not claim a
VoiceOver, UI Automation, or other screen-reader semantic tree.

## Known reports under investigation

One Windows report of characters briefly appearing unanimated ("T-posing")
at race start (issue #48) still could not be reproduced from source, and the
game applies each racer's seated animation before the first frame is drawn.
If you saw this, please retest on 1.5.1 and report either way in the issue.

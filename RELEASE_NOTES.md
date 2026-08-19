# Golden Balloon 1.5.0

*Released 2026-08-19.*

This release removes the Controller Pak ghost limit, extends the draw
distance range, fixes the flicker when finishing a race, and hides the
mouse pointer during play.

Recommended settings: **WebGPU**, **Restored**, frame limit **Original**,
Motion smoothing **Interpolated** on 120 Hz displays or **Off** elsewhere,
gameplay tick rate **Original**, camera **Authored**.

WebGPU with Restored presentation remains the qualified native and browser
visual path. Interpolated draws presentation-only in-between images from
adjacent game ticks; your inputs and the simulation always run at the
authored rate.

## Time Trial ghosts

- The Controller Pak ghost ceiling is gone (issue #46). On original
  hardware a ghost is so large that the pak fills after a handful, which
  is why "Controller Pak is full" appeared with most blocks free. The game
  now keeps one ghost per track and vehicle outside the pak and swaps the
  requested one into the pak's own save slots automatically. The pak file
  format is unchanged, existing ghosts carry over, and the ghost that
  races you is byte-identical to one saved the original way.

## Fixes

- The screen no longer flickers in the moments after crossing the finish
  line (issue #44). The finish camera and the results menu briefly
  contended for the view; the handoff is now clean, and the post-race
  track preview is smoothed the same way.
- The mouse pointer is hidden while playing and returns whenever a
  launcher menu or overlay is open (issue #45).
- Building from source works again with the stock CMake on Ubuntu 22.04.

## Changed

- Draw distance in Settings → Extras now reaches 1600%, up from 400%
  (issue #47). This keeps coins and other pickups visible far enough out
  to plan a route in the coin challenges. The setting remains visual only:
  object behavior, AI, and timing are identical at every value.

## Compatibility

Save data, settings, unlocked Magic Codes, and Time Trial ghosts from
1.4.0 carry over unchanged. Phone Party and Online Room remain out of the
player-facing build, exactly as in 1.4.0. The launcher is keyboard and
gamepad operable, but does not claim a
VoiceOver, UI Automation, or other screen-reader semantic tree.

## Known reports under investigation

One Windows report of characters briefly appearing unanimated ("T-posing")
at race start (issue #35) still could not be reproduced from source. If
you saw this on 1.3.0, please retest on 1.4.0 or later and report either
way in the issue.

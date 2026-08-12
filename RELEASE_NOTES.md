# Golden Balloon 1.3.0

*Released 2026-08-12.*

This release replaces the withdrawn 1.2.1. If you downloaded 1.2.1 for
Windows, please update: that build could not save any launcher setting.
No game data is included, and existing preferences carry over unchanged.

Recommended settings are unchanged: **WebGPU**, **Restored**, frame limit
**Original**, Motion smoothing **Off**, gameplay tick rate **Original**, camera
**Authored**.

WebGPU with Restored presentation remains the qualified native and browser
visual path. The launcher is keyboard and gamepad operable, but does not claim a
VoiceOver, UI Automation, or other screen-reader semantic tree.

## New racers: Wizpig and Terry

Beat Wizpig a second time — or enter `WIZPIGPOWER` — and he joins the
character select. Beat the Dino Domain rematch — or enter `TERRYFLY` — and
Terry does too. Wizpig rides his rocket on plane tracks; Terry flies under his
own wings. Both race with normal attacks, items, and collisions, and each has
his own portrait, placard, and character-select entrance. Their runs don't
write Time Trial records or ghosts, so your leaderboards stay yours.

`CONTROL WIZPIG` and `CONTROL TERRY` in the Magic Codes menu switch each racer
on or off once unlocked.

## Phone Party (beta)

Pair phones as extra controllers by scanning a QR code from the launcher —
nothing to install. An approved phone keeps its seat if it reconnects, and a
phone that drops out leaves its kart coasting in neutral instead of handing
the controls to someone else. Once paired, direct controls keep working through
a brief room-service interruption and reconnect in the same approved seat.
We'd love reports from real living rooms while this is in beta.

## Magic Codes remember themselves

Codes you've unlocked and switched on come back after a restart. Codes that
change how a save is read, grant one-time rewards, show the credits, or can
lock the game deliberately do not restore themselves.

## Fixed

**Windows: settings save again.** The withdrawn 1.2.1 Windows build failed to
write its settings file at all — ROM path, preferences, and ROM removal were
all lost on restart. Saving now works regardless of how the executable was
built, and the fix is guarded on every platform we ship. *(#32)*

**Missing music and silver-coin chimes.** On some tracks the busiest musical
passages dropped notes, and the silver-coin pickup jingle could cut off after
its first note. Both were limits carried over from the N64's smaller audio
budget; the full arrangement now plays. *(#30)*

**No more getting stuck in doors and trees.** A kart that ended up inside a
locked door or a piece of scenery after an angled bounce could stay wedged
there. It's now pushed back out the same way the ground already pushes karts
out.

**A plugged-in but idle controller no longer disables the keyboard.** Player
one can drive on keys with a gamepad connected; whichever device you actually
use takes over.

**Small screens: the "Forget Remembered ROM" dialog is readable again** instead
of collapsing into a tall, one-word-per-line column on handhelds.

## Smoother in-between frames

Sudden camera pitch and roll changes now cut cleanly instead of one axis
blending while another snaps, and cutscene shots are as smooth as gameplay.
With Motion smoothing on, controls are sampled right before each game tick,
trimming about a frame of input delay at 60 Hz. These changes affect
presentation only: physics, timers, audio, and saves still advance at the
game's authored rate. Interpolated draws presentation-only in-between images
from adjacent authored states; Original repeats authored holds.

# Golden Balloon 1.1.1

*Bug-fix release.*

Three reported bugs are fixed, plus four more we found by looking for the rest
of their families. The settings screen has been rewritten so it says what each
setting actually does. No game data is included.

Recommended settings are unchanged: **WebGPU**, **Restored**, frame limit
**Original**, motion smoothing **Off**, gameplay tick rate **Original**, camera
**Authored**. Existing preferences are preserved when upgrading.

## Fixed

**Character portraits stayed on screen after a Smokey Castle victory.** The
results screen shrinks a wooden frame over the picture, and the portraits were
escaping it. They were escaping every other 2D clip in the game too — the frame
is drawn as a rectangle, and rectangles were ignoring the game's own clipping.
Fixed for every rectangle, not just that screen. *(#25)*

**Adventure Two mirrored the tournament announcement text.** "ROUND ONE /
ANCIENT LAKE" came out backwards over a correctly mirrored Ancient Lake.
Adventure Two mirrors the world by flipping the camera, and the lettering — which
is drawn as rectangles — was being flipped along with it. On the console,
rectangles are drawn straight to the screen and the camera cannot reach them.
Now they are. *(#27)*

The same report mentioned a mirrored pause menu. We could not reproduce that one
on any machine here, in any mode or window shape. This fix makes every
screen-space rectangle immune to the mirror by construction, so it should be
gone — if you still see it, please say so, and a screenshot would help.

**Boss races were unwinnable at 60 FPS.** With gameplay tick rate set to
Enhanced, the Walrus and Bubbler rematches accelerated to roughly three times
the speed the game intends and could not be beaten. Bosses are now held to their
own authored top speed. See "Enhanced tick rate" below for what remains. *(#26)*

**Menus scrolled twice as fast at 60 FPS.** Holding a direction on track,
character or file select stepped every 0.27 seconds instead of 0.53, so the
selection overshot what you were aiming at.

**Waterfalls and fences could stutter with motion smoothing on.** Surfaces whose
texture scrolls at certain speeds held their position for a frame instead of
gliding.

**Rain, snow, lens flare and rain splashes stepped instead of moving** with
motion smoothing on. Precipitation moves further across the screen per frame
than anything else in the game, so it read as strobing rather than falling.

**A hard camera cut during a cutscene could blend instead of cutting**, sliding
between two viewpoints that were meant to switch instantly.

**Black panels could appear in Remastered.** Some flat-filled rectangles were
being drawn through the lighting shader, which has no lighting information for
them.

## The settings screen

Rewritten. Settings are grouped by what you are trying to do rather than by
which part of the engine owns them, every setting says in one line what it does,
and the one setting that changes how the game *plays* is marked as such and kept
apart from the ones that only change how it looks.

Two things were in the wrong place and caused real confusion: **Gameplay tick
rate** sat next to **Frame limit**, which is why people reasonably assumed it
was a frame-rate setting, and **Camera** sat under Presentation. Both moved.

## Enhanced tick rate: what it is, and what is still wrong with it

**Original is the accurate setting and the default. If you want smooth motion,
use Frame limit and Motion smoothing — they raise the frame rate without
touching the game's logic at all.**

Diddy Kong Racing's physics and AI were written to run 30 times a second.
Enhanced runs them 60 times a second. That is a genuine change to the game, not
a display option, and parts of the original physics assume the slower step.

This release fixes the worst of it — the boss runaway that made rematches
unwinnable — and a number of related problems: acceleration was twice as quick,
the AI drove laps 3–8% faster, and bosses launched with half their intended head
start. Ordinary racers now run within **0.1%** of their authored pace.

**What is still off, measured:** boss races remain the weak spot. On one
measured route a boss finishes about 2% early; on another it finishes about 27%
early and spends most of the race with wheels off the ground that should be
planted. The cause is understood — the game's contact and suspension maths is
defined at the 30 Hz step rather than derived from it — and the work continues.

**If you want a guaranteed-authentic race, use Original.** It is bit-for-bit
identical to the game as shipped, and every change described here leaves it
untouched.

## The path to Enhanced being properly faithful

Worth saying plainly, because it explains why this is taking a while.

Running the simulation twice as often cannot reproduce the original exactly. The
collision test asks "did this wheel cross the ground since the last update", the
chassis angle is solved from pairs of wheels, and drag is sampled from one wheel
— these are not approximations of a smooth system, they *are* the system, defined
at 30 steps a second. Halve the step and they answer differently, and no scaling
constant fixes that.

There is an approach that is exact, and it is already in the game: run the
simulation at its authored rate and draw extra pictures in between.
Interpolated draws presentation-only in-between images — the game's own
state still advances only on its authored ticks. That is what
**Motion smoothing** does, and because the simulation never changes, the result
is identical to Original by construction. What it does not yet give you is the
faster *control response* that a 60 Hz simulation provides, and that is what we
are building next — sampling your input at 60 Hz and acting on it as late as
possible, without touching the physics.

So: Enhanced stays available and honest about its state, Original stays exact,
and the recommended route to a smooth 60 fps picture is Motion smoothing.

## Also

- macOS is not notarized; first launch needs System Settings → Privacy &
  Security → **Open Anyway**. Windows is not code-signed: SmartScreen → **More
  info → Run anyway**.
- No game data ships. Bring your own US 1.1 ROM.

## Support boundaries

WebGPU with Restored presentation remains the qualified native and browser
configuration; other combinations run but are not what we test against.

The launcher is keyboard and gamepad navigable, and does not claim a
  VoiceOver, UI Automation, or other screen-reader semantic tree.

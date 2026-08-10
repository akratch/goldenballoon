# Golden Balloon 1.2.0

*Released 2026-08-10.*

Golden Balloon 1.2.0 fixes the three bugs you reported, plus four more we found
by looking for the rest of their families, and adds the biggest set of features
so far: content packs, a launcher that can speak, an enhancements menu, and
developer tools. The settings screen has been rewritten so it says what each
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

## Content packs

Put a pack — a folder or a `.zip` with a `pack.ini` inside — in the `mods`
folder beside your saves, and its artwork replaces the game's. Packs can also
carry custom music as ordinary `.wav` files. Settings → Content lists every
pack the game found: the ones in use, and the ones it skipped with the reason.
Press `Tab` in-game to flick the replaced artwork off and back on, so you can
compare a pack against the original on the same corner.

Worth knowing before you rely on it:

- **A pack applies in every mode, including Original.** Installing the pack is
  the opt-in. With a pack installed, Original is no longer a pixel-for-pixel
  reference of the console — remove or disable the pack if that is what you
  need.
- A pack texture is shown at its own full size up close, but it does not yet
  carry the distance-reduced versions the original artwork has, so it can
  shimmer at long range.
- Adding or removing a pack takes effect at the next launch.

How to make one — the format, the filenames, and a tool that writes out every
texture the game draws under the exact name a pack needs — is in
[docs/MODDING.md](docs/MODDING.md).

## The launcher can speak

Settings → Accessibility now holds everything about how the game presents
itself to you — text size, camera shake, reduced motion — and a new one: the
launcher and settings can **say out loud** what the keyboard or controller is
focused on, and the race can announce your position, lap, item and finish.
Both are off unless you turn them on, and each is its own switch.

Plainly, so nobody is misled: this is the app speaking for itself. It is not a
screen-reader integration — VoiceOver and similar tools still cannot see into
the launcher. On macOS we have heard it speak; the same feature is built into
the Windows and Linux versions but has not yet been heard on real hardware
there. If you use it and it is silent, that report is valuable.

## An enhancements menu

Settings → Enhancements gathers the optional extras: a speedometer, draw
distance, model detail, and opponent skill. Every row says whether it changes
how the game plays or only how it looks, and one button resets them all. All
of them are off until you opt in.

## Developer tools

Settings → App window has a **Developer tools** switch, off by default. It
unlocks a console, a free camera, collision and object viewers, a performance
window, and a crash-report screen. Switched off, they do nothing at all.

## Does this game run my cartridge dump?

The website now has a checker page: point it at your ROM file and it names the
release you have and says whether the port runs it. The file is read in your
browser and **nothing is uploaded**.

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

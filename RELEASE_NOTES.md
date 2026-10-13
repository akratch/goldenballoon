# Golden Balloon 1.8.0

*Online multiplayer is still a beta.*

This update makes texture packs smoother to play with, brings controller file
picking to Linux and Steam Deck, and fixes music, finish banners and menu navigation.

- **Export your save from Settings.** Saves → Export save creates a portable
  `.mdkr-save` copy of all three adventure slots, unlocks and time records.
  Ghosts and launcher settings are stored separately, and existing files are
  never overwritten.
- **Texture packs load in the background.** Moving into a new area no longer
  makes the game wait for each replacement image to be read and decoded. The
  original texture may appear briefly while its replacement loads. This reduces
  the pauses caused by texture loading; it does not remove every possible source
  of uneven motion.
- **Enhanced 60 keeps the music together.** Busy scenes no longer run out of
  room for the extra audio updates and drop instruments. Taj’s selection music
  stays on Character Select, and playable Wizpig uses his own voice and horn.
- **Finish banners stay inside the picture frame.** With Widescreen HUD on,
  banners are no longer cut off incorrectly when the race zooms out. OpenGL also
  keeps the HUD inside its picture-frame and split-screen views at higher render scales.
- **Pick files with a controller on Linux and Steam Deck.** Choose your ROM or
  a pack ZIP inside the game window, navigate folders with the D-pad, select
  with A and go back with B. Content’s Open mods folder button works on Linux too.
- **Pack installation is more reliable.** Windows handles folder packs and
  non-English filenames correctly. macOS can install a ZIP from Downloads,
  Desktop or Documents without asking to open the containing folder. A wrong
  pick is refused before copying, and an existing pack is never overwritten.
- **Menus keep their place.** Escape, B and closing a panel no longer leave
  keyboard navigation stuck. Confirmations start on Cancel and speak their
  question when menu speech is enabled. The exact Character Studio controls
  now speak their names and values too.
- **Supported packs can replace plants and items with 3D models.** Palms,
  trees, balloons and bananas can use a pack’s models on WebGPU. OpenGL shows
  the original sprites. No model artwork is included.
- **Modern camera** holds steadier shots near walls and scenery. “Keep the
  camera out of walls” remains an optional, experimental camera mode. Authored,
  the default camera, is unaffected.
- Windows Character Workshop drafts save on a fresh installation. In the
  browser, damaged controller-pak and ghost files are set aside so they do not
  keep returning at startup.

You still need your own supported ROM, and any replacement art you want to use.
The recommended starting point remains WebGPU, Restored presentation, Original
gameplay tick rate and the Authored camera. Interpolated draws presentation-only
in-between images from adjacent game ticks; inputs and simulation retain the
selected gameplay tick rate.

Online Room ships in native desktop packages. The published browser build remains
local-only. Both players need the same version of the game. Both players must be
on the room screen before the race can start; it will not launch just because
you’re both connected.

---

# Golden Balloon 1.7.0

*Online multiplayer is still a beta; Adventure Party and custom characters are
new — see below.*

The launcher has been rebuilt around a home screen with the game's artwork, and
everything lives under Play, Content and Settings.

Adventure is no longer a one-player game. Two to four people on the same couch
can now pick their own racers, explore the worlds together and race through the
campaign as one party. High-resolution texture packs load straight from the zip.
There is also a Character Workshop for bringing your own racers into the game,
an option to skip the launcher and go straight back to the game you played last,
and a round of online improvements:
the room now shows you what your connection looks like before a race starts,
rides out a short network wobble instead of ending the race, and says so
straight away when the other player leaves.

Recommended settings: **WebGPU**, **Restored**, frame limit **Original**,
Motion smoothing **Interpolated** on 120 Hz displays or **Off** elsewhere,
gameplay tick rate **Original**, camera **Authored**. Unchanged from 1.6.0.

WebGPU with Restored presentation remains the qualified native and browser
visual path. Interpolated draws presentation-only in-between images from
adjacent game ticks; your inputs and the simulation always run at the
authored rate.

## Adventure Party

Turn on **Adventure Party** in Settings — it is off by default — then two,
three or four controllers pick their racers at the ordinary character select
and take the ordinary route into Adventure. After that it is the game's own
menus, doors, balloons, Taj and cutscenes the whole way.

- Everybody drives their own racer around every hub in split screen. Nobody
  sits out, and nobody hands the lead back and forth.
- Golden balloons, hidden keys and doors belong to the party. Whoever reaches
  one first counts for all of you, and the whole group moves through together.
- One player pausing pauses everyone. Pull a controller out mid-game and the
  game pauses and asks for it back rather than carrying on without you.
- Silver coin races are shared: any of you can pick a coin up, it disappears on
  every screen, and the eight you need are counted across the party.
- Taj transforms the whole party at once — car, hovercraft or plane, everyone
  together, in one go.
- An ordinary balloon race runs a field of six: your two to four players plus
  enough computer racers to make a real race of it.
- Trophy races use the full eight-racer field. Everyone contends for position,
  and the championship follows player one's finish, the way the original
  ceremony has one winner.
- Player one holds the save file and makes the shared calls — which file to
  load, dialogue choices, and quitting.
- Adventure Two plays the same way, with its mirrored tracks and its own coins.

**What Adventure Party does not do yet:**

- Boss races and the four-racer challenges — Taj's, battles, eggs and bananas —
  are played by player one alone. The rest of the party waits, and the same
  party comes straight back afterwards, in the same places.
- Who is playing is fixed the moment you pick a save file. Nobody joins or
  leaves partway; go back to the title to change the line-up.
- It is for people in the same room. Adventure Party never goes online.
- One shared campaign on player one's file, not a save each.
- Save states are unavailable while a party is in progress.
- It is new, and it has not been played end to end on every platform or through
  every corner of the campaign. If a hub, a door or a course misbehaves with a
  full party, please tell us on the GitHub issues page.

## Character Workshop

Bring your own racers in. The launcher has a **Character Workshop**: point it at
a model — a `.glb` file, or a `.dae` or `.zip` it converts for you — give it a
name, a portrait and a licence, look over how it stands and how it sits in the
car, hovercraft and plane, then install it. Installed characters show up at
character select and can be handed to any of players one to four.
The [modding guide](https://github.com/akratch/goldenballoon/blob/v1.7.0/docs/MODDING.md)
walks through it.

- Nothing changes until you say so. The Workshop reads a package, shows you what
  it contains and how it differs from what you already have, and only then
  offers to install it.
- A character carries how it looks, not how it drives. You pick one of the ten
  original racers as its stand-in, and the custom racer handles exactly like
  that racer does.
- Portraits, the HUD, results and the minimap all pick your character up.
  Names in other alphabets keep their own shaping.
- Characters carry near, middle and far versions of themselves, so a full
  four-player screen has less to draw.
- Course records, Adventure saves and Time Trial ghosts stay ordinary game
  data. Ghosts recorded by an added character are kept apart from the original
  racers' records, as they were in 1.6.0.

**Honest limits:**

- **Custom characters are experimental.** This is an early slice, not a settled
  feature. Expect rough edges, and expect details to change.
- They need the **WebGPU** backend. On OpenGL the original racer is drawn in
  their place.
- Online, the other player sees the built-in racer yours borrows from, not your
  character. The select screen says so on the seat.
- The Workshop cannot tell you whether you have the right to use somebody's
  model. It asks you to confirm that you do, for your own machine, and it never
  ships anyone else's work with the game.

## The launcher

The home screen shows the presentation, frame rate and content packs you are
about to play with, and one button to start.

**Play** holds the game and every way to play it, online included. **Content**
holds the packs and characters you add yourself. **Settings** holds the rest,
with About & support underneath. To pick your game file, drag it anywhere in the
window or paste a path.

## Skip the launcher

**Skip the launcher** (issue #60) is off by default. With it on, the app opens
the game you played last instead of the launcher. Holding Shift, or both
shoulder buttons on a controller, while it opens shows you the launcher anyway,
and the setting lives in the in-game settings under Advanced — so turning it
back off never needs the launcher. The game file is checked before it opens,
exactly as pressing Play checks it: a file that has moved or changed lands you
in the launcher with the reason.

## Content packs

**High-resolution texture packs work as they come.** Put the zip or the folder
in your mods folder, or press **Install pack…** in Content and pick it, then
press Play. Nothing to unpack, convert or rename. Content lists the pack and how
many textures it supplies.

We ship no packs and host none. If you want one, the **DKR-R HDR Texture Pack**
is a community project led by `sr.gu` that redraws the original artwork in HD,
shared through the [DKR-R Discord](https://discord.com/invite/AMWfXdBjNP) and
linked from the [DKR-R project](https://github.com/ThatGuyMcd/DKR-R). Any pack
in the same Rice/GLideN64 format works.

No pack covers every texture, so some of the original artwork stays.

Content packs can also replace Taj's, Wizpig's and Terry's portraits. Those
three racers have no portrait in the original game, so the port draws its own; a
pack can put your picture in its place.
The [modding guide](https://github.com/akratch/goldenballoon/blob/v1.7.0/docs/MODDING.md)
has the filenames and the steps.

## Online multiplayer (beta)

Online Room ships in native desktop packages. The published browser build
remains local-only.

- **You can see the connection before the race starts.** Once both of you are
  in the room, the game measures the round trip between you and shows what it
  found — `~45 ms · steady` — beside the room. Pressing Start never waits for
  it; while it is still working the chip says so. A slow route is reported, not
  refused, and the race gives itself a little more room to absorb it.
- **A burst of lost packets no longer ends the race.** Inputs that go missing
  are asked for again on a channel of their own and filled back in, instead of
  the race stopping when the gap gets too old to recover.
- **When someone leaves, you find out at once.** The room now tells the other
  player the moment your opponent's connection closes, rather than leaving them
  racing an opponent who is not there for twenty-odd seconds. A brief hold
  first means a hiccup in the pairing service that recovers on its own is not
  mistaken for somebody quitting.
- **A wobble reads as a wobble.** The race says `Connection hiccup — retrying`
  while the link is late, and only says `Connection lost` when it really is,
  instead of jumping straight from "fine" to "gone".
- Each channel between you now carries its own key.
- **Both players need the same version of the game.** A different version is
  declined when joining, so update together before you race.

**What online does not do yet:**

- Both players must be on the same platform.
- A race is two players, not more.
- You can't join a race after it starts.
- If the host leaves, the race ends.
- Some networks can't connect two players directly. There is no relay
  yet, so those pairs can't race online for now.
- The service that pairs you can see that you're both connected.

Online is early access and a work in progress. Please tell us what you hit —
good or bad — on the GitHub issues page.

## Fixes

- **Taj's portrait is a blue elephant.** The original procedural results/HUD
  card now has a blue face and trunk instead of the tan placeholder. Existing
  portrait packs continue to work without renaming their files.

- **You can switch the bonus racers off.** Taj, Terry and Wizpig are on by
  default, as before. Turn **Bonus racers** off for the roster the original game
  shipped with, and they stop appearing on the character select. Anyone you have
  already unlocked is remembered and comes straight back if you turn it on
  again. Takes effect after restarting the game.

- **Opponent skill is a dropdown.** Choose Original, Hard or Brutal without
  typing a configuration value. Each choice explains its effect; changes take
  effect after restarting the game. Original retains the original speeds.

- Native release packages now include the **Online Room** beta they describe.
  The release builder could previously leave that entry out even when an
  online-enabled qualification build had it present.
- A racer wearing a custom appearance no longer drops back to its original
  character in an Adventure Party hub, at any **Model Detail** setting.
- Custom characters now use the same colour handling and the same highlight
  response as the rest of the picture, so one no longer looks washed out or
  lit from the wrong place next to the original racers.
- The Character Workshop's arrows and ticks draw as arrows and ticks instead of
  empty boxes.
- A four-player Adventure Party hub draws far more than the one-player screen
  the game set aside room for, which could corrupt the picture or take the game
  down. The game now sets aside room for the party it is actually showing, and
  a picture that runs past its buffer stops there instead of drawing whatever
  came next in memory.
- A damaged or hand-edited custom character file is turned away instead of
  taking the game down with it.
- A character name too long for its label is now cut between letters, so an
  accented or non-Latin name can never end in half a letter.
- The sky in split-screen races no longer has black bars down each side on
  widescreen displays. The two-player backdrop was drawn at the original 4:3
  width, so the edges of the screen were left unpainted — most obvious on
  Fossil Canyon and the other tracks with a black horizon.

## Known issue

- **Keep the camera out of walls** remains an optional, experimental camera
  mode. Near some continuous walls it can briefly release and reapply its
  correction, and a three-player spectator view has shown a single side switch.
  **Authored**, the default camera, is unaffected. The broader motion-quality
  check also fails in 1.6.0 on brief correction re-engagements, so this known
  class is not new to 1.7.0; it remains open rather than being counted as fixed.

## Compatibility

Save data, settings, unlocked Magic Codes, and Time Trial ghosts from 1.6.x
carry over unchanged. Two settings rows are new — **Adventure Party** and
**Skip the launcher**, both off by default; **Content packs** was already
there. The launcher is keyboard and gamepad operable, but does not claim a
VoiceOver, UI Automation, or other screen-reader semantic tree.

# Golden Balloon 1.6.0

*Released 2026-09-01. Online multiplayer is a beta — see below.*

Golden Balloon can now play online. Race a friend head-to-head over the
internet — private races with a six-digit code, single races or full
tournaments, across regions. This release also fixes T-posing characters,
wobbly best-time numbers, and several widescreen-HUD glitches.

Recommended settings: **WebGPU**, **Restored**, frame limit **Original**,
Motion smoothing **Interpolated** on 120 Hz displays or **Off** elsewhere,
gameplay tick rate **Original**, camera **Authored**.

WebGPU with Restored presentation remains the qualified native and browser
visual path. Interpolated draws presentation-only in-between images from
adjacent game ticks; your inputs and the simulation always run at the
authored rate.

## Online multiplayer (beta)

Create a private race and share the six-digit code — or the QR code — and
your friend joins with it. Pick your racer, pick a track from the full track
list, and play a single race or a tournament. When you connect, both screens
show the same three groups of words; read them aloud to each other, and if
they match, you know you are linked to the right person.

- North American and European copies of the game race each other. Online
  races run at the same speed for everyone; offline play still runs at your
  region's original speed.
- Online races are for two players in this beta.
- **Tested so far on macOS only.** We race on macOS ourselves. Windows and
  Linux have online too, but nobody has played it there yet — if you do, you
  are the first, and we want to hear how it went.
- **Both players need the same platform for now.** Mixed pairs, like Mac to
  Windows, are declined when joining; cross-platform play comes in a later
  update.
- **Both players need the same version of the game.** A different version is
  declined when joining, so update together before you race.
- Online play runs over Golden Balloon's free hosted service.

**What online does not do yet:**

- Both players must be on the same platform.
- A race is two players, not more.
- You can't join a race after it starts.
- If the host leaves, the race ends.
- Some networks can't connect two players directly. There is no relay
  yet, so those pairs can't race online for now.
- The service that pairs you can see that you're both connected.

Online is early access and a work in progress. Please tell us what you hit —
good or bad — on the GitHub issues page. Your reports decide what gets
improved next.

## Fixes

- Characters no longer briefly appear unanimated ("T-posing") at the start of
  a race, at any **Model Detail** setting (issue #48).
- The **best time** and **best lap** numbers in track previews no longer rock
  back and forth during the preview flyby (issue #59).
- With the widescreen HUD on, the banana counter no longer overlaps a
  portrait in the banana challenge, and minimap markers line up with the map
  at the correct size (issue #57). With it off, nothing changes.
- Added characters can now save Time Trial ghosts, kept separate from the
  original racers' records (issue #54).

## Compatibility

Save data, settings, unlocked Magic Codes, and Time Trial ghosts from earlier
1.5.x releases carry over unchanged. The launcher is keyboard and gamepad
operable, but does not claim a VoiceOver, UI Automation, or other
screen-reader semantic tree.

# Golden Balloon 1.5.2

*Released 2026-08-25.*

A bug-fix release closing the reports that came in after 1.5.1: the
Widescreen HUD lays out correctly across the reported screens, a stray
rectangle in the menus is gone, Time Trial times and ghosts save reliably on
Linux, a Hot Top Volcano out-of-bounds exit no longer hangs, and Nintendo
Switch Online N64 controllers map correctly. There are no new features and no
changes to gameplay.

Recommended settings: **WebGPU**, **Restored**, frame limit **Original**,
Motion smoothing **Interpolated** on 120 Hz displays or **Off** elsewhere,
gameplay tick rate **Original**, camera **Authored**.

WebGPU with Restored presentation remains the qualified native and browser
visual path. Interpolated draws presentation-only in-between images from
adjacent game ticks; your inputs and the simulation always run at the
authored rate.

## Fixes

- The Widescreen HUD — the "Expanded HUD" from the reports — now lays out
  correctly: Time Trial lap times no longer pile up on the right, the TAJ
  MAGIC label is centered, the battle-mode HUD sits where the game intended,
  and the race-start slide begins off screen instead of parking at the right
  edge (issue #51). It is still off by default, and the normal HUD is
  unchanged.
- Switching Game Paks in Save Options, scrolling the name-entry letters, and
  the end credits no longer flash a thin stretched bar across the screen. The
  game occasionally asks for a rectangle drawn with its corners swapped; real
  N64 hardware draws nothing for those, and now the port matches — which
  clears the same artifact everywhere it appeared (issues #52 and #56).
- On the PAL (European) version, the menu text no longer creeps upward after
  you return to the launcher and start the game again.
- The brief blue flash in the Walrus Cove cave is not a bug — it is faithful
  to the original game, which draws its own out-of-bounds curtain in exactly
  that blue. It looks identical with every enhancement turned off (issue #53).
- Time Trial times and ghosts now save reliably on Linux (and other
  non-macOS native builds). Saves resolve to your per-user profile
  directory; if you were relying on a folder-local `save/` it is kept in
  place, and you can force folder-local saves with a `portable.txt` beside
  the game. The game also now tells you, instead of failing silently, if it
  ever cannot write your progress (issue #54).
- Leaving Hot Top Volcano through the out-of-bounds trophy-storage route no
  longer hangs on a black screen; it now returns to the hub exactly as the
  original game does, so the trick still works (issue #55).
- Nintendo Switch Online N64 controllers now map correctly over Bluetooth —
  the C-buttons no longer open the overlay or double as Z. You can also now
  change which button opens the in-game overlay in Settings ▸ Controls
  (issue #55).

## Compatibility

Save data, settings, unlocked Magic Codes, and Time Trial ghosts from
1.5.0 and 1.5.1 carry over unchanged. The launcher is keyboard and gamepad
operable, but does not claim a VoiceOver, UI Automation, or other
screen-reader semantic tree.

## Known reports under investigation

One Windows report of characters briefly appearing unanimated ("T-posing")
at race start (issue #48) still could not be reproduced from source, and
the game applies each racer's seated animation before the first frame is
drawn. If you saw this, please retest on this release and report either
way in the issue.

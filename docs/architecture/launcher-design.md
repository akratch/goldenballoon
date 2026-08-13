# Launcher design

The native launcher (`platform/app/`) is the first thing anybody sees. This
document is the design it is built to: the panel structure, the settings
taxonomy, the visual system, and the interaction rules. It is written as an
audit followed by a numbered change plan, so every delta can be traced to the
problem it solves.

The bar it is written against: *beautiful, on parity with modern game
launchers, super clean and clear, with no duplicative options and no
unexplained effects, and easy to navigate.*

---

## 1. Audit: what the launcher was

### 1.1 Panels

| # | Label | What it holds |
|---|-------|---------------|
| 0 | Game ROM | Drop zone, native picker, typed-path entry, validation verdict, **and the Phone Party host** |
| 1 | Online Room | Room browser for an online mode this release does not ship |
| 2 | Settings | 11 collapsible groups over ~56 controls |
| 3 | Diagnostics | Renderer/SDL/OS line, log path, copy-to-clipboard, log tail |
| 4 | About | Licence and provenance copy, key hints |

Four structural problems.

**The landing panel is a chore, not a destination.** "Game ROM" is a
first-run task. Once a player has chosen a ROM they never open it again — yet
it is index 0, it is what the launcher opens on, and it is named after a file
format. A modern launcher opens on the thing you came to do.

**Phone Party is filed under the wrong noun.** Turning phones into
controllers is a decision you make when you sit down to play, with people in
the room. It is at the bottom of the page about locating a cartridge image.

**Online Room ships a room for a mode that does not exist.** Race admission
is already fail-closed, so the panel is inert — which is worse than absent: it
is a promise the build does not keep. Commit `96fceab` hid it behind
`MDKR_ONLINE_ROOM_PREVIEW=1` and was reverted for release-scheduling reasons,
not design ones.

**Diagnostics and About are one idea.** Both answer "what is this and what is
it doing"; neither is a task.

### 1.2 The settings page

Eleven groups: Picture, Accessibility, Frame rate, Gameplay, Camera, Sound,
Controller, Advanced graphics, App window, Enhancements, Content — plus an
"Other settings" safety net.

The groups are a mix of three different questions. *Picture*, *Sound* and
*Camera* name a subsystem. *Accessibility* and *Gameplay* name a player
concern. *App window* and *Advanced graphics* name neither — "App window"
holds the window mode, the update check, and the developer-tools switch,
which have nothing to do with each other or with a window.

Two of the eleven have a single row (*Camera*). One is a lid over three
unrelated rows (*App window*). And the page opens with six of the eleven
expanded, so the first screen is a wall rather than a menu.

### 1.3 The control inventory

Fifty-seven schema keys, of which fifty-three are offered
(`Video.Widescreen`, `Video.MSAA`, `Video.TexturePack` are conditionally or
permanently hidden), plus two controls with no schema key: **Interface scale**
(a launcher preference) and **Presentation pace** (pure sugar over two keys).

`Settings_dumpSchemaContract()` is the source of truth and
`tests/check_a11y_shell.py` enumerates it, so the inventory below is generated
rather than remembered.

Label and help quality were, on the whole, good — this codebase has already
been through a copy pass, and the `Copy` table in `ui_settings.cpp` exists
precisely to keep config-file language off the screen. The defects that remain
are structural, and they cluster:

* **Effect timing is stated inconsistently.** A `RESTART`-scoped key gets a
  "Next launch" chip and, when it differs from live, a "Next Play: X. Running
  now: Y." line. A `LEVEL`-scoped key gets a "Next race" chip. A `LIVE` key
  gets *nothing* — so "this applies immediately" is communicated by the
  absence of a marker, which is not communication. Eighteen of the fifty-three
  rows say nothing about when they take effect.
* **Two controls write what other controls also write** (§2 below).
* **Three rows carry a description that repeats their own group's subtitle.**

### 1.4 Visual system

`AppTheme` already is the single source of truth for the palette and the
style metrics, and that is the right shape. What had leaked out of it:

* the navigation rail and top tabs hand-roll `AppTheme::hex(0x3A3A3D)`,
  `hex(0x284B7C)`, `hex(0x424247)` inline for their own hover/active states;
* the settings section header hand-rolls `hex(0x212127)`, `hex(0x2C2C33)`,
  `hex(0x37373F)`;
* several panels carry bare pixel numbers (`8.0f`, `92.0f`, `96.0f`,
  `190.0f`, `3.0f`, `10.0f`) multiplied by `uiScale()` at the call site.

Every one of those is a colour or a metric with no name, which is how two
surfaces that should match drift apart.

The typography ramp is 13 / 17 / 19 / 24 — four steps, which is right — but
there is no named spacing ramp beyond `kGapXS/S/M/L`, and the section rhythm is
assembled ad hoc in each group.

The selected navigation item is a **flat, full-bleed, fully-saturated cobalt
rectangle**. It is the loudest thing in the window, it fights the gold primary
action for attention, and it is the single element that most dates the
interface.

### 1.5 Interaction and accessibility

The accessibility contract is genuinely well built and must not be disturbed:
`ui::SpeakFocusedItem` and `ui::SpeakSection` are the only two announcement
points in the app, every settings row reaches them through `drawKey()`, and
`check_a11y_shell.py` grades a real keyboard walk against the schema dump. Any
row added outside the shared helpers goes silent.

**This design therefore adds no hand-rolled rows.** Every control below is
either a `drawKey()` row or one of the two existing documented exceptions
(Interface scale, Presentation pace), both of which already call
`SpeakFocusedItem` explicitly.

The compact layout (`< 860 × uiScale` wide, or `< 620` tall) swaps the rail for
a header with a section dropdown. It works, and it stays first-class.

---

## 2. Duplicative and unexplained controls

Findings, each with a disposition. Persistence keys do not change meaning
anywhere in this list — renames are label-level, and every merged control reads
and writes the keys that already exist.

**D1 — "Presentation pace" writes the same two keys as "Frame limit" and
"Motion smoothing".** The quick choice (radio: Original / Smooth) and the two
individual combos are on the same page, in the same group, and the individual
pair is reachable behind a "Set the rate yourself" disclosure. A third state,
Custom, is printed as a sentence the control cannot produce.
*Disposition: merge into one control.* One row, **Frame rate**, with three
choices — Original, Smooth, Custom — where each preset's line names exactly
the two values it writes, and **Custom is a real, pressable choice that reveals
the three individual rows inline**. Off Custom, the individual rows are not
drawn at all, so the page offers one way to set the frame rate, not two.
Custom stays sticky for the session once entered, so a player who tunes their
way into a pair that happens to spell "Smooth" does not have the controls
vanish from under their hands.

**D2 — "Presentation" (`Video.Mode`) is a preset over the graphics keys, and
silently reads "Custom (Individual Settings)".** A default session shows a
value that is not in the combo's own option list, with nothing on screen saying
why. *Disposition: keep the key and the control* — it is a real persisted
setting, and unlike D1 there is no honest value to write for Custom — *and fix
the explanation.* The help states which settings the preset writes, and when
the value is Custom the row prints one line saying so in the player's words.

**D3 — the Camera group holds exactly one row.** *Disposition: remove the
group.* `Camera.Obstruction` moves to **Display** (it changes what you see);
`Camera.Comfort` stays in Accessibility, where the routing policy already puts
it.

**D4 — "App window" is a lid over three unrelated rows.** Window mode, the
update check, and developer tools. *Disposition: dissolve.* `Window.Mode` moves
to **Display**; `App.UpdateCheck` and `Tools.Enabled` move to **Advanced**,
which is also where the Interface-category catch-all loop moves so that a key
added to the schema still cannot go homeless.

**D5 — three separate statements that changes are staged.** A card at the top
of the page ("Ready for next launch"), a per-row line ("Next Play: X. Running
now: Y."), and a line at the very bottom ("Saved. They start with your next
Play."). *Disposition: keep the top card* (it is at the top, and it is the one
that names the action) *and the per-row line* (it is specific, and it is the
only place that says what is running now). *Remove the bottom line.*

**D6 — "Reset enhancements" states its scope twice**, once under the button and
once in the status line the action sets. *Disposition: keep the one under the
button*, which is readable before the player commits.

**D7 — `ui::RestartBadge()` and `Chip("Next launch")` are two spellings of one
fact** ("• restart required" versus a gold pill). *Disposition: delete
`RestartBadge`; the chip is the one spelling.*

**D8 — Motion smoothing is explained in three places** — the pace tooltip, the
Motion smoothing row, and the Frame limit help paragraph. *Disposition:* the
merged Frame rate control owns the short version; the Frame limit help keeps
the long one (it is pinned byte-for-byte by three CI contracts and is not
edited here); the Motion smoothing row keeps only what is specific to it.

**D9 — "Choose ROM" appears in the rail footer and in the Play panel.** These
are the same action at two weights: the footer is the persistent action slot
(it shows Play when a ROM is ready), and the panel's is the browse affordance
inside the drop-zone card, where a drop zone must have one. *Disposition:
accepted and made deliberate* — the footer keeps the filled brand primary, the
card's becomes a secondary "Browse…", so they read as one action at two
weights rather than as two offers.

**D10 — `ui::PrimaryButton` (cobalt) and `ui::BrandPrimaryButton` (gold).**
*Disposition: kept, and given a rule* (§4.3): gold is the launcher's single
persistent launch action and appears exactly once on screen; cobalt is the
primary action of an in-game overlay dialog.

---

## 3. Information architecture

### 3.1 Panels

Indices are load-bearing — `kLauncherPanelCount` and the smoke contracts pin
them — so the set keeps its five slots and its order, and the changes are to
what each slot *is*.

| # | Was | Is | Rationale |
|---|-----|----|-----------|
| 0 | Game ROM | **Play** | The home. It is what the launcher opens on, so it should be the thing you came for. |
| 1 | Online Room | **Online Room**, hidden unless `MDKR_ONLINE_ROOM_PREVIEW=1` | A shipping build offers exactly what the release offers. |
| 2 | Settings | **Settings** | — |
| 3 | Diagnostics | **Diagnostics** | — |
| 4 | About | **About** | — |

**Play** has two states, and they are the first-run flow and the returning
flow:

*No ROM yet* — one hero card, and nothing else competing with it: the drop
zone, a secondary "Browse…", and a collapsed "or paste a path" disclosure.
Validation feedback appears in that card, in plain language, with the action
that fixes it. This is the onboarding, and it is on the panel the launcher
already opens on, so there is nothing to hunt for.

*ROM ready* — the game's identity card (build string, verified state), a short
"what will launch" summary that names the presentation mode and the frame rate
so pressing Play holds no surprises, a secondary "Change ROM…", and the
**Play together on this screen** card that Phone Party moves into. Phone Party
is a shipping feature and is presented as one: no "beta", no "preview".

The launch action itself stays in the persistent footer/header slot, on every
panel, exactly as it already is.

### 3.2 Settings taxonomy

Nine groups, down from eleven, named for what a player came to do. Two open by
default; the rest are one keystroke away. Expert and diagnostic controls sit
behind a disclosure *inside* the group they belong to rather than in a separate
"advanced" ghetto — except the genuinely diagnostic ones, which are in
Advanced because that is what they are.

1. **Display** *(open)* — Presentation · Frame rate (+ ▸ Custom rows) ·
   Camera · Aspect ratio · Field of view · Window · Widescreen (only when a
   saved config is already on the legacy stretch path)
2. **Graphics** — Render scale · Anisotropic filtering · Mipmaps · Sharper
   lettering · World shadows · Remaster effects · MSAA (non-WebGPU only)
3. **Audio** — Master · Music · Sound effects
4. **Controls** — Rumble · Rumble strength · ▸ Button mapping (+ Restore
   defaults)
5. **Accessibility** *(open)* — Interface scale · Camera shake · Speak menus ·
   Speech speed · Speech volume · Speak race events
6. **Gameplay** — Gameplay tick rate · Menu languages. The only group whose
   rows change how the game plays, and the only one that carries the caution.
7. **Extras** — the enhancement registry's rows, grouped by its own categories,
   plus the scoped reset. Kept as one group because the scoped reset is defined
   as "exactly this group", and because "the extras I switched on" is a list a
   player looks for as a list.
8. **Content packs** — Custom content · Skipped packs · the installed/skipped
   accounting.
9. **Advanced** — Check for updates · Developer tools · the Interface-category
   catch-all · the unclaimed-key safety net.

Group membership is a routing decision, and it stays where routing decisions
already live (`AppUi_settingsSection`, `AppUi_shellPreferenceSection`), so
"exactly one section draws this control" remains a property a test can read.

### 3.3 Effect timing, stated on every control

Every row says when it takes effect, and the vocabulary is closed — three
words, learned once:

| Timing | Marker | Meaning |
|--------|--------|---------|
| Live | *(no chip; the help sentence ends "Applies straight away.")* | Changes now |
| Next race | grey "Next race" chip | Takes effect when a track next loads |
| Next launch | gold "Next launch" chip | Takes effect at the next Play |

The gap this closes: `LIVE`-scoped rows previously said nothing at all, so a
player had to infer immediacy from the absence of a badge. Every row's help now
ends with its timing sentence, generated from the schema scope rather than
written per row, so a key added tomorrow cannot be the one that forgets.

---

## 4. Visual system

`AppTheme` owns every colour and every metric. Panels compose named tokens; a
literal colour or a bare pixel number in a panel is a defect.

### 4.1 Palette

Existing brand tokens are unchanged — cobalt `#315C98`, amber gold `#D4A843`,
sky `#71BEF9`, and the semantic `good`/`bad`/`warn`/`subtle` set. What is added
are the tokens that were previously inline literals:

* `navSelected()` / `navHover()` / `navActive()` — the navigation rail and top
  tabs, so the two navigation surfaces cannot drift;
* `groupHeader()` / `groupHeaderHover()` / `groupHeaderActive()` — the settings
  section header;
* `focusRing()` — the keyboard/gamepad focus colour, so focus is one colour
  everywhere.

### 4.2 Spacing and type

The type ramp stays 13 / 17 / 19 / 24 (small / body / section / title). The
spacing ramp is named and used everywhere: `kGapXS 4` · `kGapS 8` ·
`kGapM 14` · `kGapL 22`. A group is: header, `kGapS`, rows separated by
`kGapS`, `kGapM` before the next header. One rhythm, applied by the shared
helpers rather than by each group.

### 4.3 Button hierarchy

Exactly one filled brand-gold action is visible at a time, and it is the launch
action. Everything else is a neutral filled button (`ImGuiCol_Button`) or a
plain disclosure. Cobalt fill is reserved for the in-game overlay's primary.
This is the rule that makes the gold button mean "this is the thing to press".

### 4.4 Selection and focus

The selected navigation item stops being a **full-bleed square slab** and
becomes an **inset, rounded cobalt pill with a gold leading rule** (rail) or a
rounded cobalt tab with a gold underline (compact header). The fill stays solid
brand cobalt; what changes is the geometry — inset from the rail edges, a real
corner radius, and the gold indicator doing the "you are here" work that
saturation was doing badly.

That distinction is deliberate and constrained. `tests/check_launcher_tabs.py`
identifies the selected destination by finding **exactly one** connected
component of `#315C98` within ±8 per channel and at least
`max(200, w·h/4000)` px in the navigation crop, and separately requires the
gold active indicator beneath it. Dropping the selection to a low-alpha tint
would make the launcher prettier and make that assertion undetectable — the
gate would still pass, on nothing. Keeping the fill solid and fixing the shape
gets the visual result without blinding the gate, which is the whole reason the
shape, not the alpha, is what moves.

Hover is a neutral lift, never a second selection colour. Keyboard focus is a
distinct ring in `focusRing()`, visible on every interactive element including
inside the compact dropdown.

### 4.5 The brand moment

`BrandWordmark()` (gold "Golden" + sky "Balloon") stays, and `BrandRule()`'s
checkered gantry stays as the identity nod. The rule's two 4 px rows read as a
compression artefact at 1.00× on a dark ground; it gains a defined height token
and slightly lower alpha so it reads as a deliberate finish line rather than as
noise.

---

## 5. Interaction model

* **Keyboard and gamepad first.** Focus order follows visual order in both
  layouts. No focus trap: every disclosure that can be opened can be closed
  from the keyboard, and the compact section dropdown is reachable as the first
  stop in its header.
* **Self-voicing is preserved by construction.** Every control is a `drawKey()`
  row or one of the two documented hand-rolled exceptions, both of which call
  `ui::SpeakFocusedItem` directly. No new announcement point is introduced.
  `check_a11y_shell.py` enumerates the schema and grades a real walk, so a row
  that escapes the helpers fails the gate.
* **Compact stays first-class.** Every group, every disclosure, and both Play
  states lay out at 640 × 480 and at 2.00× scale. The compact branch drops
  descriptions, never controls.
* **Errors are sentences.** Every failure names what happened, what state the
  setting is in now, and what to do — the existing `reportResult()` vocabulary,
  which is already written this way, is kept verbatim.

---

## 6. Change plan

Numbered, with the player-facing reason for each. Persistence keys are
unchanged throughout; a merged control reads the keys that already exist.

### Theme and shared primitives

1. **Hoist every inline colour into `AppTheme`** as a named token
   (`navSelected`, `navHover`, `navActive`, `groupHeader*`, `focusRing`).
   *Why: two navigation surfaces that should match were drifting apart.*
2. **Re-treat the selected navigation item** — inset, rounded cobalt pill with
   a gold leading rule, replacing the full-bleed square slab; the fill stays
   solid `#315C98` for the reason in §4.4. *Why: the loudest element in the
   window was a passive state indicator, competing with the launch action.*
3. **Delete `ui::RestartBadge()`** and standardise on the "Next launch" chip
   (D7). *Why: one fact, one spelling.*
4. **Add a timing sentence to every row's help**, derived from the schema
   scope. *Why: eighteen rows communicated "applies immediately" by saying
   nothing.*

### Panels

5. **Rename panel 0 to "Play"** and restructure it into the two states in
   §3.1. *Why: the launcher opened on a chore named after a file format.*
6. **Move Phone Party from the ROM panel into Play's ready state**, as "Play
   together on this screen", with no beta framing. *Why: it is a
   sitting-down-to-play decision and a shipping feature.*
7. **Hide the Online Room panel** behind `MDKR_ONLINE_ROOM_PREVIEW=1`, reusing
   the `panelVisible()` predicate and all five enumeration sites from
   `96fceab`. Indices stay stable; a hidden panel gets no tab and refuses
   selection by name or index. *Why: no teasers — a shipped build offers
   exactly what the release offers.*

### Settings

8. **Regroup to the nine groups in §3.2**, with Display and Accessibility open
   by default. *Why: eleven groups named after three different things, six of
   them open, is a wall.*
9. **Merge Presentation pace with Frame limit / Motion smoothing / Allow
   tearing** into the single Frame rate control of D1. *Why: one setting, one
   control.*
10. **Remove the Camera group** (D3) and **dissolve App window** (D4).
    *Why: a one-row group and a lid over three unrelated rows are both
    overhead.*
11. **Explain `Video.Mode = Custom`** on screen (D2). *Why: the default session
    shows a value that is not in the control's own list.*
12. **Remove the duplicated staged-changes line** (D5) and the duplicated
    reset-scope sentence (D6). *Why: saying it three times does not make it
    clearer.*

### Accessibility repairs found by the audit

14. **Voice the ROM panel's controls.** `ui_rom.cpp` contains zero
    `SpeakFocusedItem` calls, so all ten of its controls — Browse, Use This
    Path, the path field, Change ROM, Forget Remembered ROM, Retry Save, Cancel
    Check, and both confirmation buttons — are silent. `check_a11y_shell`
    cannot catch this, because it grades *schema* controls and these have no
    schema key. *Why: the panel a first-run player cannot get past is the one
    that says nothing.*
15. **Voice the Diagnostics copy action**, and announce its result the way
    Phone Party already announces its own copy.
16. **Fix the drifted a11y names in Phone Party** — the combo is voiced as
    "Controller Slot" while its visible label is "Controller slot", and
    "Decline" is voiced as "Decline Phone". The spoken name must be the drawn
    label; that is the same rule `drawKey()` already follows.

### Gates

17. Update `check_a11y_shell` (the panel-name tuple, and its prefix-matched
    section announcement), `check_app_capture`, `check_app_ui_input`,
    `check_launcher_tabs`, the `kLauncherPanelCount` smoke contracts, and the
    three `check_online_room_*` gates to the new truth — new panel name, new
    group names, preview env armed — **without lowering any assertion**.
    Recalibrated palette and draw-bounds expectations carry before/after
    evidence in the commit that changes them. `a11y_race` is untouched.

    Two constraints the audit surfaced and this design works *around* rather
    than through:

    * `check_launcher_tabs` drives `MDKR_APP_SMOKE_NAV_TARGET=1`, which pins
      **Online Room to navigation index 1**. Hiding the panel therefore makes
      this a **seventh** gate that must arm `MDKR_ONLINE_ROOM_PREVIEW=1` —
      `96fceab` armed six and did not cover this one.
    * The `app_schema` CTest pins the entire frame-limit help paragraph
      verbatim, including the option-group name "Higher refresh rates" and the
      label "Original (recommended)"; and `test_product_claim_boundaries.py`
      pins three sentences in `ui_settings.cpp`, one of which is the *Picture*
      group's subtitle. Those strings are carried into the new **Display**
      group unchanged. Regrouping must not become an excuse to reword text
      three CI contracts quote byte-for-byte.

    New copy is also written against `tests/check_player_prose.py`, which scans
    string literals under `platform/app/` for banned vocabulary.

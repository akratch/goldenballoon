# Sprint plans — closing the native-port feature gap

Nine scoped sprints, written 2026-08-09 against `main` at v1.1.0+15. Each one is
independently executable and produces working, shippable software on its own.

These plans exist because a survey of the Harbour Masters port family (Ship of
Harkinian, 2 Ship 2 Harkinian, Starship, SpaghettiKart, Ghostship, Lighthouse)
found that this project leads on verified correctness and trails on **product
surface** and **ecosystem**. The sprints close the second gap without spending
the first.

## What the comparison actually found

| Axis | Harbour Masters | Golden Balloon at v1.1.0 |
|---|---|---|
| Content archive + `mods/` folder | `.o2r`/`.otr`, in-app extractor, load order | none — ROM parsed at runtime each launch |
| Texture packs / custom audio | documented formats, community packs | `Video.TexturePack` is a documented inert stub |
| Enhancement + cheat menu | ~30 categories, save states | none; presentation settings only |
| In-app dev tools | console, freecam, collision/actor/DL viewers, crash screen | F10 FPS readout, diagnostics log |
| Platforms | Win/Linux/macOS (Intel + ARM), Switch, Wii U | Win x64, macOS arm64, Linux x86-64, browser |
| ROM revisions | several per port + hosted hash checker | US 1.1 + EU 1.1; others refused by name |
| Nightly builds | every platform, every port | tagged releases only |
| Docs/community site | per-game sites + Doxygen modding guides | README |
| Screen reader / TTS | speech synthesizer on Windows and macOS | not supported |
| **Differential verification vs. emulator** | **none published** | **oracle replay vs. ares, 234 test files, provenance sidecars** |
| **Browser build** | **none** | **WebGPU wasm, same source** |

The last two rows are why this list is a feature backlog and not a rescue plan.

## Sprints

| # | Sprint | Size | Depends on |
|---|---|---|---|
| [S1](S1-content-pipeline.md) | Content pipeline: archive, extractor, `mods/` loader, texture packs, custom audio | XL | — |
| [S2](S2-enhancements-menu.md) | Enhancements and quality-of-life menu, plus save states | L | — |
| [S3](S3-in-app-dev-tools.md) | In-app developer tools: console, freecam, viewers, crash screen | M | — |
| [S4](S4-platform-breadth.md) | Platform breadth: macOS universal, Windows adapter policy, Linux acceptance | M | — |
| [S5](S5-rom-region-breadth.md) | ROM and region breadth: sub-entry bounds, JP build, 1.0 revisions, hash checker | L | — |
| [S6](S6-release-engineering.md) | Release engineering: green hosted CI, nightlies, Windows signing, update check | M | — |
| [S7](S7-docs-and-community.md) | Documentation site, modding guides, contribution funnel | M | S1 |
| [S8](S8-accessibility.md) | Accessibility: screen-reader and speech output | M | — |
| [S9](S9-campaign-residuals.md) | Campaign residuals: rematch door, trophy chaining, credits tail | S | — |

## Execution status

Landed on `worktree-hm-parity-sprints`, each with a CTest gate and each
mutation-checked — the implementation was deliberately broken and the suite
confirmed to fail — because most of these guard a silent failure rather than a
crash.

| Task | Module | Gate |
|---|---|---|
| S1 T1 | `mod_manifest` | `mod_manifest` |
| S1 T2 | `mod_registry` | `mod_registry` |
| S1 T3 | `mod_texture_key` | `mod_texture_key` |
| S1 T4 | `mod_texture_store` + the `dkr_bind_tile()` hook, stb_image vendored | inertness: `check_texture_lineswap`, `check_determinism`, `check_race_drive` unchanged |
| S1 T5 | startup wiring + `Tab` toggle | positive control: pack-on and Tab-off frame hashes |
| S1 T6 | `mod_source` zip containers, miniz vendored; registry/store retargeted, one validator | `mod_source_zip`, `check_mod_music_override.py` arm C |
| S1 T8 | custom music, dr_wav vendored | `check_mod_music_override.py` |
| S1 T7 | Settings → Content, with the skip list | `app_ui_policy` |
| S1 T9 | `tools/mod_texture_dump.py`, stb_image_write vendored | round-trip: dumped digest overrides |
| S1 T10 | clean-room section 8, `mods/` ignored | `check_clean_room.sh` |
| S1 T11 | `docs/MODDING.md` + README section | `check_markdown_links.py` |
| S2 T1 | `enhancement_registry` | `enhancement_registry` |
| S2 T2 | the generated authority gate | `check_enhancement_authority.py` |
| S2 T3 | speedometer | `check_enh_speedometer.py` |
| S2 T4 | draw distance + model detail | `check_enh_draw_distance.py` |
| S2 T6 | `save_state` container | `save_state_container` |
| S2 T8 | Enhancements section + scoped reset | `app_ui_policy` |
| S3 T1 | dev-tools host, six slots, purity gate | `check_dev_tools_purity.py` |
| S3 T2 | crash report | `check_crash_screen.py` |
| S3 T3 | `dev_command` | `dev_command` |
| S3 T4 | console + diagnostics windows | `check_dev_tools_purity.py` (2 live callbacks) |
| S3 T5 | free camera | `check_tool_freecam.py` |
| S3 T6 | collision + object viewers | `check_dev_tools_purity.py` (6 of 6) |
| S3 T7 | performance window | `check_dev_tools_purity.py` (6 of 6) |
| S4 T1 | `gpu_diagnostics` | `gpu_diagnostics` |
| S4 T3 | `adapter_policy` | `adapter_policy` |
| S4 T5 | controller hotplug, virtual-device seam | `check_input_hotplug.py` (8 assertions) |
| S5 T1 | `sweep_subentry_access.py` | exits 1 on any unchecked site |
| S5 T2 | `mdkr_asset_subentry()`, `get_misc_asset` aborts | `asset_subentry`, `check_subentry_bounds.py` |
| S5 T3 | hosted ROM checker page | `check_rom_checker_page.py` |
| S5 T4 | exhaustive game-text index census | `check_rom_text_indices.py` |
| S6 T1 | hosted CI status corrected | run `31248954626` |
| S6 T4 | `update_check` | `update_check` |
| S8 T1 | `a11y_model` | `a11y_model` |
| S8 T2 | shell self-voicing | `check_a11y_shell.py` |
| S8 T3 | speech backends, one per platform | `check_a11y_shell.py` (`[SPEAK]` route) |
| S8 T4 | race announcements | `check_a11y_race.py` (3 arms) |
| S8 T5 | Accessibility section, rows enumerated from policy | `app_ui_policy`, `check_a11y_shell.py` |
| S9 T1 | roadmap reconciled | — |
| S9 T2 | `tests/route_plan.py` waypoint follower | its own self-test, 8 assertions |
| S9 T3 | lobby rematch door, driven | `check_campaign_progression.py` driven arm |
| S9 T4 | trophy + T.T.-amulet chaining, Future Fun Land | `check_campaign_progression.py`, `check_future_fun_land.py` |
| S9 T5 | credits screen from a won Wizpig 2 | `check_campaign_progression.py` seam E |

### Six defects the execution found in its own design

These are recorded because the plans were wrong, not merely incomplete.

1. **S1 T3's padding test was vacuous as written.** Zero-init and compound-literal
   both leave padding zeroed, so the test passed against an implementation that
   hashed raw struct bytes. Replaced with a `0xaa`-filled union plus a `memcmp`
   control proving the representations differ.
2. **S8 T1's CRITICAL rule was undecidable** — the header had no way to mark an
   utterance finished. And the first test of it was confounded: a critical race
   event and a focus change differ in category as well as priority, so deleting
   the exemption still passed. `mdkr_a11y_finish()` exists now, and each critical
   case is paired with an identical normal one.
3. **S1 T4's renderer hook was written from inference.** Rewritten against
   `dkr_bind_tile()` with the real key initialiser, the real miss-path call, and
   which bytes to hash. Implementation then found a hazard the plan had not
   considered: `source_size_bytes` can name more bytes than `addr` owns at the
   arena edge, so hashing it unclamped would read off the end of the arena for
   the sake of computing a name. Both `dkr_src_hash()` and the upload path
   already clamp with `dkr_arena_room()` for exactly that reason.
4. **S9 T2's oscillation detector counted the wrong thing.** The first version
   counted frames spent in a grid cell, so a genuine approach crossing one
   48-unit cell at 1 unit/frame read as stuck. It counts cell *entries* now —
   leaving and returning — which is what reverse-and-retry does and what a
   steady traversal never does. Its own self-test caught it on the very first
   assertion.
5. **S8 T5's Accessibility section broke three unrelated gates, and none of them
   said so.** It shipped `DefaultOpen` unconditionally and drawn above the
   settings category loop, which pushed Frame limit roughly 660pt below an 800pt
   panel and the UI-scale slider below a 700pt one. The scripted gates drive
   real widgets at coordinates captured on the previous frame; a widget scrolled
   past the bottom is *still submitted*, so the rect stays valid, the queued
   click lands on clipped geometry, and the failure surfaces as
   `frame-limit requested=240 actual=original` — a persistence verdict, for a
   layout fault. The plan named the target section for each new row and never
   said that adding a section is a change to every coordinate below it. Sections
   above the loop now collapse for whichever gate is armed, exempting the one
   holding that gate's own target, and both widgets report being off-screen by
   name so the next occurrence reads in one line.
6. **S8 T3's plan specified a threading discipline the code does not have.** It
   told the implementer that the worker pops while the UI pushes. `a11y_model.c`
   writes `s_head` in the push drop-oldest arm as well as in the pop, so it is
   not SPSC-safe and that split would have been a data race. The agent refused
   the instruction with the correct reason and pumps on the pushing thread into
   a mutex-guarded ring instead. An earlier commit message of mine asserted the
   SPSC discipline as fact; it was wrong.

### A deliberate duplication, with the extraction already designed

`platform/app/tool_diagnostics.cpp` and `platform/app/crash_screen.cpp` now
duplicate five things: the `extern "C"` block reaching `level_id()`/
`level_name()` and its explanation of why the app shell cannot include
`game/src/game.h`, the `g_simTickCounter > 0` guard before trusting the map id,
and three player-visible sentinels the crash-screen gate has now pinned
(`"not in a level yet"`, `"not written on this platform"`, and the version line).

It is duplicated **on purpose**, and a straight extraction would be wrong:
`tool_diagnostics.cpp` returns `std::string`, while the crash screen runs inside
a signal handler and must not allocate. The shared layer therefore has to be the
allocation-free `const char *` form — an `app_state_facts.h` exposing
`AppState_trackId()`, `AppState_trackName(int)`, `AppState_logPath()`,
`AppState_versionLine()` — with the diagnostics window wrapping *those* in
`std::string`, not the reverse.

Two callers is arguable; three is not. Fold it in when a third appears.

### Two roadmap claims that were already stale

- **Hosted CI had run green** — run `31248954626` on `main` at `080c4c4`,
  all six jobs, 2026-08-08. S6's M1 was already met. What is actually missing is
  that `main` does not *require* those jobs.
- **The campaign was gated and ghosts were 46/47**, not ungated and 1/47. See
  S9 T1.

### Where S1 actually stands

**A directory pack works end to end.** Drop `mods/<name>/pack.ini` plus
`textures/<digest>.png` next to the executable, launch, and the pack's textures
replace the ROM's; `Tab` switches them off and back on mid-race. Verified by
frame hash, including that the Tab-off frame is byte-identical to the no-pack
baseline — which is what proves `override_generation` re-uploads the ROM texels
rather than leaving packed pixels bound.

**Authoring works end to end.** `tools/mod_texture_dump.py` writes every bound
texture as `<digest>.png` — the exact filename a pack needs — and the round trip
is proven, not assumed: digest `794c3b7a424fbe63824fac04e5816fce` dumped from a
race, replaced with solid magenta, dropped into a pack, and the frame at 3800
changes (`e3852944…` → `040eeef5…`) while the frame at 3700, before that
texture's first bind, stays byte-identical. A confirming run re-dumped the
digest with the pack installed and read back pure `(255,0,255,255)`, so it is
the pack's bytes reaching `upload_texture`, not the ROM's.

**What S1 owed, and no longer does.** This list had three entries; all three
are now closed and the entries are struck rather than deleted, because which of
them turned out to be real is part of what this record is for.

1. ~~Zip packs are readable but not discoverable.~~ **Closed.** `mod_registry`
   and `mod_texture_store` are retargeted onto `mod_source` (T6 step 5);
   `name_is_zip()` / `open_pack_root()` in `platform/mod_registry.c` discover
   `.zip` packs, and a file merely *named* `.zip` fails at open rather than
   being skipped silently.
2. ~~`Content.PacksEnabled` is honoured at init only.~~ **Closed.**
   `mdkr_video_config_publish()` now carries the key to
   `mdkr_mod_texture_set_enabled()` and to a matching switch in
   `platform/mod_music.c`, so the Settings checkbox and `Tab` move the same
   lever at the same speed. The open question resolved in the easy direction:
   `platform_content_packs_init()` scans `mods/` and binds the texture store
   **unconditionally** (`platform/platform_sdl_min.c` — the scan at line 1407
   and the store bind at 1438 are both above the one place the setting is read,
   line 1439), so the setting only ever decided whether the store answers.
   off→on therefore needed no rescan and no restart. Gated by
   `check_mod_texture_override.py`, whose central assertion is that the frame
   after switching off is *byte-identical* to a run with no pack installed.

   Two things are deliberately not live, and both say so where a player reads
   them: installing or removing a pack (that is the scan, not the switch), and
   a replacement music track that is already playing — the sequence player's
   mute is a one-way redirection, so cutting the track off mid-play would leave
   silence instead of the game's own music. The music switch is therefore read
   at `mdkr_mod_music_begin()` and applies from the next track.
3. ~~Custom music (T8) is untouched.~~ **Closed.** `platform/mod_music.c`, gated
   by `check_mod_music_override.py`.

One consequence worth recording: override uploads go through
`gfx_rapi->upload_texture`, which is level 0 only. A pack texture replacing a
mipmapped ROM texture gets no mip chain even though its cache key says
`mipmaps = true`. That is what the plan specified, and it will read as aliasing
at distance once packs exist.

### Verification state

**The web build was rebuilt and links.** `STB_SOURCES` is added to the target
unconditionally and every `platform/mod_*.c` is in `PLATFORM_SOURCES`, so the
wasm target compiles all of it. `tools/web/build_web.sh` completes with exit 0
and stages `mdkr64_web.wasm`, `mdkr64_web.js` and the rest of `dist/web`.

Still not run: the `browser` and `browser_save` roles, which drive a real
Chromium profile. Nothing suggests they would fail — the mod layer touches no
platform API beyond `fs_utf8`, and the browser has no writable `mods/`, so the
store should simply stay inactive — but that is an inference, not an
observation.

```bash
MDKR_AUDIO=0 python3 tools/run_checks.py --role browser,browser_save
```

**The adopted-pacing gate has since been run with a display, and passes.**
`check_app_adopted_pacing` reported `presented=0 unavailable=1424` when this
section was first written — the FPS-only WebGPU overlay arm never got a
drawable, because that shell had no window-server session, which
[`../DEFINITION_OF_DONE.md`](../DEFINITION_OF_DONE.md) records as a requirement
of the arm rather than a defect. Re-run from a session that has one, it passes
both the WebGPU-default and explicit-GL launcher handoffs at numeric and
uncapped rates with fully drained GPU work. The earlier note is kept because
"fails without a display" is a property of the gate worth knowing before anyone
reads a red result as a regression.

**Suite coverage actually run** (in slices, because the runner's wall time
exceeds the tool's per-command ceiling): `state_hash`, `determinism`,
`race_drive`, `race_finish_time`, `race_2p_split`, `texture_lineswap`,
`mip_motion`, `texture_edge_classification`, `sprite_layout`,
`rdp_interpolation`, `font_sdf`, `collision_gridmask`, `collision_untextured`,
`door_blocks`, `boss_win_verdict`, `adventure_hub`, `array_bounds_sweep`,
`runtime_safety`, `native_ui_resolution`, `save_failsafe`, `rom_revision`,
`asset_swap_invariants`, `audio_output`, `raw16_audio`, `audio_options_persistence`,
`camera_obstruction_runtime`, `simulation_cadence`, `math_rotpy`, `math_tables`,
`restart_apply`, `shell_dropfile`, `overlay_pause`, `overlay_pause_cutscene`,
`app_capture`, `app_ui_input`, `ci_contract`, `track_sweep`, `vehicle_sweep`,
`world_shadows`, `world_fx_capture`, `shadow_stage_reset`, plus `ctest` at
134/134. **41 checks, all pass.**

That is still short of the manifest's 146 tasks. Not run: the ghost, trophy and
campaign lanes, the WebGPU content census and fault matrix, the sanitizer and
alignment configurations, the oracle routes, and everything browser-side. The
list above says what was covered so that breadth is not inferred from it —
`track_sweep` (all levels) and `vehicle_sweep` (47 combinations) are the two
that most directly exercise the asset path the override layer sits in front of.

### The complete suite has now run, and it found one thing — 2026-08-10

`run_checks.py` over the whole manifest at `081fd32`, against the canonical
artifacts (`build-rel` Release, `build-asan` with
`-fsanitize=address -g -O1 -fno-omit-frame-pointer`, and the wasm build):
**162 tasks, 192m43s, 3 failures.** One was real.

**A caveat that applies to everything above this line.** The 41-check pass was
run against `build/`, which in this worktree is `CMAKE_BUILD_TYPE=Debug`. The
suite's canonical build is `build-rel`. That is not a pedantic distinction here:
[`../RELEASE_CHECKLIST.md`](../RELEASE_CHECKLIST.md) §2 records two
player-reported bugs that the optimiser created and that were invisible at
`-O0`, because a progress flag written as `FLAG << (i + 31)` is UB, folds to
zero at `-O2`, and clang then deletes the load, the test and the store as one
dead unit — so both halves of a "show this once" latch disappear while every
native check stays green. Debug-only verification of latch-shaped code (a11y
state machines, settings persistence) is therefore weaker evidence than it
looks, not merely differently configured.

**Real: `array_bounds_sweep`, two untriaged bare-pointer sites in
`platform/mod_music.c`** — `resample_into:out` and `mdkr_mod_music_mix:out`.
Both turned out to be genuinely bounded and both now carry a triage entry
answering the three questions the class asks. The second was worth the walk:
`frames` alone does not state the capacity, because the write needs
`frames * s_channels` elements. It holds because both call sites pass the buffer
`amAudioSynthFrame(n)` returned with the same `n`, that buffer is
`n * DKR_AUDIO_CHANNELS` samples, and `s_channels` has exactly one writer,
called once, with `DKR_AUDIO_CHANNELS`. A second initialiser with a different
channel count is the one change that would break it.

This is the argument for the complete suite in one line: `array_bounds_sweep` is
a `rom`-role check, so hosted CI cannot run it and no subset battery reaches it.
The sites had been in the tree since the custom-music work and nothing had ever
looked at them.

**Not real: `app_adopted_pacing`** — `presented=0 unavailable=1478`, the
documented no-window-server signature. Proven environmental rather than argued:
the *same Debug binary* that passed this check two hours earlier failed it
identically once the machine went idle and the display slept. Binary unchanged,
result changed.

**Not real: `shell_dropfile`** — "final Play arm emitted 0 Play actions, want 1",
in a task that runs immediately after the suite's own `ctest -j` batch at peak
load. Passes standalone 3 of 3. Recorded rather than dismissed, because the arm
is an *asynchronous* Play recheck and so is genuinely timing-sensitive; a gate
that can lose a race under load is worth knowing about even when the code is
right.

The suite has not been re-run end to end since the triage entries landed. That
change is confined to a dict of strings in `tests/check_array_bounds_sweep.py`,
nothing else reads it, and that check passes on re-run — but the honest
statement is 159/162 plus a targeted re-verification, not a green suite.

### One task stopped at its own gate, which is a result

**S2 T7 (save-state capture) is not built**, and the reason is measured rather
than estimated. Its plan carried a stop-or-go step: prove a raw arena snapshot
is restorable before writing the feature.

The pointer-token half of the argument **held** — segment A came back
byte-identical across six different ASLR arena bases, with no fix-ups. The half
that failed was payload *scope*. This plan claimed `segment_consts.c` plus the
`[SIMHASH]` v3 field list were "the existing inventory of authoritative state";
they yield about 20 globals. Measured by diffing the process image's writable
sections over a 600-tick window, the sufficient payload is the arena plus
**1,686 externally-linked globals, ~809 KB**. Arena-only does not diverge, it
segfaults — a host global outside the arena still describing the post-restore
world.

Only two payload definitions are both complete and implementable, and both are
new invariants the project has not paid for: 1,686 hand-declared untyped decomp
externs, or a contiguous region whose bounds are a link-order artifact of one
toolchain. A list fitted to the 142 symbols one route touched would pass a
three-track gate and corrupt on the first unmeasured route.

Recorded in [`../open-items/save.md`](../open-items/save.md) with the unblocking
path. Stopping here was the instruction and the correct call; the container from
T6 remains shipped and gated.

### The campaign residuals are closed, and the last one was a fixture defect

S9's three residuals are all driven now. The third is worth recording in full,
because the recorded obstacle turned out to describe a symptom rather than a
cause.

The note said the cutscene stack held at `ASSET_LEVEL_WIZPIG2ANIM` for 25,000
frames with A tapped every 200. The first measurement answered why:
**`func_8006C300()` is never non-zero** — zero on all 29,929 sampled frames, and
structurally so, because `game_load_level` zeroes its backing global on every
load and the only writer is the redirect branch for a *repeat* Wizpig entry. The
A-press arm was unreachable. The 113 presses could never have popped anything;
the tapping cadence was never the variable.

Following that down: all 159 of the level's animation objects sat deactivated
because `gCutsceneID` was 5 rather than the channel the level pushed. Every
Future Fun Land header is `RACETYPE_HUBWORLD`, so the world-arrival branch ran
on a cutscene level and overwrote the channel — because the fixture did not
carry the "arrived in Future Fun Land" flag.

**That makes it a fixture defect, not a game defect**, and the reasoning is what
matters: a player cannot reach Wizpig 2 without entering the Future Fun Land
hub, and entering it with the balloons and amulet the door demands is exactly
what latches that flag. A fixture without it is a state the campaign cannot
produce. The fix is in the fixture; no `game/` behaviour changed and nothing was
skipped. The two `game/` additions are read-only `NATIVE_PORT` traces, because
`menu_credits_init` writes only display strings, a music sequence and a scroll
table — none of which a headless run can see, so there was nothing to assert on.

### A gate that was already failing on `main`

`check_array_bounds_sweep` failed when this work first ran it, and only one of
the two reasons was this work's fault.

- **Four new bare-pointer sites needed triage** — `mod_registry`, `mod_source`,
  `gpu_diagnostics`, `adapter_policy`. All four are genuinely bounded by the
  size parameter travelling with the buffer, and each now carries an entry
  saying *how*, in the terms the class asks for: what sets the count, what the
  capacity is, and whether the counter can pass the test without equalling it.
- **The shift-count ceiling was already breached before this branch existed.**
  Measured on `origin/main` at `080c4c4`, the informational population is
  **260** against a recorded ceiling of **257**. This work added exactly one
  more (the digest's little-endian field encoder), taking it to 261.

The second point is the one worth keeping. `array_bounds_sweep` is a `rom`-role
check: hosted CI cannot run it, because no ROM may exist on a runner. So it is
visible only to a local full-suite run — and a subset run does not see it
either. A gate silently red on `main` for an unknown number of commits is
exactly the failure mode that argues for running the complete suite per release
rather than the batteries that look relevant.

The three pre-existing findings are **not** identified. Doing so means bisecting
the class population across every commit since the ceiling was last set. That is
worth doing and is not this change; the ceiling comment says so rather than
letting 261 read as one coherent batch.

### The defect the sweep surfaced — now partly fixed

`get_misc_asset()` compared its index against the table length and then returned
the **section base silently** when it failed, turning an out-of-range read into
a confidently wrong one. That is fixed: it aborts, naming the section, index and
count, and the abort is proven reachable against the real loaded section rather
than only in a unit test.

Fixing it turned up **four more bugs the sweep had not named**, two of them
live:

- `emitter_init()` does not guard `particleID` at all. This index previously
  said "both its callers guard" — measurement corrected it. One caller clamps,
  the other passes straight through to a dereference.
- `particles_init()` leaves slot `[count]` of its pointer array uninitialised,
  and `mempool_alloc_safe` does not zero. Reachable through the gap above, it
  dereferenced uninitialised memory as a `ParticleDescriptor *`. It mirrors the
  N64 shape, so not a port regression — but silent and live.
- `obj_id_valid()` indexes with the unvalidated id *before* testing it: a
  validator that reads out of range first.
- `get_misc_asset_size()`'s old guard read as if it protected the `[index + 1]`
  access but was arithmetically identical to `index >= length`.

Sweep 276 → 273, CHECKED 129 → 133. **Still open**, and the reasons are
structural rather than effort: 38 compile-time indices are unbounded *by
construction* — that is the v80-constant-on-a-v77-ROM shape itself, which S5
task 4's enumeration answers and no per-site guard can — 20 terminator-walks are
bounded by a byte pattern rather than a count, and 81 sites remain unprovable.

One cheap site is deliberately left unguarded: `game_ui.c:4774`. Its input
domain spans every HUD sprite id across battle, challenge, boss and adventure
levels, and the regression routes do not reach them all. An abort added there
without enumerating that domain risks firing on a supported ROM, which is the
one outcome this work must not produce.

## Recommended order

1. **S6 — release engineering.** Hosted CI has never run green, so every claim in
   this repository currently rests on local runs. Nightlies are also how any of
   the work below gets playtested by anyone other than its author. Everything
   downstream borrows credibility from this sprint.
2. **S9 — campaign residuals.** Three named leftovers behind an already-green
   `check_campaign_progression`. Cheap, and it retires the last "is the game
   actually finishable" question.
3. **S1 — content pipeline.** The single largest structural gap, and the one that
   unblocks S7 and most community activity. Design the ROM-absence guard
   extension *first*; the guard is the reason this project can exist.
4. **S2 — enhancements menu.** The most visible player-facing gap once S1 lands.
5. **S3 — in-app dev tools.** Cheap relative to impact; also what makes external
   bug reports and contributions tractable.
6. **S4 — platform breadth**, then **S5 — region breadth**, then **S7 — docs**,
   then **S8 — accessibility**.

Switch and Wii U ports are deliberately **not** planned. That is
libultraship-shaped work with a large permanent maintenance surface, and it buys
less than S1–S3.

## A correction this plan set is built on

`ROADMAP.md` still says campaign completeness is "the largest single piece of
deferred work" and that ghost coverage is one (track, vehicle) pair of 47. Both
statements predate the "definitionally done" campaign.
[`../DEFINITION_OF_DONE.md`](../DEFINITION_OF_DONE.md) (2026-08-07) is the
current record: `check_campaign_progression` gates silver coins, all four boss
rematches, both Wizpig races and the true-ending bit, and `check_ghost_matrix`
covers 46 of 47 pairs with the 47th asserted as a non-producer. S9 scopes only
the three residuals that genuinely remain. Reconciling `ROADMAP.md` with the
closure ledger is [S9 Task 1](S9-campaign-residuals.md#task-1-reconcile-the-roadmap-with-the-closure-ledger).

## Rules every sprint inherits

These are not restated in full in each plan. Read
[`../../CONTRIBUTING.md`](../../CONTRIBUTING.md) before executing any of them.

1. **Audio safety is absolute.** Every game or test invocation passes
   `--headless-frames N` and sets `MDKR_AUDIO=0`. `MDKR_AUDIO=off` is a no-op —
   only the digit `0` disables.
2. **No ROM-derived data, ever.** No textures, audio, models, level data, saves,
   oracle captures or audio dumps in a commit. `tools/check_no_rom.sh` and
   `tools/check_clean_room.sh` fail closed.
3. **Positive controls are mandatory.** A check that passes both with and
   without the change is not a check. Revert only the fix, rebuild, watch it
   fail, and paste that output.
4. **Root cause before fix**, and **fix the instance, then sweep the class**
   with a mechanical instrument.
5. **Game-code changes go behind `#ifdef NATIVE_PORT`**, with a
   `_Static_assert` wherever a struct layout or offset assumption is involved.
6. **Never claim a pass you did not run.** Paste the command and its output.
7. **Registration is not optional.** A new `tests/check_*.py` must appear in
   `tools/run_checks.py`'s `CHECKS` tuple or `validate_manifest()` fails the
   suite. A new `tests/test_*.c` must get `add_executable` + `add_test` in
   `cmake/tests.cmake`.
8. **Public-surface hygiene.** Run `python3 tools/check_public_surface.py
   --staged` before committing. No personal filesystem paths, no tool
   transcripts, no assistant attribution trailers in commit messages — the
   pre-push hook rejects them.
9. **Player-facing text is for players.** Release notes and in-app copy carry no
   process, gate, or validation vocabulary.

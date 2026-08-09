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
| S1 T6 | `mod_source` zip containers, miniz vendored | `mod_source_zip` |
| S1 T10 | clean-room section 8, `mods/` ignored | `check_clean_room.sh` |
| S2 T1 | `enhancement_registry` | `enhancement_registry` |
| S2 T6 | `save_state` container | `save_state_container` |
| S3 T3 | `dev_command` | `dev_command` |
| S4 T1 | `gpu_diagnostics` | `gpu_diagnostics` |
| S4 T3 | `adapter_policy` | `adapter_policy` |
| S5 T1 | `sweep_subentry_access.py` | exits 1 on any unchecked site |
| S6 T1 | hosted CI status corrected | run `31248954626` |
| S6 T4 | `update_check` | `update_check` |
| S8 T1 | `a11y_model` | `a11y_model` |
| S9 T1 | roadmap reconciled | — |
| S9 T2 | `tests/route_plan.py` waypoint follower | its own self-test, 8 assertions |

### Four defects the execution found in its own design

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

**Four things S1 still owes**, none of them hidden:

1. **Zip packs are readable but not discoverable.** `mod_source` handles both
   kinds, but `mod_registry` and `mod_texture_store` have not been retargeted
   onto it (T6 step 5), so the registry still only finds directories.
2. **`Content.PacksEnabled` is honoured at init only.** The key is declared
   `SCOPE_LIVE` but nothing publishes it, so the Settings checkbox takes effect
   next launch while `Tab` flips the same lever immediately. Closing it means a
   live-apply hook in the video-config publish path.
3. **No authoring tool** (T9) and **no Settings → Content list** (T7), so an
   author has no supported way to learn a digest and a player has no way to see
   which packs loaded except the `[MODS]` log lines.
4. **Custom music** (T8) and **`docs/MODDING.md`** (T11) are untouched.

One consequence worth recording: override uploads go through
`gfx_rapi->upload_texture`, which is level 0 only. A pack texture replacing a
mipmapped ROM texture gets no mip chain even though its cache key says
`mipmaps = true`. That is what the plan specified, and it will read as aliasing
at distance once packs exist.

### One verification gap in this work

`STB_SOURCES` is added to the target unconditionally, so the wasm build compiles
`stb_image` too, and `platform/mod_*.c` are all in `PLATFORM_SOURCES` for every
platform. **The web build has not been rebuilt since.** The native and ROM
suite roles pass; the `wasm`, `browser` and `browser_save` roles were not run,
and neither was `tools/web/build_web.sh`. Nothing here is expected to break
under emscripten — stb_image is ordinary portable C and the mod layer touches
no platform API beyond `fs_utf8` — but *expected not to break* is not a
measurement, and the browser build is a shipped artifact.

Run before this branch is merged:

```bash
tools/web/build_web.sh
MDKR_AUDIO=0 python3 tools/run_checks.py --role wasm,browser,browser_save
```

There is also a live question the wasm build will answer: the pack directory
resolves relative to the working directory on non-packaged builds, and the
browser has no writable `mods/`. The store should simply stay inactive there,
which is the correct behaviour, but it is currently an inference rather than an
observation.

### One defect surfaced, not yet fixed

`tools/sweep_subentry_access.py` reports 276 sub-entry sites: 129 checked, 65
unchecked, 82 unknown. Beyond the count, three concrete findings:
`particles.c:739,757,758` (guards live in both callers, not in
`emitter_init_with_pos` itself), `objects.c:1474,4382` (no comparison at all),
and — the one that matters most — **all 75 `get_misc_asset()` sites read as
checked while the accessor returns the section base *silently* on an
out-of-range index**. A bounds check whose failure mode is plausible wrong data
is barely better than none. That is what S5 T2's `mdkr_asset_subentry()` has to
replace.

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

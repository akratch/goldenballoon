# Contributing

Read [docs/DEVELOPER_HANDBOOK.md](docs/DEVELOPER_HANDBOOK.md) first — in
particular §3, the recurring bug shapes behind nearly every hard defect in this
port. Reading it first saves days.

One of those shapes deserves calling out here because it is invisible to upstream:
**anything under `game/src/hasm/` is a C transcription of hand-written assembly
that only this port compiles.** Upstream's matching build links the real `.s`, so
its tests can never catch an error in that C. Three defects have been found there
so far. Before trusting such a function, diff it against the `.s` next to it — the
ground truth is in the tree. See the "gridmask" and "hasmaudit" waves in
[docs/open-items/](docs/open-items/README.md).

Then [docs/README.md](docs/README.md) is the map of everything else.

## Public repository boundary

This repository keeps normal public Git history. Do not commit temporary plans,
handoffs, tool transcripts, personal filesystem paths, credentials, or private
working directories. Put durable engineering knowledge in an architecture
document, the open-items register, a focused code comment, or a regression test.

Use a GitHub `noreply` address for both author and committer identity. Install
the repository hooks once with `tools/install_git_hooks.sh`; CI and pre-push run
the same fail-closed surface scan. You can run it directly before committing:

```bash
python3 tools/check_public_surface.py --staged
```

## Audio safety — hard rule

**Always pass `--headless-frames N` for game or test runs.** It returns before
the SDL audio device is opened. The recognized `--help`/`-h` path is now safe and
returns before ROM, window, or audio initialization; an unrecognized flag still
falls through to an ordinary interactive launch, so do not use exploratory
arguments as a substitute for headless mode.

Also set `MDKR_AUDIO=0`.

```bash
MDKR_AUDIO=0 ./build/mdkr64 --headless-frames 600 --rom baserom.us.v80.z64
```

> `MDKR_AUDIO=off` is a **no-op**. The check is `disable[0] == '0'`, so only the
> digit `0` disables. `off`, `false` and `no` all leave audio enabled.

This is not a style preference. It is the rule that stops an automated run from
blasting audio through someone's headphones.

## Build

Bring your own ROM — one you legally own and dumped yourself. It is never committed;
`*.z64`, `*.n64` and `*.v64` are all git-ignored, as are ROM archives (`*.zip` and
friends — a ROM handed over as a zip is still a ROM).

**Supported revisions: US 1.1 (`us.v80`) and European 1.1 (`pal.v80`).** Any of the
three byte orders is fine — `.v64`/`.n64` are converted to `.z64` on load. The other
three revisions are identified by name and refused; see
[docs/ROM_REVISIONS.md](docs/ROM_REVISIONS.md) for what each one does and what
supporting it would take.

```bash
ln -s /path/to/your/baserom.us.v80.z64 baserom.us.v80.z64
cmake -S . -B build && cmake --build build -j8
```

`MDKR_RENDERER=webgpu|gl` selects the backend — WebGPU is the native default and
OpenGL remains available explicitly for diagnostics and parity work. The
browser build is WebGPU-only; see
[docs/architecture/web.md](docs/architecture/web.md).

## Tests

[tests/README.md](tests/README.md) is authoritative: it lists every check, its frame
budget, and its expected terminal state. Everything is headless and reproducible.

```bash
MDKR_AUDIO=0 python3 tests/check_race_drive.py         # after ANY gameplay change
MDKR_AUDIO=0 python3 tests/check_determinism.py        # after ANY port-layer change
MDKR_AUDIO=0 python3 tests/check_texture_lineswap.py   # after ANY texture-decode change
MDKR_AUDIO=0 python3 tests/check_track_sweep.py        # after ANY asset/race change
MDKR_AUDIO=0 python3 tests/check_collision_gridmask.py # after ANY collision or boss-flow change
MDKR_AUDIO=0 python3 tests/check_door_blocks.py        # after ANY object-model collision or door/exit change
MDKR_AUDIO=0 python3 tests/check_boss_win_verdict.py   # after ANY courseFlagsPtr / settings->bosses write
MDKR_AUDIO=0 python3 tests/check_math_rotpy.py          # after ANY change under game/src/hasm*
MDKR_AUDIO=0 python3 tests/check_math_tables.py         #   ""
MDKR_AUDIO=0 python3 tests/check_collision_untextured.py #  ""
MDKR_AUDIO=0 python3 tests/check_race_finish_time.py
MDKR_AUDIO=0 python3 tests/check_race_2p_split.py
MDKR_AUDIO=0 python3 tests/check_native_ui_resolution.py # after HUD/text/target/order changes
MDKR_AUDIO=0 python3 tests/check_adventure_hub.py
MDKR_AUDIO=0 python3 tests/check_rom_revision.py       # after ANY ROM-loader change
MDKR_AUDIO=0 python3 tests/check_save_failsafe.py      # after ANY save/EEPROM change
MDKR_AUDIO=0 python3 tests/check_array_bounds_sweep.py # after ANY game/src change
MDKR_AUDIO=0 python3 tests/check_audio_output.py       # after ANY audio/mixer change
MDKR_AUDIO=0 python3 tests/check_raw16_audio.py        # after ANY RAW16/endian/bank change
                                                       #   (it opens NO device — see
                                                       #    tests/README.md)
```

`MDKR_RAW16=legacy` is the exact pre-fix byte-order arm used by the focused
test. Do not use it as an end-user compatibility setting or as evidence that
audio works; production/default behavior is `fixed`.

Plus the nine `nav_*` menu fixtures — run them on a clean EEPROM (`rm -f
save/eeprom.bin`), because a recorded time changes the menu route.

A run fails on a non-zero exit, a `[CRASH]` backtrace, a `[FATAL]` abort, or a
missing assertion line.

Before a release, run [docs/RELEASE_CHECKLIST.md](docs/RELEASE_CHECKLIST.md) instead
— it wraps all of the above plus the clean-room and ROM-absence gates.

### Writing a harness

Do not assert with `printf ... | grep -q` under `set -o pipefail`. `grep -q` closes
the pipe on its first match, the upstream `printf` takes SIGPIPE (141), and
`pipefail` reports the *successful* match as a pipeline failure — inverting every
assertion. Use bash `case "$out" in *"$want"*)`.

## The rules

These apply to every change. They are not new; they are the rules the project has
been operating under, promoted here from `docs/PHASE3_SCOPE.md`.

1. **Root cause before fix.** This codebase punishes guessing. Four of its hardest
   bugs were silent and produced no crash at all. Find the mechanism, then fix it.

2. **Positive controls are mandatory for anything that "doesn't crash".** A fix with
   no failing case proves nothing. Revert only the fix, rebuild, and watch the check
   fail — then paste that. If your change's failure mode is *silence* (wrong pixels,
   a stale cache, a denormal, audio that simply stops), a check that passes both
   with and without your fix is not a check. Add the positive control alongside the
   fix, in the same change.

3. **Game-code changes go behind `#ifdef NATIVE_PORT`.** `game/` is vendored
   decompilation; keep edits minimal and gated so the matching N64 build is
   untouched and upstream merges stay clean. Add a `_Static_assert` wherever a
   struct layout or offset assumption is involved — there are 63 such locks
   already, and they are how LP64 layout drift gets caught at compile time instead
   of as a crash three subsystems away. Syncing with upstream is
   [docs/DECOMP_SYNC.md](docs/DECOMP_SYNC.md).

4. **Never claim a pass you did not run.** Paste the command and its output. A
   retraction is recorded in place, not quietly deleted — see the retractions in
   `docs/OPEN_ITEMS.md` for the expected form.

5. **No ROM-derived data, ever.** No textures, audio, models, level data, save
   files, oracle captures or audio dumps in a commit. `tools/check_clean_room.sh`
   and `tools/check_no_rom.sh` enforce this and fail closed. See
   [DISCLAIMER.md](DISCLAIMER.md).

6. **Fix the instance, then sweep the class.** A single fixed instance is half a
   fix. Before you call a defect closed, answer in writing: *what is the general
   shape of this bug, where else in the tree could that shape occur, and how do I
   find every one of them mechanically?* Then go and look, and report the coverage
   you actually achieved — including "I audited N sites by this method and none of
   the others is reachable", which is a complete and valuable answer.

   This is not a counsel of perfection; it is the highest-yield thing this project
   has done. Every time it has been applied it found more real bugs than the report
   that triggered it:

   | one report | the class swept | what the sweep found |
   |---|---|---|
   | racers fell through one volcano | every C body transcribing hand-written asm (~90 functions) | `vec3f_rotate_py` had pitch and yaw transposed; an untextured collision batch silently skipped |
   | a browser crash in the wave renderer | every adjacent global pair the code indexes across (1001 pairs compared on both targets) | a five-row write into a four-row array that let the game load an **arbitrary level**, wrong on both targets |
   | one texture looked scrambled | every `LOADBLOCK` with `dxt == 0` | ~30 % of all texture uploads, including the minimaps |
   | one truncated pointer crash | every truncate-then-dereference site | a family of intermittent crashes that had looked unrelated |

   Prefer a **mechanical** sweep over reading: a sanitizer, a differential against
   ground truth, a layout comparison, a link-map diff, a count over a data table.
   Reading finds what you already suspect. And when the sweep needs an instrument
   that does not exist yet, building it is part of the fix — that is where
   `tests/check_array_bounds_sweep.py` and `tools/compare_data_layout.py` came from.
   Record the shape itself in [docs/DEVELOPER_HANDBOOK.md](docs/DEVELOPER_HANDBOOK.md) §3 so the next person
   recognises it on sight, and record any instance you leave unfixed in
   [docs/open-items/](docs/open-items/README.md) with its measured reachability — do not
   blind-fix an instance you cannot show is reached.

## Writing it up

Document a defect the way the rest of the project does:

> **mechanism → measured evidence → fix → verification**

New defects and fixes go in [docs/open-items/](docs/open-items/README.md), with the
subsystem TOC updated. Fixed entries stay — a closed write-up is the only warning
the next person gets that the trap exists.

Architecture that outlives a wave goes in
[docs/architecture/](docs/architecture/README.md).

### A note on waves

Work here proceeded in **waves**: small, single-purpose batches, each opened
against one report or one class of defect, and each closed with its own
evidence. A wave carries a short codename — `objcoll`, `lineswap`,
`shadowplay`, `boundsweep`, `tajprogress` and about thirty others — and that
codename is how its write-up is filed and cross-linked throughout
[`docs/open-items/`](docs/open-items/README.md). So a heading like
*FIXED: … — wave "shadowplay"* just means "this is the batch of work that
closed it", and a phrase like *swept as a class in wave "boundsweep"* means the
report was generalised and the whole shape was searched for, not just the one
instance.

Separately, **Waves 1–3** (numbered, not codenamed) were the three defined
milestone batches: safety and WebGPU, lighting contracts, and gameplay gates.
Those are the ones counted in the 23-of-23 completion figure in the README.

You do not need to adopt this scheme to contribute. It is described here only
so the existing write-ups read clearly.

## Things that are deliberate

Don't "clean these up":

- **The `GE007_` env-var prefix in vendored `platform/fast3d/` files.** Parts of the
  renderer are shared first-party code with the author's GoldenEye port (mgb64). The
  prefix stays so the two projects can converge on genuinely common code rather than
  diverging cosmetically. See [NOTICE.md](NOTICE.md).
- **`platform/host_window.c` sitting uncompiled.** `gfx_webgpu.c` really does call
  `platformHasHostWebGpu()`; those symbols are satisfied by the inert
  `gfx_webgpu_stubs.c`. The two files are mutually exclusive — adding
  `host_window.c` to the build without removing the stubs is a duplicate-symbol link
  error.
- **`docs/ref/`** is upstream-derived reference material, not first-party. It is the
  authority `platform/asset_swap.c` is written against.

## Reporting a bug

Include: the command line (with `--headless-frames`), the ROM version, the renderer
(`MDKR_RENDERER`), and the full `[CRASH]`/`[FATAL]` output. A headless repro with an
input script under `tests/input_scripts/` is worth more than a description.

## Licensing of contributions

First-party code in `platform/`, `tools/`, `tests/` and `docs/` is MIT
([LICENSE](LICENSE)). Contributions there are under the same terms. `game/` is
vendored decompilation carrying upstream's terms — see [NOTICE.md](NOTICE.md).

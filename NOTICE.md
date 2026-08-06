# Golden Balloon Notice

Per-component provenance. Read alongside [LICENSE](LICENSE) (first-party terms),
[DISCLAIMER.md](DISCLAIMER.md) (the project's legal position), and
[THIRD_PARTY.md](THIRD_PARTY.md) (the per-path provenance table this document
summarizes; every component named below has a row there).

This repository contains **no game ROM and no bulk game assets**, and never has —
not in the working tree and not in git history.

## First-party project code — MIT

Written for this project and covered by [LICENSE](LICENSE):

- `platform/` — the native port layer: the F3DDKR graphics HLE
  (`platform/fast3d/gfx_pc_dkr.c`), libultra replacement / cooperative scheduler
  (`stubs_dkr.c`), asset endianness normalisation (`asset_swap.c`), audio host
  output, frame pacing, and the arena/pointer-token machinery.
- `tools/` — the ROM-oracle harness, asset dump/classification tooling, the
  release ROM-absence guard, and the decomp sync tooling.
- `tests/` — fixtures and the behavioural regression checks.
- `docs/` **except `docs/ref/`** (see below), `CMakeLists.txt`, `cmake/`, and the
  web shell in `dist/web/`.

## The original game and its assets — NOT in this repository

The 1997 Nintendo 64 kart racer, its title, characters, music, textures, models,
sequences and level data are the property of their respective rights holders, which
may include (without limitation) **Nintendo** and **Rare** / **Microsoft**.

None of it is included here. You must supply your own legally-owned copy; assets
are read from it at runtime on your machine. In the browser build the ROM is read
client-side and never uploaded.

## Decompiled game code — third-party, upstream terms apply

`game/` contains C derived from the community
[Diddy Kong Racing decompilation](https://github.com/DavidSM64/Diddy-Kong-Racing)
by DavidSM64 and contributors, plus this project's `NATIVE_PORT`-gated
modifications (64-bit and endianness corrections, host-platform adaptations). It is
**not** covered by this project's MIT license; it carries whatever terms the
upstream project carries, and it reproduces the logic of a copyrighted work.

`docs/ref/` is also third-party: the `us.v80` linker script, `dkr_asset_spec.md`
and the 18 `asset_fileTypes/*.hpp` layout descriptions were vendored from upstream
at project scaffolding. They are load-bearing — `platform/asset_swap.c` is written
against those layouts — so they stay, but they are not this project's to license.

Modifications made here are contributed back under the same understanding.
The upstream baseline commit is recorded in `.decomp-baseline`; the merge process
is documented in `docs/DECOMP_SYNC.md`.

## Nintendo 64 SDK compatibility headers — clean-room

**SGI SDK *implementation* source has been removed from every shipping
target.** The 49 decompiled synthesiser sources that used to live under
`game/libultra/src/audio/` — the SGI-legend `libaudio` implementation (bank
parser, sequence player, envelope mixer, resampler, ADPCM decoder, reverb) —
were deleted outright (commit `2a180eb`) and replaced by
`platform/audio_compat.c`, `audio_event_queue.c` and `audio_fx_transfer.c` —
first-party audio code, clean-room in origin, brought over from the sister
**mgb64** codebase and extended here with DKR-specific behavior (dual aux-bus
`fxType[2]` lanes, `CSP_CHAN()` channel-state stride, arena-resident
allocation). See
`docs/MGB64_BACKFLOW.md` ("M5 audio") for the adaptation history and
[THIRD_PARTY.md](THIRD_PARTY.md) for the file-by-file provenance row.

The clean-room engine's audio *behavior* was cross-verified against the
decompiled synthesiser it replaced before that source was deleted — the same
decomp-derived lineage this document describes for `game/` generally — so the
swap changed the implementation, not the game's audio semantics. This is a
standing practice, not a one-time pass: `platform/audio_compat.c` carries an
in-code comment at every site where its behavior was checked against — and,
where they disagree, deliberately made to match — the deleted decomp source
or DKR's own documented quirks, rather than mgb64's original GoldenEye
behavior. Current sites (see the comment at each for the full reasoning):

- `alEnvmixerParam` volume application — linear, per DKR's `env.c`, not
  mgb64's square-law `(v*v)>>15` perceptual curve (commit `cdeb3e5`).
- The `AL_MIDI_VOLUME_CTRL` (raw CC7) / `AL_MIDI_UNK_8` (CC8 fade) control
  handlers — DKR's two-lane channel volume, multiplied together.
- `__resetPerfChanState` — seeds both volume lanes to full, closing an
  uninitialized-memory ordering dependency the original SGI code had.
- The `AL_MIDI_UNK_5F` handler — a faithfully-reproduced upstream oddity
  where the stored `fxmix` is cleared but the live voice is driven to the new
  value, so stored and audible state deliberately diverge.
- `__mapVoice`'s voice-limit comparison — kept as `limit < mapped` rather
  than `mapped >= limit` to match the original's effective `voiceLimit + 1`
  cap exactly.

`tools/compare_audio.py` and `tools/compare_audio_reference.py` (the
regression and reference-fidelity harnesses used to validate these sites) are
themselves ported from mgb64; see [THIRD_PARTY.md](THIRD_PARTY.md).

What remains under `game/include/` (`PR/`, `sys/`, and the top-level
`ultra64.h`) is once again true to the original framing: **declarations
only** — no SDK binaries, libraries, or implementation source are included,
and none of it is read by any build target (only the interface they describe
is). These declarations have been clean-roomed: the header *text* was
rewritten from scratch against the documented libultra interface, with codegen
proven identical to what it replaced (symbol tables byte-for-byte equal before
and after).

The declaration *text* is therefore first-party clean-room work. The interface
names, struct layouts, and calling conventions it describes necessarily stay
Nintendo/SGI-defined; this project asserts nothing over those, only over its
own expression of them. They stay because that is the ABI the decompiled game
code and the real N64 hardware both target — not a copyrightable expression
choice, the same distinction this document already draws for the game logic
itself.

`tools/check_native_sdk_surface.py` (run in CI) checks for legend text under
`game/` with two patterns — the classic SGI boilerplate, and a wider
Nintendo-copyright/bare-RCS-tag pattern that catches headers using a different
notice style. **The guard is green: assertion 3 reports zero hits, and there
is no allowlist — any hit is a real regression.** The headers that carried
the second notice style were either deleted
outright (`game/include/PR/{gs2dex,os_flash,os_gbpak,os_voice,rdb}.h`,
`game/include/sys/{asm,regdef}.h`, `game/include/ultrahost.h`) or rewritten
clean-room (`game/include/PR/os_motor.h` and `os_version.h`, which
remain). See [THIRD_PARTY.md](THIRD_PARTY.md) for the per-path provenance row.

## Emulator-derived reference material — NOT in this repository

The visual oracle (`docs/ORACLE.md`) builds a locally patched
[ares](https://ares-emu.net/) checkout and captures reference frames from the real
ROM for pixel comparison. The ares checkout, its build, and every captured frame
live under git-ignored paths (`build/ares-oracle/`, `*.ppm`). ares source is
patched in place, never vendored. No ROM-derived frame, save, or capture is
committed.

## Vendored platform code shared with mgb64

Parts of `platform/` (notably the GL and WebGPU backends under
`platform/fast3d/`) originate in the author's GoldenEye port **mgb64** and are
shared first-party work by the same authors. A standalone Metal backend
(`gfx_metal.mm`) originated there too; it was never built by any mdkr64 target
and was removed from this repository post-1.0.6 (it lives on in mgb64 and in
this repository's git history, should a Metal backend ever be revisited here).
Some of the surviving files retain that project's `GE007_` diagnostic env-var
prefix; this is intentional, not an oversight, so the two projects can converge
on genuinely common code rather than diverging cosmetically. See
`docs/DEVELOPER_HANDBOOK.md` ("Useful trace env vars") for what the prefix
covers and how to enumerate it.

## SMAA antialiasing lookup tables

`platform/fast3d/smaa_area_tex.h` and `smaa_search_tex.h` are the precomputed
lookup tables from the SMAA reference implementation (Jorge Jimenez et al.),
redistributed under the terms of that project.

## Third-party libraries

`lib/` contains third-party sources and data vendored per their own licenses and
compiled into or shipped alongside the app: Dear ImGui for the native app shell,
`glad` for GL loading, and the SDL game-controller database. Each retains its
upstream license.

## Full provenance table

The sections above are the narrative summary; [THIRD_PARTY.md](THIRD_PARTY.md)
is the canonical per-path table, and it additionally covers the pinned
build-time fetches (SDL2, the wgpu-native prebuilt) and the project-supplied
brand artwork, which do not fit neatly into the sections above.
`tools/check_third_party_notices.py` (run in CI) keeps the two documents from
drifting apart.

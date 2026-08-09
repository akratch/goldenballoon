# Content packs

How to replace Golden Balloon's textures with your own, and how the game
decides which file to use.

> **This project ships no content and hosts none.** Everything below reads files
> that you, or a pack author, put on your own machine. No pack is distributed
> with the game, no pack is hosted here, and the build fails closed if any pack
> content is ever tracked in this repository — see
> [`DISCLAIMER.md`](../DISCLAIMER.md) and [`NOTICE.md`](../NOTICE.md).

## Where packs go

| Build | Location |
|---|---|
| macOS `.app`, or any packaged build | `mods/` beside your save data, in the app's per-user data directory |
| Portable / command-line build | `mods/` next to the working directory you launch from |

There is no setting to move it. If the directory does not exist, nothing
happens and nothing is logged — that is the ordinary case.

## What a pack is

A directory — or a `.zip` — containing `pack.ini`, plus the files it replaces:

```
mods/
  sunset-skies/
    pack.ini
    textures/
      cc0b49dda797881dadb42914e932a9b6.png
      3f1a2b9c04d7e6558a1cbe07d2f43910.png
    music/
      35.wav
  aurora-skies.zip          ; the same layout, zipped
```

Both kinds go through the same reader and the same path validation, so a zipped
pack behaves identically to the unzipped one — that equivalence is asserted by a
gate rather than assumed. A zip that will not open is skipped with a reason,
like any other broken pack.

### `pack.ini`

```ini
[pack]
name     = Sunset Skies      ; required
author   = Somebody          ; optional
version  = 1.2               ; optional
priority = 100               ; optional, 0..9999, default 100
enabled  = 1                 ; optional, default 1
```

- **`name` is required.** A pack without one is skipped, and the reason is
  logged.
- **Every field has a length limit** (name and author 63 characters, version
  31). An over-long value is **rejected, not truncated** — a half-written name
  is worse than an obvious refusal.
- **`priority` decides who wins.** Packs load in ascending priority; when two
  packs supply the same file, the **higher** priority wins. Equal priorities are
  broken by directory name, compared case-insensitively so load order is the
  same on every machine.
- **`enabled = 0`** installs the pack but leaves it inert. Useful for keeping a
  pack on disk while you compare.
- Unknown sections and unknown keys are ignored, so a pack written for a later
  version still loads here.

At most **64** packs load. Beyond that the rest are skipped with a reason
rather than silently dropped.

## Replacing a texture

Texture files live in `textures/` and are named by a **content digest** — 32
lowercase hex characters — with a `.png` extension. The digest identifies the
picture, so a pack works regardless of where the game happens to load that
texture from.

Your PNG does **not** have to match the original's size. A 64×64 replacement for
a 16×16 original works; the game addresses the same logical tile either way.

### Finding a digest

```bash
MDKR_AUDIO=0 python3 tools/mod_texture_dump.py \
  --input-script tests/input_scripts/nav_to_track_select.txt \
  --frames 900 --out ~/dkr-textures
```

Every texture the game binds during that run is written as
`<digest>.png` — the exact filename a pack must use — alongside a `<digest>.txt`
recording its width, height, format and the frame it was first seen on. Each
digest is written once, however many times it is drawn. Drive whatever route
covers the textures you want; the menus, a track, a particular character.

**Never point `--out` inside this repository.** The output is decoded game data;
`mod-texture-dump/` is git-ignored for that reason, and the clean-room guard
fails closed if any of it is ever tracked.

The dump path is inert when the tool is not running: with the environment
variable unset the digest is not computed and no directory is created.

If you would rather compute names yourself, the digest is fully specified in
[`platform/mod_texture_key.h`](../platform/mod_texture_key.h) — the field list,
their order and their encoding — and has been independently reproduced from that
text alone by a second implementation.

The digest covers, in this exact order:

1. the format version (currently 1), as a little-endian `u32`
2. width and height, little-endian `u16` each
3. the RDP format, size and palette index, one byte each
4. the palette hash and palette format, little-endian `u32` each
5. the raw source texel bytes

It deliberately **excludes** anything that is a renderer choice rather than a
property of the picture — the allocation address, row addressing, whether mips
or a coverage-preserving filter were used, and whether the font atlas was
substituted. The same picture therefore has the same name however the renderer
decided to upload it.

**The digest is a published contract.** If it ever has to change, the version
constant is bumped, the old path keeps working, and this page says so.

## Replacing music

Put `music/<sequence id>.wav` in the pack and that track plays instead of the
sequenced original — starting, stopping and looping where the original would.
The Music volume slider governs it exactly as it governs the game's own music,
including the pause duck and any authored fade.

Any sample rate and channel count is accepted; it is resampled once when the
track loads, to the mixer's own rate. A file over 64 MiB is refused with a
logged reason rather than loaded.

The original sequence keeps running underneath, silently. That is deliberate:
the game's music drives timing and events that gameplay reads, so the sequence
is muted rather than skipped, and a pack cannot change how the game behaves by
replacing a track.

## Turning packs on and off

- **`Tab`** switches every override off and back on while you play, so you can
  compare a pack against the original on the same corner. It does not change
  your settings; it is a momentary A/B.
- **Settings → `Content.PacksEnabled`** is the durable switch. It currently
  takes effect the next time you launch.
- **`Content.PackDisabled`** is a comma-separated list of pack names to leave
  uninstalled. Matching ignores case and surrounding spaces.

## Seeing what loaded

**Settings → Content** lists every pack the game found: name, version, author
and priority for the ones that loaded, and every pack it skipped *with the
reason* — whether you disabled it, its own `pack.ini` switched it off, its
manifest was unreadable, or the manifest was missing a name.

The same summary is logged at startup when any pack is present:

```
[MODS] 1 pack(s) active, 3 skipped
[MODS]   skipped: Blocked Pack - listed in Content.PackDisabled
[MODS]   skipped: Off Pack - its pack.ini sets enabled = 0
[MODS]   active: Probe Pack 1.0 by verification (priority 100)
[MODS]   skipped: nomanifest - this directory has no readable pack.ini
```

Every skip carries a reason. A pack that does not appear at all is a pack the
game never saw — check the directory location above.

## Limits

- Decoded pack textures are capped at **512 MiB** in total; past that the
  least recently used are dropped and re-decoded on demand.
- PNG only, and only what stb_image's PNG decoder accepts. A file that will not
  decode is reported once, with the decoder's own reason, and then treated as
  absent.
- Override textures upload a single mip level. A pack texture replacing a
  mipmapped original will alias at distance.

## What does not work yet

Stated plainly so you do not spend an evening on it:

- **Custom models** and **custom characters**. Not implemented.

The scope and order of the remaining work is in
[`sprints/S1-content-pipeline.md`](sprints/S1-content-pipeline.md).

## For contributors

The pieces, and what each one owns:

| File | Responsibility |
|---|---|
| [`platform/mod_manifest.c`](../platform/mod_manifest.c) | Parse and validate one `pack.ini`. Pure — no filesystem. |
| [`platform/mod_registry.c`](../platform/mod_registry.c) | Discover `mods/`, order packs, resolve a relative path. Failure-isolating. |
| [`platform/mod_source.c`](../platform/mod_source.c) | One read interface over directory and zip packs, with the single path validator both use. |
| [`platform/mod_texture_key.c`](../platform/mod_texture_key.c) | The published digest. |
| [`platform/mod_texture_store.c`](../platform/mod_texture_store.c) | Decode, cache and evict override textures. |

The renderer consults the store in `dkr_bind_tile()`
([`platform/fast3d/gfx_pc_dkr.c`](../platform/fast3d/gfx_pc_dkr.c)), on a cache
miss only, and takes the ROM path unchanged when no pack answers.

Their gates are `mod_manifest`, `mod_registry`, `mod_source_zip`,
`mod_texture_key` — see [`../tests/README.md`](../tests/README.md). The rule
that keeps this feature legitimate is section 8 of
`tools/check_clean_room.sh`: no pack content may be tracked in this repository
or appear anywhere in its history.

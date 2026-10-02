# Content packs

How to replace Golden Balloon's textures with your own, and how the game
decides which file to use. This includes the portraits for Taj, Wizpig and
Terry, which the port draws itself because the original game has none —
see [The three bonus racers' portraits](#the-three-bonus-racers-portraits).

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

## High-resolution texture packs

There is a second kind of pack, and it is the commonest one: a
**Rice/GLideN64 high-resolution texture pack**, the format the Nintendo 64
emulator scene has used for twenty years. It carries no `pack.ini`. Each image
is named after the texture it replaces, like this:

```
Diddy Kong Racing#51F45E32#0#3_all.png
```

Golden Balloon reads that name directly and recognises the texture while it
draws, so **there is nothing to unpack, convert, rename or build**. Drop the
pack in as it downloaded — the `.zip`, or the folder — and press Play.

### Getting one

This project ships no pack and hosts none; the link below is a pointer to
somebody else's community, not a download from us.

The pack this release was tested against is the **DKR-R HDR Texture Pack**, a
community project led by `sr.gu` that re-imagines the original artwork in HD
while staying faithful to the game. It is distributed through the
[DKR-R Discord](https://discord.com/invite/AMWfXdBjNP), linked from the
[DKR-R project on GitHub](https://github.com/ThatGuyMcd/DKR-R). The build
verified here was `DKR REMASTERED (Almost Complete V0.1.6).zip`.

Any other pack in the same format should work; nothing about the reader is
specific to that one.

### Installing one

Either way works, and they do the same thing:

- **Content → Install pack…** — pick the pack and the launcher copies it in
  for you. On macOS you can pick the `.zip` or the folder; on Windows and
  Linux, pick the `.zip` (put a folder in with the second route). On macOS the
  mods folder lives inside a `Library` folder the Finder hides, so this is the
  easier route there.
- **Content → Open mods folder**, then drag the pack in yourself. On Linux
  this button appears when `xdg-open` is installed; the folder's location is
  always shown under the buttons.

Then press **Play**. The mods folder is rescanned on every launch, so you do
not need to restart Golden Balloon.

A pack that is already installed is never replaced. To update one, remove the
installed copy from the mods folder first, then install the new one.

### Confirming it worked

**Content** names the pack and how many textures it supplies:

```
DKR REMASTERED (Almost Complete V0.1.6).zip — high-resolution textures (1663)
```

and the log says the same thing at startup, plus — on quit — how many of them
the game actually drew:

```
[MODS] 1663 high-resolution texture identities indexed (Rice/GLideN64 pack)
[MODS] 1 pack(s) active, 0 skipped
[MODS] 169 high-resolution textures used this run
```

Those are two different claims and both are worth reading. The first says the
pack was *understood*; the second says it was *used*. A pack that indexes
thousands of identities and then draws none is installed but not matching, and
that is a different problem from one that never loaded.

### What it will not do

- **A pack only replaces what it contains.** No pack covers every texture in
  the game, so some original artwork always remains — that is the pack's
  coverage, not a failure here.
- **Colour-index textures with several palettes collapse.** Where a pack
  supplies more than one palette variant of the same picture, the last one it
  lists wins for all of them. Keying them apart needs the palette hashed at
  draw time, which this port does not do yet. No pack measured so far uses
  this: the tested pack carries a palette field on none of its 1663 textures.
- **One mip level.** As with any pack texture, a replacement for a mipmapped
  original will alias into the distance.

## Replacing a texture

Texture files live in `textures/` and are named by a **content digest** — 32
lowercase hex characters — with a `.png` extension. The digest identifies the
picture, so a pack works regardless of where the game happens to load that
texture from. Upper and lower case both work, in the folder name, the digest
and the extension, in a folder pack or a zip. If one pack holds the same digest
twice in different case, it uses the all-lowercase file (or, with none, the one
that sorts first) and the log names both when the game starts. A file under
`textures/` with any other name is never used, and the log says so when the
game starts.

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

A third file, `<digest>.texels`, holds the raw game texel bytes that texture's
name was computed from. Nothing a pack author does needs it — it is there so
tooling can recompute a *different* naming scheme's hash over the same bytes,
which is how a pack written for another emulator can be converted. Ignore it,
and never publish it: unlike the PNG it is not a picture, it is a slice of the
cartridge.

**Never point `--out` inside this repository.** The output is decoded game data;
`mod-texture-dump/` is git-ignored for that reason, and the clean-room guard
fails closed if any of it is ever tracked.

What you get is what the game drew. If one of your own packs was installed
during the run, that texture is dumped as your replacement, at your
replacement's size — under the same filename, because the name belongs to the
picture being replaced. Dump with your packs out of the way when you want the
original.

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

## The three bonus racers' portraits

Taj, Wizpig and Terry have no portrait in the original game, so the port draws
theirs itself. They are still ordinary pictures as far as a pack is concerned:
put `textures/<digest>.png` in your pack and it replaces the card, the same way
it replaces anything else. One file covers every place that racer's portrait
appears — the results card and the Adventure HUD both.

Your replacement does not have to be 40×40. Whatever size you author, it fills
the same card.

Today the three are:

| Racer | `textures/…` |
|---|---|
| Taj | `dcd45f4f32c9e1da4abeb3c1c1f8011b.png` |
| Wizpig | `813ff52ed6575a1f29c8fa2fc47b2464.png` |
| Terry | `fd222569d7bd95580075402b7315e178.png` |

You can also find them yourself, exactly as above: dump the textures on a route
that reaches the results screen with that racer in first place, and look for the
40×40 card in the output.

These three are drawn by the port rather than read from the game, so retouching
the artwork changes its content digest. The blue-elephant Taj portrait in 1.7
keeps `7757ffb6d3f809fbde246ca559d51eb4.png` as a compatibility alias: existing
packs need no changes. If valid replacements exist under both Taj names, the
current name wins. Disabling packs disables both names. The author dump always
uses the current digest, including when the replacement came from an old-name
file. Wizpig and Terry's names are unchanged.

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

## Replacement 3D models for sprites

Some scenery and pickups are flat sprites that always face the camera: the
palms (trunk and two canopy sizes), the beach, blueberry and rubber trees, the
five weapon-balloon colours, the two collectible balloons and the banana. A
pack can draw any of them as a 3D model instead. This needs the **WebGPU**
renderer, the default. The OpenGL renderer has no model path: it keeps drawing
the original sprites, and the startup log says how many sprite models it is not
drawing.

```
mods/
  island-trees/
    pack.ini
    models/
      palm/
        near.dkrmesh            ; trunk, close up
        far.dkrmesh             ; trunk, at a distance
        canopy-near.dkrmesh     ; both canopy sizes share these
        canopy-far.dkrmesh
        50414c4d33440001.png    ; the atlas every palm mesh samples
```

| Folder | Mesh files | Atlas PNG(s) |
|---|---|---|
| `palm` | `near`, `far`, `canopy-near`, `canopy-far` | `50414c4d33440001` |
| `beach-tree` | `near`, `far` | `4245414348330001` |
| `blueberry` | `near`, `far` | `424c554542330001` |
| `rubber-tree` | `near`, `far` | `5255424252330001` |
| `balloons` | `near`, `far` (the five weapon colours), `collectible-near`, `collectible-far` | `42414c4c33440093` to `...0097` (one per colour), `42414c4c3344009a`, `42414c4c3344009b` |
| `banana` | `near`, `far` | `42414e414e413301` |

- **Each sprite's files come from one pack.** Its two meshes and its atlas are
  read from the highest-priority pack that has its `near` mesh (`canopy-near`,
  `collectible-near` for those sprites). If one of them is missing there or
  does not load, that sprite keeps its original look; files are never mixed
  across packs.
- Meshes are `.dkrmesh` files (`DKRPM001`), at most 4 MiB each. Atlases are PNG,
  at most 16 MiB on disk and 4096 px a side, with at most 64 MiB decoded per
  atlas and 256 MiB in total. Texels with alpha below 0.45 are cut out; every
  other texel is drawn solid.
- A model takes on the material of the sprite it replaces: it fades in and out
  where the sprite would, and takes the level's light tint where the sprite
  would.
- **`F2`** switches the models off and back on while you play, keeping replaced
  textures, so you can compare against the original sprites. Like `Tab`, which
  turns models off together with textures, it changes no setting.
- If the graphics device cannot create what the models need, the game logs it
  once and draws the original sprites until the graphics device is rebuilt or
  the packs are rescanned.

## Turning packs on and off

- **`Tab`** switches replaced **textures** off and back on while you play, so
  you can compare a pack against the original on the same corner. It does not
  change your settings; it is a momentary A/B. Replacement music keeps playing
  either way: a track cannot be swapped part-way through without leaving a gap
  where the music should be, so music follows the setting below and not this
  key.
- **Settings → `Content.PacksEnabled`** is the durable switch, and it takes
  effect straight away — textures change within a few frames, no relaunch. If you
  had pressed `Tab` to look at the original, changing this setting ends that
  comparison and shows you what the setting says; press `Tab` again if you want
  it back.

  Music is the one thing that waits: a replacement track that is already
  playing plays out, and the switch applies from the next piece of music
  onwards. Cutting a track off mid-play would leave silence rather than the
  game's own music.

  Adding or removing a pack in the `mods` folder, or a file inside one, still
  needs a relaunch — that is the folder being read, not this switch.
- **`Content.PackDisabled`** is a comma-separated list of pack names to leave
  uninstalled. Matching ignores case and surrounding spaces.

## Packs apply in every presentation mode, including Pure

A pack replaces textures in Pure and Restored exactly as it does in Remastered.
Nothing in the override path consults the presentation mode.

**This is a decision, not an oversight**, and it is written down here because it
was previously neither. Installing a pack *is* the opt-in. A player who put a
folder in `mods/` and then launched with `--pure` asked for both things, and
silently dropping their pack — with the pack list still saying it loaded — would
be the more surprising behaviour of the two.

The consequence you need to know: **Pure with a pack installed is no longer
byte-exact to the original picture.** Pure's guarantee is about the port's own
choices — framing, filtering, timing — and it cannot extend to art you replaced
yourself. If you are using Pure as a reference for comparison against original
hardware or an emulator, turn packs off first (`Tab`, or **Settings → Content**),
because the mode indicator will still read Original either way.

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

A high-resolution pack adds the two lines shown under
[High-resolution texture packs](#high-resolution-texture-packs): how many
texture identities it indexed at startup, and how many were actually drawn by
the time you quit.

## Limits

- Replacement textures are read and decoded in the background, so scene
  changes no longer pause for them. A replacement that has not been decoded
  yet — the first time it is needed, or after it was dropped to stay under the
  cap below — shows the original texture for a few frames until it is ready.
  Replacements already decoded appear straight away after a level loads or
  packs are switched back on; at other times a burst of them can take a frame
  or two to all appear.
- Enabled zip packs are kept open while the game runs. On Windows a zip pack
  cannot be deleted or replaced until you close the game.
- Decoded pack textures are capped at **512 MiB** in total, textures still
  being decoded included; past that the least recently used are dropped and
  re-decoded on demand.
- PNG only, and only what stb_image's PNG decoder accepts. A file that will not
  decode is reported once, with the decoder's own reason, and then treated as
  absent.
- Override textures upload a single mip level. A pack texture replacing a
  mipmapped original will alias at distance.

## Experimental custom characters

The custom-character branch contains a WebGPU-only vertical slice. The stable
author handoff is a self-contained GLB 2.0 plus a declarative manifest and
license text, packaged as `.mdkrchar`. It does not require a second ROM. Install
and removal are available in the launcher's dedicated **Character Workshop**;
Settings keeps a shortcut plus the current enabled/disabled and P1-P4 assignment
summary. The Workshop can browse, drag-and-drop, or accept a typed package path, validate it
without changing installed files, compare full/short/narration/sort identity,
portrait, donor/vehicle support, rig,
LOD0 geometry, animation, and texture-memory facts against the installed
revision, then require both a local-use rights confirmation and an explicit
reviewed install. It can also rescan, assign a different presentation to P1-P4, disable or
re-enable it without losing Workshop work, permanently delete it,
and edit intended standing height, source facing, animation speed, LOD
preference, and car/hover/plane pairing. Character select, car, hovercraft, and
plane each have independent size/XYZ/rotation controls and an anchor reset, so
placing feet on the roster floor cannot disturb a pelvis-to-seat fit. Front,
side, and top spatial pads edit the same translation values; a forward dial
edits context yaw; and vehicle contact pads directly tune each mapped hand and
foot offset. Fit can be copied explicitly between qualified vehicles and every
gesture remains available through Fit undo/redo.
Package-specific fit follows the same character when it is assigned to another
player. Those settings never alter physics or the vehicle selected by the game.

```sh
# Optional CLI equivalent of the Workshop's no-overwrite DAE/ZIP conversion.
# A ZIP must contain exactly one DAE or character-ready self-contained GLB.
# Stored and Deflate members are accepted; per-member and aggregate expansion
# ratios are checked from ZIP metadata before any model or nested archive read.
python3 tools/character_package_manager.py --directory characters \
  convert-authoring-source downloaded-model.zip model.glb

# The lower-level adapter remains useful for an explicit bounded DAE subset.
python3 tools/collada_to_glb.py source.dae --output model.glb

# Inspect before packaging; --require-character applies the renderer contract.
python3 tools/character_asset_probe.py probe model.glb --require-character

# Optional: generate a reviewable manifest from named clips/nodes rather than
# memorizing semantic and socket names. Review and edit the emitted JSON.
python3 tools/character_manifest_wizard.py model.glb \
  --id org.example.character-name --display-name "Character Name" \
  --spdx CC-BY-4.0 --attribution "Creator Name" \
  --source-url https://example.invalid/source \
  --source-forward=-z --target-height 1.25 --output manifest.json

# Optional source-v4 humanoid proposal. This also requires --portrait and
# --minimap-rgb. Inferred mappings are deliberately emitted reviewed:false;
# inspect/edit the manifest before explicitly changing that author decision.
python3 tools/character_manifest_wizard.py model.glb \
  --id org.example.character-name --display-name "Character Name" \
  --spdx CC-BY-4.0 --attribution "Creator Name" \
  --source-url https://example.invalid/source --portrait portrait.png \
  --minimap-rgb 220 72 144 --rig-mode humanoid-retarget-v1 \
  --output manifest.json

python3 tools/character_asset_probe.py pack \
  --model model.glb --manifest manifest.json --license LICENSE.txt \
  --output character.mdkrchar
python3 tools/character_asset_probe.py verify character.mdkrchar

# Make the same package player-portable by embedding the deterministic cache.
# A packaged launcher imports this natively and does not need Python.
python3 tools/character_package_manager.py prepare \
  character.mdkrchar character-portable.mdkrchar

# Direct CLI install remains useful for automation and source checkouts.
python3 tools/character_package_manager.py \
  --directory characters install character.mdkrchar

# Cache/source provenance and lifecycle commands use the manifest's stable id.
python3 tools/character_package_manager.py --directory characters list
python3 tools/character_package_manager.py \
  --directory characters disable org.example.character-name
python3 tools/character_package_manager.py \
  --directory characters enable org.example.character-name

# Enumerate every authenticated retained revision. Restore accepts any exact
# source SHA shown here and preserves the package's enabled/disabled state.
python3 tools/character_package_manager.py \
  --directory characters revisions org.example.character-name
python3 tools/character_package_manager.py \
  --directory characters restore org.example.character-name SOURCE_SHA256

# Export never overwrites an existing path.
python3 tools/character_package_manager.py \
  --directory characters export org.example.character-name SOURCE_SHA256 \
  recovered-character.mdkrchar

# Destructive: removes the cache plus every locally retained source revision
# and provenance report for this exact id. It does not touch an external file.
python3 tools/character_package_manager.py \
  --directory characters remove org.example.character-name
```

### Reproducible release-spike evidence

Maintainers can exercise the complete intake-to-render path without a private
model. The generated fixture is CC0, deterministic, and deliberately awkward:
centimeter transforms, sibling pelvis/spine roots, long limbs, hair joints,
multiple materials, unusual proportions, and an animationless skinned source. The
command below uses a disposable character library and refuses to overwrite an
existing evidence directory:

```sh
python3 tools/run_character_spike_evidence.py \
  --source fixture \
  --license tests/fixtures/custom_character_adversarial/LICENSE.txt \
  --rom /path/to/legally-obtained-dkr.z64 \
  --evidence-dir /new/path/character-evidence \
  --build build \
  --synthetic-validation-seam
```

The synthetic seam is CI-only and is rejected for private/external assets. For
a GLB, DAE, or unambiguous ZIP, omit that flag and provide the pinned native
Khronos validator (or configure it normally), plus a reviewed decisions file:

```sh
python3 tools/run_character_spike_evidence.py \
  --source /private/path/model.glb \
  --license /private/path/LICENSE.txt \
  --decisions /private/path/reviewed-decisions.json \
  --rom /private/path/dkr.z64 \
  --evidence-dir /new/path/private-model-evidence \
  --build build
```

The decisions shape is documented in
[`docs/ref/mdkr-character-spike-decisions-v1.example.json`](ref/mdkr-character-spike-decisions-v1.example.json).
It records human choices—identity, rights metadata, donor/vehicles, front,
height, sockets, all 16 humanoid roles, and deliberately disabled animation
semantics—rather than pretending those decisions are safe to infer. An adjacent
`model.glb.mdkr-character-spike.json` is discovered automatically when
`--decisions` is omitted.

Only redacted logs, PNG screenshots, and `evidence.json` are retained. The
manifest records source/license/ROM digests and sizes, never those payloads or
their absolute paths. Conversion, packaging, validation, compilation, and the
temporary installed catalog are destroyed when the run exits. The normal
Character Workshop library is never read or modified. Cadence is measured only
after warm-up under real-time enhanced pacing; screenshots come from a separate
exact-context run so capture I/O cannot manufacture a p99 regression or pass.

The runtime never reads DAE, GLB, JSON, or PNG source packages during a frame.
Installation validates and compiles them into a bounded `.mdkc`; the launcher
discovers that cache on its next scan. A portable package carries the exact
validated `.mdkc` generated from its source. The packaged launcher validates
that cache and cryptographically binds it to the exact manifest, model, and
license bytes; gameplay never compiles GLB. Release packages also carry the
project-owned importer as a self-contained, attested helper beside the game
executable. The Workshop invokes that helper directly for raw GLB/DAE/ZIP and
source-only package authoring, so players do not install Python or trust a tool
from `PATH`. Source checkouts use the same manager through Python as a developer
fallback. The manager recompiles and byte-compares portable caches. The manifest
must match the complete
example and schema in the architecture document, and every named animation or
socket must exist in the GLB.

Validation and installation are deliberately separate. Candidate review binds
the exact package-file SHA-256 and the installed cache's canonical source digest.
It also shows the package's authenticated SPDX declaration, creator /
attribution, and source URL beside the installed revision. The exact
`LICENSE.txt` is included in the same source digest, but neither those bytes nor
the manifest declarations establish that the person importing the package has
the necessary rights; the final local-use confirmation therefore remains
required. Compiler-v1 through compiler-v6 portable caches continue to work.
Versions before v5 are labeled as legacy when their cache cannot expose
provenance; versions before v6 fall back to the display name for compact,
narrated, and sorting identity. Use `prepare` with current tools to add the
current records.
At commit, both are checked again under the shared import lock. If the package
changed, an update of the same ID appeared, or the installed revision changed
after review, nothing is published and the Workshop requires a fresh review.
Portable packages use the native path without invoking the helper. Source-only
packages use the bundled self-contained helper (or the source-checkout fallback)
for the same versioned fixed-field, bounded summary and reviewed transaction.
Drag-and-drop stages the same review instead of bypassing it.

An enabled cache is named `<id>.mdkc`. Disable atomically moves the same
validated bytes to `<id>.mdkc.disabled`, which the game does not scan but the
Workshop still inventories. Importing an update or saving a Portrait, Rig, or
Profile Studio revision preserves that state. Player assignments, package fit,
and review evidence remain stored while disabled; the built-in racer is used
until the package is enabled again. Permanent deletion is separate and removes
the cache, all content-addressed Workshop source revisions and reports, and the
package-owned local preferences. Its source-generation-bound recovery marker
and private quarantine are committed under the same cross-process lifecycle
lock as those launcher preferences, named drafts, and exact-test records. A
concurrent same-ID reinstall waits; restart recovery never deletes a newly
installed same-ID package or its current metadata.

The Package tab authenticates every retained revision before offering recovery.
**Export selected source** copies those exact authoring bytes. **Export portable
package** instead compiles that selected revision with the current compiler and
embeds the validated cache for another player; it publishes exclusively and
never overwrites a destination or changes the installed character. **Rebuild
current assembly** re-authenticates the active source and atomically replaces
only its disposable cache after successful compilation and validation. A
disabled character stays disabled, and any failure leaves the last known-good
cache and all source history intact.

The Workshop accepts a self-contained GLB 2.0 file as an authoring source. It
also accepts an explicit DAE or a recursively nested authoring ZIP when the ZIP
contains exactly one DAE or character-ready GLB. The user chooses a new `.glb`
destination; traversal, symlinks, encryption, unsafe nesting, expanded-size and
member-count overflow, excessive per-member or aggregate compression ratios,
unsupported compression, ambiguous model choices, external resources, and
every overwrite fail closed. Conversion uses a private temporary extraction
and never changes the download. Missing archive license material is reported, but the
raw draft still requires the user to choose exact license/notice bytes before
Build. The published intake ceiling is expanded bytes <= 200 times compressed
member bytes plus 1 MiB, applied to each regular member and to the whole archive
at every nesting level. Inventory JSON reports compressed/expanded totals and
both policy constants so rejection is diagnosable rather than a hidden limit.

GLB inspection validates every buffer, buffer view, and accessor before using
its metadata: declared-buffer/BIN bounds and padding, positive counts,
component and 4-byte vertex alignment, effective strides, normalized/type
contracts, tightly packed non-vertex data, and exact attribute/sampler counts.
Sparse accessors and packed integer matrices require normalization in the DCC
export because cache v1 does not silently reinterpret them. The probe reads the
actual POSITION floats and refuses declared min/max that do not match, so source
height, ground anchoring, and later vehicle fit cannot be derived from false
metadata. It likewise rejects non-finite consumed floats, primitive-restart or
out-of-range indices, zero directions, invalid tangent handedness, negative or
zero-total weights, malformed animation times, mismatched cubic output, and
invalid inverse-bind/image views. Scene hierarchy cycles, multiple parents,
non-unit rotations, affine shear the v1 cache cannot preserve, invalid material
or texture references, unsupported required extensions, and corrupt or
dimension-lying embedded PNGs also fail before packaging. These are local
profile checks; they do not replace the planned pinned Khronos Validator report
for complete glTF conformance. Reports retain the first 256 exact GLB
diagnostics and one suppression count, so a hostile file cannot turn error text
itself into an allocation problem.

Character materials may embed ordinary PNG images or Basis Universal KTX2
images through required `KHR_texture_basisu`. KTX2 must be a single 2D ETC1S or
UASTC image, no larger than 4096 pixels per side, with 1–13 authored mip levels;
UASTC may use Zstandard supercompression. Encode base-colour and emissive maps
with an sRGB transfer function, and metallic/roughness, normal, and occlusion
maps as linear. The importer rejects a mismatch instead of letting lighting
look subtly wrong. Do not choose BC, ASTC, or ETC at export time: the WebGPU
renderer automatically transcodes to BC7, ASTC 4x4, or ETC2 when granted by the
device and otherwise uses bounded RGBA8. Workshop review reports the exact
source codec, mip range, compressed bytes, and RGBA-equivalent safety budget;
runtime diagnostics record the format actually selected. PNG remains the
simple universal source option and receives a generated full mip chain.

This is not an install shortcut. A resumable first-import draft fingerprints and
inventories the bounded GLB, then requires a stable package ID, display name,
exact license/notice file, SPDX expression, attribution, source URL, built-in
gameplay donor, vehicle scope, forward axis, standing height, motion starting point,
seat/pelvis node, and head node. Inferred clip/socket names are starting points,
not truth, and remain directly selectable from the exact model inventory. SPDX
syntax is parsed in the editor and again at the package trust boundary; this
checks expression structure and identifier spelling, not whether an identifier
is present in a particular evolving License List or whether the declaration
grants the user rights.
Duplicate animation or node names fail explicitly because a name-based manifest
could not identify them unambiguously. If the GLB changes after inspection,
Build refuses it and requires a new inventory. The deterministic source-only
candidate then enters the same package diff, provenance review, local-use rights
confirmation, and optimistic install transaction described above. Clearing the
draft removes only launcher-owned form data and its disposable candidate; it
never deletes the external GLB or license file.

The wizard emits `mdkr-character-source-v2` by default, v3 when identity media
is supplied, and v4 when `--rig-mode` is also selected. Source-v5 is the
strict additive contract for optional authored joint constraints and secondary
chains; until its dedicated Studio lands it is intended for hand-authored or
external-tool manifests, and v1-v4 do not silently accept its fields.
`--source-forward` declares which
local horizontal axis the model's face points toward (`+z`, `-z`, `+x`, or
`-x`); geometry alone cannot answer that reliably. `--target-height` is the
intended standing height in meters. The compiler measures the transformed
source bounds, normalizes them to unit height, records a bottom-center ground
anchor, and creates separate ground/select and pelvis/vehicle profiles. Old v1
packages still load but are clearly marked as legacy calibration in the
workshop. If a preview is backward, use the visible 180-degree correction; if
it is misplaced, adjust only the affected context.

Source-v3/v4/v5 identity may additionally author `short_name`, `narration_name`,
and `sort_label` (each up to 96 UTF-8 bytes). Portrait Studio exposes all four
names only inside a named draft, so a label edit cannot accidentally publish a
partial identity revision. Compact select tiles use the short name, the detail
view uses the full display name, and roster ordering uses the sort label. The
narration field is spoken by the Workshop library and player-assignment
controls. Covered Latin, Greek, Cyrillic, Arabic, Hebrew, combining, and mixed
right-to-left names use the same pinned HarfBuzz/SheenBidi raster path previewed
in Portrait Studio. Missing glyphs and unsafe invisible direction controls fail
closed to the disclosed retail-font projection; no host font or second ROM is
consulted. Launcher fields accept logical-order UTF-8 and merge the complete
pinned character-name faces so input stays readable; the shaped character
heading and Identity previews are the authoritative in-game order, joining,
and width fit.

Portrait Studio can also start from any local, non-animated, non-interlaced,
8-bit RGB/RGBA PNG from 16 through 4096 pixels per side. It validates the full
PNG container and CRCs, then provides a deterministic square crop, crisp or
premultiplied-area reduction, optional edge-connected matte removal, and local
background frames before the existing style and pixel-editing stages. A
stabilized exact-renderer PNG from the Test capture tray uses the same path.
Named drafts retain the source kind, SHA-256, dimensions, crop recipe and exact
40x40 intermediate; the external path is only a reload convenience and the
source PNG is never bundled into the character package.

Animation names are mapped to engine intent, not hard-coded frame numbers. The
recommended race states are `race.steer`, `race.reverse`, `race.boost`,
`race.damage`, `race.item`, `race.spin`, `race.airborne`, `race.land`,
`race.finish_win`, and `race.finish_lose`; selection clips are `select.idle`,
`select.hover`, and `select.confirm`. Missing optional states use the required
`fallback` clip. A skinned GLB with no animation may instead use the Workshop's
explicit bind-pose starting point; the compiler creates no source animation and
the character cannot be enabled until reviewed humanoid reference motion or
moving source clips cover every required semantic. Author `race.steer` as a pose strip: phase 0 full left, 0.5
neutral, and 1 full right. Damage, land, and selection confirmation are
one-shots; landing is driven for 0.2 seconds after an airborne-to-grounded
edge. The launcher names missing mappings, reports which mapped clips actually
move, and separately reports geometry, normalization, anchors, rig sockets,
motion, semantic skeleton roles, review state, and donor qualification. A positive-duration
identity clip remains a T-pose—it proves timing plumbing, not authored motion.
Root rotation and seat offsets cannot repair that. A model needs real authored
semantic clips or a reviewed humanoid role map. Source-v4 validates and
compiles that role contract; reviewed humanoids receive bounded engine-reference
poses and vehicle hand/foot contact solving for missing semantics while
explicit authored clips win. The importer refuses to disguise either static
clips or an unreviewed map as animation readiness.

Exact vehicle tests reset contact telemetry after their normal warm-up and
return the number of bounded solves plus mean and maximum endpoint-to-target
error in physical units. This is fit evidence, not an import limit: unusual
proportions or intentionally unreachable tuned targets may legitimately report
larger distances.
Each vehicle placement tab keeps the matching last warmed result beside its
contact controls and offers a one-click save-and-test action into the real
renderer, so authors do not have to shuttle between unrelated Workshop panels.
Changing any fit/solver setting or revising a package invalidates the prior
session result; stale measurements are never presented as evidence for the
new configuration.
After a successful warmed test that actually drew the replacement, an author
can mark that context reviewed. The review is persisted against a SHA-256
signature of the exact package source, donor, global fit, animation speed,
context transform, and (for vehicles) contact offsets. It therefore survives a
restart but automatically becomes “review required” when any relevant input
changes. Select, car, hovercraft, and plane are reviewed independently.
The Workshop summarizes current approvals across enabled contexts and lets an
author explicitly reopen any review without changing the saved fit.

The Workshop's Rig Studio loads the exact skin-joint inventory from the active
cache, shows every semantic role with its compiled node number, name,
inference provenance, confidence, rest correction, and bend axis, and prevents
one joint from being assigned twice. Humanoid review stays disabled until all
16 roles form the required ancestor chains. Any mapping or solver-basis edit
clears review. Saving uses the package manager's transactional `revise-rig`
operation: v3 identity sources upgrade to v4, all other source members remain
byte-identical, and a failed compile leaves the current playable cache active.
The bind-pose skeleton canvas is derived from the compiled node TRS hierarchy,
supports front/side projections and helper-joint filtering, highlights the
selected semantic chain, and can assign a visually selected joint to the active
role. Exact named controls remain the accessible authoritative path, and every
canvas assignment clears rig review before it can be compiled.

In character select, press **R** for an unconfirmed player to open that player's
independent custom-racer browser. It shows eight portrait/name tiles per page,
supports all 64 bounded local catalog entries, centers partial pages, and keeps
each local player's choice separate; players may also choose the same package.
The selected package's declared donor remains its clearly labelled gameplay
profile. Its `select.idle`, `select.hover`, and `select.confirm` mapping then
drives the exact fingerprint-qualified donor actor seam while the numbered
player placard remains. The package name, portrait, minimap colour, and
assignment identity remain independent of that donor; donor voice and music
are deliberately neutral during custom selection.

The Workshop's gameplay-profile picker compares all ten qualified donors using
exact coefficients extracted during the launcher's existing supported-ROM
validation pass. It shows effective weight (including the game's authored 0.45
scale), signed handling, and all 14 acceleration-curve samples. Relative bars
only locate a value within the built-in roster; they are not ratings and do not
change gameplay. Car, hovercraft, and plane curves are resolved independently
through the same object-header translation path used by the game. The plot has
an exact text table for keyboard and screen-reader use. The summary contains
only finite numeric values and never
stores ROM bytes in the package, configuration, or installed-character cache.
If no supported ROM is currently verified, package selection and saving remain
available while the comparison explicitly says its evidence is unavailable.
The adjacent ownership card is the save review: the package owns local model,
materials, rig/animation/fit, name, portrait and minimap colour; the donor keeps
simulation, collision, in-race voice/horn/vehicle audio, ghost character ID and
network/rollback character ID. Course records and adventure saves remain normal
game data and do not store the package. Character-select audio stays neutral,
and online package negotiation/fallback is not yet implemented.

`MDKR_CUSTOM_CHARACTER_DIRECTORY` is a diagnostic and automated-test override
for launching against an isolated catalog. Ordinary players should use the
workshop-managed per-user character directory; the override does not bypass
package validation or installation policy.

The launcher Workshop's **Test in the exact game renderer** section starts the
selected package directly in character select or an Ancient Lake car,
hovercraft, or plane race. Choose any one- through four-player layout first;
all requested local seats temporarily use the package so split-screen cost and
fit are visible. The launcher still performs its final full ROM integrity check
before starting. These tests use the real select/race initialization and WebGPU
character path without controller scripts, do not change saved P1-P4
assignments, and restore pre-existing diagnostic environment values when the
engine returns. Vehicle buttons are unavailable when that package's saved
compatibility profile excludes the vehicle. Press **F1** to return to the
launcher and the same package inspector.

An exact-context result card appears after returning. The first 120 authored
ticks are a warm-up and are excluded so level loading and initial shader work do
not masquerade as steady-state character cost. Opening F1 freezes the sample
before overlay navigation. With at least 60 displayed intervals under real-time
pacing, the card reports median, 95th/99th percentile and worst displayed
cadence, authored tick-wall mean, and modern-character replacement/part counts.
Short or synthetic runs remain visible but are labelled diagnostic-only.

This is an exact in-game test route, not an embedded renderer. Wall cadence
includes the complete scene, presentation policy, resolution and device. On
WebGPU devices with timestamp-query support, a separate exact result brackets
the initial gameplay color/depth pass; native devices that additionally expose
in-pass timestamps report the sum of exact accepted custom-character draw
ranges. A six-slot asynchronous readback ring never stalls a frame. Unsupported,
pending, ring-full, invalid, device-lost and error states stay visible, and the
Workshop never substitutes wall cadence for GPU time or claims spare headroom.
The adjacent pose inspector can hold
every supported animation semantic at an exact normalized phase. Zero
yaw/pitch uses the ordinary gameplay camera. Every nonzero vehicle view is an
absolute racer-relative orbit around the imported model's fitted bounds, with
automatic vehicle-aware pull-back and transient cutscene-camera suppression;
the existing obstruction resolver still owns the final camera. Neutral,
bright, low-key, and backlit presets change only the custom
character light; they never change the world, vehicle, simulation, or saved
fit. Character select deliberately keeps its authored camera.

An inspection may exclusively create one output-sized PNG after the 120-tick
warm-up and 12 consecutive frames with a completed replacement draw, requested
pose, view, and character light. Successful captures enter a session-only report tray
with exact source/fit digests, context, pose/phase, view, light, dimensions,
and exact-versus-fallback state. The tray exports a self-contained responsive
HTML contact sheet containing base64 PNGs and machine-readable JSON, but no
model, package, ROM, or original capture-path bytes. Existing PNG or HTML files
are never overwritten. Representative scene variants and an embedded renderer
remain product work. Device-profile collection is supported through an
optional complete-matrix JSON export with an explicit GPU/driver identity
disclosure; representative physical-device observations still need ongoing
project maintenance. The public format and comparison rules are documented in
[`architecture/character-device-profile-v1.md`](architecture/character-device-profile-v1.md).

All ten retail vehicle-model families now have exact revision-1 fingerprint and
driver-batch profiles for car, hovercraft, plane, and character select. Their
collapsed/merged far LOD is capped before carving. Unknown models or changed
fingerprints remain visible, and OpenGL keeps every retail visual. Packages are
local-only; the game does not transfer them to peers. See
[`architecture/custom-character-pipeline.md`](architecture/custom-character-pipeline.md)
for the format, limits, security model, current proof and remaining gates, and
[`architecture/custom-character-workshop-ux.md`](architecture/custom-character-workshop-ux.md)
for the complete library/editor, Portrait Studio, donor-profile, vehicle-test,
performance-assembly, preview and optional gameplay-mod UX plan.

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

### Rice packs, for contributors

A Rice pack loads at runtime and needs no tooling — see
[High-resolution texture packs](#high-resolution-texture-packs) for the player
route. What follows is how that works and what is still offline.

`platform/rice_crc.c` computes the pack's own texture identity from the texel
span the renderer is about to upload, and `mod_registry.c` indexes a pack's
filenames into `<crc, fmt, siz>` keys. The renderer asks for a replacement only
when an enabled Rice pack is installed, so a player without one pays nothing
per texture.

**The CRC is validated**, which it was not when this port was written. Two
independent lines of evidence, both against a real ROM and a real pack:

- Every format class shows a *partial* miss rate (0/2: 332 hit, 97 miss; 0/3:
  264/23; 3/1: 54/28). A wrong reading for a class scores exactly zero in it,
  so a partial rate in every class cannot come from a wrong CRC.
- Not one unmatched texture matches any pack key under any of the eight
  computed variants. A sibling variant would rescue them if ours were wrong.

`rice_crc_parity` holds `platform/rice_crc.c` to vectors generated from
`tools/ricepack/rice_crc.py`, so the C and the Python cannot drift apart — a
drift that would otherwise stop shipped packs resolving while the offline tool
kept working, which is very hard to see from either side alone.

To re-run the measurement that settled it:

```sh
python3 tools/ricepack/measure_crc_variants.py \
    --build build --rom baserom.us.v80.z64 \
    --pack /path/to/rice-pack --out ~/dkr-crc-experiment
```

It drives several routes, computes every candidate over each dumped span, and
prints how many of the pack's keys each one matched. A wrong variant scores
zero, so the table is read directly rather than interpreted. This is also the
right tool for the opposite question: if a pack's coverage is ever disputed, a
variant that rescues the unmatched textures means our CRC is wrong, and no
variant rescuing them means the pack lacks them.

**`tools/ricepack/import_rice_pack.py` still exists but is no longer required.**
It converts a Rice pack into a digest-keyed content pack, which needs a
crosswalk built from a texture dump taken from a running game. That was the only
route before the runtime path existed. Keep it for building a pack that mixes
Rice art with digest-keyed replacements; reach for nothing at all to simply play
with a Rice pack.

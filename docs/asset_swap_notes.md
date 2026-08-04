# Asset endianness-normalization notes (`platform/asset_swap.c` / `.h`)

mdkr64 milestone **M2-swap**. This documents the byteswap layer that converts
DKR's big-endian (N64) asset data to host-native order at the asset-load
boundary, per ARCHITECTURE_DECISIONS.md decision #7.

Scope of this module: `platform/asset_swap.c`, `platform/asset_swap.h`, this
doc. It defines the swappers + the integration contract only; wiring the hook
calls into `game/src/asset_loading.c` and the decompressing callers is done by
the loader owner (see "Integration contract" below and the header comment).

---

## Design decisions

1. **Explicit on-disk offsets, not host structs.** The swappers address raw byte
   offsets rather than casting to the game's C structs. On a 64-bit host the
   game structs have **8-byte** pointer fields, but the on-disk records use
   **4-byte** BE offset/pointer fields (N64 layout). Casting host structs onto
   the ROM bytes would therefore mis-locate every field past the first pointer.
   The on-disk layout is taken from `docs/ref/asset_fileTypes/*.hpp` (the
   authoritative binary spec) and cross-checked against `game/include/structs.h`
   (what the game actually reads) and the parsing code.

2. **The hook runs on the RAW image, before offset→pointer patching.** Callers
   like `object_models.c`/`tracks.c`/`objects.c` add a base pointer to each
   on-disk offset field (e.g. `objMdl->textures = (s32)objMdl->textures +
   base`). The swap must run *before* that so the game reads a native offset.

3. **Gzip: swap AFTER inflate.** Compressed sections (`OBJECT_MODELS`,
   `OBJECT_ANIMATIONS`, `LEVEL_MODELS`, `LEVEL_OBJECT_MAPS`, compressed
   `TEXTURES_*`) are swapped on the decompressed bytes. The gzip stream's own
   little-endian length word is handled by game code (`gzip.c` `byteswap32`),
   not by this layer.

4. **Texel payloads stay unswapped.** N64 texel formats (RGBA16/RGBA32/IA/I/CI)
   are byte-order-defined; mgb64's fast3d texture uploader
   (`mgb64/src/platform/fast3d/gfx_pc.c`) consumes them as raw bytes. Only the
   `TextureHeader` in front of the texels is swapped. CI4/CI8 TLUT palettes
   (`ASSET_EMPTY_14`) are RGBA16 texels and are likewise left untouched.
   *Verify point — CLOSED 2026-07-30, no swap needed.* `dkr_dp_load_tlut()` in
   `platform/fast3d/gfx_pc_dkr.c` assembles each palette entry from raw bytes
   MSB-first (`pal[i] = (src[i*2] << 8) | src[i*2+1]`), never through a
   `uint16_t*` cast, so it is host-endianness-independent — the identical
   pattern the same file uses for RGBA16/IA16 texels. `ASSET_EMPTY_14` stays a
   no-op, and that is correct rather than merely untested.

5. **No embedded display lists in assets.** DKR does **not** store raw `Gfx`
   command lists in any asset. Geometry is stored as `Vertex`/`Triangle`/
   `TriangleBatchInfo` arrays and turned into F3DDKR command lists at runtime
   (`rcp_dkr.c`); each `TextureHeader.cmd` list is built at runtime by
   `material_init`. So there are no two-word `Gfx` commands in asset payloads to
   u32-swap. (the architecture decisions' generic "DL words get swapped" note is about the HLE's
   runtime input, which is already native — it does not apply to on-disk data.)

6. **Union fields (bytes vs word).** `Triangle`'s first 4 bytes (flags,vi0,vi1,
   vi2) and `TexCoords`'s `u32 texCoords` union are only ever written as a word
   by *runtime-built* models (`object_functions.c` flag model). Disk-loaded
   triangles are read as bytes (`verticesArray`, `.flags`) and s16 (`uv.u/.v`),
   so the index bytes stay unswapped and each UV s16 is swapped. Same reasoning
   keeps `ColourRGBA` (particle colour) as bytes.

---

## Per-asset-type coverage table

Enum = `AssetSectionsEnum` in `game/include/asset_enums.h`.
Status: **Full** / **Partial** / **None (byte data)** / **Punt**.

| Enum | Status | Swapper / rule | Layout source |
|------|--------|----------------|---------------|
| `ASSET_AI_BEHAVIOUR` | Full¶ | array of `AIBehaviourTable` (2×f32 + s8[16]) | structs `AIBehaviourTable` |
| `ASSET_AI_BEHAVIOUR_TABLE` | Full | u32 offset table | loader |
| `ASSET_TEXTURES_3D` | Full† | per-frame `swap_texture_header()` at `load_texture()`; texels kept | texture.hpp / structs `TextureHeader` |
| `ASSET_TEXTURES_3D_TABLE` | Full | u32 offset table | loader |
| `ASSET_TEXTURES_2D` | Full† | as above | as above |
| `ASSET_TEXTURES_2D_TABLE` | Full | u32 offset table | loader |
| `ASSET_GAME_TEXT` | None | textbox/dialog command byte stream | gameText.hpp |
| `ASSET_GAME_TEXT_TABLE` | Full | u32 entries (hi byte = flag, lo 24 = offset) | game_text.c |
| `ASSET_MENU_TEXT` | None | ASCII strings | spec |
| `ASSET_MENU_TEXT_TABLE` | Full | u32 offset table | loader |
| `ASSET_SCREENS` | None | 16B header + RGBA16 texels (unused in vanilla) | screen_asset.c |
| `ASSET_SCREENS_TABLE` | Full | u32 offset table | loader |
| `ASSET_SPRITES` | Full | `SpriteAsset` head (4×s16 + s32); frame bytes kept | sprite.hpp / structs |
| `ASSET_SPRITES_TABLE` | Full | u32 offset table | loader |
| `ASSET_EMPTY_14` | None✔ | CI4/CI8 TLUT palette texels — **verified correct**, HLE reads MSB-first bytes | textures_sprites.c / gfx_pc_dkr.c |
| `ASSET_MISC` | **Partial** | per-sub-asset lists + on-demand converters (see below) | misc.hpp (partial) |
| `ASSET_MISC_TABLE` | Full | u32 offset table | loader |
| `ASSET_HUD_ELEMENT_IDS` | Full | s16 array | game_ui.c (`s16*`) |
| `ASSET_MENU_ELEMENT_IDS` | Full | s16 array | menu.c (`s16*`) |
| `ASSET_WEATHER_PARTICLES` | Full | s32 table (`gWeatherAssetTable`) | weather.c |
| `ASSET_LEVEL_OBJECT_MAPS_TABLE` | Full | u32 offset table | loader |
| `ASSET_LEVEL_OBJECT_MAPS` | **Partial** | 16B header `fileSize` + per-entry `x/y/z`; body punted | levelObjectMap.hpp + level_object_entries.h |
| `ASSET_LEVEL_HEADERS_TABLE` | Full | u32 offset table | loader |
| `ASSET_LEVEL_HEADERS` | Full | `LevelHeader` field-by-field (0xC4/0xC8) | structs `LevelHeader` |
| `ASSET_LEVEL_NAMES` | None | ASCII strings | spec |
| `ASSET_LEVEL_NAMES_TABLE` | Full | u32 offset table | loader |
| `ASSET_LEVEL_MODELS_TABLE` | Full | u32 offset table | loader |
| `ASSET_LEVEL_MODELS` | Full‡ | `LevelModel` hdr + segments + vtx/tri/batch/texinfo/bbox | structs `LevelModel`/`LevelModelSegment` |
| `ASSET_OBJECT_MODELS_TABLE` | Full | u32 offset table | loader |
| `ASSET_OBJECT_MODELS` | Full | `ObjectModel` hdr + texinfo/vtx/tri/batch/attach/spheres/animIdx | objectModel.hpp / structs |
| `ASSET_ANIMATION_IDS` | Full | s16 array | object_models.c (`s16*`) |
| `ASSET_OBJECT_ANIMATIONS_TABLE` | Full | u32 offset table | loader |
| `ASSET_OBJECT_ANIMATIONS` | Full§ | keyframeCount + frame-0 header/delta table + every keyframe header | hasm_native/obj_animate.c (asm) |
| `ASSET_OBJECT_HEADERS_TABLE` | Full | u32 offset table | loader |
| `ASSET_OBJECTS` | Full | `ObjectHeader` (0x78) + modelIds/vehParts/particles/`unk24` | objectHeader.hpp / structs |
| `ASSET_LEVEL_OBJECT_TRANSLATION_TABLE` | Full | s16 array (512 slots) | levelObjectTranslationTable.hpp |
| `ASSET_EMPTY_37` / `_TABLE` | None/Full | empty section / table | — |
| `ASSET_AUDIO` | Full✔ | BE parsed explicitly by `bnkf.c` under `NATIVE_PORT` (not this module) | bnkf.c / audio.c |
| `ASSET_AUDIO_TABLE` | Full | u32 offset table | loader |
| `ASSET_PARTICLES_TABLE` | Full | u32 offset table | loader |
| `ASSET_PARTICLES` | Full | array of `ParticleDescriptor` (0x18) | particles.h |
| `ASSET_PARTICLE_BEHAVIORS_TABLE` | Full | u32 offset table | loader |
| `ASSET_PARTICLE_BEHAVIORS` | Full | array of `ParticleBehaviour` (0xA0) | particles.h |
| `ASSET_FONTS` | Full | u32 count + `FontFile`[0x400] (4×s16 + s16[32]) | fonts.hpp / font.h |
| `ASSET_JAPANESE_FONTS` / `_TABLE` | **Punt** | not used by us_v80 (REGION ≠ JP) | jpFonts.hpp |
| `ASSET_DUMMY_PARTICLE_IDS` | Full | s16 array | particles.c (`s16*`) |
| `ASSET_TTGHOSTS_TABLE` | Full | `{u8 mapId, u8 vehId, pad2, s32 offset}` — **NOT** a u32 LUT | objects.h `TTGhostTable` |
| `ASSET_TTGHOSTS` | Full¶ | `GhostHeader` + `GhostNode`[] (all s16) | ttGhost.hpp |

† Textures: Full. `load_texture()` walks `numberOfTextures` frames itself and
calls the exported `swap_texture_header()` per frame (including the
`textureSize == 0` "write-size false" case), so the `swap_texture()` case in the
dispatch is never reached — `ASSET_TEXTURES_*` never go through
`asset_table_load()`. Kept as a defensive no-op.
¶ **Was unreachable until the 2026-07-30 audit.** The swapper existed and this
table said "Full", but the asset is only ever loaded through the raw
`asset_load()` DMA, which does not run the normalize hook — so it was consumed
big-endian in every release. Now swapped at its call site
(`game.c aitable_init` / `racer.c load_tt_ghost`). This is exactly the failure
mode `tests/check_asset_swap_invariants.py` ARM 2 now prevents.
✔ Verified by the 2026-07-30 audit; see "Open questions / verify points".
‡ Level model: the **BSP-tree node array** is not swapped (node count is not in
the header). See "Open questions".
§ Object animations: Full, but via the dedicated
`asset_swap_object_animation(data, size, numAnimatedVertices)` entry point — the
blob does not carry its own animated-vertex count, so the generic
`asset_swap_normalize(ASSET_OBJECT_ANIMATIONS, …)` path only swaps the leading
`keyframeCount`. The integration hook in `object_models.c` must call the
dedicated function with `model->numberOfAnimatedVertices` (see below).

---

## `structs.h` vs `.hpp` discrepancies (structs.h wins — it's what runs)

- **LevelHeader 0xA0**: levelHeader.hpp declares `be_int16 unkA0`; `structs.h`
  splits 0xA0/0xA1 into two `u8` (`unkA0`,`unkA1`) which `tracks.c` reads
  separately. → treated as bytes, **no swap** at 0xA0.
- **LevelHeader 0x20 (`AILevelTable`)**: `structs.h` types it as `s8*` (pointer)
  but the game takes its **address** (`aitable_init(&hdr->AILevelTable)`) and
  reads it as an inline `s8[]` AI-level array (matches levelHeader.hpp's two
  `AiLevels` structs). → inline bytes, **no swap** for 0x20–0x33.
- **LevelHeader 0xC4 (`unkC4`)**: present in levelHeader.hpp (record size 0xC8),
  absent from `structs.h` (size 0xC4); the game never reads it. Swapped only if
  the loaded record is ≥ 0xC8 bytes (harmless).
- **ObjectModel/LevelModel/ObjectHeader pointer fields**: on disk 4-byte
  offsets; in `structs.h` typed as pointers. We swap the 4-byte on-disk values.
- **`ObjectModel.animatedVertexIndices` (0x4C)**: `structs.h` types it `s32*`,
  but the authoritative asm (`hasm_native/obj_animate.c`:
  `indices = (s16*)model->animatedVertexIndices; for i<nVerts: idx=indices[i]`)
  reads it as **`s16[numberOfVertices]`** — one entry per model vertex, `-1` for
  non-animated. The swapper follows the asm (`sw16` × `numberOfVertices`), not
  `structs.h`. Bound is also confirmed by `model_instance_init`'s scratch buffer
  (`mi->vertices[2]` = `numberOfAnimatedVertices*3` s16), which the `idx` values
  index into.

---

## Punts — detail

- **`ASSET_AUDIO` — NO LONGER A PUNT (closed 2026-07-30).** The sound/sequence
  banks are libultra-format `ALBankFile`/`ALSeqFile`, and audio is live and
  enabled by default. The conversion is implemented, but **outside this
  module**: the clean-room audio engine (`platform/audio_compat.c`) parses the
  bank image through explicit big-endian byte accessors and builds native host
  structs — which also solves the LP64 4-byte-offset-vs-8-byte-pointer problem
  rather than just the byte order. Covered there: bank/instrument/sound counts
  and offsets, envelope/keyMap/wavetable/book/loop pointers, ADPCM book
  order/npredictors and coefficients, loop start/end/count/state, and the
  sequence file's count and per-entry offset/len. Raw ADPCM sample bytes stay
  unswapped (byte-order-defined). The `case ASSET_AUDIO:` no-op in this
  module's dispatch is therefore correct as written — the audio engine never
  calls `asset_swap_normalize()` on audio data at all.

- **`ASSET_MISC`** — one section holding many unrelated sub-assets (indexed by
  `ASSET_MISC_*`): some are `s8`/`u8` byte arrays (e.g. boss-level maps read as
  `s8[]`), some are `s16`/`s32` id lists, some are structs
  (`TitleScreenDemos` = 3×s8, `CheatsTableEntry` = 2×be_int16 per misc.hpp).
  A single blanket rule is unsafe, so each sub-asset gets the swap kind its
  layout requires.
  Sub-assets are normalized **once, at MISC-section load**, from the explicit lists
  in `objects.c dkr_misc_normalize_tables()` (called from `allocate_object_pools()`).
  Each sub-asset gets exactly the swap kind its layout requires — the kinds are not
  interchangeable, and picking the wrong one is silently destructive:

  | kind | primitive | covers |
  |---|---|---|
  | 32-bit words | `dkr_misc_swap_words(index)` | `f32[]`/`s32[]` arrays |
  | 16-bit halfwords | `dkr_misc_swap_halfwords(index)` | `s16[]`/`u16[]` arrays |
  | per-field, in place | `dkr_misc_swap_shield_records()` | `SHIELD_DATA` (s16+f32 mix) |
  | per-field + stride copy | `asset_swap_misc_boost()` | `MISC_20` (boost record) |
  | on-demand, per record | `asset_swap_misc_lightdata()` | `LevelHeader_70` colour cycles |
  | on-demand, per record | `asset_swap_misc_pulsating()` | `PulsatingLightData` (added 2026-07-30) |
  | on-demand, bounded | `asset_swap_misc_magic_codes()` | `MAGIC_CODES` index block (added 2026-07-30) |

  A 32-bit swap on a 16-bit array *also transposes the two halfwords inside every
  word* (for `RUMBLE_DATA` that swaps strength with duration in each pair); a 16-bit
  swap on a float array byte-reverses each half of it. The shared `sMiscSwapDone[]`
  flag is per **index**, not per kind, so a sub-asset is swapped exactly once in
  exactly one way.

  - **32-bit word list:** `MISC_4` (per-character model scale), `MISC_8`
    (per-character steer-slide divisor), `RACER_WEIGHT`, `RACER_HANDLING`,
    `RACER_UNUSED_11`, `MISC_17`, `MISC_18`, `MAGNET_DATA`, `MISC_32` (stone grip),
    the 13 `RACERACCELERATION_*` curves (`ObjectHeader.unk5C`), `MISC_51..55`
    (wheel-collision points, `ObjectHeader.unk5D`) and `RACER_HITBOX_SIZE`.
  - **16-bit halfword list:** `RUMBLE_DATA` (19) — `u16[38]` = 19
    `{strength, duration}` pairs (`save_data.c`); `MISC_23` (23) — `u16[576]` = 48
    levels × 3 save files × `{courseTime, initials, flapTime, initials}` defaults
    (`thread3_main.c clear_lap_records`); `GHOST_UNLOCK_TIMES` (24) — `u16[20]`
    staff times, pairing 1:1 with the 20 `MAIN_TRACKS_IDS` entries before the `-1`
    terminator (`objects.c`).
  - **`SHIELD_DATA` (21)** — `RacerShieldGfx[30]` (3 vehicles × 10 characters),
    `4×s16 + 2×f32` per 16-byte record. Needs a per-field swizzle, but **no** stride
    conversion: the record has no trailing pointer fields, so
    `sizeof(RacerShieldGfx) == 0x10 ==` the on-disk stride on N64 and LP64 alike
    (locked with `_Static_assert`s). Swizzled in place.
  - `LevelHeader_70` pulsating-light records — `asset_swap_misc_lightdata(blob)`.
  - **sub-asset 20, the boost/exhaust table** — `asset_swap_misc_boost()`. Needs a
    *per-field* swizzle (it packs `s16 spriteId; s16 textureId` at 0x6C and four
    `u8`/`s8` at 0x70, which `dkr_misc_swap_words` would scramble) **and** a
    host-layout copy: the on-disk stride is 0x80 but `sizeof(Object_Boost)` is 0x88
    on LP64, because the record's last two fields are runtime pointers. This is the
    only sub-asset that cannot be normalized in place. See the block comment on the
    implementation and `docs/OPEN_ITEMS.md`.

  **Prefer extending a list over adding a lazy call site.** The opt-in-per-consumer
  approach is exactly how `MISC_8` stayed big-endian for three waves and silently
  flung the player racer out of the world (docs/OPEN_ITEMS.md).

  **Give every new entry a plausibility bound** in `dkr_misc_verify_tables()`. Four
  of the tables above are on paths no fixture reaches (the shield needs a racer
  holding a shield, `RUMBLE_DATA` needs a rumble pak, `MISC_23`/`GHOST_UNLOCK_TIMES`
  sit on save-file and time-trial-record paths), so a green regression matrix says
  nothing about them — the bound is the only thing between a wrong swap and a silent
  wrong table. Bounds are chosen so the correct decode clears them with room to
  spare and the byte-reversed decode violates them outright; all four are verified
  to abort when their swap is removed. Use `tools/dump_misc_asset.py` to classify a
  new sub-asset and to check the decode independently of the running game.

  **Closed by the 2026-07-30 audit:**
  - `MAGIC_CODES` (65) — the cheat table, read as `u16(*)[30]` by `menu.c`. It is
    a MIXED blob: a u16 index block (`numberOfCheats`, then 3 byte-offsets per
    cheat) followed by raw ASCII strings, so a blanket halfword swap would
    reverse every string. `asset_swap_misc_magic_codes()` swaps only the index
    block, called AFTER `decrypt_magic_codes()`. The cipher transposes bit pairs
    across each four-byte group, so it does **not** commute with a halfword swap;
    the plaintext must exist first. Before: `numberOfCheats` 7424, first
    string offset 45056 in a 1520-byte blob. After: 29 and 176. The identity
    `firstOffset == (1 + numberOfCheats*3) * 2` pins the field map and the
    converter reverts itself if it does not hold.
  - `PulsatingLightData` (reached via `LevelHeader.pulseLightData`; us.v80 uses
    sub-asset 64 for Spaceport Alpha and Star City) — every field multi-byte.
    Before: `numberFrames` 1024 in a 28-byte blob. After: 4, frames
    (254,30) (0,12) (254,12) (0,30). Drives `outColorValue`, the PrimColor for
    `RENDER_PULSING_LIGHTS` batches.

  Still unclassified: the remaining `s16`/`s32` id lists and `CheatsTableEntry`
  (2×`be_int16` per misc.hpp), which are consumed as bytes or not at all today,
  plus the colour-loop sub-assets 58/59/60 — see "Residual risks".

- **`ASSET_LEVEL_OBJECT_MAPS` body** — we swap the file header and each entry's
  common `x/y/z` (s16). Per-behavior body params (rotations, misc s16/s32,
  starting at entry+0x08) vary per object type (dozens of structs in
  `level_object_entries.h`) and are punted. The entry walk is byte-driven
  (`entry[1] & 0x3F`), so this is safe (no crash); object rotations/params will
  be wrong until covered. **To cover:** dispatch on `objectID` to a per-type
  body swapper.

---

## Object-animation blob layout (`asset_swap_object_animation`)

Authoritative source: `game/src/hasm_native/obj_animate.c` (traced from the
shipping asm `game/src/hasm/obj_animate.s`). `model_anim_init` strips a leading
`s32` count and advances `animData` past it (`anim++`), so obj_animate's
`animData` == blob + 4. Layout, **blob-relative** (as the swapper sees it):

```
0x00                      s32  keyframeCount (== animLength)          [swap]
0x04                      12B  frame-0 offset header:
                               +0x0 offsetX  +0x2 offsetY  +0x4 offsetZ
                               +0xA headTilt  (s16, swap; +0x6..+0x9 unused)
0x10                      frame-0 delta table: 3 * nAnimVerts s16     [swap]
                               (indexed by animated-vertex slot; the bound is the
                                mi->vertices[2] scratch = nAnimVerts*3 s16)
keyframe blocks, keyframeSize = nAnimVerts*3 + 12 each:
  header m at blob (keyframeSize*m - 8), m = 2 .. animLength:
                               12B header, s16 at +0x0/+0x2/+0x4/+0xA  [swap]
  s8 delta block at blob (keyframeSize*m + 4): nAnimVerts*3 s8 bytes   [NO swap]
```

Notes:
- `nAnimVerts` = `ObjectModel.numberOfAnimatedVertices`, which the blob does not
  store — hence the dedicated API taking it as an argument.
- Headers start at **m = 2**. obj_animate never reads header `m = 1` (its
  smallest keyframe-header index is 2); `keyframeSize*1 - 8` would alias the
  frame-0 delta table, so swapping it would double-swap those s16 values. The
  frame-0 table ends exactly where header m=2 begins (`0x10 + nAnimVerts*6`).
- The per-animated-vertex delta blocks are single s8 bytes (x,y,z) and are left
  untouched; only the frame-0 full-precision deltas and the offset headers are
  s16. Validated by the extended synthetic self-test (nAnimVerts=2, animLength=4:
  frame-0 header/table + headers m=2..4, s8 blocks confirmed untouched).

---

## Open questions / verify points — all five now closed

> **Closed.** The five questions below were open when this note was written.
> Each has since been answered with measurement, and the answers are recorded
> in [`open-items/portability.md`](open-items/portability.md) (see the
> asset-normalisation closeout list). They are kept here, unedited, because the
> reasoning that raised them is the reasoning anyone extending the normaliser
> needs. **Answers in brief:** (1) only single-frame `write-size == false`
> assets may encode a zero stride, and the walk now rejects a zero/invalid one;
> (2) the BSP node array is swapped, as of M4; (3) `LevelModelSegment.unk8` is
> normalised as a 32-bit value and no production read of it exists;
> (4) `ObjHeaderParticleEntry` is definitively two `s32` words, size 8, locked
> by the object-layout contract; (5) `ASSET_EMPTY_14` stays byte-order-defined
> deliberately — the HLE reconstructs each TLUT entry MSB-first from raw bytes.


Every item below was closed by the field-level asset-swap audit. Answers and
evidence follow; nothing here is still
open. New findings that the audit could NOT close are in "Residual risks".

1. **Animated (multi-frame) textures.** *CLOSED — not a gap; the generic
   `swap_texture()` is dead code.* `ASSET_TEXTURES_2D/_3D` never reach
   `asset_table_load()`, so the dispatch case never fires. `load_texture()`
   (`textures_sprites.c`) does the real work: it walks `numberOfTextures`
   frames itself and calls the exported `swap_texture_header()` per frame, and
   it already handles the `textureSize == 0` ("write-size false") case
   explicitly by taking the decoded payload boundary as the material cursor.
   So the walk that "can't advance" was never the shipping walk. Kept in the
   dispatch as a defensive no-op.
2. **Level-model BSP tree.** *CLOSED — covered, but at the caller.*
   `tracks.c` (`generate_track`, under `NATIVE_PORT`) swaps
   `leftNode`/`rightNode`/`splitValue` for `numberOfSegments - 1` nodes right
   after `LOCAL_OFFSET_TO_RAM_ADDRESS`. `asset_swap.c` deliberately skips it
   (the count is not in the header), so it is swapped **exactly once**. No
   double-swap hazard. The `N-1` node count is a structural claim about a
   binary tree over N segments — see "Residual risks".
3. **Level-model segment scratch fields.** *CLOSED as documented.* The
   classification stands; `unk8` remains a defensive `s32` swap with no
   confirmed disk read. No consumer was found that reads it before it is
   overwritten, so the swap is inert either way.
4. **`ObjHeaderParticleEntry`.** *CLOSED — s32 form confirmed.* The swap is
   `s32` pairs and the game reads the `s32` form; the `.hpp` union's `2×s16`
   view has no reader.
5. **`ASSET_EMPTY_14` TLUT.** *CLOSED — no swap needed; current no-op is
   correct.* `gCiPalettes` is a `u8*` buffer and the DKR HLE reads palette
   entries byte-at-a-time, MSB-first, in `dkr_dp_load_tlut()`
   (`platform/fast3d/gfx_pc_dkr.c`): `pal[i] = (src[i*2] << 8) | src[i*2+1]`.
   That is byte-order-defined and host-independent — never a `uint16_t*` cast.
   It is the identical pattern used for RGBA16/IA16 *texels* in the same file,
   so palettes and texels are consistent. `ciPaletteOffset` (a byte offset into
   the palette heap, not colour data) is correctly swapped as an ordinary s16
   in the texture header. Also observed: the CI path is implemented but
   empirically unexercised — no CI4/CI8 texture was loaded on any audited route.
6. **Object-animation caller wiring.** *CLOSED — correct.* `object_models.c`
   calls `asset_swap_object_animation(anim, size, model->numberOfAnimatedVertices)`
   after `gzip_inflate`. `ASSET_OBJECT_ANIMATIONS` never reaches
   `asset_table_load()`, so the generic `keyframeCount`-only case does not also
   run: no double swap.

### `ASSET_AUDIO` — heterogeneous, with typed ownership

The punt below was written when audio was stubbed to silence for bring-up.
Audio is live and on by default now (M5), but the section still cannot be sent
through one blanket swap: it mixes pointer-bearing bank images, byte streams,
small typed tables, and raw samples. Ownership is therefore per consumer:

- `platform/audio_compat.c` implements `alBnkfNew`/`alSeqFileNewFrom` with
  explicit `bank_ctl_u16/s16/u32/s32` reads and builds native host structs,
  which also solves the LP64 4-byte-offset problem.
- `SoundData.soundBite`/`.range`, the custom-FX s32 array, and the compressed
  MIDI header convert at their load sites. `MusicData` is three `u8` fields.
- Raw ADPCM/RAW16/event bytes retain their format-defined representation until
  their typed decoder consumes them.
- The 30 `VehicleSoundAsset` records at `ASSET_AUDIO_7` mix u8 data with u16/s16
  fields. They are normalized by `asset_swap_vehicle_sound()` immediately after
  each raw record load.

That last owner was missing: normal kart sound ID 115 decoded as 29440, so the
main engine loop could not start, while its byte-sized idle ID remained valid.
The source-coverage arm now inventories all 16 textual `ASSET_AUDIO` raw-load
sites and fails if a new heterogeneous consumer is added without an audit. The
`case ASSET_AUDIO:` no-op in the generic dispatch remains correct because audio
data is never homogeneous enough for `asset_swap_normalize()`.

---

## Verification harness — BUILT (`tests/check_asset_swap_invariants.py`)

The sketch below was never built; it is now, as the `asset_swap_invariants`
gate in `tools/run_checks.py` (role `rom`). It has two arms.

**ARM 1 — ROM data invariants.** An *independent* decoder (it re-implements the
LUT walk rather than calling port code, so agreement is cross-checking and not a
tautology) reads every swapped asset type out of the retail ROM and asserts
spec-derived invariants: enum fields in range, offsets inside their blob,
self-describing size identities, counts consistent with payload length.

Every invariant carries a **positive control**: the same field decoded with its
bytes reversed must *violate* the bound. This is the part that matters. The wave
bug survived a year because every oracle it faced was equally satisfied by the
wrong data, so a bound that both decodes pass is worse than no bound — it looks
like coverage. The gate therefore tallies each control as *discriminating* or
*value-limited* (some authored values are genuinely too small to tell apart:
`waveSineHeight0 == 512` reverses to 2 and both sit inside 0..4963) and asserts
that **every asset type has at least one discriminating control**.

Current: 113 field checks, 491 controls, 490 discriminating, 1 value-limited.

The strongest controls are the self-describing identities, because they tie a
header field to the blob's own length and no reversed value can satisfy them:

- TT ghost: `align16(12 * nodeCount + 8) == blobSize` (all 20 ghosts)
- magic codes: first string offset `== (1 + numberOfCheats*3) * 2` (`== 176`)
- pulsating light: `numberFrames <= (blobSize - 0x0C) / 4`

**ARM 2 — source coverage.** `asset_load()` is a raw ROM DMA; only
`asset_table_load()` runs the `asset_swap_normalize()` hook. So every
`asset_load(ASSET_X, …)` site owes a swap, and a swapper that exists in the
dispatch but is never reached is dead code masquerading as coverage. Both
defects this gate was written for were exactly that shape. Arm 2 holds every raw
`asset_load()` site against an explicit disposition table (`swap` / `bytes` /
`own`), so a new site cannot appear undeclared and an existing one cannot
silently lose its swap. Verified by negative control: deleting the
`ASSET_AI_BEHAVIOUR` swap fails the gate.

Two bounds had to be **re-derived from the retail corpus** because `structs.h`'s
inline comments are wrong — documented in the gate, not loosened:
`wavePower` is commented "always 256" but is authored `{128, 207, 256}`, and
`race_type` is a sparse enum (challenge modes at 64/65/66), not a dense range.

---

## Load-bearing misread: the AI table drives autopilot, and three gates baked it

The `ASSET_AI_BEHAVIOUR` fix is the one place where correcting the data moved
existing gates. Recorded here in full because the movement is real, expected,
and must not be mistaken for a regression by the next person who sees it red.

**Mechanism.** The two `AIBehaviourTable` f32 feed `racer->unk124` in
`func_80042D20()` — DKR's AI throttle/behaviour routine. Every headless fixture
runs `MDKR_AUTOPILOT=1`, and autopilot drives the test racer *through that same
routine*, so the AI table governs the test racer's speed. With the table finally
decoding correctly the racer travels ~2.7% slower once the race starts
(displacement over the 24-frame capture window: 540.1 -> 525.5). The traces are
bit-identical up to frame **2864** and diverge exactly there, which is where
`gRaceStartTimer == 0` first lets `func_80042D20` read the table.

**Attribution is proven, not inferred.** A build of this branch with *only* the
`ASSET_AI_BEHAVIOUR` swap reverted reproduces the parent's numbers exactly
(`moved=516.0`, `activeTrackPixels=6340`, 5.33%/5.16%, PASS). No other fix in
this branch moves any gate.

| gate | parent | this branch |
|---|---|---|
| `mip_motion` | 5.33% / 5.16% PASS | 4.61% / 4.93% |
| `world_fx_capture` | PASS | `matrices=8` (needs `> 10`) |
| `world_shadows` | PASS (21713 px) | handoff `changed=35526` |

**Scope of the movement is narrow.** 65 of the 70 completed gates pass,
including 17 autopilot-driven ones — and critically including `world_fx_matrix`
and `shadow_visual_ab`, which drive the *same* `nav_to_time_trial_race` route,
plus `render_purity`, `determinism`, `state_hash`, `track_sweep` and
`vehicle_sweep`. Only gates that sample a *fixed frame index* through a *fixed
screen crop* are affected.

**One more gate moved later, for a different reason (release battery, v1.0.0).**
`first_boss_progression` is not a fixed-frame sampler: its Adventure route
*wipes out*. On the corrected table the fourth Dino race diverges at Hot Top
Volcano checkpoint 4, leaves the course at the crater jump around frame 3025,
and wedges the kart at (-2139.4, -120.7, 1498.9) for the remaining 18,677
frames, so the route never reaches Tricky 1 and every downstream persistence
assertion fails for want of progress. Same attribution method: with only this
swap reverted, the identical route finishes the boss and persists every flag.
The kart cannot recover because `racer_AI_pathing_inputs()`'s reverse-out is
gated on `unk215`, a cooldown only decremented while the kart drives forwards
(unchanged from the decomp baseline, so left alone in production); the fixture
now re-arms that cooldown through `MDKR_AUTOPILOT_UNSTICK` once the kart is
provably immobile, and fails if the recovery ever fires inside Tricky 1.

**Why this was NOT re-baselined here.** The obvious move is to shift
`CAPTURE_FROM` so the crop sees an equivalent scene. Measuring that first shows
why it would be dishonest — the metric is not stable across nearby windows:

| `CAPTURE_FROM` | activeTrackPixels | mip benefit |
|---|---|---|
| 3300 | 10395 | 6.19% |
| 3380 | 11627 | 15.65% |
| 3476 (current) | 4709 | 4.61% |
| 3560 | 10877 | 3.52% |
| 3640 | 924 | 1.87% |
| 3720 | 2826 | 9.16% |

An 8x spread across neighbouring windows means the 5% threshold describes one
hand-picked frame, not a robust renderer property — the parent cleared it by
only 0.33pp. Any window chosen now would be chosen *because it passes*, which is
shopping for green dressed up as a re-baseline. The same applies to
`world_fx_capture`'s `matrices > 10` liveness bound (it reads 8 at this camera
position) and to `world_shadows`' pixel-count handoff.

**Recommended fix, for the gate owner rather than this audit:** make the
measurement scene-independent — average the benefit over several capture windows,
or select the window by the gate's own `activeTrackPixels` criterion and state
that criterion in the gate — then re-derive the threshold against the corrected
simulation. That is a gate redesign, not a byte-swap decision, so it is reported
here rather than guessed at. The byte-swap fix itself is not in question: the
authored ramp (-6,-5,-4,-3,-1.5,-0.5,0,2,5,7) is unambiguously the real data and
the pre-fix denormals unambiguously were not.

---

## Residual risks (found by the audit, deliberately NOT fixed)

- **Colour-loop / `LevelHeader_70` aliasing.** `ParticleBehaviour.colourLoop`
  resolves to MISC sub-assets 58/59/60 and is read as `ColorLoopEntry[]`
  (`s32 numEntries` + RGBA, 8-byte stride). Left big-endian, `numEntries`
  decodes to `0x04000000` instead of 4, defeating the
  `colourIndex >= numEntries` bound before `colourLoop[colourIndex + 2]` — a
  potential out-of-bounds read, confirmed against the ROM. **But** sub-asset 58
  is *also* read as a `LevelHeader_70` by `game_ui.c` (`ASSET_MISC_58`) and
  normalized by `asset_swap_misc_lightdata()`, and the two struct shapes
  disagree about which words are `s32`: the lightdata view swaps 0x04/0x0C and
  treats 0x10/0x14 as RGBA bytes, while the colour-loop view needs 0x10 as an
  `s32` and 0x04/0x0C as bytes. The views are mutually destructive, and which
  one runs first is initialization-order dependent. Fixing this needs a decision
  about what sub-asset 58 actually *is*, which is beyond an endianness audit.
  The gate **reports** the mismatch and deliberately does not enforce a bound
  (enforcing one would encode a decision this audit left open).
- **`ObjectHeader.unk24` light array.** `objects.c` indexes
  `unk24[i]` for `i < numLightSources`, but `swap_object_header()` normalizes
  only record 0. This is inert today: **every one of the 304 retail object
  headers declares `numLightSources == 0`**, and `unk24` is never an in-bounds
  0x18 record (49 headers point exactly at `fileSize`, 255 past it), so the
  swap's `in_bounds` guard already makes it a no-op. The gate asserts
  `numLightSources == 0` for all headers, so a ROM or edit that populates the
  array trips the gate instead of shipping big-endian records.
- **BSP node count.** `tracks.c` swaps `numberOfSegments - 1` BSP nodes. That is
  the correct count for a binary tree partitioning N segments, but it is a
  structural inference, not a value read from the asset. Not re-derived here.

---

## Verification harness sketch (superseded — kept for provenance)

The original sketch proposed mmap'ing the ROM, resolving
`__ASSETS_LUT_START`/`__ASSETS_LUT_END`, walking `lut[0]` = section count and
`lut[1+i]` = section offset, slicing `entry[levelId]..entry[levelId+1]` out of a
`*_TABLE`, calling `asset_swap_normalize()` and asserting known-good values for
level 0 (`course_height` a sane float, `laps` in [1,6], `race_type` a valid
`RaceType`, `cameraFOV` in (0,90], `world` a valid `World`), then repeating for
`ASSET_OBJECT_MODELS` after inflate. `tests/check_asset_swap_invariants.py`
implements all of that and extends it with the positive controls and the source
coverage arm, across every level header rather than just level 0.

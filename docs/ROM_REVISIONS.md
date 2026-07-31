# ROM revisions: what works, what does not, and exactly how it fails

**Short answer:** the product scope is explicitly frozen to two of the five released
revisions: **us.v80** (US 1.1) and **pal.v80** (European 1.1). The other three
are recognized and deliberately refused. This is not an accidental compatibility
gap or an implicit promise that an unknown ROM might work: the accepted digest
set is enforced by `tests/check_rom_revision.py`.

With only the asset-lookup-table offset corrected, all three unsupported
revisions can finish a 3-lap Time Trial with the same course time. That is useful
diagnostic evidence, not a support claim. They remain unvalidated at the asset,
region, timing, audio, and reference-oracle boundaries described below.

Everything below was measured on an M3 Max against the five real ROMs. Every
run was headless and muted (`--headless-frames`, `MDKR_AUDIO=0`), per
[CONTRIBUTING.md](../CONTRIBUTING.md).

---

## 1. Inventory (verified, not assumed)

Five revisions, extracted to the git-ignored `build/roms/`. Each one's sha1 matches
the decompilation's own `ver/verification/dkr.<build>.sha1`, so these are the
reference dumps the decomp is built against — the strongest provenance available.

| decomp build | game code | ver | CRC1 / CRC2 | sha1 matches decomp |
|---|---|---|---|---|
| `us.v77`  | `NDYE` | 0x00 | `53D440E7` / `7519B011` | yes |
| `pal.v77` | `NDYP` | 0x00 | `FD73F775` / `9724755A` | yes |
| `jpn.v79` | `NDYJ` | 0x00 | `7435C9BB` / `39763CF4` | yes |
| `us.v80`  | `NDYE` | 0x01 | `E402430D` / `D2FCFC9D` | yes |
| `pal.v80` | `NDYP` | 0x01 | `596E145B` / `F7D9879F` | yes |

The expected header values come from the decomp's `src/hasm/header.s`, which emits
CRC1/CRC2, country code and revision per `VERSION_*`. All five real ROMs agree with
it exactly. That file — not folklore — is the authority for the table in
`platform/rom_id.c`.

Header layout used for identification: CRC1 `0x10`, CRC2 `0x14`, internal title
`0x20`, media format `0x3B` (`N`), cart id `0x3C` (`DY`), country `0x3E`, version
`0x3F` (= `revision | savetype << 4`; only the low nibble is compared, because the
decomp's own `NON_MATCHING` build sets savetype 1).

---

## 2. The asset lookup table is at a different ROM offset in every revision

On the N64 `__ASSETS_LUT_START` / `__ASSETS_LUT_END` are linker-generated. There is
no linker here, so the port carried them as constants. From the decomp's
per-version splat configs (`ver/splat/dkr.*.yaml`):

| build | `assets.lut` | `assets` base | ROM end |
|---|---|---|---|
| `us.v77`  | `0x0ECB60` | `0x0ECC30` | `0xAC9630` |
| `pal.v77` | `0x0ECBF0` | `0x0ECCC0` | `0xAC96C0` |
| `jpn.v79` | `0x0EE5D0` | `0x0EE6A0` | `0xB8E4E0` |
| `us.v80`  | `0x0ED0E0` | `0x0ED1B0` | `0xB8CFD0` |
| `pal.v80` | `0x0ED170` | `0x0ED240` | `0xB8D060` |

All five differ. `pal.v80` — the nearest neighbour — is `0x90` bytes away, i.e. the
LUT read lands 36 entries early, inside the preceding code/data.

---

## 3. Failure taxonomy A: the old gate (us.v80 offsets, every revision)

This is what a user got before `platform/rom_id.c` existed. Size + magic + "title
contains diddy" is true of all five revisions, so all five were accepted.

Every revision's LUT holds exactly 50 entries at its **own** offset. Read at
us.v80's offset instead:

| revision | entry count read | derived `ASSET_AUDIO_TABLE` size | outcome |
|---|---|---|---|
| `us.v80`  | 50 | 64 | correct |
| `us.v77`  | 3,886,268,156 | **−453,982,075** | SIGSEGV in `sw32_arr` |
| `pal.v77` | 830,361,298 | **−1,327,275,831** | SIGSEGV in `sw32_arr` |
| `pal.v80` | 0 | (unreached) | SIGSEGV in `audio_init` |
| `jpn.v79` | 0 | (unreached) | SIGSEGV in `audio_init` |

Two distinct mechanisms, both **before the first rendered frame**:

**v77 pair — negative size, then a walk off the allocation.**
```
sw32_arr <- asset_swap_lut <- asset_swape_normalize <- asset_table_load <- audio_init
```
`asset_table_load()` computes `size = LUT[i+1] - LUT[i]` from garbage, gets a large
negative number, allocates on it, and `asset_swap_lut()` byteswaps `size/4` words
straight off the end.

**pal.v80 / jpn.v79 — NULL, then an unchecked dereference.**
```
audio_init + 100
```
The entry count reads 0, so `asset_table_load()`'s own guard
(`if (gAssetsLookupTable[0] < assetIndex) return NULL;`) fires and returns NULL.
`audio_init()` then does `addrPtr = asset_table_load(ASSET_AUDIO_TABLE);` followed
immediately by `addrPtr[ASSET_AUDIO_2]` with no NULL check (`game/src/audio.c`).

Reproduce with `MDKR_ROM_ANY_REVISION=1` and the LUT re-pointing removed, or see
the positive control in §7.

---

## 4. Failure taxonomy B: with each revision's own LUT offsets

`platform/rom_io.c` now re-points `gDkrAssetsLutStart`/`gDkrAssetsLutEnd` from the
identified revision. With that one change, **nothing fails**:

| revision | boot (30f) | menus (1500f × 3 routes) | race (4300f) | full 3-lap Time Trial + EEPROM |
|---|---|---|---|---|
| `us.v80`  | ok | ok | ok | PASS, course time 4713 |
| `pal.v80` | ok | ok | ok | PASS, course time 4713 |
| `us.v77`  | ok | ok | ok | PASS, course time 4713 |
| `pal.v77` | ok | ok | ok | PASS, course time 4713 |
| `jpn.v79` | ok | ok | ok | PASS, course time 4713 |

No `[CRASH]`, no `[FATAL]`, exit 0 everywhere. Rendering was compared frame by
frame against us.v80 — every 25th frame over three text-heavy menu routes
(`nav_to_magic_codes`, `nav_to_options`, `nav_to_track_select`), 60 frames each:
**180/180 byte-identical for all four**, plus an identical racer trajectory at
frame 4300 (`x=-4453.1 y=29.0 z=-8714.0 clock=1183 cp=8`).

**Why this works at all:** the port does not execute ROM code. It runs its own
compiled C (`VERSION_us_v80` / `REGION_NA`) and uses the ROM purely as a data
source reached through the LUT. The asset payload is *structurally* identical
across revisions — same 50 sections, same asset types, same record layouts — so
the loader does not care that the content differs.

### An earlier claim, retracted

An earlier draft of `platform/rom_id.c` stated that correcting the offsets "would
NOT make another revision work" and "would be actively worse". That was a
prediction, it is contradicted by the table above, and it is withdrawn. The
prediction reasoned from the 423 compile-time `#if VERSION` / `#if REGION` sites in
`game/`, which is a real constraint on *building* for another revision — but this
port never builds another revision, it only *reads* another revision's data.

---

## 5. How much of the data actually differs

Per-section comparison, walking each revision's own LUT and hashing each of the 50
LUT-delimited sections against us.v80's:

| revision | sections identical | what differs |
|---|---|---|
| `pal.v80` | **50 / 50** | nothing — byte-identical payload |
| `jpn.v79` | 46 / 50 | `LEVEL_OBJECT_MAPS` (+table), `LEVEL_MODELS` (+table) |
| `us.v77`  | 26 / 50 | textures 2D/3D (+tables), `GAME_TEXT` (+table), `MENU_TEXT` (+table), `SPRITES`, `MISC` (+table), `MENU_ELEMENT_IDS`, `LEVEL_OBJECT_MAPS` (+table), `LEVEL_HEADERS`, `LEVEL_NAMES` (+table), `LEVEL_MODELS` (+table), `AUDIO`, `PARTICLE_BEHAVIORS`, `FONTS`, `JAPANESE_FONTS` (+table) |
| `pal.v77` | 26 / 50 | the same set — `us.v77` and `pal.v77` have byte-identical asset payloads |

**`pal.v80` is us.v80's data relocated by 0x90.** Its LUT is identical entry for
entry and all 50 sections hash the same. Running it is indistinguishable from
running us.v80 — which is why it is *supported*, not merely tolerated, and why
`tests/check_rom_revision.py` asserts byte-identical rendering for it rather than
just "it loaded".

`us.v80` is largely `us.v77` plus content: 343 `GAME_TEXT` entries against 259,
and an 823,296-byte `JAPANESE_FONTS` section against 70,272. Within the shared
range the entries are identical (`GAME_TEXT` first differs at index 256), which is
why the English NTSC screens render the same.

### A measurement that was wrong, and how

The first version of §5 compared the asset blobs at fixed byte offsets and reported
"2423 of 2525 sampled windows differ" for us.v77 — which would have meant almost
nothing was shared. That number is meaningless: the blobs have different *lengths*
(10,340,864 vs 11,140,640 bytes), so everything after the first size change is
shifted and compares unequal. The section-walk above is the correct metric. The
lesson is the one in DEVELOPER_HANDBOOK.md §3 — a plausible-looking number from the wrong
comparison is worse than no number.

---

## 6. Why three revisions are still refused

They pass every check that was run against them. They are refused anyway:

1. **Sub-entry indices are unbounded.** `asset_table_load()` bounds-checks the
   SECTION index (`gAssetsLookupTable[0] < assetIndex`). Nothing bounds-checks an
   index *within* a section. This build is compiled for v80 data and asks for v80
   indices; us.v77's `GAME_TEXT` has 259 entries where us.v80 has 343, so a
   v80-only string index on a v77 ROM is an out-of-bounds read with no guard and no
   diagnostic — precisely this project's silent failure class. **Auditing that is
   the bounded work between "passes our fixtures" and "supported."** It is
   plausible no fixture reaches such an index, which is exactly why "it passed" is
   not evidence.
2. **`jpn.v79` needs `REGION_JP`.** `font.c`, `game_text.c` and `save_data.c` all
   change behaviour for it (glyph handling, EEPROM layout) and we compile
   `REGION_NA`. The JP cart would run with the non-JP text path.
3. **No oracle parity, no audio.** There is no ares pixel-parity route
   (`docs/ORACLE.md`) for any non-us.v80 revision, and audio is unverified for all
   of them — `us.v77`'s `AUDIO` section differs and every check runs muted by the
   hard rule.
4. **PAL timing is not modelled.** The pacer synthesises 60 Hz NTSC fields. This
   does not affect `pal.v80` (the code is ours and its data is us.v80's), but a
   genuine 50 Hz PAL build would need it, and every calibrated number in `tests/`
   (lap ~1650 clocks, course time 4713, race start frame 3120) is NTSC-specific.

Things that turned out **not** to be blockers, having been checked:

- The four per-version `*Checksum` constants in `game/include/common.h` feed
  anti-tamper checks gated on `ANTI_TAMPER`, which this build does not define. Inert.
- `asset_loading.c` picks `dmacopy_v1` + a DMA mutex at `VERSION >= 79`. We are
  `VERSION_80` so we always take it; on native the mutex is a no-op ring buffer.
- `cheatmenu_checksum()` (`menu.c`) sums ROM `0x1000..__ROM_END` and *prints*
  the result on the cheat screen. It gates nothing. The native loader now
  validates and selects that linker-generated end from the same per-revision
  descriptor as the asset LUT, so both supported revisions use their own extent.

### Future scope expansion: the remaining three

Adding **us.v77**, **pal.v77**, or **jpn.v79** is a future product-scope
expansion, not a foundation blocker for the current two-revision port. It must
not be enabled by changing one acceptance boolean. In order, cheapest first:

1. Audit sub-entry index bounds for the sections that differ (§5) — or add a bounds
   check to the sub-entry accessors, which is cheaper than auditing and turns a
   silent OOB read into a loud abort.
2. Build `jpn.v79` as a separate binary with `-DVERSION_jpn_v79`. `VERSION_us_v80`
   is already just a compile definition, so a second build directory plus a
   per-version asset-offset default is most of the work, and it sidesteps the 423
   compile-time gates entirely. Runtime dispatch across those gates is the
   expensive alternative and buys little.
3. Oracle routes + audio verification per revision.

Until those gates exist, the runtime must continue to identify each digest by
name and refuse it before asset mutation.

---

## 7. The regression check, and both directions

`tests/check_rom_revision.py`. Headless, muted, synthesises its own inputs at
runtime and deletes them, and skips cleanly for any revision that is absent — it
still passes on a machine with only `baserom.us.v80.z64`.

```
1. identification (each revision present is classified and named)
   us.v80   ok  accepted and named 'US 1.1'
   pal.v80  ok  accepted and named 'European'
   us.v77   ok  refused, named 'US 1.0'
   pal.v77  ok  refused, named 'European'
   jpn.v79  ok  refused, named 'Japanese'
2. a non-DKR N64 ROM is refused as non-DKR
   ok  refused as non-DKR
3. byte order: synthesised .v64 / .n64 race identically to the .z64
   v64  ok  5 frames byte-identical, same trajectory
   n64  ok  5 frames byte-identical, same trajectory
4. platform/rom_id.c and dist/web/rom-id.js agree
   ok  5 revision rows identical in both
   us.v80 / pal.v80 / us.v77 / pal.v77 / jpn.v79  ok  same verdict on both sides
5. every other supported revision races identically to us.v80
   pal.v80  ok  5 frames byte-identical, same trajectory

check_rom_revision: PASS
```

**Positive control A — revision check reverted** (`if (0)` in place of
`if (id.verdict != DKR_ROM_SUPPORTED)`), rebuilt:

```
   us.v77   FAIL exit -11 (not a clean refusal)
   pal.v77  FAIL exit -11 (not a clean refusal)
   pal.v80  FAIL exit -11 (not a clean refusal)
   jpn.v79  FAIL exit -11 (not a clean refusal)
2. a non-DKR N64 ROM is refused as non-DKR
   FAIL accepted
check_rom_revision: FAIL (5)
```

**Positive control B — byte-order conversion reverted** (magic recognised in all
three orders, nothing converted), rebuilt:

```
3. byte order: synthesised .v64 / .n64 race identically to the .z64
   z64  exit=0    frames=5
   n64  exit=1    frames=0
   v64  exit=1    frames=0
   v64  FAIL exit 1
   n64  FAIL exit 1
check_rom_revision: FAIL (2)
```

Note what control B also shows: with the conversion gone, the unconverted image is
caught by the *revision* check (its header reads as garbage, so it is not DKR) and
refused cleanly rather than run as silent nonsense. The two mechanisms are
complementary.

### The check had to be fixed twice before it was worth trusting

Both flaws were found by running the controls, not by reading the code:

- **It counted a SIGSEGV as a refusal.** Control A initially *passed* assertion 1:
  with the gate disabled the four revisions crashed, the non-zero exit read as
  "refused", and the accept path's message still named the revision. A clean
  refusal is now specifically exit 1 with no `[CRASH]` and no renderer bring-up.
- **It compared only the intersection of two frame sets.** A deliberately corrupted
  ROM SIGSEGV'd after one black frame and the comparison reported "1/1 identical,
  0 differ". A differing frame *count* is now a failure in its own right, and every
  comparison asserts a minimum frame count.

The harness's ROM-sensitivity is itself controlled: inverting the whole `FONTS`
(4112 bytes) or `SPRITES` (3776 bytes) section demonstrably changes the run, so a
"frames identical" result means something.

---

## 8. Byte order

`.v64` (16-bit byteswapped) and `.n64` (32-bit little-endian) are normalised to
`.z64` **in place at load**, on both sides:

- native: `dkr_rom_normalize_byte_order()` (`platform/rom_id.c`), called from
  `platformInitRom()` before any header field is read;
- browser: `dkrNormalizeByteOrder()` (`dist/web/rom-id.js`), so the copy persisted
  to IDBFS is canonical `.z64` and the header can be read to identify the revision
  at all.

The native side already converted before this change; the shell advertised
`.v64`/`.n64` by magic and converted nothing, and neither side had a test. A
synthesised `.v64` and `.n64` of the supported ROM now boot and race byte-identically
to the `.z64` — same frame hashes, same trajectory (assertion 3 above).

Nothing byte-swapped is ever committed: the synthetic images are written under a
git-ignored path and deleted, and `tools/check_no_rom.sh` /
`tools/check_clean_room.sh` gate it fail-closed.

---

## 9. Where the code lives

| file | role |
|---|---|
| `platform/rom_id.c` / `.h` | revision table, identification, byte-order normalisation, user-facing messages |
| `dist/web/rom-id.js` | the browser mirror; loaded by `index.html` before the shell |
| `platform/rom_io.c` | the load gate: size → byte order → revision → re-point the asset LUT |
| `game/src/asset_loading.c` | `gDkrAssetsLutStart` / `gDkrAssetsLutEnd` (defaults us.v80) |
| `platform/segment_consts.c` | the five-revision offset table, and the `__ROM_END` caveat |
| `tests/check_rom_revision.py` | the regression check |

`MDKR_ROM_ANY_REVISION=1` loads a refused revision anyway. It is an
**investigation** hook — it is how this document was measured — not a compatibility
switch. Do not offer it to a user as a way to play another region.

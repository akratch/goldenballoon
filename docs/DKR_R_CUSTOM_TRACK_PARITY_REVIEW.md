# DKR-R custom-track parity: source review and native integration path

Review date: 2026-09-23. This is an implementation backlog, not a claim that
Golden Balloon supports custom tracks. Golden Balloon is a native port.
No upstream repository, release, issue or user installation was changed.

## Current native loader integration status

Object-map structural admission now reuses the engine's
`mdkr_level_object_record_parse` through a read-only
`mdkr_level_object_map_validate` entry point. It checks the still-big-endian
16-byte map header, declared entry extent, every record's minimum common size,
even stride, bounds and translation-table ID capacity. Failure leaves the output
record count unchanged; trailing map padding is allowed. Synthetic malformed
fixtures pass in Release/ASan, and all 136 nonempty retail US v80 maps pass in
the installed-session corpus test. A distinct required map-corpus marker prevents
an older compression-only fixture satisfying this coverage. This does not yet
resolve object headers or validate behavior-specific sizes, semantic references,
AI/checkpoints or gameplay; structural acceptance alone must not publish a map.

The behavior-aware spawn validator is now exposed as
`mdkr_objmap_validate_bodies`: it checks the complete entry stream without
changing entry bytes, reusing the active ROM's translation/header resolution
and existing behavior minimum-size rules. It requires initialized object tables
and the game thread; missing tables fail closed. The spawn path validates first,
then performs body normalization in a second traversal. This avoids modifying
early records before a later invalid record is discovered. It may populate the
shared header-behavior cache, so read-only refers to map bytes, not all engine
state. Release object-wedge gameplay regression and sanitizer loader/corpus
checks pass. Importer invocation and behavior-field semantic validation are
still unfinished; no new all-behavior admission coverage is claimed.

Lifecycle review found the shared behavior cache survived for the process
lifetime and cached a failed header load permanently. It now resets with
`allocate_object_pools` and leaves failed loads uncached, permitting a later
retry. `check_runtime_safety.py` pins reset placement, validation-before-swap
and failure non-caching with removal-based positive controls. This is source-
verified lifecycle hardening; no multi-ROM same-process reproduction or injected
header-allocation-failure recovery is claimed by the ordinary route tests.

CT1/CT4 compressed-input foundation: `custom_track_decode.c/.h` provides a
bounded admission decoder over bundled miniz, separate from the engine's shared
inflater state. It reads the five-byte rzip wrapper's little-endian size, checks
an independent caller capacity and signed-size limit, and requires a complete
raw-deflate stream with exactly the declared output length. Alignment padding
after the stream is allowed. Rejected output is unpublished scratch data, not
a valid asset. Independent stored-block fixtures cover successful output,
truncation at every byte, capacity limits, short/long decoded-length mismatch,
malformed LEN/NLEN, null arguments and output canaries. Release and ASan/UBSan
CTest pass; native and MinGW production compilation is warning-clean. This
decoder is not yet wired into package admission and does not validate geometry,
object records, cross-references or custom courses. The native loader fixture
now decodes every nonempty retail object-map and level-model entry through it:
136 maps and 55 models in local US v80, passing in Release and ASan/UBSan within
the engine's current 0x3000/0x82A00 output limits. The wrapper explicitly requires
both nonzero corpus counts, so an older fixture cannot pass silently. ROM bytes
remain private in memory. This establishes retail compression compatibility,
not actual authored-stream compatibility or a safe geometry importer.

The native build now links the immutable session implementation. `asset_loading.c`
maps the five section kinds to native table/data enums, copies already-normalized
session tables into caller-owned game-pool allocations, routes partial payload
loads through the checked split reader, and returns logical table/data sizes.
ROM-pointer and alternate whole-section APIs fail closed for session-backed
sections. `asset_custom_track_session_set` borrows a session only at a stopped,
quiescent engine boundary; the owner must detach it before destruction and must
prepare it against the active ROM. Publication now requires initialized ROM DMA
and validates every supplied section against a freshly read, normalized retail
table (exact original count and offsets, excluding alignment padding). Failure
preserves the installed session. This is not live reload or package admission,
nor does matching tables prove identical ROM content or valid authored payloads.

Full Release build passes. Focused `custom_track_table` and `asset_subentry`
CTest checks pass. ROM-backed `check_subentry_bounds.py` passes: ordinary boot
is clean and four negative controls abort with their expected diagnostic.
Follow-up `tests/check_custom_track_loader.py` passes in Release and ASan/UBSan
with both ordinary boot and an explicitly enabled installed-session fixture.
The fixture exercises all five actual ROM table mappings, caller-owned table
copies, logical sizes, cross-boundary loads, invalid-range nonmutation and
detach/restoration before consumer initialization. It uses synthetic appended
bytes and does not decode them as playable assets or export ROM data.
It exposed a pre-existing DMA shim bug: full host destinations were truncated
and reconstructed with arena upper bits. Both DMA entry points now retain full
native pointers and widen only legacy 32-bit tokens. The `dma_destination`
boundary test and address-domain source gate pass. No production importer
currently publishes a session. The installed-session fixture additionally rejects
an altered retail prefix in each of the five section types while retaining the
valid installed session. ROM-content compatibility admission,
boot lifetime ownership, format validation, relocation, importer/UI and gameplay
qualification remain open. Earlier dated foundation notes below describe the
incremental path and are superseded by this status where they say no hooks exist.

## Pinned evidence

Local CT2 session follow-up: the immutable section foundation now supports
all-or-nothing multi-section construction with aggregate payload/table limits.
The two-section fixture verifies duplicate/invalid kind rejection, missing
section lookup, malformed second-section rollback, exact aggregate limits,
copied payload ownership, and all nine allocation-failure sites while preserving
an existing session. Release CTest and a standalone ASan/UBSan run pass;
production C11 compilation is warning-clean under native Clang and MinGW.
This is not Windows runtime qualification. Session publication, engine asset
dispatch, format validation, importer/UI and actual custom-course execution
remain unfinished; this foundation alone does not complete CT2 or issue #65.

- DKR-R source-available beta.11 branch remains at
  `4ddfa1405eb76e8c073ea02603c5d557725376cf`.
- Reviewed `runtime-recomp/src/game/custom_tracks.hpp`, selected importer,
  parser and arena-measurement paths in `custom_tracks.cpp`, the native asset
  loader/track allocator, and upstream custom-track documentation. This is
  **not** a completed audit of every beta.11 runtime hook or Blender operator.
- [Upstream implementation](https://github.com/ThatGuyMcd/DKR-R/blob/4ddfa1405eb76e8c073ea02603c5d557725376cf/runtime-recomp/src/game/custom_tracks.cpp)
  and [format/architecture document](https://github.com/ThatGuyMcd/DKR-R/blob/4ddfa1405eb76e8c073ea02603c5d557725376cf/docs/CUSTOM_TRACKS.md).
  The document's opening statement that hooks/UI are not wired is stale
  relative to its later sections and the checked-in hooks/UI; do not use that
  opening as a feature verdict.
- Read-only public issue refresh still returned #58, #65, #66, #67 and #68.
  [#65](https://github.com/akratch/goldenballoon/issues/65) remains the parent
  custom-track request. No issues were closed or edited.

## Model artwork is a separate acceptance gap

The elliotttate fork's sprite-model loader consumes separately supplied
`models/<family>/*near.dkrmesh`, `*far.dkrmesh` and PNG atlases. The current
candidate implements that path for the enumerated sprite families. It neither
creates additional courses nor implements `.dkrmap`.

The maintainer has no additional artwork archive. Earlier local searches found
none, and the supplied SR.GU V0.2.1 pack contains texture replacements, not this
geometry. Additional read-only checks found:

- No releases returned from `elliotttate/goldenballoon`.
- The DKR-R beta.11 recursive tree has no separate replacement-model artwork.
- Downloaded and listed all 50 entries in the beta.10 Windows release ZIP;
  there are no separately bundled model meshes. No downloaded binary was run.
  SHA-256 `b7392fdebae29b9bdf1a5f12155bdba644e813bfcfcb901783797893a5822aa9`
  matches upstream `SHA256SUMS.txt`. Private inspection copy:
  `/tmp/mdkr-upstream-artwork.L59wDp/beta10-windows.zip`.
- [Upstream README](https://github.com/ThatGuyMcd/DKR-R/blob/4ddfa1405eb76e8c073ea02603c5d557725376cf/README.md)
  points to Discord for the community texture pack, not a downloadable model
  archive. These checks do not prove that artwork never existed or that no
  other distribution contains it.

Keep actual-art visual acceptance open. Do not repeatedly ask the maintainer
for an archive they have already said they do not possess. Synthetic loader,
GPU and resource tests remain useful but cannot replace that acceptance.

## Findings that block copying the upstream importer unchanged

These are source-inspection findings; no destructive exploit was executed.
They concern the pinned upstream importer, **not an installed Golden Balloon
custom-track feature**.

1. **High — install destination can escape the managed directory.**
   `parse_track` checks only that `id` is nonempty. For a bare ZIP root,
   `install` derives `folder_name = probe.id + ".dkrmap"`, joins it to the
   install directory, then calls `remove_all` on an existing destination.
   An ID containing parent components or an absolute path reaches this path
   construction without a containment check. ZIP-entry path validation does
   not validate JSON field values. Require a bounded identifier grammar and
   independent destination containment; never recursively replace an
   unchecked path.
2. **High — replacement is destructive before publication succeeds.**
   Existing destination contents are removed before recursive copy. A failed
   copy can lose the prior working install and leave a partial new install.
   Reuse the native staged/no-overwrite publication approach; implement an
   explicit recoverable update transaction if replacements are supported.
3. **High — directory payload confinement is lexical, not filesystem-safe.**
   Rejecting absolute paths and strings containing `..` does not prevent a
   relative payload symlink from escaping a working directory. `read_file`
   follows the path through ordinary `file_size`/`fopen`. Require regular files
   and containment throughout traversal; cover parent-component symlinks and
   path changes between inspection and open. Do not infer safety from the
   stronger, separate uninstall containment check.
4. **Medium — per-file limits do not bound an import.**
   Payload reads cap each file at 16 MiB, but `extract_zip` lacks explicit
   entry-count, total inflated-byte and per-entry expansion limits. Manifest
   entries can also accumulate payload copies. Bound archive expansion,
   manifest size/depth/count, decoded geometry and aggregate retained bytes
   independently, before allocating or writing each payload.
5. **Robustness review needed — typed JSON and binary admission.**
   `.value()` calls expect specific types after syntax-only parsing; audit
   exception handling through every scan/install/UI caller. The header map
   checks deliberately skip fields outside a short header rather than proving
   the whole header valid. A native importer must reject malformed types,
   undersized records and invalid cross-references before reaching game code.

## Engineering sequence and acceptance

### Manifest compatibility contract (pinned parser review)

Reviewed `parse_track` at the pinned beta.11 commit directly, not inferred from
the package extension. The working-directory root contains `manifest.json`.
Version 1 has an ID, optional display name (defaults to ID), author and a
nonempty `adds` array. Each addition names one of `LEVEL_HEADERS`,
`LEVEL_OBJECT_MAPS`, `LEVEL_NAMES`, `LEVEL_MODELS`, `TEXTURES_3D` and a relative
payload file. Object-map additions require a `structure` or `collectables`
slot. Preserve manifest order for identity assignment; do not sort payloads
by filename during import. The optional companion-HD object names a sibling
file and texture digest; it must never authorize arbitrary filesystem reads.

Native typed admission must explicitly check object/array/string/integer types
before extracting values, including optional metadata, rather than relying on
upstream `.value()` calls to reject wrong types by throwing. Validate IDs
separately from payload paths. Repeated singleton sections and repeated map
roles need explicit ambiguity rejection; repeated texture entries are expected
and must not be rejected as duplicate section names. Header map relocation must
distinguish authored retail references from placeholders: at offsets 0x36 and
0xBA, zero is a real retail map ID, not absence. A placeholder without its role's
payload is unsafe. Upstream skips these checks for undersized headers; native
admission must reject undersized records before reading/relocating fields.

This defines the next importer work, not an implemented manifest parser or
authorization to copy upstream code without the licensing review. Compatibility
source: [pinned parser](https://github.com/ThatGuyMcd/DKR-R/blob/4ddfa1405eb76e8c073ea02603c5d557725376cf/runtime-recomp/src/game/custom_tracks.cpp).

| Work item | Native implementation boundary | Required proof |
|---|---|---|
| CT1: package admission | New bounded `.dkrmap` manifest/payload layer over contained source reads; keep it distinct from `pack.ini` texture packs | Valid directory/ZIP fixtures; wrong JSON types, traversal IDs, symlinks, duplicate IDs/sections, truncated records, decompression and aggregate limits; no changes to a prior install on failure |
| CT2: immutable asset session | Extend normalized native tables in `asset_table_load`; serve checked appended payload spans in `asset_load` without passing them to ROM DMA | Preserve every retail offset/last-entry size and terminator; reject overflow; exact endian contract; all-or-nothing session lifetime and unload/reload tests |
| CT3: level/texture identity | Relocate header model/name/object-map references and model texture IDs; preserve structure versus collectables roles | Multiple tracks plus character textures; no stale IDs after rescan; explicit restart/reload semantics for boot-published texture tables |
| CT4: geometry and render budgets | Validate/decode before allocating `generate_track` storage; account for collision planes, wave work, visible batches, matrices and each viewport | Boundary fixtures, 1P–4P, preview-to-race-to-preview, malformed adjacency/PVS, finite indices, allocation failure and sanitizer checks |
| CT5: gameplay/save/netplay | Define added-level identity, menu launch/return, AI/checkpoints, course/world limits and save sidecar ownership | Finish/restart/unload behavior; no retail EEPROM expansion or pollution; deterministic content identity before any online admission |
| CT6: authoring parity | Audit and adapt beta.11 Blender exports for water/waves, waterfalls, transparency, texture scrolling and race AI | Independently authored end-to-end exported tracks; visible animation/transparency; collision/AI behavior; missing-HD-pack fallback |
| CT7: user workflow | Controller-accessible track library, working-folder iteration, Track Lab, install/update/uninstall and companion HD-pack handling | Minimum-size keyboard/gamepad journeys, cancellation, transactional disk failure, managed-copy-only deletion and preservation of working sources/saves |

CT1/CT2 must precede executing untrusted custom geometry. CT3/CT4 qualify loading;
CT5–CT7 complete the requested capability rather than stopping at a parser.
Source-available prerelease status is not a reason to defer any of these.

CT2 foundation started: `platform/custom_track_table.h` implements bounded,
host-endian offset-table extension with explicit input lengths, preserved retail
end offsets, signed-loader-range overflow checks and no output writes on failure.
Synthetic tests cover last-retail size, multiple appended entries, no additions,
insufficient output, malformed sentinel/order, empty payload and overflow.
Standalone warning-clean and ASan/UBSan runs pass; registered `custom_track_table`
in CTest. No engine hook or package parser is wired yet: this does not enable
custom tracks. Immutable payload ownership, contained admission, remapping and
all remaining CT1–CT7 acceptance criteria still apply.

Added constant-time, three-way section-span classification: RETAIL, CUSTOM or
INVALID. A request spanning the retail/custom boundary, overrunning the custom
arena, using negative/zero lengths, or exceeding the signed loader range is
INVALID rather than a ROM fallback. CUSTOM returns an arena-relative offset;
all other outcomes preserve that output. Tests cover exact first/last-byte
boundaries and malformed ranges. Registered Release CTest and standalone
ASan/UBSan test pass. This assumes a validated contiguous immutable arena;
ownership, entry-format validation and native loader dispatch are still unwired.

Owned-section foundation now implemented in `custom_track_section.c/.h`:
creation privately copies table/payloads under caller-provided byte/word limits,
validates complete ranges before returning an opaque immutable section, and
releases partial allocations on failure. Read dispatch returns RETAIL/CUSTOM/
INVALID; invalid and retail reads do not modify the destination. Tests prove
source-byte mutation after creation cannot change the owned payload, preserve
retail offsets, reject budget/size violations and free the section under
ASan/UBSan. Release CTest and standalone sanitizer runs pass. This replaces the
earlier "ownership unwired" status for this primitive only: multi-section
session lifetime, allocation-failure injection, package/asset-format admission,
relocation, loader integration and gameplay remain unimplemented/unqualified.

Owned-section allocation-failure qualification now covers each of its four
allocation sites via a compile-time-only allocator fixture. Every injected
failure returns no new section, returns live allocation count to baseline and
leaves an existing section readable with unchanged bytes; final destruction
returns the count to zero. Release CTest and ASan/UBSan pass. Production code
also compiles warning-clean without the fixture on native Clang and Windows
MinGW; no Windows executable was run. Multi-section publication is still absent.

Native foundation follow-up: new tests also exposed symlink-following and
root-path replacement in Golden Balloon's existing directory pack reader.
The POSIX implementation now pins a root descriptor and uses no-follow
directory-relative reads/enumeration. The Windows counterpart is now implemented
with root-relative NT file handles, no-follow/reparse rejection and handle-based
enumeration. It and its Windows-specific tests cross-compile/link, but have not
yet run on Windows. Do not treat a lexical name validator or cross-compile as
proof of runtime confinement. This is a CT1 dependency, not `.dkrmap` support.

API basis: Microsoft's [NtOpenFile documentation](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntopenfile)
and the shared [relative-name/open-option contract](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntcreatefile)
describe a retained parent handle and non-following reparse opens. The source
opens one component at a time and validates the resulting handle; it never
reconstructs an untrusted descendant pathname for opening. Enumeration uses
[GetFileInformationByHandleEx](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getfileinformationbyhandleex)
on an independent directory handle. API availability or query failures remain
fail-closed rather than falling back to the former pathname-based reader.

### Native-port differences that must not be papered over

- Direct local us.v80 ROM inspection confirmed padding after the first table
  sentinel in four of the five relevant sections; header/name data sections
  also extend beyond their table's last logical offset. The bounded
  `mdkr_custom_track_measure_table` helper now finds the first sentinel in a
  host-normalized table, checks monotonic offsets against the physical section,
  and excludes trailing padding. Synthetic padded/malformed fixtures pass in
  Release and ASan/UBSan. No ROM bytes or extracted assets enter these fixtures.
- `game.c` loads all level names in one `asset_load` from offset zero through
  the last logical offset. Once extended, this deliberately crosses the
  retail/custom boundary. The current single-source classifier rejects such
  spans correctly for its contract, but is insufficient as the complete loader
  adapter. A separately checked split-read plan and section-load helper now
  support a bounded retail prefix plus custom suffix. Exhaustive small-range
  tests validate source selection, with signed-limit cases and callback tests
  for retail-only/custom-only/crossing reads. Invalid requests never call the
  reader; a failed retail read may modify its prefix but never copies the custom
  suffix. Release and ASan/UBSan pass, and production MinGW compilation is
  warning-clean. Engine callback wiring remains unfinished;
  never treat INVALID as permission to fall back to ROM. Append at the last
  logical offset, not the physical padding end, to preserve last-retail sizes.

- Loader integration requires **three coordinated hooks**, not just extended
  tables plus payload dispatch: `asset_table_size()` must return the session's
  logical extent too. `generate_track` checks that size before loading models
  (fatal on a rejected range), and `load_texture` rejects spans beyond it before
  reading a texture header. The immutable section now exposes its logical size;
  Release and ASan/UBSan tests cover an appended extent, retail-only section,
  exact `INT32_MAX` extent and rejection one byte beyond it. This is logical
  address space, not resident RAM usage. Keep table copies caller-owned through
  the game pool; do not return the session's borrowed table to existing callers
  that free it. `asset_rom_offset()` must never translate appended offsets into
  cartridge addresses. No production loader hooks are installed yet.

- Current native source searches found no `.dkrmap`/custom-track implementation.
  The asset loader presently enforces retail section bounds. Extending a table
  alone would send appended offsets into rejected ROM spans, not a working
  new level.
- Tables are normalized BE-to-host in `asset_table_load`; decompressed models
  are normalized separately in `generate_track`. Do not copy a guest-memory
  big-endian hook into either boundary without an explicit representation.
- `tracks.c` reserves `LEVEL_MODEL_MAX_SIZE = 0x82A00` and limits segments to
  128. Native checks currently abort on invalid compressed spans/decoded
  sizes. Import admission must turn unsupported authored content into a
  recoverable diagnostic before it reaches those fatal engine guards.
- Correct an earlier parity note: the inspected upstream policy grows a
  guest main pool **into 8 MiB**, not beyond 8 MiB. Its model measurement also
  has a 1 MiB inflated-input ceiling, an 8 MiB computed-arena ceiling and a
  signed collision-plane-index limit. Golden Balloon needs native checked
  budgets, not a literal copy of guest addresses `0x80400000/0x80800000`.
- Review upstream licensing/notices before copying code. Interface/file-format
  compatibility and independent synthetic fixtures do not grant redistribution
  rights to separately supplied game artwork.

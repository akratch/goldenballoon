# S1 — Content pipeline: packs, texture overrides, custom music

> **Executing this plan:** work the tasks in order, one `- [ ]` step at a time.
> Tick a step only once its command has been run and its output read. The
> run-it-and-watch-it-fail steps are load-bearing; skipping ahead to the
> implementation forfeits the positive control that makes the gate mean anything.

**Goal:** Let a player drop a folder or `.zip` into `mods/` and have it replace
textures and music at runtime, without the project ever shipping, hosting, or
tracking a single byte of game content.

**Architecture:** A read-only override layer that sits *in front of* the existing
ROM asset path, never inside it. A registry scans `mods/`, parses one INI
manifest per pack, and orders packs by priority. Two consumers query that
registry: the Fast3D texture cache (before upload) and the audio sequence start
path (before the sequence player runs). Nothing in `game/` changes. The ROM
remains the only source of base content, so the clean-room guard keeps its
current meaning and gains one new rule: packs are user data and can never be
tracked or packaged.

**Tech stack:** C11, existing `platform/config_ini.c` for manifest parsing,
existing `platform/sha256.c` for content addressing, `stb_image.h` (MIT, vendored)
for PNG decode, `miniz` (MIT, vendored) for zip containers, `dr_wav.h` (public
domain, vendored) for WAV decode.

## Global constraints

- Every game or test invocation passes `--headless-frames N` and sets
  `MDKR_AUDIO=0`. `MDKR_AUDIO=off` is a no-op — only the digit `0` disables.
- No ROM-derived data in any commit. `tools/check_no_rom.sh` and
  `tools/check_clean_room.sh` fail closed and must stay that way.
- No change under `game/`. If this sprint appears to need one, stop and escalate:
  the override layer is specified to sit outside the vendored decompilation.
- Every new `tests/check_*.py` is registered in `tools/run_checks.py`'s `CHECKS`
  tuple. Every new `tests/test_*.c` gets `add_executable` + `add_test` in
  `cmake/tests.cmake`.
- Every vendored third-party file is recorded in `THIRD_PARTY.md` and
  `NOTICE.md` in the same commit that adds it.
- New settings keys are **appended** to `MdkrVideoKey` in
  `platform/video_config.h`, never inserted — the enum is the schema table index
  and the in-game menu's wheel order.
- Run `python3 tools/check_public_surface.py --staged` before every commit.
- Commit messages carry no assistant attribution trailer; the pre-push hook
  rejects them.

---

## User stories

**US-1 — Install a pack.** As a player, I drop `MyHDPack/` into `mods/`, launch
the game, and the pack's textures appear, so that I can use community art
without patching a ROM or replacing an executable.

**US-2 — See what loaded.** As a player, I open Settings → Content and see every
pack that was found, its author and version, whether it is enabled, and the
order packs override each other in, so that a pack failing to load is never
silent.

**US-3 — Compare instantly.** As a player, I press `Tab` during play and every
override switches off, and press it again and they switch back on, so that I can
judge a pack against the original without restarting.

**US-4 — Turn one pack off.** As a player, I untick a pack in Settings → Content
and it stops applying at the next track load, so that I can isolate which pack
is responsible for something I dislike.

**US-5 — Replace the music.** As a player, I put `music/<id>.wav` in a pack and
that track plays instead of the sequenced original, at the right moments, with
the same volume control, so that arranged soundtracks work.

**US-6 — Author a pack.** As a pack author, I run
`tools/mod_texture_dump.py --run` while playing, get a folder of PNGs named by
content hash, replace the ones I want at any resolution, add a `pack.ini`, and
ship the folder, so that authoring does not require reading the renderer.

**US-7 — Ship nothing.** As the maintainer, I need the build, the release
artifacts and the repository to remain provably free of game content even after
this feature exists, so that the project's legal position is unchanged.

---

## Milestones and acceptance criteria

### M1 — Pack registry (directory packs, no content types yet)

**Done when:**
- `mods/` is discovered next to the executable for portable builds and under the
  per-user data directory for packaged builds, using the same policy
  `platform/user_paths.c` already applies to saves and config.
- A pack is a directory containing `pack.ini`. A directory without one is
  reported as skipped, not silently ignored.
- Packs load in ascending `priority`; equal priorities break ties by
  case-insensitive directory name so ordering is deterministic across
  filesystems.
- `mdkr_mod_registry_resolve()` returns the highest-priority enabled pack
  holding a given relative path.
- A malformed manifest disables that pack and records a one-line reason; it
  never aborts startup and never disables other packs.
- `tests/test_mod_manifest.c` and `tests/test_mod_registry.c` pass under ctest
  with no ROM and no window.

### M2 — Texture overrides

**Done when:**
- Every texture the renderer is about to upload is first hashed to a stable
  32-hex-character digest and looked up as `textures/<digest>.png`.
- A found PNG is decoded, uploaded in place of the ROM texels, and cached under
  the same `struct DkrTexCacheKey` identity with an added `override_generation`
  field so toggling invalidates correctly.
- A pack texture of different pixel dimensions than the original works; UVs
  continue to address the logical tile.
- `Tab` toggles all overrides live, within one frame, with no leak and no
  re-upload storm (proven by a bounded upload count in the gate).
- `tools/mod_texture_dump.py` writes the digest-named PNG corpus for a running
  session to a git-ignored directory.
- `tests/check_mod_texture_override.py` proves an override changes pixels, that
  `Tab` restores the original exactly, and — the positive control — that the
  gate fails when the override lookup is stubbed out.

### M3 — Zip pack containers

**Done when:**
- A `.zip` in `mods/` is a pack, read without extraction to disk.
- Directory packs and zip packs resolve through one interface; the texture and
  music consumers do not know which they are reading from.
- A corrupt zip disables that pack with a reason and leaves the others working.
- Zip path traversal (`../`, absolute paths, symlink entries) is rejected with a
  test that attempts each.

### M4 — Custom music

**Done when:**
- `music/<sequence_id>.wav` in a pack plays instead of the sequenced original,
  starting and stopping at the same points the sequence would.
- Music volume, master volume, pause, and track transitions behave identically
  to the sequenced path.
- Audio determinism gates still pass: replacement is presentation-layer, and
  `check_state_hash` streams byte-identical with a pack installed.
- A WAV whose sample rate or channel count differs from the mixer's is
  resampled, not rejected.

### M5 — Guard extension and documentation

**Done when:**
- `mods/` is in `.gitignore`, and `tools/check_clean_room.sh` fails if any file
  under `mods/` is tracked by git.
- A new gate fails a release artifact that contains any `pack.ini`, `.gbpack`,
  or `mods/` entry.
- `docs/MODDING.md` documents the pack layout, manifest keys, priority rules,
  the texture digest, and the music naming scheme.
- `README.md` gains a short "Custom content" section that states plainly that no
  content is included and none is hosted.

---

## File structure

**Create:**

| Path | Responsibility |
|---|---|
| `platform/mod_manifest.h/.c` | Parse and validate one `pack.ini`. Pure; no filesystem. |
| `platform/mod_registry.h/.c` | Discover `mods/`, own the ordered pack list, resolve relative paths. |
| `platform/mod_source.h/.c` | One read interface over a directory pack and a zip pack. |
| `platform/mod_texture_key.h/.c` | Stable content digest for a texture. Pure; no GPU. |
| `platform/mod_texture_store.h/.c` | Decode and cache override PNGs; owns the override generation counter. |
| `platform/mod_music.h/.c` | Resolve and decode replacement music; feed the existing mixer. |
| `lib/stb/stb_image.h` | Vendored PNG decoder (MIT). |
| `lib/miniz/miniz.h`, `lib/miniz/miniz.c` | Vendored zip reader (MIT). |
| `lib/dr_libs/dr_wav.h` | Vendored WAV decoder (public domain). |
| `tools/mod_texture_dump.py` | Author-side digest corpus dumper. |
| `docs/MODDING.md` | Pack authoring reference. |
| `tests/test_mod_manifest.c` | Manifest parse/validate unit test. |
| `tests/test_mod_registry.c` | Ordering, resolution, and failure-isolation unit test. |
| `tests/test_mod_texture_key.c` | Digest stability and sensitivity unit test. |
| `tests/test_mod_source_zip.c` | Zip reading and traversal-rejection unit test. |
| `tests/check_mod_texture_override.py` | End-to-end override + `Tab` toggle gate. |
| `tests/check_mod_music_override.py` | End-to-end music replacement gate. |
| `tests/check_mod_content_absence.py` | Guard-extension gate. |

**Modify:**

| Path | Change |
|---|---|
| `platform/fast3d/gfx_texture_cache_key.h` | Add `uint32_t override_generation` to `struct DkrTexCacheKey` and to `dkr_texcache_key_equal()`. |
| `platform/fast3d/gfx_pc_dkr.c` | Query the override store before the ROM texel upload. |
| `platform/video_config.h` | Append `MDKR_CONTENT_PACKS_ENABLED`, `MDKR_CONTENT_PACK_DISABLED_LIST`. |
| `platform/video_config.c` | Schema rows for the two new keys. |
| `platform/app/ui_settings.cpp` | Settings → Content section. |
| `platform/app/app_ui_policy.cpp` | Visibility rule for the new keys. |
| `platform/platform_sdl_min.c` | `Tab` binding for the override toggle. |
| `cmake/tests.cmake` | Register the four new unit tests. |
| `CMakeLists.txt` | Add the new `platform/mod_*.c` sources and vendored libraries. |
| `tools/run_checks.py` | Register the three new checks in `CHECKS`. |
| `tools/check_clean_room.sh` | Fail on tracked `mods/` content. |
| `.gitignore` | Ignore `mods/`. |
| `THIRD_PARTY.md`, `NOTICE.md` | Record stb_image, miniz, dr_wav. |

---

## Task 1: Manifest parser

**Files:**
- Create: `platform/mod_manifest.h`, `platform/mod_manifest.c`
- Test: `tests/test_mod_manifest.c`
- Modify: `cmake/tests.cmake`

**Interfaces:**
- Consumes: `platform/config_ini.h` (existing INI reader).
- Produces: `MdkrModManifest`, `mdkr_mod_manifest_parse()`. Task 2 depends on both.

- [ ] **Step 1: Write the failing test**

Create `tests/test_mod_manifest.c`:

```c
#include "mod_manifest.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int cond, const char *what) {
    if (!cond) { printf("FAIL %s\n", what); failures++; }
    else       { printf("ok   %s\n", what); }
}

static void test_minimal_manifest_defaults(void) {
    static const char ini[] = "[pack]\nname = Sunset Skies\n";
    MdkrModManifest m;
    char err[128];
    int rc = mdkr_mod_manifest_parse(ini, sizeof(ini) - 1, &m, err, sizeof err);
    expect(rc == 0, "minimal manifest parses");
    expect(!strcmp(m.name, "Sunset Skies"), "name read");
    expect(m.priority == 100, "priority defaults to 100");
    expect(m.enabled == 1, "enabled defaults to on");
    expect(m.author[0] == '\0', "absent author is empty, not garbage");
}

static void test_missing_name_is_rejected(void) {
    static const char ini[] = "[pack]\nauthor = Somebody\n";
    MdkrModManifest m;
    char err[128];
    int rc = mdkr_mod_manifest_parse(ini, sizeof(ini) - 1, &m, err, sizeof err);
    expect(rc != 0, "manifest without a name is rejected");
    expect(strstr(err, "name") != NULL, "error names the missing key");
}

static void test_priority_out_of_range_is_rejected(void) {
    static const char ini[] = "[pack]\nname = X\npriority = 100000\n";
    MdkrModManifest m;
    char err[128];
    expect(mdkr_mod_manifest_parse(ini, sizeof(ini) - 1, &m, err, sizeof err) != 0,
           "priority above 9999 is rejected");
}

static void test_name_longer_than_field_is_rejected_not_truncated(void) {
    char ini[256];
    snprintf(ini, sizeof ini, "[pack]\nname = %0*d\n", 100, 0);
    MdkrModManifest m;
    char err[128];
    expect(mdkr_mod_manifest_parse(ini, strlen(ini), &m, err, sizeof err) != 0,
           "over-long name is rejected rather than silently truncated");
}

int main(void) {
    test_minimal_manifest_defaults();
    test_missing_name_is_rejected();
    test_priority_out_of_range_is_rejected();
    test_name_longer_than_field_is_rejected_not_truncated();
    printf(failures ? "FAILURES: %d\n" : "all manifest assertions passed\n", failures);
    return failures ? 1 : 0;
}
```

- [ ] **Step 2: Register the test and run it to verify it fails**

Add to `cmake/tests.cmake`, inside the existing
`if(BUILD_TESTING AND NOT EMSCRIPTEN)` block, following the
`mdkr_display_config_test` pattern exactly:

```cmake
    add_executable(mdkr_mod_manifest_test
        ${CMAKE_SOURCE_DIR}/tests/test_mod_manifest.c
        ${CMAKE_SOURCE_DIR}/platform/mod_manifest.c
        ${CMAKE_SOURCE_DIR}/platform/config_ini.c)
    target_include_directories(mdkr_mod_manifest_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_mod_manifest_test PRIVATE m)
    endif()
    add_test(NAME mod_manifest COMMAND mdkr_mod_manifest_test)
```

Run:

```bash
cmake --preset rel && cmake --build build-rel -j8 --target mdkr_mod_manifest_test
```

Expected: FAIL at configure or compile with `mod_manifest.h: No such file`.

- [ ] **Step 3: Write the header**

Create `platform/mod_manifest.h`:

```c
/* mod_manifest.h — one content pack's `pack.ini`, parsed and validated.
 *
 * Pure: no filesystem, no allocation. The caller reads the file; this decides
 * whether its contents describe a loadable pack. A rejected manifest disables
 * exactly one pack and never aborts startup, so every failure path here must
 * produce a human-readable reason rather than a return code alone.
 */
#ifndef MDKR64_MOD_MANIFEST_H
#define MDKR64_MOD_MANIFEST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MOD_NAME_MAX    64
#define MDKR_MOD_AUTHOR_MAX  64
#define MDKR_MOD_VERSION_MAX 32

typedef struct MdkrModManifest {
    char name[MDKR_MOD_NAME_MAX];
    char author[MDKR_MOD_AUTHOR_MAX];
    char version[MDKR_MOD_VERSION_MAX];
    /* Ascending load order. Later-loaded packs win a path collision.
     * 0..9999; defaults to 100 so authors can sit either side of the default. */
    int  priority;
    /* 1 unless the manifest says otherwise. The player's own disable list is
     * separate and lives in Content.PackDisabled, not here. */
    int  enabled;
} MdkrModManifest;

/* Returns 0 on success. On failure returns non-zero, writes a one-line reason
 * into `err`, and leaves `*out` unspecified. `ini_text` need not be
 * NUL-terminated; `len` is authoritative. */
int mdkr_mod_manifest_parse(const char *ini_text, size_t len,
                            MdkrModManifest *out, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MOD_MANIFEST_H */
```

- [ ] **Step 4: Implement `platform/mod_manifest.c`**

Read `platform/config_ini.h` first and use its existing reader rather than
writing a second INI parser. The implementation must:

1. Parse the `[pack]` section only; ignore unknown sections so a future schema
   addition does not break old readers.
2. Require `name`; reject absence with an error containing the word `name`.
3. Copy `name`, `author`, `version` with an explicit length check — reject when
   the source is longer than the destination rather than truncating. Silent
   truncation is the exact shape §3 of the developer handbook warns about.
4. Default `priority` to 100 and `enabled` to 1 before reading, so an absent key
   and a present-but-empty key behave the same.
5. Reject `priority` outside 0..9999.
6. Zero `*out` at entry so absent optional fields are empty strings, never
   uninitialised stack.

- [ ] **Step 5: Run the test to verify it passes**

```bash
cmake --build build-rel -j8 --target mdkr_mod_manifest_test && \
  ctest --test-dir build-rel -R '^mod_manifest$' --output-on-failure
```

Expected: PASS, 4 assertion groups, `all manifest assertions passed`.

- [ ] **Step 6: Commit**

```bash
git add platform/mod_manifest.h platform/mod_manifest.c \
        tests/test_mod_manifest.c cmake/tests.cmake
python3 tools/check_public_surface.py --staged
git commit -m "feat: parse and validate content-pack manifests"
```

---

## Task 2: Pack registry — discovery, ordering, resolution

**Files:**
- Create: `platform/mod_registry.h`, `platform/mod_registry.c`
- Test: `tests/test_mod_registry.c`
- Modify: `cmake/tests.cmake`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `mdkr_mod_manifest_parse()` (Task 1); `mdkr_user_save_directory()`
  from `platform/user_paths.h`.
- Produces: `MdkrModRegistry`, `mdkr_mod_registry_init/shutdown/count/entry/resolve`.
  Tasks 4, 6 and 8 depend on `mdkr_mod_registry_resolve()`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_mod_registry.c`. It builds a temporary pack tree under a
path given by argv[1] (ctest passes a build-dir-relative scratch path), so it
touches no user directory:

```c
#include "mod_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static void expect(int c, const char *w) {
    if (!c) { printf("FAIL %s\n", w); failures++; } else printf("ok   %s\n", w);
}

/* Writes `text` to `dir/rel`, creating `dir` and its immediate child. */
static void write_pack_file(const char *root, const char *pack,
                            const char *rel, const char *text);

static void test_priority_orders_packs(const char *root) {
    write_pack_file(root, "alpha", "pack.ini", "[pack]\nname=Alpha\npriority=10\n");
    write_pack_file(root, "bravo", "pack.ini", "[pack]\nname=Bravo\npriority=90\n");
    write_pack_file(root, "alpha", "textures/deadbeef.png", "A");
    write_pack_file(root, "bravo", "textures/deadbeef.png", "B");

    MdkrModRegistry reg;
    expect(mdkr_mod_registry_init(&reg, root) == 0, "registry initialises");
    expect(mdkr_mod_registry_count(&reg) == 2, "both packs discovered");

    char resolved[1024];
    expect(mdkr_mod_registry_resolve(&reg, "textures/deadbeef.png",
                                     resolved, sizeof resolved) == 1,
           "collision resolves");
    expect(strstr(resolved, "bravo") != NULL,
           "higher priority wins the collision");
    mdkr_mod_registry_shutdown(&reg);
}

static void test_malformed_manifest_isolates(const char *root) {
    write_pack_file(root, "good", "pack.ini", "[pack]\nname=Good\n");
    write_pack_file(root, "broken", "pack.ini", "[pack]\nauthor=NoName\n");

    MdkrModRegistry reg;
    expect(mdkr_mod_registry_init(&reg, root) == 0,
           "a broken pack does not fail the registry");
    expect(mdkr_mod_registry_count(&reg) == 1, "only the good pack loads");
    expect(mdkr_mod_registry_skipped(&reg) == 1, "the broken pack is counted");
    expect(mdkr_mod_registry_skip_reason(&reg, 0) != NULL &&
           mdkr_mod_registry_skip_reason(&reg, 0)[0] != '\0',
           "the skip carries a reason");
    mdkr_mod_registry_shutdown(&reg);
}

static void test_missing_mods_dir_is_not_an_error(void) {
    MdkrModRegistry reg;
    expect(mdkr_mod_registry_init(&reg, "definitely/not/here") == 0,
           "absent mods dir is the normal case, not a failure");
    expect(mdkr_mod_registry_count(&reg) == 0, "and yields zero packs");
    mdkr_mod_registry_shutdown(&reg);
}
```

Write `write_pack_file()` with `mkdir`/`fopen` guarded by `#ifdef _WIN32` for
`_mkdir`, and a `main()` that runs the three tests against
`argv[1] ? argv[1] : "mod_registry_scratch"`, removing the tree first.

- [ ] **Step 2: Register and run to verify it fails**

```cmake
    add_executable(mdkr_mod_registry_test
        ${CMAKE_SOURCE_DIR}/tests/test_mod_registry.c
        ${CMAKE_SOURCE_DIR}/platform/mod_registry.c
        ${CMAKE_SOURCE_DIR}/platform/mod_manifest.c
        ${CMAKE_SOURCE_DIR}/platform/config_ini.c)
    target_include_directories(mdkr_mod_registry_test PRIVATE
        ${CMAKE_SOURCE_DIR}/platform)
    if(NOT MSVC)
        target_link_libraries(mdkr_mod_registry_test PRIVATE m)
    endif()
    add_test(NAME mod_registry COMMAND mdkr_mod_registry_test
             ${CMAKE_CURRENT_BINARY_DIR}/mod_registry_scratch)
```

Run: `cmake --build build-rel -j8 --target mdkr_mod_registry_test`
Expected: FAIL, `mod_registry.h: No such file`.

- [ ] **Step 3: Write the header**

```c
/* mod_registry.h — the ordered set of installed content packs.
 *
 * Discovery is read-only and failure-isolating: one unreadable or malformed
 * pack disables itself and records a reason, and every other pack still loads.
 * A missing `mods/` directory is the ordinary case and returns success with
 * zero packs, because the overwhelming majority of installs have none.
 */
#ifndef MDKR64_MOD_REGISTRY_H
#define MDKR64_MOD_REGISTRY_H

#include "mod_manifest.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MOD_MAX_PACKS 64
#define MDKR_MOD_PATH_MAX  1024

typedef struct MdkrModRegistry MdkrModRegistry_Opaque;

typedef struct MdkrModEntry {
    MdkrModManifest manifest;
    char root[MDKR_MOD_PATH_MAX]; /* directory path, or zip path for M3 */
    int  is_zip;
} MdkrModEntry;

typedef struct MdkrModRegistry {
    MdkrModEntry entries[MDKR_MOD_MAX_PACKS];
    int          count;
    char         skip_name[MDKR_MOD_MAX_PACKS][MDKR_MOD_NAME_MAX];
    char         skip_reason[MDKR_MOD_MAX_PACKS][128];
    int          skipped;
} MdkrModRegistry;

/* Scans `mods_dir`. Returns 0 on success including "directory absent". */
int mdkr_mod_registry_init(MdkrModRegistry *reg, const char *mods_dir);
void mdkr_mod_registry_shutdown(MdkrModRegistry *reg);

int  mdkr_mod_registry_count(const MdkrModRegistry *reg);
const MdkrModEntry *mdkr_mod_registry_entry(const MdkrModRegistry *reg, int i);
int  mdkr_mod_registry_skipped(const MdkrModRegistry *reg);
const char *mdkr_mod_registry_skip_reason(const MdkrModRegistry *reg, int i);

/* Highest-priority enabled pack holding `relative_path`. Returns 1 and fills
 * `out_path` when found, 0 when not. `relative_path` uses '/' on every
 * platform and is rejected if it contains '..' or a leading separator. */
int mdkr_mod_registry_resolve(const MdkrModRegistry *reg,
                              const char *relative_path,
                              char *out_path, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MOD_REGISTRY_H */
```

- [ ] **Step 4: Implement `platform/mod_registry.c`**

Requirements, each of which the test above pins:

1. Enumerate `mods_dir` with `opendir`/`readdir` on POSIX and
   `FindFirstFileW`/`FindNextFileW` on Windows. The Windows arm must go through
   `platform/fs_utf8.h`, which already owns the UTF-8↔UTF-16 boundary — do not
   add a second conversion.
2. Skip `.` and `..` and any entry beginning with `.`.
3. For each directory, read `pack.ini` (cap the read at 64 KiB), parse it, and
   on failure record `skip_name`/`skip_reason` and continue.
4. Stop at `MDKR_MOD_MAX_PACKS` and record the overflow as a skip reason so the
   64th-plus pack is never silently dropped.
5. Sort ascending by `priority`, then by case-insensitive name. Use a stable
   insertion sort; `qsort` is not stable and equal-priority ordering is part of
   the contract.
6. `resolve()` walks the sorted list **backwards** (highest priority first),
   rejects any `relative_path` containing `..`, a leading `/`, a leading `\`, or
   a Windows drive prefix, and checks existence with `stat`/`_wstat`.

- [ ] **Step 5: Run the test to verify it passes**

```bash
cmake --build build-rel -j8 --target mdkr_mod_registry_test && \
  ctest --test-dir build-rel -R '^mod_registry$' --output-on-failure
```

Expected: PASS.

- [ ] **Step 6: Add the sources to the main target**

In `CMakeLists.txt`, add `platform/mod_manifest.c` and `platform/mod_registry.c`
to the same source list that already carries `platform/video_config.c`.

Run: `cmake --build build-rel -j8` — expected: builds clean.

- [ ] **Step 7: Commit**

```bash
git add platform/mod_registry.h platform/mod_registry.c \
        tests/test_mod_registry.c cmake/tests.cmake CMakeLists.txt
python3 tools/check_public_surface.py --staged
git commit -m "feat: discover and order installed content packs"
```

---

## Task 3: Texture content digest

**Files:**
- Create: `platform/mod_texture_key.h`, `platform/mod_texture_key.c`
- Test: `tests/test_mod_texture_key.c`
- Modify: `cmake/tests.cmake`

**Interfaces:**
- Consumes: `struct DkrTexCacheKey` from
  `platform/fast3d/gfx_texture_cache_key.h`; `platform/sha256.h`.
- Produces: `mdkr_mod_texture_digest()`. Task 4 and `tools/mod_texture_dump.py`
  both depend on producing byte-identical digests.

The digest is the contract between the renderer and every pack ever authored.
Once a pack exists in the wild, changing this function silently invalidates it,
so its inputs are fixed here deliberately and narrowly.

- [ ] **Step 1: Write the failing test**

`tests/test_mod_texture_key.c` must assert:

```c
static void test_digest_is_stable_across_runs(void) {
    struct DkrTexCacheKey k = {0};
    k.width = 32; k.height = 32; k.fmt = 0; k.siz = 2;
    static const uint8_t texels[32 * 32 * 2] = { 0x12, 0x34 };
    char a[33], b[33];
    mdkr_mod_texture_digest(&k, texels, sizeof texels, a);
    mdkr_mod_texture_digest(&k, texels, sizeof texels, b);
    expect(!strcmp(a, b), "digest is deterministic");
    expect(strlen(a) == 32, "digest is 32 hex characters");
    /* Pinned value: changing it invalidates every pack in existence.
     * Regenerate ONLY with a deliberate format version bump. */
    expect(!strcmp(a, "<paste the value the first green run prints>"),
           "digest matches the pinned contract value");
}

static void test_digest_ignores_transient_fields(void) {
    struct DkrTexCacheKey a = {0}, b = {0};
    a.width = b.width = 8; a.height = b.height = 8; a.siz = b.siz = 2;
    a.addr = (const uint8_t *)0x1000;  /* allocation identity: transient */
    b.addr = (const uint8_t *)0x2000;
    a.mipmaps = 0; b.mipmaps = 1;      /* a renderer choice, not content */
    static const uint8_t t[8 * 8 * 2] = { 1 };
    char da[33], db[33];
    mdkr_mod_texture_digest(&a, t, sizeof t, da);
    mdkr_mod_texture_digest(&b, t, sizeof t, db);
    expect(!strcmp(da, db),
           "allocation address and mip choice do not change the digest");
}

static void test_digest_is_sensitive_to_content(void) {
    struct DkrTexCacheKey k = {0};
    k.width = 8; k.height = 8; k.siz = 2;
    uint8_t t1[8 * 8 * 2] = { 1 }, t2[8 * 8 * 2] = { 2 };
    char d1[33], d2[33];
    mdkr_mod_texture_digest(&k, t1, sizeof t1, d1);
    mdkr_mod_texture_digest(&k, t2, sizeof t2, d2);
    expect(strcmp(d1, d2) != 0, "one changed texel changes the digest");
}

static void test_digest_is_sensitive_to_format(void) {
    /* Two textures with identical bytes but different RDP formats are
     * different pictures and must not share an override. */
    struct DkrTexCacheKey a = {0}, b = {0};
    a.width = b.width = 8; a.height = b.height = 8;
    a.siz = 2; b.siz = 3;
    static const uint8_t t[8 * 8 * 4] = { 7 };
    char da[33], db[33];
    mdkr_mod_texture_digest(&a, t, 8 * 8 * 2, da);
    mdkr_mod_texture_digest(&b, t, 8 * 8 * 4, db);
    expect(strcmp(da, db) != 0, "format participates in the digest");
}
```

- [ ] **Step 2: Register and run to verify it fails**

Add the `mdkr_mod_texture_key_test` block to `cmake/tests.cmake` following the
Task 1 pattern, with sources `tests/test_mod_texture_key.c`,
`platform/mod_texture_key.c`, `platform/sha256.c`, and an extra include
directory `${CMAKE_SOURCE_DIR}/platform/fast3d`.

Expected: FAIL to compile.

- [ ] **Step 3: Implement**

```c
/* mod_texture_key.h — the stable name a pack author addresses a texture by.
 *
 * THIS IS A PUBLISHED CONTRACT. Every pack in existence names its files by the
 * output of this function. Changing which fields participate, their order, or
 * their encoding renames every texture and silently breaks every pack. If it
 * ever must change, bump MDKR_MOD_TEXTURE_DIGEST_VERSION, keep the old path
 * readable, and say so in docs/MODDING.md.
 */
#ifndef MDKR64_MOD_TEXTURE_KEY_H
#define MDKR64_MOD_TEXTURE_KEY_H

#include "gfx_texture_cache_key.h"
#include <stddef.h>
#include <stdint.h>

#define MDKR_MOD_TEXTURE_DIGEST_VERSION 1u

/* Writes 32 lowercase hex characters plus NUL into `out_hex`.
 *
 * Participating inputs, in this exact order:
 *   1. MDKR_MOD_TEXTURE_DIGEST_VERSION  (u32 little-endian)
 *   2. key->width, key->height          (u16 little-endian each)
 *   3. key->fmt, key->siz, key->palette (u8 each)
 *   4. key->palette_hash, key->palette_fmt (u32 little-endian each)
 *   5. the decoded texel payload, `texel_bytes` of it
 *
 * Deliberately EXCLUDED, with reasons:
 *   key->addr              — an allocation address; different every launch
 *   key->source_line_bytes — an addressing detail of the same picture
 *   key->source_size_bytes — implied by width/height/siz
 *   key->line_swapped      — a decode fix, not a difference in the picture
 *   key->font_remastered   — a renderer choice
 *   key->mipmaps           — a renderer choice
 *   key->cutout            — a renderer choice
 *   key->override_generation — the toggle, added in Task 4
 */
void mdkr_mod_texture_digest(const struct DkrTexCacheKey *key,
                             const uint8_t *texels, size_t texel_bytes,
                             char out_hex[33]);

#endif /* MDKR64_MOD_TEXTURE_KEY_H */
```

Implement with `platform/sha256.c`, hashing the fields in the documented order
into the running context, then formatting the first 16 bytes of the digest as
lowercase hex. Do **not** hash the struct with `memcpy` — padding bytes are
uninitialised and would make the digest non-deterministic. That is exactly the
silent-failure shape this codebase punishes.

- [ ] **Step 4: Run, capture the pinned value, run again**

```bash
cmake --build build-rel -j8 --target mdkr_mod_texture_key_test && \
  ctest --test-dir build-rel -R '^mod_texture_key$' --output-on-failure
```

The pinned-value assertion fails on the first run and prints the actual digest.
Paste that value into the test, rebuild, and confirm all four groups pass.

- [ ] **Step 5: Commit**

```bash
git add platform/mod_texture_key.h platform/mod_texture_key.c \
        tests/test_mod_texture_key.c cmake/tests.cmake
python3 tools/check_public_surface.py --staged
git commit -m "feat: address pack textures by a stable content digest"
```

---

## Task 4: Override store and the renderer hook

**Files:**
- Create: `platform/mod_texture_store.h`, `platform/mod_texture_store.c`,
  `lib/stb/stb_image.h`
- Modify: `platform/fast3d/gfx_texture_cache_key.h`,
  `platform/fast3d/gfx_pc_dkr.c`, `CMakeLists.txt`, `THIRD_PARTY.md`,
  `NOTICE.md`

**Interfaces:**
- Consumes: `mdkr_mod_registry_resolve()` (Task 2),
  `mdkr_mod_texture_digest()` (Task 3).
- Produces: `mdkr_mod_texture_lookup()`, `mdkr_mod_texture_set_enabled()`,
  `mdkr_mod_texture_generation()`. Task 5 (`Tab`) and Task 7 (settings) consume
  the latter two.

- [ ] **Step 1: Extend the cache key**

In `platform/fast3d/gfx_texture_cache_key.h`, add the field and its comparison:

```c
    bool cutout;                /* plain versus coverage-preserving mips */
    uint32_t override_generation; /* bumped when pack overrides toggle */
```

and in `dkr_texcache_key_equal()`:

```c
        left->mipmaps == right->mipmaps && left->cutout == right->cutout &&
        left->override_generation == right->override_generation;
```

This is what makes the `Tab` toggle correct rather than approximately correct:
the two variants of a texture are different cache entries, so toggling neither
mutates a live entry nor leaks the old one.

- [ ] **Step 2: Write the failing gate**

Create `tests/check_mod_texture_override.py`, modelled on an existing frame-hash
check — read `tests/check_texture_lineswap.py` first for the established shape
of "boot headless, capture frames, compare hashes". It must:

1. Build a temporary pack under a git-ignored scratch directory containing one
   solid-magenta PNG named for a digest the run itself reports.
2. Run the binary once with `MDKR_MOD_TEXTURE_DUMP=<dir>` to learn a real digest
   from the current ROM, so the test never hard-codes ROM-derived data.
3. Run with the pack installed and assert the captured frame differs from the
   no-pack baseline.
4. Run with the pack installed and overrides toggled off and assert the frame is
   **byte-identical** to the baseline.
5. Assert the upload count with the toggle exercised 10 times stays under a
   bound, so toggling cannot become a re-upload storm.

Every invocation uses `MDKR_AUDIO=0` and `--headless-frames`.

- [ ] **Step 3: Run it to verify it fails**

```bash
MDKR_AUDIO=0 python3 tests/check_mod_texture_override.py
```

Expected: FAIL — the override store does not exist, so the pack frame equals the
baseline frame.

- [ ] **Step 4: Vendor stb_image and record it**

Download `stb_image.h` from the upstream `nothings/stb` repository at a pinned
commit. Add to `lib/stb/stb_image.h`. Add a row to `THIRD_PARTY.md` and
`NOTICE.md` naming the component, its licence (MIT/public-domain dual), the
pinned commit, and what it is used for. `tools/check_third_party_notices.py`
enforces that both files agree — run it.

- [ ] **Step 5: Implement the store**

```c
/* mod_texture_store.h — decoded pack textures, keyed by content digest. */
#ifndef MDKR64_MOD_TEXTURE_STORE_H
#define MDKR64_MOD_TEXTURE_STORE_H

#include "mod_registry.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct MdkrModTexture {
    const uint8_t *rgba;   /* store-owned; valid until shutdown */
    int width, height;
} MdkrModTexture;

void mdkr_mod_texture_store_init(const MdkrModRegistry *registry);
void mdkr_mod_texture_store_shutdown(void);

/* 1 and fills `out` when an enabled pack provides `digest_hex`; 0 otherwise.
 * Returns 0 unconditionally while overrides are disabled. */
int  mdkr_mod_texture_lookup(const char *digest_hex, MdkrModTexture *out);

void     mdkr_mod_texture_set_enabled(bool enabled);
bool     mdkr_mod_texture_enabled(void);
/* Increments on every enable/disable. Feeds DkrTexCacheKey.override_generation. */
uint32_t mdkr_mod_texture_generation(void);

#endif /* MDKR64_MOD_TEXTURE_STORE_H */
```

Implementation notes that the gate depends on:

- Decode lazily on first lookup and cache the RGBA8 result keyed by digest.
  Decoding every pack PNG at startup would add seconds to launch for a large
  pack.
- Cap total decoded bytes (start at 512 MiB) and evict least-recently-used past
  the cap, so a 4K pack cannot exhaust memory silently.
- `stbi_load_from_memory` with `desired_channels = 4`. A decode failure logs the
  digest and the stb reason once and is then treated as absent — a broken PNG
  must not repeat-log every frame.
- Guard the whole store with the existing single-threaded assumption; do not add
  a mutex. `docs/ARCHITECTURE_DECISIONS.md` records the cooperative
  single-thread decision and this module inherits it.

- [ ] **Step 6: Hook the renderer**

In `platform/fast3d/gfx_pc_dkr.c`, at the point where a texture is about to be
uploaded, compute the digest and consult the store:

```c
    key.override_generation = mdkr_mod_texture_generation();
    /* ... existing cache lookup on `key` ... */

    /* Miss: decide the source before decoding ROM texels. */
    char digest[33];
    mdkr_mod_texture_digest(&key, source_texels, source_size_bytes, digest);
    MdkrModTexture over;
    if (mdkr_mod_texture_lookup(digest, &over)) {
        upload_rgba8(over.rgba, over.width, over.height);
    } else {
        /* existing ROM decode + upload path, unchanged */
    }
```

Keep the ROM path byte-for-byte unchanged in the miss case. If the diff shows
the original path being restructured rather than wrapped, revert and re-do it —
a regression there is a regression in every texture in the game.

- [ ] **Step 7: Run the gate to verify it passes**

```bash
MDKR_AUDIO=0 python3 tests/check_mod_texture_override.py
```

Expected: PASS.

- [ ] **Step 8: Prove the positive control**

Comment out the `mdkr_mod_texture_lookup()` branch only, rebuild, re-run the
gate, and confirm it **fails**. Paste that output into the commit message body.
Restore the branch and rebuild.

- [ ] **Step 9: Run the texture regressions**

```bash
MDKR_AUDIO=0 python3 tests/check_texture_lineswap.py
MDKR_AUDIO=0 python3 tests/check_mip_motion.py
MDKR_AUDIO=0 python3 tests/check_texture_edge_classification.py
MDKR_AUDIO=0 python3 tests/check_determinism.py
```

All four must pass unchanged — the override layer is inert with no packs
installed, and these prove it.

- [ ] **Step 10: Register the gate and commit**

Add to `tools/run_checks.py`'s `CHECKS` tuple:

```python
    Check("mod_texture_override", "check_mod_texture_override.py", "rom",
          "content-pack texture overrides apply, toggle live, and restore exactly"),
```

```bash
git add platform/mod_texture_store.h platform/mod_texture_store.c \
        platform/fast3d/gfx_texture_cache_key.h platform/fast3d/gfx_pc_dkr.c \
        lib/stb/stb_image.h tests/check_mod_texture_override.py \
        tools/run_checks.py CMakeLists.txt THIRD_PARTY.md NOTICE.md
python3 tools/check_public_surface.py --staged
git commit
```

---

## Task 5: The `Tab` toggle

**Files:**
- Modify: `platform/platform_sdl_min.c`, `platform/app/ui_overlay.cpp`

- [ ] **Step 1: Extend the gate** — add an assertion to
  `tests/check_mod_texture_override.py` that drives a synthetic `Tab` keypress
  through the same input path the existing overlay tests use (read
  `tests/check_app_ui_input.py` for how a key is injected headlessly) and
  asserts the frame flips and flips back.

- [ ] **Step 2: Run it, verify it fails.**

- [ ] **Step 3: Implement.** In `platform/platform_sdl_min.c`, beside the
  existing `F1`/`F10` handling, add `SDLK_TAB` → `mdkr_mod_texture_set_enabled(
  !mdkr_mod_texture_enabled())`. Suppress it while the overlay has keyboard
  focus so `Tab` still navigates the settings UI.

- [ ] **Step 4: Run the gate, verify it passes.**

- [ ] **Step 5: Verify the overlay still tabs between fields** by running
  `MDKR_AUDIO=0 python3 tests/check_app_ui_input.py`.

- [ ] **Step 6: Commit.**

---

## Task 6: Zip pack containers (M3)

**Files:**
- Create: `platform/mod_source.h`, `platform/mod_source.c`,
  `lib/miniz/miniz.h`, `lib/miniz/miniz.c`, `tests/test_mod_source_zip.c`
- Modify: `platform/mod_registry.c`, `platform/mod_texture_store.c`,
  `cmake/tests.cmake`, `CMakeLists.txt`, `THIRD_PARTY.md`, `NOTICE.md`

- [ ] **Step 1: Write the failing test.** `tests/test_mod_source_zip.c` builds a
  zip in memory with miniz's writer and asserts:
  - a stored and a deflated entry both read back byte-identically;
  - an entry named `../escape.png` is **rejected**, not resolved;
  - an entry named `/etc/passwd` is rejected;
  - an entry whose name contains a backslash is rejected;
  - a truncated central directory yields a clean failure, not a crash;
  - a zip-bomb entry declaring 4 GiB uncompressed is refused above a 256 MiB
    per-entry cap.

- [ ] **Step 2: Run it, verify it fails.**

- [ ] **Step 3: Vendor miniz**, pinned by commit, recorded in `THIRD_PARTY.md`
  and `NOTICE.md`. Run `python3 tools/check_third_party_notices.py`.

- [ ] **Step 4: Implement `mod_source.c`** — one interface,
  `mdkr_mod_source_open(path, is_zip)`, `mdkr_mod_source_read(src, rel, buf,
  cap, out_len)`, `mdkr_mod_source_has(src, rel)`, `mdkr_mod_source_close(src)`.
  Directory sources use the Task 2 path logic; zip sources use miniz. Path
  validation lives in **one** function used by both, so a traversal fix can
  never apply to one and not the other.

- [ ] **Step 5: Retarget the registry and store** at `mod_source` rather than
  raw paths. `mdkr_mod_registry_resolve()` gains a sibling
  `mdkr_mod_registry_open_file()` returning a source + relative path pair.

- [ ] **Step 6: Run the unit test and the M2 gate.** Both must pass.

- [ ] **Step 7: Add a zip arm to `check_mod_texture_override.py`** — the same
  pack, zipped, must produce the identical frame hash as the directory pack.
  This is the assertion that keeps the two source kinds honest.

- [ ] **Step 8: Commit.**

---

## Task 7: Settings → Content

**Files:**
- Modify: `platform/video_config.h`, `platform/video_config.c`,
  `platform/app/ui_settings.cpp`, `platform/app/app_ui_policy.cpp`
- Test: extend `tests/test_video_config.c`, `tests/test_app_ui_policy.cpp`

- [ ] **Step 1: Append the keys** to `MdkrVideoKey`, at the end, with the
  comment block above the append point honoured:

```c
    MDKR_CONTENT_PACKS_ENABLED,
    MDKR_CONTENT_PACK_DISABLED,
```

- [ ] **Step 2: Add schema rows** in `platform/video_config.c`:

```c
    [MDKR_CONTENT_PACKS_ENABLED] = {
        "Content.PacksEnabled", "MDKR_CONTENT_PACKS",
        MDKR_VIDEO_TYPE_INT, MDKR_VIDEO_SCOPE_LIVE, 0.0f, 1.0f,
        "Custom content",
        "Apply installed content packs. Tab toggles this while you play.",
        MDKR_VIDEO_CAT_FIDELITY
    },
    [MDKR_CONTENT_PACK_DISABLED] = {
        "Content.PackDisabled", "MDKR_CONTENT_PACK_DISABLED",
        MDKR_VIDEO_TYPE_STRING, MDKR_VIDEO_SCOPE_RESTART, 0.0f, 0.0f,
        "Disabled packs",
        "Comma-separated pack names to skip. Set from the Content list.",
        MDKR_VIDEO_CAT_FIDELITY
    },
```

Add the matching entries to the default/range/text tables further down the file
— `tests/test_video_config.c` already asserts every key has one, so a missed
table shows up as a unit-test failure rather than a runtime surprise.

- [ ] **Step 3: Add the Content section** to `platform/app/ui_settings.cpp`
  using the existing `drawSettingsSectionHeader()` + `drawKey()` helpers, plus a
  read-only list rendering `mdkr_mod_registry_entry()` for each pack (name,
  author, version, priority) and each skip with its reason. Skipped packs must
  be visible — US-2 exists because a silently ignored pack is the single most
  common modding support question.

- [ ] **Step 4: Hide `Video.TexturePack`** — it is superseded. In
  `platform/app/app_ui_policy.cpp` it already returns `false`; now also mark the
  schema description as superseded by `Content.PacksEnabled` so the INI comment
  tells an upgrading player where the setting went.

- [ ] **Step 5: Run the UI and config tests.**

```bash
ctest --test-dir build-rel -R 'video_config|app_ui_policy' --output-on-failure
MDKR_AUDIO=0 python3 tests/check_app_ui_input.py
```

- [ ] **Step 6: Commit.**

---

## Task 8: Custom music (M4)

**Files:**
- Create: `platform/mod_music.h`, `platform/mod_music.c`,
  `lib/dr_libs/dr_wav.h`, `tests/check_mod_music_override.py`
- Modify: `platform/audio_seqplayer.c` (call site only),
  `CMakeLists.txt`, `tools/run_checks.py`, `THIRD_PARTY.md`, `NOTICE.md`

**Interfaces:**
- Consumes: `mdkr_mod_registry_open_file()` (Task 6).
- Produces: `mdkr_mod_music_begin(sequence_id)`, `mdkr_mod_music_active()`,
  `mdkr_mod_music_mix(int16_t *out, int frames)`, `mdkr_mod_music_stop()`.

- [ ] **Step 1: Write the failing gate.** `tests/check_mod_music_override.py`
  must, with `MDKR_AUDIO=0` throughout (the mixer runs; no device opens — read
  `tests/check_raw16_audio.py` for how an existing check captures PCM without a
  device):
  1. capture the PCM a known sequence produces with no pack;
  2. install a pack with a synthesised `music/<id>.wav` generated by the test
     itself (a 440 Hz sine — authored by the test, so no ROM audio is involved);
  3. assert the captured PCM matches the synthesised WAV and not the baseline;
  4. assert `Music volume = 0` silences it exactly as it silences the sequence;
  5. assert `check_state_hash`-style state streaming is byte-identical with and
     without the pack, proving the replacement is presentation-only.

- [ ] **Step 2: Run it, verify it fails.**

- [ ] **Step 3: Vendor dr_wav**, recorded in `THIRD_PARTY.md`/`NOTICE.md`.

- [ ] **Step 4: Implement `mod_music.c`.** Decode to the mixer's native rate and
  channel count at load, with a linear resampler — the mixer's rate is fixed at
  init, so resampling once at load beats resampling per callback. Cap decoded
  music at 64 MiB per track and refuse above it with a logged reason.

- [ ] **Step 5: Hook the sequence start path.** In `platform/audio_seqplayer.c`,
  where a sequence begins, call `mdkr_mod_music_begin(id)`; when it returns 1,
  the sequence still advances its own state (so timing, events and any
  sequence-driven gameplay hooks are unchanged) but its output is muted and
  `mdkr_mod_music_mix()` supplies the samples instead. **Do not** skip running
  the sequence — muting output and skipping the player are different, and only
  the first keeps determinism.

- [ ] **Step 6: Run the gate and the audio regressions.**

```bash
MDKR_AUDIO=0 python3 tests/check_mod_music_override.py
MDKR_AUDIO=0 python3 tests/check_audio_output.py
MDKR_AUDIO=0 python3 tests/check_raw16_audio.py
MDKR_AUDIO=0 python3 tests/check_determinism.py
```

- [ ] **Step 7: Positive control** — stub `mdkr_mod_music_begin()` to return 0,
  rebuild, confirm the new gate fails, restore.

- [ ] **Step 8: Register in `CHECKS` and commit.**

---

## Task 9: Author tooling — `tools/mod_texture_dump.py`

**Files:**
- Create: `tools/mod_texture_dump.py`
- Modify: `platform/mod_texture_store.c` (dump hook), `.gitignore`

- [ ] **Step 1:** Add an `MDKR_MOD_TEXTURE_DUMP=<dir>` env path to the store
  that, when set, writes each texture it is asked about as
  `<dir>/<digest>.png` alongside a `<digest>.txt` recording width, height,
  `fmt`, `siz` and where it was first seen. Encode PNG with stb_image_write
  (vendor `lib/stb/stb_image_write.h` the same way).

- [ ] **Step 2:** Write `tools/mod_texture_dump.py` — a thin wrapper that runs
  the binary headlessly over a caller-supplied input script with the env var
  set, then prints a summary count.

- [ ] **Step 3:** Add `mods/` and the default dump directory to `.gitignore`.

- [ ] **Step 4:** Run it once and confirm it produces files and that
  `git status` shows nothing new tracked.

- [ ] **Step 5: Commit.**

---

## Task 10: Guard extension and release gate (M5)

**Files:**
- Create: `tests/check_mod_content_absence.py`
- Modify: `tools/check_clean_room.sh`, `tools/check_no_rom.sh`,
  `tools/run_checks.py`, `docs/RELEASE_CHECKLIST.md`

This task is the reason the sprint is allowed to exist. Do not defer it.

- [ ] **Step 1: Write the failing gate.** `tests/check_mod_content_absence.py`
  must assert:
  1. `git ls-files mods/` is empty — no pack content is tracked;
  2. `mods/` is matched by `.gitignore` (`git check-ignore -q mods`);
  3. the packaged artifacts produced by `tools/package_linux_appimage.sh` and
     `tools/package_windows_zip.sh` contain no `pack.ini`, no `.png` under a
     `textures/` path, and no `mods/` entry;
  4. a deliberately planted `mods/planted/pack.ini` **is** caught by
     `tools/check_clean_room.sh` when force-added — the positive control, run in
     a scratch clone so the working tree is untouched.

- [ ] **Step 2: Run it, verify assertion 4 fails** (the guard does not know
  about `mods/` yet).

- [ ] **Step 3: Extend `tools/check_clean_room.sh`** with the tracked-`mods/`
  rule, and `tools/check_no_rom.sh` with the packaged-artifact rule.

- [ ] **Step 4: Run the gate, verify it passes**, then run the existing guards
  on the clean tree to confirm no false positive:

```bash
bash tools/check_clean_room.sh
bash tools/check_no_rom.sh
```

- [ ] **Step 5: Register in `CHECKS`, add the row to
  `docs/RELEASE_CHECKLIST.md`, and commit.**

---

## Task 11: `docs/MODDING.md` and the README section

**Files:**
- Create: `docs/MODDING.md`
- Modify: `README.md`, `docs/README.md`

- [ ] **Step 1: Write `docs/MODDING.md`** covering: the `mods/` location per
  platform; directory vs. zip packs; every `pack.ini` key with its default and
  range; priority and tie-breaking; the texture digest and how to obtain one
  with `tools/mod_texture_dump.py`; the `music/<id>.wav` naming; the memory
  caps; and the explicit statement that the project hosts no packs and ships no
  content.

- [ ] **Step 2: Add a "Custom content" section to `README.md`** — four or five
  sentences, player-facing language only, no gate or validation vocabulary.

- [ ] **Step 3: Add both to the `docs/README.md` index tables.**

- [ ] **Step 4: Run the link checker.**

```bash
python3 tools/check_markdown_links.py
```

- [ ] **Step 5: Commit.**

---

## Task 12: Full-suite verification

- [ ] **Step 1: Run the complete suite.** It is sequential by design — do not
  parallelise it; concurrent suites race on `save/eeprom.bin` and produce fake
  save-path failures.

```bash
rm -f save/eeprom.bin
MDKR_AUDIO=0 python3 tools/run_checks.py
```

- [ ] **Step 2: Paste the terminal summary into the final commit body.** Any
  non-zero exit, `[CRASH]`, `[FATAL]`, or missing assertion line is a failure —
  fix it here, not in a follow-up.

- [ ] **Step 3: Confirm the inert case.** With `mods/` absent entirely, the
  frame hashes from `check_texture_lineswap`, `check_race_drive` and
  `check_state_hash` must equal their pre-sprint values. If any moved, the
  override layer is not inert and the sprint is not done.

---

## Self-review

**Spec coverage.** M1 → Tasks 1–2. M2 → Tasks 3–5. M3 → Task 6. M4 → Task 8.
M5 → Tasks 10–11. Settings surface (US-2, US-4) → Task 7. Authoring (US-6) →
Task 9. Suite integrity → Task 12.

**Known gaps left deliberately out of scope**, to be raised as open items rather
than silently dropped:

- **Model and character replacement.** Needs an import format and a rigging
  story; a texture-and-audio pipeline is the useful 80% and does not commit the
  project to either.
- **Ogg/Vorbis music.** WAV first because it needs no decoder beyond dr_wav and
  proves the seam. Add `stb_vorbis` in a follow-up once the seam is gated.
- **Browser packs.** The wasm build has no writable `mods/`; wiring packs
  through IDBFS is its own sprint and would otherwise silently halve the
  feature's platform coverage.

**Type consistency.** `mdkr_mod_registry_resolve()` (Task 2) is superseded by
`mdkr_mod_registry_open_file()` in Task 6 — Task 6 Step 5 states the retarget
explicitly rather than leaving two spellings live.
`MDKR_MOD_TEXTURE_DIGEST_VERSION` is defined in Task 3 and referenced only
there. `override_generation` is added to `struct DkrTexCacheKey` in Task 4
Step 1 and consumed in Task 4 Step 6 and Task 5.

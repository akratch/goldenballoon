/* mod_registry.h — the ordered set of installed content packs.
 *
 * Discovery is read-only and failure-isolating: one unreadable or malformed
 * pack disables itself and records a reason, and every other pack still loads.
 * A missing `mods/` directory is the ordinary case and returns success with
 * zero packs, because the overwhelming majority of installs have none.
 *
 * Two traps this module exists to close. The first is silent loss: a pack that
 * is dropped without a reason looks to the player exactly like a pack that is
 * loaded and doing nothing, so every rejection lands in the skip table with
 * text a human can act on. The second is escape: a pack names the files it
 * overrides, so those names are attacker-controlled input to a path join.
 * Every path a pack supplies goes through one validator before any filesystem
 * call, and there is deliberately only one, so a future fix cannot land on one
 * caller and miss another. That validator is
 * `mdkr_mod_source_path_is_safe()`; this module used to carry a second, private
 * copy of it and no longer does, because two validators are exactly the shape
 * of defect the first one's comment warned about.
 *
 * A pack is either a directory or a `.zip`, and past discovery nothing here
 * knows which: every byte a pack serves is read through `mod_source.h`, so the
 * two kinds cannot answer differently about what a name resolves to.
 */
#ifndef MDKR64_MOD_REGISTRY_H
#define MDKR64_MOD_REGISTRY_H

#include "mod_manifest.h"
#include "mod_source.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define MDKR_MOD_MAX_PACKS 64
#define MDKR_MOD_PATH_MAX  1024

typedef struct MdkrModEntry {
    MdkrModManifest manifest;
    /* The pack directory, or the `.zip` file itself. Either way it is what
     * `mdkr_mod_source_open(root, is_zip)` takes, which is why the flag next to
     * it is the same flag that function's parameter is. */
    char root[MDKR_MOD_PATH_MAX];
    int  is_zip;
    /* A Rice/GLideN64 pack: no manifest, textures named by the identity the
     * running game computes. Its index is built after the entries are sorted,
     * so this flag is how the second pass knows which roots to walk. */
    int  is_rice;
    /* 1 once every name this pack serves is in the registry's name index
     * (see mdkr_mod_registry_name_packs). 0 when the walk could not finish --
     * out of memory, or a root that would not reopen -- in which case the
     * index answers "maybe" for this pack rather than a confident "no". */
    int  names_indexed;
    /* Names of the form textures/<32 characters>.png: the files a digest
     * lookup can match. Zero lets the texture store skip that probe for this
     * pack entirely, which for a Rice pack is every probe it would make. */
    int  digest_textures;
    /* Files under textures/ (any case) ending in .png that are NOT named by a
     * content digest, so no lookup can ever reach them -- counted at scan,
     * with the first one's name, so the player can be told why. Zero for a
     * Rice pack, whose own convention names its files. */
    int  textures_skipped;
    char textures_skipped_first[128];
} MdkrModEntry;

/* One texture in a Rice/GLideN64 high-resolution pack.
 *
 * A Rice pack has no manifest and no content digests: it names each texture by
 * an identity the RUNNING GAME computes -- `<rom>#<crc>#<fmt>#<siz>` -- and
 * nests the files in whatever folders its author chose. So the pack has to be
 * indexed before any of it can be found, which is what this array is.
 *
 * The three variants are the format's own, not an invention here: `_all` is a
 * complete image; `_rgb` carries colour with `_a` carrying alpha in its RED
 * channel; and an `_rgb` with no partner is opaque. `_all` wins when a pack
 * supplies both, because it is the one the author exported last. */
typedef struct MdkrModRiceTexture {
    uint32_t crc;
    int      fmt;
    int      siz;
    int      pack;            /* index into entries[] */
    char    *all;             /* NULL when the pack supplies no _all */
    char    *rgb;             /* NULL when the pack supplies no _rgb */
    char    *alpha;           /* NULL when the pack supplies no _a */
} MdkrModRiceTexture;

/* One indexed name: where its text sits in the pool, its hash, and which
 * packs (bit i = entries[i]) serve it. Private to mod_registry.c; declared
 * here only because the registry is a plain value its owners embed. */
typedef struct MdkrModNameSlot {
    uint64_t packs;     /* 0 marks an empty slot */
    uint32_t hash;      /* of the key */
    uint32_t offset;    /* the key, in name_pool */
    uint32_t spelling;  /* the name as these packs store it; == offset unless
                         * the key was folded to lowercase */
} MdkrModNameSlot;

typedef struct MdkrModRegistry {
    MdkrModEntry entries[MDKR_MOD_MAX_PACKS];
    int          count;
    char         skip_name[MDKR_MOD_MAX_PACKS][MDKR_MOD_NAME_MAX];
    char         skip_reason[MDKR_MOD_MAX_PACKS][128];
    int          skipped;
    /* Owned here and freed by shutdown(). Empty unless a Rice pack is
     * installed, so a player with only digest-keyed packs pays nothing. */
    MdkrModRiceTexture *rice;
    int                 rice_count;
    int                 rice_capacity;
    /* Every file name every pack serves, built at scan and owned here: an
     * open-addressed table over one string pool. Empty with no packs. */
    MdkrModNameSlot *names;
    size_t           name_capacity;   /* a power of two, or 0 */
    size_t           name_count;
    char            *name_pool;
    size_t           name_pool_size;
    size_t           name_pool_capacity;
    /* Packs whose walk did not finish (names_indexed == 0). Every name
     * query reports them as possible holders rather than guessing no. */
    uint64_t         names_unindexed;
} MdkrModRegistry;

/* Scans `mods_dir`. Returns 0 on success including "directory absent".
 *
 * `*reg` is fully overwritten, so an uninitialised registry is safe to pass.
 * Entries come back sorted ascending by priority, ties broken by ASCII
 * case-insensitive directory name so the order never depends on what order
 * the filesystem happened to enumerate. A pack whose manifest sets
 * `enabled = 0` is still discovered and listed here — the player installed it
 * and should see it — but never wins a lookup.
 *
 * Both shapes a pack can take are discovered here: a subdirectory holding
 * `pack.ini`, and a `.zip` holding `pack.ini` at its root. Nothing else in
 * `mods_dir` is a pack, and a loose file that is not a `.zip` is not reported
 * as a broken one — players keep readmes and screenshots next to their packs.
 *
 * The registry itself owns no open file handles: a source is opened for the
 * manifest read and closed again before this returns. */
int mdkr_mod_registry_init(MdkrModRegistry *reg, const char *mods_dir);

/* Read-only admission through an already-open source; never reopens a path.
 * Borrows source. Returns 1 for an admitted manifest or Rice texture pack,
 * otherwise 0 with a bounded reason. Outputs are required. A name starting
 * with '.' is refused: the scan above skips such entries, so admitting one
 * would install a pack the game never loads. */
int mdkr_mod_registry_probe_source(MdkrModSource *source, const char *name,
    MdkrModManifest *manifest, int *is_rice, char *reason, size_t reason_size);
void mdkr_mod_registry_shutdown(MdkrModRegistry *reg);

int  mdkr_mod_registry_count(const MdkrModRegistry *reg);

/* Rice/GLideN64 lookup: the texture a pack supplies for the identity the game
 * just computed, or NULL when no installed pack has it.
 *
 * `fmt`/`siz` are the RDP format and size codes of the tile being uploaded,
 * and they are part of the identity -- the same CRC under a different format
 * is a different texture, and matching on the CRC alone would hand the
 * renderer somebody else's picture. */
const MdkrModRiceTexture *mdkr_mod_registry_rice_lookup(
    const MdkrModRegistry *reg, uint32_t crc, int fmt, int siz);

/* How many Rice identities an ENABLED pack supplies. Zero when none is
 * installed or the player switched it off, which is what the texture store
 * tests before it computes a key at all -- so a disabled pack costs nothing
 * per texture upload rather than a hash and a failed lookup. */
int  mdkr_mod_registry_rice_count(const MdkrModRegistry *reg);

/* How many identities the pack at `index` supplies, enabled or not. The panel
 * shows this per row: a high-resolution pack carries no pack.ini to name a
 * version or an author, so the texture count is the only thing that
 * distinguishes "it loaded" from "it loaded and is empty". */
int  mdkr_mod_registry_pack_rice_count(const MdkrModRegistry *reg, int index);
const MdkrModEntry *mdkr_mod_registry_entry(const MdkrModRegistry *reg, int i);
int  mdkr_mod_registry_skipped(const MdkrModRegistry *reg);
const char *mdkr_mod_registry_skip_reason(const MdkrModRegistry *reg, int i);

/* Which packs serve `relative_path`, from the index built at scan: bit i set
 * means entries[i] holds that file, or could not be fully indexed and so
 * might. Enabled or not -- the caller applies its own policy. No file I/O and
 * no allocation; 0 for a name no pack serves, for a name the validator
 * refuses, and for a shut-down registry.
 *
 * Matching is exact, except that a texture-digest name ignores case (see
 * name_hits() below), on every platform. The index is the scan's answer:
 * a file added or removed afterwards is seen at the next rescan, the same
 * point at which a new pack is. */
uint64_t mdkr_mod_registry_name_packs(const MdkrModRegistry *reg,
                                      const char *relative_path);

/* One way the packs spell a name: the packs that store it this way, and the
 * spelling to read it by. Points into the registry; valid until shutdown. */
typedef struct MdkrModNameHit {
    uint64_t    packs;
    const char *name;
} MdkrModNameHit;

/* Every spelling under which some pack serves `relative_path`, up to `max`;
 * returns how many were written. Usually one, spelled as asked. A name that is
 * a texture digest -- textures/<32 hex>.png -- matches whatever its case, in
 * the directory, the digest and the extension alike, because 1.7.0 found such
 * files through filesystems that ignore case; each hit then carries the
 * spelling the pack really uses. Every other name, Rice textures included,
 * matches exactly. No I/O. Packs that could not be indexed are not listed
 * here (see name_packs()). */
int mdkr_mod_registry_name_hits(const MdkrModRegistry *reg,
                                const char *relative_path,
                                MdkrModNameHit *out, int max);

/* The index of the highest-priority ENABLED pack the index says serves
 * `relative_path`, or -1. The same choice open_file() makes, without opening
 * anything. */
int mdkr_mod_registry_find_file(const MdkrModRegistry *reg,
                                const char *relative_path);

/* An open pack and the name to read out of it. `source` is owned by the caller
 * and must be released with mdkr_mod_registry_close_file(); `relative` is the
 * name that was asked for, carried alongside so a consumer can hand the pair
 * straight to mdkr_mod_source_read() without keeping the two in step itself. */
typedef struct MdkrModFile {
    MdkrModSource *source;
    char           relative[MDKR_MOD_PATH_MAX];
    /* Index into the registry of the pack that answered, for diagnostics. */
    int            pack_index;
} MdkrModFile;

/* Opens the highest-priority enabled pack that holds `relative_path`, whatever
 * shape that pack is. Returns 1 and fills `*out` when one does, 0 when none
 * does — including when the name itself is refused, which is checked before
 * any pack is touched.
 *
 * This is the lookup every content consumer uses. A source is opened per
 * successful call and closed by the caller, which for a zip means re-reading
 * its central directory each time; that is deliberate, because the alternative
 * — caching an open archive inside a registry callers hold by const pointer —
 * makes the file handle outlive a rescan. Consumers cache the *decision* (see
 * mod_texture_store.c's slot states and mod_music.c's per-track cache), so a
 * lookup happens once per asset, not once per use. */
int  mdkr_mod_registry_open_file(const MdkrModRegistry *reg,
                                 const char *relative_path,
                                 MdkrModFile *out);
/* Releases what open_file() returned. Safe on a zeroed MdkrModFile and safe to
 * call twice. */
void mdkr_mod_registry_close_file(MdkrModFile *file);

/* The OS path of the highest-priority enabled DIRECTORY pack holding
 * `relative_path`. Returns 1 and fills `out_path` when found, 0 when not.
 * `relative_path` uses '/' on every platform and goes through
 * mdkr_mod_source_path_is_safe() before any filesystem call. `out_path` is
 * emptied on every path that returns 0, so a caller that forgets to check the
 * return value reads an empty string rather than a stale one.
 *
 * Zip packs are invisible here, because an entry inside an archive has no path
 * for a caller to open. This exists for the one thing a path is still good for
 * — telling a person where a file came from — and is NOT the way to read pack
 * content: use mdkr_mod_registry_open_file(), which answers for both kinds. */
int mdkr_mod_registry_resolve(const MdkrModRegistry *reg,
                              const char *relative_path,
                              char *out_path, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MOD_REGISTRY_H */

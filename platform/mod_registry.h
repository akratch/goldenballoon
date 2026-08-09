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

#define MDKR_MOD_MAX_PACKS 64
#define MDKR_MOD_PATH_MAX  1024

typedef struct MdkrModEntry {
    MdkrModManifest manifest;
    /* The pack directory, or the `.zip` file itself. Either way it is what
     * `mdkr_mod_source_open(root, is_zip)` takes, which is why the flag next to
     * it is the same flag that function's parameter is. */
    char root[MDKR_MOD_PATH_MAX];
    int  is_zip;
} MdkrModEntry;

typedef struct MdkrModRegistry {
    MdkrModEntry entries[MDKR_MOD_MAX_PACKS];
    int          count;
    char         skip_name[MDKR_MOD_MAX_PACKS][MDKR_MOD_NAME_MAX];
    char         skip_reason[MDKR_MOD_MAX_PACKS][128];
    int          skipped;
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
void mdkr_mod_registry_shutdown(MdkrModRegistry *reg);

int  mdkr_mod_registry_count(const MdkrModRegistry *reg);
const MdkrModEntry *mdkr_mod_registry_entry(const MdkrModRegistry *reg, int i);
int  mdkr_mod_registry_skipped(const MdkrModRegistry *reg);
const char *mdkr_mod_registry_skip_reason(const MdkrModRegistry *reg, int i);

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

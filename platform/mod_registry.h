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
 * caller and miss another.
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

/* Scans `mods_dir`. Returns 0 on success including "directory absent".
 *
 * `*reg` is fully overwritten, so an uninitialised registry is safe to pass.
 * Entries come back sorted ascending by priority, ties broken by ASCII
 * case-insensitive directory name so the order never depends on what order
 * the filesystem happened to enumerate. A pack whose manifest sets
 * `enabled = 0` is still discovered and listed here — the player installed it
 * and should see it — but never wins a lookup. */
int mdkr_mod_registry_init(MdkrModRegistry *reg, const char *mods_dir);
void mdkr_mod_registry_shutdown(MdkrModRegistry *reg);

int  mdkr_mod_registry_count(const MdkrModRegistry *reg);
const MdkrModEntry *mdkr_mod_registry_entry(const MdkrModRegistry *reg, int i);
int  mdkr_mod_registry_skipped(const MdkrModRegistry *reg);
const char *mdkr_mod_registry_skip_reason(const MdkrModRegistry *reg, int i);

/* Highest-priority enabled pack holding `relative_path`. Returns 1 and fills
 * `out_path` when found, 0 when not. `relative_path` uses '/' on every
 * platform and is rejected if it contains '..' or a leading separator.
 * `out_path` is emptied on every path that returns 0, so a caller that
 * forgets to check the return value reads an empty string rather than a
 * stale one. */
int mdkr_mod_registry_resolve(const MdkrModRegistry *reg,
                              const char *relative_path,
                              char *out_path, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MOD_REGISTRY_H */

/* asset_subentry.h — bounds-checked access to one entry inside an asset section.
 *
 * Section indices have always been checked (`asset_section_bounds()` in
 * game/src/asset_loading.c); entry indices inside a section have not. That gap
 * is currently unreachable because unsupported ROM revisions are refused before
 * engine boot (platform/rom_id.c), and it becomes reachable the moment the
 * supported set widens: this build is compiled for us.v80 data and asks for
 * us.v80 indices, while us.v77's GAME_TEXT holds 259 entries where us.v80 holds
 * 343 (docs/ROM_REVISIONS.md §5-6).
 *
 * A loud abort is strictly better than the silent out-of-range read it replaces,
 * and better than the *silent in-range substitute* the one existing check
 * performed: get_misc_asset() compared the index against gAssetsMiscTableLength
 * and then returned the section base when it failed, converting an out-of-range
 * read into confidently wrong data. This codebase's dominant defect shape is the
 * silent one; see docs/open-items/portability.md, wave "subentry".
 *
 * Nothing here fires on a supported revision. Every index the port asks for on
 * us.v80 is in range by construction, so an abort from this file means either a
 * ROM whose section is shorter than the build expects, or a genuine indexing
 * bug. Both are worth stopping for.
 */
#ifndef MDKR64_ASSET_SUBENTRY_H
#define MDKR64_ASSET_SUBENTRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define MDKR_SUBENTRY_NORETURN __attribute__((noreturn))
#else
#define MDKR_SUBENTRY_NORETURN
#endif

/* One asset section, as the sub-entry accessor needs to see it.
 *
 * `offsets` holds entry_count + 1 values: entry `i` spans
 * [offsets[i], offsets[i + 1]). The trailing value is the section terminator in
 * the tables that carry one, which is why a size is derived rather than trusted
 * (see mdkr_asset_subentry).
 *
 * `offset_scale` is the number of BYTES one unit of `offsets` represents. The
 * ASSET_MISC table stores s32-WORD offsets, so it passes 4; a byte-offset table
 * passes 1. Zero is read as 1, so a brace-initialised descriptor that omits the
 * field behaves as byte offsets rather than collapsing every entry onto the
 * section base. */
typedef struct MdkrAssetSection {
    const char *name;        /* e.g. "GAME_TEXT" */
    const uint8_t *base;
    uint32_t entry_count;
    const uint32_t *offsets; /* entry_count + 1 entries */
    uint32_t offset_scale;   /* bytes per unit in `offsets`; 0 means 1 */
} MdkrAssetSection;

/* Returns the entry base, or aborts with a named diagnostic. Never returns
 * NULL: a caller that could handle absence would have checked already.
 *
 * `out_size` (optional) receives the entry's byte length, or 0 when the next
 * offset is the section terminator or does not advance — the same answer
 * get_misc_asset_size() has always given for those, and not an error. */
const uint8_t *mdkr_asset_subentry(const MdkrAssetSection *section,
                                   uint32_t index, uint32_t *out_size);

/* The same diagnostic and the same abort, for a site whose addressing this
 * header cannot describe (a rebuilt pointer array, a two-level table). The
 * caller keeps its own `index >= count` comparison — which is also what makes
 * the site legible to tools/sweep_subentry_access.py — and calls this on the
 * failing arm. `where` names the function, so the message identifies the site
 * and not just the section. */
MDKR_SUBENTRY_NORETURN void mdkr_asset_subentry_out_of_range(
    const char *section_name, long long index, long long entry_count,
    const char *where);

/* Negative control (tests/check_subentry_bounds.py). Reads
 * MDKR_SUBENTRY_TEST_INDEX and, when it is set, drives that index through
 * mdkr_asset_subentry() over a synthetic one-entry section. No-op otherwise, so
 * a normal run never touches it. */
void mdkr_asset_subentry_env_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_ASSET_SUBENTRY_H */

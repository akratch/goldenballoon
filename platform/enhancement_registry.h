/* enhancement_registry.h — every optional player-facing behaviour, in one table.
 *
 * The AUTHORITY CLASS is the point of this module. An enhancement that claims
 * PRESENTATION is asserted, by a gate generated from this table, to leave the
 * authoritative [SIMHASH] v3 stream byte-identical. An enhancement that claims
 * GAMEPLAY is asserted to change it. Neither claim is checked by hand, so the
 * table cannot drift away from what the gates test.
 */
#ifndef MDKR64_ENHANCEMENT_REGISTRY_H
#define MDKR64_ENHANCEMENT_REGISTRY_H

#include "video_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MdkrEnhAuthority {
    /* Cannot move authoritative state. Proven, not asserted. */
    MDKR_ENH_PRESENTATION = 0,
    /* Deliberately changes how the game plays. Excluded from parity gates
     * BY THIS DECLARATION, which is why it is not a comment. */
    MDKR_ENH_GAMEPLAY
} MdkrEnhAuthority;

typedef enum MdkrEnhCategory {
    MDKR_ENH_CAT_DISPLAY = 0,
    MDKR_ENH_CAT_DIFFICULTY,
    MDKR_ENH_CAT_COSMETIC
} MdkrEnhCategory;

typedef struct MdkrEnhancement {
    MdkrVideoKey     key;       /* persistence, env override, INI round-trip */
    const char      *label;     /* player-facing, no process vocabulary */
    const char      *help;      /* one sentence, player-facing */
    MdkrEnhAuthority authority;
    MdkrEnhCategory  category;
    /* A non-default value the authority gate flips this row to.
     *
     * Declared here rather than in the gate on purpose. A list of test values
     * living in the test is a second list that drifts: add a row, forget the
     * list, and the gate silently exercises one fewer enhancement while still
     * reporting a pass. Keeping it in the row means adding an enhancement
     * forces you to say how to exercise it, and the row-completeness test
     * fails if you do not. */
    const char      *probe_value;
} MdkrEnhancement;

int                     mdkr_enhancement_count(void);
const MdkrEnhancement  *mdkr_enhancement_at(int index);
/* NULL when `key` is not an enhancement. */
const MdkrEnhancement  *mdkr_enhancement_for_key(MdkrVideoKey key);

/* Emits one `[ENHTABLE] key=... authority=... category=...` line per row.
 *
 * This exists so check_enhancement_authority.py enumerates the table the
 * RUNNING BINARY has, not a copy of it kept in the test. A test carrying its
 * own list of enhancements would keep passing after someone adds a row and
 * forgets the gate, which is the exact drift the authority class is meant to
 * make impossible. Driven by MDKR_ENH_DUMP_TABLE=1 at startup. */
void                    mdkr_enhancement_dump_table(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_ENHANCEMENT_REGISTRY_H */

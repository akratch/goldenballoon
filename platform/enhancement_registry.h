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
} MdkrEnhancement;

int                     mdkr_enhancement_count(void);
const MdkrEnhancement  *mdkr_enhancement_at(int index);
/* NULL when `key` is not an enhancement. */
const MdkrEnhancement  *mdkr_enhancement_for_key(MdkrVideoKey key);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_ENHANCEMENT_REGISTRY_H */

#ifndef MDKR_ROLLBACK_GAME_AUTHORITY_H
#define MDKR_ROLLBACK_GAME_AUTHORITY_H

#include <stdbool.h>

#include "rollback_snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_ROLLBACK_TAG_OBJECT_LIST UINT32_C(0x00410001)

/* Register the currently implemented real-game authority foundation. This is
 * valid only after object pools, object lists, settings and the level maps have
 * been initialized, and before match tick zero. Registration is atomic. */
bool mdkr_rollback_game_authority_register(
    MdkrRollbackSnapshotRegistry *registry);
/* Reject a newly reachable POOL_MAIN behaviour allocation that was not part of
 * the frozen registry. This turns dynamic authority growth into an immediate,
 * named unsupported-mode failure instead of a later replay desync. */
bool mdkr_rollback_game_authority_validate_dynamic_coverage(
    const MdkrRollbackSnapshotRegistry *registry);
bool mdkr_rollback_game_authority_is_input_tag(uint32_t tag);

#ifdef __cplusplus
}
#endif
#endif

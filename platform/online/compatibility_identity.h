/* Cross-platform release provenance -> online compatibility identity v1. */
#ifndef MDKR_ONLINE_COMPATIBILITY_IDENTITY_H
#define MDKR_ONLINE_COMPATIBILITY_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

#include "lobby_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Matches the browser publisher handoff exactly. Dirty/development provenance,
 * malformed semver/commit strings and unsupported ROM revisions fail without
 * changing output. The ROM itself must already have passed local SHA-256
 * validation; this function never receives ROM bytes or its digest. */
bool mdkr_online_compatibility_from_provenance(
    const char                *version,
    const char                *source_commit,
    bool                       source_dirty,
    uint8_t                    rom_revision,
    MdkrOnlineCompatibilityV1 *output);

#ifdef __cplusplus
}
#endif
#endif

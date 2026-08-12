/* Endpoint-local seats merged into a canonical four-slot input frame. */
#ifndef MDKR_NET_LOCAL_INPUT_H
#define MDKR_NET_LOCAL_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "../input_tick_queue.h"
#include "net_roster.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdkrCanonicalInputFrame {
    MdkrInputSample slots[MDKR_MATCH_SLOTS];
    uint8_t locally_authored_mask;
} MdkrCanonicalInputFrame;

/* Merge is copy-out and atomic. `canonical_base` is the transport/history
 * frame (normally neutral or remote-authored); only slots owned by local seats
 * may be replaced. Local samples are indexed by local seat, never by player
 * or viewport. On failure `out` is untouched. */
bool mdkr_net_local_input_merge(
    const MdkrNetRoster *roster,
    const MdkrInputSample *local_samples,
    unsigned local_sample_count,
    const MdkrInputSample canonical_base[MDKR_MATCH_SLOTS],
    MdkrCanonicalInputFrame *out);

#ifdef __cplusplus
}
#endif
#endif

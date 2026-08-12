/* Canonical four-slot rollback input history. Pure C; no transport or clock. */
#ifndef MDKR_NET_INPUT_H
#define MDKR_NET_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "../input_tick_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_NET_INPUT_SLOTS 4u
#define MDKR_NET_INPUT_CAPACITY 128u
#define MDKR_NET_INPUT_MAX_FUTURE 32u

typedef enum MdkrNetInputStatus {
    MDKR_NET_INPUT_ABSENT = 0,
    MDKR_NET_INPUT_PREDICTED,
    MDKR_NET_INPUT_RECEIVED
} MdkrNetInputStatus;

typedef enum MdkrNetInputSubmitResult {
    MDKR_NET_SUBMIT_ACCEPTED = 0,
    MDKR_NET_SUBMIT_CORRECTED,
    MDKR_NET_SUBMIT_DUPLICATE,
    MDKR_NET_SUBMIT_INVALID_SLOT,
    MDKR_NET_SUBMIT_TOO_OLD,
    MDKR_NET_SUBMIT_TOO_FAR_FUTURE,
    MDKR_NET_SUBMIT_CONFLICT
} MdkrNetInputSubmitResult;

typedef struct MdkrNetInputSet {
    uint32_t tick;
    MdkrInputSample samples[MDKR_NET_INPUT_SLOTS];
    uint8_t status[MDKR_NET_INPUT_SLOTS];
    bool all_received;
} MdkrNetInputSet;

typedef struct MdkrNetInputCell {
    uint32_t tick;
    MdkrInputSample samples[MDKR_NET_INPUT_SLOTS];
    uint8_t status[MDKR_NET_INPUT_SLOTS];
    bool occupied;
} MdkrNetInputCell;

typedef struct MdkrNetInputHistory {
    MdkrNetInputCell cells[MDKR_NET_INPUT_CAPACITY];
    uint32_t first_tick;
    uint32_t current_tick;
    uint32_t confirmed_through;
    uint32_t earliest_dirty_tick;
    bool have_confirmed;
    bool have_dirty;
} MdkrNetInputHistory;

void mdkr_net_input_init(MdkrNetInputHistory *history, uint32_t first_tick);
void mdkr_net_input_set_current_tick(MdkrNetInputHistory *history, uint32_t tick);
MdkrNetInputSubmitResult mdkr_net_input_submit(
    MdkrNetInputHistory *history, unsigned slot, uint32_t tick,
    const MdkrInputSample *sample);
bool mdkr_net_input_for_tick(
    MdkrNetInputHistory *history, uint32_t tick, MdkrNetInputSet *out);
/* Strict replay view: succeeds only for an already-materialized complete cell
 * and never predicts, allocates, advances, or otherwise mutates history. */
bool mdkr_net_input_copy_existing(
    const MdkrNetInputHistory *history, uint32_t tick, MdkrNetInputSet *out);
/* Atomically discard and re-derive only predicted cells in an already-authored
 * suffix after late corrections. Received samples are never rewritten. */
bool mdkr_net_input_rebuild_predictions(
    MdkrNetInputHistory *history, uint32_t first_tick, uint32_t last_tick);
bool mdkr_net_input_confirm_through(
    MdkrNetInputHistory *history, uint32_t tick);
bool mdkr_net_input_take_dirty(
    MdkrNetInputHistory *history, uint32_t *out_tick);

/* RFC-1982-style half-range ordering, shared by protocol tests. */
bool mdkr_net_tick_after(uint32_t candidate, uint32_t reference);

#ifdef __cplusplus
}
#endif
#endif

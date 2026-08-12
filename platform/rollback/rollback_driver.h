#ifndef MDKR_ROLLBACK_DRIVER_H
#define MDKR_ROLLBACK_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "rollback_events.h"
#include "rollback_ring.h"
#include "../net/net_input.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*MdkrRollbackSimulateFn)(
    void *context, uint32_t tick, const MdkrNetInputSet *inputs,
    bool resimulating);

typedef enum MdkrRollbackDriverError {
    MDKR_ROLLBACK_DRIVER_OK = 0,
    MDKR_ROLLBACK_DRIVER_SNAPSHOT_MISSING,
    MDKR_ROLLBACK_DRIVER_RESIM_LIMIT,
    MDKR_ROLLBACK_DRIVER_INPUT_UNAVAILABLE,
    MDKR_ROLLBACK_DRIVER_SIMULATION_FAILED,
    MDKR_ROLLBACK_DRIVER_CAPTURE_FAILED
} MdkrRollbackDriverError;

typedef struct MdkrRollbackDriverStats {
    uint64_t authored_ticks;
    uint64_t rollback_count;
    uint64_t resimulated_ticks;
    uint32_t deepest_rollback;
} MdkrRollbackDriverStats;

typedef struct MdkrRollbackDriver {
    MdkrNetInputHistory *inputs;
    MdkrRollbackRing *snapshots;
    MdkrRollbackEventJournal *events;
    MdkrRollbackSimulateFn simulate;
    void *context;
    uint32_t current_tick;
    uint8_t max_resimulation_ticks;
    MdkrRollbackDriverError error;
    MdkrRollbackDriverStats stats;
} MdkrRollbackDriver;

bool mdkr_rollback_driver_init(
    MdkrRollbackDriver *driver, MdkrNetInputHistory *inputs,
    MdkrRollbackRing *snapshots, MdkrRollbackEventJournal *events,
    MdkrRollbackSimulateFn simulate, void *context, uint32_t first_tick,
    unsigned max_resimulation_ticks);
/* Snapshot labels are completed authored boundaries: snapshot T contains state
 * after tick T. The caller must seed snapshot (first_tick - 1) before init. */
bool mdkr_rollback_driver_advance(MdkrRollbackDriver *driver);
/* Apply all retained late corrections without authoring another tick. */
bool mdkr_rollback_driver_reconcile(MdkrRollbackDriver *driver);

#ifdef __cplusplus
}
#endif
#endif

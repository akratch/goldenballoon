/* One source of truth for the production real-game rollback budget. */
#ifndef MDKR_ROLLBACK_LIMITS_H
#define MDKR_ROLLBACK_LIMITS_H

/* A correction needs the completed boundary immediately before its first
 * replayed tick. Thirty-two snapshots therefore retain at most thirty-one
 * replayed ticks, or an input whose authored-tick age is at most thirty. */
#define MDKR_ROLLBACK_SNAPSHOT_SLOTS 32u
#define MDKR_ROLLBACK_MAX_REPLAY_TICKS (MDKR_ROLLBACK_SNAPSHOT_SLOTS - 1u)
#define MDKR_ROLLBACK_MAX_INPUT_AGE_TICKS \
    (MDKR_ROLLBACK_MAX_REPLAY_TICKS - 1u)
/* Client-side authority memory is a product budget, not a best-effort malloc.
 * Sixteen MiB includes the fixed ModelInstance arena while still rejecting a
 * silently expanded authority surface. */
#define MDKR_ROLLBACK_MAX_RING_BYTES (16u * 1024u * 1024u)

#endif /* MDKR_ROLLBACK_LIMITS_H */

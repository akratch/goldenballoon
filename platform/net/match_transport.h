/* Launcher-owned canonical input adapter. Networking/authentication stays out
 * of the engine; this boundary accepts only already-authenticated peer input. */
#ifndef MDKR_MATCH_TRANSPORT_H
#define MDKR_MATCH_TRANSPORT_H

#include "net_input.h"
#include "rollback/rollback_limits.h"
#include "session/session_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Must match the real-game snapshot retention contract: a packet outside this
 * authored depth cannot be reconciled and is rejected before it can dirty the
 * engine timeline. */
#define MDKR_MATCH_TRANSPORT_ROLLBACK_TICKS \
    MDKR_ROLLBACK_MAX_INPUT_AGE_TICKS

typedef enum MdkrMatchRecoveryReason {
    MDKR_MATCH_RECOVERY_NONE = 0,
    MDKR_MATCH_RECOVERY_INPUT_GAP,
    MDKR_MATCH_RECOVERY_LATE_INPUT
} MdkrMatchRecoveryReason;

typedef struct MdkrMatchRecovery {
    MdkrMatchRecoveryReason reason;
    uint32_t first_unrecoverable_tick;
    uint32_t observed_at_tick;
    uint8_t canonical_slot;
} MdkrMatchRecovery;

typedef enum MdkrMatchTransportIngressResult {
    MDKR_MATCH_INGRESS_ACCEPTED = 0,
    MDKR_MATCH_INGRESS_CORRECTED,
    MDKR_MATCH_INGRESS_DUPLICATE,
    MDKR_MATCH_INGRESS_INVALID,
    MDKR_MATCH_INGRESS_STALE_EPOCH,
    MDKR_MATCH_INGRESS_UNAUTHORIZED,
    MDKR_MATCH_INGRESS_CONFLICT,
    MDKR_MATCH_INGRESS_OUT_OF_WINDOW,
    MDKR_MATCH_INGRESS_TAKEN_OVER
} MdkrMatchTransportIngressResult;

typedef enum MdkrMatchTakeoverResult {
    MDKR_MATCH_TAKEOVER_ACCEPTED = 0,
    MDKR_MATCH_TAKEOVER_DUPLICATE,
    MDKR_MATCH_TAKEOVER_INVALID,
    MDKR_MATCH_TAKEOVER_STALE_EPOCH,
    MDKR_MATCH_TAKEOVER_TOO_LATE,
    MDKR_MATCH_TAKEOVER_CONFLICT
} MdkrMatchTakeoverResult;

typedef struct MdkrMatchTransportStats {
    uint32_t accepted;
    uint32_t corrected;
    uint32_t duplicates;
    uint32_t invalid;
    uint32_t stale_epoch;
    uint32_t unauthorized;
    uint32_t conflicts;
    uint32_t out_of_window;
    uint32_t drained;
    uint32_t drain_rejected;
    uint32_t takeover_started;
    uint32_t takeover_ignored_inputs;
} MdkrMatchTransportStats;

typedef struct MdkrMatchTransport {
    MdkrNetInputHistory history;
    MdkrSessionBridge *bridge;
    uint32_t match_epoch;
    uint8_t active_slot_mask;
    uint8_t local_slot_mask;
    uint8_t remote_slot_mask;
    uint8_t reserved;
    uint32_t remote_confirmed_through[MDKR_NET_INPUT_SLOTS];
    uint32_t ai_takeover_tick[MDKR_NET_INPUT_SLOTS];
    bool remote_have_confirmed[MDKR_NET_INPUT_SLOTS];
    uint8_t ai_takeover_scheduled_mask;
    uint8_t ai_takeover_started_mask;
    MdkrMatchRecovery recovery;
    MdkrMatchTransportStats stats;
    bool ready;
} MdkrMatchTransport;

/* Binds exactly one admitted bridge epoch. The caller owns both objects and
 * must keep the bridge alive until shutdown. */
bool mdkr_match_transport_init(
    MdkrMatchTransport *transport, MdkrSessionBridge *bridge,
    uint32_t first_tick);

/* `authenticated_slot_mask` comes from the launcher's authenticated peer
 * identity, never from packet bytes. It may authorize several couch seats;
 * the requested slot must be remote, active, and included in that mask. */
MdkrMatchTransportIngressResult mdkr_match_transport_receive(
    MdkrMatchTransport *transport, uint32_t match_epoch,
    uint8_t authenticated_slot_mask, unsigned slot, uint32_t tick,
    const MdkrPadSample *sample);

/* Samples local seats once, combines them with received/repeat-last remote
 * input, and commits atomically through MdkrSessionBridge. */
bool mdkr_match_transport_drain_tick(
    MdkrMatchTransport *transport, uint32_t match_epoch, uint32_t tick,
    const MdkrPadSample *local_samples, unsigned local_sample_count);

bool mdkr_match_transport_take_dirty(
    MdkrMatchTransport *transport, uint32_t *tick);
/* Apply an already-authorized room control-log decision. Activation must be a
 * future authored tick. It is immutable/idempotent and has no mid-race
 * handback: a different tick for the same slot is a conflict. */
MdkrMatchTakeoverResult mdkr_match_transport_schedule_ai_takeover(
    MdkrMatchTransport *transport, uint32_t match_epoch, unsigned slot,
    uint32_t activation_tick);
bool mdkr_match_transport_ai_takeover_mask_for_tick(
    const MdkrMatchTransport *transport, uint32_t match_epoch, uint32_t tick,
    uint8_t *slot_mask);
/* Copy the canonical frame retained for an authored/replay tick. This is the
 * only history view exported to the engine provider; internal status cells and
 * mutable transport state never cross the launcher boundary. */
bool mdkr_match_transport_inputs_for_tick(
    MdkrMatchTransport *transport, uint32_t match_epoch, uint32_t tick,
    MdkrInputSet *out);
const MdkrMatchTransportStats *mdkr_match_transport_stats(
    const MdkrMatchTransport *transport);
/* Sticky, exact-epoch recovery request. Once a gap can no longer be replayed,
 * the launcher must stop admission/gameplay and choose resync or disconnect. */
bool mdkr_match_transport_recovery(
    const MdkrMatchTransport *transport, MdkrMatchRecovery *recovery);

#ifdef __cplusplus
}
#endif
#endif

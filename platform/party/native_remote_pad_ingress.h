/*
 * Native Phone Party transport -> engine handoff.
 *
 * WebRTC callbacks run outside the engine thread.  This bounded process-owned
 * queue is the only native crossing: transport code publishes already-framed
 * Party v1 packets and the SDL input boundary drains them.  It deliberately
 * owns no sockets, room state or UI.
 */
#ifndef MDKR_NATIVE_REMOTE_PAD_INGRESS_H
#define MDKR_NATIVE_REMOTE_PAD_INGRESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "party_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_NATIVE_REMOTE_PAD_PORTS 4u
#define MDKR_NATIVE_REMOTE_PAD_QUEUE_CAPACITY 32u

typedef struct MdkrNativeRemotePadIngressStats {
    uint64_t binds;
    uint64_t releases;
    uint64_t packets;
    uint64_t malformed;
    uint64_t stale;
    uint64_t overflows;
    uint64_t rumble_requests;
} MdkrNativeRemotePadIngressStats;

/*
 * Bind/rebind is a reliable-control-plane event.  owner and
 * connection_sequence are non-zero. A new binding clears buffered data from
 * the old epoch before becoming visible to the engine.
 */
bool mdkr_native_remote_pad_bind(
    unsigned port, uint64_t owner, uint32_t connection_sequence);

/* A delayed release cannot revoke a replacement: both identity fields match. */
bool mdkr_native_remote_pad_release(
    unsigned port, uint64_t owner, uint32_t connection_sequence);
bool mdkr_native_remote_pad_set_haptics(
    unsigned port, uint64_t owner, uint32_t connection_sequence,
    bool supported);
bool mdkr_native_remote_pad_haptics_supported(unsigned port);

/*
 * Validates the complete Party v1 frame before enqueueing it. Queue exhaustion
 * revokes the binding and clears every queued packet, forcing neutral rather
 * than allowing an old accelerator sample to survive congestion.
 */
bool mdkr_native_remote_pad_push(
    unsigned port, uint64_t owner, uint32_t connection_sequence,
    const uint8_t *bytes, size_t length);

/* Engine-thread read side. Identity is a snapshot, not a borrowed pointer. */
bool mdkr_native_remote_pad_info(
    unsigned port, uint64_t *out_owner, uint32_t *out_connection_sequence);
size_t mdkr_native_remote_pad_pop(
    unsigned port, uint64_t owner, uint32_t connection_sequence,
    uint8_t *output, size_t capacity);

/* Engine -> launcher transport control. Only the newest magnitude is useful. */
bool mdkr_native_remote_pad_request_rumble(
    unsigned port, uint16_t strength);
bool mdkr_native_remote_pad_take_rumble(
    unsigned port, uint64_t owner, uint32_t connection_sequence,
    uint16_t *out_strength);
/*
 * Read-only twin of take (M5 sustained rumble): reports the mailbox's
 * current magnitude without consuming the pending flag. The magnitude
 * persists after a take because the engine only posts CHANGES (start /
 * stop), while a phone's rumble command is a deliberate 250 ms one-shot --
 * so this is how the launcher's refresh loop asks "does the engine still
 * want this motor on?" between posts. Identity-checked like every other
 * crossing, and a rebind, release, or haptics loss zeroes the magnitude
 * (clearPayload / set_haptics), so a stale channel can never keep
 * reporting strength.
 */
bool mdkr_native_remote_pad_peek_rumble(
    unsigned port, uint64_t owner, uint32_t connection_sequence,
    uint16_t *out_strength);

void mdkr_native_remote_pad_stats(
    unsigned port, MdkrNativeRemotePadIngressStats *out_stats);

/* Process/test teardown. Ordinary engine-session teardown must not call this:
 * an approved phone keeps its launcher-owned seat across a rematch. */
void mdkr_native_remote_pad_reset_all(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_NATIVE_REMOTE_PAD_INGRESS_H */

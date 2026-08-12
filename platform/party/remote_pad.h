/* Validated remote-pad history, dedupe and fail-neutral timeout. */
#ifndef MDKR_REMOTE_PAD_H
#define MDKR_REMOTE_PAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "party_protocol.h"
#include "../input_tick_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_REMOTE_PAD_TRANSITION_CAPACITY 8u
#define MDKR_REMOTE_PAD_TIMEOUT_MS 250u

typedef struct MdkrRemotePadTransition {
    uint32_t sequence;
    MdkrInputSample sample;
} MdkrRemotePadTransition;

typedef struct MdkrRemotePadStats {
    uint64_t packets;
    uint64_t malformed;
    uint64_t duplicates;
    uint64_t stale_packets;
    uint64_t stale_connections;
    uint64_t history_deduped;
    uint64_t overflow_neutralizations;
    uint64_t timeouts;
} MdkrRemotePadStats;

typedef struct MdkrRemotePad {
    uint32_t connection_sequence;
    uint32_t last_sequence;
    uint64_t last_receive_ms;
    MdkrInputSample latest;
    MdkrRemotePadTransition transitions[MDKR_REMOTE_PAD_TRANSITION_CAPACITY];
    uint8_t head;
    uint8_t count;
    bool has_sequence;
    bool received;
    bool timeout_latched;
    MdkrRemotePadStats stats;
} MdkrRemotePad;

void mdkr_remote_pad_init(MdkrRemotePad *pad, uint32_t connection_sequence);

/* A reconnect is a reliable-control-plane event. It clears stale history and
 * publishes neutral before packets from the new epoch can be accepted. */
void mdkr_remote_pad_rebind(
    MdkrRemotePad *pad, uint32_t connection_sequence, uint64_t now_ms);

MdkrPartyDecodeResult mdkr_remote_pad_accept(
    MdkrRemotePad *pad, const uint8_t *bytes, size_t length, uint64_t now_ms);

bool mdkr_remote_pad_pop(
    MdkrRemotePad *pad, MdkrRemotePadTransition *out_transition);

/* Returns true only when this call newly emits the timeout neutral edge. */
bool mdkr_remote_pad_expire(MdkrRemotePad *pad, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_REMOTE_PAD_H */

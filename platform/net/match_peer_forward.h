/* Bounded admission for an authenticated peer forwarding opaque ciphertext. */
#ifndef MDKR_MATCH_PEER_FORWARD_H
#define MDKR_MATCH_PEER_FORWARD_H

#include "match_peer_crypto.h"
#include "match_peer_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MATCH_PEER_FORWARD_DIRECTIONS 12u

typedef struct MdkrMatchPeerForwardWindow {
    uint64_t source_endpoint_id;
    uint64_t destination_endpoint_id;
    uint32_t source_generation;
    uint32_t destination_generation;
    uint64_t greatest_sequence;
    uint64_t seen_bitmap;
    bool occupied;
} MdkrMatchPeerForwardWindow;

typedef struct MdkrMatchPeerForwarder {
    uint32_t match_epoch;
    uint64_t local_endpoint_id;
    uint32_t local_generation;
    MdkrMatchPeerForwardWindow windows[MDKR_MATCH_PEER_FORWARD_DIRECTIONS];
    bool ready;
} MdkrMatchPeerForwarder;

typedef enum MdkrMatchPeerForwardResult {
    MDKR_MATCH_PEER_FORWARD_ACCEPTED = 0,
    MDKR_MATCH_PEER_FORWARD_DUPLICATE,
    MDKR_MATCH_PEER_FORWARD_INVALID,
    MDKR_MATCH_PEER_FORWARD_STALE_EPOCH,
    MDKR_MATCH_PEER_FORWARD_STALE_GENERATION,
    MDKR_MATCH_PEER_FORWARD_WRONG_ROUTE,
    MDKR_MATCH_PEER_FORWARD_CAPACITY
} MdkrMatchPeerForwardResult;

bool mdkr_match_peer_forwarder_init(
    MdkrMatchPeerForwarder *forwarder, uint32_t match_epoch,
    uint64_t local_endpoint_id, uint32_t local_generation);

/* The input channel already authenticates the immediate source and connection
 * generation. This function requires both to equal the ciphertext header and
 * the complete generation-checked route to name this endpoint as its one
 * deterministic intermediate. It never decrypts or changes the envelope. */
MdkrMatchPeerForwardResult mdkr_match_peer_forwarder_admit(
    MdkrMatchPeerForwarder *forwarder, const MdkrMatchPeerGraph *graph,
    uint64_t authenticated_source_endpoint_id,
    uint32_t authenticated_source_generation,
    const MdkrMatchPeerEnvelopeContext *context);

#ifdef __cplusplus
}
#endif
#endif

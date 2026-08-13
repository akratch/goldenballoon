/* Canonical peer-key transcript and human verification phrase. */
#ifndef MDKR_MATCH_PEER_TRANSCRIPT_H
#define MDKR_MATCH_PEER_TRANSCRIPT_H

#include "online/lobby_core.h"
#include "match_peer_crypto.h"
#include "match_peer_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MATCH_PEER_ROOM_ID_BYTES 16u
#define MDKR_MATCH_PEER_TRANSCRIPT_DIGEST_BYTES 32u
#define MDKR_MATCH_PEER_PHRASE_BYTES 64u
#define MDKR_MATCH_PEER_COMMIT_NONCE_BYTES 32u
#define MDKR_MATCH_PEER_COMMIT_BYTES 32u

typedef struct MdkrMatchPeerTranscriptEntry {
    uint64_t endpoint_id;
    uint32_t generation;
    uint8_t public_key[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES];
    /* Round-1 commitment as *received from that endpoint*, before any public
     * key was revealed, plus the round-2 opening nonce. Both are required: the
     * digest below recomputes the commitment and refuses to produce a phrase
     * when it disagrees, so a phrase cannot be derived from key material that
     * was never committed to. */
    uint8_t commitment[MDKR_MATCH_PEER_COMMIT_BYTES];
    uint8_t commit_nonce[MDKR_MATCH_PEER_COMMIT_NONCE_BYTES];
} MdkrMatchPeerTranscriptEntry;

typedef struct MdkrMatchPeerTranscript {
    uint8_t room_id[MDKR_MATCH_PEER_ROOM_ID_BYTES];
    uint32_t match_epoch;
    MdkrOnlineCompatibilityV1 compatibility;
    MdkrMatchPeerTranscriptEntry entries[MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS];
    uint8_t entry_count;
} MdkrMatchPeerTranscript;

/* Round 1 of the verification handshake. Every endpoint publishes this value
 * *before* any public key is revealed, so no endpoint can choose its key after
 * seeing another's. `nonce` must be freshly random per epoch and is revealed
 * only in round 2. The epoch and the authenticated endpoint identity are bound
 * in, so a commitment cannot be lifted into another room, epoch or generation. */
bool mdkr_match_peer_commitment(
    uint32_t match_epoch, uint64_t endpoint_id, uint32_t generation,
    const uint8_t nonce[MDKR_MATCH_PEER_COMMIT_NONCE_BYTES],
    const uint8_t public_key[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES],
    uint8_t commitment[MDKR_MATCH_PEER_COMMIT_BYTES]);

/* Round 2 check. Constant-time comparison; false on any mismatch or malformed
 * input. Callers must run this for every peer, but the digest below also
 * re-runs it, so forgetting to call it cannot produce a usable phrase. */
bool mdkr_match_peer_commitment_verify(
    uint32_t match_epoch, uint64_t endpoint_id, uint32_t generation,
    const uint8_t nonce[MDKR_MATCH_PEER_COMMIT_NONCE_BYTES],
    const uint8_t public_key[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES],
    const uint8_t commitment[MDKR_MATCH_PEER_COMMIT_BYTES]);

/* Entries are sorted by authenticated endpoint id before hashing, so join or
 * array order cannot split honest peers. Each endpoint must replace its own
 * service-projected public key with its locally generated key before calling.
 * Every entry's commitment is re-verified here; one mismatch fails the whole
 * digest. */
bool mdkr_match_peer_transcript_digest(
    const MdkrMatchPeerTranscript *transcript,
    uint8_t digest[MDKR_MATCH_PEER_TRANSCRIPT_DIGEST_BYTES]);
bool mdkr_match_peer_verification_phrase(
    const uint8_t digest[MDKR_MATCH_PEER_TRANSCRIPT_DIGEST_BYTES],
    char phrase[MDKR_MATCH_PEER_PHRASE_BYTES]);

#ifdef __cplusplus
}
#endif
#endif

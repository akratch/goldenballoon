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

typedef struct MdkrMatchPeerTranscriptEntry {
    uint64_t endpoint_id;
    uint32_t generation;
    uint8_t public_key[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES];
} MdkrMatchPeerTranscriptEntry;

typedef struct MdkrMatchPeerTranscript {
    uint8_t room_id[MDKR_MATCH_PEER_ROOM_ID_BYTES];
    uint32_t match_epoch;
    MdkrOnlineCompatibilityV1 compatibility;
    MdkrMatchPeerTranscriptEntry entries[MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS];
    uint8_t entry_count;
} MdkrMatchPeerTranscript;

/* Entries are sorted by authenticated endpoint id before hashing, so join or
 * array order cannot split honest peers. Each endpoint must replace its own
 * service-projected public key with its locally generated key before calling. */
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

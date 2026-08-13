/* Pure launcher-owned consensus gate between room Loading and engine launch. */
#ifndef MDKR_MATCH_PREFLIGHT_H
#define MDKR_MATCH_PREFLIGHT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "match_launch_descriptor.h"
#include "match_peer_crypto.h"
#include "match_peer_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MATCH_PREFLIGHT_VERSION           1u
#define MDKR_MATCH_PREFLIGHT_DIGEST_BYTES      32u
#define MDKR_MATCH_PREFLIGHT_ROM_VERIFIED      0x01u
#define MDKR_MATCH_PREFLIGHT_PHRASE_CONFIRMED  0x02u
#define MDKR_MATCH_PREFLIGHT_CHANNELS_READY    0x04u
#define MDKR_MATCH_PREFLIGHT_ALL_FLAGS         0x07u
#define MDKR_MATCH_PREFLIGHT_ATTESTATION_BYTES 124u
#define MDKR_MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES 6u
#define MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES 58u
#define MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT 3u

#if defined(__cplusplus)
static_assert(MDKR_MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES +
                      MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES ==
                  MDKR_MATCH_PEER_PAYLOAD_BYTES,
              "preflight fragment must fill one peer payload");
static_assert(MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT ==
                  (MDKR_MATCH_PREFLIGHT_ATTESTATION_BYTES +
                   MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES - 1u) /
                      MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES,
              "preflight fragment count must cover exactly one attestation");
#else
_Static_assert(MDKR_MATCH_PREFLIGHT_FRAGMENT_HEADER_BYTES +
                       MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES ==
                   MDKR_MATCH_PEER_PAYLOAD_BYTES,
               "preflight fragment must fill one peer payload");
_Static_assert(MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT ==
                   (MDKR_MATCH_PREFLIGHT_ATTESTATION_BYTES +
                    MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES - 1u) /
                       MDKR_MATCH_PREFLIGHT_FRAGMENT_DATA_BYTES,
               "preflight fragment count must cover exactly one attestation");
#endif

typedef struct MdkrMatchPreflightAttestationV1 {
    uint32_t protocol_version;
    uint32_t match_epoch;
    uint32_t connection_generation;
    uint32_t sequence;
    uint64_t endpoint_id;
    uint8_t  descriptor_digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    uint8_t  transcript_digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    uint8_t  graph_digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    uint8_t  flags;
    uint8_t  reserved[7];
} MdkrMatchPreflightAttestationV1;

typedef struct MdkrMatchPreflightFragmentState {
    /* Canonical authenticated sender -> recipient direction for every
     * fragment in this reassembly. It is never inferred from report bytes. */
    MdkrMatchPeerKeyContext direction;
    uint32_t sequence;
    uint8_t  present_mask;
    uint8_t  encoded[MDKR_MATCH_PREFLIGHT_ATTESTATION_BYTES];
} MdkrMatchPreflightFragmentState;

typedef enum MdkrMatchPreflightFragmentResult {
    MDKR_MATCH_PREFLIGHT_FRAGMENT_ACCEPTED = 0,
    MDKR_MATCH_PREFLIGHT_FRAGMENT_COMPLETE,
    MDKR_MATCH_PREFLIGHT_FRAGMENT_DUPLICATE,
    MDKR_MATCH_PREFLIGHT_FRAGMENT_INVALID,
    MDKR_MATCH_PREFLIGHT_FRAGMENT_CONTEXT_MISMATCH,
    MDKR_MATCH_PREFLIGHT_FRAGMENT_STALE_SEQUENCE,
    MDKR_MATCH_PREFLIGHT_FRAGMENT_CONFLICT
} MdkrMatchPreflightFragmentResult;

typedef enum MdkrMatchPreflightSubmitResult {
    MDKR_MATCH_PREFLIGHT_SUBMIT_ACCEPTED = 0,
    MDKR_MATCH_PREFLIGHT_SUBMIT_DUPLICATE,
    MDKR_MATCH_PREFLIGHT_SUBMIT_INVALID,
    MDKR_MATCH_PREFLIGHT_SUBMIT_AUTHENTICATED_SOURCE_MISMATCH,
    MDKR_MATCH_PREFLIGHT_SUBMIT_STALE_EPOCH,
    MDKR_MATCH_PREFLIGHT_SUBMIT_UNKNOWN_ENDPOINT,
    MDKR_MATCH_PREFLIGHT_SUBMIT_STALE_GENERATION,
    MDKR_MATCH_PREFLIGHT_SUBMIT_STALE_SEQUENCE,
    MDKR_MATCH_PREFLIGHT_SUBMIT_CONFLICT
} MdkrMatchPreflightSubmitResult;

typedef enum MdkrMatchPreflightState {
    MDKR_MATCH_PREFLIGHT_INVALID = 0,
    MDKR_MATCH_PREFLIGHT_DESCRIPTOR_MISMATCH,
    MDKR_MATCH_PREFLIGHT_TRANSCRIPT_MISMATCH,
    MDKR_MATCH_PREFLIGHT_GRAPH_MISMATCH,
    MDKR_MATCH_PREFLIGHT_ROUTE_UNAVAILABLE,
    MDKR_MATCH_PREFLIGHT_WAITING_FOR_PEERS,
    MDKR_MATCH_PREFLIGHT_ROM_UNVERIFIED,
    MDKR_MATCH_PREFLIGHT_VERIFY_PHRASE,
    MDKR_MATCH_PREFLIGHT_CHANNELS_NOT_READY,
    MDKR_MATCH_PREFLIGHT_READY
} MdkrMatchPreflightState;

typedef struct MdkrMatchPreflightStatus {
    MdkrMatchPreflightState state;
    /* Lowest authenticated endpoint id currently responsible for the state,
     * or zero for a room-wide route/validation failure. */
    uint64_t                endpoint_id;
    uint8_t                 received_count;
    uint8_t                 required_count;
} MdkrMatchPreflightStatus;

typedef struct MdkrMatchPreflightV1 {
    uint32_t           protocol_version;
    uint32_t           match_epoch;
    uint64_t           local_endpoint_id;
    uint32_t           local_connection_generation;
    MdkrMatchPeerGraph graph;
    uint8_t            descriptor_digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    uint8_t            transcript_digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    uint8_t            graph_digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES];
    MdkrMatchPreflightAttestationV1
            attestations[MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS];
    uint8_t present_mask;
} MdkrMatchPreflightV1;

/* SHA-256 of the canonical 148-byte descriptor. Output is unchanged on error. */
bool mdkr_match_preflight_descriptor_digest(
    const MdkrMatchLaunchDescriptorV1 *descriptor,
    uint8_t                            output[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES]);

/* SHA-256 of an order-independent canonical graph encoding. Reachability bits
 * are remapped into ascending endpoint-id order, so equivalent topology
 * snapshots cannot disagree merely because their arrays use different orders.
 * Output is unchanged on error. */
bool mdkr_match_preflight_graph_digest(
    const MdkrMatchPeerGraph *graph,
    uint8_t                   output[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES]);

/* Fixed big-endian MPF1 report codec. Authentication and endpoint binding are
 * supplied by the pairwise carrier; the service must never originate these
 * bytes. Decode requires exactly 124 bytes and leaves output unchanged on any
 * malformed header, reserved bit or zero identity/generation/sequence. */
bool mdkr_match_preflight_attestation_encode(
    const MdkrMatchPreflightAttestationV1 *attestation,
    uint8_t                               *output,
    size_t                                 capacity);
bool mdkr_match_preflight_attestation_decode(
    const uint8_t                   *bytes,
    size_t                           length,
    MdkrMatchPreflightAttestationV1 *output);

/* The 124-byte report crosses the fixed 64-byte encrypted peer carrier as
 * three sequence-bound fragments on its reliable control path. Each returned
 * payload is sealed with payload type PREFLIGHT_FRAGMENT and a globally unique
 * AEAD sequence. Reassembly accepts reordering, exact duplicates and a newer
 * report replacing an incomplete older one; malformed/conflicting fragments
 * never change state or output. */
bool mdkr_match_preflight_fragment_encode(
    const MdkrMatchPreflightAttestationV1 *attestation,
    unsigned                               fragment_index,
    uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES]);
bool mdkr_match_preflight_fragment_state_init(
    MdkrMatchPreflightFragmentState *state,
    const MdkrMatchPeerKeyContext   *authenticated_direction);
MdkrMatchPreflightFragmentResult mdkr_match_preflight_fragment_submit(
    MdkrMatchPreflightFragmentState       *state,
    const MdkrMatchPeerEnvelopeContext    *authenticated_context,
    const uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES],
    MdkrMatchPreflightAttestationV1       *output);

/* Copies all immutable expectations. An inadmissible but structurally valid
 * graph is retained so evaluate() can present Route unavailable honestly. */
bool mdkr_match_preflight_init(
    MdkrMatchPreflightV1              *preflight,
    const MdkrMatchLaunchDescriptorV1 *descriptor,
    const MdkrMatchPeerGraph          *graph,
    const uint8_t                      transcript_digest[MDKR_MATCH_PREFLIGHT_DIGEST_BYTES],
    uint64_t                           local_endpoint_id,
    uint32_t                           local_connection_generation);

/* The caller must pass the identity authenticated by the carrier (or the
 * launcher's own trusted local identity). The embedded endpoint/generation
 * must match it exactly, so report bytes can never claim another member.
 * Higher sequences may advance local checks; exact retries are idempotent and
 * same-sequence changes conflict without mutation. */
MdkrMatchPreflightSubmitResult mdkr_match_preflight_submit(
    MdkrMatchPreflightV1                  *preflight,
    uint64_t                               authenticated_endpoint_id,
    uint32_t                               authenticated_connection_generation,
    const MdkrMatchPreflightAttestationV1 *attestation);

/* Pure status projection. READY is consensus evidence, not engine authority. */
MdkrMatchPreflightStatus mdkr_match_preflight_evaluate(
    const MdkrMatchPreflightV1 *preflight);

#ifdef __cplusplus
}
#endif
#endif

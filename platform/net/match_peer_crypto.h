/* Reviewed native crypto adapter for recipient-encrypted match payloads. */
#ifndef MDKR_MATCH_PEER_CRYPTO_H
#define MDKR_MATCH_PEER_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_MATCH_PEER_SECRET_BYTES 32u
#define MDKR_MATCH_PEER_TRANSCRIPT_BYTES 32u
#define MDKR_MATCH_PEER_KEY_BYTES 32u
#define MDKR_MATCH_PEER_PUBLIC_KEY_BYTES 65u
#define MDKR_MATCH_PEER_PAYLOAD_BYTES 64u
#define MDKR_MATCH_PEER_HEADER_BYTES 52u
#define MDKR_MATCH_PEER_TAG_BYTES 16u
#define MDKR_MATCH_PEER_ENVELOPE_BYTES 132u

#define MDKR_MATCH_PEER_PAYLOAD_INPUT              0u
#define MDKR_MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT 1u
#define MDKR_MATCH_PEER_PAYLOAD_TYPE_MAX           1u

#if defined(__cplusplus)
static_assert(MDKR_MATCH_PEER_ENVELOPE_BYTES ==
                  MDKR_MATCH_PEER_HEADER_BYTES + MDKR_MATCH_PEER_PAYLOAD_BYTES +
                      MDKR_MATCH_PEER_TAG_BYTES,
              "match peer envelope constants must remain exact");
#else
_Static_assert(MDKR_MATCH_PEER_ENVELOPE_BYTES ==
                   MDKR_MATCH_PEER_HEADER_BYTES + MDKR_MATCH_PEER_PAYLOAD_BYTES +
                       MDKR_MATCH_PEER_TAG_BYTES,
               "match peer envelope constants must remain exact");
#endif

typedef struct MdkrMatchPeerKeyContext {
    uint32_t match_epoch;
    uint64_t source_endpoint_id;
    uint32_t source_generation;
    uint64_t destination_endpoint_id;
    uint32_t destination_generation;
} MdkrMatchPeerKeyContext;

typedef struct MdkrMatchPeerEnvelopeContext {
    MdkrMatchPeerKeyContext key;
    /* Zero for direct delivery; otherwise the sole authorized forwarder. */
    uint64_t intermediate_endpoint_id;
    uint64_t sequence;
    /* Authenticated dispatch discriminator. Both payloads remain exactly 64
     * bytes; forwarders never inspect either kind. */
    uint8_t payload_type;
} MdkrMatchPeerEnvelopeContext;

/* Sender input deliberately has no sequence field. The seal window below is
 * the sole owner of the AES-GCM nonce sequence for one derived direction. */
typedef struct MdkrMatchPeerSendContext {
    MdkrMatchPeerKeyContext key;
    /* Zero for direct delivery; otherwise the sole authorized forwarder. */
    uint64_t intermediate_endpoint_id;
    uint8_t payload_type;
} MdkrMatchPeerSendContext;

typedef struct MdkrMatchPeerSealWindow {
    MdkrMatchPeerKeyContext direction;
    uint64_t next_sequence;
    bool ready;
} MdkrMatchPeerSealWindow;

typedef struct MdkrMatchPeerReplayWindow {
    uint64_t greatest_sequence;
    uint64_t seen_bitmap;
    bool initialized;
} MdkrMatchPeerReplayWindow;

typedef enum MdkrMatchPeerCryptoResult {
    MDKR_MATCH_PEER_CRYPTO_OK = 0,
    MDKR_MATCH_PEER_CRYPTO_INVALID,
    MDKR_MATCH_PEER_CRYPTO_STALE_EPOCH,
    MDKR_MATCH_PEER_CRYPTO_WRONG_SOURCE,
    MDKR_MATCH_PEER_CRYPTO_WRONG_RECIPIENT,
    MDKR_MATCH_PEER_CRYPTO_STALE_GENERATION,
    MDKR_MATCH_PEER_CRYPTO_AUTHENTICATION,
    MDKR_MATCH_PEER_CRYPTO_REPLAY
} MdkrMatchPeerCryptoResult;

typedef struct MdkrMatchPeerIdentity MdkrMatchPeerIdentity;

/* Ephemeral P-256 identity. Public output is the standard uncompressed point;
 * private scalar and DRBG state never cross this adapter. */
MdkrMatchPeerIdentity *mdkr_match_peer_identity_create(void);
void mdkr_match_peer_identity_destroy(MdkrMatchPeerIdentity *identity);
bool mdkr_match_peer_identity_public_key(
    const MdkrMatchPeerIdentity *identity,
    uint8_t public_key[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES]);
bool mdkr_match_peer_identity_derive_key(
    MdkrMatchPeerIdentity *identity,
    const uint8_t peer_public_key[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES],
    const uint8_t transcript_digest[MDKR_MATCH_PEER_TRANSCRIPT_BYTES],
    const MdkrMatchPeerKeyContext *context,
    uint8_t key[MDKR_MATCH_PEER_KEY_BYTES]);

/* HKDF-SHA-256. The transcript digest is the salt; direction, epoch and both
 * authenticated endpoint generations are domain-separated HKDF info. */
bool mdkr_match_peer_derive_key(
    const uint8_t shared_secret[MDKR_MATCH_PEER_SECRET_BYTES],
    const uint8_t transcript_digest[MDKR_MATCH_PEER_TRANSCRIPT_BYTES],
    const MdkrMatchPeerKeyContext *context,
    uint8_t key[MDKR_MATCH_PEER_KEY_BYTES]);
void mdkr_match_peer_forget_key(uint8_t key[MDKR_MATCH_PEER_KEY_BYTES]);

/* Parses authenticated-header-shaped routing metadata without authenticating
 * it. Forwarders must compare it to a current generation-checked graph route;
 * only the destination may treat it as trusted after `open` succeeds. */
bool mdkr_match_peer_inspect(
    const uint8_t envelope[MDKR_MATCH_PEER_ENVELOPE_BYTES],
    MdkrMatchPeerEnvelopeContext *context);

/* Initialize an all-zero window exactly once for a freshly derived directional
 * key. An active or exhausted window cannot be reset. Reconnects derive a new
 * generation-bound key and therefore require a different zeroed window. */
bool mdkr_match_peer_seal_window_init(
    MdkrMatchPeerSealWindow *window,
    const MdkrMatchPeerKeyContext *direction);

/* AES-256-GCM. The fixed header is authenticated additional data. Sequence is
 * assigned and advanced only by the direction-bound window after encryption
 * succeeds, so callers cannot accidentally reuse or reorder a nonce. Output
 * and window remain unchanged on failure; UINT64_MAX seals once then exhausts
 * the window permanently. */
bool mdkr_match_peer_seal(
    const uint8_t key[MDKR_MATCH_PEER_KEY_BYTES],
    MdkrMatchPeerSealWindow *window,
    const MdkrMatchPeerSendContext *context,
    const uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES],
    uint8_t envelope[MDKR_MATCH_PEER_ENVELOPE_BYTES]);

/* The caller supplies the complete direction expected for the selected key;
 * the authenticated header must match its source, recipient, epoch and both
 * generations before decryption. Authentication happens before replay state
 * or output changes. Each replay window belongs to exactly one direction. */
MdkrMatchPeerCryptoResult mdkr_match_peer_open(
    const uint8_t key[MDKR_MATCH_PEER_KEY_BYTES],
    const MdkrMatchPeerKeyContext *expected_context,
    MdkrMatchPeerReplayWindow *replay,
    const uint8_t envelope[MDKR_MATCH_PEER_ENVELOPE_BYTES],
    MdkrMatchPeerEnvelopeContext *context,
    uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES]);

#ifdef __cplusplus
}
#endif
#endif

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

#define MDKR_MATCH_PEER_KEYRING_SLOTS 8u
#define MDKR_MATCH_PEER_FINGERPRINT_BYTES 32u

/* A derived directional key and the one seal window that owns its nonce
 * sequence, in a single object. There is deliberately no way to obtain the key
 * without its window, and no way to mint a second window for one key: that
 * pairing is what keeps an AES-GCM nonce from ever repeating. */
typedef struct MdkrMatchPeerSealingKey {
    MdkrMatchPeerKeyContext direction;
    uint8_t fingerprint[MDKR_MATCH_PEER_FINGERPRINT_BYTES];
    uint8_t key[MDKR_MATCH_PEER_KEY_BYTES];
    MdkrMatchPeerSealWindow window;
    bool occupied;
} MdkrMatchPeerSealingKey;

/* Caller-owned, zero-initialized. Scoped per room/epoch rather than global so
 * the adapter stays reentrant. Deriving the same inputs twice returns the same
 * slot, so a second derivation cannot restart the sequence space. */
typedef struct MdkrMatchPeerKeyring {
    MdkrMatchPeerSealingKey slots[MDKR_MATCH_PEER_KEYRING_SLOTS];
} MdkrMatchPeerKeyring;

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
MdkrMatchPeerSealingKey *mdkr_match_peer_identity_derive_key(
    MdkrMatchPeerKeyring *ring,
    MdkrMatchPeerIdentity *identity,
    const uint8_t peer_public_key[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES],
    const uint8_t transcript_digest[MDKR_MATCH_PEER_TRANSCRIPT_BYTES],
    const MdkrMatchPeerKeyContext *context);

/* HKDF-SHA-256. The transcript digest is the salt; direction, epoch and both
 * authenticated endpoint generations are domain-separated HKDF info. The key
 * and its seal window are minted together into a keyring slot.
 *
 * Deriving the identical (secret, transcript, direction) twice returns the
 * SAME slot rather than a second key with a fresh sequence — two windows over
 * one key would repeat a nonce and leak the GHASH subkey. Returns NULL when
 * the inputs are invalid or the ring is full; a full ring fails closed. */
MdkrMatchPeerSealingKey *mdkr_match_peer_derive_key(
    MdkrMatchPeerKeyring *ring,
    const uint8_t shared_secret[MDKR_MATCH_PEER_SECRET_BYTES],
    const uint8_t transcript_digest[MDKR_MATCH_PEER_TRANSCRIPT_BYTES],
    const MdkrMatchPeerKeyContext *context);

/* Retires every slot, including key bytes and sequence state. */
void mdkr_match_peer_keyring_forget(MdkrMatchPeerKeyring *ring);

/* Parses authenticated-header-shaped routing metadata without authenticating
 * it. Forwarders must compare it to a current generation-checked graph route;
 * only the destination may treat it as trusted after `open` succeeds. */
bool mdkr_match_peer_inspect(
    const uint8_t envelope[MDKR_MATCH_PEER_ENVELOPE_BYTES],
    MdkrMatchPeerEnvelopeContext *context);

/* AES-256-GCM. The fixed header is authenticated additional data. Sequence is
 * assigned and advanced only by the sealing key's own window after encryption
 * succeeds, so callers cannot reuse or reorder a nonce — there is no separate
 * window argument to get wrong. Output and window remain unchanged on failure;
 * UINT64_MAX seals once then exhausts the window permanently, after which the
 * direction requires a reconnect and a fresh generation-bound key. */
bool mdkr_match_peer_seal(
    MdkrMatchPeerSealingKey *sealing,
    const MdkrMatchPeerSendContext *context,
    const uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES],
    uint8_t envelope[MDKR_MATCH_PEER_ENVELOPE_BYTES]);

/* The caller supplies the complete direction expected for the selected key;
 * the authenticated header must match its source, recipient, epoch and both
 * generations before decryption. Authentication happens before replay state
 * or output changes. Each replay window belongs to exactly one direction. */
MdkrMatchPeerCryptoResult mdkr_match_peer_open(
    const MdkrMatchPeerSealingKey *opening,
    const MdkrMatchPeerKeyContext *expected_context,
    MdkrMatchPeerReplayWindow *replay,
    const uint8_t envelope[MDKR_MATCH_PEER_ENVELOPE_BYTES],
    MdkrMatchPeerEnvelopeContext *context,
    uint8_t payload[MDKR_MATCH_PEER_PAYLOAD_BYTES]);

#ifdef __cplusplus
}
#endif
#endif

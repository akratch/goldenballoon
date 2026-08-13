#include "match_peer_transcript.h"

#include "sha256.h"

#include <stdio.h>
#include <string.h>

static const char *const kLeft[32] = {
    "Amber", "Brave", "Bright", "Calm", "Coral", "Cosmic", "Daring",
    "Flying", "Gentle", "Golden", "Happy", "Icy", "Jolly", "Lucky",
    "Mighty", "Neon", "Nimble", "Orange", "Rapid", "Royal", "Silver",
    "Solar", "Sunny", "Swift", "Teal", "Tiny", "Turbo", "Velvet",
    "Violet", "Warm", "Wild", "Zippy"};
static const char *const kRight[32] = {
    "Balloon", "Comet", "Drum", "Falcon", "Fox", "Glider", "Kite", "Lion",
    "Moon", "Otter", "Panda", "Parrot", "Pebble", "Pilot", "Rocket", "Sail",
    "Sparrow", "Star", "Tiger", "Toucan", "Turtle", "Whale", "Wing", "Cloud",
    "Dolphin", "Lantern", "Meteor", "Penguin", "Planet", "Raven", "Sunrise",
    "Thunder"};
/* v2 adds the round-1 key commitments. The domain change is the version tag:
 * a v1 peer and a v2 peer derive different digests, so they cannot agree on a
 * phrase or a key and fail closed rather than negotiating down. */
static const char kDomain[] = "golden-balloon-match-peer-transcript-v2";
static const char kCommitDomain[] = "golden-balloon-match-peer-commit-v1";

static bool nonzero(const uint8_t *bytes, size_t count) {
    uint8_t value = 0u;
    size_t index;
    for (index = 0u; index < count; index++) value |= bytes[index];
    return value != 0u;
}

static void store32(uint8_t out[4], uint32_t value) {
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void store64(uint8_t out[8], uint64_t value) {
    unsigned index;
    for (index = 0u; index < 8u; index++)
        out[index] = (uint8_t)(value >> (56u - index * 8u));
}

static bool entry_valid(const MdkrMatchPeerTranscriptEntry *entry) {
    return entry->endpoint_id != 0u && entry->generation != 0u &&
        entry->public_key[0] == 4u &&
        nonzero(entry->public_key + 1u, MDKR_MATCH_PEER_PUBLIC_KEY_BYTES - 1u) &&
        /* An all-zero opening nonce is a negligible random draw and a likely
         * uninitialized buffer; refusing it keeps a degenerate commitment from
         * silently weakening the round. */
        nonzero(entry->commit_nonce, MDKR_MATCH_PEER_COMMIT_NONCE_BYTES) &&
        nonzero(entry->commitment, MDKR_MATCH_PEER_COMMIT_BYTES);
}

bool mdkr_match_peer_commitment(
    uint32_t match_epoch, uint64_t endpoint_id, uint32_t generation,
    const uint8_t nonce[MDKR_MATCH_PEER_COMMIT_NONCE_BYTES],
    const uint8_t public_key[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES],
    uint8_t commitment[MDKR_MATCH_PEER_COMMIT_BYTES]) {
    MdkrSha256 hash;
    uint8_t encoded[8];
    if (nonce == NULL || public_key == NULL || commitment == NULL ||
        match_epoch == 0u || endpoint_id == 0u || generation == 0u ||
        public_key[0] != 4u ||
        !nonzero(public_key + 1u, MDKR_MATCH_PEER_PUBLIC_KEY_BYTES - 1u) ||
        !nonzero(nonce, MDKR_MATCH_PEER_COMMIT_NONCE_BYTES)) return false;
    mdkr_sha256_init(&hash);
    mdkr_sha256_update(&hash, kCommitDomain, sizeof(kCommitDomain) - 1u);
    store32(encoded, match_epoch);
    mdkr_sha256_update(&hash, encoded, 4u);
    store64(encoded, endpoint_id);
    mdkr_sha256_update(&hash, encoded, 8u);
    store32(encoded, generation);
    mdkr_sha256_update(&hash, encoded, 4u);
    mdkr_sha256_update(&hash, nonce, MDKR_MATCH_PEER_COMMIT_NONCE_BYTES);
    mdkr_sha256_update(&hash, public_key, MDKR_MATCH_PEER_PUBLIC_KEY_BYTES);
    mdkr_sha256_final(&hash, commitment);
    return true;
}

bool mdkr_match_peer_commitment_verify(
    uint32_t match_epoch, uint64_t endpoint_id, uint32_t generation,
    const uint8_t nonce[MDKR_MATCH_PEER_COMMIT_NONCE_BYTES],
    const uint8_t public_key[MDKR_MATCH_PEER_PUBLIC_KEY_BYTES],
    const uint8_t commitment[MDKR_MATCH_PEER_COMMIT_BYTES]) {
    uint8_t expected[MDKR_MATCH_PEER_COMMIT_BYTES];
    uint8_t difference = 0u;
    unsigned index;
    if (commitment == NULL ||
        !mdkr_match_peer_commitment(match_epoch, endpoint_id, generation, nonce,
                                    public_key, expected)) return false;
    for (index = 0u; index < MDKR_MATCH_PEER_COMMIT_BYTES; index++)
        difference |= (uint8_t)(expected[index] ^ commitment[index]);
    memset(expected, 0, sizeof(expected));
    return difference == 0u;
}

bool mdkr_match_peer_transcript_digest(
    const MdkrMatchPeerTranscript *transcript,
    uint8_t digest[MDKR_MATCH_PEER_TRANSCRIPT_DIGEST_BYTES]) {
    MdkrMatchPeerTranscriptEntry sorted[MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS];
    MdkrSha256 hash;
    uint8_t encoded[8];
    unsigned index;
    if (transcript == NULL || digest == NULL || transcript->match_epoch == 0u ||
        transcript->entry_count < 2u ||
        transcript->entry_count > MDKR_MATCH_PEER_GRAPH_MAX_ENDPOINTS ||
        !nonzero(transcript->room_id, sizeof(transcript->room_id)) ||
        !mdkr_online_compatibility_valid(&transcript->compatibility)) return false;
    memcpy(sorted, transcript->entries,
           transcript->entry_count * sizeof(sorted[0]));
    /* Validate the original roster before sorting. Validation inside the sort
     * loop can be bypassed when an invalid lower-id entry is swapped into an
     * index that has already been checked. */
    for (index = 0u; index < transcript->entry_count; index++) {
        unsigned other;
        if (!entry_valid(&sorted[index])) return false;
        /* Re-run round 2 here, not only at the call site. A phrase derived
         * from a key that was never committed to is exactly the value an
         * active man in the middle wants, so the digest refuses to produce
         * one rather than trusting the caller to have checked. */
        if (!mdkr_match_peer_commitment_verify(
                transcript->match_epoch, sorted[index].endpoint_id,
                sorted[index].generation, sorted[index].commit_nonce,
                sorted[index].public_key, sorted[index].commitment)) {
            memset(sorted, 0, sizeof(sorted));
            return false;
        }
        for (other = 0u; other < index; other++)
            if (sorted[index].endpoint_id == sorted[other].endpoint_id)
                return false;
    }
    for (index = 0u; index < transcript->entry_count; index++) {
        unsigned other;
        for (other = index + 1u; other < transcript->entry_count; other++) {
            if (sorted[index].endpoint_id > sorted[other].endpoint_id) {
                MdkrMatchPeerTranscriptEntry swap = sorted[index];
                sorted[index] = sorted[other];
                sorted[other] = swap;
            }
        }
    }
    mdkr_sha256_init(&hash);
    mdkr_sha256_update(&hash, kDomain, sizeof(kDomain) - 1u);
    mdkr_sha256_update(&hash, transcript->room_id, sizeof(transcript->room_id));
    store32(encoded, transcript->compatibility.protocol_version);
    mdkr_sha256_update(&hash, encoded, 4u);
    store32(encoded, transcript->match_epoch);
    mdkr_sha256_update(&hash, encoded, 4u);
    mdkr_sha256_update(&hash, transcript->compatibility.build_id,
                       sizeof(transcript->compatibility.build_id));
    mdkr_sha256_update(&hash, transcript->compatibility.gameplay_digest,
                       sizeof(transcript->compatibility.gameplay_digest));
    encoded[0] = transcript->compatibility.rom_revision;
    encoded[1] = transcript->compatibility.cadence_hz;
    encoded[2] = transcript->entry_count;
    mdkr_sha256_update(&hash, encoded, 3u);
    for (index = 0u; index < transcript->entry_count; index++) {
        store64(encoded, sorted[index].endpoint_id);
        mdkr_sha256_update(&hash, encoded, 8u);
        store32(encoded, sorted[index].generation);
        mdkr_sha256_update(&hash, encoded, 4u);
        /* The commitment is hashed as well as the key it opens, so the phrase
         * covers the whole two-round handshake rather than only its outcome. */
        mdkr_sha256_update(&hash, sorted[index].commitment,
                           sizeof(sorted[index].commitment));
        mdkr_sha256_update(&hash, sorted[index].public_key,
                           sizeof(sorted[index].public_key));
    }
    mdkr_sha256_final(&hash, digest);
    memset(sorted, 0, sizeof(sorted));
    return true;
}

bool mdkr_match_peer_verification_phrase(
    const uint8_t digest[MDKR_MATCH_PEER_TRANSCRIPT_DIGEST_BYTES],
    char phrase[MDKR_MATCH_PEER_PHRASE_BYTES]) {
    uint32_t value;
    int written;
    if (digest == NULL || phrase == NULL) return false;
    /* Three speakable compounds expose 30 transcript bits. Online setup can
     * be retried many times and protects up to four peers, so it deliberately
     * uses a stronger phrase than Phone Party's separate two-compound SAS. */
    value = (uint32_t)digest[0] << 22u | (uint32_t)digest[1] << 14u |
        (uint32_t)digest[2] << 6u | (uint32_t)digest[3] >> 2u;
    written = snprintf(phrase, MDKR_MATCH_PEER_PHRASE_BYTES,
        "%s-%s %s-%s %s-%s",
        kLeft[(value >> 25u) & 31u], kRight[(value >> 20u) & 31u],
        kLeft[(value >> 15u) & 31u], kRight[(value >> 10u) & 31u],
        kLeft[(value >> 5u) & 31u], kRight[value & 31u]);
    return written > 0 && written < (int)MDKR_MATCH_PEER_PHRASE_BYTES;
}

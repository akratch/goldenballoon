#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "platform/net/match_peer_transcript.h"

static void fixture(MdkrMatchPeerTranscript *value) {
    unsigned index;
    memset(value, 0, sizeof(*value));
    for (index = 0u; index < sizeof(value->room_id); index++)
        value->room_id[index] = (uint8_t)(index + 1u);
    value->match_epoch = 7u;
    value->compatibility.protocol_version = 1u;
    for (index = 0u; index < sizeof(value->compatibility.build_id); index++)
        value->compatibility.build_id[index] = (uint8_t)(index + 1u);
    for (index = 0u; index < sizeof(value->compatibility.gameplay_digest); index++)
        value->compatibility.gameplay_digest[index] = (uint8_t)(0x80u + index);
    value->compatibility.rom_revision = 1u;
    value->compatibility.cadence_hz = 30u;
    value->entry_count = 3u;
    value->entries[0].endpoint_id = 300u;
    value->entries[0].generation = 3u;
    value->entries[1].endpoint_id = 100u;
    value->entries[1].generation = 1u;
    value->entries[2].endpoint_id = 200u;
    value->entries[2].generation = 2u;
    for (index = 0u; index < value->entry_count; index++) {
        unsigned byte;
        const uint32_t generation = value->entries[index].generation;
        value->entries[index].public_key[0] = 4u;
        for (byte = 1u; byte < MDKR_MATCH_PEER_PUBLIC_KEY_BYTES; byte++)
            value->entries[index].public_key[byte] =
                (uint8_t)(value->entries[index].endpoint_id / 100u + byte);
        /* Same round-1 fixture as tests/test_match_peer_crypto_js.mjs, so the
         * pinned digest below is a genuine cross-implementation vector. */
        for (byte = 0u; byte < MDKR_MATCH_PEER_COMMIT_NONCE_BYTES; byte++)
            value->entries[index].commit_nonce[byte] =
                (uint8_t)(generation * 16u + byte + 1u);
        assert(mdkr_match_peer_commitment(
            value->match_epoch, value->entries[index].endpoint_id, generation,
            value->entries[index].commit_nonce,
            value->entries[index].public_key,
            value->entries[index].commitment));
    }
}

static void hex(const uint8_t *bytes, size_t count, char *out) {
    static const char digits[] = "0123456789abcdef";
    size_t index;
    for (index = 0u; index < count; index++) {
        out[index * 2u] = digits[bytes[index] >> 4u];
        out[index * 2u + 1u] = digits[bytes[index] & 15u];
    }
    out[count * 2u] = '\0';
}

int main(void) {
    MdkrMatchPeerTranscript value;
    MdkrMatchPeerTranscript reordered;
    uint8_t digest[MDKR_MATCH_PEER_TRANSCRIPT_DIGEST_BYTES];
    uint8_t second[MDKR_MATCH_PEER_TRANSCRIPT_DIGEST_BYTES];
    char encoded[MDKR_MATCH_PEER_TRANSCRIPT_DIGEST_BYTES * 2u + 1u];
    char phrase[MDKR_MATCH_PEER_PHRASE_BYTES];
    fixture(&value);
    assert(mdkr_match_peer_transcript_digest(&value, digest));
    hex(digest, sizeof(digest), encoded);
    assert(strcmp(encoded,
        "7ae1f0dae7ca2f575475b8278815071b1a32b929c44022bad73c093f62e3c3d2") == 0);
    assert(mdkr_match_peer_verification_phrase(digest, phrase));
    assert(strcmp(phrase, "Neon-Parrot Nimble-Thunder Brave-Wing") == 0);

    reordered = value;
    reordered.entries[0] = value.entries[2];
    reordered.entries[2] = value.entries[0];
    assert(mdkr_match_peer_transcript_digest(&reordered, second));
    assert(memcmp(digest, second, sizeof(digest)) == 0);
    /* Generation and public key are both committed to in round 1, so changing
     * either no longer merely moves the digest — the commitment stops opening
     * and no phrase is produced at all. */
    reordered = value;
    reordered.entries[1].generation++;
    memset(second, 0xa5, sizeof(second));
    assert(!mdkr_match_peer_transcript_digest(&reordered, second));
    for (unsigned index = 0u; index < sizeof(second); index++)
        assert(second[index] == 0xa5u);
    reordered = value;
    reordered.entries[1].public_key[7] ^= 1u;
    memset(second, 0xa5, sizeof(second));
    assert(!mdkr_match_peer_transcript_digest(&reordered, second));
    for (unsigned index = 0u; index < sizeof(second); index++)
        assert(second[index] == 0xa5u);
    reordered = value;
    reordered.entries[1].endpoint_id = reordered.entries[0].endpoint_id;
    memset(second, 0xa5, sizeof(second));
    assert(!mdkr_match_peer_transcript_digest(&reordered, second));
    for (unsigned index = 0u; index < sizeof(second); index++)
        assert(second[index] == 0xa5u);

    /* An invalid lowest-id entry is swapped into index zero during sorting;
     * validation must happen before that swap rather than skipping it. */
    reordered = value;
    reordered.entries[1].public_key[0] = 5u;
    memset(second, 0xa5, sizeof(second));
    assert(!mdkr_match_peer_transcript_digest(&reordered, second));
    for (unsigned index = 0u; index < sizeof(second); index++)
        assert(second[index] == 0xa5u);

    /* Every byte of a commitment and of the nonce that opens it, the way the
     * envelope sweep covers all 132 envelope bytes. A phrase derived from key
     * material that was never committed to is exactly what an active man in
     * the middle wants, so each of these must refuse. */
    for (unsigned byte = 0u; byte < MDKR_MATCH_PEER_COMMIT_BYTES; byte++) {
        reordered = value;
        reordered.entries[1].commitment[byte] ^= 0x40u;
        memset(second, 0xa5, sizeof(second));
        assert(!mdkr_match_peer_transcript_digest(&reordered, second));
        for (unsigned index = 0u; index < sizeof(second); index++)
            assert(second[index] == 0xa5u);
    }
    for (unsigned byte = 0u; byte < MDKR_MATCH_PEER_COMMIT_NONCE_BYTES; byte++) {
        reordered = value;
        reordered.entries[1].commit_nonce[byte] ^= 0x40u;
        memset(second, 0xa5, sizeof(second));
        assert(!mdkr_match_peer_transcript_digest(&reordered, second));
        for (unsigned index = 0u; index < sizeof(second); index++)
            assert(second[index] == 0xa5u);
    }
    /* Substituting another peer's valid key while keeping the old commitment
     * is the grinding attack the round exists to stop. */
    reordered = value;
    memcpy(reordered.entries[0].public_key, value.entries[2].public_key,
           sizeof(reordered.entries[0].public_key));
    assert(!mdkr_match_peer_transcript_digest(&reordered, second));
    /* A degenerate all-zero opening nonce is refused rather than quietly
     * weakening the round. */
    reordered = value;
    memset(reordered.entries[0].commit_nonce, 0,
           sizeof(reordered.entries[0].commit_nonce));
    assert(!mdkr_match_peer_transcript_digest(&reordered, second));
    /* A commitment is bound to its epoch and identity and cannot be lifted. */
    assert(mdkr_match_peer_commitment_verify(
        value.match_epoch, value.entries[0].endpoint_id,
        value.entries[0].generation, value.entries[0].commit_nonce,
        value.entries[0].public_key, value.entries[0].commitment));
    assert(!mdkr_match_peer_commitment_verify(
        value.match_epoch + 1u, value.entries[0].endpoint_id,
        value.entries[0].generation, value.entries[0].commit_nonce,
        value.entries[0].public_key, value.entries[0].commitment));
    assert(!mdkr_match_peer_commitment_verify(
        value.match_epoch, value.entries[0].endpoint_id,
        value.entries[0].generation + 1u, value.entries[0].commit_nonce,
        value.entries[0].public_key, value.entries[0].commitment));

    puts("test_match_peer_transcript: PASS");
    return 0;
}

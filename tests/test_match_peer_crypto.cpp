/* Assert-driven test: NDEBUG (the Release default) would compile every
 * check away — and delete the calls the asserts wrap. */
#undef NDEBUG

#include "platform/net/match_peer_crypto.h"
#include "platform/net/match_preflight.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

static void unchanged(const uint8_t *bytes, size_t size) {
    for (size_t index = 0u; index < size; ++index) assert(bytes[index] == 0xa5u);
}

static unsigned nibble(char value) {
    return value >= '0' && value <= '9' ? static_cast<unsigned>(value - '0') :
        static_cast<unsigned>(value - 'a' + 10);
}

static bool equalsHex(const uint8_t *bytes, size_t size, const char *hex) {
    for (size_t index = 0u; index < size; ++index) {
        if (bytes[index] != static_cast<uint8_t>(
                nibble(hex[index * 2u]) << 4u | nibble(hex[index * 2u + 1u])))
            return false;
    }
    return hex[size * 2u] == '\0';
}

static bool contextEquals(const MdkrMatchPeerEnvelopeContext &left,
                          const MdkrMatchPeerEnvelopeContext &right) {
    return left.key.match_epoch == right.key.match_epoch &&
        left.key.source_endpoint_id == right.key.source_endpoint_id &&
        left.key.source_generation == right.key.source_generation &&
        left.key.destination_endpoint_id == right.key.destination_endpoint_id &&
        left.key.destination_generation == right.key.destination_generation &&
        left.intermediate_endpoint_id == right.intermediate_endpoint_id &&
        left.sequence == right.sequence &&
        left.payload_type == right.payload_type;
}

static bool keyContextEquals(const MdkrMatchPeerKeyContext &left,
                             const MdkrMatchPeerKeyContext &right) {
    return left.match_epoch == right.match_epoch &&
        left.source_endpoint_id == right.source_endpoint_id &&
        left.source_generation == right.source_generation &&
        left.destination_endpoint_id == right.destination_endpoint_id &&
        left.destination_generation == right.destination_generation;
}

static bool sealWindowEquals(const MdkrMatchPeerSealWindow &left,
                             const MdkrMatchPeerSealWindow &right) {
    return keyContextEquals(left.direction, right.direction) &&
        left.next_sequence == right.next_sequence && left.ready == right.ready;
}

static bool replayWindowEquals(const MdkrMatchPeerReplayWindow &left,
                               const MdkrMatchPeerReplayWindow &right) {
    return left.greatest_sequence == right.greatest_sequence &&
        left.seen_bitmap == right.seen_bitmap &&
        left.initialized == right.initialized;
}

/* Models an adversary that legitimately holds this direction's key material and
 * then claims a different identity in the protected header. */
static MdkrMatchPeerSealingKey forgedKeyFor(
    const MdkrMatchPeerSealingKey &source,
    const MdkrMatchPeerKeyContext &claimed) {
    MdkrMatchPeerSealingKey forged{};
    std::memcpy(forged.key, source.key, sizeof(forged.key));
    forged.direction = claimed;
    forged.window.direction = claimed;
    forged.window.next_sequence = 1u;
    forged.window.ready = true;
    forged.occupied = true;
    return forged;
}

int main() {
    std::array<uint8_t, MDKR_MATCH_PEER_SECRET_BYTES> secret{};
    std::array<uint8_t, MDKR_MATCH_PEER_TRANSCRIPT_BYTES> transcript{};
    MdkrMatchPeerKeyring ring{};
    std::array<uint8_t, MDKR_MATCH_PEER_PAYLOAD_BYTES> payload{};
    std::array<uint8_t, MDKR_MATCH_PEER_PAYLOAD_BYTES> opened{};
    std::array<uint8_t, MDKR_MATCH_PEER_ENVELOPE_BYTES> envelope{};
    MdkrMatchPeerKeyContext direction{7u, 100u, 2u, 400u, 9u};
    MdkrMatchPeerSendContext sendContext{
        direction, 200u, MDKR_MATCH_PEER_PAYLOAD_INPUT};
    MdkrMatchPeerEnvelopeContext expected{
        direction, 200u, 1u, MDKR_MATCH_PEER_PAYLOAD_INPUT};
    MdkrMatchPeerEnvelopeContext decoded{};
    MdkrMatchPeerReplayWindow replay{};
    for (size_t index = 0u; index < secret.size(); ++index) {
        secret[index] = static_cast<uint8_t>(index);
        transcript[index] = static_cast<uint8_t>(0xa0u + index);
    }
    for (size_t index = 0u; index < payload.size(); ++index)
        payload[index] = static_cast<uint8_t>(0x40u + index);

    /* Both endpoint identities derive the same direction-specific key without
     * exporting either private scalar. Invalid curve points fail atomically. */
    MdkrMatchPeerIdentity *left = mdkr_match_peer_identity_create();
    MdkrMatchPeerIdentity *right = mdkr_match_peer_identity_create();
    std::array<uint8_t, MDKR_MATCH_PEER_PUBLIC_KEY_BYTES> leftPublic{};
    std::array<uint8_t, MDKR_MATCH_PEER_PUBLIC_KEY_BYTES> rightPublic{};
    MdkrMatchPeerKeyring identityRing{};
    assert(left != nullptr && right != nullptr);
    assert(mdkr_match_peer_identity_public_key(left, leftPublic.data()));
    assert(mdkr_match_peer_identity_public_key(right, rightPublic.data()));
    assert(leftPublic[0] == 4u && rightPublic[0] == 4u && leftPublic != rightPublic);
    MdkrMatchPeerSealingKey *leftKey = mdkr_match_peer_identity_derive_key(
        &identityRing, left, rightPublic.data(), transcript.data(), &direction);
    MdkrMatchPeerSealingKey *rightKey = mdkr_match_peer_identity_derive_key(
        &identityRing, right, leftPublic.data(), transcript.data(), &direction);
    /* ECDH symmetry is now directly observable: identical inputs name one ring
     * slot, so both endpoints share a single window instead of two windows over
     * one AES key. Broken symmetry would produce different fingerprints and
     * therefore different slots. */
    assert(leftKey != nullptr && leftKey == rightKey);
    auto invalidIdentityContext = direction;
    invalidIdentityContext.destination_generation = 0u;
    assert(mdkr_match_peer_identity_derive_key(
        &identityRing, left, rightPublic.data(), transcript.data(),
        &invalidIdentityContext) == nullptr);
    auto invalidPublic = rightPublic;
    invalidPublic[0] = 5u;
    assert(mdkr_match_peer_identity_derive_key(
        &identityRing, left, invalidPublic.data(), transcript.data(),
        &direction) == nullptr);
    mdkr_match_peer_keyring_forget(&identityRing);
    mdkr_match_peer_identity_destroy(right);
    mdkr_match_peer_identity_destroy(left);

    MdkrMatchPeerSealingKey *key = mdkr_match_peer_derive_key(
        &ring, secret.data(), transcript.data(), &direction);
    assert(key != nullptr && key->occupied);
    MdkrMatchPeerSealWindow &sealWindow = key->window;
    assert(sealWindow.ready && sealWindow.next_sequence == 1u);
    /* G-2: a repeated derivation of identical inputs returns the same slot.
     * A second slot would restart the sequence at 1 under one key, repeating a
     * (key, nonce) pair and leaking the GHASH subkey. */
    assert(mdkr_match_peer_derive_key(
        &ring, secret.data(), transcript.data(), &direction) == key);
    {
        auto invalidDirection = direction;
        invalidDirection.source_generation = 0u;
        assert(mdkr_match_peer_derive_key(
            &ring, secret.data(), transcript.data(), &invalidDirection) ==
            nullptr);
    }
    {
        auto invalidEnvelopeContext = sendContext;
        const MdkrMatchPeerSealWindow windowBefore = sealWindow;
        std::array<uint8_t, MDKR_MATCH_PEER_ENVELOPE_BYTES> untouchedEnvelope;
        untouchedEnvelope.fill(0xa5u);
        invalidEnvelopeContext.payload_type =
            MDKR_MATCH_PEER_PAYLOAD_TYPE_MAX + 1u;
        assert(!mdkr_match_peer_seal(
            key, &invalidEnvelopeContext, payload.data(),
            untouchedEnvelope.data()));
        assert(sealWindowEquals(sealWindow, windowBefore));
        unchanged(untouchedEnvelope.data(), untouchedEnvelope.size());
    }
    {
        auto invalidContext = direction;
        invalidContext.destination_generation = 0u;
        assert(mdkr_match_peer_derive_key(&ring, secret.data(),
                                          transcript.data(),
                                          &invalidContext) == nullptr);
    }
    assert(mdkr_match_peer_seal(
        key, &sendContext, payload.data(), envelope.data()));
    assert(sealWindow.ready && sealWindow.next_sequence == 2u);
    assert(equalsHex(key->key, sizeof(key->key),
        "4569b1edaec1b95100404c552fa93b885c280074eac696624f3313c3081bdd1e"));
    assert(equalsHex(envelope.data(), envelope.size(),
        "4d50453102010000000000070000000000000064000000020000000000000190"
        "0000000900000000000000c80000000000000001f51c2e8a849b8a8e48aba590"
        "238f4b4a42ad2017043dc723841ce6955caad4ecea6dddd15df2e07494afc0d9"
        "3024366db9095a0a4219bda447fa73d765b1b9dbf5db7ea7551bfa70d48b88e2"
        "0d6b4386"));
    assert(mdkr_match_peer_inspect(envelope.data(), &decoded));
    assert(contextEquals(decoded, expected));
    std::memset(opened.data(), 0xa5, opened.size());
    assert(mdkr_match_peer_open(
        key, &direction, &replay, envelope.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_OK);
    assert(std::memcmp(opened.data(), payload.data(), payload.size()) == 0);
    assert(contextEquals(decoded, expected));
    assert(replay.initialized && replay.greatest_sequence == 1u &&
           replay.seen_bitmap == 1u);

    /* Exact replay is rejected after authentication without changing output. */
    const MdkrMatchPeerReplayWindow replayBefore = replay;
    std::memset(opened.data(), 0xa5, opened.size());
    assert(mdkr_match_peer_open(
        key, &direction, &replay, envelope.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_REPLAY);
    assert(replayWindowEquals(replay, replayBefore));
    unchanged(opened.data(), opened.size());

    /* Contradictory caller-owned replay state fails closed rather than
     * rebuilding history and admitting an already-seen sequence. */
    {
        MdkrMatchPeerReplayWindow corrupt{0u, 0u, true};
        const MdkrMatchPeerReplayWindow corruptBefore = corrupt;
        std::memset(opened.data(), 0xa5, opened.size());
        assert(mdkr_match_peer_open(
            key, &direction, &corrupt, envelope.data(), &decoded,
            opened.data()) == MDKR_MATCH_PEER_CRYPTO_INVALID);
        assert(replayWindowEquals(corrupt, corruptBefore));
        unchanged(opened.data(), opened.size());
        corrupt = MdkrMatchPeerReplayWindow{1u, 1u, false};
        assert(mdkr_match_peer_open(
            key, &direction, &corrupt, envelope.data(), &decoded,
            opened.data()) == MDKR_MATCH_PEER_CRYPTO_INVALID);
    }

    /* Header, ciphertext and tag mutations authenticate neither data nor replay. */
    for (size_t index = 0u; index < envelope.size(); ++index) {
        auto mutated = envelope;
        MdkrMatchPeerReplayWindow fresh{};
        mutated[index] ^= 0x40u;
        std::memset(opened.data(), 0xa5, opened.size());
        const auto result = mdkr_match_peer_open(
            key, &direction, &fresh, mutated.data(), &decoded,
            opened.data());
        assert(result != MDKR_MATCH_PEER_CRYPTO_OK);
        assert(!fresh.initialized);
        unchanged(opened.data(), opened.size());
    }

    /* The caller supplies the complete direction expected for this key. A
     * peer cannot authenticate with its own key while claiming another source
     * identity in the protected header. */
    std::memset(opened.data(), 0xa5, opened.size());
    MdkrMatchPeerReplayWindow fresh{};
    auto wrongExpected = direction;
    wrongExpected.match_epoch = 6u;
    assert(mdkr_match_peer_open(
        key, &wrongExpected, &fresh, envelope.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_STALE_EPOCH);
    wrongExpected = direction;
    wrongExpected.destination_endpoint_id = 300u;
    assert(mdkr_match_peer_open(
        key, &wrongExpected, &fresh, envelope.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_WRONG_RECIPIENT);
    wrongExpected = direction;
    wrongExpected.destination_generation = 8u;
    assert(mdkr_match_peer_open(
        key, &wrongExpected, &fresh, envelope.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_STALE_GENERATION);
    wrongExpected = direction;
    wrongExpected.source_endpoint_id = 0u;
    assert(mdkr_match_peer_open(
        key, &wrongExpected, &fresh, envelope.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_INVALID);
    auto forgedSource = sendContext;
    forgedSource.key.source_endpoint_id = 300u;
    forgedSource.key.source_generation = 5u;
    MdkrMatchPeerSealingKey forgedSourceSealing = forgedKeyFor(*key, forgedSource.key);
    assert(mdkr_match_peer_seal(
        &forgedSourceSealing, &forgedSource, payload.data(), envelope.data()));
    assert(mdkr_match_peer_open(
        key, &direction, &fresh, envelope.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_WRONG_SOURCE);
    forgedSource = sendContext;
    forgedSource.key.source_generation++;
    MdkrMatchPeerSealingKey forgedGenerationSealing =
        forgedKeyFor(*key, forgedSource.key);
    assert(mdkr_match_peer_seal(
        &forgedGenerationSealing, &forgedSource, payload.data(),
        envelope.data()));
    assert(mdkr_match_peer_open(
        key, &direction, &fresh, envelope.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_STALE_GENERATION);
    assert(!fresh.initialized);
    unchanged(opened.data(), opened.size());

    /* The sender owns a strictly monotonic sequence even when the network
     * reorders delivery. An unseen in-window packet can arrive later, while a
     * packet outside the 64-sequence receive window fails. */
    std::array<uint8_t, MDKR_MATCH_PEER_ENVELOPE_BYTES> envelope2{};
    std::array<uint8_t, MDKR_MATCH_PEER_ENVELOPE_BYTES> envelope3{};
    std::array<uint8_t, MDKR_MATCH_PEER_ENVELOPE_BYTES> envelope4{};
    assert(mdkr_match_peer_seal(
        key, &sendContext, payload.data(), envelope2.data()));
    assert(mdkr_match_peer_seal(
        key, &sendContext, payload.data(), envelope3.data()));
    assert(mdkr_match_peer_open(
        key, &direction, &replay, envelope3.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_OK);
    assert(decoded.sequence == 3u);
    assert(mdkr_match_peer_open(
        key, &direction, &replay, envelope2.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_OK);
    assert(decoded.sequence == 2u);
    assert(mdkr_match_peer_seal(
        key, &sendContext, payload.data(), envelope4.data()));
    for (uint64_t sequence = 5u; sequence <= 68u; ++sequence) {
        assert(mdkr_match_peer_seal(
            key, &sendContext, payload.data(),
            envelope.data()));
        assert(mdkr_match_peer_inspect(envelope.data(), &decoded));
        assert(decoded.sequence == sequence);
    }
    assert(mdkr_match_peer_open(
        key, &direction, &replay, envelope.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_OK);
    assert(decoded.sequence == 68u && sealWindow.next_sequence == 69u);
    assert(mdkr_match_peer_open(
        key, &direction, &replay, envelope4.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_REPLAY);

    /* The authenticated type byte carries one complete 124-byte report in
     * three reliable fixed payloads without exposing it to a one-hop
     * forwarder. Fragment order may differ from transport sequence order. */
    {
        MdkrMatchPreflightAttestationV1 report{};
        MdkrMatchPreflightAttestationV1 reconstructed{};
        MdkrMatchPreflightFragmentState fragments{};
        std::array<std::array<uint8_t, MDKR_MATCH_PEER_PAYLOAD_BYTES>,
                   MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT> fragmentPayloads{};
        const unsigned fragmentOrder[] = {2u, 0u, 1u};
        report.protocol_version = MDKR_MATCH_PREFLIGHT_VERSION;
        report.match_epoch = 7u;
        report.connection_generation = 2u;
        report.sequence = 13u;
        report.endpoint_id = 100u;
        report.flags = MDKR_MATCH_PREFLIGHT_ALL_FLAGS;
        for (size_t index = 0u; index < MDKR_MATCH_PREFLIGHT_DIGEST_BYTES;
             index++) {
            report.descriptor_digest[index] = static_cast<uint8_t>(index + 1u);
            report.transcript_digest[index] =
                static_cast<uint8_t>(index + 0x41u);
            report.graph_digest[index] = static_cast<uint8_t>(index + 0x81u);
        }
        assert(mdkr_match_preflight_fragment_state_init(
            &fragments, &direction));
        for (unsigned index = 0u; index < MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT;
             index++)
            assert(mdkr_match_preflight_fragment_encode(
                &report, index, fragmentPayloads[index].data()));
        sendContext.payload_type = MDKR_MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT;
        for (unsigned index = 0u; index < MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT;
             index++) {
            const unsigned fragmentIndex = fragmentOrder[index];
            assert(mdkr_match_peer_seal(
                key, &sendContext,
                fragmentPayloads[fragmentIndex].data(), envelope.data()));
            assert(envelope[6] == MDKR_MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT);
            assert(mdkr_match_peer_open(
                key, &direction, &replay, envelope.data(), &decoded,
                opened.data()) == MDKR_MATCH_PEER_CRYPTO_OK);
            assert(decoded.sequence == 69u + index);
            assert(decoded.payload_type ==
                   MDKR_MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT);
            const auto fragmentResult = mdkr_match_preflight_fragment_submit(
                &fragments, &decoded, opened.data(), &reconstructed);
            assert(fragmentResult ==
                   (index + 1u == MDKR_MATCH_PREFLIGHT_FRAGMENT_COUNT
                        ? MDKR_MATCH_PREFLIGHT_FRAGMENT_COMPLETE
                        : MDKR_MATCH_PREFLIGHT_FRAGMENT_ACCEPTED));
        }
        assert(std::memcmp(&reconstructed, &report, sizeof(report)) == 0);
        sendContext.payload_type = MDKR_MATCH_PEER_PAYLOAD_INPUT;
    }

    /* Invalid and exhausted seal state is fail-atomic. UINT64_MAX is usable
     * exactly once, after which the direction must rekey/reconnect. */
    {
        MdkrMatchPeerSealWindow corrupt = sealWindow;
        corrupt.next_sequence = 0u;
        const MdkrMatchPeerSealWindow corruptBefore = corrupt;
        std::array<uint8_t, MDKR_MATCH_PEER_ENVELOPE_BYTES> untouchedEnvelope;
        untouchedEnvelope.fill(0xa5u);
        MdkrMatchPeerSealingKey corruptSealing = *key;
        corruptSealing.window = corrupt;
        assert(!mdkr_match_peer_seal(
            &corruptSealing, &sendContext, payload.data(),
            untouchedEnvelope.data()));
        assert(sealWindowEquals(corruptSealing.window, corruptBefore));
        unchanged(untouchedEnvelope.data(), untouchedEnvelope.size());

        MdkrMatchPeerSealingKey exhaustedSealing = *key;
        exhaustedSealing.window.next_sequence = UINT64_MAX;
        exhaustedSealing.window.ready = true;
        assert(mdkr_match_peer_seal(
            &exhaustedSealing, &sendContext, payload.data(), envelope.data()));
        assert(!exhaustedSealing.window.ready &&
               exhaustedSealing.window.next_sequence == 0u);
        assert(mdkr_match_peer_inspect(envelope.data(), &decoded));
        assert(decoded.sequence == UINT64_MAX);
        const MdkrMatchPeerSealWindow exhaustedBefore = exhaustedSealing.window;
        untouchedEnvelope.fill(0xa5u);
        assert(!mdkr_match_peer_seal(
            &exhaustedSealing, &sendContext, payload.data(),
            untouchedEnvelope.data()));
        assert(sealWindowEquals(exhaustedSealing.window, exhaustedBefore));
        unchanged(untouchedEnvelope.data(), untouchedEnvelope.size());
    }

    /* Key derivation is directional and generation-bound. */
    auto reverse = direction;
    reverse.source_endpoint_id = 400u;
    reverse.source_generation = 9u;
    reverse.destination_endpoint_id = 100u;
    reverse.destination_generation = 2u;
    MdkrMatchPeerSealingKey *other = mdkr_match_peer_derive_key(
        &ring, secret.data(), transcript.data(), &reverse);
    assert(other != nullptr && other != key);
    assert(other != key);
    reverse = direction;
    reverse.destination_generation++;
    MdkrMatchPeerSealingKey *rekeyed = mdkr_match_peer_derive_key(
        &ring, secret.data(), transcript.data(), &reverse);
    assert(rekeyed != nullptr && rekeyed != key && rekeyed != other);
    assert(std::memcmp(rekeyed->key, key->key, sizeof(key->key)) != 0);

    /* A full ring fails closed rather than evicting a live sequence space. */
    {
        MdkrMatchPeerKeyring small{};
        for (unsigned index = 0u; index < MDKR_MATCH_PEER_KEYRING_SLOTS;
             ++index) {
            auto slotDirection = direction;
            slotDirection.destination_generation = 100u + index;
            assert(mdkr_match_peer_derive_key(
                &small, secret.data(), transcript.data(), &slotDirection) !=
                nullptr);
        }
        auto overflowDirection = direction;
        overflowDirection.destination_generation = 999u;
        assert(mdkr_match_peer_derive_key(
            &small, secret.data(), transcript.data(), &overflowDirection) ==
            nullptr);
        mdkr_match_peer_keyring_forget(&small);
        assert(!small.slots[0].occupied);
    }

    /* Cross-version confusion fails closed: a v1 envelope, the protocol that
     * predates the key-commitment round, is refused at the header before any
     * decryption is attempted. */
    {
        auto downgraded = envelope;
        MdkrMatchPeerReplayWindow downgradeReplay{};
        downgraded[4] = 1u;
        assert(!mdkr_match_peer_inspect(downgraded.data(), &decoded));
        std::memset(opened.data(), 0xa5, opened.size());
        assert(mdkr_match_peer_open(
            key, &direction, &downgradeReplay, downgraded.data(), &decoded,
            opened.data()) == MDKR_MATCH_PEER_CRYPTO_INVALID);
        unchanged(opened.data(), opened.size());
    }

    mdkr_match_peer_keyring_forget(&ring);
    assert(!ring.slots[0].occupied);

    std::puts("test_match_peer_crypto: PASS");
    return 0;
}

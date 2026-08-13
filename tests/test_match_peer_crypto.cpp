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

int main() {
    std::array<uint8_t, MDKR_MATCH_PEER_SECRET_BYTES> secret{};
    std::array<uint8_t, MDKR_MATCH_PEER_TRANSCRIPT_BYTES> transcript{};
    std::array<uint8_t, MDKR_MATCH_PEER_KEY_BYTES> key{};
    std::array<uint8_t, MDKR_MATCH_PEER_PAYLOAD_BYTES> payload{};
    std::array<uint8_t, MDKR_MATCH_PEER_PAYLOAD_BYTES> opened{};
    std::array<uint8_t, MDKR_MATCH_PEER_ENVELOPE_BYTES> envelope{};
    MdkrMatchPeerKeyContext direction{7u, 100u, 2u, 400u, 9u};
    MdkrMatchPeerSendContext sendContext{
        direction, 200u, MDKR_MATCH_PEER_PAYLOAD_INPUT};
    MdkrMatchPeerSealWindow sealWindow{};
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
    std::array<uint8_t, MDKR_MATCH_PEER_KEY_BYTES> leftKey{};
    std::array<uint8_t, MDKR_MATCH_PEER_KEY_BYTES> rightKey{};
    assert(left != nullptr && right != nullptr);
    assert(mdkr_match_peer_identity_public_key(left, leftPublic.data()));
    assert(mdkr_match_peer_identity_public_key(right, rightPublic.data()));
    assert(leftPublic[0] == 4u && rightPublic[0] == 4u && leftPublic != rightPublic);
    assert(mdkr_match_peer_identity_derive_key(
        left, rightPublic.data(), transcript.data(), &direction, leftKey.data()));
    assert(mdkr_match_peer_identity_derive_key(
        right, leftPublic.data(), transcript.data(), &direction, rightKey.data()));
    assert(leftKey == rightKey);
    auto invalidIdentityContext = direction;
    invalidIdentityContext.destination_generation = 0u;
    std::memset(rightKey.data(), 0xa5, rightKey.size());
    assert(!mdkr_match_peer_identity_derive_key(
        left, rightPublic.data(), transcript.data(), &invalidIdentityContext,
        rightKey.data()));
    unchanged(rightKey.data(), rightKey.size());
    auto invalidPublic = rightPublic;
    invalidPublic[0] = 5u;
    std::memset(rightKey.data(), 0xa5, rightKey.size());
    assert(!mdkr_match_peer_identity_derive_key(
        left, invalidPublic.data(), transcript.data(), &direction, rightKey.data()));
    unchanged(rightKey.data(), rightKey.size());
    mdkr_match_peer_identity_destroy(right);
    mdkr_match_peer_identity_destroy(left);

    assert(mdkr_match_peer_derive_key(
        secret.data(), transcript.data(), &direction, key.data()));
    {
        MdkrMatchPeerSealWindow invalidWindow{};
        const MdkrMatchPeerSealWindow windowBefore = invalidWindow;
        auto invalidDirection = direction;
        invalidDirection.source_generation = 0u;
        assert(!mdkr_match_peer_seal_window_init(
            &invalidWindow, &invalidDirection));
        assert(sealWindowEquals(invalidWindow, windowBefore));
    }
    assert(mdkr_match_peer_seal_window_init(&sealWindow, &direction));
    {
        const MdkrMatchPeerSealWindow windowBefore = sealWindow;
        assert(!mdkr_match_peer_seal_window_init(&sealWindow, &direction));
        assert(sealWindowEquals(sealWindow, windowBefore));
    }
    {
        auto invalidEnvelopeContext = sendContext;
        const MdkrMatchPeerSealWindow windowBefore = sealWindow;
        std::array<uint8_t, MDKR_MATCH_PEER_ENVELOPE_BYTES> untouchedEnvelope;
        untouchedEnvelope.fill(0xa5u);
        invalidEnvelopeContext.payload_type =
            MDKR_MATCH_PEER_PAYLOAD_TYPE_MAX + 1u;
        assert(!mdkr_match_peer_seal(
            key.data(), &sealWindow, &invalidEnvelopeContext, payload.data(),
            untouchedEnvelope.data()));
        assert(sealWindowEquals(sealWindow, windowBefore));
        unchanged(untouchedEnvelope.data(), untouchedEnvelope.size());
    }
    {
        auto invalidContext = direction;
        std::array<uint8_t, MDKR_MATCH_PEER_KEY_BYTES> untouchedKey;
        untouchedKey.fill(0xa5u);
        invalidContext.destination_generation = 0u;
        assert(!mdkr_match_peer_derive_key(secret.data(), transcript.data(),
                                           &invalidContext,
                                           untouchedKey.data()));
        unchanged(untouchedKey.data(), untouchedKey.size());
    }
    assert(mdkr_match_peer_seal(
        key.data(), &sealWindow, &sendContext, payload.data(), envelope.data()));
    assert(sealWindow.ready && sealWindow.next_sequence == 2u);
    assert(equalsHex(key.data(), key.size(),
        "0b23edf3d8577e0e580dfc112979a882e5d9907b63d61acfe6d98c6e596f6f33"));
    assert(equalsHex(envelope.data(), envelope.size(),
        "4d50453101010000000000070000000000000064000000020000000000000190"
        "0000000900000000000000c800000000000000012bd9f1554abd62235a212387"
        "c3d2cd82030455aa45e53f72d45d982c44ced93ea300c68543d2e9b9c16ce23"
        "ed3b5d35f57d13a9dbf3a3010fa37df24c6aefaedeb1d33859f2376cde014319"
        "33d5f1fad"));
    assert(mdkr_match_peer_inspect(envelope.data(), &decoded));
    assert(contextEquals(decoded, expected));
    std::memset(opened.data(), 0xa5, opened.size());
    assert(mdkr_match_peer_open(
        key.data(), &direction, &replay, envelope.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_OK);
    assert(std::memcmp(opened.data(), payload.data(), payload.size()) == 0);
    assert(contextEquals(decoded, expected));
    assert(replay.initialized && replay.greatest_sequence == 1u &&
           replay.seen_bitmap == 1u);

    /* Exact replay is rejected after authentication without changing output. */
    const MdkrMatchPeerReplayWindow replayBefore = replay;
    std::memset(opened.data(), 0xa5, opened.size());
    assert(mdkr_match_peer_open(
        key.data(), &direction, &replay, envelope.data(), &decoded,
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
            key.data(), &direction, &corrupt, envelope.data(), &decoded,
            opened.data()) == MDKR_MATCH_PEER_CRYPTO_INVALID);
        assert(replayWindowEquals(corrupt, corruptBefore));
        unchanged(opened.data(), opened.size());
        corrupt = MdkrMatchPeerReplayWindow{1u, 1u, false};
        assert(mdkr_match_peer_open(
            key.data(), &direction, &corrupt, envelope.data(), &decoded,
            opened.data()) == MDKR_MATCH_PEER_CRYPTO_INVALID);
    }

    /* Header, ciphertext and tag mutations authenticate neither data nor replay. */
    for (size_t index = 0u; index < envelope.size(); ++index) {
        auto mutated = envelope;
        MdkrMatchPeerReplayWindow fresh{};
        mutated[index] ^= 0x40u;
        std::memset(opened.data(), 0xa5, opened.size());
        const auto result = mdkr_match_peer_open(
            key.data(), &direction, &fresh, mutated.data(), &decoded,
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
        key.data(), &wrongExpected, &fresh, envelope.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_STALE_EPOCH);
    wrongExpected = direction;
    wrongExpected.destination_endpoint_id = 300u;
    assert(mdkr_match_peer_open(
        key.data(), &wrongExpected, &fresh, envelope.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_WRONG_RECIPIENT);
    wrongExpected = direction;
    wrongExpected.destination_generation = 8u;
    assert(mdkr_match_peer_open(
        key.data(), &wrongExpected, &fresh, envelope.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_STALE_GENERATION);
    wrongExpected = direction;
    wrongExpected.source_endpoint_id = 0u;
    assert(mdkr_match_peer_open(
        key.data(), &wrongExpected, &fresh, envelope.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_INVALID);
    auto forgedSource = sendContext;
    forgedSource.key.source_endpoint_id = 300u;
    forgedSource.key.source_generation = 5u;
    MdkrMatchPeerSealWindow forgedSourceWindow{};
    assert(mdkr_match_peer_seal_window_init(
        &forgedSourceWindow, &forgedSource.key));
    assert(mdkr_match_peer_seal(
        key.data(), &forgedSourceWindow, &forgedSource, payload.data(),
        envelope.data()));
    assert(mdkr_match_peer_open(
        key.data(), &direction, &fresh, envelope.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_WRONG_SOURCE);
    forgedSource = sendContext;
    forgedSource.key.source_generation++;
    MdkrMatchPeerSealWindow forgedGenerationWindow{};
    assert(mdkr_match_peer_seal_window_init(
        &forgedGenerationWindow, &forgedSource.key));
    assert(mdkr_match_peer_seal(
        key.data(), &forgedGenerationWindow, &forgedSource, payload.data(),
        envelope.data()));
    assert(mdkr_match_peer_open(
        key.data(), &direction, &fresh, envelope.data(), &decoded,
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
        key.data(), &sealWindow, &sendContext, payload.data(), envelope2.data()));
    assert(mdkr_match_peer_seal(
        key.data(), &sealWindow, &sendContext, payload.data(), envelope3.data()));
    assert(mdkr_match_peer_open(
        key.data(), &direction, &replay, envelope3.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_OK);
    assert(decoded.sequence == 3u);
    assert(mdkr_match_peer_open(
        key.data(), &direction, &replay, envelope2.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_OK);
    assert(decoded.sequence == 2u);
    assert(mdkr_match_peer_seal(
        key.data(), &sealWindow, &sendContext, payload.data(), envelope4.data()));
    for (uint64_t sequence = 5u; sequence <= 68u; ++sequence) {
        assert(mdkr_match_peer_seal(
            key.data(), &sealWindow, &sendContext, payload.data(),
            envelope.data()));
        assert(mdkr_match_peer_inspect(envelope.data(), &decoded));
        assert(decoded.sequence == sequence);
    }
    assert(mdkr_match_peer_open(
        key.data(), &direction, &replay, envelope.data(), &decoded,
        opened.data()) == MDKR_MATCH_PEER_CRYPTO_OK);
    assert(decoded.sequence == 68u && sealWindow.next_sequence == 69u);
    assert(mdkr_match_peer_open(
        key.data(), &direction, &replay, envelope4.data(), &decoded,
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
                key.data(), &sealWindow, &sendContext,
                fragmentPayloads[fragmentIndex].data(), envelope.data()));
            assert(envelope[6] == MDKR_MATCH_PEER_PAYLOAD_PREFLIGHT_FRAGMENT);
            assert(mdkr_match_peer_open(
                key.data(), &direction, &replay, envelope.data(), &decoded,
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
        assert(!mdkr_match_peer_seal(
            key.data(), &corrupt, &sendContext, payload.data(),
            untouchedEnvelope.data()));
        assert(sealWindowEquals(corrupt, corruptBefore));
        unchanged(untouchedEnvelope.data(), untouchedEnvelope.size());

        MdkrMatchPeerSealWindow exhausted{};
        assert(mdkr_match_peer_seal_window_init(&exhausted, &direction));
        exhausted.next_sequence = UINT64_MAX;
        assert(mdkr_match_peer_seal(
            key.data(), &exhausted, &sendContext, payload.data(),
            envelope.data()));
        assert(!exhausted.ready && exhausted.next_sequence == 0u);
        assert(mdkr_match_peer_inspect(envelope.data(), &decoded));
        assert(decoded.sequence == UINT64_MAX);
        const MdkrMatchPeerSealWindow exhaustedBefore = exhausted;
        untouchedEnvelope.fill(0xa5u);
        assert(!mdkr_match_peer_seal(
            key.data(), &exhausted, &sendContext, payload.data(),
            untouchedEnvelope.data()));
        assert(sealWindowEquals(exhausted, exhaustedBefore));
        unchanged(untouchedEnvelope.data(), untouchedEnvelope.size());
    }

    /* Key derivation is directional and generation-bound. */
    auto other = key;
    auto reverse = direction;
    reverse.source_endpoint_id = 400u;
    reverse.source_generation = 9u;
    reverse.destination_endpoint_id = 100u;
    reverse.destination_generation = 2u;
    assert(mdkr_match_peer_derive_key(
        secret.data(), transcript.data(), &reverse, other.data()));
    assert(other != key);
    reverse = direction;
    reverse.destination_generation++;
    assert(mdkr_match_peer_derive_key(
        secret.data(), transcript.data(), &reverse, other.data()));
    assert(other != key);

    mdkr_match_peer_forget_key(other.data());
    assert(other == decltype(other){});

    std::puts("test_match_peer_crypto: PASS");
    return 0;
}
